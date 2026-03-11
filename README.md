## Snake Game

### Build

Preferred build (no host SDK installs needed):
```bash
make local-build
```

`make local-build` compiles `snake_server` inside Docker and writes the binary to this repo.
It also generates `assets/world_evolution_log.json` from `CHANGELOG.md`.

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
- `MAX_BORROW_PER_CALL` (default `1000000`)
- `FOOD_REWARD_CELLS` (default `1`)
- `RESIZE_THRESHOLD` (default `0.05`)
- `WORLD_ASPECT_RATIO` (default `1.7777777778`)
- `ECON_PERIOD_SECONDS` (default `300` local, `86400` prod)
- `ECON_PERIOD_TZ` (default `America/New_York`)
- `ECON_PERIOD_ALIGN` (`rolling` local, `midnight` prod)
- `ECONOMIC_PERIOD_DURATION_SECONDS` (alias for period seconds)
- `ECONOMIC_PERIOD_MODE` (`fixed_seconds` local, `prod_midnight_nyc` prod)
- `ECONOMY_FLUSH_SECONDS` (default `10`, buffered raw economy flush interval)
- `ECONOMY_PERIOD_HISTORY_DAYS` (default `90`, retention policy knob)
- `AUTO_EXPANSION_ENABLED` (default `true`)
- `AUTO_EXPANSION_TRIGGER_RATIO` (default `2.0`)
- `TARGET_SPATIAL_RATIO` (default `3.2`)
- `AUTO_EXPANSION_CHECKS_PER_PERIOD` (default `48`)
- `TARGET_LCR` (default `1.2`)
- `LCR_STRESS_THRESHOLD` (default `0.7`)
- `MAX_AUTO_MONEY_GROWTH` (default `0.08`)
- `ECONOMY_CACHE_MS` (default `2000`, min `500`, max `10000`) for `/economy/state` read cache
- `PERSISTENCE_PROFILE` (`minimal|standard|payments_safe|strict`, default `minimal`)
- `PERSISTENCE_SQLITE_PATH` (default `/var/lib/snake/persistence.db`)
- `PERSISTENCE_SQLITE_MAX_MB` (default `256`)
- `PERSISTENCE_SQLITE_RETENTION_HOURS` (default `72`)
- `PERSISTENCE_FLUSH_CHUNKS_SECONDS` (default `2`)
- `PERSISTENCE_FLUSH_SNAPSHOTS_SECONDS` (default `10`)
- `PERSISTENCE_FLUSH_PERIOD_DELTAS_SECONDS` (default `10`)
- `PERSISTENCE_RETRY_BACKOFF_MS` (default `250`)
- `PERSISTENCE_DEBUG_LOGGING` (`0|1`, default `0`)
- `GOOGLE_AUTH_ENABLED` (`true`/`false`, default `true` in infra defaults)
- `GOOGLE_CLIENT_ID` (Google Web OAuth client id, required when Google auth is enabled)
- `STARTER_LIQUID_ASSETS` (default `25`)
- `SEED_ENABLED` (`true`/`false`, default `false`)
- `SEED_CONFIG_PATH` (path to a static seed config file; JSON-compatible YAML)
- `APP_ENV` (`local|staging|prod`; prod requires `SEED_ENABLED=true` to apply seed)

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
- `MAX_BORROW_PER_CALL=1000000`
- `FOOD_REWARD_CELLS=1`
- `RESIZE_THRESHOLD=0.05`
- `WORLD_ASPECT_RATIO=1.7777777778`

### Google Sign-In Setup

The v1 public auth flow is Google Sign-In only.

1. In Google Cloud Console, create an OAuth client of type **Web application**.
2. Configure **Authorized JavaScript origins**:
   - Local: `http://127.0.0.1:8080` and/or `http://localhost:8080`
   - Prod: `https://terrariumsnake.com`
3. If your Google project setup asks for redirect URIs, add your deployed auth return URL(s) used by your environment.
4. Set env vars:
   - `GOOGLE_AUTH_ENABLED=true`
   - `GOOGLE_CLIENT_ID=<your-google-web-client-id>`
   - optional starter economy amount: `STARTER_LIQUID_ASSETS=25`
