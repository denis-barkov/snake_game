#!/usr/bin/env python3
import argparse
import datetime as dt
import json
import os
import signal
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path
from typing import Dict, List, Optional, Tuple

try:
    from zoneinfo import ZoneInfo
except Exception:  # pragma: no cover
    ZoneInfo = None


def env(name: str, default: Optional[str] = None) -> Optional[str]:
    return os.environ.get(name, default)


def now_unix() -> int:
    return int(dt.datetime.now(dt.timezone.utc).timestamp())


def current_period_key() -> str:
    period_seconds = int(env("ECON_PERIOD_SECONDS", "300") or "300")
    align = (env("ECON_PERIOD_ALIGN", "rolling") or "rolling").strip().lower()
    tz_name = env("ECON_PERIOD_TZ", "America/New_York") or "America/New_York"
    if align == "midnight":
        if ZoneInfo is not None:
            now_local = dt.datetime.now(ZoneInfo(tz_name))
        else:
            now_local = dt.datetime.now()
        return now_local.strftime("%Y%m%d")
    now_utc = dt.datetime.now(dt.timezone.utc)
    idx = int(now_utc.timestamp()) // max(60, period_seconds)
    return f"p{idx}"


def table_env(name_suffix: str) -> str:
    modern = f"TABLE_{name_suffix}"
    legacy = f"DYNAMO_TABLE_{name_suffix}"
    value = env(modern) or env(legacy)
    if not value:
        raise RuntimeError(f"Missing required environment variable: {modern} (or {legacy})")
    return value


def ddb_endpoint() -> str:
    return env("DDB_ENDPOINT") or env("DYNAMO_ENDPOINT") or ""


def api_base_url() -> str:
    return (env("SNAKECLI_API", "http://127.0.0.1:8080") or "http://127.0.0.1:8080").rstrip("/")


def api_json(method: str, path: str, token: Optional[str], payload: Optional[Dict] = None) -> Dict:
    url = f"{api_base_url()}{path}"
    body = None if payload is None else json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=body, method=method.upper())
    req.add_header("Accept", "application/json")
    if payload is not None:
        req.add_header("Content-Type", "application/json")
    if token:
        req.add_header("X-Admin-Token", token)
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            raw = resp.read().decode("utf-8") or "{}"
            return json.loads(raw)
    except urllib.error.HTTPError as e:
        raw = e.read().decode("utf-8", errors="replace")
        raise RuntimeError(f"HTTP {e.code}: {raw}")
    except urllib.error.URLError as e:
        raise RuntimeError(f"API unreachable at {url}: {e}")


def effective_admin_token(args) -> str:
    return args.token if getattr(args, "token", None) is not None else (env("ADMIN_TOKEN") or "")


def aws_base() -> List[str]:
    region = env("AWS_REGION", "us-east-1")
    cmd = ["aws", "--region", region]
    endpoint = ddb_endpoint()
    if endpoint:
        cmd.extend(["--endpoint-url", endpoint])
    cmd.extend(["dynamodb"])
    return cmd


def run_aws_json(args: List[str]) -> Dict:
    proc = subprocess.run(
        aws_base() + args + ["--output", "json"],
        check=False,
        text=True,
        capture_output=True,
    )
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or f"aws command failed: {' '.join(args)}")
    return json.loads(proc.stdout or "{}")


def ddb_get_item(table: str, key: Dict) -> Dict:
    out = run_aws_json(["get-item", "--table-name", table, "--key", json.dumps(key)])
    return out.get("Item", {})


def ddb_put_item(table: str, item: Dict) -> None:
    run_aws_json(["put-item", "--table-name", table, "--item", json.dumps(item)])


def ddb_scan_all(table: str) -> List[Dict]:
    items: List[Dict] = []
    start_key = None
    while True:
        args = ["scan", "--table-name", table]
        if start_key:
            args.extend(["--exclusive-start-key", json.dumps(start_key)])
        out = run_aws_json(args)
        items.extend(out.get("Items", []))
        start_key = out.get("LastEvaluatedKey")
        if not start_key:
            break
    return items


