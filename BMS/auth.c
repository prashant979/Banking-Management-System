#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "common.h"
#include "utils.h"
#include "customer.h"
#include "staff.h"
#include "auth.h"

#define MAX_LOGIN_ATTEMPTS 3

/* Locate -> lock ONE record -> re-check under the lock -> set logged_in.
   Only this customer's record is locked, so other customers are never blocked. */
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

/* Same pattern for staff accounts; the role decides which file is searched. */
static AuthStatus login_staff(int role, const char *username, const char *password, Session *s) {
    int fd = staff_open(role);
    if (fd < 0) return AUTH_ERROR;

    int idx = staff_find_by_username(fd, username);          /* lock-free hint */
    if (idx == -2) { close(fd); return AUTH_ERROR; }
    if (idx < 0)   { close(fd); return AUTH_INVALID_CREDENTIALS; }

    if (staff_lock(fd, idx, F_WRLCK) < 0) { close(fd); return AUTH_ERROR; }

    AuthStatus status;
    Staff m;
    if (staff_read_record(fd, idx, &m) != 0) {
        status = AUTH_ERROR;
    } else if (strcmp(m.username, username) != 0 || strcmp(m.password, password) != 0) {
        status = AUTH_INVALID_CREDENTIALS;
    } else if (!m.active) {
        status = AUTH_ACCOUNT_INACTIVE;
    } else if (m.logged_in) {
        status = AUTH_ALREADY_LOGGED_IN;
    } else {
        m.logged_in = 1;
        if (staff_write_record(fd, idx, &m) != 0) {
            status = AUTH_ERROR;
        } else {
            memset(s, 0, sizeof(*s));
            s->role      = role;
            s->user_id   = m.id;
            s->logged_in = 1;
            strncpy(s->username, m.username, sizeof(s->username) - 1);
            status = AUTH_OK;
        }
    }

    staff_unlock(fd, idx);
    close(fd);
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

static void logout_staff(int role, int user_id) {
    int fd = staff_open(role);
    if (fd < 0) return;

    int idx = staff_find_by_id(fd, user_id);
    if (idx >= 0 && staff_lock(fd, idx, F_WRLCK) == 0) {
        Staff m;
        if (staff_read_record(fd, idx, &m) == 0 && m.id == user_id) {
            m.logged_in = 0;
            staff_write_record(fd, idx, &m);
        }
        staff_unlock(fd, idx);
    }
    close(fd);
}

/* Role dispatch: add ROLE_MANAGER / ROLE_ADMIN cases here when those modules arrive */
AuthStatus auth_login(int role, const char *username, const char *password, Session *s) {
    switch (role) {
    case ROLE_CUSTOMER: return login_customer(username, password, s);
    case ROLE_EMPLOYEE:
    case ROLE_MANAGER: 
    case ROLE_ADMIN:  return login_staff(role, username, password, s);
    default:            return AUTH_ROLE_UNSUPPORTED;
    }
}

void auth_logout(Session *s) {
    if (!s->logged_in) return;
    if (s->role == ROLE_CUSTOMER)
        logout_customer(s->user_id);
    else
        logout_staff(s->role, s->user_id);
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

/* ---- startup cleanup: clear stale logged_in flags left by a crash ---- */

static int reset_customers(void) {
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

static int reset_staff(int role) {
    int fd = staff_open(role);
    if (fd < 0) return -1;

    int n = staff_count(fd);
    int rc = (n < 0) ? -1 : 0;
    for (int i = 0; i < n; i++) {
        if (staff_lock(fd, i, F_WRLCK) < 0) { rc = -1; continue; }
        Staff m;
        if (staff_read_record(fd, i, &m) != 0) {
            rc = -1;
        } else if (m.logged_in) {
            m.logged_in = 0;
            if (staff_write_record(fd, i, &m) != 0) rc = -1;
        }
        staff_unlock(fd, i);
    }
    close(fd);
    return rc;
}

int auth_reset_sessions(void) {
    int rc = reset_customers();
    if (reset_staff(ROLE_EMPLOYEE) != 0) rc = -1;
    if (reset_staff(ROLE_MANAGER)  != 0) rc = -1;
    if (reset_staff(ROLE_ADMIN) != 0) rc = -1;
    return rc;
}

/* Shared change-password screen. The session decides whose password changes and which
   store is used, so the client can never name another account. */
void auth_prompt_change_password(int fd, const Session *s) {
    char old_pw[BUFFER_SIZE], new1[BUFFER_SIZE], new2[BUFFER_SIZE], msg[BUFFER_SIZE];
    PasswordStatus st;

    if (!s->logged_in) {
        send_message(fd, "Access denied.\n");
        return;
    }

    if (prompt_input(fd, "Enter current password: ", old_pw, sizeof(old_pw)) <= 0) return;
    if (prompt_input(fd, "Enter new password: ", new1, sizeof(new1)) <= 0) return;
    if (prompt_input(fd, "Confirm new password: ", new2, sizeof(new2)) <= 0) return;

    if (strcmp(new1, new2) != 0) {
        send_message(fd, "Passwords do not match. Password not changed.\n");
        return;
    }

    switch (s->role) {
    case ROLE_CUSTOMER:
        st = customer_change_password(s->user_id, old_pw, new1);
        break;
    case ROLE_EMPLOYEE:
    case ROLE_MANAGER:
    case ROLE_ADMIN:
        st = staff_change_password(s->role, s->user_id, old_pw, new1);
        break;
    default:
        st = PW_ERROR;
    }

    switch (st) {
    case PW_OK:
        send_message(fd, "Password changed successfully.\n");
        break;
    case PW_WRONG_OLD:
        send_message(fd, "Current password is incorrect. Password not changed.\n");
        break;
    case PW_INVALID_NEW:
        snprintf(msg, sizeof(msg),
                 "New password must be %d-%d characters with no spaces or commas.\n",
                 MIN_PASSWORD_LEN, MAX_PASSWORD_LEN);
        send_message(fd, msg);
        break;
    case PW_SAME_AS_OLD:
        send_message(fd, "New password must be different from the current one.\n");
        break;
    default:
        send_message(fd, "Could not change the password due to a server error. "
                         "Your password was not changed.\n");
    }
}

/* Test hook: if the environment variable BMS_CRASH_POINT equals name, die immediately, like a power
   cut for this process. No cleanup, no unlock, no logout. Does nothing in normal runs. */
void crash_point(const char *name) {
    const char *p = getenv("BMS_CRASH_POINT");
    if (p && strcmp(p, name) == 0) {
        fprintf(stderr, "*** simulated crash at '%s' (pid %d) ***\n", name, (int)getpid());
        _exit(1);
    }
}