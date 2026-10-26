#ifndef ADMIN_H
#define ADMIN_H

#include "common.h"

#define ADMIN_MENU_LOGOUT 0      /* back to the role menu */
#define ADMIN_MENU_EXIT   1      /* log out and close the connection */

int admin_menu(int fd, Session *session);

#endif