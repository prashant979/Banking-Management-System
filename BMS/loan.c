#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <ctype.h>
#include <time.h>
#include "common.h"
#include "utils.h"
#include "customer.h"
#include "loan.h"
#include "staff.h"

#define LOAN_ROW_FMT   "%-8d,%-6d,%14.2f,%-3d,%-30s,%c,%-6d,%-6d,%-12ld,%-12ld,%-12ld\n"
#define LOAN_PAGE_SIZE 10

static void trim_right(char *s) {
    size_t len = strlen(s);
    while (len > 0 && s[len - 1] == ' ') s[--len] = '\0';
}

static int valid_status(char c) {
    return c == LOAN_STATUS_PENDING || c == LOAN_STATUS_ASSIGNED ||
           c == LOAN_STATUS_APPROVED || c == LOAN_STATUS_REJECTED;
}

const char *loan_status_name(char status) {
    switch (status) {
    case LOAN_STATUS_PENDING:  return "Pending";
    case LOAN_STATUS_ASSIGNED: return "Assigned";
    case LOAN_STATUS_APPROVED: return "Approved";
    case LOAN_STATUS_REJECTED: return "Rejected";
    default:                   return "Unknown";
    }
}

static int loan_format(const Loan *l, char *out) {
    if (strlen(l->purpose) > LOAN_PURPOSE_LEN || strchr(l->purpose, ',') != NULL) return -1;
    if (!valid_status(l->status)) return -1;
    int n = snprintf(out, LOAN_RECORD_SIZE + 1, LOAN_ROW_FMT,
                     l->id, l->customer_id, l->amount, l->term_months, l->purpose,
                     l->status, l->employee_id, l->assigned_by,
                     l->applied_at, l->assigned_at, l->decided_at);
    return (n == LOAN_RECORD_SIZE) ? 0 : -1;
}

static int loan_parse(const char *row, Loan *l) {
    char buf[LOAN_RECORD_SIZE + 1], status = 0;
    memcpy(buf, row, LOAN_RECORD_SIZE);
    buf[LOAN_RECORD_SIZE] = '\0';
    if (buf[LOAN_RECORD_SIZE - 1] != '\n') return -1;

    memset(l, 0, sizeof(*l));
    int n = sscanf(buf, "%d ,%d ,%lf,%d ,%30[^,],%c,%d ,%d ,%ld ,%ld ,%ld",
                   &l->id, &l->customer_id, &l->amount, &l->term_months, l->purpose,
                   &status, &l->employee_id, &l->assigned_by,
                   &l->applied_at, &l->assigned_at, &l->decided_at);
    if (n != 11 || !valid_status(status)) return -1;      /* header, blank or damaged row */
    l->status = status;
    trim_right(l->purpose);
    return 0;
}

int loan_validate_file(void) {
    char hdr[LOAN_RECORD_SIZE + 1];
    snprintf(hdr, sizeof(hdr), "%-8s,%-6s,%14s,%-3s,%-30s,%s,%-6s,%-6s,%-12s,%-12s,%-12s\n",
             "loan_id", "cust", "amount", "trm", "purpose", "s", "emp", "by",
             "applied_at", "assigned_at", "decided_at");
    return log_validate_file(LOAN_FILE, LOAN_RECORD_SIZE, hdr);
}

/* 1-30 chars: letters, digits, space, '-', '.'; no leading/trailing space; at least one letter or digit */
static int valid_purpose(const char *s) {
    size_t len = strlen(s);
    int alnum = 0;
    if (len < 1 || len > LOAN_PURPOSE_LEN) return 0;
    if (s[0] == ' ' || s[len - 1] == ' ') return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c)) alnum = 1;
        else if (c != ' ' && c != '-' && c != '.') return 0;
    }
    return alnum;
}

/* Lock order: customer row (EXCLUSIVE, held throughout) -> loan rows.
   Holding the customer's row makes "no open loan? then append" one atomic step for that
   customer, while other customers apply in parallel. */
