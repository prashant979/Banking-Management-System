#ifndef COMMON_H
#define COMMON_H

#define SERVER_PORT   8080
#define BACKLOG       10
#define BUFFER_SIZE   1024

/* Roles (same numbers as the login menu) */
#define ROLE_NONE      0
#define ROLE_CUSTOMER  1
#define ROLE_EMPLOYEE  2
#define ROLE_MANAGER   3
#define ROLE_ADMIN     4

#define CUSTOMER_FILE    "customer.csv"
#define CUSTOMER_HEADER  "id,firstname,lastname,username,password,balance,active,logged_in"

/* Offsets/size assume a 64-bit system (double aligned to 8 bytes) */
typedef struct {
    int    id;              /* offset   0 */
    char   firstname[50];   /* offset   4 */
    char   lastname[50];    /* offset  54 */
    char   username[50];    /* offset 104 */
    char   password[50];    /* offset 154, ends at 203 */
                            /* 4 bytes padding so double lands on 8-byte boundary */
    double balance;         /* offset 208 */
    int    active;          /* offset 216: 1 = active, 0 = deactivated */
    int    logged_in;       /* offset 220: 1 = has a live session */
} Customer;                 /* sizeof = 224 */

/* Who is logged in for this client connection (lives inside the child process) */
typedef struct {
    int  role;              /* ROLE_* */
    int  user_id;
    char username[50];
    int  logged_in;
} Session;

#define CUSTOMER_LOCK_FILE "customer.csv.lock"
#define TXN_FILE           "transaction.csv"
#define TXN_HEADER         "txn_id,customer_id,type,amount,balance_after,timestamp"
#define MAX_TXN_AMOUNT     1000000.00

#define TXN_TYPE_DEPOSIT   "DEPOSIT"
#define TXN_TYPE_WITHDRAW  "WITHDRAW"
#define TXN_TYPE_TRANSFER_OUT "TRANSFER_OUT"
#define TXN_TYPE_TRANSFER_IN  "TRANSFER_IN"

typedef struct {
    int    id;
    int    customer_id;
    char   type[16];        /* DEPOSIT, WITHDRAW, ... */
    double amount;
    double balance_after;
    long   timestamp;       /* seconds since epoch */
} Transaction;

#define FEEDBACK_FILE     "feedback.csv"
#define FEEDBACK_HEADER   "id,customer_id,timestamp,message"
#define FEEDBACK_MAX_LEN  200

typedef struct {
    int  id;
    int  customer_id;
    long timestamp;                       /* seconds since epoch */
    char message[FEEDBACK_MAX_LEN + 1];
} Feedback;



#endif
