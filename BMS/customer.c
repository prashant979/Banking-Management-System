#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <sys/stat.h>
#include "common.h"
#include "utils.h"
#include "auth.h"
#include "customer.h"


#define HISTORY_LIMIT 20

/* =====================  customer.csv: fixed-width record store  ===================== */
/*
 * Row 0 is the header; customer number i (0-based) is row i+1. Every row is exactly
 * RECORD_SIZE bytes:  id(6),first(20),last(20),user(20),pass(20),balance(14),active,logged_in\n
 * so a customer is a fixed byte range of the file that can be locked with fcntl.
 */

#define ROW_FMT "%-6d,%-20s,%-20s,%-20s,%-20s,%14.2f,%d,%d\n"

static off_t record_offset(int idx) {
    return (off_t)(idx + 1) * RECORD_SIZE;
}

static void trim_right(char *s) {
    size_t len = strlen(s);
    while (len > 0 && s[len - 1] == ' ') s[--len] = '\0';
}

/* Row text -> Customer. Returns 0 if the row is well formed. */
static int record_parse(const char *row, Customer *c) {
    char buf[RECORD_SIZE + 1];
    memcpy(buf, row, RECORD_SIZE);
    buf[RECORD_SIZE] = '\0';
    if (buf[RECORD_SIZE - 1] != '\n') return -1;

    memset(c, 0, sizeof(*c));
    /* "%d ," : the space lets sscanf skip the padding after the id */
    int n = sscanf(buf, "%d ,%20[^,],%20[^,],%20[^,],%20[^,],%lf,%d,%d",
                   &c->id, c->firstname, c->lastname, c->username, c->password,
                   &c->balance, &c->active, &c->logged_in);
    if (n != 8) return -1;

    trim_right(c->firstname);
    trim_right(c->lastname);
    trim_right(c->username);
    trim_right(c->password);
    return 0;
}

static int field_ok(const char *s) {
    return strlen(s) <= CUSTOMER_FIELD_LEN && strchr(s, ',') == NULL && strchr(s, '\n') == NULL;
}

/* Customer -> row text (RECORD_SIZE bytes + NUL in out). Refuses anything that would
   not fit the fixed layout, so a bad value can never shift the rows. */
static int record_format(const Customer *c, char *out) {
    if (!field_ok(c->firstname) || !field_ok(c->lastname) ||
        !field_ok(c->username)  || !field_ok(c->password))
        return -1;
    int n = snprintf(out, RECORD_SIZE + 1, ROW_FMT,
                     c->id, c->firstname, c->lastname, c->username, c->password,
                     c->balance, c->active ? 1 : 0, c->logged_in ? 1 : 0);
    return (n == RECORD_SIZE) ? 0 : -1;
}

static void header_row(char *out) {
    snprintf(out, RECORD_SIZE + 1, "%-6s,%-20s,%-20s,%-20s,%-20s,%14s,%s,%s\n",
             "id", "firstname", "lastname", "username", "password", "balance", "a", "l");
}

/* Open the data file ONCE per request and keep the descriptor until all locks are released:
   closing any descriptor of a file drops ALL of this process's fcntl locks on it. */
int customer_open(void) {
    return open(CUSTOMER_FILE, O_RDWR);
}

int customer_count(int fd) {
    struct stat st;
    if (fstat(fd, &st) < 0) return -1;
    if (st.st_size < RECORD_SIZE || st.st_size % RECORD_SIZE != 0) return -1;
    return (int)(st.st_size / RECORD_SIZE) - 1;
}

int customer_lock(int fd, int idx, short type) {
    return lock_record(fd, type, record_offset(idx), RECORD_SIZE);
}

int customer_unlock(int fd, int idx) {
    return unlock_record(fd, record_offset(idx), RECORD_SIZE);
}

int customer_read_record(int fd, int idx, Customer *c) {
    char row[RECORD_SIZE];
    ssize_t n;
    do {
        n = pread(fd, row, RECORD_SIZE, record_offset(idx));
    } while (n < 0 && errno == EINTR);
    if (n != RECORD_SIZE) return -1;
    return record_parse(row, c);
}

/* Caller must hold an exclusive lock on this record. */
int customer_write_record(int fd, int idx, const Customer *c) {
    char row[RECORD_SIZE + 1];
    if (record_format(c, row) != 0) return -1;
    if (pwrite(fd, row, RECORD_SIZE, record_offset(idx)) != RECORD_SIZE) return -1;
    return (fsync(fd) == 0) ? 0 : -1;
}

/* Lock-free LOCATE: finds which record holds this username (or id). It is only a hint;
   callers lock the record and re-read it before trusting anything in it. */
static int find_customer(int fd, const char *username, int id) {
    int n = customer_count(fd);
    if (n < 0) return -2;
    for (int i = 0; i < n; i++) {
        Customer c;
        if (customer_read_record(fd, i, &c) != 0) continue;
        if (username ? (strcmp(c.username, username) == 0) : (c.id == id)) return i;
    }
    return -1;
}

