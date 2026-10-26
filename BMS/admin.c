#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"
#include "utils.h"
#include "auth.h"
#include "admin.h"
#include "staff.h"

/* Shared by "Add New Bank Manager" and "Add New Bank Employee". */
static void do_add_staff(int fd, const Session *session, int role) {
    char first[BUFFER_SIZE], last[BUFFER_SIZE], user[BUFFER_SIZE], pass[BUFFER_SIZE];
    char msg[BUFFER_SIZE];
    const char *title = (role == ROLE_MANAGER) ? "Manager" : "Employee";
    int new_id = 0;

    if (!auth_require_role(session, ROLE_ADMIN)) {          /* role check on the action itself */
        send_message(fd, "Access denied.\n");
        return;
    }

    snprintf(msg, sizeof(msg), "\n--- Add New Bank %s ---\n", title);
    send_message(fd, msg);

    if (prompt_input(fd, "First name: ", first, sizeof(first)) <= 0) return;
    if (prompt_input(fd, "Last name: ", last, sizeof(last)) <= 0) return;
    if (prompt_input(fd, "Username (3-20 chars: a-z, 0-9, _): ", user, sizeof(user)) <= 0) return;
    if (prompt_input(fd, "Initial password: ", pass, sizeof(pass)) <= 0) return;

    switch (staff_add(role, first, last, user, pass, &new_id)) {
    case STAFF_ADD_OK:
        snprintf(msg, sizeof(msg),
                 "%s created.\nID      : %d\nUsername: %.20s\n"
                 "Ask them to change the password after the first login.\n",
                 title, new_id, user);
        send_message(fd, msg);
        break;
    case STAFF_ADD_BAD_FIRSTNAME:
    case STAFF_ADD_BAD_LASTNAME:
        snprintf(msg, sizeof(msg), "Names must be 1-%d letters (A-Z, a-z) only. Nothing was saved.\n",
                 CUSTOMER_FIELD_LEN);
        send_message(fd, msg);
        break;
    case STAFF_ADD_BAD_USERNAME:
        snprintf(msg, sizeof(msg),
                 "Username must be 3-%d characters: lowercase letters, digits or underscore. "
                 "Nothing was saved.\n", CUSTOMER_FIELD_LEN);
        send_message(fd, msg);
        break;
    case STAFF_ADD_BAD_PASSWORD:
        snprintf(msg, sizeof(msg),
                 "Password must be %d-%d characters with no spaces or commas. Nothing was saved.\n",
                 MIN_PASSWORD_LEN, MAX_PASSWORD_LEN);
        send_message(fd, msg);
        break;
    case STAFF_ADD_USERNAME_TAKEN:
        send_message(fd, "That username is already taken. Nothing was saved.\n");
        break;
    default:
        send_message(fd, "Could not create the account due to a server error. Nothing was saved.\n");
    }
}

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
        case 1: 
            do_add_staff(fd, session, ROLE_MANAGER);
            break;
        case 2: 
            do_add_staff(fd, session, ROLE_EMPLOYEE);
            break;
        case 3: case 4:
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