LoanResult loan_apply(int customer_id, double amount, int term_months,
                      const char *purpose, int *loan_id) {
    LoanResult res = LN_IO_ERROR;
    Loan l, open_loan;
    char row[LOAN_RECORD_SIZE + 1];
    int idx, locked = 0, lfd = -1;
    long next = 0, n;

    if (amount <= 0.0 || amount > MAX_TXN_AMOUNT)        return LN_BAD_AMOUNT;
    if (term_months < 1 || term_months > LOAN_MAX_TERM)  return LN_BAD_TERM;
    if (!valid_purpose(purpose))                         return LN_BAD_PURPOSE;

    int cfd = customer_open();
    if (cfd < 0) return LN_IO_ERROR;

    idx = customer_find_by_id(cfd, customer_id);          /* lock-free locate */
    if (idx == -2) goto done;
    if (idx < 0) { res = LN_NOT_FOUND; goto done; }

    /* 1. exclusive lock on this customer's row, held until the new loan is written */
    if (customer_lock(cfd, idx, F_WRLCK) < 0) goto done;
    locked = 1;
    {
        Customer c;
        if (customer_read_record(cfd, idx, &c) != 0 || c.id != customer_id) goto done;
        if (!c.active) { res = LN_CUSTOMER_INACTIVE; goto done; }
    }

    /* 2. any open loan (Pending or Assigned)? */
    int found = loan_page(customer_id, 0, "PA", 1, &open_loan, 1, &next);
    if (found < 0) goto done;
    if (found > 0) {
        *loan_id = open_loan.id;
        res = LN_OPEN_LOAN_EXISTS;
        goto done;
    }

    /* 3. append: guard only while reserving, then only our own row */
    lfd = open(LOAN_FILE, O_RDWR);
    if (lfd < 0) goto done;
    n = log_reserve(lfd, LOAN_RECORD_SIZE);
    if (n < 0) goto done;

    memset(&l, 0, sizeof(l));
    l.id          = (int)n;
    l.customer_id = customer_id;
    l.amount      = amount;
    l.term_months = term_months;
    l.status      = LOAN_STATUS_PENDING;
    l.applied_at  = (long)time(NULL);
    strncpy(l.purpose, purpose, sizeof(l.purpose) - 1);

    if (loan_format(&l, row) == 0 && log_write_row(lfd, n, LOAN_RECORD_SIZE, row) == 0) {
        *loan_id = l.id;
        res = LN_OK;
    }
    log_release_row(lfd, n, LOAN_RECORD_SIZE);            /* on failure a blank row stays; readers skip it */

done:
    if (lfd >= 0) close(lfd);
    if (locked) customer_unlock(cfd, idx);                /* release in reverse order */
    close(cfd);
    return res;
}

/* Each row read under a SHARED lock on that row only. */
int loan_page(int customer_id, int employee_id, const char *statuses,
              long start_row, Loan *out, int max, long *next_row) {
    int n = 0;
    *next_row = 0;
    if (start_row < 1) start_row = 1;

    int fd = open(LOAN_FILE, O_RDONLY);
    if (fd < 0) return -1;

    long rows = log_row_count(fd, LOAN_RECORD_SIZE);
    if (rows < 0) { close(fd); return -1; }

    for (long r = start_row; r <= rows; r++) {
        off_t off = (off_t)r * LOAN_RECORD_SIZE;
        char row[LOAN_RECORD_SIZE];
        Loan l;

        if (lock_record(fd, F_RDLCK, off, LOAN_RECORD_SIZE) < 0) continue;
        ssize_t got = pread(fd, row, LOAN_RECORD_SIZE, off);
        unlock_record(fd, off, LOAN_RECORD_SIZE);

        if (got != LOAN_RECORD_SIZE || loan_parse(row, &l) != 0) continue;
        if (customer_id && l.customer_id != customer_id) continue;
        if (employee_id && l.employee_id != employee_id) continue;
        if (statuses && strchr(statuses, l.status) == NULL) continue;

        if (n == max) { *next_row = r; break; }           /* one more match: a next page exists */
        out[n++] = l;
    }
    close(fd);
    return n;
}

static void fmt_time(long t, char *out, size_t size) {
    struct tm tmv;
    time_t ts = (time_t)t;
    localtime_r(&ts, &tmv);
    strftime(out, size, "%Y-%m-%d %H:%M", &tmv);
}

