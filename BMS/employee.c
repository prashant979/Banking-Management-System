#include <stdio.h>
#include <stdlib.h>
#include "common.h"
#include "utils.h"
#include "auth.h"
#include "employee.h"

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
        case 1: case 2: case 3: case 4:
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