int customer_find_by_username(int fd, const char *username) {
    return find_customer(fd, username, 0);
}

int customer_find_by_id(int fd, int id) {
    return find_customer(fd, NULL, id);
}

/* Startup check: create the file with its header if missing, then verify every row. */
int customer_validate_file(void) {
    int fd = open(CUSTOMER_FILE, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return -1; }

    if (st.st_size == 0) {
        char hdr[RECORD_SIZE + 1];
        header_row(hdr);
        int ok = (write(fd, hdr, RECORD_SIZE) == RECORD_SIZE) && (fsync(fd) == 0);
        close(fd);
        return ok ? 0 : -1;
    }

    int n = customer_count(fd);
    int rc = (n < 0) ? -1 : 0;
    for (int i = 0; rc == 0 && i < n; i++) {
        Customer c;
        if (customer_read_record(fd, i, &c) != 0) rc = -1;
    }
    close(fd);
    return rc;
}

/* ---- Compatibility layer for operations not yet converted to per-record locking ----
   customer_open_locked() locks the WHOLE byte range, i.e. every record at once. That
   overlaps any record lock held by another process, so old-style and new-style code
   still exclude each other. Converted away one operation at a time. */

static int g_whole_fd = -1;           /* one request at a time per process */

FILE *customer_open_locked(short lock_type) {
    int fd = customer_open();
    if (fd < 0) return NULL;
    if (lock_record(fd, lock_type, 0, 0) < 0) { close(fd); return NULL; }

    FILE *fp = fdopen(fd, "r");       /* only a handle: data is read with pread */
    if (!fp) { unlock_record(fd, 0, 0); close(fd); return NULL; }
    g_whole_fd = fd;
    return fp;
}

void customer_close_unlock(FILE *fp) {
    unlock_record(fileno(fp), 0, 0);
    g_whole_fd = -1;
    fclose(fp);
}

int customer_read_all(FILE *fp, Customer **out) {
    int fd = fileno(fp);
    int n = customer_count(fd);
    if (n < 0) return -1;

    Customer *arr = malloc((n > 0 ? n : 1) * sizeof(Customer));
    if (!arr) return -1;
    for (int i = 0; i < n; i++) {
        if (customer_read_record(fd, i, &arr[i]) != 0) { free(arr); return -1; }
    }
    *out = arr;
    return n;
}

/* Writes only the rows that changed. All rows are validated before the first write, and if
   a write fails the rows already written are restored, so the caller still gets all-or-nothing. */
int customer_write_all(const Customer *arr, int n) {
    int fd = g_whole_fd, i, j, failed = 0, rc = -1;
    char *oldrows = NULL, *newrows = NULL;
    char tmp[RECORD_SIZE + 1];

    if (fd < 0) return -1;
    if (n == 0) return 0;

    oldrows = malloc((size_t)n * RECORD_SIZE);
    newrows = malloc((size_t)n * RECORD_SIZE);
    if (!oldrows || !newrows) goto done;

    for (i = 0; i < n; i++) {                       /* 1. validate before touching the file */
        if (record_format(&arr[i], tmp) != 0) goto done;
        memcpy(newrows + (size_t)i * RECORD_SIZE, tmp, RECORD_SIZE);
        if (pread(fd, oldrows + (size_t)i * RECORD_SIZE, RECORD_SIZE, record_offset(i)) != RECORD_SIZE)
            goto done;
    }

    for (i = 0; i < n && !failed; i++) {            /* 2. write only changed rows */
        char *o = oldrows + (size_t)i * RECORD_SIZE;
        char *w = newrows + (size_t)i * RECORD_SIZE;
        if (memcmp(o, w, RECORD_SIZE) != 0 &&
            pwrite(fd, w, RECORD_SIZE, record_offset(i)) != RECORD_SIZE)
            failed = 1;
    }

    if (failed) {                                   /* 3. fallback: put the old rows back */
        for (j = 0; j <= i && j < n; j++) {
            ssize_t r = pwrite(fd, oldrows + (size_t)j * RECORD_SIZE, RECORD_SIZE, record_offset(j));
            (void)r;
        }
        fsync(fd);
        goto done;
    }
    if (fsync(fd) != 0) goto done;
    rc = 0;

done:
    free(oldrows);
    free(newrows);
    return rc;
}

/* =====================  transaction.csv: append-only fixed-width log  ===================== */
/* Row: txn_id(10),cust(6),peer(6),type(12),amount(14),balance_after(14),timestamp(12),status(1)\n */

#define TXN_ROW_FMT "%-10d,%-6d,%-6d,%-12s,%14.2f,%14.2f,%-12ld,%c\n"

static int txn_open(void) {
    return open(TXN_FILE, O_RDWR);
}

static int txn_format(const Transaction *t, char *out) {
    if (strlen(t->type) > 12 || strchr(t->type, ',') != NULL) return -1;
    int n = snprintf(out, TXN_RECORD_SIZE + 1, TXN_ROW_FMT,
                     t->id, t->customer_id, t->peer_id, t->type,
                     t->amount, t->balance_after, t->timestamp, t->status);
    return (n == TXN_RECORD_SIZE) ? 0 : -1;
}

