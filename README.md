## Snake Game

### Build

Required libs:
- `boost`
- `aws-sdk-cpp` (DynamoDB + core)
- single-header `cpp-httplib` in `api/httplib.h`

macOS install example:
```bash
brew install boost aws-sdk-cpp
```

If you want to avoid local installs, use Docker local runner:
```bash
make local-run-docker
```

Example build:
```bash
clang++ -std=c++17 -O2 -pthread \
  api/snake_server.cpp \
  api/protocol/encode_json.cpp \
  api/storage/dynamo_storage.cpp \
  api/storage/storage_factory.cpp \
  config/runtime_config.cpp \
  -lboost_system -laws-cpp-sdk-dynamodb -laws-cpp-sdk-core \
  -o snake_server
```

### Protocol source of truth

Snapshot JSON protocol is defined in `api/protocol`.
Do not inline snapshot JSON shapes in server code.

### Runtime Hz config

- `TICK_HZ` (default `10`, min `5`, max `60`)
- `SPECTATOR_HZ` (default `10`, min `1`, max `60`)
- `PLAYER_HZ` (placeholder, currently unused)
- `ENABLE_BROADCAST` (`true`/`false`, default `true`)
- `LOG_HZ` (`true`/`false`, default `true`)

Default run values in Make:
- `TICK_HZ=10`
- `SPECTATOR_HZ=10`

## Local DynamoDB (Docker)

### Quick (Make)

```bash
make local-setup
make local-run
```

`make local-run-docker` builds/runs the server inside Docker and talks to local DynamoDB.

Reset local data:
```bash
make local-reset
make local-seed
```

Local admin CLI (mirrors EC2 verbs):
```bash
make local-admin CMD=reload
make local-admin CMD=seed-reload
make local-admin CMD=reset-seed-reload
```

Shortcuts:
```bash
make local-reload
make local-seed-reload
make local-reset-seed-reload
```

Stop local Dynamo:
```bash
make local-dynamo-down
```

### Full commands

```bash
docker compose -f docker/dynamodb-local.yml up -d

export DYNAMO_ENDPOINT=http://127.0.0.1:8000
export AWS_REGION=us-east-1
export DYNAMO_REGION=us-east-1
export AWS_ACCESS_KEY_ID=local
export AWS_SECRET_ACCESS_KEY=local

export DYNAMO_TABLE_USERS=snake-local-users
export DYNAMO_TABLE_SNAKE_CHECKPOINTS=snake-local-snake_checkpoints
export DYNAMO_TABLE_EVENT_LEDGER=snake-local-event_ledger
export DYNAMO_TABLE_SETTINGS=snake-local-settings

python3 tools/create_local_tables.py
python3 tools/seed_local.py

./snake_server serve
```

## AWS DynamoDB (EC2)

Use instance role (no `DYNAMO_ENDPOINT`).

```bash
export AWS_REGION=us-east-1
export DYNAMO_REGION=us-east-1
export DYNAMO_TABLE_USERS=snake-mvp-users
export DYNAMO_TABLE_SNAKE_CHECKPOINTS=snake-mvp-snake_checkpoints
export DYNAMO_TABLE_EVENT_LEDGER=snake-mvp-event_ledger
export DYNAMO_TABLE_SETTINGS=snake-mvp-settings

./snake_server serve
```

### Seed on AWS via SSH

On the EC2 host:

```bash
sudo snake-admin reset-seed-reload
sudo systemctl status snake --no-pager
```

Seed only (without reset):

```bash
sudo snake-admin seed-reload
```

Systemd alternatives:

```bash
sudo systemctl start snake-seed
sudo systemctl start snake-reset
sudo systemctl start snake-reset-seed
sudo systemctl start snake-reload
sudo systemctl start snake-seed-reload
```

## Modes

- `./snake_server serve`
- `./snake_server seed`
- `./snake_server reset`

Admin commands are shared between local and AWS via `tools/snake-admin.sh`.

Default seeded users:
- `user1 / pass1`
- `user2 / pass2`
