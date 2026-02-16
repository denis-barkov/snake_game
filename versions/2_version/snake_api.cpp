#include <iostream>
#include <vector>
#include <cstdlib>
#include <ctime>
#include <thread>
#include <sstream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <mutex>
#include <chrono>

using namespace std;

// Game settings
const int width = 20;
const int height = 10;

int x, y, fruitX, fruitY, score;
vector<pair<int, int>> tail;
int nTail = 0;

enum Direction { STOP = 0, LEFT, RIGHT, UP, DOWN };
Direction dir = STOP;
bool gameOver = false;

// Debug flag
bool debug = true;

// Thread safety
std::mutex gameMutex;

// Setup game
void Setup() {
    // Caller should lock if needed (we lock where we call it)
    gameOver = false;
    dir = STOP;
    x = width / 2;
    y = height / 2;
    fruitX = rand() % width;
    fruitY = rand() % height;
    score = 0;
    nTail = 0;
    tail.clear();
}

// Game logic tick
void MoveSnake() {
    std::lock_guard<std::mutex> lock(gameMutex);

    if (gameOver) return;

    // Update tail to follow head (store current head position)
    if (nTail > 0) {
        tail.insert(tail.begin(), make_pair(x, y));
        if ((int)tail.size() > nTail) tail.pop_back();
    }

    // Move head
    switch (dir) {
        case LEFT:  x--; break;
        case RIGHT: x++; break;
        case UP:    y--; break;
        case DOWN:  y++; break;
        default: break; // STOP -> do nothing
    }

    // Wrap around
    if (x >= width) x = 0;
    else if (x < 0) x = width - 1;

    if (y >= height) y = 0;
    else if (y < 0) y = height - 1;

    // Collision with tail
    for (int i = 0; i < (int)tail.size(); i++) {
        if (tail[i].first == x && tail[i].second == y) {
            gameOver = true;
            return;
        }
    }

    // Eat fruit
    if (x == fruitX && y == fruitY) {
        score += 10;
        nTail++;

        // Re-roll fruit (basic)
        fruitX = rand() % width;
        fruitY = rand() % height;
    }
}

// Generate JSON representation of the game state
string GetGameState() {
    std::lock_guard<std::mutex> lock(gameMutex);

    ostringstream oss;
    oss << "{ \"gameOver\": " << (gameOver ? "true" : "false")
        << ", \"score\": " << score
        << ", \"snake\": [";

    // Include head FIRST (critical fix)
    oss << "{ \"x\": " << x << ", \"y\": " << y << "}";

    // Then include tail segments
    for (size_t i = 0; i < tail.size(); ++i) {
        oss << ", { \"x\": " << tail[i].first << ", \"y\": " << tail[i].second << "}";
    }

    oss << "], \"fruit\": { \"x\": " << fruitX << ", \"y\": " << fruitY << " } }";
    return oss.str();
}

// HTTP Response Helper with CORS + debug
string HTTPResponse(const string& body, const string& contentType = "application/json") {
    ostringstream oss;
    oss << "HTTP/1.1 200 OK\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        << "Access-Control-Allow-Headers: Content-Type\r\n"
        << "Content-Length: " << body.size() << "\r\n\r\n"
        << body;

    if (debug) {
        cout << "API Response Body:\n" << body << endl;
    }
    return oss.str();
}

// Extract method and path
pair<string, string> ParseHTTPRequest(const string& request) {
    istringstream reqStream(request);
    string method, path;
    reqStream >> method >> path;
    if (debug) {
        cout << "HTTP Method: " << method << ", Path: " << path << endl;
    }
    return make_pair(method, path);
}

// Parse direction from /game/direction/<int>
int ParseDirection(const string& path) {
    const char* prefix = "/game/direction/";
    size_t pos = path.find(prefix);
    if (pos != string::npos) {
        pos += strlen(prefix);
        try {
            return stoi(path.substr(pos));
        } catch (...) {
            return -1;
        }
    }
    return -1;
}

// Server loop
void RunServer() {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket failed");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("Setsockopt failed");
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr("127.0.0.1");
    address.sin_port = htons(8080);

    if (::bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 16) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    while (true) {
        if (debug) cout << "Waiting for connections..." << endl;

        if ((new_socket = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen)) < 0) {
            perror("Accept failed");
            exit(EXIT_FAILURE);
        }

        char buffer[30000] = {0};
        ssize_t bytesRead = read(new_socket, buffer, sizeof(buffer) - 1);
        if (bytesRead <= 0) {
            close(new_socket);
            continue;
        }

        string request(buffer);

        auto [method, path] = ParseHTTPRequest(request);

        // Handle CORS preflight
        if (method == "OPTIONS") {
            string response = "HTTP/1.1 204 No Content\r\n"
                              "Access-Control-Allow-Origin: *\r\n"
                              "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                              "Access-Control-Allow-Headers: Content-Type\r\n"
                              "Content-Length: 0\r\n\r\n";
            send(new_socket, response.c_str(), response.size(), 0);
            close(new_socket);
            continue;
        }

        if (method == "GET" && path == "/game/state") {
            string response = HTTPResponse(GetGameState());
            send(new_socket, response.c_str(), response.size(), 0);
        }
        else if (method == "POST" && path.find("/game/direction/") == 0) {
            int newDir = ParseDirection(path);
            if (newDir >= 1 && newDir <= 4) {
                std::lock_guard<std::mutex> lock(gameMutex);
                dir = static_cast<Direction>(newDir);
            }
            string response = HTTPResponse("{ \"status\": \"OK\" }");
            send(new_socket, response.c_str(), response.size(), 0);
        }
        else if (method == "POST" && path == "/game/reset") {
            {
                std::lock_guard<std::mutex> lock(gameMutex);
                Setup();
            }
            string response = HTTPResponse("{ \"status\": \"RESET\" }");
            send(new_socket, response.c_str(), response.size(), 0);
        }
        else {
            string response = HTTPResponse("{ \"status\": \"Not Found\" }", "application/json");
            send(new_socket, response.c_str(), response.size(), 0);
        }

        close(new_socket);
    }
}

int main() {
    srand((unsigned)time(0));
    {
        std::lock_guard<std::mutex> lock(gameMutex);
        Setup();
    }

    thread gameThread([]() {
        while (true) {
            MoveSnake();
            this_thread::sleep_for(chrono::milliseconds(200));
        }
    });

    RunServer();

    gameThread.join();
    return 0;
}