static int txn_parse(const char *row, Transaction *t) {
    char buf[TXN_RECORD_SIZE + 1], status = 0;
    memcpy(buf, row, TXN_RECORD_SIZE);
    buf[TXN_RECORD_SIZE] = '\0';
    if (buf[TXN_RECORD_SIZE - 1] != '\n') return -1;

    memset(t, 0, sizeof(*t));
    int n = sscanf(buf, "%d ,%d ,%d ,%12[^,],%lf,%lf,%ld ,%c",
                   &t->id, &t->customer_id, &t->peer_id, t->type,
                   &t->amount, &t->balance_after, &t->timestamp, &status);
    if (n != 8) return -1;
    if (status != TXN_STATUS_PENDING && status != TXN_STATUS_COMMITTED && status != TXN_STATUS_VOID)
        return -1;

    t->status = status;
    trim_right(t->type);
    return 0;
}

/* Write-ahead: reserve a row, write it as PENDING, fsync. Returns the row number or -1.
   The row lock is held only while the row is written. */
static long txn_begin(int lfd, int customer_id, int peer_id, const char *type,
                      double amount, double balance_after) {
    Transaction t;
    char row[TXN_RECORD_SIZE + 1];

    long n = log_reserve(lfd, TXN_RECORD_SIZE);
    if (n < 0) return -1;

    memset(&t, 0, sizeof(t));
    t.id            = (int)n;
    t.customer_id   = customer_id;
    t.peer_id       = peer_id;
    t.amount        = amount;
    t.balance_after = balance_after;
    t.timestamp     = (long)time(NULL);
    t.status        = TXN_STATUS_PENDING;
    strncpy(t.type, type, sizeof(t.type) - 1);

    int ok = (txn_format(&t, row) == 0) &&
             (log_write_row(lfd, n, TXN_RECORD_SIZE, row) == 0);
    log_release_row(lfd, n, TXN_RECORD_SIZE);
    return ok ? n : -1;                 /* on failure a blank row stays behind; readers skip it */
}

/* Change one row's status byte (PENDING -> COMMITTED or VOID) under that row's lock only. */
static int txn_finish(int lfd, long row_no, char status) {
    off_t off = (off_t)row_no * TXN_RECORD_SIZE;
    int rc = -1;

    if (lock_record(lfd, F_WRLCK, off, TXN_RECORD_SIZE) < 0) return -1;
    if (pwrite(lfd, &status, 1, off + TXN_RECORD_SIZE - 2) == 1 && fsync(lfd) == 0) rc = 0;
    unlock_record(lfd, off, TXN_RECORD_SIZE);
    return rc;
}

/* =====================  operations  ===================== */

static double round2(double x) {
    return (double)(long long)(x * 100.0 + 0.5) / 100.0;
}

static long long to_cents(double x) {
    return (long long)(x * 100.0 + 0.5);
}

/* Digits and at most one '.', max 2 decimals, 0 (only if allow_zero) up to MAX_TXN_AMOUNT. */
static int parse_money(const char *s, int allow_zero, double *out) {
    int digits = 0, dots = 0;
    for (const char *p = s; *p; p++) {
        if (isdigit((unsigned char)*p)) digits++;
        else if (*p == '.') dots++;
        else return -1;
    }
    if (digits == 0 || dots > 1) return -1;

    double v = strtod(s, NULL);
    if (v > MAX_TXN_AMOUNT) return -1;
    if (v == 0.0 && !allow_zero) return -1;

    long long cents = (long long)(v * 100.0 + 0.5);
    double diff = v * 100.0 - (double)cents;
    if (diff < 0) diff = -diff;
    if (diff > 1e-6) return -1;                         /* more than 2 decimals */

    *out = (double)cents / 100.0;
    return 0;
}

static int parse_amount(const char *s, double *out) {
    return parse_money(s, 0, out);
}

/* Digits only, 1-9 characters, value > 0. */
static int parse_account_id(const char *s, int *out) {
    size_t len = strlen(s);
    if (len == 0 || len > 9) return -1;
    for (size_t i = 0; i < len; i++)
        if (!isdigit((unsigned char)s[i])) return -1;
    int v = atoi(s);
    if (v <= 0) return -1;
    *out = v;
    return 0;
}

/* Reads the latest balance under a SHARED lock on this customer's record only. */
int customer_get_balance(int user_id, double *balance) {
    int fd = customer_open();
    if (fd < 0) return -1;

    int idx = customer_find_by_id(fd, user_id);        /* lock-free locate */
    if (idx < 0) { close(fd); return -1; }

    int rc = -1;
    if (customer_lock(fd, idx, F_RDLCK) == 0) {        /* shared: readers don't block readers */
        Customer c;
        if (customer_read_record(fd, idx, &c) == 0 && c.id == user_id) {   /* re-check under lock */
            *balance = c.balance;
            rc = 0;
        }
        customer_unlock(fd, idx);
    }
    close(fd);                                         /* close only after unlocking */
    return rc;
}

