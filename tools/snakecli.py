#!/usr/bin/env python3
import argparse
import datetime as dt
import json
import os
import random
import signal
import subprocess
import sys
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


def compute_economy_state(
    params: Dict,
    sum_mi: int,
    delta_m_buy: int,
    k_snakes: int,
    harvested_food: int = 0,
    movement_ticks: int = 0,
    prev_snapshot: Optional[Dict] = None,
) -> Dict:
    m = int(sum_mi) + int(params["m_gov_reserve"])
    k = max(0, int(k_snakes) + int(params["delta_k_obs"]))
    y = max(0, int(harvested_food))
    l = max(0, int(movement_ticks))
    p = float(m) / float(max(y, 1))
    alpha_bootstrap = False
    if prev_snapshot is None:
        alpha = 0.5
        pi = 0.0
        alpha_bootstrap = True
    else:
        prev_y = int(prev_snapshot.get("total_output", 0))
        prev_k = int(prev_snapshot.get("total_capital", 0))
        prev_p = float(prev_snapshot.get("price_index", 0.0))
        d_y = y - prev_y
        d_k = k - prev_k
        mpk = (float(d_y) / float(d_k)) if d_k > 0 else 0.0
        alpha = max(0.0, min(1.0, (mpk * float(k)) / float(max(y, 1))))
        pi = (p - prev_p) / max(prev_p, 1e-9)
    k_term = float(max(k, 1)) ** alpha if k > 0 else 1.0
    l_term = float(max(l, 1)) ** (1.0 - alpha) if l > 0 else 1.0
    A = float(y) / max(1e-9, k_term * l_term)
    a_world = int(params["k_land"]) * int(m)
    m_white = max(0, a_world - k)
    return {
        "M": m,
        "K": k,
        "L": l,
        "Y": y,
        "alpha": alpha,
        "A": A,
        "P": p,
        "pi": pi,
        "A_world": a_world,
        "M_white": m_white,
        "treasury_balance": int(params["m_gov_reserve"]),
        "alpha_bootstrap": alpha_bootstrap,
        "delta_m_buy": int(delta_m_buy),
    }


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


def weighted_split(total: int, n: int, rng: random.Random) -> List[int]:
    if n <= 0:
        return []
    if total <= 0:
        return [0] * n
    weights = [rng.uniform(0.2, 1.6) for _ in range(n)]
    s = sum(weights)
    raw = [int(total * w / s) for w in weights]
    # Distribute residue.
    residue = total - sum(raw)
    for _ in range(residue):
        raw[rng.randrange(n)] += 1
    return raw


def generate_lengths(snakes_num: int, target_total_k: int, world_area: int, rng: random.Random) -> List[int]:
    if snakes_num <= 0:
        return []
    base = [rng.randint(3, 20) for _ in range(snakes_num)]
    if snakes_num >= 2:
        base[0] = rng.randint(1, 3)
        base[1] = rng.randint(1, 3)
    if snakes_num >= 4:
        base[2] = rng.randint(10, 30)
        base[3] = rng.randint(10, 30)
    if snakes_num >= 5 and world_area >= 4000:
        base[4] = rng.randint(50, 90)

    cur = sum(base)
    target = clamp_int(target_total_k, snakes_num, max(snakes_num, int(world_area * 0.90)))
    if cur == 0:
        cur = 1
    scale = target / float(cur)
    out = [max(1, int(round(v * scale))) for v in base]
    diff = target - sum(out)
    while diff != 0:
        i = rng.randrange(len(out))
        if diff > 0:
            out[i] += 1
            diff -= 1
        else:
            if out[i] > 1:
                out[i] -= 1
                diff += 1
    return out