def ddb_batch_write(request_items: Dict) -> Dict:
    return run_aws_json(["batch-write-item", "--request-items", json.dumps(request_items)])


def ddb_batch_put_items(table: str, items: List[Dict]) -> None:
    requests = [{"PutRequest": {"Item": item}} for item in items]
    for batch in chunked(requests, 25):
        remaining = batch
        attempts = 0
        while remaining and attempts < 8:
            resp = ddb_batch_write({table: remaining})
            remaining = resp.get("UnprocessedItems", {}).get(table, [])
            attempts += 1
        if remaining:
            raise RuntimeError(f"Batch put failed for table {table}: unprocessed items remain")


def chunked(items: List, n: int):
    for i in range(0, len(items), n):
        yield items[i : i + n]


def delete_all_items(table: str, key_fields: List[str], progress_label: str = "") -> int:
    deleted = 0
    start_key = None
    while True:
        args = ["scan", "--table-name", table]
        if start_key:
            args.extend(["--exclusive-start-key", json.dumps(start_key)])
        out = run_aws_json(args)
        items = out.get("Items", [])
        if not items:
            start_key = out.get("LastEvaluatedKey")
            if not start_key:
                break
            continue

        deletes = []
        for item in items:
            key = {}
            for f in key_fields:
                if f not in item:
                    raise RuntimeError(f"Cannot wipe table {table}: missing key field {f} in scanned item")
                key[f] = item[f]
            deletes.append({"DeleteRequest": {"Key": key}})

        for batch in chunked(deletes, 25):
            req = {table: batch}
            resp = ddb_batch_write(req)
            unprocessed = resp.get("UnprocessedItems", {}).get(table, [])
            # Retry a few times for throttled writes.
            retry = 0
            while unprocessed and retry < 5:
                resp = ddb_batch_write({table: unprocessed})
                unprocessed = resp.get("UnprocessedItems", {}).get(table, [])
                retry += 1
            if unprocessed:
                raise RuntimeError(f"Wipe failed for {table}: unprocessed delete requests remain")
            deleted += len(batch)
            if progress_label:
                print(f"{progress_label}: deleted {deleted}", flush=True)

        start_key = out.get("LastEvaluatedKey")
        if not start_key:
            break
    return deleted


def av_s(item: Dict, key: str, default: str = "") -> str:
    attr = item.get(key, {})
    return attr.get("S", default)


def av_n(item: Dict, key: str, default: int = 0) -> int:
    attr = item.get(key, {})
    val = attr.get("N")
    if val is None:
        return default
    try:
        return int(float(val))
    except Exception:
        return default


def av_bool(item: Dict, key: str, default: bool = False) -> bool:
    attr = item.get(key, {})
    if "BOOL" in attr:
        return bool(attr["BOOL"])
    return default


def to_params(item: Dict) -> Dict:
    return {
        "version": av_n(item, "version", 1),
        "k_land": av_n(item, "k_land", 24),
        "a_productivity": float(item.get("a_productivity", {}).get("N", "1.0")),
        "v_velocity": float(item.get("v_velocity", {}).get("N", "2.0")),
        "food_spawn_target": av_n(item, "food_spawn_target", 1),
        "alpha_bootstrap_default": float(item.get("alpha_bootstrap_default", {}).get("N", "0.5")),
        "m_gov_reserve": av_n(item, "m_gov_reserve", 400),
        "cap_delta_m": av_n(item, "cap_delta_m", 5000),
        "delta_m_issue": av_n(item, "delta_m_issue", 0),
        "delta_k_obs": av_n(item, "delta_k_obs", 0),
        "updated_at": av_n(item, "updated_at", 0),
        "updated_by": av_s(item, "updated_by", ""),
    }


