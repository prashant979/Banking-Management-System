#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "common.h"
#include "utils.h"
#include "auth.h"
#include "customer.h"
#include "manager.h"
#include "staff.h"
#include "loan.h"

#define FEEDBACK_PAGE_SIZE 20

/* Shows feedback 20 records per page with Next / Previous / Exit.
   customer_id 0 = everyone's feedback, otherwise only that customer's. */
static void feedback_browser(int fd, int customer_id) {
    Feedback page[FEEDBACK_PAGE_SIZE];
    char buf[BUFFER_SIZE], msg[BUFFER_SIZE];
    int cap = 16, depth = 0, done = 0;
    long *starts = malloc(cap * sizeof(long));      /* starts[d] = first row of page d */

    if (!starts) {
        send_message(fd, "Server error. Please try again later.\n");
        return;
    }
    starts[0] = 1;

    while (!done) {
        long next = 0;
        int n = customer_feedback_page(customer_id, starts[depth], page,
                                       FEEDBACK_PAGE_SIZE, &next);
        if (n < 0) {
            send_message(fd, "Could not read the feedback. Please try again.\n");
            break;
        }
        if (n == 0 && depth == 0) {
            send_message(fd, customer_id ? "No feedback found for this customer.\n"
                                         : "No feedback has been submitted yet.\n");
            break;
        }

        snprintf(msg, sizeof(msg), "\n--- Feedback: page %d (records %d-%d) ---\n",
                 depth + 1, depth * FEEDBACK_PAGE_SIZE + 1, depth * FEEDBACK_PAGE_SIZE + n);
        send_message(fd, msg);

        for (int i = 0; i < n; i++) {
            char when[32];
            struct tm tmv;
            time_t ts = (time_t)page[i].timestamp;
            localtime_r(&ts, &tmv);
            strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tmv);

            snprintf(msg, sizeof(msg), "%3d. Customer %d  %s\n     %s\n",
                     depth * FEEDBACK_PAGE_SIZE + i + 1, page[i].customer_id, when, page[i].message);
            send_message(fd, msg);
        }

        for (;;) {                                   /* choices for this page */
            if (prompt_input(fd, "\n1. Next page   2. Previous page   3. Exit\nEnter choice: ",
                             buf, sizeof(buf)) <= 0) {
                done = 1;                            /* client disconnected */
                break;
            }
            int choice = atoi(buf);

            if (choice == 1) {
                if (next > 0) {
                    if (depth + 1 >= cap) {
                        cap *= 2;
                        long *tmp = realloc(starts, cap * sizeof(long));
                        if (!tmp) { done = 1; break; }
                        starts = tmp;
                    }
                    starts[++depth] = next;          /* remember where the new page begins */
                    break;                           /* show it */
                }
                send_message(fd, "This is the last page.\n");
            } else if (choice == 2) {
                if (depth > 0) {
                    depth--;
                    break;                           /* show the earlier page */
                }
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

static void view_all_feedback(int fd, const Session *session) {
    if (!auth_require_role(session, ROLE_MANAGER)) {        /* role check on the action itself */
        send_message(fd, "Access denied.\n");
        return;
    }
    feedback_browser(fd, 0);
}

static void view_customer_feedback(int fd, const Session *session) {
    char buf[BUFFER_SIZE], msg[BUFFER_SIZE];
    int id;
    Customer c;

    if (!auth_require_role(session, ROLE_MANAGER)) {
        send_message(fd, "Access denied.\n");
        return;
    }

    if (prompt_input(fd, "Enter customer account ID: ", buf, sizeof(buf)) <= 0) return;
    if (parse_id(buf, &id) != 0) {
        send_message(fd, "Invalid account ID.\n");
        return;
    }
    if (customer_get_details(id, &c) != 0) {
        send_message(fd, "Customer not found.\n");
        return;
    }

    snprintf(msg, sizeof(msg), "\nFeedback from %.20s %.20s (account %d)\n",
             c.firstname, c.lastname, c.id);
    send_message(fd, msg);
    feedback_browser(fd, id);
}

static void do_assign_loan(int fd, const Session *session) {
    char buf[BUFFER_SIZE], msg[BUFFER_SIZE];
    int loan_id, emp_id;
    Loan l;
    Customer c;
    Staff e;

    if (!auth_require_role(session, ROLE_MANAGER)) {        /* role check on the action itself */
        send_message(fd, "Access denied.\n");
        return;
    }

    loan_browser(fd, 0, 0, "PA", "Open Loan Applications");

    if (prompt_input(fd, "\nEnter loan ID to assign (0 to go back): ", buf, sizeof(buf)) <= 0) return;
    if (strcmp(buf, "0") == 0) return;
    if (parse_id(buf, &loan_id) != 0 || loan_get(loan_id, &l) != 0) {
        send_message(fd, "Loan not found.\n");
        return;
    }
    if (l.status == LOAN_STATUS_APPROVED || l.status == LOAN_STATUS_REJECTED) {
        snprintf(msg, sizeof(msg), "Loan #%d is already %s.\n", l.id, loan_status_name(l.status));
        send_message(fd, msg);
        return;
    }

    if (customer_get_details(l.customer_id, &c) != 0) memset(&c, 0, sizeof(c));
    snprintf(msg, sizeof(msg),
             "\nLoan #%d  Customer %d (%.20s %.20s)\n"
             "Amount %.2f  Term %d months  Purpose: %.30s\n"
             "Currently: %s",
             l.id, l.customer_id, c.firstname, c.lastname,
             l.amount, l.term_months, l.purpose, loan_status_name(l.status));
    send_message(fd, msg);
    if (l.status == LOAN_STATUS_ASSIGNED) {
        snprintf(msg, sizeof(msg), " to employee %d\n", l.employee_id);
        send_message(fd, msg);
    } else {
        send_message(fd, " (not assigned)\n");
    }

    if (prompt_input(fd, "Assign to employee ID: ", buf, sizeof(buf)) <= 0) return;
    if (parse_id(buf, &emp_id) != 0 || staff_get_details(ROLE_EMPLOYEE, emp_id, &e) != 0) {
        send_message(fd, "Employee not found.\n");
        return;
    }

    snprintf(msg, sizeof(msg), "%s loan #%d to employee %d (%.20s %.20s)? (y/n): ",
             l.status == LOAN_STATUS_ASSIGNED ? "Reassign" : "Assign",
             l.id, e.id, e.firstname, e.lastname);
    if (prompt_input(fd, msg, buf, sizeof(buf)) <= 0) return;
    if (strcmp(buf, "y") != 0 && strcmp(buf, "Y") != 0) {
        send_message(fd, "Cancelled. Nothing was changed.\n");
        return;
    }

    /* expected employee = what this manager was just shown (stale check) */
    switch (loan_assign(l.id, emp_id, session->user_id, l.employee_id)) {
    case LN_OK:
        snprintf(msg, sizeof(msg), "Loan #%d is now assigned to employee %d.\n", l.id, emp_id);
        send_message(fd, msg);
        break;
    case LN_STALE:
        send_message(fd, "This loan was changed by someone else since you viewed it. "
                         "Please view it again. Nothing was changed.\n");
        break;
    case LN_SAME_EMPLOYEE:
        send_message(fd, "The loan is already assigned to that employee.\n");
        break;
    case LN_EMPLOYEE_INVALID:
        send_message(fd, "That employee is deactivated or no longer exists. Nothing was changed.\n");
        break;
    case LN_DECIDED:
        send_message(fd, "The loan has already been decided. Nothing was changed.\n");
        break;
    case LN_NOT_FOUND:
        send_message(fd, "Loan not found. Nothing was changed.\n");
        break;
    default:
        send_message(fd, "Could not assign the loan due to a server error. Nothing was changed.\n");
    }
}

void manager_menu(int fd, Session *session) {
    char buf[BUFFER_SIZE], msg[BUFFER_SIZE];

    /* Role verification: refuse anyone who isn't a logged-in manager */
    if (!auth_require_role(session, ROLE_MANAGER)) {
        send_message(fd, "Access denied.\n");
        return;
    }

    snprintf(msg, sizeof(msg), "\nWelcome, %s!\n", session->username);
    send_message(fd, msg);

    while (1) {
        send_message(fd,
            "\n--- Manager Menu ---\n"
            "1. Activate/Deactivate Customer Accounts\n"
            "2. Assign Loan Applications to Employees\n"
            "3. View All Customer Feedback\n"
            "4. View Feedback of a Specific Customer\n"
            "5. Change Password\n"
            "6. Logout\n");

        if (prompt_input(fd, "Enter choice: ", buf, sizeof(buf)) <= 0)
            return;                                      /* client disconnected */

        switch (atoi(buf)) {
        case 1: 
            send_message(fd, "This feature will be implemented in a later step.\n");
            break;
        case 2:
            do_assign_loan(fd, session);
            break;
        case 3:
            view_all_feedback(fd, session);
            break;
        case 4:
            view_customer_feedback(fd, session);
            break;
        case 5:
            auth_prompt_change_password(fd, session);
            break;
        case 6:
            send_message(fd, "Logged out.\n");
            return;
        default:
            send_message(fd, "Invalid choice.\n");
        }
    }
}