/* Returns this customer's COMMITTED transactions, oldest first. Each row is read under a
   SHARED lock on that row only, so a reader waits only if a writer is on that very row. */
int customer_get_history(int user_id, Transaction **out) {
    int fd = txn_open();
    if (fd < 0) return -1;

    long rows = log_row_count(fd, TXN_RECORD_SIZE);
    if (rows < 0) { close(fd); return -1; }

    int cap = 16, n = 0;
    Transaction *arr = malloc(cap * sizeof(Transaction));
    if (!arr) { close(fd); return -1; }

    for (long r = 1; r <= rows; r++) {
        off_t off = (off_t)r * TXN_RECORD_SIZE;
        char row[TXN_RECORD_SIZE];
        Transaction t;

        if (lock_record(fd, F_RDLCK, off, TXN_RECORD_SIZE) < 0) continue;
        ssize_t got = pread(fd, row, TXN_RECORD_SIZE, off);
        unlock_record(fd, off, TXN_RECORD_SIZE);

        if (got != TXN_RECORD_SIZE || txn_parse(row, &t) != 0) continue;   /* blank or damaged row */
        if (t.customer_id != user_id || t.status != TXN_STATUS_COMMITTED) continue;

        if (n == cap) {
            cap *= 2;
            Transaction *tmp = realloc(arr, cap * sizeof(Transaction));
            if (!tmp) { free(arr); close(fd); return -1; }
            arr = tmp;
        }
        arr[n++] = t;
    }

    close(fd);
    *out = arr;
    return n;
}

/* Feedback row: id(8),cust(6),timestamp(12),message(200)\n  (commas are fine in the message:
   it is the last column). A failed write leaves a blank row that readers skip. */
static int feedback_format(const Feedback *f, char *out) {
    if (strlen(f->message) > FEEDBACK_MAX_LEN || strchr(f->message, '\n') != NULL) return -1;
    int n = snprintf(out, FEEDBACK_RECORD_SIZE + 1, "%-8d,%-6d,%-12ld,%-200s\n",
                     f->id, f->customer_id, f->timestamp, f->message);
    return (n == FEEDBACK_RECORD_SIZE) ? 0 : -1;
}

int customer_add_feedback(int user_id, const char *message) {
    Feedback f;
    char row[FEEDBACK_RECORD_SIZE + 1];
    int rc = -1;

    int fd = open(FEEDBACK_FILE, O_RDWR);
    if (fd < 0) return -1;

    long n = log_reserve(fd, FEEDBACK_RECORD_SIZE);       /* guard lock only while reserving */
    if (n < 0) { close(fd); return -1; }

    memset(&f, 0, sizeof(f));
    f.id          = (int)n;
    f.customer_id = user_id;
    f.timestamp   = (long)time(NULL);
    strncpy(f.message, message, sizeof(f.message) - 1);

    if (feedback_format(&f, row) == 0 &&
        log_write_row(fd, n, FEEDBACK_RECORD_SIZE, row) == 0)
        rc = 0;

    log_release_row(fd, n, FEEDBACK_RECORD_SIZE);
    close(fd);
    return rc;
}

/* Startup: create transaction.csv and feedback.csv with headers if missing, else verify the format. */
int customer_validate_logs(void) {
    char hdr[FEEDBACK_RECORD_SIZE + 1];

    snprintf(hdr, sizeof(hdr), "%-10s,%-6s,%-6s,%-12s,%14s,%14s,%-12s,%s\n",
             "txn_id", "cust", "peer", "type", "amount", "balance_after", "timestamp", "s");
    if (log_validate_file(TXN_FILE, TXN_RECORD_SIZE, hdr) != 0) return -1;

    snprintf(hdr, sizeof(hdr), "%-8s,%-6s,%-12s,%-200s\n", "id", "cust", "timestamp", "message");
    return log_validate_file(FEEDBACK_FILE, FEEDBACK_RECORD_SIZE, hdr);
}

/* 6-49 printable ASCII characters, no spaces, no commas (a comma would break the CSV row). */
static int password_policy_ok(const char *pw) {
    size_t len = strlen(pw);
    if (len < MIN_PASSWORD_LEN || len > MAX_PASSWORD_LEN) return -1;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)pw[i];
        if (c <= 32 || c >= 127 || c == ',') return -1;
    }
    return 0;
}

/* Verifies the old password and saves the new one under an EXCLUSIVE lock on this
   customer's record only. A failed write restores the previous row. */