def params_item(params_id: str, params: Dict) -> Dict:
    return {
        "params_id": {"S": params_id},
        "version": {"N": str(int(params["version"]))},
        "k_land": {"N": str(int(params["k_land"]))},
        "a_productivity": {"N": str(float(params["a_productivity"]))},
        "v_velocity": {"N": str(float(params["v_velocity"]))},
        "food_spawn_target": {"N": str(int(params.get("food_spawn_target", 1)))},
        "alpha_bootstrap_default": {"N": str(float(params.get("alpha_bootstrap_default", 0.5)))},
        "m_gov_reserve": {"N": str(int(params["m_gov_reserve"]))},
        "cap_delta_m": {"N": str(int(params["cap_delta_m"]))},
        "delta_m_issue": {"N": str(int(params["delta_m_issue"]))},
        "delta_k_obs": {"N": str(int(params["delta_k_obs"]))},
        "updated_at": {"N": str(int(params["updated_at"]))},
        "updated_by": {"S": params.get("updated_by", "admin")},
    }


def load_active_params(table: str) -> Dict:
    item = ddb_get_item(table, {"params_id": {"S": "active"}})
    if not item:
        item = ddb_get_item(table, {"params_id": {"S": "global"}})
    if not item:
        return {
            "version": 1,
            "k_land": 24,
            "a_productivity": 1.0,
            "v_velocity": 2.0,
            "food_spawn_target": 1,
            "alpha_bootstrap_default": 0.5,
            "m_gov_reserve": 400,
            "cap_delta_m": 5000,
            "delta_m_issue": 0,
            "delta_k_obs": 0,
            "updated_at": now_unix(),
            "updated_by": "bootstrap",
        }
    return to_params(item)


def upsert_active_and_history(table: str, params: Dict) -> None:
    version = int(params["version"])
    ddb_put_item(table, params_item(f"ver#{version}", params))
    ddb_put_item(table, params_item("active", params))


def get_period(table: str, period_key: str) -> Dict:
    item = ddb_get_item(table, {"period_key": {"S": period_key}})
    if not item:
        return {
            "period_key": period_key,
            "harvested_food": 0,
            "real_output": 0,
            "movement_ticks": 0,
            "total_output": 0,
            "total_capital": 0,
            "total_labor": 0,
            "capital_share": 0.5,
            "productivity_index": 0.0,
            "money_supply": 0,
            "price_index": 0.0,
            "inflation_rate": 0.0,
            "treasury_balance": 0,
            "alpha_bootstrap": True,
            "is_finalized": False,
            "finalized_at": 0,
            "snapshot_status": "live_unfinalized",
            "period_ends_in_seconds": 0,
            "delta_m_buy": 0,
            "computed_m": 0,
            "computed_k": 0,
            "computed_y": 0,
            "computed_p": 0,
            "computed_pi": 0,
            "computed_world_area": 0,
            "computed_white": 0,
            "computed_at": 0,
        }
    return {
        "period_key": period_key,
        "harvested_food": av_n(item, "harvested_food", 0),
        "real_output": av_n(item, "real_output", av_n(item, "harvested_food", 0)),
        "movement_ticks": av_n(item, "movement_ticks", 0),
        "total_output": av_n(item, "total_output", 0),
        "total_capital": av_n(item, "total_capital", 0),
        "total_labor": av_n(item, "total_labor", 0),
        "capital_share": float(item.get("capital_share", {}).get("N", "0.5")),
        "productivity_index": float(item.get("productivity_index", {}).get("N", "0.0")),
        "money_supply": av_n(item, "money_supply", 0),
        "price_index": float(item.get("price_index", {}).get("N", "0.0")),
        "inflation_rate": float(item.get("inflation_rate", {}).get("N", "0.0")),
        "treasury_balance": av_n(item, "treasury_balance", 0),
        "alpha_bootstrap": av_bool(item, "alpha_bootstrap", False),
        "is_finalized": av_bool(item, "is_finalized", False),
        "finalized_at": av_n(item, "finalized_at", 0),
        "snapshot_status": av_s(item, "snapshot_status", "live_unfinalized"),
        "period_ends_in_seconds": av_n(item, "period_ends_in_seconds", 0),
        "delta_m_buy": av_n(item, "delta_m_buy", 0),
        "computed_m": av_n(item, "computed_m", 0),
        "computed_k": av_n(item, "computed_k", 0),
        "computed_y": av_n(item, "computed_y", 0),
        "computed_p": av_n(item, "computed_p", 0),
        "computed_pi": av_n(item, "computed_pi", 0),
        "computed_world_area": av_n(item, "computed_world_area", 0),
        "computed_white": av_n(item, "computed_white", 0),
        "computed_at": av_n(item, "computed_at", 0),
    }


