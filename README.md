## Snake Game

- if don't have SqlLte yet: `brew install sqlite`
- You also need the single-header HTTP library. Download httplib.h:
    `curl -L https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h -o api/httplib.h`
- compile: `clang++ -std=c++17 -O2 -pthread api/snake_server.cpp -lsqlite3 -o snake_server`
- start the server: `./snake_server serve`
- Open the front `index.html` to play

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
