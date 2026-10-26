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

/* Append-only fixed-width files: row 0 = header, row n at offset n * row_size. */
long log_reserve(int fd, int row_size);     /* new row number (>= 1); that row stays exclusively locked; -1 on error */
int  log_write_row(int fd, long row_no, int row_size, const char *row);   /* pwrite + fsync */
int  log_release_row(int fd, long row_no, int row_size);                  /* unlock the row */
long log_row_count(int fd, int row_size);   /* number of data rows, -1 if the file size is not a multiple of row_size */
int  log_validate_file(const char *path, int row_size, const char *header);   /* create with header if missing, else verify */

int password_policy_ok(const char *pw);   /* 0 if acceptable, -1 otherwise */

int parse_id(const char *s, int *out);     /* digits only, 1-9 chars, value > 0; 0 on success */

int is_valid_name(const char *s);        /* letters only, 1-20 */
int is_valid_username(const char *s);    /* a-z, 0-9, '_', 3-20 */
int is_valid_password(const char *s);    /* 6-20 printable chars, no spaces or commas */

#endif