def put_period(table: str, period: Dict) -> None:
    ddb_put_item(
        table,
        {
            "period_key": {"S": period["period_key"]},
            "harvested_food": {"N": str(int(period.get("harvested_food", 0)))},
            "real_output": {"N": str(int(period.get("real_output", period.get("harvested_food", 0))))},
            "movement_ticks": {"N": str(int(period.get("movement_ticks", 0)))},
            "total_output": {"N": str(int(period.get("total_output", 0)))},
            "total_capital": {"N": str(int(period.get("total_capital", 0)))},
            "total_labor": {"N": str(int(period.get("total_labor", 0)))},
            "capital_share": {"N": str(float(period.get("capital_share", 0.5)))},
            "productivity_index": {"N": str(float(period.get("productivity_index", 0.0)))},
            "money_supply": {"N": str(int(period.get("money_supply", 0)))},
            "price_index": {"N": str(float(period.get("price_index", 0.0)))},
            "inflation_rate": {"N": str(float(period.get("inflation_rate", 0.0)))},
            "treasury_balance": {"N": str(int(period.get("treasury_balance", 0)))},
            "alpha_bootstrap": {"BOOL": bool(period.get("alpha_bootstrap", False))},
            "is_finalized": {"BOOL": bool(period.get("is_finalized", False))},
            "finalized_at": {"N": str(int(period.get("finalized_at", 0)))},
            "snapshot_status": {"S": str(period.get("snapshot_status", "live_unfinalized"))},
            "period_ends_in_seconds": {"N": str(int(period.get("period_ends_in_seconds", 0)))},
            "delta_m_buy": {"N": str(int(period["delta_m_buy"]))},
            "computed_m": {"N": str(int(period["computed_m"]))},
            "computed_k": {"N": str(int(period["computed_k"]))},
            "computed_y": {"N": str(int(period["computed_y"]))},
            "computed_p": {"N": str(int(period["computed_p"]))},
            "computed_pi": {"N": str(int(period["computed_pi"]))},
            "computed_world_area": {"N": str(int(period["computed_world_area"]))},
            "computed_white": {"N": str(int(period["computed_white"]))},
            "computed_at": {"N": str(int(period["computed_at"]))},
        },
    )


def aggregate_inputs(users_table: str, snakes_table: str) -> Tuple[int, int]:
    users = ddb_scan_all(users_table)
    sum_mi = sum(av_n(u, "balance_mi", 0) for u in users)

    snakes = ddb_scan_all(snakes_table)
    k_snakes = 0
    for s in snakes:
        alive = av_bool(s, "alive", True)
        is_on_field = av_bool(s, "is_on_field", alive)
        if alive and is_on_field:
            k_snakes += max(0, av_n(s, "length_k", 0))
    return sum_mi, k_snakes


def clamp_int(value: int, min_v: int, max_v: int) -> int:
    return max(min_v, min(max_v, value))


def ceil_div(a: int, b: int) -> int:
    return (a + b - 1) // b


def previous_period_key(period_key: str) -> Optional[str]:
    if period_key.startswith("p"):
        try:
            idx = int(period_key[1:])
        except Exception:
            return None
        if idx <= 0:
            return None
        return f"p{idx - 1}"
    if len(period_key) == 8 and period_key.isdigit():
        try:
            d = dt.datetime.strptime(period_key, "%Y%m%d").date()
        except Exception:
            return None
        return (d - dt.timedelta(days=1)).strftime("%Y%m%d")
    return None