5. Restart the server process (`snake` systemd on prod, or `make local-run` locally).

Notes:
- Backend verifies Google ID tokens and then issues the game’s own session token.
- Password-based login has been fully removed; Google OAuth is the only auth path.
- You can (and usually should) use separate OAuth clients for local and prod.

Makefile split knobs:
- Local runtime:
  - `GAME_GOOGLE_AUTH_ENABLED_LOCAL`
  - `GAME_GOOGLE_CLIENT_ID_LOCAL`
- AWS deploy:
  - `GAME_GOOGLE_AUTH_ENABLED_PROD`
  - `GAME_GOOGLE_CLIENT_ID_PROD`

Examples:
```bash
# local
make local-run GAME_GOOGLE_AUTH_ENABLED_LOCAL=true GAME_GOOGLE_CLIENT_ID_LOCAL="<local-web-client-id>"

# aws deploy
make aws-code-deploy BRANCH=main \
  GAME_GOOGLE_AUTH_ENABLED_PROD=true \
  GAME_GOOGLE_CLIENT_ID_PROD="<prod-web-client-id>"
```

Notes:
- With default rollout flags (`SINGLE_CHUNK_MODE=true`, `AOI_ENABLED=false`) behavior stays equivalent to previous full-world snapshots.
- AOI can be enabled later without changing frontend payload schema.

### SSL Certificate Setup (AWS ACM)

Terraform now consumes an existing ACM certificate (it does not create certificates).

1. Create certificate (once per env/domain):
```bash
make ssl-cert-create ENV=prod DOMAIN=terrariumsnake.com
# optional wildcard SAN (*.domain + root domain):
make ssl-cert-create ENV=prod DOMAIN=terrariumsnake.com WILDCARD=true
```
2. `ssl-cert-create` now auto-creates/UPSERTs ACM DNS validation CNAME records in Route53 and waits up to 3 minutes for `ISSUED`.
3. Run infrastructure apply/deploy:
```bash
make aws-apply BRANCH=main
```
(`aws-plan`/`aws-apply` now run a certificate pre-check and fail fast with a clear message if cert is missing or not `ISSUED`.)

Notes:
- Certificates are tagged and discovered by:
  - `project=snake-game`
  - `environment=<ENV>`
  - `domain=<DOMAIN>`
- Delete a certificate explicitly if needed:
```bash
make ssl-cert-delete ENV=prod DOMAIN=terrariumsnake.com
```
- ACM auto-renews certificates roughly every ~198 days; renewal starts around ~45 days before expiry.
- Renewal remains automatic only while DNS validation records stay present.

### Persistence matrix

Persistence is routed through `api/persistence` using typed intents and an active profile:

- Layer 0: runtime in-memory (`RuntimeStateStore`)
- Layer 1: buffered SQLite (`BufferedSqliteStore`)
- Layer 2: permanent DynamoDB (`PermanentDynamoStore`)

Routing is selected by `PERSISTENCE_PROFILE`:

- `minimal`: strongest Dynamo cost reduction; non-critical data buffered in SQLite
- `standard`: more frequent buffered flushes
- `payments_safe`: critical economic mutations direct to Dynamo with tighter flushes
- `strict`: highest durability (more direct+buffered writes)

Current phase defaults:

- hot-path/non-critical (`snake_events`, dirty world chunks, non-critical snake snapshots) -> SQLite buffered then async flush
- critical value-transfer actions (`user balance`, `snake creation/extension/settlement`, finalized period aggregates) -> direct/near-immediate Dynamo by policy

Gameplay code emits intents through `PersistenceCoordinator`; it does not write SQLite/Dynamo directly for migrated paths.

### Economy core v1

- Backend endpoints:
  - `GET /economy/state` (global macro metrics + countdown)
  - `GET /economy/user` (auth, personal metrics)
  - `GET /economy/debug` (admin token required; raw counters + flush state)
- Frontend economy panels are WS-driven (`economy_world` and `user_state.economy_user`) with no periodic `/economy/state` polling.
- Compatibility aliases in payloads:
  - `liquid_assets` mirrors `balance_mi`
  - `extracted_output` mirrors global `Y`
  - `extracted_output_u` mirrors personal `Y_u`
  - Legacy fields remain present for backward compatibility.

