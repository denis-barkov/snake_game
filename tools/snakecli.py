#!/usr/bin/env python3
import argparse
import datetime as dt
import json
import os
import signal
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple


def env(name: str, default: Optional[str] = None) -> Optional[str]:
    return os.environ.get(name, default)


def now_unix() -> int:
    return int(dt.datetime.now(dt.timezone.utc).timestamp())


def current_period_key() -> str:
    return dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d%H")


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


def compute_economy_state(params: Dict, sum_mi: int, delta_m_buy: int, k_snakes: int) -> Dict:
    m = sum_mi + int(params["m_gov_reserve"])
    delta_m = min(int(params["cap_delta_m"]), int(params["delta_m_issue"])) + int(delta_m_buy)
    k = k_snakes + int(params["delta_k_obs"])
    y = float(params["a_productivity"]) * float(k)
    p = (float(m) * float(params["v_velocity"])) / max(y, 1.0)
    pi = float(delta_m) / float(max(m, 1))
    a_world = int(params["k_land"]) * int(m)
    m_white = max(0, a_world - k)
    return {
        "M": m,
        "K": k,
        "Y": y,
        "P": p,
        "pi": pi,
        "A_world": a_world,
        "M_white": m_white,
    }


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
    for k in ["delta_m_buy", "computed_m", "computed_k", "computed_y", "computed_p", "computed_pi", "computed_world_area", "computed_white", "computed_at"]:
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
    print(f"Updated {args.param}={new_value}; new version={params['version']}")
    return 0


def cmd_economy_recompute(_args) -> int:
    params_table = table_env("ECONOMY_PARAMS")
    period_table = table_env("ECONOMY_PERIOD")
    users_table = table_env("USERS")
    snakes_table = table_env("SNAKES")

    period_key = current_period_key()
    params = load_active_params(params_table)
    period = get_period(period_table, period_key)
    sum_mi, k_snakes = aggregate_inputs(users_table, snakes_table)
    state = compute_economy_state(params, sum_mi, period["delta_m_buy"], k_snakes)

    period["computed_m"] = state["M"]
    period["computed_k"] = state["K"]
    period["computed_y"] = int(state["Y"])
    period["computed_p"] = int(state["P"] * 1_000_000)
    period["computed_pi"] = int(state["pi"] * 1_000_000)
    period["computed_world_area"] = state["A_world"]
    period["computed_white"] = state["M_white"]
    period["computed_at"] = now_unix()
    put_period(period_table, period)

    print(f"Recomputed period {period_key}")
    print(f"M={state['M']} K={state['K']} Y={state['Y']:.3f} P={state['P']:.6f} pi={state['pi']:.6f} A_world={state['A_world']} M_white={state['M_white']}")
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
    if subprocess.call(["systemctl", "kill", "-s", "USR1", "snake"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) == 0:
        return 0
    for proc_name in ["snake_server"]:
        if subprocess.call(["pkill", "-USR1", "-x", proc_name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) == 0:
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

    sub.add_parser("help")
    return parser, parser.parse_args()


def is_write_command(args) -> bool:
    return (
        (args.group == "economy" and args.cmd in {"set", "recompute"})
        or (args.group == "app" and args.cmd in {"seed", "reset", "reload", "seed-reload", "reset-seed", "reset-seed-reload"})
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
    if args.group in {"economy", "firms", "snakes"}:
        # Shared storage config requirements for data commands.
        table_env("USERS")
        table_env("SNAKES")
        table_env("ECONOMY_PARAMS")
        table_env("ECONOMY_PERIOD")

    if args.group == "economy" and args.cmd == "status":
        return cmd_economy_status(args)
    if args.group == "economy" and args.cmd == "set":
        return cmd_economy_set(args)
    if args.group == "economy" and args.cmd == "recompute":
        return cmd_economy_recompute(args)
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

    raise RuntimeError("Unsupported command")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
