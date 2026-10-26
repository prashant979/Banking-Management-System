#include <stdio.h>
#include <stdlib.h>
#include "common.h"
#include "utils.h"
#include "auth.h"
#include "admin.h"

int admin_menu(int fd, Session *session) {
    char buf[BUFFER_SIZE], msg[BUFFER_SIZE];

    /* Role verification: refuse anyone who isn't a logged-in administrator */
    if (!auth_require_role(session, ROLE_ADMIN)) {
        send_message(fd, "Access denied.\n");
        return ADMIN_MENU_LOGOUT;
    }

    snprintf(msg, sizeof(msg), "\nWelcome, %.20s!\n", session->username);
    send_message(fd, msg);

    while (1) {
        send_message(fd,
            "\n--- Administrator Menu ---\n"
            "1. Add New Bank Manager\n"
            "2. Add New Bank Employee\n"
            "3. Modify Manager/Customer/Employee Details\n"
            "4. Manage User Roles\n"
            "5. Change Password\n"
            "6. Logout\n");

        if (prompt_input(fd, "Enter choice: ", buf, sizeof(buf)) <= 0)
            return ADMIN_MENU_EXIT;                      /* client disconnected */

        switch (atoi(buf)) {
        case 1: case 2: case 3: case 4:
            send_message(fd, "This feature will be implemented in a later step.\n");
            break;
        case 5:
            auth_prompt_change_password(fd, session);
            break;
        case 6:
            send_message(fd, "Logged out.\n");
            return ADMIN_MENU_LOGOUT;
        default:
            send_message(fd, "Invalid choice.\n");
        }
    }
}