def pick_snake_body(length_k: int, w: int, h: int, occupied: set, rng: random.Random) -> Optional[List[Tuple[int, int]]]:
    dirs = [(1, 0), (-1, 0), (0, 1), (0, -1)]
    for _ in range(500):
        head_x = rng.randint(0, w - 1)
        head_y = rng.randint(0, h - 1)
        dx, dy = dirs[rng.randrange(len(dirs))]
        body = []
        ok = True
        for i in range(length_k):
            x = head_x - i * dx
            y = head_y - i * dy
            if not (0 <= x < w and 0 <= y < h):
                ok = False
                break
            if (x, y) in occupied:
                ok = False
                break
            body.append((x, y))
        if ok:
            return body
    return None


def snake_event_item(event: Dict) -> Dict:
    item = {
        "snake_id": {"S": event["snake_id"]},
        "event_id": {"S": event["event_id"]},
        "event_type": {"S": event["event_type"]},
        "x": {"N": str(event["x"])},
        "y": {"N": str(event["y"])},
        "delta_length": {"N": str(event.get("delta_length", 0))},
        "tick_number": {"N": str(int(event.get("tick_number", 0)))},
        "world_version": {"N": str(int(event.get("world_version", 0)))},
        "created_at": {"N": str(int(event["created_at"]))},
    }
    if event.get("other_snake_id"):
        item["other_snake_id"] = {"S": event["other_snake_id"]}
    return item


def user_item(user: Dict) -> Dict:
    return {
        "user_id": {"S": user["user_id"]},
        "username": {"S": user["username"]},
        "password_hash": {"S": user["password_hash"]},
        "balance_mi": {"N": str(user["balance_mi"])},
        "role": {"S": "player"},
        "created_at": {"N": str(user["created_at"])},
        "company_name": {"S": user["company_name"]},
    }


def snake_item(snake: Dict, ts: int) -> Dict:
    body = snake["body"]
    return {
        "snake_id": {"S": snake["snake_id"]},
        "owner_user_id": {"S": snake["owner_user_id"]},
        "alive": {"BOOL": True},
        "is_on_field": {"BOOL": True},
        "head_x": {"N": str(body[0][0])},
        "head_y": {"N": str(body[0][1])},
        "direction": {"N": str(snake["direction"])},
        "paused": {"BOOL": False},
        "length_k": {"N": str(snake["length_k"])},
        "body_compact": {"S": encode_body(body)},
        "color": {"S": snake["color"]},
        "last_event_id": {"S": snake.get("last_event_id", "")},
        "created_at": {"N": str(ts)},
        "updated_at": {"N": str(ts)},
    }


def ddb_put_world_chunk(world_chunks_table: str, width: int, height: int, obstacles: List[Tuple[int, int]], foods: List[Tuple[int, int]], ts: int) -> None:
    ddb_put_item(
        world_chunks_table,
        {
            "chunk_id": {"S": "main"},
            "width": {"N": str(width)},
            "height": {"N": str(height)},
            "obstacles": {"S": obstacle_json(obstacles)},
            "food_state": {"S": encode_points(foods)},
            "version": {"N": "1"},
            "updated_at": {"N": str(ts)},
        },
    )