void loan_browser(int fd, int customer_id, int employee_id, const char *statuses, const char *title) {
    Loan page[LOAN_PAGE_SIZE];
    char buf[BUFFER_SIZE], msg[BUFFER_SIZE];
    int cap = 16, depth = 0, done = 0;
    long *starts = malloc(cap * sizeof(long));            /* starts[d] = first row of page d */

    if (!starts) { send_message(fd, "Server error. Please try again later.\n"); return; }
    starts[0] = 1;

    while (!done) {
        long next = 0;
        int n = loan_page(customer_id, employee_id, statuses, starts[depth],
                          page, LOAN_PAGE_SIZE, &next);
        if (n < 0) { send_message(fd, "Could not read the loans. Please try again.\n"); break; }
        if (n == 0 && depth == 0) {
            snprintf(msg, sizeof(msg), "%s: no loans to show.\n", title);
            send_message(fd, msg);
            break;
        }

        snprintf(msg, sizeof(msg), "\n--- %s: page %d ---\n", title, depth + 1);
        send_message(fd, msg);

        for (int i = 0; i < n; i++) {
            char applied[32], extra[96] = "";
            fmt_time(page[i].applied_at, applied, sizeof(applied));

            if (page[i].status == LOAN_STATUS_ASSIGNED) {
                snprintf(extra, sizeof(extra), " (employee %d)", page[i].employee_id);
            } else if (page[i].status == LOAN_STATUS_APPROVED ||
                       page[i].status == LOAN_STATUS_REJECTED) {
                char decided[32];
                fmt_time(page[i].decided_at, decided, sizeof(decided));
                snprintf(extra, sizeof(extra), " by employee %d on %s", page[i].employee_id, decided);
            }

            snprintf(msg, sizeof(msg),
                     "Loan #%d  Customer %d  Applied %s\n"
                     "   Amount %.2f  Term %d months  Purpose: %s\n"
                     "   Status: %s%s\n",
                     page[i].id, page[i].customer_id, applied,
                     page[i].amount, page[i].term_months, page[i].purpose,
                     loan_status_name(page[i].status), extra);
            send_message(fd, msg);
        }

        for (;;) {
            if (prompt_input(fd, "\n1. Next page   2. Previous page   3. Exit\nEnter choice: ",
                             buf, sizeof(buf)) <= 0) { done = 1; break; }
            int choice = atoi(buf);
            if (choice == 1) {
                if (next > 0) {
                    if (depth + 1 >= cap) {
                        cap *= 2;
                        long *tmp = realloc(starts, cap * sizeof(long));
                        if (!tmp) { done = 1; break; }
                        starts = tmp;
                    }
                    starts[++depth] = next;
                    break;
                }
                send_message(fd, "This is the last page.\n");
            } else if (choice == 2) {
                if (depth > 0) { depth--; break; }
                send_message(fd, "This is the first page.\n");
            } else if (choice == 3) {
                done = 1;
                break;
            } else {
                send_message(fd, "Invalid choice.\n");
            }
        }
    }
    free(starts);
}
/* ---------- direct row access: loan id = row number ---------- */

static off_t loan_offset(int id) {
    return (off_t)id * LOAN_RECORD_SIZE;
}

static int loan_exists(int fd, int id) {
    long rows = log_row_count(fd, LOAN_RECORD_SIZE);
    return rows >= 0 && id >= 1 && id <= rows;
}

static int loan_read_at(int fd, int id, Loan *l) {
    char row[LOAN_RECORD_SIZE];
    if (pread(fd, row, LOAN_RECORD_SIZE, loan_offset(id)) != LOAN_RECORD_SIZE) return -1;
    if (loan_parse(row, l) != 0 || l->id != id) return -1;     /* blank (failed) row counts as missing */
    return 0;
}

/* Caller holds an exclusive lock on this loan's row. */
static int loan_write_at(int fd, const Loan *l) {
    char row[LOAN_RECORD_SIZE + 1];
    if (loan_format(l, row) != 0) return -1;
    if (pwrite(fd, row, LOAN_RECORD_SIZE, loan_offset(l->id)) != LOAN_RECORD_SIZE) return -1;
    return (fsync(fd) == 0) ? 0 : -1;
}

static double loan_round2(double x) {
    return (double)(long long)(x * 100.0 + 0.5) / 100.0;
}

int loan_get(int loan_id, Loan *out) {
    int rc = -1;
    int fd = open(LOAN_FILE, O_RDONLY);
    if (fd < 0) return -1;
    if (loan_exists(fd, loan_id) &&
        lock_record(fd, F_RDLCK, loan_offset(loan_id), LOAN_RECORD_SIZE) == 0) {
        rc = loan_read_at(fd, loan_id, out);
        unlock_record(fd, loan_offset(loan_id), LOAN_RECORD_SIZE);
    }
    close(fd);
    return rc;
}

