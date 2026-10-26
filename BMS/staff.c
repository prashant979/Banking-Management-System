#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include "common.h"
#include "utils.h"
#include "staff.h"

/* Row 0 is the header; staff member i (0-based) is row i+1. Every row is exactly
   STAFF_RECORD_SIZE bytes:  id(6),first(20),last(20),user(20),pass(20),active,logged_in\n */

#define STAFF_ROW_FMT "%-6d,%-20s,%-20s,%-20s,%-20s,%d,%d\n"

static const char *staff_path(int role) {
    switch (role) {
    case ROLE_EMPLOYEE: return EMPLOYEE_FILE;
    case ROLE_MANAGER:  return MANAGER_FILE;
    case ROLE_ADMIN:    return ADMIN_FILE;
    default:            return NULL;
    }
}

static off_t record_offset(int idx) {
    return (off_t)(idx + 1) * STAFF_RECORD_SIZE;
}

static void trim_right(char *s) {
    size_t len = strlen(s);
    while (len > 0 && s[len - 1] == ' ') s[--len] = '\0';
}

static int record_parse(const char *row, Staff *s) {
    char buf[STAFF_RECORD_SIZE + 1];
    memcpy(buf, row, STAFF_RECORD_SIZE);
    buf[STAFF_RECORD_SIZE] = '\0';
    if (buf[STAFF_RECORD_SIZE - 1] != '\n') return -1;

    memset(s, 0, sizeof(*s));
    int n = sscanf(buf, "%d ,%20[^,],%20[^,],%20[^,],%20[^,],%d,%d",
                   &s->id, s->firstname, s->lastname, s->username, s->password,
                   &s->active, &s->logged_in);
    if (n != 7) return -1;

    trim_right(s->firstname);
    trim_right(s->lastname);
    trim_right(s->username);
    trim_right(s->password);
    return 0;
}

static int field_ok(const char *s) {
    return strlen(s) <= CUSTOMER_FIELD_LEN && strchr(s, ',') == NULL && strchr(s, '\n') == NULL;
}

static int record_format(const Staff *s, char *out) {
    if (!field_ok(s->firstname) || !field_ok(s->lastname) ||
        !field_ok(s->username)  || !field_ok(s->password))
        return -1;
    int n = snprintf(out, STAFF_RECORD_SIZE + 1, STAFF_ROW_FMT,
                     s->id, s->firstname, s->lastname, s->username, s->password,
                     s->active ? 1 : 0, s->logged_in ? 1 : 0);
    return (n == STAFF_RECORD_SIZE) ? 0 : -1;
}

int staff_open(int role) {
    const char *path = staff_path(role);
    return path ? open(path, O_RDWR) : -1;
}

int staff_count(int fd) {
    struct stat st;
    if (fstat(fd, &st) < 0) return -1;
    if (st.st_size < STAFF_RECORD_SIZE || st.st_size % STAFF_RECORD_SIZE != 0) return -1;
    return (int)(st.st_size / STAFF_RECORD_SIZE) - 1;
}

int staff_lock(int fd, int idx, short type) {
    return lock_record(fd, type, record_offset(idx), STAFF_RECORD_SIZE);
}

int staff_unlock(int fd, int idx) {
    return unlock_record(fd, record_offset(idx), STAFF_RECORD_SIZE);
}

int staff_read_record(int fd, int idx, Staff *s) {
    char row[STAFF_RECORD_SIZE];
    ssize_t n;
    do {
        n = pread(fd, row, STAFF_RECORD_SIZE, record_offset(idx));
    } while (n < 0 && errno == EINTR);
    if (n != STAFF_RECORD_SIZE) return -1;
    return record_parse(row, s);
}

/* Caller must hold an exclusive lock on this record. */
int staff_write_record(int fd, int idx, const Staff *s) {
    char row[STAFF_RECORD_SIZE + 1];
    if (record_format(s, row) != 0) return -1;
    if (pwrite(fd, row, STAFF_RECORD_SIZE, record_offset(idx)) != STAFF_RECORD_SIZE) return -1;
    return (fsync(fd) == 0) ? 0 : -1;
}

/* Lock-free LOCATE: only a hint. Callers lock the record and re-read before trusting it. */
static int find_staff(int fd, const char *username, int id) {
    int n = staff_count(fd);
    if (n < 0) return -2;
    for (int i = 0; i < n; i++) {
        Staff s;
        if (staff_read_record(fd, i, &s) != 0) continue;
        if (username ? (strcmp(s.username, username) == 0) : (s.id == id)) return i;
    }
    return -1;
}

int staff_find_by_username(int fd, const char *username) {
    return find_staff(fd, username, 0);
}

int staff_find_by_id(int fd, int id) {
    return find_staff(fd, NULL, id);
}

/* Startup check: create the file with its header if missing, then verify every row. */
int staff_validate_file(int role) {
    const char *path = staff_path(role);
    if (!path) return -1;

    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return -1; }

    if (st.st_size == 0) {
        char hdr[STAFF_RECORD_SIZE + 1];
        snprintf(hdr, sizeof(hdr), "%-6s,%-20s,%-20s,%-20s,%-20s,%s,%s\n",
                 "id", "firstname", "lastname", "username", "password", "a", "l");
        int ok = (write(fd, hdr, STAFF_RECORD_SIZE) == STAFF_RECORD_SIZE) && (fsync(fd) == 0);
        close(fd);
        return ok ? 0 : -1;
    }

    int n = staff_count(fd);
    int rc = (n < 0) ? -1 : 0;
    for (int i = 0; rc == 0 && i < n; i++) {
        Staff s;
        if (staff_read_record(fd, i, &s) != 0) rc = -1;
    }
    close(fd);
    return rc;
}

/* Verifies the old password and saves the new one under an EXCLUSIVE lock on this staff
   member's record only. A failed write restores the previous row. Works for any staff file. */
PasswordStatus staff_change_password(int role, int user_id, const char *old_pw, const char *new_pw) {
    if (password_policy_ok(new_pw) != 0) return PW_INVALID_NEW;

    int fd = staff_open(role);
    if (fd < 0) return PW_ERROR;

    int idx = staff_find_by_id(fd, user_id);              /* lock-free locate */
    if (idx < 0) { close(fd); return PW_ERROR; }

    PasswordStatus st = PW_ERROR;
    if (staff_lock(fd, idx, F_WRLCK) == 0) {
        Staff m;
        if (staff_read_record(fd, idx, &m) == 0 && m.id == user_id) {   /* re-check under lock */
            if (strcmp(m.password, old_pw) != 0) {
                st = PW_WRONG_OLD;
            } else if (strcmp(old_pw, new_pw) == 0) {
                st = PW_SAME_AS_OLD;
            } else {
                Staff before = m;                         /* before-image for undo */
                memset(m.password, 0, sizeof(m.password));
                strncpy(m.password, new_pw, sizeof(m.password) - 1);
                if (staff_write_record(fd, idx, &m) == 0) {
                    st = PW_OK;
                } else {
                    staff_write_record(fd, idx, &before); /* fallback: old password stays */
                    st = PW_ERROR;
                }
            }
        }
        staff_unlock(fd, idx);
    }
    close(fd);                                            /* close only after unlocking */
    return st;
}