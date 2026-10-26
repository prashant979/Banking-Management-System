#ifndef STAFF_H
#define STAFF_H

#include "common.h"

/* Fixed-width staff records (employee.csv / manager.csv / admin.csv).
   One fcntl byte-range lock per record, same scheme as customer.csv. */
int staff_validate_file(int role);          /* startup check; creates the file if missing */
int staff_open(int role);                   /* fd (open once per request, keep it) or -1 */
int staff_count(int fd);                    /* number of staff members, -1 on error */
int staff_lock(int fd, int idx, short type);
int staff_unlock(int fd, int idx);
int staff_read_record(int fd, int idx, Staff *s);
int staff_write_record(int fd, int idx, const Staff *s);       /* in place + fsync */
int staff_find_by_username(int fd, const char *username);      /* index, -1 not found, -2 error */
int staff_find_by_id(int fd, int id);                          /* same return values */

PasswordStatus staff_change_password(int role, int user_id, const char *old_pw, const char *new_pw);

typedef enum {
    STAFF_ADD_OK = 0,
    STAFF_ADD_BAD_FIRSTNAME,
    STAFF_ADD_BAD_LASTNAME,
    STAFF_ADD_BAD_USERNAME,
    STAFF_ADD_BAD_PASSWORD,
    STAFF_ADD_USERNAME_TAKEN,
    STAFF_ADD_BAD_ROLE,
    STAFF_ADD_IO_ERROR
} StaffAddStatus;

/* Appends a new employee / manager / admin to that role's file. */
StaffAddStatus staff_add(int role, const char *firstname, const char *lastname,
                         const char *username, const char *password, int *new_id);
   
int staff_get_details(int role, int id, Staff *out);       /* shared-lock read; password blanked */
int staff_lock_guard(int fd);                              /* byte 0 of the file */
int staff_unlock_guard(int fd);
/* Caller holds the guard. Appends *s (sets s->id); the new row is left EXCLUSIVELY locked. */
int staff_append_locked(int fd, int role, Staff *s, int *idx_out);
/* Caller holds the guard and idx is the last row: removes it again. */
int staff_truncate_locked(int fd, int idx);

#endif