### World Evolution Log

- Source of truth: `CHANGELOG.md` (main-branch release history).
- Build/deploy generates: `assets/world_evolution_log.json` via:
  - `python3 tools/generate_world_evolution_log.py --input CHANGELOG.md --output assets/world_evolution_log.json`
- In-game UX:
  - top-right `Version X.Y.Z` badge
  - `World Evolution Log` panel (latest 10 by default, full archive on demand)
  - one-time per-user modal after login when a new version is unseen
- Backend user field:
  - `users.last_seen_world_version`
  - update endpoint: `POST /user/world-version-seen` with `{ "version": "X.Y.Z" }` (auth required)

### Economy/game write paths (Step 10)

- `GET /user/me` (auth) returns `balance_mi` (`liquid_assets` alias), deployed capital, and snake count.
- `POST /user/borrow` (auth) with `{ "amount": <int> }` credits user liquid assets and debits treasury by the same amount.
  - reject codes: `invalid_amount`, `insufficient_treasury`, `persistence_write_failed`, `unauthorized`, `user_not_found`, `internal_error`
- `POST /snake/{snake_id}/attach` (auth) with `{ "amount": <int> }` moves cells from storage to selected snake.
  - if snake id is missing/not-owned/not-attachable, request fails and does **not** create any snake.
- `POST /me/snakes` (auth) with `{ "snake_name": "...", "color": "#RRGGBB" }` creates a new named snake.
- `POST /snake/{snake_id}/rename` (auth) with `{ "snake_name": "..." }` renames an owned snake.
- `POST /economy/purchase` remains as an alias of `/user/borrow` for compatibility.

UI terminology:
- User-facing label `Storage/Balance` is shown as `Liquid Assets`.
- Snake cards show `Deployed Capital: <cells>` as the per-snake capital proxy.
- Economy panels use `Extracted Output (Y)` naming while gameplay still uses food mechanics.
- Food is treated as extracted output: harvesting mints new value into the economy and does not debit treasury.

Gameplay update:
- Food events credit owner storage (`FOOD_REWARD_CELLS`) instead of auto-growing snake length.
- Snake-vs-snake collisions are bounded: max 1 cell loss per snake per tick.
- Tail-hit: attacker pauses, victim loses 1, attacker owner gains 1 storage cell.
- Head-oncoming (same-next-cell or swap): both lose 1 to system reserve and reverse direction.
- Side head-hit: both pause, then duel resolves once after ~1 second; winner gains 1 storage cell.
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
`make local-setup` intentionally leaves local data unseeded (prod-like empty start).
For explicit local seed data setup, use `make local-setup-seeded`.

SQLite buffered persistence survives container restarts through a host-mounted path:
- host: `.local/persistence`
- container: `/var/lib/snake`
- DB file: `/var/lib/snake/persistence.db`

`make local-run-docker` remains available explicitly.

Reset local data:
```bash
docker exec -it snake-local-run bash
export ADMIN_TOKEN=devtoken
snakecli --token "$ADMIN_TOKEN" app reset
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
export DYNAMO_TABLE_ECONOMY_PERIOD_USER=snake-local-economy_period_user

python3 tools/create_local_tables.py

# Optional static seed (only on empty DB)
SEED_ENABLED=true APP_ENV=local SEED_CONFIG_PATH=./seeds/dev.seed.yaml python3 tools/apply_seed_config.py

./snake_server serve
```

## AWS DynamoDB (EC2)

Use instance role (no `DYNAMO_ENDPOINT`).
Instance role must include `dynamodb:TransactWriteItems` in addition to standard DynamoDB read/write actions.