def cmd_smartseed(args) -> int:
    if args.worldsize <= 0:
        raise RuntimeError("--worldsize must be a positive integer")

    if args.worldsize > 50_000_000 and not args.force:
        raise RuntimeError("Refusing very large --worldsize without --force")

    users_table = table_env("USERS")
    snakes_table = table_env("SNAKES")
    snake_events_table = table_env("SNAKE_EVENTS")
    world_chunks_table = table_env("WORLD_CHUNKS")
    economy_params_table = table_env("ECONOMY_PARAMS")
    economy_period_table = table_env("ECONOMY_PERIOD")

    if args.wipe and not args.force:
        if not sys.stdin.isatty():
            raise RuntimeError("--wipe in non-interactive mode requires --force")
        print("This will wipe users, snakes, snake_events, world_chunks, economy_params, economy_period.")
        reply = input("Type WIPE to continue: ").strip()
        if reply != "WIPE":
            print("Aborted.")
            return 1

    rng = random.Random(args.seed if args.seed is not None else int(now_unix()))
    ts = now_unix()

    if args.wipe:
        print("Wiping tables...")
        del_users = delete_all_items(users_table, ["user_id"], "users")
        del_snakes = delete_all_items(snakes_table, ["snake_id"], "snakes")
        del_events = delete_all_items(snake_events_table, ["snake_id", "event_id"], "snake_events")
        del_chunks = delete_all_items(world_chunks_table, ["chunk_id"], "world_chunks")
        del_ep = delete_all_items(economy_params_table, ["params_id"], "economy_params")
        del_eper = delete_all_items(economy_period_table, ["period_key"], "economy_period")
        print(f"Wipe complete: users={del_users}, snakes={del_snakes}, events={del_events}, world_chunks={del_chunks}, economy_params={del_ep}, economy_period={del_eper}")

    params = load_active_params(economy_params_table)
    if args.wipe:
        params = {
            "version": 1,
            "k_land": 24,
            "a_productivity": 1.0,
            "v_velocity": 2.0,
            "m_gov_reserve": 400,
            "cap_delta_m": 5000,
            "delta_m_issue": 0,
            "delta_k_obs": 0,
            "updated_at": ts,
            "updated_by": "smartseed",
        }
        upsert_active_and_history(economy_params_table, params)

    k_land = max(1, int(params["k_land"]))
    m_target = ceil_div(args.worldsize, k_land)
    m_g_current = int(params["m_gov_reserve"])
    # Keep economy identity M = ΣM_i + M_G while ensuring at least minimal user balances when feasible.
    m_g_effective = min(m_g_current, max(0, m_target - 2))
    if m_target < 2:
        m_g_effective = min(m_g_current, m_target)
    if m_g_effective != m_g_current:
        params["version"] = int(params["version"]) + 1
        params["m_gov_reserve"] = int(m_g_effective)
        params["updated_at"] = ts
        params["updated_by"] = "smartseed"
        upsert_active_and_history(economy_params_table, params)
    sum_mi_target = max(0, m_target - int(params["m_gov_reserve"]))

    users_num = args.usersnum if args.usersnum is not None else clamp_int(int(m_target ** 0.5), 3, 25)
    users_num = max(3, users_num)
    snakes_num = args.snakesnum if args.snakesnum is not None else users_num * 2
    snakes_num = max(users_num, snakes_num)
    snakes_num = clamp_int(snakes_num, users_num, 200)

    w, h = generate_world_dimensions(args.worldsize)
    world_area = w * h
    if world_area < snakes_num:
        raise RuntimeError("World too small for requested snakes. Increase --worldsize or reduce --snakesnum.")

    free_ratio = 0.40
    k_target = int(world_area * (1.0 - free_ratio))
    k_snakes_target = max(snakes_num, k_target - int(params["delta_k_obs"]))
    lengths = generate_lengths(snakes_num, k_snakes_target, world_area, rng)

    # Use numeric IDs because server auth/world code parses IDs with stoi.
    # For additive mode, continue from current numeric max to avoid PK collisions.
    next_user_id = 1
    next_snake_id = 1
    if not args.wipe:
        try:
            existing_users = ddb_scan_all(users_table)
            existing_snakes = ddb_scan_all(snakes_table)
            user_ids = [int(av_s(u, "user_id", "0")) for u in existing_users if av_s(u, "user_id", "").isdigit()]
            snake_ids = [int(av_s(s, "snake_id", "0")) for s in existing_snakes if av_s(s, "snake_id", "").isdigit()]
            if user_ids:
                next_user_id = max(user_ids) + 1
            if snake_ids:
                next_snake_id = max(snake_ids) + 1
        except Exception:
            # Non-fatal fallback; put-item semantics still prevent partial corruption.
            pass

    # Users
    uname_prefix = "seeduser" if args.wipe else f"seeduser{ts}_"
    users = []
    balances = weighted_split(sum_mi_target, users_num, rng)
    non_zero = sum(1 for b in balances if b > 0)
    if users_num >= 2 and non_zero < 2 and sum_mi_target > 1:
        # Guarantee at least two users with positive storage for richer testing scenarios.
        balances[0] += 1
        balances[1] += 1
        if sum(balances) > sum_mi_target:
            for i in range(users_num - 1, -1, -1):
                if balances[i] > 0 and sum(balances) > sum_mi_target:
                    balances[i] -= 1
    for i in range(users_num):
        users.append(
            {
                "user_id": str(next_user_id + i),
                "username": f"{uname_prefix}{i+1}",
                "password_hash": f"pass{i+1}",
                "balance_mi": int(balances[i]),
                "created_at": ts,
                "company_name": f"Seed Company {i+1}",
            }
        )

    # Snakes
    occupied = set()
    snakes = []
    colors = ["#00ff00", "#00aaff", "#ff00ff", "#ffaa00", "#00ffaa", "#ff6666", "#8888ff", "#66cc66"]
    for i in range(snakes_num):
        owner = users[i % users_num]["user_id"]
        body = pick_snake_body(lengths[i], w, h, occupied, rng)
        if body is None:
            # fallback short snake if space is fragmented
            body = pick_snake_body(1, w, h, occupied, rng)
            if body is None:
                break
            lengths[i] = 1
        for p in body:
            occupied.add(p)
        snakes.append(
            {
                "snake_id": str(next_snake_id + i),
                "owner_user_id": owner,
                "length_k": len(body),
                "direction": rng.choice([0, 1, 2, 3, 4]),
                "color": colors[i % len(colors)],
                "body": body,
            }
        )

    if len(snakes) < users_num:
        raise RuntimeError("Could not place enough snakes in world. Try larger --worldsize or fewer snakes.")

    # World chunk extras.
    obstacle_count = clamp_int(int(world_area * 0.01), 20, 2000)
    obstacles = []
    while len(obstacles) < obstacle_count:
        x = rng.randint(0, w - 1)
        y = rng.randint(0, h - 1)
        if (x, y) in occupied:
            continue
        occupied.add((x, y))
        obstacles.append((x, y))
    foods = []
    while len(foods) < 1:
        x = rng.randint(0, w - 1)
        y = rng.randint(0, h - 1)
        if (x, y) in occupied:
            continue
        foods.append((x, y))

    # Persist users/snakes/chunk.
    ddb_batch_put_items(users_table, [user_item(u) for u in users])
    ddb_batch_put_items(snakes_table, [snake_item(s, ts) for s in snakes])
    ddb_put_world_chunk(world_chunks_table, w, h, obstacles, foods, ts)

    # Events (bounded).
    desired_events = clamp_int(max(200, len(snakes) * 8), 200, 500)
    if args.worldsize > 2_000_000:
        desired_events = clamp_int(desired_events + 200, 200, 2000)
    event_types = ["FOOD", "BITE", "BITTEN", "SELF_COLLISION", "DEATH"]
    event_items: List[Dict] = []
    period_key = current_period_key()
    for i in range(desired_events):
        snake = snakes[rng.randrange(len(snakes))]
        et = rng.choices(event_types, weights=[55, 20, 10, 10, 5], k=1)[0]
        other = ""
        if et in ("BITE", "BITTEN") and len(snakes) > 1:
            other_snake = snake
            while other_snake["snake_id"] == snake["snake_id"]:
                other_snake = snakes[rng.randrange(len(snakes))]
            other = other_snake["snake_id"]
        point = snake["body"][0]
        ev = {
            "snake_id": snake["snake_id"],
            "event_id": f"{period_key}-{ts}-{i+1}",
            "event_type": et,
            "x": point[0],
            "y": point[1],
            "other_snake_id": other,
            "delta_length": 1 if et == "FOOD" else (-1 if et in ("BITE", "BITTEN", "DEATH", "SELF_COLLISION") else 0),
            "tick_number": i * 10,
            "world_version": 1,
            "created_at": ts - (desired_events - i),
        }
        event_items.append(snake_event_item(ev))
    ddb_batch_put_items(snake_events_table, event_items)
    event_count = len(event_items)

    # Economy recompute + period snapshot.
    sum_mi, k_snakes = aggregate_inputs(users_table, snakes_table)
    delta_m_buy = min(500, max(0, m_target // 100))
    state = compute_economy_state(params, sum_mi, delta_m_buy, k_snakes)
    period = get_period(economy_period_table, period_key)
    period["delta_m_buy"] = delta_m_buy
    period["computed_m"] = state["M"]
    period["computed_k"] = state["K"]
    period["computed_y"] = int(state["Y"])
    period["computed_p"] = int(state["P"] * 1_000_000)
    period["computed_pi"] = int(state["pi"] * 1_000_000)
    period["computed_world_area"] = state["A_world"]
    period["computed_white"] = state["M_white"]
    period["computed_at"] = ts
    put_period(economy_period_table, period)

    lengths_actual = [s["length_k"] for s in snakes]
    min_len = min(lengths_actual) if lengths_actual else 0
    max_len = max(lengths_actual) if lengths_actual else 0
    avg_len = (sum(lengths_actual) / len(lengths_actual)) if lengths_actual else 0.0

    print("smartseed complete")
    print(f"  worldsize requested: {args.worldsize}")
    print(f"  world dims generated: {w}x{h} (area={world_area})")
    print(f"  M_target: {m_target}")
    print(f"  users created: {len(users)}")
    print(f"  snakes created: {len(snakes)} min/avg/max len = {min_len}/{avg_len:.2f}/{max_len}")
    print(f"  ΣM_i: {sum_mi}")
    print(f"  M_G: {int(params['m_gov_reserve'])}")
    print(
        f"  economy: M={state['M']} K={state['K']} A_world={state['A_world']} M_white={state['M_white']} "
        f"P={state['P']:.6f} pi={state['pi']:.6f}"
    )
    print(f"  events written: {event_count}")
    print("  sample credentials:")
    for u in users[: min(10, len(users))]:
        print(f"    {u['username']} / {u['password_hash']}")
    if len(users) > 10:
        print(f"    ... and {len(users) - 10} more users")
    try:
        send_reload_signal()
        print("  runtime reload: requested")
    except Exception as e:
        print(f"  runtime reload: skipped ({e})")
    return 0


def cmd_economy_status(_args) -> int:
    params_table = table_env("ECONOMY_PARAMS")
    period_table = table_env("ECONOMY_PERIOD")
    period_key = current_period_key()
    params = load_active_params(params_table)
    period = get_period(period_table, period_key)
    print(f"period_key: {period_key}")
    print("active params:")
    for k in ["version", "k_land", "a_productivity", "v_velocity", "m_gov_reserve", "cap_delta_m", "delta_m_issue", "delta_k_obs", "updated_at", "updated_by"]:
        print(f"  {k}: {params[k]}")
    print("period:")
    for k in [
        "harvested_food",
        "movement_ticks",
        "total_output",
        "total_capital",
        "total_labor",
        "capital_share",
        "productivity_index",
        "money_supply",
        "price_index",
        "inflation_rate",
        "treasury_balance",
        "snapshot_status",
        "delta_m_buy",
        "computed_m",
        "computed_k",
        "computed_y",
        "computed_p",
        "computed_pi",
        "computed_world_area",
        "computed_white",
        "computed_at",
    ]:
        print(f"  {k}: {period[k]}")
    return 0


def cmd_economy_set(args) -> int:
    valid_types = {
        "k_land": int,
        "a_productivity": float,
        "v_velocity": float,
        "m_gov_reserve": int,
        "cap_delta_m": int,
        "delta_m_issue": int,
        "delta_k_obs": int,
    }
    if args.param not in valid_types:
        raise RuntimeError(f"Unsupported param: {args.param}")

    caster = valid_types[args.param]
    new_value = caster(args.value)

    params_table = table_env("ECONOMY_PARAMS")
    params = load_active_params(params_table)
    params[args.param] = new_value
    params["version"] = int(params["version"]) + 1
    params["updated_at"] = now_unix()
    params["updated_by"] = env("USER", "snakecli")
    upsert_active_and_history(params_table, params)

    # Keep world dimensions aligned with economy policy on admin policy updates.
    users_table = table_env("USERS")
    snakes_table = table_env("SNAKES")
    world_chunks_table = table_env("WORLD_CHUNKS")
    period_table = table_env("ECONOMY_PERIOD")
    period = get_period(period_table, current_period_key())
    sum_mi, k_snakes = aggregate_inputs(users_table, snakes_table)
    state = compute_economy_state(
        params,
        sum_mi,
        period["delta_m_buy"],
        k_snakes,
        harvested_food=period.get("harvested_food", 0),
        movement_ticks=period.get("movement_ticks", 0),
    )
    w, h = resize_world_chunk_to_area(world_chunks_table, int(state["A_world"]))
    print(f"Updated {args.param}={new_value}; new version={params['version']}")
    print(f"World resized to {w}x{h} (A_world={state['A_world']})")
    try:
        send_reload_signal()
        print("runtime reload: requested")
    except Exception as e:
        print(f"runtime reload: skipped ({e})")
    return 0


def cmd_economy_recompute(_args) -> int:
    params_table = table_env("ECONOMY_PARAMS")
    period_table = table_env("ECONOMY_PERIOD")
    users_table = table_env("USERS")
    snakes_table = table_env("SNAKES")
    world_chunks_table = table_env("WORLD_CHUNKS")

    period_key = current_period_key()
    params = load_active_params(params_table)
    period = get_period(period_table, period_key)
    sum_mi, k_snakes = aggregate_inputs(users_table, snakes_table)
    prev_key = previous_period_key(period_key)
    prev_snapshot = get_period(period_table, prev_key) if prev_key else None
    if prev_snapshot and int(prev_snapshot.get("computed_at", 0)) <= 0:
        prev_snapshot = None
    state = compute_economy_state(
        params,
        sum_mi,
        period["delta_m_buy"],
        k_snakes,
        harvested_food=period.get("harvested_food", 0),
        movement_ticks=period.get("movement_ticks", 0),
        prev_snapshot=prev_snapshot,
    )

    period["total_output"] = int(state["Y"])
    period["total_capital"] = int(state["K"])
    period["total_labor"] = int(state["L"])
    period["capital_share"] = float(state["alpha"])
    period["productivity_index"] = float(state["A"])
    period["money_supply"] = int(state["M"])
    period["price_index"] = float(state["P"])
    period["inflation_rate"] = float(state["pi"])
    period["treasury_balance"] = int(state["treasury_balance"])
    period["alpha_bootstrap"] = bool(state["alpha_bootstrap"])
    period["snapshot_status"] = "cached"

    period["computed_m"] = state["M"]
    period["computed_k"] = state["K"]
    period["computed_y"] = int(state["Y"])
    period["computed_p"] = int(state["P"] * 1_000_000)
    period["computed_pi"] = int(state["pi"] * 1_000_000)
    period["computed_world_area"] = state["A_world"]
    period["computed_white"] = state["M_white"]
    period["computed_at"] = now_unix()
    put_period(period_table, period)
    w, h = resize_world_chunk_to_area(world_chunks_table, int(state["A_world"]))

    print(f"Recomputed period {period_key}")
    print(
        f"M={state['M']} K={state['K']} L={state['L']} Y={state['Y']} "
        f"alpha={state['alpha']:.6f} A={state['A']:.6f} "
        f"P={state['P']:.6f} pi={state['pi']:.6f} "
        f"A_world={state['A_world']} M_white={state['M_white']}"
    )
    print(f"World resized to {w}x{h}")
    try:
        send_reload_signal()
        print("runtime reload: requested")
    except Exception as e:
        print(f"runtime reload: skipped ({e})")
    return 0


def cmd_treasury_set(args) -> int:
    amount = int(args.amount)
    if amount < 0:
        raise RuntimeError("treasury amount must be >= 0")
    params_table = table_env("ECONOMY_PARAMS")
    params = load_active_params(params_table)
    params["m_gov_reserve"] = amount
    params["version"] = int(params["version"]) + 1
    params["updated_at"] = now_unix()
    params["updated_by"] = env("USER", "snakecli")
    upsert_active_and_history(params_table, params)
    print(f"Updated treasury_balance (m_gov_reserve) to {amount}; version={params['version']}")
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
            "username": av_s(u, "username", ""),
            "balance": av_n(u, "balance_mi", 0),
            "capital": capital_by_user.get(user_id, 0),
        })

    key = "balance" if args.by == "balance" else "capital"
    rows.sort(key=lambda r: r[key], reverse=True)
    rows = rows[: args.limit]

    print(f"Top firms by {key}:")
    print("user_id\tusername\tbalance_mi\tcapital_k")
    for r in rows:
        print(f"{r['user_id']}\t{r['username']}\t{r['balance']}\t{r['capital']}")
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


