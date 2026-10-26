#ifndef EMPLOYEE_H
#define EMPLOYEE_H

#include "common.h"

void employee_menu(int fd, Session *session);

void modify_customer_screen(int fd); /* caller must have checked the role */
#endif