PasswordStatus customer_change_password(int user_id, const char *old_pw, const char *new_pw) {
    if (password_policy_ok(new_pw) != 0) return PW_INVALID_NEW;

    int fd = customer_open();
    if (fd < 0) return PW_ERROR;

    int idx = customer_find_by_id(fd, user_id);           /* lock-free locate */
    if (idx < 0) { close(fd); return PW_ERROR; }

    PasswordStatus st = PW_ERROR;
    if (customer_lock(fd, idx, F_WRLCK) == 0) {
        Customer c;
        if (customer_read_record(fd, idx, &c) == 0 && c.id == user_id) {   /* re-check under lock */
            if (strcmp(c.password, old_pw) != 0) {
                st = PW_WRONG_OLD;
            } else if (strcmp(old_pw, new_pw) == 0) {
                st = PW_SAME_AS_OLD;
            } else {
                Customer before = c;                      /* before-image for undo */
                strncpy(c.password, new_pw, sizeof(c.password) - 1);
                if (customer_write_record(fd, idx, &c) == 0) {
                    st = PW_OK;
                } else {
                    customer_write_record(fd, idx, &before);   /* fallback: old password stays */
                    st = PW_ERROR;
                }
            }
        }
        customer_unlock(fd, idx);
    }
    close(fd);                                            /* close only after unlocking */
    return st;
}

static int valid_name(const char *s) {                  /* letters only, 1-20 */
    size_t len = strlen(s);
    if (len < 1 || len > CUSTOMER_FIELD_LEN) return 0;
    for (size_t i = 0; i < len; i++)
        if (!isalpha((unsigned char)s[i])) return 0;
    return 1;
}

static int valid_username(const char *s) {              /* lowercase letters, digits, '_', 3-20 */
    size_t len = strlen(s);
    if (len < 3 || len > CUSTOMER_FIELD_LEN) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!islower(c) && !isdigit(c) && c != '_') return 0;
    }
    return 1;
}

AddStatus customer_add(const char *firstname, const char *lastname, const char *username,
                       const char *password, const char *balance_str, int *new_id) {
    AddStatus status = ADD_IO_ERROR;
    int guard = 0, row_locked = 0, n, i;
    off_t off = 0;
    double balance;
    Customer c, other;
    char row[RECORD_SIZE + 1];

    if (!valid_name(firstname))               return ADD_BAD_FIRSTNAME;
    if (!valid_name(lastname))                return ADD_BAD_LASTNAME;
    if (!valid_username(username))            return ADD_BAD_USERNAME;
    if (password_policy_ok(password) != 0)    return ADD_BAD_PASSWORD;
    if (parse_money(balance_str, 1, &balance) != 0) return ADD_BAD_BALANCE;

    int fd = customer_open();
    if (fd < 0) return ADD_IO_ERROR;

    if (lock_record(fd, F_WRLCK, 0, 1) < 0) goto done;     /* guard: one add at a time */
    guard = 1;

    n = customer_count(fd);                                /* rows are stable while we hold the guard */
    if (n < 0) goto done;

    for (i = 0; i < n; i++) {                              /* username must be unused */
        int taken = 0;
        if (customer_lock(fd, i, F_RDLCK) < 0) goto done;
        if (customer_read_record(fd, i, &other) == 0 && strcmp(other.username, username) == 0)
            taken = 1;
        customer_unlock(fd, i);
        if (taken) { status = ADD_USERNAME_TAKEN; goto done; }
    }

    memset(&c, 0, sizeof(c));
    c.id        = CUSTOMER_ID_BASE + n + 1;                /* row number n + 1 */
    c.balance   = balance;
    c.active    = 1;
    c.logged_in = 0;
    strncpy(c.firstname, firstname, sizeof(c.firstname) - 1);
    strncpy(c.lastname,  lastname,  sizeof(c.lastname)  - 1);
    strncpy(c.username,  username,  sizeof(c.username)  - 1);
    strncpy(c.password,  password,  sizeof(c.password)  - 1);
    if (record_format(&c, row) != 0) goto done;

    off = record_offset(n);                                /* = current end of file */
    if (lock_record(fd, F_WRLCK, off, RECORD_SIZE) < 0) goto done;
    row_locked = 1;

    if (pwrite(fd, row, RECORD_SIZE, off) != RECORD_SIZE || fsync(fd) != 0) {
        if (ftruncate(fd, off) == 0) fsync(fd);            /* fallback: cut the new row off again */
        goto done;
    }

    *new_id = c.id;
    status = ADD_OK;

done:
    if (row_locked) unlock_record(fd, off, RECORD_SIZE);
    if (guard)      unlock_record(fd, 0, 1);
    close(fd);
    return status;
}