def encode_body(body: List[Tuple[int, int]]) -> str:
    return "[" + ",".join([f"[{x},{y}]" for (x, y) in body]) + "]"


def encode_points(points: List[Tuple[int, int]]) -> str:
    return "[" + ",".join([f"[{x},{y}]" for (x, y) in points]) + "]"


def obstacle_json(points: List[Tuple[int, int]]) -> str:
    # Keep compatibility with current world chunk format consumed by server.
    return encode_points(points)


def generate_world_dimensions(area_target: int) -> Tuple[int, int]:
    w = max(10, int(round(area_target ** 0.5)))
    h = ceil_div(area_target, w)
    return w, h


def resize_world_chunk_to_area(world_chunks_table: str, area_target: int) -> Tuple[int, int]:
    w, h = generate_world_dimensions(area_target)
    existing = ddb_get_item(world_chunks_table, {"chunk_id": {"S": "main"}})
    obstacles_attr = existing.get("obstacles", {"S": "[]"})
    food_attr = existing.get("food_state", {"S": "[]"})
    next_version = av_n(existing, "version", 0) + 1
    ts = now_unix()
    ddb_put_item(
        world_chunks_table,
        {
            "chunk_id": {"S": "main"},
            "width": {"N": str(w)},
            "height": {"N": str(h)},
            "obstacles": obstacles_attr if isinstance(obstacles_attr, dict) else {"S": "[]"},
            "food_state": food_attr if isinstance(food_attr, dict) else {"S": "[]"},
            "version": {"N": str(next_version)},
            "updated_at": {"N": str(ts)},
        },
    )
    return w, h


def cmd_economy_status(args) -> int:
    token = effective_admin_token(args)
    status = api_json("GET", "/admin/economy/status", token)
    debug = api_json("GET", "/economy/debug", token)
    print(f"period_id: {status.get('period_id', '—')}")
    print(f"is_finalized: {status.get('is_finalized', False)}")
    print(f"finalized_at: {status.get('finalized_at', 0)}")
    print("macro:")
    for k in ["Y", "K", "L", "alpha", "A", "M", "P", "pi", "snapshot_status", "period_ends_in_seconds"]:
        print(f"  {k}: {status.get(k)}")
    print("stabilization:")
    print(f"  field_size: {status.get('field_size')}")
    print(f"  free_space_on_field: {status.get('free_space_on_field')}")
    print(f"  system_white_space_reserve: {status.get('system_white_space_reserve')}")
    print(f"  R: {status.get('spatial_ratio_r')}")
    print(f"  LCR: {status.get('lcr')}")
    print(f"  treasury_white_space: {status.get('treasury_white_space')}")
    print(f"  failures_this_period: {status.get('failures_this_period')}")
    print(f"  status: {status.get('stabilization_status') or status.get('stabilization_mode')}")
    print(f"  next_fast_check_in_seconds: {status.get('next_fast_check_in_seconds')}")
    print(f"  next_period_close_in_seconds: {status.get('period_ends_in_seconds')}")
    print("debug:")
    pending = debug.get("pending", {})
    print(f"  harvested_food: {pending.get('harvested_food', 0)}")
    print(f"  real_output: {pending.get('real_output', 0)}")
    print(f"  movement_ticks: {pending.get('movement_ticks', 0)}")
    print(f"  users: {pending.get('users', 0)}")
    print(f"  flush_interval_sec: {debug.get('flush_interval_sec', 0)}")
    print(f"  seconds_since_last_flush: {debug.get('seconds_since_last_flush', 0)}")
    return 0


def cmd_economy_set(args) -> int:
    token = effective_admin_token(args)
    resp = api_json("POST", "/admin/economy/set", token, {"param": args.param, "value": str(args.value)})
    print("Updated economy parameter.")
    print(json.dumps(resp, indent=2))
    return 0


