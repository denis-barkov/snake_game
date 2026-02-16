#include <iostream>
#include <vector>
#include <cstdlib>
#include <ctime>
#include <thread>
#include <chrono>

using namespace std;

// Game settings
const int width = 20;
const int height = 10;
int x, y, fruitX, fruitY, score;
vector<pair<int, int> > tail;
int nTail = 0;
enum Direction { STOP = 0, LEFT, RIGHT, UP, DOWN };
Direction dir = STOP;
bool gameOver = false;

// Function to initialize game settings
void Setup() {
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

// Draw the game grid
void Draw() {
    system("clear");  // Clear the terminal screen (works on most UNIX-based systems)

    for (int i = 0; i < width + 2; i++) cout << "#";
    cout << endl;

    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            if (j == 0) cout << "#";  // Left border

            if (i == y && j == x)
                cout << "O";  // Snake head
            else if (i == fruitY && j == fruitX)
                cout << "F";  // Fruit
            else {
                bool print = false;
                for (int k = 0; k < nTail; k++) {
                    if (tail[k].first == j && tail[k].second == i) {
                        cout << "o";  // Snake tail
                        print = true;
                    }
                }
                if (!print) cout << " ";
            }

            if (j == width - 1) cout << "#";  // Right border
        }
        cout << endl;
    }

    for (int i = 0; i < width + 2; i++) cout << "#";
    cout << endl;
    cout << "Score: " << score << endl;
    cout << "Enter 'a' (left), 'd' (right), 'w' (up), 's' (down), or 'q' to quit: ";
}

// Handle user input
void Input() {
    char ch;
    cin >> ch;

    switch (ch) {
        case 'a':
            if (dir != RIGHT) dir = LEFT;
            break;
        case 'd':
            if (dir != LEFT) dir = RIGHT;
            break;
        case 'w':
            if (dir != DOWN) dir = UP;
            break;
        case 's':
            if (dir != UP) dir = DOWN;
            break;
        case 'q':
            gameOver = true;
            break;
    }
}

// Game logic
void Logic() {
    // Update tail
    if (nTail > 0) {
        tail.insert(tail.begin(), make_pair(x, y));  // Use make_pair to create a pair
        if (tail.size() > nTail) tail.pop_back();
    }

    // Move the snake's head
    switch (dir) {
        case LEFT:
            x--;
            break;
        case RIGHT:
            x++;
            break;
        case UP:
            y--;
            break;
        case DOWN:
            y++;
            break;
        default:
            break;
    }

    // Wrap the snake around borders
    if (x >= width) x = 0;
    else if (x < 0) x = width - 1;
    if (y >= height) y = 0;
    else if (y < 0) y = height - 1;

    // Check for collision with tail
    for (int i = 0; i < nTail; i++) {
        if (tail[i].first == x && tail[i].second == y) {
            gameOver = true;
            break;
        }
    }

    // Check for collision with fruit
    if (x == fruitX && y == fruitY) {
        score += 10;
        nTail++;
        fruitX = rand() % width;
        fruitY = rand() % height;
    }
}

int main() {
    srand(time(0));  // Seed random number generator
    Setup();

    while (!gameOver) {
        Draw();
        Input();
        Logic();
        this_thread::sleep_for(chrono::milliseconds(200));  // Adjust speed
    }

    cout << "Game Over! Final Score: " << score << endl;
    return 0;
}
