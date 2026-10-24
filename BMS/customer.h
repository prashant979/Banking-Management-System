#ifndef CUSTOMER_H
#define CUSTOMER_H

#include <stdio.h>
#include "common.h"

typedef enum {
    TXN_OK = 0,
    TXN_NOT_FOUND,
    TXN_INACTIVE,
    TXN_INSUFFICIENT_FUNDS,
    TXN_RECIPIENT_INVALID,      /* recipient does not exist or is deactivated */
    TXN_SAME_ACCOUNT,
    TXN_IO_ERROR
} TxnStatus;

/* --- customer.csv storage helpers --- */
FILE *customer_open_locked(short lock_type);        /* F_RDLCK / F_WRLCK on customer.csv.lock */
void  customer_close_unlock(FILE *fp);
int   customer_read_all(FILE *fp, Customer **out);  /* count, -1 on error; caller frees */
int   customer_write_all(const Customer *arr, int n); /* atomic: temp file + fsync + rename */

/* --- operations --- */
int       customer_get_balance(int user_id, double *balance);
TxnStatus customer_deposit(int user_id, double amount, double *new_balance);
TxnStatus customer_withdraw(int user_id, double amount, double *new_balance);
TxnStatus customer_transfer(int from_id, int to_id, double amount, double *new_balance);
int customer_get_history(int user_id, Transaction **out);   /* count (oldest first), -1 on error; caller frees */
int customer_add_feedback(int user_id, const char *message);   /* 0 ok, -1 error */

void  customer_menu(int fd, Session *session);

#endif