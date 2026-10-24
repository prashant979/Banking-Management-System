#ifndef AUTH_H
#define AUTH_H

#include "common.h"

typedef enum {
    AUTH_OK = 0,
    AUTH_INVALID_CREDENTIALS,
    AUTH_ACCOUNT_INACTIVE,
    AUTH_ALREADY_LOGGED_IN,
    AUTH_ROLE_UNSUPPORTED,
    AUTH_ERROR
} AuthStatus;

/* Return values of auth_prompt_login() */
#define LOGIN_OK            0
#define LOGIN_FAILED       -1
#define LOGIN_DISCONNECTED -2

AuthStatus auth_login(int role, const char *username, const char *password, Session *session);
void       auth_logout(Session *session);
int        auth_prompt_login(int fd, int role, Session *session);
int        auth_require_role(const Session *session, int role);
int        auth_reset_sessions(void);   /* clears stale logged_in flags (server startup) */

#endif
