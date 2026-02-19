#!/usr/bin/env python3
import os
import subprocess
import sys


def env(name: str, default: str) -> str:
    return os.environ.get(name, default)


def run(cmd):
    return subprocess.run(cmd, check=False, text=True, capture_output=True)


def aws_base():
    region = env("AWS_REGION", "us-east-1")
    endpoint = env("DYNAMO_ENDPOINT", "http://127.0.0.1:8000")
    return ["aws", "--region", region, "--endpoint-url", endpoint, "dynamodb"]


def table_exists(name: str) -> bool:
    r = run(aws_base() + ["describe-table", "--table-name", name])
    return r.returncode == 0


def create_users(name: str):
    run(aws_base() + [
        "create-table",
        "--table-name", name,
        "--attribute-definitions",
        "AttributeName=user_id,AttributeType=S",
        "AttributeName=username,AttributeType=S",
        "--key-schema", "AttributeName=user_id,KeyType=HASH",
        "--global-secondary-indexes",
        "IndexName=gsi_username,KeySchema=[{AttributeName=username,KeyType=HASH}],Projection={ProjectionType=ALL}",
        "--billing-mode", "PAY_PER_REQUEST",
    ])


def create_snake_checkpoints(name: str):
    run(aws_base() + [
        "create-table",
        "--table-name", name,
        "--attribute-definitions",
        "AttributeName=snake_id,AttributeType=S",
        "AttributeName=ts,AttributeType=N",
        "--key-schema",
        "AttributeName=snake_id,KeyType=HASH",
        "AttributeName=ts,KeyType=RANGE",
        "--billing-mode", "PAY_PER_REQUEST",
    ])


def create_event_ledger(name: str):
    run(aws_base() + [
        "create-table",
        "--table-name", name,
        "--attribute-definitions",
        "AttributeName=pk,AttributeType=S",
        "AttributeName=sk,AttributeType=S",
        "--key-schema",
        "AttributeName=pk,KeyType=HASH",
        "AttributeName=sk,KeyType=RANGE",
        "--billing-mode", "PAY_PER_REQUEST",
    ])


def create_settings(name: str):
    run(aws_base() + [
        "create-table",
        "--table-name", name,
        "--attribute-definitions",
        "AttributeName=pk,AttributeType=S",
        "AttributeName=sk,AttributeType=S",
        "--key-schema",
        "AttributeName=pk,KeyType=HASH",
        "AttributeName=sk,KeyType=RANGE",
        "--billing-mode", "PAY_PER_REQUEST",
    ])


def main():
    users = env("DYNAMO_TABLE_USERS", "snake-local-users")
    snake = env("DYNAMO_TABLE_SNAKE_CHECKPOINTS", "snake-local-snake_checkpoints")
    events = env("DYNAMO_TABLE_EVENT_LEDGER", "snake-local-event_ledger")
    settings = env("DYNAMO_TABLE_SETTINGS", "snake-local-settings")

    creators = [
        (users, create_users),
        (snake, create_snake_checkpoints),
        (events, create_event_ledger),
        (settings, create_settings),
    ]

    for table_name, creator in creators:
        if table_exists(table_name):
            print(f"Table exists: {table_name}")
            continue
        print(f"Creating table: {table_name}")
        creator(table_name)
        r = run(aws_base() + ["wait", "table-exists", "--table-name", table_name])
        if r.returncode != 0:
            print(r.stderr, file=sys.stderr)
            sys.exit(r.returncode)

    print("Local DynamoDB tables are ready.")


if __name__ == "__main__":
    main()
