#include <iostream>
#include <ncurses.h>  // Use ncurses for macOS/Linux compatibility
#include <cstdlib>
#include <ctime>
#include <chrono>
#include <thread>

using namespace std;

// Game settings
const int width = 40;  // Increased board width
const int height = 20;  // Increased board height
int x, y, fruitX1, fruitY1, fruitX2, fruitY2, score;
int tailX[100], tailY[100];
int nTail;
bool gameOver;
bool isPaused = false;  // Pause flag
double gameSpeed = 1.25;  // Initial speed reduction factor
enum eDirection { STOP = 0, LEFT, RIGHT, UP, DOWN };
eDirection dir;

// Additional variables for random borders
int borderX1, borderY1, borderWidth1, borderHeight1;
int borderX2, borderY2, borderWidth2, borderHeight2;

// Function to generate random borders
void GenerateRandomBorders() {
    borderX1 = rand() % width;  // Random position for first border
    borderY1 = rand() % height;
    borderWidth1 = rand() % (width / 4) + 3;  // Random width (minimum size 3)
    borderHeight1 = rand() % (height / 4) + 2;  // Random height (minimum size 2)

    borderX2 = rand() % width;  // Random position for second border
    borderY2 = rand() % height;
    borderWidth2 = rand() % (width / 4) + 3;  // Random width (minimum size 3)
    borderHeight2 = rand() % (height / 4) + 2;  // Random height (minimum size 2)
}

// Function to check if fruit is inside any border
bool isInsideBorder(int fruitX, int fruitY, int borderX, int borderY, int borderWidth, int borderHeight) {
    return fruitX >= borderX && fruitX < (borderX + borderWidth) &&
           fruitY >= borderY && fruitY < (borderY + borderHeight);
}

// Function to place fruit ensuring it is not inside a border
void PlaceFruit(int &fruitX, int &fruitY) {
    do {
        fruitX = rand() % width;
        fruitY = rand() % height;
    } while (isInsideBorder(fruitX, fruitY, borderX1, borderY1, borderWidth1, borderHeight1) ||
             isInsideBorder(fruitX, fruitY, borderX2, borderY2, borderWidth2, borderHeight2));
}

// Update the Setup function to ensure correct placement of fruits
void Setup() {
    gameOver = false;
    dir = STOP;
    x = width / 2;
    y = height / 2;

    // Set initial positions for fruits, ensuring they don't spawn inside borders
    PlaceFruit(fruitX1, fruitY1);
    PlaceFruit(fruitX2, fruitY2);

    score = 0;
    nTail = 0;
    
    GenerateRandomBorders();  // Initialize random borders
}

// Function to draw the game board with colors (now also draws random borders)
void Draw() {
    clear();  // Clear screen

    // Draw top border with color
    attron(COLOR_PAIR(1));  // Set color pair 1 (for borders)
    for (int i = 0; i < width + 2; i++)
        mvprintw(0, i, "#");
    attroff(COLOR_PAIR(1));  // Turn off color pair 1

    // Draw game area with colors
    for (int i = 0; i < height; i++) {
        for (int j = 0; j < width; j++) {
            if (j == 0) {
                attron(COLOR_PAIR(1));  // Set color pair 1 for left border
                mvprintw(i + 1, 0, "#");
                attroff(COLOR_PAIR(1));  // Turn off color pair 1
            }

            // Draw snake head with color
            if (i == y && j == x) {
                attron(COLOR_PAIR(2));  // Set color pair 2 (for snake)
                mvprintw(i + 1, j + 1, "O");
                attroff(COLOR_PAIR(2));  // Turn off color pair 2
            }
            // Draw first fruit with color
            else if (i == fruitY1 && j == fruitX1) {
                attron(COLOR_PAIR(3));  // Set color pair 3 (for fruits)
                mvprintw(i + 1, j + 1, "F");
                attroff(COLOR_PAIR(3));  // Turn off color pair 3
            }
            // Draw second fruit with color
            else if (i == fruitY2 && j == fruitX2) {
                attron(COLOR_PAIR(3));  // Set color pair 3 (for fruits)
                mvprintw(i + 1, j + 1, "F");
                attroff(COLOR_PAIR(3));  // Turn off color pair 3
            }
            else {
                bool print = false;
                // Draw snake tail with color
                for (int k = 0; k < nTail; k++) {
                    if (tailX[k] == j && tailY[k] == i) {
                        attron(COLOR_PAIR(2));  // Set color pair 2 (for snake)
                        mvprintw(i + 1, j + 1, "o");
                        attroff(COLOR_PAIR(2));  // Turn off color pair 2
                        print = true;
                    }
                }

                // Draw random borders
                if (!print) {
                    if ((j >= borderX1 && j < borderX1 + borderWidth1 && i >= borderY1 && i < borderY1 + borderHeight1) ||
                        (j >= borderX2 && j < borderX2 + borderWidth2 && i >= borderY2 && i < borderY2 + borderHeight2)) {
                        attron(COLOR_PAIR(1));  // Set color pair 1 (for borders)
                        mvprintw(i + 1, j + 1, "#");
                        attroff(COLOR_PAIR(1));  // Turn off color pair 1
                    }
                    else
                        mvprintw(i + 1, j + 1, " ");
                }
            }

            if (j == width - 1) {
                attron(COLOR_PAIR(1));  // Set color pair 1 for right border
                mvprintw(i + 1, j + 2, "#");
                attroff(COLOR_PAIR(1));  // Turn off color pair 1
            }
        }
    }

    // Draw bottom border with color
    attron(COLOR_PAIR(1));  // Set color pair 1 (for borders)
    for (int i = 0; i < width + 2; i++){
        mvprintw(height + 1, i, "#");
    }
    attroff(COLOR_PAIR(1));  // Turn off color pair 1

    // Display score with color
    attron(COLOR_PAIR(4));  // Set color pair 4 (for text)
    mvprintw(height + 3, 0, "Score: %d", score);
    mvprintw(height + 4, 0, "Press 'p' to pause, 'q' to quit.");
    attroff(COLOR_PAIR(4));  // Turn off color pair 4

    refresh();  // Refresh screen to show changes
}