def cmd_economy_recompute(args) -> int:
    token = effective_admin_token(args)
    payload = {"force_rewrite": bool(getattr(args, "force_rewrite", False))}
    if getattr(args, "period_id", None):
        payload["period_id"] = args.period_id
    resp = api_json("POST", "/admin/economy/recompute", token, payload)
    print("Recompute requested.")
    print(json.dumps(resp, indent=2))
    return 0


def cmd_treasury_set(args) -> int:
    token = effective_admin_token(args)
    resp = api_json("POST", "/admin/treasury/set", token, {"amount": int(args.amount)})
    print("Updated treasury balance.")
    print(json.dumps(resp, indent=2))
    return 0


def cmd_firms_top(args) -> int:
    users_table = table_env("USERS")
    snakes_table = table_env("SNAKES")
    users = ddb_scan_all(users_table)
    snakes = ddb_scan_all(snakes_table)

    capital_by_user: Dict[str, int] = {}
    for s in snakes:
        alive = av_bool(s, "alive", True)
        is_on_field = av_bool(s, "is_on_field", alive)
        if not (alive and is_on_field):
            continue
        owner = av_s(s, "owner_user_id", "")
        if not owner:
            continue
        capital_by_user[owner] = capital_by_user.get(owner, 0) + max(0, av_n(s, "length_k", 0))

    rows = []
    for u in users:
        user_id = av_s(u, "user_id", "")
        rows.append({
            "user_id": user_id,
            "balance": av_n(u, "balance_mi", 0),
            "capital": capital_by_user.get(user_id, 0),
        })

    key = "balance" if args.by == "balance" else "capital"
    rows.sort(key=lambda r: r[key], reverse=True)
    rows = rows[: args.limit]

    print(f"Top firms by {key}:")
    print("user_id\tbalance_mi\tcapital_k")
    for r in rows:
        print(f"{r['user_id']}\t{r['balance']}\t{r['capital']}")
    return 0


def cmd_snakes_list(args) -> int:
    snakes_table = table_env("SNAKES")
    snakes = ddb_scan_all(snakes_table)

    rows = []
    for s in snakes:
        alive = av_bool(s, "alive", True)
        is_on_field = av_bool(s, "is_on_field", alive)
        if args.onfield and not is_on_field:
            continue
        rows.append({
            "snake_id": av_s(s, "snake_id", ""),
            "owner_user_id": av_s(s, "owner_user_id", ""),
            "length_k": av_n(s, "length_k", 0),
            "alive": alive,
            "is_on_field": is_on_field,
            "last_event_id": av_s(s, "last_event_id", ""),
        })

    rows.sort(key=lambda r: r["length_k"], reverse=True)
    rows = rows[: args.limit]

    print("snake_id\towner_user_id\tlength_k\talive\tis_on_field\tlast_event_id")
    for r in rows:
        print(f"{r['snake_id']}\t{r['owner_user_id']}\t{r['length_k']}\t{str(r['alive']).lower()}\t{str(r['is_on_field']).lower()}\t{r['last_event_id']}")
    return 0


def load_server_env_if_present() -> None:
    env_file = Path("/etc/snake.env")
    if not env_file.exists():
        return
    for raw in env_file.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip().strip('"').strip("'")
        if key and key not in os.environ:
            os.environ[key] = value


def resolve_server_binary() -> Tuple[str, str]:
    candidates = [
        ("/opt/snake/snake_server", "/opt/snake"),
        (str((Path(__file__).resolve().parents[1] / "snake_server")), str(Path(__file__).resolve().parents[1])),
        ("./snake_server", str(Path.cwd())),
    ]
    for binary, cwd in candidates:
        if os.path.isfile(binary) and os.access(binary, os.X_OK):
            return binary, cwd
    raise RuntimeError("snake_server binary not found")


def cmd_app_reset(_args) -> int:
    bin_path, cwd = resolve_server_binary()
    return subprocess.call([bin_path, "reset"], cwd=cwd)


