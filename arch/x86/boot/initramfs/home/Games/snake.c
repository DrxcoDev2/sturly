#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <time.h>
#include <sys/select.h>

#define WIDTH 20
#define HEIGHT 20

// Configura la entrada sin buffer
void setBufferedInput(int enable) {
    static struct termios old;
    static int isBuffered = 1;

    if (enable && !isBuffered) {
        tcsetattr(STDIN_FILENO, TCSANOW, &old);
        isBuffered = 1;
    } else if (!enable && isBuffered) {
        struct termios new;
        tcgetattr(STDIN_FILENO, &old);
        new = old;
        new.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &new);
        isBuffered = 0;
    }
}

int kbhit() {
    struct timeval tv = {0L, 0L};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO+1, &fds, NULL, NULL, &tv);
}

int getch() {
    int r;
    unsigned char c;
    if ((r = read(STDIN_FILENO, &c, sizeof(c))) < 0) return r;
    else return c;
}

int main() {
    setBufferedInput(0); // Activar entrada sin buffer

    static int x = WIDTH / 2, y = HEIGHT / 2;
    static int fruitX, fruitY;
    static int score = 0;
    static int tailX[100], tailY[100];
    static int nTail = 0;
    static enum Direction { STOP = 0, LEFT, RIGHT, UP, DOWN } dir = STOP;
    static int gameOver = 0;
    static unsigned long lastMove = 0;

    srand(time(0));
    fruitX = rand() % WIDTH;
    fruitY = rand() % HEIGHT;

    while (!gameOver) {
        // Dibujar mapa
        system("clear");
        for (int i = 0; i < WIDTH + 2; i++) printf("#");
        printf("\n");

        for (int i = 0; i < HEIGHT; i++) {
            for (int j = 0; j < WIDTH; j++) {
                if (j == 0) printf("#");

                if (i == y && j == x)
                    printf("O");
                else if (i == fruitY && j == fruitX)
                    printf("F");
                else {
                    int print = 0;
                    for (int k = 0; k < nTail; k++) {
                        if (tailX[k] == j && tailY[k] == i) {
                            printf("o");
                            print = 1;
                            break;
                        }
                    }
                    if (!print) printf(" ");
                }

                if (j == WIDTH - 1) printf("#");
            }
            printf("\n");
        }

        for (int i = 0; i < WIDTH + 2; i++) printf("#");
        printf("\nScore: %d\n", score);

        // Entrada
        if (kbhit()) {
            switch (getch()) {
                case 'a': dir = LEFT; break;
                case 'd': dir = RIGHT; break;
                case 'w': dir = UP; break;
                case 's': dir = DOWN; break;
                case 'x': gameOver = 1; break;
            }
        }

        // Movimiento cada 200ms
        unsigned long now = clock();
        if ((now - lastMove) > 200000) {  // en ticks de CLOCKS_PER_SEC
            lastMove = now;

            // Mover cola
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

            // Movimiento cabeza
            switch (dir) {
                case LEFT:  x--; break;
                case RIGHT: x++; break;
                case UP:    y--; break;
                case DOWN:  y++; break;
            }

            // Colisi�n con bordes
            if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT)
                gameOver = 1;

            // Colisi�n con cola
            for (int i = 0; i < nTail; i++) {
                if (tailX[i] == x && tailY[i] == y)
                    gameOver = 1;
            }

            // Comer fruta
            if (x == fruitX && y == fruitY) {
                score += 10;
                fruitX = rand() % WIDTH;
                fruitY = rand() % HEIGHT;
                nTail++;
            }
        }

        usleep(10000);  // evitar alto uso de CPU (10ms)
    }

    printf("Game Over!\n");
    setBufferedInput(1); // Restaurar entrada est�ndar
    return 0;
}