static TxnStatus apply_transaction(int user_id, double amount,
                                   const char *type, int is_debit,
                                   double *new_balance) {
    TxnStatus status = TXN_IO_ERROR;
    int idx, locked = 0, lfd = -1;
    long row_no;
    double updated;
    Customer c, before;

    int fd = customer_open();
    if (fd < 0) return TXN_IO_ERROR;

    idx = customer_find_by_id(fd, user_id);               /* lock-free locate */
    if (idx == -2) goto done;
    if (idx < 0)   { status = TXN_NOT_FOUND; goto done; }

    if (customer_lock(fd, idx, F_WRLCK) < 0) goto done;   /* exclusive lock on ONE record */
    locked = 1;

    if (customer_read_record(fd, idx, &c) != 0 || c.id != user_id) goto done;
    if (!c.active) { status = TXN_INACTIVE; goto done; }

    if (is_debit && to_cents(c.balance) < to_cents(amount)) {
        *new_balance = c.balance;
        status = TXN_INSUFFICIENT_FUNDS;
        goto done;
    }

    before  = c;                                          /* before-image for undo */
    updated = round2(is_debit ? c.balance - amount : c.balance + amount);

    lfd = txn_open();
    if (lfd < 0) goto done;

    row_no = txn_begin(lfd, user_id, 0, type, amount, updated);   /* 1. durable PENDING row */
    if (row_no < 0) goto done;

    c.balance = updated;
    if (customer_write_record(fd, idx, &c) != 0) {        /* 2. change the balance */
        customer_write_record(fd, idx, &before);
        txn_finish(lfd, row_no, TXN_STATUS_VOID);
        goto done;
    }

    if (txn_finish(lfd, row_no, TXN_STATUS_COMMITTED) != 0) {   /* 3. commit */
        customer_write_record(fd, idx, &before);
        txn_finish(lfd, row_no, TXN_STATUS_VOID);
        goto done;
    }

    *new_balance = updated;
    status = TXN_OK;

done:
    if (lfd >= 0) close(lfd);
    if (locked) customer_unlock(fd, idx);
    close(fd);                                            /* close only after unlocking */
    return status;
}

TxnStatus customer_deposit(int user_id, double amount, double *new_balance) {
    return apply_transaction(user_id, amount, TXN_TYPE_DEPOSIT, 0, new_balance);
}

TxnStatus customer_withdraw(int user_id, double amount, double *new_balance) {
    return apply_transaction(user_id, amount, TXN_TYPE_WITHDRAW, 1, new_balance);
}

TxnStatus customer_transfer(int from_id, int to_id, double amount, double *new_balance) {
    TxnStatus status = TXN_IO_ERROR;
    int from, to, lo, hi, lo_locked = 0, hi_locked = 0, lfd = -1;
    long row_out = -1, row_in = -1;
    double src_after, dst_after;
    Customer src, dst, src_before, dst_before;

    if (from_id == to_id) return TXN_SAME_ACCOUNT;

    int fd = customer_open();
    if (fd < 0) return TXN_IO_ERROR;

    from = customer_find_by_id(fd, from_id);              /* lock-free locate */
    to   = customer_find_by_id(fd, to_id);
    if (from == -2 || to == -2) goto done;
    if (from < 0) { status = TXN_NOT_FOUND;         goto done; }
    if (to   < 0) { status = TXN_RECIPIENT_INVALID; goto done; }

    /* Global lock order: lower record position first */
    lo = (from < to) ? from : to;
    hi = (from < to) ? to : from;

    if (customer_lock(fd, lo, F_WRLCK) < 0) goto done;
    lo_locked = 1;
    if (customer_lock(fd, hi, F_WRLCK) < 0) goto done;
    hi_locked = 1;

    if (customer_read_record(fd, from, &src) != 0 || src.id != from_id) goto done;
    if (customer_read_record(fd, to,   &dst) != 0 || dst.id != to_id)   goto done;

    if (!src.active) { status = TXN_INACTIVE;          goto done; }
    if (!dst.active) { status = TXN_RECIPIENT_INVALID; goto done; }

    if (to_cents(src.balance) < to_cents(amount)) {
        *new_balance = src.balance;
        status = TXN_INSUFFICIENT_FUNDS;
        goto done;
    }

    src_before = src;
    dst_before = dst;
    src_after  = round2(src.balance - amount);
    dst_after  = round2(dst.balance + amount);

    lfd = txn_open();
    if (lfd < 0) goto done;

    /* 1. both PENDING rows are durable BEFORE any balance changes */
    row_out = txn_begin(lfd, from_id, to_id, TXN_TYPE_TRANSFER_OUT, amount, src_after);
    if (row_out < 0) goto done;
    row_in = txn_begin(lfd, to_id, from_id, TXN_TYPE_TRANSFER_IN, amount, dst_after);
    if (row_in < 0) { txn_finish(lfd, row_out, TXN_STATUS_VOID); goto done; }

    /* 2. change both balances */
    src.balance = src_after;
    dst.balance = dst_after;
    if (customer_write_record(fd, from, &src) != 0 ||
        customer_write_record(fd, to,   &dst) != 0) {
        customer_write_record(fd, from, &src_before);     /* fallback: restore both rows ... */
        customer_write_record(fd, to,   &dst_before);
        txn_finish(lfd, row_out, TXN_STATUS_VOID);        /* ... and void both log rows */
        txn_finish(lfd, row_in,  TXN_STATUS_VOID);
        goto done;
    }

    /* 3. commit both rows */
    if (txn_finish(lfd, row_out, TXN_STATUS_COMMITTED) != 0 ||
        txn_finish(lfd, row_in,  TXN_STATUS_COMMITTED) != 0) {
        customer_write_record(fd, from, &src_before);
        customer_write_record(fd, to,   &dst_before);
        txn_finish(lfd, row_out, TXN_STATUS_VOID);
        txn_finish(lfd, row_in,  TXN_STATUS_VOID);
        goto done;
    }

    *new_balance = src_after;
    status = TXN_OK;

done:
    if (lfd >= 0) close(lfd);
    if (hi_locked) customer_unlock(fd, hi);               /* release in reverse order */
    if (lo_locked) customer_unlock(fd, lo);
    close(fd);
    return status;
}

