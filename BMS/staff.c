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
#include "loan.h"

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

static int staff_id_base(int role) {
    switch (role) {
    case ROLE_EMPLOYEE: return EMPLOYEE_ID_BASE;
    case ROLE_MANAGER:  return MANAGER_ID_BASE;
    case ROLE_ADMIN:    return ADMIN_ID_BASE;
    default:            return -1;
    }
}

/* Scans every row (active or not) for username, skipping row skip_idx. Each row is read under a
   brief SHARED lock. Returns 1 if found (fills *found_active / *found_id if not NULL), 0 if not,
   -1 on error. Callers hold the guards, so usernames cannot change during the scan. */
static int staff_scan_username(int fd, const char *username, int skip_idx,
                               int *found_active, int *found_id) {
    int n = staff_count(fd);
    if (n < 0) return -1;

    for (int i = 0; i < n; i++) {
        Staff s;
        int match = 0;
        if (i == skip_idx) continue;
        if (staff_lock(fd, i, F_RDLCK) < 0) return -1;
        if (staff_read_record(fd, i, &s) == 0 && strcmp(s.username, username) == 0) match = 1;
        staff_unlock(fd, i);
        if (match) {
            if (found_active) *found_active = s.active;
            if (found_id)     *found_id = s.id;
            return 1;
        }
    }
    return 0;
}

/* Employees and managers: both guards (employee first, the global staff order), username checked
   in BOTH files, then the row is appended to the role's own file. Admins use only admin.csv. */
StaffAddStatus staff_add(int role, const char *firstname, const char *lastname,
                         const char *username, const char *password, int *new_id) {
    StaffAddStatus status = STAFF_ADD_IO_ERROR;
    int efd = -1, mfd = -1, afd = -1, fd = -1;
    int eg = 0, mg = 0, ag = 0, idx = -1, row_locked = 0, r = -1;
    Staff s;

    if (staff_id_base(role) < 0)       return STAFF_ADD_BAD_ROLE;
    if (!is_valid_name(firstname))     return STAFF_ADD_BAD_FIRSTNAME;
    if (!is_valid_name(lastname))      return STAFF_ADD_BAD_LASTNAME;
    if (!is_valid_username(username))  return STAFF_ADD_BAD_USERNAME;
    if (!is_valid_password(password))  return STAFF_ADD_BAD_PASSWORD;

    if (role == ROLE_ADMIN) {
        afd = staff_open(ROLE_ADMIN);
        if (afd < 0) goto done;
        if (staff_lock_guard(afd) < 0) goto done;
        ag = 1;
        fd = afd;
        r = staff_scan_username(afd, username, -1, NULL, NULL);
    } else {
        efd = staff_open(ROLE_EMPLOYEE);
        mfd = staff_open(ROLE_MANAGER);
        if (efd < 0 || mfd < 0) goto done;
        if (staff_lock_guard(efd) < 0) goto done;            /* always employee first */
        eg = 1;
        if (staff_lock_guard(mfd) < 0) goto done;
        mg = 1;
        fd = (role == ROLE_EMPLOYEE) ? efd : mfd;
        r = staff_scan_username(efd, username, -1, NULL, NULL);
        if (r == 0) r = staff_scan_username(mfd, username, -1, NULL, NULL);
    }
    if (r < 0) goto done;
    if (r == 1) { status = STAFF_ADD_USERNAME_TAKEN; goto done; }

    memset(&s, 0, sizeof(s));
    s.active    = 1;
    s.logged_in = 0;
    strncpy(s.firstname, firstname, sizeof(s.firstname) - 1);
    strncpy(s.lastname,  lastname,  sizeof(s.lastname)  - 1);
    strncpy(s.username,  username,  sizeof(s.username)  - 1);
    strncpy(s.password,  password,  sizeof(s.password)  - 1);

    if (staff_append_locked(fd, role, &s, &idx) != 0) goto done;   /* new row stays locked */
    row_locked = 1;

    *new_id = s.id;
    status = STAFF_ADD_OK;

done:
    if (row_locked) staff_unlock(fd, idx);                   /* release in reverse order */
    if (mg) staff_unlock_guard(mfd);
    if (eg) staff_unlock_guard(efd);
    if (ag) staff_unlock_guard(afd);
    if (afd >= 0) close(afd);
    if (mfd >= 0) close(mfd);
    if (efd >= 0) close(efd);
    return status;
}
int staff_get_details(int role, int id, Staff *out) {
    int fd = staff_open(role);
    if (fd < 0) return -1;

    int idx = staff_find_by_id(fd, id);                     /* lock-free locate */
    if (idx < 0) { close(fd); return -1; }

    int rc = -1;
    if (staff_lock(fd, idx, F_RDLCK) == 0) {
        Staff s;
        if (staff_read_record(fd, idx, &s) == 0 && s.id == id) {   /* re-check under lock */
            memset(s.password, 0, sizeof(s.password));
            *out = s;
            rc = 0;
        }
        staff_unlock(fd, idx);
    }
    close(fd);
    return rc;
}

int staff_lock_guard(int fd)   { return lock_record(fd, F_WRLCK, 0, 1); }
int staff_unlock_guard(int fd) { return unlock_record(fd, 0, 1); }

int staff_append_locked(int fd, int role, Staff *s, int *idx_out) {
    char row[STAFF_RECORD_SIZE + 1];
    int base = staff_id_base(role);
    int n = staff_count(fd);
    if (base < 0 || n < 0) return -1;

    s->id = base + n + 1;
    if (record_format(s, row) != 0) return -1;

    off_t off = record_offset(n);
    if (lock_record(fd, F_WRLCK, off, STAFF_RECORD_SIZE) < 0) return -1;
    if (pwrite(fd, row, STAFF_RECORD_SIZE, off) != STAFF_RECORD_SIZE || fsync(fd) != 0) {
        if (ftruncate(fd, off) == 0) fsync(fd);
        unlock_record(fd, off, STAFF_RECORD_SIZE);
        return -1;
    }
    *idx_out = n;                                           /* row stays locked for the caller */
    return 0;
}