def send_reload_signal() -> int:
    attempts = [
        ["sudo", "-n", "systemctl", "kill", "-s", "USR1", "snake"],
        ["systemctl", "kill", "-s", "USR1", "snake"],
        ["sudo", "-n", "pkill", "-USR1", "-x", "snake_server"],
        ["pkill", "-USR1", "-x", "snake_server"],
    ]
    for cmd in attempts:
        if subprocess.call(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) == 0:
            return 0
    raise RuntimeError("No running snake_server process found for reload")


def cmd_app_reload(_args) -> int:
    return send_reload_signal()


def parse_args():
    parser = argparse.ArgumentParser(description="Unified snake admin CLI")
    parser.add_argument("--token", help="Admin token (must match ADMIN_TOKEN env for write commands)")
    sub = parser.add_subparsers(dest="group", required=True)

    eco = sub.add_parser("economy")
    eco_sub = eco.add_subparsers(dest="cmd", required=True)
    eco_sub.add_parser("status")
    eco_set = eco_sub.add_parser("set")
    eco_set.add_argument("param")
    eco_set.add_argument("value")
    eco_recompute = eco_sub.add_parser("recompute")
    eco_recompute.add_argument("--force-rewrite", action="store_true")
    eco_recompute.add_argument("--period-id")

    treasury = sub.add_parser("treasury")
    treasury_sub = treasury.add_subparsers(dest="cmd", required=True)
    treasury_set = treasury_sub.add_parser("set")
    treasury_set.add_argument("amount", type=int)

    firms = sub.add_parser("firms")
    firms_sub = firms.add_subparsers(dest="cmd", required=True)
    firms_top = firms_sub.add_parser("top")
    firms_top.add_argument("--by", choices=["balance", "capital"], default="balance")
    firms_top.add_argument("--limit", type=int, default=10)

    snakes = sub.add_parser("snakes")
    snakes_sub = snakes.add_subparsers(dest="cmd", required=True)
    snakes_list = snakes_sub.add_parser("list")
    snakes_list.add_argument("--onfield", action="store_true")
    snakes_list.add_argument("--limit", type=int, default=50)

    app = sub.add_parser("app")
    app_sub = app.add_subparsers(dest="cmd", required=True)
    app_sub.add_parser("reset")
    app_sub.add_parser("reload")

    sub.add_parser("help")
    return parser, parser.parse_args()


def is_write_command(args) -> bool:
    return (
        (args.group == "economy" and args.cmd in {"set", "recompute"})
        or (args.group == "treasury" and args.cmd in {"set"})
        or (args.group == "app" and args.cmd in {"reset", "reload"})
    )


def verify_token(args) -> None:
    if not is_write_command(args):
        return
    expected = env("ADMIN_TOKEN")
    provided = args.token if args.token is not None else expected
    if not provided:
        raise RuntimeError("Write commands require --token or ADMIN_TOKEN")
    if expected and provided != expected:
        raise RuntimeError("Invalid admin token")


def main() -> int:
    load_server_env_if_present()
    parser, args = parse_args()
    if args.group == "help":
        parser.print_help()
        return 0

    verify_token(args)
    if args.group in {"firms", "snakes"}:
        # Shared storage config requirements for direct DynamoDB data commands.
        table_env("USERS")
        table_env("SNAKES")
    if args.group == "economy" and args.cmd == "status":
        return cmd_economy_status(args)
    if args.group == "economy" and args.cmd == "set":
        return cmd_economy_set(args)
    if args.group == "economy" and args.cmd == "recompute":
        return cmd_economy_recompute(args)
    if args.group == "treasury" and args.cmd == "set":
        return cmd_treasury_set(args)
    if args.group == "firms" and args.cmd == "top":
        return cmd_firms_top(args)
    if args.group == "snakes" and args.cmd == "list":
        return cmd_snakes_list(args)
    if args.group == "app" and args.cmd == "reset":
        return cmd_app_reset(args)
    if args.group == "app" and args.cmd == "reload":
        return cmd_app_reload(args)

    raise RuntimeError("Unsupported command")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
