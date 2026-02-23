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

### Economy v1 write paths (Step 5)

- Purchase write endpoint: `POST /economy/purchase` (auth required)
  - body: `{"cells": <positive_int>}` (or `purchased_cells`)
  - writes:
    - `users.balance_mi += cells`
    - `economy_period[YYYYMMDDHH].delta_m_buy += cells`
- Economy params now use:
  - active row: `params_id=active`
  - history rows: `params_id=ver#<version>`

Storage env naming:
- Server and `snakecli` accept both naming styles:
  - `TABLE_*` (preferred)
  - `DYNAMO_TABLE_*` (legacy-compatible)
- Endpoint env supports both:
  - `DDB_ENDPOINT` (preferred)
  - `DYNAMO_ENDPOINT` (legacy-compatible)

## Local DynamoDB (Docker)

### Quick (Make)

```bash
make local-setup
make local-run
```

`make local-run` runs fully in Docker and talks to local DynamoDB.  
It publishes app on `http://127.0.0.1:8080`.
`make local-run` stays in foreground by design (it is the running server). Open the app in browser while it is running; stop with `Ctrl+C`.

`make local-run-docker` remains available explicitly.

Reset local data:
```bash
docker exec -it snake-local-run bash
export ADMIN_TOKEN=devtoken
snakecli --token "$ADMIN_TOKEN" app reset-seed-reload
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
export ENV=local
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

### Admin CLI (`snakecli`)

`snakecli` runs where the app runs (same runtime, same env, same credentials).

Local access pattern (Docker runtime shell):
```bash
docker exec -it snake-local-run bash
snakecli help
```

Prod access pattern (AWS EC2 shell):
```bash
ssh -i <your-key>.pem ec2-user@<ec2-ip>
snakecli help
```

`snakecli` command cheat sheet (same in local/prod):
```bash
export ADMIN_TOKEN=your-secret-token

snakecli economy status
snakecli --token "$ADMIN_TOKEN" economy set cap_delta_m 6000
snakecli --token "$ADMIN_TOKEN" economy recompute
snakecli firms top --by balance --limit 10
snakecli snakes list --onfield --limit 25

# app-level admin actions
snakecli --token "$ADMIN_TOKEN" app seed
snakecli --token "$ADMIN_TOKEN" app reset-seed-reload
```

## Modes

- `./snake_server serve`
- `./snake_server seed`
- `./snake_server reset`

`snakecli` is installed to `/usr/local/bin/snakecli` in runtime environments.

Default seeded users:
- `user1 / pass1`
- `user2 / pass2`