bool CheckBorderCollision(int x, int y) {
    if ((x >= borderX1 && x < borderX1 + borderWidth1 && y >= borderY1 && y < borderY1 + borderHeight1) ||
        (x >= borderX2 && x < borderX2 + borderWidth2 && y >= borderY2 && y < borderY2 + borderHeight2)) {
        return true;  // Collision with random border
    }
    return false;
}

// Function to handle input
void Input() {
    keypad(stdscr, TRUE);  // Enable arrow keys
    nodelay(stdscr, TRUE); // Non-blocking input
    int ch = getch();
    switch (ch) {
        case KEY_LEFT:
            if (dir != RIGHT) dir = LEFT;
            break;
        case KEY_RIGHT:
            if (dir != LEFT) dir = RIGHT;
            break;
        case KEY_UP:
            if (dir != DOWN) dir = UP;
            break;
        case KEY_DOWN:
            if (dir != UP) dir = DOWN;
            break;
        case 'p':  // Toggle pause
            isPaused = !isPaused;
            if (isPaused) mvprintw(height + 5, 0, "Game Paused. Press 'p' to resume.");
            refresh();
            break;
        case 'q':  // Quit the game
            gameOver = true;
            break;
    }
}

// Function to handle input, logic, etc.
void Logic() {
    if (isPaused) return;  // Skip logic if paused

    // Update tail positions
    int prevX = tailX[0];
    int prevY = tailY[0];
    int prev2X, prev2Y;
    tailX[0] = x;
    tailY[0] = y;
    for (int i = 1; i < nTail; i++) {
        prev2X = tailX[i];
        prev2Y = tailY[i];
        tailX[i] = prevX;
        tailY[i] = prevY;
        prevX = prev2X;
        prevY = prev2Y;
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

    // Check for collisions with walls or random borders
    if (x >= width || x < 0 || y >= height || y < 0 || CheckBorderCollision(x, y))
        gameOver = true;

    // Check for collisions with tail
    for (int i = 0; i < nTail; i++) {
        if (tailX[i] == x && tailY[i] == y)
            gameOver = true;
    }

    // Check if either fruit is eaten
    if (x == fruitX1 && y == fruitY1) {
        score += 10;
        fruitX1 = rand() % width;
        fruitY1 = rand() % height;
        nTail++;
        gameSpeed *= 0.98;  // Increase speed by 2%
        GenerateRandomBorders();  // Generate new random borders
    } else if (x == fruitX2 && y == fruitY2) {
        score += 10;
        fruitX2 = rand() % width;
        fruitY2 = rand() % height;
        nTail++;
        gameSpeed *= 0.98;  // Increase speed by 2%
        GenerateRandomBorders();  // Generate new random borders
    }
}

int main() {
    srand(time(0));  // Seed random number generator

    initscr();  // Initialize ncurses mode
    noecho();   // Don't show keypresses on the screen
    curs_set(0);  // Hide the cursor

    // Initialize color pairs
    if (has_colors() == FALSE) {
        endwin();
        cout << "Your terminal does not support color." << endl;
        return 1;
    }
    start_color();
    init_pair(1, COLOR_RED, COLOR_BLACK);     // Borders color
    init_pair(2, COLOR_GREEN, COLOR_BLACK);   // Snake color
    init_pair(3, COLOR_YELLOW, COLOR_BLACK);  // Fruits color
    init_pair(4, COLOR_CYAN, COLOR_BLACK);    // Text color

    Setup();  // Setup initial game state

    while (!gameOver) {
        Draw();    // Draw the game board
        Input();   // Handle user input
        Logic();   // Apply game logic
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(100 * gameSpeed)));  // Control game speed
    }

    // Clean up
}