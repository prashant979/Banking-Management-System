#ifndef LOAN_H
#define LOAN_H

#include "common.h"

typedef enum {
    LN_OK = 0,
    LN_BAD_AMOUNT,
    LN_BAD_TERM,
    LN_BAD_PURPOSE,
    LN_CUSTOMER_INACTIVE,
    LN_OPEN_LOAN_EXISTS,
    LN_NOT_FOUND,
    LN_EMPLOYEE_INVALID,     /* no such employee, or deactivated */
    LN_SAME_EMPLOYEE,        /* already assigned to that employee */
    LN_STALE,                /* loan changed since the manager viewed it */
    LN_DECIDED,              /* already approved or rejected */
    LN_NOT_YOURS,            /* not assigned to this employee (any more) */
    LN_IO_ERROR
} LoanResult;

int         loan_validate_file(void);
const char *loan_status_name(char status);

/* Customer. On LN_OPEN_LOAN_EXISTS, *loan_id is the open loan. */
LoanResult  loan_apply(int customer_id, double amount, int term_months,
                       const char *purpose, int *loan_id);

/* Read one loan by id (shared lock on that row). 0 ok, -1 not found / error. */
int         loan_get(int loan_id, Loan *out);

/* Manager. expected_employee_id = the employee the manager was shown (0 for a pending loan). */
LoanResult  loan_assign(int loan_id, int employee_id, int manager_id, int expected_employee_id);

/* Employee. Approve credits the amount to the customer (ACID across three files). */
LoanResult  loan_approve(int loan_id, int employee_id, double *new_balance);
LoanResult  loan_reject(int loan_id, int employee_id);

/* 1 if the employee has loans in Assigned state, 0 if none, -1 on error. */
int         loan_employee_has_assigned(int employee_id);

int  loan_page(int customer_id, int employee_id, const char *statuses,
               long start_row, Loan *out, int max, long *next_row);
void loan_browser(int fd, int customer_id, int employee_id, const char *statuses, const char *title);

int loan_revert_approval(int loan_id, int customer_id);  /* recovery only */
#endif