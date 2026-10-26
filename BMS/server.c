#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "common.h"
#include "utils.h"
#include "auth.h"
#include "customer.h"
#include "staff.h"
#include "employee.h"
#include "manager.h"
#include "admin.h"
/* Reap finished children so they don't become zombies. */
static void sigchld_handler(int sig) {
    (void)sig;
    int saved = errno;
    while (waitpid(-1, NULL, WNOHANG) > 0);
    errno = saved;
}

/* Runs inside the child process. Placeholder for login + role routing. */
static const char *MAIN_MENU =
    "\nBanking Management System ...\n"
    "Select Role to Login:\n"
    "1. Customer\n"
    "2. Bank Employee\n"
    "3. Manager\n"
    "4. Administrator\n"
    "5. Exit\n";

/* Runs inside the child process: role menu -> login -> role handler */
static void handle_client(int client_fd) {
    char buf[BUFFER_SIZE];
    Session session;
    memset(&session, 0, sizeof(session));

    while (1) {
        send_message(client_fd, MAIN_MENU);
        if (prompt_input(client_fd, "Enter choice: ", buf, sizeof(buf)) <= 0)
            break;                                    /* client disconnected */

        int choice = atoi(buf);
        if (choice == 5) {
            send_message(client_fd, "Goodbye!\n");
            break;
        }
        if (choice < ROLE_CUSTOMER || choice > ROLE_ADMIN) {
            send_message(client_fd, "Invalid choice.\n");
            continue;
        }

        int r = auth_prompt_login(client_fd, choice, &session);
        if (r == LOGIN_DISCONNECTED) break;
        if (r == LOGIN_OK) {
            switch (choice) {
                case ROLE_CUSTOMER: customer_menu(client_fd, &session); break;
                case ROLE_EMPLOYEE: employee_menu(client_fd, &session); break;
                case ROLE_MANAGER:  manager_menu(client_fd, &session);  break;
                case ROLE_ADMIN: admin_menu(client_fd, &session); break;
            }
            auth_logout(&session);       /* always release the session, even if client dropped */
        }
    }
}

int main(void) {
    int server_fd, client_fd;
    struct sockaddr_in addr, cli_addr;
    socklen_t cli_len = sizeof(cli_addr);

    /* Install SIGCHLD handler */
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGCHLD, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);   /* a write to a closed socket returns EPIPE instead of killing the child */

    /* socket */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(EXIT_FAILURE); }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* bind */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(SERVER_PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(server_fd); exit(EXIT_FAILURE);
    }

    /* listen */
    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen"); close(server_fd); exit(EXIT_FAILURE);
    }
    printf("Server listening on port %d...\n", SERVER_PORT);
    
    if (customer_validate_file() != 0) {
        fprintf(stderr, "%s is missing or not in the fixed-width record format "
                        "(%d bytes per row). Convert it, then restart.\n",
                CUSTOMER_FILE, RECORD_SIZE);
        exit(EXIT_FAILURE);
    }
    
    if (staff_validate_file(ROLE_EMPLOYEE) != 0) {
        fprintf(stderr, "%s is not in the fixed-width format (%d bytes per row). "
                        "Recreate it, then restart.\n", EMPLOYEE_FILE, STAFF_RECORD_SIZE);
        exit(EXIT_FAILURE);
    }

    if (staff_validate_file(ROLE_MANAGER) != 0) {
        fprintf(stderr, "%s is not in the fixed-width format (%d bytes per row). "
                        "Recreate it, then restart.\n", MANAGER_FILE, STAFF_RECORD_SIZE);
        exit(EXIT_FAILURE);
    }

    if (staff_validate_file(ROLE_ADMIN) != 0) {
        fprintf(stderr, "%s is not in the fixed-width format (%d bytes per row). "
                        "Recreate it, then restart.\n", ADMIN_FILE, STAFF_RECORD_SIZE);
        exit(EXIT_FAILURE);
    }
    if (customer_validate_logs() != 0) {
        fprintf(stderr, "%s or %s is not in the fixed-width log format. "
                        "Move the old files aside and restart.\n", TXN_FILE, FEEDBACK_FILE);
        exit(EXIT_FAILURE);
    }

    if (auth_reset_sessions() != 0) {
    	fprintf(stderr, "Warning: could not reset sessions in %s\n", CUSTOMER_FILE);
    }
    /* accept loop */
    while (1) {
        client_fd = accept(server_fd, (struct sockaddr *)&cli_addr, &cli_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        printf("Client connected: %s:%d\n",
               inet_ntoa(cli_addr.sin_addr), ntohs(cli_addr.sin_port));

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            close(client_fd);
        } else if (pid == 0) {            /* CHILD */
            close(server_fd);             /* child doesn't need listener */
            handle_client(client_fd);
            close(client_fd);
            printf("Client disconnected (pid %d)\n", getpid());
            exit(EXIT_SUCCESS);
        } else {                          /* PARENT */
            close(client_fd);             /* parent doesn't need this client */
        }
    }

    close(server_fd);
    return 0;
}