/* =====================  customer session  ===================== */

static void view_balance(int fd, const Session *session) {
    double balance;
    char msg[BUFFER_SIZE];

    if (customer_get_balance(session->user_id, &balance) != 0) {
        send_message(fd, "Could not retrieve balance. Please try again.\n");
        return;
    }
    snprintf(msg, sizeof(msg),
             "Account holder : %s\nCurrent balance: %.2f\n",
             session->username, balance);
    send_message(fd, msg);
}

/* is_deposit = 1 for deposit, 0 for withdraw */
static void do_money_op(int fd, const Session *session, int is_deposit) {
    char buf[BUFFER_SIZE], msg[BUFFER_SIZE];
    double amount, balance;
    const char *verb = is_deposit ? "deposit" : "withdraw";

    snprintf(msg, sizeof(msg), "Enter amount to %s: ", verb);
    if (prompt_input(fd, msg, buf, sizeof(buf)) <= 0)
        return;                                          /* disconnected: menu loop will notice */

    if (parse_amount(buf, &amount) != 0) {
        snprintf(msg, sizeof(msg),
                 "Invalid amount. Enter a positive number with at most 2 decimals (max %.2f).\n",
                 MAX_TXN_AMOUNT);
        send_message(fd, msg);
        return;
    }

    TxnStatus st = is_deposit ? customer_deposit(session->user_id, amount, &balance)
                              : customer_withdraw(session->user_id, amount, &balance);

    switch (st) {
    case TXN_OK:
        snprintf(msg, sizeof(msg),
                 "%s successful.\n%s : %.2f\nNew balance: %.2f\n",
                 is_deposit ? "Deposit" : "Withdrawal",
                 is_deposit ? "Deposited " : "Withdrawn  ",
                 amount, balance);
        send_message(fd, msg);
        break;
    case TXN_INSUFFICIENT_FUNDS:
        snprintf(msg, sizeof(msg),
                 "Insufficient funds. Available balance: %.2f. Transaction cancelled.\n",
                 balance);
        send_message(fd, msg);
        break;
    case TXN_INACTIVE:
        send_message(fd, "Your account is deactivated. Transaction cancelled.\n");
        break;
    case TXN_NOT_FOUND:
        send_message(fd, "Account not found. Transaction cancelled.\n");
        break;
    default:
        snprintf(msg, sizeof(msg),
                 "Transaction failed due to a server error. "
                 "No money was %s. Please try again.\n",
                 is_deposit ? "deposited" : "withdrawn");
        send_message(fd, msg);
    }
}

static void do_transfer(int fd, const Session *session) {
    char buf[BUFFER_SIZE], msg[BUFFER_SIZE];
    int to_id;
    double amount, balance;

    if (prompt_input(fd, "Enter recipient account ID: ", buf, sizeof(buf)) <= 0)
        return;                                          /* disconnected */
    if (parse_account_id(buf, &to_id) != 0) {
        send_message(fd, "Invalid account ID.\n");
        return;
    }

    if (prompt_input(fd, "Enter amount to transfer: ", buf, sizeof(buf)) <= 0)
        return;
    if (parse_amount(buf, &amount) != 0) {
        snprintf(msg, sizeof(msg),
                 "Invalid amount. Enter a positive number with at most 2 decimals (max %.2f).\n",
                 MAX_TXN_AMOUNT);
        send_message(fd, msg);
        return;
    }

    switch (customer_transfer(session->user_id, to_id, amount, &balance)) {
    case TXN_OK:
        snprintf(msg, sizeof(msg),
                 "Transfer successful.\nTransferred : %.2f to account %d\nNew balance : %.2f\n",
                 amount, to_id, balance);
        send_message(fd, msg);
        break;
    case TXN_SAME_ACCOUNT:
        send_message(fd, "You cannot transfer money to your own account.\n");
        break;
    case TXN_RECIPIENT_INVALID:
        send_message(fd, "Recipient account not found or not active. Transfer cancelled.\n");
        break;
    case TXN_INSUFFICIENT_FUNDS:
        snprintf(msg, sizeof(msg),
                 "Insufficient funds. Available balance: %.2f. Transfer cancelled.\n", balance);
        send_message(fd, msg);
        break;
    case TXN_INACTIVE:
        send_message(fd, "Your account is deactivated. Transfer cancelled.\n");
        break;
    case TXN_NOT_FOUND:
        send_message(fd, "Account not found. Transfer cancelled.\n");
        break;
    default:
        send_message(fd, "Transfer failed due to a server error. "
                         "No money was moved. Please try again.\n");
    }
}

