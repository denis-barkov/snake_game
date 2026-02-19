## Snake Game

- if don't have SqlLte yet: `brew install sqlite`
- You also need the single-header HTTP library. Download httplib.h:
    `curl -L https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h -o api/httplib.h`
- compile: `clang++ -std=c++17 -O2 -pthread api/snake_server.cpp api/protocol/encode_json.cpp config/runtime_config.cpp -lsqlite3 -o snake_server`
- start the server: `./snake_server serve`
- Open `index.html` to play (works from `file://` and uses local API `http://127.0.0.1:8080`)

### Runtime Hz config

Environment variables:
- `TICK_HZ` (default `20`, min `5`, max `60`)
- `SPECTATOR_HZ` (default `10`, min `1`, max `60`)
- `PLAYER_HZ` (placeholder, currently unused)
- `ENABLE_BROADCAST` (`true`/`false`, default `true`)
- `LOG_HZ` (`true`/`false`, default `true`)

Quick test:
```bash
export TICK_HZ=20
export SPECTATOR_HZ=10
./snake_server serve
```

Expected:
- Game simulation runs at ~20 ticks/sec
- SSE `/game/stream` publishes ~10 frames/sec

### Protocol source of truth

Snapshot protocol JSON is defined in `api/protocol`.
Do not inline snapshot JSON shapes in server code.

### Seed / reset data

Server modes:
- `./snake_server serve`
- `./snake_server seed`
- `./snake_server reset`

Local seed/reset (from project root):
```bash
./snake_server reset
./snake_server seed
./snake_server serve
```

EC2 seed/reset (same DB used by systemd service):
```bash
sudo systemctl stop snake
cd /opt/snake
sudo ./snake_server reset
sudo ./snake_server seed
sudo systemctl start snake
```

Default seeded users:
- `user1 / pass1`
- `user2 / pass2`
