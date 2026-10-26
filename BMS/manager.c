#include <stdio.h>
#include <stdlib.h>
#include "common.h"
#include "utils.h"
#include "auth.h"
#include "manager.h"

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
        case 1: case 2: case 3: case 4:
            send_message(fd, "This feature will be implemented in a later step.\n");
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