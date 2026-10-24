#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "common.h"

int main(int argc, char *argv[]) {
    const char *ip = (argc > 1) ? argv[1] : "127.0.0.1";

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); exit(EXIT_FAILURE); }

    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_port   = htons(SERVER_PORT);
    if (inet_pton(AF_INET, ip, &serv.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address: %s\n", ip);
        exit(EXIT_FAILURE);
    }

    if (connect(sock, (struct sockaddr *)&serv, sizeof(serv)) < 0) {
        perror("connect"); close(sock); exit(EXIT_FAILURE);
    }

    struct pollfd fds[2];
    fds[0].fd = STDIN_FILENO; fds[0].events = POLLIN;
    fds[1].fd = sock;         fds[1].events = POLLIN;

    char buf[BUFFER_SIZE];

    while (1) {
        if (poll(fds, 2, -1) < 0) { perror("poll"); break; }

        /* Data from server */
        if (fds[1].revents & (POLLIN | POLLHUP)) {
            ssize_t n = read(sock, buf, sizeof(buf) - 1);
            if (n <= 0) {
                printf("\nConnection closed by server.\n");
                break;
            }
            buf[n] = '\0';
            printf("%s", buf);
            fflush(stdout);
        }

        /* Data from keyboard */
        if (fds[0].revents & POLLIN) {
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf) - 1);
            if (n <= 0) break;                 /* Ctrl+D */
            if (write(sock, buf, n) < 0) { perror("write"); break; }
        }
    }

    close(sock);
    return 0;
}
