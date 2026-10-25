#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"
#include "utils.h"
#include "auth.h"
#include "customer.h"
#include "employee.h"

static void do_add_customer(int fd, const Session *session) {
    char first[BUFFER_SIZE], last[BUFFER_SIZE], user[BUFFER_SIZE];
    char pass[BUFFER_SIZE], bal[BUFFER_SIZE], msg[BUFFER_SIZE];
    int new_id = 0;

    if (!auth_require_role(session, ROLE_EMPLOYEE)) {      /* role check on the action itself */
        send_message(fd, "Access denied.\n");
        return;
    }

    if (prompt_input(fd, "Customer first name: ", first, sizeof(first)) <= 0) return;
    if (prompt_input(fd, "Customer last name: ", last, sizeof(last)) <= 0) return;
    if (prompt_input(fd, "Username (3-20 chars: a-z, 0-9, _): ", user, sizeof(user)) <= 0) return;
    if (prompt_input(fd, "Initial password: ", pass, sizeof(pass)) <= 0) return;
    if (prompt_input(fd, "Opening balance (0 for none): ", bal, sizeof(bal)) <= 0) return;

    switch (customer_add(first, last, user, pass, bal, &new_id)) {
    case ADD_OK:
        snprintf(msg, sizeof(msg),
                 "Customer created.\nAccount ID: %d\nUsername  : %.20s\n"
                 "Ask the customer to change the password after the first login.\n",
                 new_id, user);
        send_message(fd, msg);
        break;
    case ADD_BAD_FIRSTNAME:
    case ADD_BAD_LASTNAME:
        snprintf(msg, sizeof(msg), "Names must be 1-%d letters (A-Z, a-z) only. Customer not created.\n",
                 CUSTOMER_FIELD_LEN);
        send_message(fd, msg);
        break;
    case ADD_BAD_USERNAME:
        snprintf(msg, sizeof(msg),
                 "Username must be 3-%d characters: lowercase letters, digits or underscore. "
                 "Customer not created.\n", CUSTOMER_FIELD_LEN);
        send_message(fd, msg);
        break;
    case ADD_BAD_PASSWORD:
        snprintf(msg, sizeof(msg),
                 "Password must be %d-%d characters with no spaces or commas. Customer not created.\n",
                 MIN_PASSWORD_LEN, MAX_PASSWORD_LEN);
        send_message(fd, msg);
        break;
    case ADD_BAD_BALANCE:
        snprintf(msg, sizeof(msg),
                 "Opening balance must be 0 to %.2f with at most 2 decimals. Customer not created.\n",
                 MAX_TXN_AMOUNT);
        send_message(fd, msg);
        break;
    case ADD_USERNAME_TAKEN:
        send_message(fd, "That username is already taken. Customer not created.\n");
        break;
    default:
        send_message(fd, "Could not create the customer due to a server error. Nothing was saved.\n");
    }
}

void employee_menu(int fd, Session *session) {
    char buf[BUFFER_SIZE], msg[BUFFER_SIZE];

    /* Role verification: refuse anyone who isn't a logged-in employee */
    if (!auth_require_role(session, ROLE_EMPLOYEE)) {
        send_message(fd, "Access denied.\n");
        return;
    }

    snprintf(msg, sizeof(msg), "\nWelcome, %s!\n", session->username);
    send_message(fd, msg);

    while (1) {
        send_message(fd,
            "\n--- Employee Menu ---\n"
            "1. Add New Customer\n"
            "2. Modify Customer Details\n"
            "3. View/Process Assigned Loans\n"
            "4. Change Password\n"
            "5. Logout\n");

        if (prompt_input(fd, "Enter choice: ", buf, sizeof(buf)) <= 0)
            return;                                      /* client disconnected */

        switch (atoi(buf)) {
        case 1:
            do_add_customer(fd, session);
            break;
        case 2: case 3: case 4:
            send_message(fd, "This feature will be implemented in a later step.\n");
            break;
        case 5:
            send_message(fd, "Logged out.\n");
            return;
        default:
            send_message(fd, "Invalid choice.\n");
        }
    }
}