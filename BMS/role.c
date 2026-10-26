#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include "common.h"
#include "utils.h"
#include "staff.h"
#include "role.h"
#include "loan.h"

/* Row: id(8),username(20),from_role(1),from_id(6),to_role(1),to_id(6),by(6),timestamp(12),status(1)\n */
#define HIST_ROW_FMT "%-8d,%-20s,%d,%-6d,%d,%-6d,%-6d,%-12ld,%c\n"

static void trim_right(char *s) {
    size_t len = strlen(s);
    while (len > 0 && s[len - 1] == ' ') s[--len] = '\0';
}

static int hist_format(const RoleChange *h, char *out) {
    if (strlen(h->username) > 20 || strchr(h->username, ',') != NULL) return -1;
    int n = snprintf(out, ROLE_HIST_RECORD_SIZE + 1, HIST_ROW_FMT,
                     h->id, h->username, h->from_role, h->from_id,
                     h->to_role, h->to_id, h->changed_by, h->timestamp, h->status);
    return (n == ROLE_HIST_RECORD_SIZE) ? 0 : -1;
}

static int hist_parse(const char *row, RoleChange *h) {
    char buf[ROLE_HIST_RECORD_SIZE + 1], status = 0;
    memcpy(buf, row, ROLE_HIST_RECORD_SIZE);
    buf[ROLE_HIST_RECORD_SIZE] = '\0';
    if (buf[ROLE_HIST_RECORD_SIZE - 1] != '\n') return -1;

    memset(h, 0, sizeof(*h));
    int n = sscanf(buf, "%d ,%20[^,],%d,%d ,%d,%d ,%d ,%ld ,%c",
                   &h->id, h->username, &h->from_role, &h->from_id,
                   &h->to_role, &h->to_id, &h->changed_by, &h->timestamp, &status);
    if (n != 9) return -1;                                  /* header, blank or damaged row */
    h->status = status;
    trim_right(h->username);
    return 0;
}

int role_validate_history(void) {
    char hdr[ROLE_HIST_RECORD_SIZE + 1];
    snprintf(hdr, sizeof(hdr), "%-8s,%-20s,%s,%-6s,%s,%-6s,%-6s,%-12s,%s\n",
             "id", "username", "f", "from", "t", "to", "by", "timestamp", "s");
    return log_validate_file(ROLE_HIST_FILE, ROLE_HIST_RECORD_SIZE, hdr);
}

/* Write-ahead: reserve a row and write it as PENDING. Returns the row number or -1. */
static long hist_begin(int hfd, RoleChange *h) {
    char row[ROLE_HIST_RECORD_SIZE + 1];
    long n = log_reserve(hfd, ROLE_HIST_RECORD_SIZE);
    if (n < 0) return -1;

    h->id        = (int)n;
    h->timestamp = (long)time(NULL);
    h->status    = TXN_STATUS_PENDING;

    int ok = (hist_format(h, row) == 0) &&
             (log_write_row(hfd, n, ROLE_HIST_RECORD_SIZE, row) == 0);
    log_release_row(hfd, n, ROLE_HIST_RECORD_SIZE);
    return ok ? n : -1;
}

static void copy_identity(Staff *dst, const Staff *src) {
    memset(dst->firstname, 0, sizeof(dst->firstname));
    memset(dst->lastname,  0, sizeof(dst->lastname));
    memset(dst->username,  0, sizeof(dst->username));
    memset(dst->password,  0, sizeof(dst->password));
    strncpy(dst->firstname, src->firstname, sizeof(dst->firstname) - 1);
    strncpy(dst->lastname,  src->lastname,  sizeof(dst->lastname)  - 1);
    strncpy(dst->username,  src->username,  sizeof(dst->username)  - 1);
    strncpy(dst->password,  src->password,  sizeof(dst->password)  - 1);
}

/* Employee <-> Manager. Lock order: employee guard -> manager guard -> source row -> target rows
   -> history row. Only role changes hold locks in two staff files, and they always start with the
   employee guard, so they run one at a time and cannot deadlock. */