```bash
export AWS_REGION=us-east-1
export DYNAMO_REGION=us-east-1
export DYNAMO_TABLE_USERS=snake-game-prod-users
export DYNAMO_TABLE_SNAKES=snake-game-prod-snakes
export DYNAMO_TABLE_WORLD_CHUNKS=snake-game-prod-world_chunks
export DYNAMO_TABLE_SNAKE_EVENTS=snake-game-prod-snake_events
export DYNAMO_TABLE_SETTINGS=snake-game-prod-settings
export DYNAMO_TABLE_ECONOMY_PARAMS=snake-game-prod-economy_params
export DYNAMO_TABLE_ECONOMY_PERIOD=snake-game-prod-economy_period
export DYNAMO_TABLE_ECONOMY_PERIOD_USER=snake-game-prod-economy_period_user

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

### Prod server start/restart (AWS EC2)

From an EC2 shell:
```bash
ssh -i <your-key>.pem ec2-user@<ec2-ip>
```

Check status:
```bash
sudo systemctl status snake --no-pager
```

Start server:
```bash
sudo systemctl start snake
```

Restart server:
```bash
sudo systemctl restart snake
```

Tail server logs:
```bash
sudo journalctl -u snake -f
```

If public HTTPS is not reachable, also check proxy services:
```bash
sudo systemctl status caddy --no-pager || true
sudo systemctl status nginx --no-pager || true
```

`snakecli` command cheat sheet (same in local/prod):
```bash
export ADMIN_TOKEN=your-secret-token
export SNAKECLI_API=http://127.0.0.1:8080

snakecli economy status
snakecli --token "$ADMIN_TOKEN" economy set cap_delta_m 6000
snakecli --token "$ADMIN_TOKEN" economy recompute
snakecli --token "$ADMIN_TOKEN" economy recompute --force-rewrite
snakecli --token "$ADMIN_TOKEN" treasury set 1200
snakecli firms top --by balance --limit 10
snakecli snakes list --onfield --limit 25

# app-level admin actions
snakecli --token "$ADMIN_TOKEN" app reset
snakecli --token "$ADMIN_TOKEN" app reload
```

Notes:
- `snakecli economy ...` and `snakecli treasury ...` now call admin HTTP endpoints on the running server (`SNAKECLI_API`), so server must be running.
- `firms top` and `snakes list` access DynamoDB directly.

### Static Seed Config

Smartseed and runtime seed endpoints were removed. Seeding now supports only static config applied before server start:

- Config file: JSON-compatible YAML (example: [`seeds/dev.seed.yaml`](/Users/denis_pm/C++ projects/snake_terminal/seeds/dev.seed.yaml))
- Apply script: `tools/apply_seed_config.py`
- Guards:
  - no users in DB
  - `APP_ENV != prod` OR `SEED_ENABLED=true`
- No API/CLI runtime seed trigger exists after startup.

### Prod parity smoke checks

After deploy, verify public routing:

```bash
curl -sS https://terrariumsnake.com/health
curl -sS https://terrariumsnake.com/economy/state
```

Expected:
- `/health` -> `{"ok":true}`
- `/economy/state` -> JSON with numeric `Y`, `K`, `L`, `alpha`, `A`, `M`, `P`, `pi`, `treasury_balance`, `period_ends_in_seconds`
  - includes validity flags: `price_index_valid`, `inflation_valid` (false in zero-output edge periods)

### Minimal Economy/Player Smoke Test

Run the critical auth/onboarding/borrow/attach flow locally:

```bash
make smoke-economy-local
```

Equivalent direct command:

```bash
python3 tools/smoke_economy_flow.py --base-url http://127.0.0.1:8080 --token "<bearer-token>"
```

The script fails loudly if any of these fail:
- auth probe/onboarding
- starter snake visibility
- borrow `amount=1`
- attach `amount=1` to starter snake

### Prod Manual QA Checklist (Non-Destructive)

1. Login with a fresh user and complete onboarding.
2. Confirm `/me/snakes` returns at least one snake id.
3. Borrow 1 cell and confirm user liquid assets increased by 1.
4. Attach 1 cell to the selected snake and confirm snake length increased by 1.
5. Confirm signed-in state hides Google sign-in and shows `Log out`.
6. Confirm snake names are globally unique:
   - creating/renaming to an existing snake name returns `409 {"error":"snake_name_taken"}`
   - UI shows: `This snake name is already taken. Please choose another name.`
7. Confirm newly created snakes keep the exact user-provided name immediately (no `Snake #N` fallback labels).
8. Confirm snake delete works from the three-dots menu:
   - snake is removed from world and `/me/snakes`
   - liquid assets increase by deleted snake length
   - deleted snake name becomes available for reuse.
