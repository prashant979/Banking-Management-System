#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "common.h"
#include "utils.h"
#include "customer.h"
#include "auth.h"

#define MAX_LOGIN_ATTEMPTS 3

/* Locate -> lock ONE record -> re-check under the lock -> set logged_in.
   Only this customer's 110 bytes are locked, so other customers are never blocked. */
static AuthStatus login_customer(const char *username, const char *password, Session *s) {
    int fd = customer_open();
    if (fd < 0) return AUTH_ERROR;

    int idx = customer_find_by_username(fd, username);       /* lock-free hint */
    if (idx == -2) { close(fd); return AUTH_ERROR; }
    if (idx < 0)   { close(fd); return AUTH_INVALID_CREDENTIALS; }

    if (customer_lock(fd, idx, F_WRLCK) < 0) { close(fd); return AUTH_ERROR; }

    AuthStatus status;
    Customer c;
    if (customer_read_record(fd, idx, &c) != 0) {
        status = AUTH_ERROR;
    } else if (strcmp(c.username, username) != 0 || strcmp(c.password, password) != 0) {
        status = AUTH_INVALID_CREDENTIALS;        /* also covers "record changed since the scan" */
    } else if (!c.active) {
        status = AUTH_ACCOUNT_INACTIVE;
    } else if (c.logged_in) {
        status = AUTH_ALREADY_LOGGED_IN;
    } else {
        c.logged_in = 1;
        if (customer_write_record(fd, idx, &c) != 0) {
            status = AUTH_ERROR;
        } else {
            memset(s, 0, sizeof(*s));
            s->role      = ROLE_CUSTOMER;
            s->user_id   = c.id;
            s->logged_in = 1;
            strncpy(s->username, c.username, sizeof(s->username) - 1);
            status = AUTH_OK;
        }
    }

    customer_unlock(fd, idx);
    close(fd);                                    /* close only AFTER unlocking */
    return status;
}

static void logout_customer(int user_id) {
    int fd = customer_open();
    if (fd < 0) return;

    int idx = customer_find_by_id(fd, user_id);
    if (idx >= 0 && customer_lock(fd, idx, F_WRLCK) == 0) {
        Customer c;
        if (customer_read_record(fd, idx, &c) == 0 && c.id == user_id) {
            c.logged_in = 0;
            customer_write_record(fd, idx, &c);
        }
        customer_unlock(fd, idx);
    }
    close(fd);
}

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

/* Clear stale logged_in flags left by a crash. Called once at server startup. */
int auth_reset_sessions(void) {
    int fd = customer_open();
    if (fd < 0) return -1;

    int n = customer_count(fd);
    int rc = (n < 0) ? -1 : 0;
    for (int i = 0; i < n; i++) {
        if (customer_lock(fd, i, F_WRLCK) < 0) { rc = -1; continue; }
        Customer c;
        if (customer_read_record(fd, i, &c) != 0) {
            rc = -1;
        } else if (c.logged_in) {
            c.logged_in = 0;
            if (customer_write_record(fd, i, &c) != 0) rc = -1;
        }
        customer_unlock(fd, i);
    }
    close(fd);
    return rc;
}