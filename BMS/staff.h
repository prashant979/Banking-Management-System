#ifndef STAFF_H
#define STAFF_H

#include "common.h"

/* Fixed-width staff records (employee.csv / manager.csv / admin.csv).
   One fcntl byte-range lock per record, same scheme as customer.csv. */
int staff_validate_file(int role);          /* startup check; creates the file if missing */
int staff_open(int role);                   /* fd (open once per request, keep it) or -1 */
int staff_count(int fd);                    /* number of staff members, -1 on error */
int staff_lock(int fd, int idx, short type);
int staff_unlock(int fd, int idx);
int staff_read_record(int fd, int idx, Staff *s);
int staff_write_record(int fd, int idx, const Staff *s);       /* in place + fsync */
int staff_find_by_username(int fd, const char *username);      /* index, -1 not found, -2 error */
int staff_find_by_id(int fd, int id);                          /* same return values */

#endif