def cmd_app_seed(_args) -> int:
    bin_path, cwd = resolve_server_binary()
    return subprocess.call([bin_path, "seed"], cwd=cwd)


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


def cmd_app_seed_reload(_args) -> int:
    rc = cmd_app_seed(_args)
    if rc != 0:
        return rc
    return cmd_app_reload(_args)


def cmd_app_reset_seed(_args) -> int:
    rc = cmd_app_reset(_args)
    if rc != 0:
        return rc
    return cmd_app_seed(_args)


def cmd_app_reset_seed_reload(_args) -> int:
    rc = cmd_app_reset_seed(_args)
    if rc != 0:
        return rc
    return cmd_app_reload(_args)


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
    eco_sub.add_parser("recompute")

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
    app_sub.add_parser("seed")
    app_sub.add_parser("reset")
    app_sub.add_parser("reload")
    app_sub.add_parser("seed-reload")
    app_sub.add_parser("reset-seed")
    app_sub.add_parser("reset-seed-reload")

    smartseed = sub.add_parser("smartseed")
    smartseed.add_argument("--worldsize", type=int, required=True, help="Target world area A_world")
    smartseed.add_argument("--usersnum", type=int, help="Number of users to seed")
    smartseed.add_argument("--snakesnum", type=int, help="Number of snakes to seed")
    smartseed.add_argument("--seed", type=int, help="Deterministic RNG seed")
    smartseed.add_argument("--wipe", action="store_true", help="Wipe game content tables before seeding")
    smartseed.add_argument("--force", action="store_true", help="Bypass confirmations and size guardrails")

    sub.add_parser("help")
    return parser, parser.parse_args()


