#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"
#include "utils.h"
#include "customer.h"
#include "auth.h"

#define MAX_LOGIN_ATTEMPTS 3

/* Check credentials AND set logged_in = 1 under ONE exclusive lock,
   so two simultaneous logins for the same account cannot both succeed. */
static AuthStatus login_customer(const char *username, const char *password, Session *s) {
    FILE *fp = customer_open_locked(F_WRLCK);
    if (!fp) return AUTH_ERROR;

    Customer *list = NULL;
    int n = customer_read_all(fp, &list);
    if (n < 0) { customer_close_unlock(fp); return AUTH_ERROR; }

    AuthStatus status = AUTH_INVALID_CREDENTIALS;
    for (int i = 0; i < n; i++) {
        if (strcmp(list[i].username, username) != 0) continue;
        if (strcmp(list[i].password, password) != 0) break;   /* same status as "no such user" */

        /* Password is correct: only now reveal account state */
        if (!list[i].active)    { status = AUTH_ACCOUNT_INACTIVE;  break; }
        if (list[i].logged_in)  { status = AUTH_ALREADY_LOGGED_IN; break; }

        list[i].logged_in = 1;
        if (customer_write_all(fp, list, n) != 0) { status = AUTH_ERROR; break; }

        memset(s, 0, sizeof(*s));
        s->role      = ROLE_CUSTOMER;
        s->user_id   = list[i].id;
        s->logged_in = 1;
        strncpy(s->username, list[i].username, sizeof(s->username) - 1);
        status = AUTH_OK;
        break;
    }

    free(list);
    customer_close_unlock(fp);
    return status;
}

static void logout_customer(int user_id) {
    FILE *fp = customer_open_locked(F_WRLCK);
    if (!fp) return;

    Customer *list = NULL;
    int n = customer_read_all(fp, &list);
    if (n >= 0) {
        for (int i = 0; i < n; i++) {
            if (list[i].id == user_id) { list[i].logged_in = 0; break; }
        }
        customer_write_all(fp, list, n);
        free(list);
    }
    customer_close_unlock(fp);
}

/* Role dispatch: add Employee/Manager/Admin cases in later steps */
AuthStatus auth_login(int role, const char *username, const char *password, Session *s) {
    switch (role) {
    case ROLE_CUSTOMER: return login_customer(username, password, s);
    default:            return AUTH_ROLE_UNSUPPORTED;
    }
}

void auth_logout(Session *s) {
    if (!s->logged_in) return;
    switch (s->role) {
    case ROLE_CUSTOMER: logout_customer(s->user_id); break;
    default: break;
    }
    memset(s, 0, sizeof(*s));
}

int auth_require_role(const Session *s, int role) {
    return s->logged_in && s->role == role;
}

/* Prompts for credentials (max 3 attempts). */
int auth_prompt_login(int fd, int role, Session *s) {
    char username[BUFFER_SIZE], password[BUFFER_SIZE];

    for (int attempt = 1; attempt <= MAX_LOGIN_ATTEMPTS; attempt++) {
        if (prompt_input(fd, "Username: ", username, sizeof(username)) <= 0) return LOGIN_DISCONNECTED;
        if (prompt_input(fd, "Password: ", password, sizeof(password)) <= 0) return LOGIN_DISCONNECTED;

        switch (auth_login(role, username, password, s)) {
        case AUTH_OK:
            send_message(fd, "Login successful.\n");
            return LOGIN_OK;
        case AUTH_INVALID_CREDENTIALS:
            send_message(fd, "Invalid username or password.\n");
            break;                                   /* try again */
        case AUTH_ACCOUNT_INACTIVE:
            send_message(fd, "Your account is deactivated. Contact the bank.\n");
            return LOGIN_FAILED;
        case AUTH_ALREADY_LOGGED_IN:
            send_message(fd, "This account is already logged in elsewhere.\n");
            return LOGIN_FAILED;
        case AUTH_ROLE_UNSUPPORTED:
            send_message(fd, "This role is not available yet.\n");
            return LOGIN_FAILED;
        default:
            send_message(fd, "Server error. Please try again later.\n");
            return LOGIN_FAILED;
        }
    }
    send_message(fd, "Too many failed attempts.\n");
    return LOGIN_FAILED;
}

/* If the server or a client crashed, logged_in flags can be left at 1. Clear them at startup. */
int auth_reset_sessions(void) {
    FILE *fp = customer_open_locked(F_WRLCK);
    if (!fp) return -1;

    Customer *list = NULL;
    int n = customer_read_all(fp, &list);
    int rc = -1;
    if (n >= 0) {
        for (int i = 0; i < n; i++) list[i].logged_in = 0;
        rc = customer_write_all(fp, list, n);
        free(list);
    }
    customer_close_unlock(fp);
    return rc;
}
