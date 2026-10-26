#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common.h"
#include "utils.h"
#include "auth.h"
#include "admin.h"
#include "staff.h"
#include <time.h>
#include "role.h"

#define ROLE_HISTORY_MAX 50


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

static const char *role_name(int role) {
    switch (role) {
    case ROLE_EMPLOYEE: return "Employee";
    case ROLE_MANAGER:  return "Manager";
    case ROLE_ADMIN:    return "Administrator";
    default:            return "Customer";
    }
}

static void do_change_role(int fd, const Session *session) {
    char buf[BUFFER_SIZE], msg[BUFFER_SIZE];
    int from_role, id, to_role = 0, to_id = 0;
    Staff s;

    if (!auth_require_role(session, ROLE_ADMIN)) {
        send_message(fd, "Access denied.\n");
        return;
    }

    if (prompt_input(fd, "Current role (2 = Employee, 3 = Manager): ", buf, sizeof(buf)) <= 0) return;
    from_role = atoi(buf);
    if (from_role != ROLE_EMPLOYEE && from_role != ROLE_MANAGER) {
        send_message(fd, "Only Employee <-> Manager role changes are allowed.\n");
        return;
    }

    if (prompt_input(fd, "Account ID: ", buf, sizeof(buf)) <= 0) return;
    if (parse_id(buf, &id) != 0) {
        send_message(fd, "Invalid account ID.\n");
        return;
    }
    if (staff_get_details(from_role, id, &s) != 0) {
        send_message(fd, "Account not found.\n");
        return;
    }

    int target = (from_role == ROLE_EMPLOYEE) ? ROLE_MANAGER : ROLE_EMPLOYEE;
    snprintf(msg, sizeof(msg),
             "\n%s %d: %.20s %.20s (username %.20s, %s)\nMove to %s? (y/n): ",
             role_name(from_role), s.id, s.firstname, s.lastname, s.username,
             s.active ? "active" : "deactivated", role_name(target));
    if (prompt_input(fd, msg, buf, sizeof(buf)) <= 0) return;
    if (strcmp(buf, "y") != 0 && strcmp(buf, "Y") != 0) {
        send_message(fd, "Cancelled. Nothing was changed.\n");
        return;
    }

    switch (role_change(from_role, id, session->user_id, &to_role, &to_id)) {
    case RC_OK:
        snprintf(msg, sizeof(msg),
                 "Role changed: %.20s is now %s (ID %d).\n"
                 "The old %s account %d is deactivated. Log in with the new role.\n",
                 s.username, role_name(to_role), to_id, role_name(from_role), id);
        send_message(fd, msg);
        break;
    case RC_NOT_FOUND:
        send_message(fd, "Account not found. Nothing was changed.\n");
        break;
    case RC_INACTIVE:
        send_message(fd, "This account is deactivated (it may already have been moved). Nothing was changed.\n");
        break;
    case RC_LOGGED_IN:
        send_message(fd, "This user is currently logged in. Ask them to log out first. Nothing was changed.\n");
        break;
    case RC_USERNAME_TAKEN:
        send_message(fd, "An active account with this username already exists in the target role. "
                         "Nothing was changed.\n");
        break;
    default:
        send_message(fd, "Could not change the role due to a server error. Nothing was changed.\n");
    }
}

static void do_view_role_history(int fd, const Session *session) {
    char buf[BUFFER_SIZE], msg[BUFFER_SIZE];
    RoleChange list[ROLE_HISTORY_MAX];

    if (!auth_require_role(session, ROLE_ADMIN)) {
        send_message(fd, "Access denied.\n");
        return;
    }

    if (prompt_input(fd, "Username: ", buf, sizeof(buf)) <= 0) return;
    if (!is_valid_username(buf)) {
        send_message(fd, "Invalid username.\n");
        return;
    }

    int n = role_history_for_user(buf, list, ROLE_HISTORY_MAX);
    if (n < 0) {
        send_message(fd, "Could not read the role history. Please try again.\n");
        return;
    }
    if (n == 0) {
        send_message(fd, "No role changes recorded for this user.\n");
        return;
    }

    snprintf(msg, sizeof(msg), "\nRole history of %.20s (oldest first):\n", buf);
    send_message(fd, msg);

    for (int i = 0; i < n; i++) {
        char when[32];
        struct tm tmv;
        time_t ts = (time_t)list[i].timestamp;
        localtime_r(&ts, &tmv);
        strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tmv);

        snprintf(msg, sizeof(msg), "%2d. %s  %s %d -> %s %d  (by admin %d)\n",
                 i + 1, when,
                 role_name(list[i].from_role), list[i].from_id,
                 role_name(list[i].to_role), list[i].to_id, list[i].changed_by);
        send_message(fd, msg);
    }

    snprintf(msg, sizeof(msg), "Previous role: %s (ID %d)\nCurrent role : %s (ID %d)\n",
             role_name(list[n - 1].from_role), list[n - 1].from_id,
             role_name(list[n - 1].to_role), list[n - 1].to_id);
    send_message(fd, msg);
}

static void manage_roles_menu(int fd, const Session *session) {
    char buf[BUFFER_SIZE];

    while (1) {
        send_message(fd,
            "\n--- Manage User Roles ---\n"
            "1. Change Role (Employee <-> Manager)\n"
            "2. View Role History of a User\n"
            "3. Back\n");

        if (prompt_input(fd, "Enter choice: ", buf, sizeof(buf)) <= 0) return;

        switch (atoi(buf)) {
        case 1: do_change_role(fd, session);       break;
        case 2: do_view_role_history(fd, session); break;
        case 3: return;
        default: send_message(fd, "Invalid choice.\n");
        }
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
        case 3:
            send_message(fd, "This feature will be implemented in a later step.\n");
            break;
        case 4:
            manage_roles_menu(fd, session);
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