int staff_truncate_locked(int fd, int idx) {
    if (ftruncate(fd, record_offset(idx)) != 0) return -1;
    return (fsync(fd) == 0) ? 0 : -1;
}

StaffModStatus staff_modify(int role, int id, const char *firstname, const char *lastname,
                            const char *username, int active_change, int *conflict_id) {
    StaffModStatus st = SM_IO_ERROR;
    int efd = -1, mfd = -1, fd = -1, ofd = -1, eg = 0, mg = 0;
    int idx = -1, locked = 0, r, found_active = 0, found_id = 0;
    int chg_first = (firstname && *firstname);
    int chg_last  = (lastname  && *lastname);
    int chg_user  = (username  && *username);
    Staff s, before, cur;

    if (role != ROLE_EMPLOYEE && role != ROLE_MANAGER) return SM_BAD_ROLE;
    if (chg_first && !is_valid_name(firstname))    return SM_BAD_FIRSTNAME;
    if (chg_last  && !is_valid_name(lastname))     return SM_BAD_LASTNAME;
    if (chg_user  && !is_valid_username(username)) return SM_BAD_USERNAME;
    if (!chg_first && !chg_last && !chg_user && active_change < 0) return SM_NO_CHANGE;

    efd = staff_open(ROLE_EMPLOYEE);
    mfd = staff_open(ROLE_MANAGER);
    if (efd < 0 || mfd < 0) goto done;

    /* 1. both guards, employee first (same order as add and role change) */
    if (staff_lock_guard(efd) < 0) goto done;
    eg = 1;
    if (staff_lock_guard(mfd) < 0) goto done;
    mg = 1;

    fd  = (role == ROLE_EMPLOYEE) ? efd : mfd;               /* this person's file */
    ofd = (role == ROLE_EMPLOYEE) ? mfd : efd;               /* the other staff file */

    idx = staff_find_by_id(fd, id);                          /* lock-free locate */
    if (idx == -2) goto done;
    if (idx < 0) { st = SM_NOT_FOUND; goto done; }

    /* 2. current values. Usernames and 'active' only change under both guards, which we hold. */
    if (staff_lock(fd, idx, F_RDLCK) < 0) goto done;
    r = staff_read_record(fd, idx, &cur);
    staff_unlock(fd, idx);
    if (r != 0 || cur.id != id) goto done;

    /* 3. new username must be unused in BOTH files (active or deactivated), except this row */
    if (chg_user && strcmp(username, cur.username) != 0) {
        r = staff_scan_username(fd, username, idx, NULL, NULL);
        if (r == 0) r = staff_scan_username(ofd, username, -1, NULL, NULL);
        if (r < 0) goto done;
        if (r == 1) { st = SM_USERNAME_TAKEN; goto done; }
    }

    /* 4. activating: the same person must not already be active in the other role */
    if (active_change == 1 && !cur.active) {
        r = staff_scan_username(ofd, cur.username, -1, &found_active, &found_id);
        if (r < 0) goto done;
        if (r == 1 && found_active) {
            if (conflict_id) *conflict_id = found_id;
            st = SM_ACTIVE_IN_OTHER_ROLE;
            goto done;
        }
    }

    /* 5. exclusive lock on the row, LAST, so we never hold a row while waiting for another */
    if (staff_lock(fd, idx, F_WRLCK) < 0) goto done;
    locked = 1;
    if (staff_read_record(fd, idx, &s) != 0 || s.id != id) goto done;

    if (active_change == 0 && s.active && s.logged_in) {     /* logged_in can change: re-check */
        st = SM_LOGGED_IN;
        goto done;
    }
    if (role == ROLE_EMPLOYEE && active_change == 0 && s.active) {   /* lock order: staff row -> loan rows */
        int has = loan_employee_has_assigned(id);
        if (has < 0) goto done;
        if (has > 0) { st = SM_HAS_LOANS; goto done; }
    }
    before = s;                                              /* before-image for undo */

    if (chg_first) {
        memset(s.firstname, 0, sizeof(s.firstname));
        strncpy(s.firstname, firstname, sizeof(s.firstname) - 1);
    }
    if (chg_last) {
        memset(s.lastname, 0, sizeof(s.lastname));
        strncpy(s.lastname, lastname, sizeof(s.lastname) - 1);
    }
    if (chg_user) {
        memset(s.username, 0, sizeof(s.username));
        strncpy(s.username, username, sizeof(s.username) - 1);
    }
    if (active_change >= 0) s.active = active_change;

    if (strcmp(s.firstname, before.firstname) == 0 &&
        strcmp(s.lastname,  before.lastname)  == 0 &&
        strcmp(s.username,  before.username)  == 0 &&
        s.active == before.active) {
        st = SM_NO_CHANGE;
        goto done;
    }

    if (staff_write_record(fd, idx, &s) != 0) {              /* 6. write in place + fsync */
        staff_write_record(fd, idx, &before);                /* fallback: restore old row */
        goto done;
    }
    st = SM_OK;

done:
    if (locked) staff_unlock(fd, idx);                       /* release in reverse order */
    if (mg) staff_unlock_guard(mfd);
    if (eg) staff_unlock_guard(efd);
    if (mfd >= 0) close(mfd);
    if (efd >= 0) close(efd);
    return st;
}