## Snake Game

- if don't have SqlLte yet: `brew install sqlite`
- You also need the single-header HTTP library. Download httplib.h:
    `curl -L https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h -o api/httplib.h`
- compile: `clang++ -std=c++17 -O2 -pthread api/snake_server.cpp -lsqlite3 -o snake_server`
- start the server: `./snake_server`
- Open the front `index.html` to play