def is_write_command(args) -> bool:
    return (
        (args.group == "economy" and args.cmd in {"set", "recompute"})
        or (args.group == "treasury" and args.cmd in {"set"})
        or (args.group == "app" and args.cmd in {"seed", "reset", "reload", "seed-reload", "reset-seed", "reset-seed-reload"})
        or (args.group == "smartseed")
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
    if args.group in {"economy", "treasury", "firms", "snakes"}:
        # Shared storage config requirements for data commands.
        table_env("USERS")
        table_env("SNAKES")
        table_env("ECONOMY_PARAMS")
        table_env("ECONOMY_PERIOD")
    if args.group == "smartseed":
        table_env("USERS")
        table_env("SNAKES")
        table_env("SNAKE_EVENTS")
        table_env("WORLD_CHUNKS")
        table_env("ECONOMY_PARAMS")
        table_env("ECONOMY_PERIOD")

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
    if args.group == "app" and args.cmd == "seed":
        return cmd_app_seed(args)
    if args.group == "app" and args.cmd == "reset":
        return cmd_app_reset(args)
    if args.group == "app" and args.cmd == "reload":
        return cmd_app_reload(args)
    if args.group == "app" and args.cmd == "seed-reload":
        return cmd_app_seed_reload(args)
    if args.group == "app" and args.cmd == "reset-seed":
        return cmd_app_reset_seed(args)
    if args.group == "app" and args.cmd == "reset-seed-reload":
        return cmd_app_reset_seed_reload(args)
    if args.group == "smartseed":
        return cmd_smartseed(args)

    raise RuntimeError("Unsupported command")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
