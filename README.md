## Snake Game

### Build

Preferred build (no host SDK installs needed):
```bash
make local-build
```

`make local-build` compiles `snake_server` inside Docker and writes the binary to this repo.

### Protocol source of truth

Snapshot JSON protocol is defined in `api/protocol`.
Do not inline snapshot JSON shapes in server code.

Simulation internals are structured in `api/world`:
- `World` owns state
- `systems/*` mutate state each tick
- server/network layer only queues intents and reads snapshots

### Runtime Hz config

- `TICK_HZ` (default `10`, min `5`, max `60`)
- `SPECTATOR_HZ` (default `10`, min `1`, max `60`)
- `PLAYER_HZ` (placeholder, currently unused)
- `ENABLE_BROADCAST` (`true`/`false`, default `true`)
- `DEBUG_TPS` (`true`/`false`, default `false`)
- `ECONOMY_CACHE_MS` (default `2000`, min `500`, max `10000`) for `/economy/state` read cache

Default run values in Make:
- `TICK_HZ=10`
- `SPECTATOR_HZ=10`

### Economy v1 (read-only)

- Backend endpoint: `GET /economy/state`
- Frontend HUD polls this endpoint every 2 seconds.
- Economy is computed outside the tick loop and cached in-process to avoid DynamoDB hammering.
- No gameplay rules are changed by economy values in this step (display-only).

## Local DynamoDB (Docker)

### Quick (Make)

```bash
make local-setup
make local-run
```

`make local-run` runs fully in Docker and talks to local DynamoDB.  
It publishes app on `http://127.0.0.1:8080`.
`make local-run` stays in foreground by design (it is the running server). Open the app in browser while it is running; stop with `Ctrl+C`.

`make local-seed`, `make local-reset`, and `make local-admin ...` also run inside Docker (same runtime as `local-run`) to avoid host binary/SDK mismatch.

`make local-run-docker` remains available explicitly.

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
export DYNAMO_TABLE_SNAKES=snake-local-snakes
export DYNAMO_TABLE_WORLD_CHUNKS=snake-local-world_chunks
export DYNAMO_TABLE_SNAKE_EVENTS=snake-local-snake_events
export DYNAMO_TABLE_SETTINGS=snake-local-settings
export DYNAMO_TABLE_ECONOMY_PARAMS=snake-local-economy_params
export DYNAMO_TABLE_ECONOMY_PERIOD=snake-local-economy_period

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
export DYNAMO_TABLE_SNAKES=snake-mvp-snakes
export DYNAMO_TABLE_WORLD_CHUNKS=snake-mvp-world_chunks
export DYNAMO_TABLE_SNAKE_EVENTS=snake-mvp-snake_events
export DYNAMO_TABLE_SETTINGS=snake-mvp-settings
export DYNAMO_TABLE_ECONOMY_PARAMS=snake-mvp-economy_params
export DYNAMO_TABLE_ECONOMY_PERIOD=snake-mvp-economy_period

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
