#!/usr/bin/env python3
import json
import os
import subprocess
import sys
import time
from typing import Dict, List, Optional, Tuple


def env(name: str, default: Optional[str] = None) -> Optional[str]:
    return os.environ.get(name, default)


def env_table(suffix: str, default: str) -> str:
    return os.environ.get(f"TABLE_{suffix}") or os.environ.get(f"DYNAMO_TABLE_{suffix}") or default


def aws_base() -> List[str]:
    region = env("AWS_REGION", "us-east-1")
    endpoint = env("DYNAMO_ENDPOINT", "")
    cmd = ["aws", "--region", region]
    if endpoint:
        cmd.extend(["--endpoint-url", endpoint])
    cmd.append("dynamodb")
    return cmd


def run_json(args: List[str]) -> Dict:
    proc = subprocess.run(aws_base() + args + ["--output", "json"], check=False, text=True, capture_output=True)
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or f"aws failed: {' '.join(args)}")
    return json.loads(proc.stdout or "{}")


def put_item(table: str, item: Dict) -> None:
    run_json(["put-item", "--table-name", table, "--item", json.dumps(item)])


def delete_item(table: str, key: Dict) -> None:
    run_json(["delete-item", "--table-name", table, "--key", json.dumps(key)])


def has_any_users(users_table: str) -> bool:
    out = run_json(["scan", "--table-name", users_table, "--limit", "1"])
    return len(out.get("Items", [])) > 0


def normalize_name(value: str) -> str:
    return " ".join(value.strip().lower().split())


def encode_body(points: List[Tuple[int, int]]) -> str:
    return json.dumps([[x, y] for x, y in points], separators=(",", ":"))


def parse_seed_config(path: str) -> Dict:
    raw = open(path, "r", encoding="utf-8").read()
    json_like_lines = []
    for line in raw.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        if stripped.startswith("#"):
            continue
        json_like_lines.append(line)
    payload = "\n".join(json_like_lines).strip()
    if not payload:
        raise RuntimeError("seed config is empty")
    try:
        data = json.loads(payload)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"seed config must be JSON-compatible YAML: {exc}") from exc
    if not isinstance(data, dict):
        raise RuntimeError("seed config root must be an object")
    return data


def bool_env(name: str, default: bool) -> bool:
    value = (env(name, "true" if default else "false") or "").strip().lower()
    if value in {"1", "true", "yes", "on"}:
        return True
    if value in {"0", "false", "no", "off"}:
        return False
    return default


def validate_name(name: str, field: str) -> str:
    value = (name or "").strip()
    if len(value) < 4:
        raise RuntimeError(f"{field} must be at least 4 characters")
    return value


def place_snake_body(width: int, height: int, used: set, length: int) -> List[Tuple[int, int]]:
    for y in range(1, max(2, height - 1)):
        for x in range(1, max(2, width - length - 1)):
            body = [(x + i, y) for i in range(length)]
            if all((px, py) not in used for (px, py) in body):
                return body
    raise RuntimeError("seed world too small to place requested snakes")


