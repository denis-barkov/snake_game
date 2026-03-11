#!/usr/bin/env python3
"""
Minimal API smoke test for auth/onboarding/borrow/attach flow.

Usage:
  python3 tools/smoke_economy_flow.py --base-url http://127.0.0.1:8080 --token <bearer-token>
"""

from __future__ import annotations

import argparse
import json
import random
import string
import sys
import time
import urllib.error
import urllib.request


def req(base_url: str, path: str, method: str = "GET", token: str | None = None, body: dict | None = None):
    payload = None
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    if body is not None:
        payload = json.dumps(body).encode("utf-8")
    request = urllib.request.Request(
        url=f"{base_url.rstrip('/')}{path}",
        method=method,
        data=payload,
        headers=headers,
    )
    try:
        with urllib.request.urlopen(request, timeout=15) as resp:
            raw = resp.read().decode("utf-8")
            data = json.loads(raw) if raw else {}
            return resp.status, data, raw
    except urllib.error.HTTPError as exc:
        raw = exc.read().decode("utf-8")
        data = {}
        try:
            data = json.loads(raw) if raw else {}
        except Exception:
            pass
        return exc.code, data, raw


def random_name(prefix: str) -> str:
    suffix = "".join(random.choices(string.ascii_lowercase + string.digits, k=8))
    return f"{prefix}{suffix}"


def require(ok: bool, msg: str):
    if not ok:
        print(f"FAIL: {msg}")
        sys.exit(1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="http://127.0.0.1:8080")
    parser.add_argument("--token", required=True, help="Bearer token from Google-authenticated session")
    args = parser.parse_args()
    token = str(args.token).strip()
    require(len(token) > 0, "missing --token")

    status, me_probe, raw = req(args.base_url, "/user/me", token=token)
    require(status == 200, f"auth probe failed status={status} body={raw}")
    onboarding_required = bool(me_probe.get("onboarding_completed") is False)

    if onboarding_required:
        company = random_name("Firm")
        snake = random_name("Snake")
        status, _, raw = req(
            args.base_url,
            "/onboarding/complete",
            method="POST",
            token=token,
            body={"company_name": company, "snake_name": snake},
        )
        require(status == 200, f"onboarding failed status={status} body={raw}")
        time.sleep(0.2)

    status, me_before, raw = req(args.base_url, "/user/me", token=token)
    require(status == 200, f"/user/me failed status={status} body={raw}")
    bal_before = int(me_before.get("liquid_assets", me_before.get("balance_mi", 0)))

    status, snakes_before, raw = req(args.base_url, "/me/snakes", token=token)
    require(status == 200, f"/me/snakes failed status={status} body={raw}")
    snakes = snakes_before.get("snakes", [])
    require(len(snakes) > 0, "starter snake missing after login/onboarding")
    snake_id = int(snakes[0]["id"])
    snake_len_before = int(snakes[0].get("len", 0))

    status, borrow_data, raw = req(
        args.base_url,
        "/user/borrow",
        method="POST",
        token=token,
        body={"amount": 1},
    )
    require(status == 200 and borrow_data.get("ok") is True, f"borrow failed status={status} body={raw}")

    status, me_after_borrow, raw = req(args.base_url, "/user/me", token=token)
    require(status == 200, f"/user/me after borrow failed status={status} body={raw}")
    bal_after_borrow = int(me_after_borrow.get("liquid_assets", me_after_borrow.get("balance_mi", 0)))
    require(bal_after_borrow == bal_before + 1, f"borrow balance mismatch expected {bal_before + 1} got {bal_after_borrow}")

    status, attach_data, raw = req(
        args.base_url,
        f"/snake/{snake_id}/attach",
        method="POST",
        token=token,
        body={"amount": 1},
    )
    require(status == 200 and attach_data.get("ok") is True, f"attach failed status={status} body={raw}")

    status, me_after_attach, raw = req(args.base_url, "/user/me", token=token)
    require(status == 200, f"/user/me after attach failed status={status} body={raw}")
    bal_after_attach = int(me_after_attach.get("liquid_assets", me_after_attach.get("balance_mi", 0)))
    require(bal_after_attach == bal_after_borrow - 1, f"attach balance mismatch expected {bal_after_borrow - 1} got {bal_after_attach}")

    status, snakes_after, raw = req(args.base_url, "/me/snakes", token=token)
    require(status == 200, f"/me/snakes after attach failed status={status} body={raw}")
    snake_after = None
    for s in snakes_after.get("snakes", []):
        if int(s["id"]) == snake_id:
            snake_after = s
            break
    require(snake_after is not None, "starter snake missing after attach")
    snake_len_after = int(snake_after.get("len", 0))
    require(snake_len_after == snake_len_before + 1, f"snake length mismatch expected {snake_len_before + 1} got {snake_len_after}")

    print("PASS: smoke_economy_flow")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
