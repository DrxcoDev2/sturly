#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <ctype.h>

#define BUFFER_SIZE 8192
#define MAX_LINKS 100

typedef struct {
    char href[256];
    char text[256];
} Link;

typedef struct {
    char *html;
    size_t len;
    Link links[MAX_LINKS];
    int link_count;
} Page;

// Hace petici�n HTTP simple GET
char *http_get(const char *host, const char *path) {
    struct hostent *server;
    struct sockaddr_in serv_addr;
    int sockfd;
    char request[1024], response[BUFFER_SIZE];
    char *result = NULL;
    size_t total_len = 0;

    server = gethostbyname(host);
    if (server == NULL) {
        fprintf(stderr, "No se pudo resolver el host\n");
        return NULL;
    }

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("Error abriendo socket");
        return NULL;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    serv_addr.sin_port = htons(80);

    if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Error conectando");
        close(sockfd);
        return NULL;
    }

    snprintf(request, sizeof(request),
             "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: mini-lynx/0.1\r\n\r\n",
             path, host);

    if (write(sockfd, request, strlen(request)) < 0) {
        perror("Error enviando petici�n");
        close(sockfd);
        return NULL;
    }

    while (1) {
        ssize_t len = read(sockfd, response, sizeof(response) - 1);
        if (len < 0) {
            perror("Error leyendo respuesta");
            free(result);
            close(sockfd);
            return NULL;
        }
        if (len == 0) break;

        response[len] = '\0';
        char *tmp = realloc(result, total_len + len + 1);
        if (!tmp) {
            perror("Error en realloc");
            free(result);
            close(sockfd);
            return NULL;
        }
        result = tmp;
        memcpy(result + total_len, response, len + 1);
        total_len += len;
    }

    close(sockfd);
    return result;
}

// Extrae el cuerpo (salta headers HTTP)
char *get_body(char *html) {
    char *body = strstr(html, "\r\n\r\n");
    if (body) return body + 4;
    return html;
}

// Reemplaza algunas entidades HTML b�sicas por caracteres
void replace_html_entities(char *text) {
    char *p = text;
    char buffer[8192];
    size_t i = 0;
    while (*p && i < sizeof(buffer) - 1) {
        if (strncmp(p, "&lt;", 4) == 0) {
            buffer[i++] = '<';
            p += 4;
        } else if (strncmp(p, "&gt;", 4) == 0) {
            buffer[i++] = '>';
            p += 4;
        } else if (strncmp(p, "&amp;", 5) == 0) {
            buffer[i++] = '&';
            p += 5;
        } else if (strncmp(p, "&nbsp;", 6) == 0) {
            buffer[i++] = ' ';
            p += 6;
        } else {
            buffer[i++] = *p++;
        }
    }
    buffer[i] = 0;
    strcpy(text, buffer);
}

// Limpia texto de etiquetas HTML, mantiene texto plano y detecta enlaces <a>
void parse_html(Page *page) {
    char *p = page->html;
    char *out = malloc(strlen(p) + 1);
    if (!out) return;
    size_t out_i = 0;
    int in_tag = 0;
    int in_link = 0;
    char link_href[256] = {0};
    char link_text[256] = {0};
    size_t link_text_i = 0;

    page->link_count = 0;

    while (*p) {
        if (*p == '<') {
            in_tag = 1;
            // Detecta <a href="...">
            if (strncmp(p, "<a ", 3) == 0) {
                in_link = 1;
                // Busca href
                char *href_start = strstr(p, "href=\"");
                if (href_start) {
                    href_start += 6;
                    char *href_end = strchr(href_start, '"');
                    if (href_end && (href_end - href_start) < (int)sizeof(link_href)) {
                        strncpy(link_href, href_start, href_end - href_start);
                        link_href[href_end - href_start] = '\0';
                    }
                }
            }
            p++;
            continue;
        }
        if (*p == '>') {
            in_tag = 0;
            // Cierra etiqueta <a>
            if (in_link && strncmp(p - 2, "</a", 3) == 0) {
                in_link = 0;
                // Guarda link
                if (page->link_count < MAX_LINKS) {
                    strncpy(page->links[page->link_count].href, link_href, 255);
                    page->links[page->link_count].href[255] = 0;
                    // Quitar espacios al principio y fin de link_text
                    size_t start = 0, end = strlen(link_text);
                    while (start < end && isspace((unsigned char)link_text[start])) start++;
                    while (end > start && isspace((unsigned char)link_text[end - 1])) end--;
                    link_text[end] = '\0';
                    strncpy(page->links[page->link_count].text, link_text + start, 255);
                    page->links[page->link_count].text[255] = 0;

                    page->link_count++;
                }
                link_href[0] = 0;
                link_text[0] = 0;
                link_text_i = 0;
            }
            p++;
            continue;
        }
        if (!in_tag) {
            // Copia texto plano
            if (in_link) {
                if (link_text_i < sizeof(link_text) - 1) {
                    link_text[link_text_i++] = *p;
                    link_text[link_text_i] = 0;
                }
            }
            out[out_i++] = *p++;
        } else {
            p++;
        }
    }
    out[out_i] = 0;

    // Reemplaza entidades HTML
    replace_html_entities(out);

    free(page->html);
    page->html = out;
    page->len = out_i;
}