RoleStatus role_change(int from_role, int from_id, int admin_id, int *to_role_out, int *to_id_out) {
    RoleStatus st = RC_IO_ERROR;
    int to_role, efd = -1, mfd = -1, hfd = -1, eg = 0, mg = 0;
    int src_fd = -1, dst_fd = -1, src_idx = -1, dst_idx = -1;
    int src_locked = 0, dst_locked = 0, reused = 0, appended = 0, n, i;
    long hrow = -1;
    Staff src, src_before, dst, dst_before, other;
    RoleChange h;

    if (from_role == ROLE_EMPLOYEE)     to_role = ROLE_MANAGER;
    else if (from_role == ROLE_MANAGER) to_role = ROLE_EMPLOYEE;
    else return RC_BAD_ROLE;

    efd = staff_open(ROLE_EMPLOYEE);
    mfd = staff_open(ROLE_MANAGER);
    hfd = open(ROLE_HIST_FILE, O_RDWR);
    if (efd < 0 || mfd < 0 || hfd < 0) goto done;

    /* 1. both guards, always employee first */
    if (staff_lock_guard(efd) < 0) goto done;
    eg = 1;
    if (staff_lock_guard(mfd) < 0) goto done;
    mg = 1;

    src_fd = (from_role == ROLE_EMPLOYEE) ? efd : mfd;
    dst_fd = (from_role == ROLE_EMPLOYEE) ? mfd : efd;

    /* 2. the person's current account */
    src_idx = staff_find_by_id(src_fd, from_id);
    if (src_idx == -2) goto done;
    if (src_idx < 0) { st = RC_NOT_FOUND; goto done; }
    if (staff_lock(src_fd, src_idx, F_WRLCK) < 0) goto done;
    src_locked = 1;

    if (staff_read_record(src_fd, src_idx, &src) != 0 || src.id != from_id) goto done;
    if (!src.active)   { st = RC_INACTIVE;  goto done; }
    if (src.logged_in) { st = RC_LOGGED_IN; goto done; }
    src_before = src;

    if (from_role == ROLE_EMPLOYEE) {                                 /* source row is held: no assign can slip in */
        int has = loan_employee_has_assigned(from_id);
        if (has < 0) goto done;
        if (has > 0) { st = RC_HAS_LOANS; goto done; }
    }
    /* 3. target file: an ACTIVE row with this username blocks; an INACTIVE one is reused */
    n = staff_count(dst_fd);
    if (n < 0) goto done;
    for (i = 0; i < n; i++) {
        int match = 0;
        if (staff_lock(dst_fd, i, F_RDLCK) < 0) goto done;
        if (staff_read_record(dst_fd, i, &other) == 0 && strcmp(other.username, src.username) == 0)
            match = 1;
        staff_unlock(dst_fd, i);
        if (match) {
            if (other.active) { st = RC_USERNAME_TAKEN; goto done; }
            dst_idx = i;
            break;
        }
    }

    memset(&h, 0, sizeof(h));
    strncpy(h.username, src.username, sizeof(h.username) - 1);
    h.from_role  = from_role;
    h.from_id    = from_id;
    h.to_role    = to_role;
    h.changed_by = admin_id;

    if (dst_idx >= 0) {                                     /* returning to a role held before */
        if (staff_lock(dst_fd, dst_idx, F_WRLCK) < 0) goto done;
        dst_locked = 1;
        if (staff_read_record(dst_fd, dst_idx, &dst) != 0 ||
            strcmp(dst.username, src.username) != 0) goto done;
        if (dst.active) { st = RC_USERNAME_TAKEN; goto done; }
        dst_before = dst;
        h.to_id = dst.id;
    } else {
        h.to_id = ((to_role == ROLE_EMPLOYEE) ? EMPLOYEE_ID_BASE : MANAGER_ID_BASE) + n + 1;               /* the row staff_append_locked will use */
    }

    /* 4. write-ahead: durable PENDING row before any account changes */
    hrow = hist_begin(hfd, &h);
    if (hrow < 0) goto done;

    /* 5a. activate the account in the new role */
    if (dst_idx >= 0) {
        copy_identity(&dst, &src);
        dst.active = 1;
        dst.logged_in = 0;
        if (staff_write_record(dst_fd, dst_idx, &dst) != 0) {
            staff_write_record(dst_fd, dst_idx, &dst_before);
            goto void_out;
        }
        reused = 1;
    } else {
        Staff ns;
        memset(&ns, 0, sizeof(ns));
        copy_identity(&ns, &src);
        ns.active = 1;
        ns.logged_in = 0;
        if (staff_append_locked(dst_fd, to_role, &ns, &dst_idx) != 0) goto void_out;
        dst_locked = 1;                                     /* new row is held until we finish */
        appended = 1;
        if (ns.id != h.to_id) goto undo;                    /* should never happen under the guard */
    }

    /* 5b. deactivate the old account */
    src.active = 0;
    if (staff_write_record(src_fd, src_idx, &src) != 0) goto undo;

    /* 6. commit */
    if (log_set_status(hfd, hrow, ROLE_HIST_RECORD_SIZE, TXN_STATUS_COMMITTED) != 0) goto undo;

    *to_role_out = to_role;
    *to_id_out   = h.to_id;
    st = RC_OK;
    goto done;

undo:                                                       /* put both accounts back */
    staff_write_record(src_fd, src_idx, &src_before);
    if (reused)   staff_write_record(dst_fd, dst_idx, &dst_before);
    if (appended) staff_truncate_locked(dst_fd, dst_idx);   /* safe: we hold the target guard */
void_out:
    log_set_status(hfd, hrow, ROLE_HIST_RECORD_SIZE, TXN_STATUS_VOID);
done:
    if (dst_locked) staff_unlock(dst_fd, dst_idx);          /* release in reverse order */
    if (src_locked) staff_unlock(src_fd, src_idx);
    if (mg) staff_unlock_guard(mfd);
    if (eg) staff_unlock_guard(efd);
    if (hfd >= 0) close(hfd);
    if (mfd >= 0) close(mfd);
    if (efd >= 0) close(efd);
    return st;
}

/* Each row read under a SHARED lock on that row only. Only COMMITTED changes are returned. */
int role_history_for_user(const char *username, RoleChange *out, int max) {
    int fd = open(ROLE_HIST_FILE, O_RDONLY);
    if (fd < 0) return -1;

    long rows = log_row_count(fd, ROLE_HIST_RECORD_SIZE);
    if (rows < 0) { close(fd); return -1; }

    int n = 0;
    for (long r = 1; r <= rows && n < max; r++) {
        off_t off = (off_t)r * ROLE_HIST_RECORD_SIZE;
        char row[ROLE_HIST_RECORD_SIZE];
        RoleChange h;

        if (lock_record(fd, F_RDLCK, off, ROLE_HIST_RECORD_SIZE) < 0) continue;
        ssize_t got = pread(fd, row, ROLE_HIST_RECORD_SIZE, off);
        unlock_record(fd, off, ROLE_HIST_RECORD_SIZE);

        if (got != ROLE_HIST_RECORD_SIZE || hist_parse(row, &h) != 0) continue;
        if (h.status != TXN_STATUS_COMMITTED || strcmp(h.username, username) != 0) continue;
        out[n++] = h;
    }
    close(fd);
    return n;
}