int loan_employee_has_assigned(int employee_id) {
    Loan tmp;
    long next;
    int n = loan_page(0, employee_id, "A", 1, &tmp, 1, &next);
    return (n < 0) ? -1 : (n > 0);
}

/* ---------- Manager: assign / reassign ---------- */

/* Lock order: employee row (SHARED, so they can't be deactivated or moved meanwhile)
   -> loan row (EXCLUSIVE). The stale check compares the loan's current employee with what
   the manager was shown, so two managers can't silently overwrite each other. */
LoanResult loan_assign(int loan_id, int employee_id, int manager_id, int expected_employee_id) {
    LoanResult res = LN_IO_ERROR;
    int sfd = -1, sidx = -1, slocked = 0, lfd = -1, llocked = 0;
    Staff e;
    Loan l, before;

    sfd = staff_open(ROLE_EMPLOYEE);
    if (sfd < 0) goto done;
    sidx = staff_find_by_id(sfd, employee_id);
    if (sidx == -2) goto done;
    if (sidx < 0) { res = LN_EMPLOYEE_INVALID; goto done; }
    if (staff_lock(sfd, sidx, F_RDLCK) < 0) goto done;
    slocked = 1;
    if (staff_read_record(sfd, sidx, &e) != 0 || e.id != employee_id) goto done;
    if (!e.active) { res = LN_EMPLOYEE_INVALID; goto done; }

    lfd = open(LOAN_FILE, O_RDWR);
    if (lfd < 0) goto done;
    if (!loan_exists(lfd, loan_id)) { res = LN_NOT_FOUND; goto done; }
    if (lock_record(lfd, F_WRLCK, loan_offset(loan_id), LOAN_RECORD_SIZE) < 0) goto done;
    llocked = 1;
    if (loan_read_at(lfd, loan_id, &l) != 0) { res = LN_NOT_FOUND; goto done; }

    if (l.status == LOAN_STATUS_APPROVED || l.status == LOAN_STATUS_REJECTED) {
        res = LN_DECIDED; goto done;
    }
    if (l.employee_id != expected_employee_id) { res = LN_STALE; goto done; }
    if (l.status == LOAN_STATUS_ASSIGNED && l.employee_id == employee_id) {
        res = LN_SAME_EMPLOYEE; goto done;
    }

    before = l;                                            /* before-image for undo */
    l.status      = LOAN_STATUS_ASSIGNED;
    l.employee_id = employee_id;
    l.assigned_by = manager_id;
    l.assigned_at = (long)time(NULL);
    if (loan_write_at(lfd, &l) != 0) {
        loan_write_at(lfd, &before);
        goto done;
    }
    res = LN_OK;

done:
    if (llocked) unlock_record(lfd, loan_offset(loan_id), LOAN_RECORD_SIZE);   /* reverse order */
    if (lfd >= 0) close(lfd);
    if (slocked) staff_unlock(sfd, sidx);
    if (sfd >= 0) close(sfd);
    return res;
}

/* ---------- Employee: reject ---------- */

LoanResult loan_reject(int loan_id, int employee_id) {
    LoanResult res = LN_IO_ERROR;
    int lfd, llocked = 0;
    Loan l, before;

    lfd = open(LOAN_FILE, O_RDWR);
    if (lfd < 0) return LN_IO_ERROR;
    if (!loan_exists(lfd, loan_id)) { res = LN_NOT_FOUND; goto done; }
    if (lock_record(lfd, F_WRLCK, loan_offset(loan_id), LOAN_RECORD_SIZE) < 0) goto done;
    llocked = 1;
    if (loan_read_at(lfd, loan_id, &l) != 0) { res = LN_NOT_FOUND; goto done; }

    if (l.status == LOAN_STATUS_APPROVED || l.status == LOAN_STATUS_REJECTED) {
        res = LN_DECIDED; goto done;
    }
    if (l.status != LOAN_STATUS_ASSIGNED || l.employee_id != employee_id) {
        res = LN_NOT_YOURS; goto done;
    }

    before = l;
    l.status     = LOAN_STATUS_REJECTED;
    l.decided_at = (long)time(NULL);
    if (loan_write_at(lfd, &l) != 0) {
        loan_write_at(lfd, &before);
        goto done;
    }
    res = LN_OK;

done:
    if (llocked) unlock_record(lfd, loan_offset(loan_id), LOAN_RECORD_SIZE);
    close(lfd);
    return res;
}

