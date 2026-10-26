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

/* customer.csv is fixed-width: one customer = one 110-byte row = one lockable record */
#define CUSTOMER_FIELD_LEN  20      /* width of firstname, lastname, username, password */
#define RECORD_SIZE         110     /* bytes per row, including the newline */
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

/* transaction.csv and feedback.csv are append-only, fixed-width files: row 0 = header, row n at n * size */
#define TXN_RECORD_SIZE       83
#define FEEDBACK_RECORD_SIZE  230

#define TXN_STATUS_PENDING    'P'
#define TXN_STATUS_COMMITTED  'C'
#define TXN_STATUS_VOID       'V'

typedef struct {
    int    id;                 /* = row number in transaction.csv */
    int    customer_id;
    int    peer_id;            /* other account in a transfer, 0 otherwise */
    char   type[16];           /* DEPOSIT, WITHDRAW, TRANSFER_OUT, TRANSFER_IN */
    double amount;
    double balance_after;
    long   timestamp;          /* seconds since epoch */
    char   status;             /* P = pending, C = committed, V = void */
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

#define MIN_PASSWORD_LEN  6
#define MAX_PASSWORD_LEN  20

#define EMPLOYEE_FILE      "employee.csv"
#define MANAGER_FILE       "manager.csv"
#define ADMIN_FILE         "admin.csv"
#define STAFF_RECORD_SIZE  95        /* bytes per staff row, including the newline */

/* One layout for all staff roles; each role has its own file */
typedef struct {
    int  id;
    char firstname[50];
    char lastname[50];
    char username[50];
    char password[50];
    int  active;
    int  logged_in;
} Staff;

typedef Staff Employee;
typedef Staff Manager;
typedef Staff Admin;

#define CUSTOMER_ID_BASE  1000 
#define EMPLOYEE_ID_BASE  1000      /* staff id = base + row number in its file */
#define MANAGER_ID_BASE   1000
#define ADMIN_ID_BASE     1000     /* customer id = 1000 + row number in customer.csv */
typedef enum {
    PW_OK = 0,
    PW_WRONG_OLD,
    PW_INVALID_NEW,
    PW_SAME_AS_OLD,
    PW_ERROR
} PasswordStatus;

#define ROLE_HIST_FILE         "role_history.csv"
#define ROLE_HIST_RECORD_SIZE  70     /* bytes per history row, including the newline */

/* One role change. Status uses TXN_STATUS_PENDING / COMMITTED / VOID. */
typedef struct {
    int  id;                /* = row number in role_history.csv */
    char username[50];
    int  from_role, from_id;
    int  to_role, to_id;
    int  changed_by;        /* admin id */
    long timestamp;
    char status;
} RoleChange;

#endif
