#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include "common.h"
#include "utils.h"
#include "auth.h"
#include "customer.h"

#define HISTORY_LIMIT 20

/* =====================  customer.csv storage  ===================== */

/* The lock lives on a separate, never-replaced file, because customer.csv itself
   gets replaced by rename(). One request at a time per process, so a static is fine. */
static int g_lock_fd = -1;

FILE *customer_open_locked(short lock_type) {
    int lfd = open(CUSTOMER_LOCK_FILE, O_RDWR | O_CREAT, 0644);
    if (lfd < 0) return NULL;
    if (lock_file(lfd, lock_type) < 0) { close(lfd); return NULL; }

    int dfd = open(CUSTOMER_FILE, O_RDONLY | O_CREAT, 0644);
    FILE *fp = (dfd >= 0) ? fdopen(dfd, "r") : NULL;
    if (!fp) {
        if (dfd >= 0) close(dfd);
        unlock_file(lfd);
        close(lfd);
        return NULL;
    }
    g_lock_fd = lfd;
    return fp;
}

void customer_close_unlock(FILE *fp) {
    fclose(fp);
    if (g_lock_fd >= 0) {
        unlock_file(g_lock_fd);
        close(g_lock_fd);
        g_lock_fd = -1;
    }
}

/* Returns 0 for a valid record; header/blank/bad lines return -1 and are skipped. */
static int customer_parse_line(const char *line, Customer *c) {
    memset(c, 0, sizeof(*c));
    int n = sscanf(line, "%d,%49[^,],%49[^,],%49[^,],%49[^,],%lf,%d,%d",
                   &c->id, c->firstname, c->lastname, c->username, c->password,
                   &c->balance, &c->active, &c->logged_in);
    return (n == 8) ? 0 : -1;
}

int customer_read_all(FILE *fp, Customer **out) {
    int cap = 16, n = 0;
    Customer *arr = malloc(cap * sizeof(Customer));
    if (!arr) return -1;

    char line[512];
    rewind(fp);
    while (fgets(line, sizeof(line), fp)) {
        Customer c;
        if (customer_parse_line(line, &c) != 0) continue;
        if (n == cap) {
            cap *= 2;
            Customer *tmp = realloc(arr, cap * sizeof(Customer));
            if (!tmp) { free(arr); return -1; }
            arr = tmp;
        }
        arr[n++] = c;
    }
    *out = arr;
    return n;
}

/* All-or-nothing: write temp file, fsync, atomically rename over the real file.
   On any failure the old customer.csv is left untouched. */
int customer_write_all(const Customer *arr, int n) {
    const char *tmp_path = CUSTOMER_FILE ".tmp";

    FILE *out = fopen(tmp_path, "w");
    if (!out) return -1;

    fprintf(out, "%s\n", CUSTOMER_HEADER);
    for (int i = 0; i < n; i++) {
        fprintf(out, "%d,%s,%s,%s,%s,%.2f,%d,%d\n",
                arr[i].id, arr[i].firstname, arr[i].lastname,
                arr[i].username, arr[i].password,
                arr[i].balance, arr[i].active, arr[i].logged_in);
    }

    if (ferror(out) || fflush(out) != 0 || fsync(fileno(out)) != 0) {
        fclose(out);
        unlink(tmp_path);
        return -1;
    }
    if (fclose(out) != 0) { unlink(tmp_path); return -1; }

    if (rename(tmp_path, CUSTOMER_FILE) != 0) { unlink(tmp_path); return -1; }

    int dfd = open(".", O_RDONLY);          /* make the rename itself durable */
    if (dfd >= 0) { fsync(dfd); close(dfd); }
    return 0;
}

/* =====================  transaction.csv helpers  ===================== */

static void txn_close_unlock(FILE *fp) {
    fflush(fp);
    unlock_file(fileno(fp));
    fclose(fp);
}

/* Opens transaction.csv with an exclusive lock, writes the header if the file is new,
   and reports the current size so a failed transaction can be rolled back. */
