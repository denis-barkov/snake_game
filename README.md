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
- `CHUNK_SIZE` (default `64`)
- `AOI_RADIUS` (default `1`)
- `SINGLE_CHUNK_MODE` (`true`/`false`, default `true`)
- `AOI_ENABLED` (`true`/`false`, default `false`)
- `PUBLIC_VIEW_ENABLED` (`true`/`false`, default `true`)
- `PUBLIC_SPECTATOR_HZ` (default `10`)
- `AUTH_SPECTATOR_HZ` (default `10`)
- `PUBLIC_CAMERA_SWITCH_TICKS` (default `600`)
- `PUBLIC_AOI_RADIUS` (default `1`)
- `AUTH_AOI_RADIUS` (default `2`)
- `CAMERA_MSG_MAX_HZ` (default `10`)
- `ECONOMY_CACHE_MS` (default `2000`, min `500`, max `10000`) for `/economy/state` read cache

Default run values in Make:
- `TICK_HZ=10`
- `SPECTATOR_HZ=10`
- `CHUNK_SIZE=64`
- `AOI_RADIUS=1`
- `SINGLE_CHUNK_MODE=true`
- `AOI_ENABLED=false`
- `PUBLIC_VIEW_ENABLED=true`
- `PUBLIC_SPECTATOR_HZ=10`
- `AUTH_SPECTATOR_HZ=10`
- `PUBLIC_CAMERA_SWITCH_TICKS=600`
- `PUBLIC_AOI_RADIUS=1`
- `AUTH_AOI_RADIUS=2`
- `CAMERA_MSG_MAX_HZ=10`

Notes:
- With default rollout flags (`SINGLE_CHUNK_MODE=true`, `AOI_ENABLED=false`) behavior stays equivalent to previous full-world snapshots.
- AOI can be enabled later without changing frontend payload schema.

### Economy v1 (read-only)

- Backend endpoint: `GET /economy/state`
- Frontend HUD polls this endpoint every 2 seconds.
- Economy is computed outside the tick loop and cached in-process to avoid DynamoDB hammering.
- No gameplay rules are changed by economy values in this step (display-only).
- User HUD does not show `period_key`; period remains available in `snakecli economy status`.
- Public health/debug endpoint: `GET /health` returns `{"ok":true}`.

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

# smart progressed-world seed (Step 9.2)
snakecli --token "$ADMIN_TOKEN" smartseed --worldsize 200000 --seed 123 --wipe --force
```

### Smartseed (Step 9.2)

Command:
```bash
snakecli --token "$ADMIN_TOKEN" smartseed --worldsize <A_world> [--usersnum N] [--snakesnum M] [--seed S] [--wipe] [--force]
```

Rules:
- `--worldsize` is required (area in tiles).
- `ADMIN_TOKEN` is required (write command).
- `--wipe` deletes game-content tables (`users`, `snakes`, `snake_events`, `world_chunks`, `economy_params`, `economy_period`).
- `--wipe` requires confirmation unless `--force` is present.
- Existing `app seed` command is unchanged.
- Smartseed users are login-ready immediately (`seeduser...` / `passN` shown in output).
- Smartseed now requests a live server reload after write so new snakes/world appear without manual restart.

Output summary includes:
- target world/economy metrics (`M_target`, `ΣM_i`, `M_G`, `M`, `K`, `A_world`, `M_white`, `P`, `pi`)
- created user/snake counts + snake length min/avg/max
- bounded event count

### Prod parity smoke checks

After deploy, verify public routing:

```bash
curl -sS https://terrariumsnake.com/health
curl -sS https://terrariumsnake.com/economy/state
```

Expected:
- `/health` -> `{"ok":true}`
- `/economy/state` -> JSON with numeric `M`, `P`, `pi`, `A_world`, `M_white`

## Watch camera / AOI-ready behavior

- Frontend stream now uses a stable session id (`sid`) via `/game/stream?sid=...`.
- Frontend optionally posts camera center to `POST /game/camera` (low rate) so backend can apply AOI filtering when enabled.
- Frontend also sends `zoom` with camera updates (schema-compatible, optional).
- In **My Snakes**, a `watch` checkbox appears under each snake:
  - only the selected snake can be watched
  - only one snake can be watched at a time
  - selecting another snake clears previous watch selection
- Old clients remain compatible: if no camera updates are sent, server defaults to center-camera behavior.

### Zoom + debug overlay (Step 9)

- UI controls:
  - `+ Zoom In`
  - `- Zoom Out`
  - `Reset` (returns zoom to `1.0` and default/follow center)
- Zoom bounds: `0.25 .. 4.0`
- Optional debug overlay:
  - add `?debug=1` to URL
  - shows mode (`PUBLIC`/`AUTH`), camera center, zoom, public camera chunk, AOI chunk count, and last snapshot payload size

### Auth-gated camera/zoom (Step 9.1)

- Camera/zoom updates are accepted only for authenticated sessions.
- Unauthenticated visitors receive a server-driven public activity view.
- Public view switches focus periodically using in-memory chunk activity scores (no DB writes).
- `POST /game/camera` is auth-gated and rate-limited by `CAMERA_MSG_MAX_HZ`.
- `GET /game/view` provides debug-safe mode/camera/AOI info for overlays.
- Watch stream broadcast rate is restored to `SPECTATOR_HZ` (default `10 Hz`) for everyone.
- Snapshot streaming is full-world again by default so zoom/camera never changes world visibility.

## Modes

- `./snake_server serve`
- `./snake_server seed`
- `./snake_server reset`

`snakecli` is installed to `/usr/local/bin/snakecli` in runtime environments.

Default seeded users:
- `user1 / pass1`
- `user2 / pass2`