9. Confirm Create Snake uses the app modal (not browser `prompt`) and supports inline validation/retry.
10. Confirm duplicate snake names are rejected immediately even for rapid consecutive creates/renames.

## Watch camera / AOI-ready behavior

- Runtime stream uses a single WebSocket endpoint: `GET /ws`.
- Frontend sends runtime messages over WS (`auth`, `input`, `camera_set`) and receives `world_snapshot`, `economy_world`, `user_state`, `system_message`.
- Frontend renderer is WebGL canvas-based (no DOM cell grid), with map-style zoom.
- Runtime endpoints:
  - local: `ws://127.0.0.1:8080/ws`
  - prod: `wss://terrariumsnake.com/ws`
- In **My Snakes**, a `watch` checkbox appears under each snake:
  - only the selected snake can be watched
  - only one snake can be watched at a time
  - selecting another snake clears previous watch selection
- Old compatibility routes remain available on backend (`/game/stream`, `/game/view`, `/game/camera`) but the current frontend does not use them.

### Zoom + debug overlay (Step 9)

- UI controls:
  - `+ Zoom In`
  - `- Zoom Out`
  - `Reset` (returns zoom to `1.0` and default/follow center)
  - `Grid`: `Off` / `Cells` / `Cells+Chunks`
  - `Bounds`: world border toggle
- Zoom bounds: `0.25 .. 4.0`
- Optional debug overlay:
  - add `?debug=1` to URL
  - shows mode (`PUBLIC`/`AUTH`), camera center, zoom, chunk size, camera chunk, AOI range/chunk count, and last snapshot payload size
- Visual overlays are WebGL-rendered and session-persisted via `localStorage`.

### Auth-gated camera/zoom (Step 9.1)

- Camera/zoom updates are accepted only for authenticated sessions.
- Unauthenticated visitors receive a server-driven public activity view.
- Public view switches focus periodically using in-memory chunk activity scores (no DB writes).
- `camera_set` WS messages are auth-gated and rate-limited by `CAMERA_MSG_MAX_HZ`.
- Debug overlay (`?debug=1`) reads mode/camera/AOI/public chunk from WS snapshot metadata.
- Watch stream broadcast rate is restored to `SPECTATOR_HZ` (default `10 Hz`) for everyone.
- AOI filtering is active with chunk-based replication; zoom/camera only changes viewport, not world simulation.
- AOI edge stability uses `AOI_PAD_CHUNKS` (default `1`) to avoid chunk-boundary flicker.
- Optional torn playable-world mask:
  - `WORLD_MASK_MODE=none|torn`
  - `WORLD_MASK_SEED=<int>`
  - `WORLD_MASK_STYLE=jagged`
  - playable cells stay tied to economy target area (`A_world`), with non-playable cells pushed to deterministic torn edges.

## Stabilization Automation

- Fast spatial check cadence is derived from period length:
  - `check_interval_seconds = ECONOMIC_PERIOD_DURATION_SECONDS / AUTO_EXPANSION_CHECKS_PER_PERIOD`
- `snakecli economy status` now includes stabilization metrics:
  - `R`, `LCR`, `treasury_white_space`, `failures_this_period`, current mode, next fast-check ETA, next period-close ETA
- Global Economy panel includes:
  - `Field Size`, `Free Space on Field`, `System White Space Reserve`, `Spatial Ratio (R)`, `Stabilization Status`
- Admin status endpoint (`GET /admin/economy/status`) exposes the same stabilization fields for automation tooling.

## Changelog CI rules

`tools/generate_world_evolution_log.py` enforces:
- strict SemVer (`MAJOR.MINOR.PATCH`)
- release date per entry
- 3-7 bullet points per version
- no duplicate versions

CI/workflows:
- `.github/workflows/changelog-validate.yml` validates/generates on PR/push to `main`.
- `.github/workflows/deploy.yml` validates and generates the artifact before deploy.

## Modes

- `./snake_server serve`
- `./snake_server reset`

`snakecli` is installed to `/usr/local/bin/snakecli` in runtime environments.
