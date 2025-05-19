#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <termios.h>

#define MAX_FILES 256
#define MAX_NAME_LEN 256

void enableRawMode(struct termios *orig_termios) {
    struct termios raw;
    tcgetattr(STDIN_FILENO, orig_termios);
    raw = *orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void disableRawMode(struct termios *orig_termios) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, orig_termios);
}

int compare(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

void clearScreen() {
    printf("\033[H\033[J");
}

int main() {
    struct termios orig_termios;
    char files[MAX_FILES][MAX_NAME_LEN];
    int count = 0;
    int selected = 0;

    DIR *d = opendir(".");
    if (!d) {
        perror("opendir");
        return 1;
    }

    count = 0;
    struct dirent *dir;
    while ((dir = readdir(d)) != NULL) {
        if (strcmp(dir->d_name, ".") == 0)
            continue;
        if (count >= MAX_FILES) break;
        strncpy(files[count], dir->d_name, MAX_NAME_LEN - 1);
        files[count][MAX_NAME_LEN - 1] = '\0';
        count++;
    }
    closedir(d);

    // Ordenar
    qsort(files, count, sizeof(files[0]), compare);

    enableRawMode(&orig_termios);

    while (1) {
        clearScreen();
        printf("Directorio actual (no cambia realmente)\n\n");
        printf("Navega con flechas Arriba/Abajo, Enter para imprimir 'cd carpeta', q para salir\n\n");

        for (int i = 0; i < count; i++) {
            struct stat st;
            stat(files[i], &st);
            int isdir = S_ISDIR(st.st_mode);

            if (i == selected) {
                printf("> ");
            } else {
                printf("  ");
            }

            if (isdir) {
                printf("\033[32m%s\033[0m\n", files[i]);
            } else {
                printf("%s\n", files[i]);
            }
        }

        char c = 0;
        read(STDIN_FILENO, &c, 1);

        if (c == 'q') {
            disableRawMode(&orig_termios);
            clearScreen();
            return 0;
        } else if (c == '\033') {
            char seq[2];
            if (read(STDIN_FILENO, &seq[0], 1) == 0) continue;
            if (read(STDIN_FILENO, &seq[1], 1) == 0) continue;

            if (seq[0] == '[') {
                if (seq[1] == 'A') {
                    if (selected > 0) selected--;
                } else if (seq[1] == 'B') {
                    if (selected < count - 1) selected++;
                }
            }
        } else if (c == '\n') {
            struct stat st;
            if (stat(files[selected], &st) == 0 && S_ISDIR(st.st_mode)) {
                // En lugar de cambiar de directorio, imprimir el comando
                printf("\ncd %s\n", files[selected]);
                printf("Presiona cualquier tecla para continuar...");
                disableRawMode(&orig_termios);
                getchar();  // Esperar tecla
                enableRawMode(&orig_termios);
            }
        }
    }

    disableRawMode(&orig_termios);
    return 0;
}
