#!/usr/bin/env python3
import os
import subprocess
import time


def env(name: str, default: str) -> str:
    return os.environ.get(name, default)


def run(cmd):
    subprocess.run(cmd, check=True, text=True)


def aws_base():
    region = env("AWS_REGION", "us-east-1")
    endpoint = env("DYNAMO_ENDPOINT", "http://127.0.0.1:8000")
    return ["aws", "--region", region, "--endpoint-url", endpoint, "dynamodb"]


def put_user(table: str, user_id: str, username: str, password: str):
    run(aws_base() + [
        "put-item",
        "--table-name", table,
        "--item", (
            "{"
            f"\"user_id\":{{\"S\":\"{user_id}\"}},"
            f"\"username\":{{\"S\":\"{username}\"}},"
            f"\"password_hash\":{{\"S\":\"{password}\"}},"
            "\"balance_mi\":{\"N\":\"0\"},"
            "\"role\":{\"S\":\"player\"},"
            f"\"created_at\":{{\"N\":\"{int(time.time())}\"}}"
            "}"
        ),
    ])


def put_snake(table: str, snake_id: str, owner_user_id: str, x: int, y: int):
    ts = int(time.time())
    run(aws_base() + [
        "put-item",
        "--table-name", table,
        "--item", (
            "{"
            f"\"snake_id\":{{\"S\":\"{snake_id}\"}},"
            f"\"owner_user_id\":{{\"S\":\"{owner_user_id}\"}},"
            "\"alive\":{\"BOOL\":true},"
            f"\"head_x\":{{\"N\":\"{x}\"}},"
            f"\"head_y\":{{\"N\":\"{y}\"}},"
            "\"direction\":{\"N\":\"0\"},"
            "\"paused\":{\"BOOL\":false},"
            "\"length_k\":{\"N\":\"1\"},"
            f"\"body_compact\":{{\"S\":\"[[{x},{y}]]\"}},"
            "\"color\":{\"S\":\"#00ff00\"},"
            f"\"created_at\":{{\"N\":\"{ts}\"}},"
            f"\"updated_at\":{{\"N\":\"{ts}\"}}"
            "}"
        ),
    ])


def put_world_chunk(table: str):
    ts = int(time.time())
    run(aws_base() + [
        "put-item",
        "--table-name", table,
        "--item", (
            "{"
            "\"chunk_id\":{\"S\":\"main\"},"
            "\"width\":{\"N\":\"40\"},"
            "\"height\":{\"N\":\"20\"},"
            "\"obstacles\":{\"S\":\"[]\"},"
            "\"food_state\":{\"S\":\"[[10,10]]\"},"
            "\"version\":{\"N\":\"1\"},"
            f"\"updated_at\":{{\"N\":\"{ts}\"}}"
            "}"
        ),
    ])


def main():
    users = env("DYNAMO_TABLE_USERS", "snake-local-users")
    snakes = env("DYNAMO_TABLE_SNAKES", "snake-local-snakes")
    world_chunks = env("DYNAMO_TABLE_WORLD_CHUNKS", "snake-local-world_chunks")

    put_user(users, "1", "user1", "pass1")
    put_user(users, "2", "user2", "pass2")
    put_snake(snakes, "1", "1", 5, 5)
    put_snake(snakes, "2", "2", 15, 10)
    put_world_chunk(world_chunks)
    print("Seed complete: user1/pass1, user2/pass2")


if __name__ == "__main__":
    main()
