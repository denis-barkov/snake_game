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
            f"\"created_at\":{{\"N\":\"{int(time.time())}\"}}"
            "}"
        ),
    ])


def put_snake(table: str, snake_id: str, owner_user_id: str, x: int, y: int):
    run(aws_base() + [
        "put-item",
        "--table-name", table,
        "--item", (
            "{"
            f"\"snake_id\":{{\"S\":\"{snake_id}\"}},"
            f"\"ts\":{{\"N\":\"{int(time.time() * 1000)}\"}},"
            f"\"owner_user_id\":{{\"S\":\"{owner_user_id}\"}},"
            "\"dir\":{\"N\":\"0\"},"
            "\"paused\":{\"BOOL\":false},"
            f"\"body\":{{\"S\":\"[[{x},{y}]]\"}},"
            "\"length\":{\"N\":\"1\"},"
            "\"score\":{\"N\":\"1\"},"
            "\"w\":{\"N\":\"40\"},"
            "\"h\":{\"N\":\"20\"}"
            "}"
        ),
    ])


def main():
    users = env("DYNAMO_TABLE_USERS", "snake-local-users")
    snake = env("DYNAMO_TABLE_SNAKE_CHECKPOINTS", "snake-local-snake_checkpoints")

    put_user(users, "1", "user1", "pass1")
    put_user(users, "2", "user2", "pass2")
    put_snake(snake, "1", "1", 5, 5)
    put_snake(snake, "2", "2", 15, 10)
    print("Seed complete: user1/pass1, user2/pass2")


if __name__ == "__main__":
    main()