// Muestra la p�gina con enlaces numerados
void print_page(Page *page) {
    printf("\n=== P�gina ===\n\n");
    printf("%s\n\n", page->html);

    if (page->link_count == 0) {
        printf("[No hay enlaces]\n");
        return;
    }

    printf("Enlaces:\n");
    for (int i = 0; i < page->link_count; i++) {
        printf("  %d) %s (%s)\n", i + 1,
            page->links[i].text[0] ? page->links[i].text : "(sin texto)",
            page->links[i].href);
    }
}

// Parsea URL en host y path (solo HTTP)
int parse_url(const char *url, char *host, char *path) {
    if (strncmp(url, "http://", 7) == 0) {
        url += 7;
    }
    else if (strncmp(url, "https://", 8) == 0) {
        fprintf(stderr, "HTTPS no soportado en esta versi�n\n");
        return -1;
    }

    const char *slash = strchr(url, '/');
    if (slash) {
        size_t host_len = slash - url;
        strncpy(host, url, host_len);
        host[host_len] = 0;
        strcpy(path, slash);
    } else {
        strcpy(host, url);
        strcpy(path, "/");
    }
    return 0;
}

int main(int argc, char **argv) {
    char url[512];
    if (argc == 2) {
        strncpy(url, argv[1], 511);
        url[511] = 0;
    } else {
        printf("Uso: %s http://host/path\n", argv[0]);
        return 1;
    }

    char host[256], path[256];
    if (parse_url(url, host, path) != 0) return 1;

    while (1) {
        printf("\nCargando: http://%s%s\n", host, path);

        char *raw_html = http_get(host, path);
        if (!raw_html) {
            printf("Error al descargar la p�gina.\n");
            break;
        }

        char *body = get_body(raw_html);

        Page page = {0};
        page.html = strdup(body);
        page.len = strlen(body);

        parse_html(&page);
        print_page(&page);

        free(raw_html);
        free(page.html);

        if (page.link_count == 0) break;

        printf("\nSelecciona enlace (0 para salir): ");
        int choice = -1;
        if (scanf("%d", &choice) != 1 || choice < 0 || choice > page.link_count) {
            printf("Entrada inv�lida.\n");
            break;
        }
        if (choice == 0) break;

        // Construir nueva URL
        const char *href = page.links[choice - 1].href;
        char new_url[512];
        if (strncmp(href, "http://", 7) == 0) {
            strncpy(new_url, href, sizeof(new_url) - 1);
            new_url[sizeof(new_url) - 1] = 0;
        } else if (href[0] == '/') {
            snprintf(new_url, sizeof(new_url), "http://%s%s", host, href);
        } else {
            // link relativo simple (concatenar con host y /)
            snprintf(new_url, sizeof(new_url), "http://%s/%s", host, href);
        }
        new_url[511] = 0;

        if (parse_url(new_url, host, path) != 0) {
            printf("URL no soportada: %s\n", new_url);
            break;
        }
    }

    printf("Adi�s!\n");
    return 0;
}
