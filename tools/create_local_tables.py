#!/usr/bin/env python3
import os
import subprocess
import sys


def env(name: str, default: str) -> str:
    return os.environ.get(name, default)


def env_table(suffix: str, default: str) -> str:
    return os.environ.get(f"TABLE_{suffix}") or os.environ.get(f"DYNAMO_TABLE_{suffix}") or default


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


def create_snakes(name: str):
    run(aws_base() + [
        "create-table",
        "--table-name", name,
        "--attribute-definitions",
        "AttributeName=snake_id,AttributeType=S",
        "--key-schema", "AttributeName=snake_id,KeyType=HASH",
        "--billing-mode", "PAY_PER_REQUEST",
    ])


def create_world_chunks(name: str):
    run(aws_base() + [
        "create-table",
        "--table-name", name,
        "--attribute-definitions",
        "AttributeName=chunk_id,AttributeType=S",
        "--key-schema", "AttributeName=chunk_id,KeyType=HASH",
        "--billing-mode", "PAY_PER_REQUEST",
    ])


def create_snake_events(name: str):
    run(aws_base() + [
        "create-table",
        "--table-name", name,
        "--attribute-definitions",
        "AttributeName=snake_id,AttributeType=S",
        "AttributeName=event_id,AttributeType=S",
        "--key-schema",
        "AttributeName=snake_id,KeyType=HASH",
        "AttributeName=event_id,KeyType=RANGE",
        "--billing-mode", "PAY_PER_REQUEST",
    ])


def create_settings(name: str):
    run(aws_base() + [
        "create-table",
        "--table-name", name,
        "--attribute-definitions",
        "AttributeName=settings_id,AttributeType=S",
        "--key-schema", "AttributeName=settings_id,KeyType=HASH",
        "--billing-mode", "PAY_PER_REQUEST",
    ])


def create_economy_params(name: str):
    run(aws_base() + [
        "create-table",
        "--table-name", name,
        "--attribute-definitions",
        "AttributeName=params_id,AttributeType=S",
        "--key-schema", "AttributeName=params_id,KeyType=HASH",
        "--billing-mode", "PAY_PER_REQUEST",
    ])


def create_economy_period(name: str):
    run(aws_base() + [
        "create-table",
        "--table-name", name,
        "--attribute-definitions",
        "AttributeName=period_key,AttributeType=S",
        "--key-schema", "AttributeName=period_key,KeyType=HASH",
        "--billing-mode", "PAY_PER_REQUEST",
    ])


def main():
    users = env_table("USERS", "snake-local-users")
    snakes = env_table("SNAKES", "snake-local-snakes")
    world_chunks = env_table("WORLD_CHUNKS", "snake-local-world_chunks")
    snake_events = env_table("SNAKE_EVENTS", "snake-local-snake_events")
    settings = env_table("SETTINGS", "snake-local-settings")
    economy_params = env_table("ECONOMY_PARAMS", "snake-local-economy_params")
    economy_period = env_table("ECONOMY_PERIOD", "snake-local-economy_period")

    creators = [
        (users, create_users),
        (snakes, create_snakes),
        (world_chunks, create_world_chunks),
        (snake_events, create_snake_events),
        (settings, create_settings),
        (economy_params, create_economy_params),
        (economy_period, create_economy_period),
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