static void view_history(int fd, const Session *session) {
    Transaction *list = NULL;
    char msg[BUFFER_SIZE];

    int n = customer_get_history(session->user_id, &list);
    if (n < 0) {
        send_message(fd, "Could not retrieve transaction history. Please try again.\n");
        return;
    }
    if (n == 0) {
        send_message(fd, "No transactions found.\n");
        free(list);
        return;
    }

    int shown = (n < HISTORY_LIMIT) ? n : HISTORY_LIMIT;
    snprintf(msg, sizeof(msg),
             "\nLast %d of %d transaction(s), newest first:\n"
             "%-6s %-19s  %-12s %12s %12s\n"
             "-------------------------------------------------------------------\n",
             shown, n, "ID", "Date & Time", "Type", "Amount", "Balance");
    send_message(fd, msg);

    for (int i = n - 1; i >= n - shown; i--) {           /* newest first */
        char when[32], amt[32];
        struct tm tmv;
        time_t ts = (time_t)list[i].timestamp;
        localtime_r(&ts, &tmv);
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tmv);

        int debit = (strcmp(list[i].type, TXN_TYPE_WITHDRAW) == 0 ||
                     strcmp(list[i].type, TXN_TYPE_TRANSFER_OUT) == 0);
        snprintf(amt, sizeof(amt), "%c%.2f", debit ? '-' : '+', list[i].amount);

        snprintf(msg, sizeof(msg), "%-6d %-19s  %-12s %12s %12.2f\n",
                 list[i].id, when, list[i].type, amt, list[i].balance_after);
        send_message(fd, msg);
    }
    free(list);
}

static void do_feedback(int fd, const Session *session) {
    char buf[BUFFER_SIZE], msg[BUFFER_SIZE];

    if (prompt_input(fd, "Enter your feedback (max 200 characters): ", buf, sizeof(buf)) <= 0)
        return;                                          /* disconnected */

    size_t len = strlen(buf);

    /* empty or spaces-only */
    size_t i = 0;
    while (i < len && buf[i] == ' ') i++;
    if (i == len) {
        send_message(fd, "Feedback cannot be empty.\n");
        return;
    }

    if (len > FEEDBACK_MAX_LEN) {
        snprintf(msg, sizeof(msg),
                 "Feedback is too long (%zu characters). Maximum is %d.\n",
                 len, FEEDBACK_MAX_LEN);
        send_message(fd, msg);
        return;
    }

    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c < 32 || c == 127) {
            send_message(fd, "Feedback contains invalid characters.\n");
            return;
        }
    }

    if (customer_add_feedback(session->user_id, buf) == 0)
        send_message(fd, "Thank you! Your feedback has been submitted.\n");
    else
        send_message(fd, "Could not save your feedback. Please try again.\n");
}

static void do_change_password(int fd, const Session *session) {
    char old_pw[BUFFER_SIZE], new1[BUFFER_SIZE], new2[BUFFER_SIZE], msg[BUFFER_SIZE];

    if (prompt_input(fd, "Enter current password: ", old_pw, sizeof(old_pw)) <= 0) return;
    if (prompt_input(fd, "Enter new password: ", new1, sizeof(new1)) <= 0) return;
    if (prompt_input(fd, "Confirm new password: ", new2, sizeof(new2)) <= 0) return;

    if (strcmp(new1, new2) != 0) {
        send_message(fd, "Passwords do not match. Password not changed.\n");
        return;
    }

    switch (customer_change_password(session->user_id, old_pw, new1)) {
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

void customer_menu(int fd, Session *session) {
    char buf[BUFFER_SIZE], msg[BUFFER_SIZE];

    /* Role verification: refuse anyone who isn't a logged-in customer */
    if (!auth_require_role(session, ROLE_CUSTOMER)) {
        send_message(fd, "Access denied.\n");
        return;
    }

    snprintf(msg, sizeof(msg), "\nWelcome, %s!\n", session->username);
    send_message(fd, msg);

    while (1) {
        send_message(fd,
            "\n--- Customer Menu ---\n"
            "1. View Balance\n"
            "2. Deposit\n"
            "3. Withdraw\n"
            "4. Transfer Funds\n"
            "5. Apply for Loan\n"
            "6. Transaction History\n"
            "7. Add Feedback\n"
            "8. Change Password\n"
            "9. Logout\n");

        if (prompt_input(fd, "Enter choice: ", buf, sizeof(buf)) <= 0)
            return;                                      /* client disconnected */

        int choice = atoi(buf);
        switch (choice) {
        case 1:
            view_balance(fd, session);
            break;
        case 2:
            do_money_op(fd, session, 1);
            break;
        case 3:
            do_money_op(fd, session, 0);
            break;
        case 4:
            do_transfer(fd, session);
            break;
        case 6:
            view_history(fd, session);
            break;
        case 7:
            do_feedback(fd, session);
            break;
        case 8:
            do_change_password(fd, session);
            break;
        case 9:
            send_message(fd, "Logged out.\n");
            return;
        case 5:
            send_message(fd, "This feature will be implemented in the next step.\n");
            break;
        default:
            send_message(fd, "Invalid choice.\n");
        }
    }
}