/* ---------- Employee: approve = credit money, ACID across three files ---------- */

/* Lock order: customer row -> loan row -> transaction log row.
   Write-ahead: PENDING LOAN_CREDIT -> credit balance -> loan APPROVED -> COMMITTED.
   Any failure restores both rows from before-images and marks the transaction VOID. */
LoanResult loan_approve(int loan_id, int employee_id, double *new_balance) {
    LoanResult res = LN_IO_ERROR;
    int cfd = -1, cidx = -1, clocked = 0, lfd = -1, llocked = 0, tfd = -1;
    long trow = -1;
    double updated;
    Loan peek, l, lbefore;
    Customer c, cbefore;

    /* 0. lock-free peek to learn which customer (a hint; re-checked under the locks) */
    if (loan_get(loan_id, &peek) != 0) return LN_NOT_FOUND;

    /* 1. customer row EXCLUSIVE */
    cfd = customer_open();
    if (cfd < 0) goto done;
    cidx = customer_find_by_id(cfd, peek.customer_id);
    if (cidx == -2) goto done;
    if (cidx < 0) { res = LN_NOT_FOUND; goto done; }
    if (customer_lock(cfd, cidx, F_WRLCK) < 0) goto done;
    clocked = 1;
    if (customer_read_record(cfd, cidx, &c) != 0 || c.id != peek.customer_id) goto done;

    /* 2. loan row EXCLUSIVE, then re-check everything */
    lfd = open(LOAN_FILE, O_RDWR);
    if (lfd < 0) goto done;
    if (!loan_exists(lfd, loan_id)) { res = LN_NOT_FOUND; goto done; }
    if (lock_record(lfd, F_WRLCK, loan_offset(loan_id), LOAN_RECORD_SIZE) < 0) goto done;
    llocked = 1;
    if (loan_read_at(lfd, loan_id, &l) != 0) { res = LN_NOT_FOUND; goto done; }
    if (l.customer_id != c.id) goto done;                 /* never happens: customer_id is fixed */

    if (l.status == LOAN_STATUS_APPROVED || l.status == LOAN_STATUS_REJECTED) {
        res = LN_DECIDED; goto done;
    }
    if (l.status != LOAN_STATUS_ASSIGNED || l.employee_id != employee_id) {
        res = LN_NOT_YOURS; goto done;
    }
    if (!c.active) { res = LN_CUSTOMER_INACTIVE; goto done; }

    cbefore = c;                                          /* before-images for undo */
    lbefore = l;
    updated = loan_round2(c.balance + l.amount);

    /* 3. write-ahead: durable PENDING LOAN_CREDIT row (peer column = loan id) */
    tfd = txn_open();
    if (tfd < 0) goto done;
    trow = txn_begin(tfd, c.id, l.id, TXN_TYPE_LOAN_CREDIT, l.amount, updated);
    if (trow < 0) goto done;

    /* 4. credit the customer */
    c.balance = updated;
    if (customer_write_record(cfd, cidx, &c) != 0) {
        customer_write_record(cfd, cidx, &cbefore);
        goto void_txn;
    }

    /* 5. mark the loan approved */
    l.status     = LOAN_STATUS_APPROVED;
    l.decided_at = (long)time(NULL);
    if (loan_write_at(lfd, &l) != 0) {
        loan_write_at(lfd, &lbefore);
        goto undo_balance;
    }

    /* 6. commit */
    if (txn_finish(tfd, trow, TXN_STATUS_COMMITTED) != 0) {
        loan_write_at(lfd, &lbefore);
        goto undo_balance;
    }

    *new_balance = updated;
    res = LN_OK;
    goto done;

undo_balance:
    customer_write_record(cfd, cidx, &cbefore);
void_txn:
    txn_finish(tfd, trow, TXN_STATUS_VOID);
done:
    if (tfd >= 0) close(tfd);                             /* release in reverse order */
    if (llocked) unlock_record(lfd, loan_offset(loan_id), LOAN_RECORD_SIZE);
    if (lfd >= 0) close(lfd);
    if (clocked) customer_unlock(cfd, cidx);
    if (cfd >= 0) close(cfd);
    return res;
}