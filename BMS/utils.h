#ifndef UTILS_H
#define UTILS_H

#include <stddef.h>
#include <sys/types.h>
#include <fcntl.h>      /* F_RDLCK, F_WRLCK */
#include <stdio.h>

int get_next_id_csv(FILE *fp);   /* max id in first column + 1 (1 if empty) */
int     send_message(int fd, const char *msg);
ssize_t read_input(int fd, char *buf, size_t size);
ssize_t prompt_input(int fd, const char *prompt, char *buf, size_t size);
void    trim_newline(char *s);

/* fcntl whole-file locks. type: F_RDLCK (shared) or F_WRLCK (exclusive). Blocks until granted. */
int     lock_file(int fd, short type);
int     unlock_file(int fd);

/* fcntl byte-range (record) locks. type: F_RDLCK / F_WRLCK. Blocks until granted. */
int lock_record(int fd, short type, off_t start, off_t len);
int unlock_record(int fd, off_t start, off_t len);

#endif
