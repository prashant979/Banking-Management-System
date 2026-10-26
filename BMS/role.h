#ifndef ROLE_H
#define ROLE_H

#include "common.h"

typedef enum {
    RC_OK = 0,
    RC_BAD_ROLE,
    RC_NOT_FOUND,
    RC_INACTIVE,
    RC_LOGGED_IN,
    RC_USERNAME_TAKEN,
    RC_HAS_LOANS,
    RC_IO_ERROR
} RoleStatus;

int        role_validate_history(void);      /* startup: create or verify role_history.csv */
RoleStatus role_change(int from_role, int from_id, int admin_id, int *to_role, int *to_id);
int        role_history_for_user(const char *username, RoleChange *out, int max);   /* COMMITTED rows, oldest first */

int role_recover(void); /* startup: roll back PENDING role changes */
#endif