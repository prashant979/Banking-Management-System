#ifndef CUSTOMER_H
#define CUSTOMER_H

#include <stdio.h>
#include "common.h"

typedef enum {
    TXN_OK = 0,
    TXN_NOT_FOUND,
    TXN_INACTIVE,
    TXN_INSUFFICIENT_FUNDS,
    TXN_RECIPIENT_INVALID,
    TXN_SAME_ACCOUNT,
    TXN_IO_ERROR
} TxnStatus;

typedef enum {
    PW_OK = 0,
    PW_WRONG_OLD,
    PW_INVALID_NEW,
    PW_SAME_AS_OLD,
    PW_ERROR
} PasswordStatus;

/* --- record store: fixed-width rows, one fcntl byte-range lock per customer --- */
int customer_validate_file(void);                      /* startup check; creates the file if missing */
int customer_open(void);                               /* fd (open once per request, keep it) or -1 */
int customer_count(int fd);                            /* number of customers, -1 on error */
int customer_lock(int fd, int idx, short type);        /* F_RDLCK / F_WRLCK on ONE record */
int customer_unlock(int fd, int idx);
int customer_read_record(int fd, int idx, Customer *c);
int customer_write_record(int fd, int idx, const Customer *c);   /* in place + fsync */
int customer_find_by_username(int fd, const char *username);     /* index, -1 not found, -2 error */
int customer_find_by_id(int fd, int id);                         /* same return values */

/* --- temporary compatibility layer: locks the WHOLE file range (all records) --- */
FILE *customer_open_locked(short lock_type);
void  customer_close_unlock(FILE *fp);
int   customer_read_all(FILE *fp, Customer **out);               /* count, -1 on error; caller frees */
int   customer_write_all(const Customer *arr, int n);            /* writes changed rows, undoes on failure */

/* --- operations --- */
int            customer_get_balance(int user_id, double *balance);
TxnStatus      customer_deposit(int user_id, double amount, double *new_balance);
TxnStatus      customer_withdraw(int user_id, double amount, double *new_balance);
TxnStatus      customer_transfer(int from_id, int to_id, double amount, double *new_balance);
int            customer_get_history(int user_id, Transaction **out);
int            customer_add_feedback(int user_id, const char *message);
PasswordStatus customer_change_password(int user_id, const char *old_pw, const char *new_pw);

void customer_menu(int fd, Session *session);

int customer_validate_logs(void); 
    /* startup check for transaction.csv and feedback.csv */

#endif