def main() -> int:
    app_env = (env("APP_ENV", "prod") or "prod").strip().lower()
    seed_enabled = bool_env("SEED_ENABLED", False)
    seed_path = env("SEED_CONFIG_PATH", "") or ""
    users_table = env_table("USERS", "snake-local-users")
    snakes_table = env_table("SNAKES", "snake-local-snakes")
    world_chunks_table = env_table("WORLD_CHUNKS", "snake-local-world_chunks")
    economy_params_table = env_table("ECONOMY_PARAMS", "snake-local-economy_params")

    # Seed only in non-prod by default, or explicitly enabled for prod-like environments.
    if app_env == "prod" and not seed_enabled:
        print("seed: skipped (prod and SEED_ENABLED=false)")
        return 0
    if not seed_path:
        print("seed: skipped (SEED_CONFIG_PATH not set)")
        return 0
    if not os.path.isfile(seed_path):
        raise RuntimeError(f"seed config not found: {seed_path}")
    if has_any_users(users_table):
        print("seed: skipped (users already exist)")
        return 0

    config = parse_seed_config(seed_path)
    users_cfg = config.get("users", [])
    if not isinstance(users_cfg, list) or not users_cfg:
        raise RuntimeError("seed config must provide non-empty users array")

    world_cfg = config.get("world", {}) if isinstance(config.get("world", {}), dict) else {}
    width = int(world_cfg.get("width", 120))
    height = int(world_cfg.get("height", 80))
    if width < 20 or height < 20:
        raise RuntimeError("world.width/world.height must both be >= 20")

    treasury_balance = int(config.get("treasury_balance", 400))
    if treasury_balance < 0:
        raise RuntimeError("treasury_balance must be >= 0")

    economy_defaults = {
        "version": 1,
        "k_land": 24,
        "a_productivity": 1.0,
        "v_velocity": 2.0,
        "food_spawn_target": 1,
        "alpha_bootstrap_default": 0.5,
        "cap_delta_m": 5000,
        "delta_m_issue": 0,
        "delta_k_obs": 0,
    }
    eco_cfg = config.get("economy_params", {}) if isinstance(config.get("economy_params", {}), dict) else {}
    economy_defaults.update(eco_cfg)
    ts = int(time.time())

    users_to_write: List[Dict] = []
    snakes_to_write: List[Dict] = []
    used_cells = set()
    used_company_norm = set()
    used_snake_norm = set()
    next_user_id = 1
    next_snake_id = 1

    for u in users_cfg:
        if not isinstance(u, dict):
            raise RuntimeError("each users[] entry must be an object")
        company_name = validate_name(str(u.get("company_name", "")), "company_name")
        company_norm = normalize_name(company_name)
        if company_norm in used_company_norm:
            raise RuntimeError(f"duplicate company_name in seed config: {company_name}")
        used_company_norm.add(company_norm)
        balance_mi = int(u.get("balance_mi", 0))
        if balance_mi < 0:
            raise RuntimeError("balance_mi must be >= 0")
        snakes_cfg = u.get("snakes", [])
        if not isinstance(snakes_cfg, list) or not snakes_cfg:
            raise RuntimeError(f"user {company_name} must define at least one snake")

        user_id = str(next_user_id)
        next_user_id += 1
        starter_snake_id = str(next_snake_id)
        users_to_write.append(
            {
                "user_id": {"S": user_id},
                "balance_mi": {"N": str(balance_mi)},
                "debt_principal": {"N": "0"},
                "debt_interest_rate": {"N": "0"},
                "debt_accrued_interest": {"N": "0"},
                "role": {"S": "player"},
                "created_at": {"N": str(ts)},
                "updated_at": {"N": str(ts)},
                "company_name": {"S": company_name},
                "company_name_normalized": {"S": company_norm},
                "auth_provider": {"S": "google"},
                "onboarding_completed": {"BOOL": True},
                "starter_snake_id": {"S": starter_snake_id},
                "account_status": {"S": "active"},
            }
        )

        for s in snakes_cfg:
            if not isinstance(s, dict):
                raise RuntimeError(f"user {company_name} has invalid snake definition")
            snake_name = validate_name(str(s.get("snake_name", "")), "snake_name")
            snake_norm = normalize_name(snake_name)
            if snake_norm in used_snake_norm:
                raise RuntimeError(f"duplicate snake_name in seed config: {snake_name}")
            used_snake_norm.add(snake_norm)
            length_k = int(s.get("length_k", 1))
            if length_k <= 0:
                raise RuntimeError("snake length_k must be > 0")
            color = str(s.get("color", "#00ff00")).strip() or "#00ff00"
            snake_id = str(next_snake_id)
            next_snake_id += 1
            body = place_snake_body(width, height, used_cells, length_k)
            for cell in body:
                used_cells.add(cell)
            snakes_to_write.append(
                {
                    "snake_id": {"S": snake_id},
                    "owner_user_id": {"S": user_id},
                    "snake_name": {"S": snake_name},
                    "snake_name_normalized": {"S": snake_norm},
                    "alive": {"BOOL": True},
                    "is_on_field": {"BOOL": True},
                    "head_x": {"N": str(body[0][0])},
                    "head_y": {"N": str(body[0][1])},
                    "direction": {"N": "0"},
                    "paused": {"BOOL": False},
                    "length_k": {"N": str(length_k)},
                    "body_compact": {"S": encode_body(body)},
                    "color": {"S": color},
                    "created_at": {"N": str(ts)},
                    "updated_at": {"N": str(ts)},
                }
            )

    created_user_ids: List[str] = []
    created_snake_ids: List[str] = []
    try:
        for user in users_to_write:
            put_item(users_table, user)
            created_user_ids.append(user["user_id"]["S"])
        for snake in snakes_to_write:
            put_item(snakes_table, snake)
            created_snake_ids.append(snake["snake_id"]["S"])

        put_item(
            world_chunks_table,
            {
                "chunk_id": {"S": "main"},
                "width": {"N": str(width)},
                "height": {"N": str(height)},
                "obstacles": {"S": "[]"},
                "food_state": {"S": "[]"},
                "version": {"N": "1"},
                "updated_at": {"N": str(ts)},
            },
        )

        eco_item_common = {
            "version": {"N": str(int(economy_defaults["version"]))},
            "k_land": {"N": str(int(economy_defaults["k_land"]))},
            "a_productivity": {"N": str(float(economy_defaults["a_productivity"]))},
            "v_velocity": {"N": str(float(economy_defaults["v_velocity"]))},
            "food_spawn_target": {"N": str(int(economy_defaults["food_spawn_target"]))},
            "alpha_bootstrap_default": {"N": str(float(economy_defaults["alpha_bootstrap_default"]))},
            "m_gov_reserve": {"N": str(treasury_balance)},
            "cap_delta_m": {"N": str(int(economy_defaults["cap_delta_m"]))},
            "delta_m_issue": {"N": str(int(economy_defaults["delta_m_issue"]))},
            "delta_k_obs": {"N": str(int(economy_defaults["delta_k_obs"]))},
            "updated_at": {"N": str(ts)},
            "updated_by": {"S": "seed_config"},
        }
        put_item(economy_params_table, {"params_id": {"S": "ver#1"}, **eco_item_common})
        put_item(economy_params_table, {"params_id": {"S": "active"}, **eco_item_common})
    except Exception:
        for snake_id in created_snake_ids:
            try:
                delete_item(snakes_table, {"snake_id": {"S": snake_id}})
            except Exception:
                pass
        for user_id in created_user_ids:
            try:
                delete_item(users_table, {"user_id": {"S": user_id}})
            except Exception:
                pass
        raise

    print(
        f"seed: applied from {seed_path} "
        f"(users={len(users_to_write)}, snakes={len(snakes_to_write)}, app_env={app_env})"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"seed: failed: {exc}", file=sys.stderr)
        sys.exit(1)