static FILE *txn_open_locked(long *size_before) {
    int fd = open(TXN_FILE, O_RDWR | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return NULL;
    if (lock_file(fd, F_WRLCK) < 0) { close(fd); return NULL; }

    FILE *fp = fdopen(fd, "a+");
    if (!fp) { unlock_file(fd); close(fd); return NULL; }

    fseek(fp, 0, SEEK_END);
    if (ftell(fp) == 0) {                               /* brand-new file */
        fprintf(fp, "%s\n", TXN_HEADER);
        if (fflush(fp) != 0) { txn_close_unlock(fp); return NULL; }
    }
    fseek(fp, 0, SEEK_END);
    *size_before = ftell(fp);
    return fp;
}

static int txn_append(FILE *fp, Transaction *t) {
    t->id = get_next_id_csv(fp);
    t->timestamp = (long)time(NULL);

    fseek(fp, 0, SEEK_END);
    if (fprintf(fp, "%d,%d,%s,%.2f,%.2f,%ld\n",
                t->id, t->customer_id, t->type,
                t->amount, t->balance_after, t->timestamp) < 0) return -1;
    if (fflush(fp) != 0) return -1;
    if (fsync(fileno(fp)) != 0) return -1;
    return 0;
}

/* Undo an append: cut the file back to its size before the transaction. */
static void txn_rollback(FILE *fp, long size_before) {
    fflush(fp);
    if (ftruncate(fileno(fp), size_before) == 0) fsync(fileno(fp));
}

/* =====================  operations  ===================== */

static double round2(double x) {
    return (double)(long long)(x * 100.0 + 0.5) / 100.0;
}

static long long to_cents(double x) {
    return (long long)(x * 100.0 + 0.5);
}

/* Accepts only digits and at most one '.', > 0, <= MAX_TXN_AMOUNT, max 2 decimals. */
static int parse_amount(const char *s, double *out) {
    int digits = 0, dots = 0;
    for (const char *p = s; *p; p++) {
        if (isdigit((unsigned char)*p)) digits++;
        else if (*p == '.') dots++;
        else return -1;
    }
    if (digits == 0 || dots > 1) return -1;

    double v = strtod(s, NULL);
    if (v <= 0.0 || v > MAX_TXN_AMOUNT) return -1;

    long long cents = (long long)(v * 100.0 + 0.5);
    double diff = v * 100.0 - (double)cents;
    if (diff < 0) diff = -diff;
    if (diff > 1e-6) return -1;                         /* more than 2 decimals */

    *out = (double)cents / 100.0;
    return 0;
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
/* Reads the latest balance under a SHARED lock. */
int customer_get_balance(int user_id, double *balance) {
    FILE *fp = customer_open_locked(F_RDLCK);
    if (!fp) return -1;

    Customer *list = NULL;
    int n = customer_read_all(fp, &list);
    int rc = -1;
    if (n >= 0) {
        for (int i = 0; i < n; i++) {
            if (list[i].id == user_id) {
                *balance = list[i].balance;
                rc = 0;
                break;
            }
        }
        free(list);
    }
    customer_close_unlock(fp);
    return rc;
}

/* Returns this customer's transactions, oldest first, read under a SHARED lock.
   Returns the count (possibly 0), or -1 on error. The caller must free(*out). */
int customer_get_history(int user_id, Transaction **out) {
    int fd = open(TXN_FILE, O_RDONLY | O_CREAT, 0644);   /* create if no transaction happened yet */
    if (fd < 0) return -1;
    if (lock_file(fd, F_RDLCK) < 0) { close(fd); return -1; }

    FILE *fp = fdopen(fd, "r");
    if (!fp) { unlock_file(fd); close(fd); return -1; }

    int cap = 16, n = 0;
    Transaction *arr = malloc(cap * sizeof(Transaction));
    if (!arr) { unlock_file(fd); fclose(fp); return -1; }

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        Transaction t;
        memset(&t, 0, sizeof(t));
        if (sscanf(line, "%d,%d,%15[^,],%lf,%lf,%ld",
                   &t.id, &t.customer_id, t.type,
                   &t.amount, &t.balance_after, &t.timestamp) != 6)
            continue;                                    /* header or bad line */
        if (t.customer_id != user_id) continue;

        if (n == cap) {
            cap *= 2;
            Transaction *tmp = realloc(arr, cap * sizeof(Transaction));
            if (!tmp) { free(arr); unlock_file(fd); fclose(fp); return -1; }
            arr = tmp;
        }
        arr[n++] = t;
    }

    unlock_file(fd);
    fclose(fp);
    *out = arr;
    return n;
}

/* Appends one feedback row under an EXCLUSIVE lock, so picking the next ID and writing
   the row cannot interleave with another client. Returns 0 on success, -1 on failure
   (in which case the file is restored to its previous size). */
int customer_add_feedback(int user_id, const char *message) {
    int rc = -1;
    long size_before;
    Feedback f;

    int fd = open(FEEDBACK_FILE, O_RDWR | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return -1;
    if (lock_file(fd, F_WRLCK) < 0) { close(fd); return -1; }

    FILE *fp = fdopen(fd, "a+");
    if (!fp) { unlock_file(fd); close(fd); return -1; }

    fseek(fp, 0, SEEK_END);
    if (ftell(fp) == 0) {                                /* brand-new file: write header */
        fprintf(fp, "%s\n", FEEDBACK_HEADER);
        if (fflush(fp) != 0) goto done;
    }
    fseek(fp, 0, SEEK_END);
    size_before = ftell(fp);

    memset(&f, 0, sizeof(f));
    f.id          = get_next_id_csv(fp);
    f.customer_id = user_id;
    f.timestamp   = (long)time(NULL);
    strncpy(f.message, message, sizeof(f.message) - 1);

    fseek(fp, 0, SEEK_END);
    if (fprintf(fp, "%d,%d,%ld,%s\n", f.id, f.customer_id, f.timestamp, f.message) < 0 ||
        fflush(fp) != 0 ||
        fsync(fileno(fp)) != 0) {
        fflush(fp);
        if (ftruncate(fileno(fp), size_before) == 0) fsync(fileno(fp));   /* fallback */
        goto done;
    }
    rc = 0;

done:
    fflush(fp);
    unlock_file(fileno(fp));
    fclose(fp);
    return rc;
}

/* Shared by deposit and withdraw: log record + balance update, all or nothing.
   On success *new_balance is the balance after the transaction.
   On TXN_INSUFFICIENT_FUNDS *new_balance is the current (unchanged) balance. */
static TxnStatus apply_transaction(int user_id, double amount,
                                   const char *type, int is_debit,
                                   double *new_balance) {
    TxnStatus status = TXN_IO_ERROR;
    Customer *list = NULL;
    FILE *txn = NULL;
    long txn_size = 0;
    int n, idx = -1;
    double updated;
    Transaction t;

    FILE *fp = customer_open_locked(F_WRLCK);           /* serialises every balance change */
    if (!fp) return TXN_IO_ERROR;

    n = customer_read_all(fp, &list);
    if (n < 0) goto done;

    for (int i = 0; i < n; i++)
        if (list[i].id == user_id) { idx = i; break; }
    if (idx < 0)           { status = TXN_NOT_FOUND; goto done; }
    if (!list[idx].active) { status = TXN_INACTIVE;  goto done; }

    /* Check and debit under the SAME lock: no other process can spend this money in between */
    if (is_debit && to_cents(list[idx].balance) < to_cents(amount)) {
        *new_balance = list[idx].balance;
        status = TXN_INSUFFICIENT_FUNDS;
        goto done;
    }

    updated = round2(is_debit ? list[idx].balance - amount
                              : list[idx].balance + amount);

    txn = txn_open_locked(&txn_size);                   /* lock order: customer, then txn */
    if (!txn) goto done;

    memset(&t, 0, sizeof(t));
    t.customer_id   = user_id;
    t.amount        = amount;
    t.balance_after = updated;
    strncpy(t.type, type, sizeof(t.type) - 1);

    if (txn_append(txn, &t) != 0) {                     /* step 1: write the log */
        txn_rollback(txn, txn_size);
        goto done;
    }

    list[idx].balance = updated;
    if (customer_write_all(list, n) != 0) {             /* step 2: commit the balance */
        txn_rollback(txn, txn_size);                    /* fallback: remove the log record */
        goto done;
    }

    *new_balance = updated;
    status = TXN_OK;

done:
    if (txn) txn_close_unlock(txn);
    free(list);
    customer_close_unlock(fp);
    return status;
}

TxnStatus customer_deposit(int user_id, double amount, double *new_balance) {
    return apply_transaction(user_id, amount, TXN_TYPE_DEPOSIT, 0, new_balance);
}

TxnStatus customer_withdraw(int user_id, double amount, double *new_balance) {
    return apply_transaction(user_id, amount, TXN_TYPE_WITHDRAW, 1, new_balance);
}

/* Debit sender, credit recipient, two log rows, one atomic commit.
   On success *new_balance is the SENDER's balance afterwards.
   On TXN_INSUFFICIENT_FUNDS *new_balance is the sender's current balance. */
TxnStatus customer_transfer(int from_id, int to_id, double amount, double *new_balance) {
    TxnStatus status = TXN_IO_ERROR;
    Customer *list = NULL;
    FILE *txn = NULL;
    long txn_size = 0;
    int n, from = -1, to = -1;
    double from_after, to_after;
    Transaction t;

    if (from_id == to_id) return TXN_SAME_ACCOUNT;

    FILE *fp = customer_open_locked(F_WRLCK);           /* one lock covers BOTH accounts */
    if (!fp) return TXN_IO_ERROR;

    n = customer_read_all(fp, &list);
    if (n < 0) goto done;

    for (int i = 0; i < n; i++) {
        if (list[i].id == from_id)    from = i;
        else if (list[i].id == to_id) to = i;
    }
    if (from < 0)           { status = TXN_NOT_FOUND;         goto done; }
    if (!list[from].active) { status = TXN_INACTIVE;          goto done; }
    if (to < 0 || !list[to].active) { status = TXN_RECIPIENT_INVALID; goto done; }

    if (to_cents(list[from].balance) < to_cents(amount)) {
        *new_balance = list[from].balance;
        status = TXN_INSUFFICIENT_FUNDS;
        goto done;
    }

    from_after = round2(list[from].balance - amount);
    to_after   = round2(list[to].balance + amount);

    txn = txn_open_locked(&txn_size);
    if (!txn) goto done;

    /* log row 1: sender */
    memset(&t, 0, sizeof(t));
    t.customer_id   = from_id;
    t.amount        = amount;
    t.balance_after = from_after;
    strncpy(t.type, TXN_TYPE_TRANSFER_OUT, sizeof(t.type) - 1);
    if (txn_append(txn, &t) != 0) { txn_rollback(txn, txn_size); goto done; }

    /* log row 2: recipient */
    memset(&t, 0, sizeof(t));
    t.customer_id   = to_id;
    t.amount        = amount;
    t.balance_after = to_after;
    strncpy(t.type, TXN_TYPE_TRANSFER_IN, sizeof(t.type) - 1);
    if (txn_append(txn, &t) != 0) { txn_rollback(txn, txn_size); goto done; }

    /* commit: both balances change in ONE atomic file replacement */
    list[from].balance = from_after;
    list[to].balance   = to_after;
    if (customer_write_all(list, n) != 0) {
        txn_rollback(txn, txn_size);                    /* removes BOTH log rows */
        goto done;
    }

    *new_balance = from_after;
    status = TXN_OK;

done:
    if (txn) txn_close_unlock(txn);
    free(list);
    customer_close_unlock(fp);
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
        case 9:
            send_message(fd, "Logged out.\n");
            return;
        case 5: case 8:
            send_message(fd, "This feature will be implemented in the next step.\n");
            break;
        default:
            send_message(fd, "Invalid choice.\n");
        }
    }
}