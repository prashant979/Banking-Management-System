#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "utils.h"
#include "common.h"

/* Writes the whole string, handling partial writes and EINTR. */
int send_message(int fd, const char *msg) {
    size_t total = 0, len = strlen(msg);
    while (total < len) {
        ssize_t n = write(fd, msg + total, len - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += n;
    }
    return 0;
}

/* Reads ONE line (byte by byte), strips \r\n. Returns >0 on success, 0 on EOF, -1 on error. */
ssize_t read_input(int fd, char *buf, size_t size) {
    size_t i = 0;
    int got_any = 0;
    char c;

    for (;;) {
        ssize_t n = read(fd, &c, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break;                      /* EOF */
        got_any = 1;
        if (c == '\n') break;
        if (i < size - 1) buf[i++] = c;         /* extra chars of an over-long line are dropped */
    }
    buf[i] = '\0';
    if (!got_any) return 0;                     /* clean EOF */
    trim_newline(buf);
    return (ssize_t)(i ? i : 1);
}

/* Sends a prompt, then reads the answer. Same return values as read_input. */
ssize_t prompt_input(int fd, const char *prompt, char *buf, size_t size) {
    if (send_message(fd, prompt) < 0) return -1;
    return read_input(fd, buf, size);
}

void trim_newline(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'))
        s[--len] = '\0';
}

/* Lock the whole file. F_SETLKW = wait until the lock is available. */
int lock_file(int fd, short type) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type   = type;
    fl.l_whence = SEEK_SET;
    fl.l_start  = 0;
    fl.l_len    = 0;               /* 0 = to end of file */
    while (fcntl(fd, F_SETLKW, &fl) < 0) {
        if (errno != EINTR) return -1;
    }
    return 0;
}

int unlock_file(int fd) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type   = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start  = 0;
    fl.l_len    = 0;
    return fcntl(fd, F_SETLK, &fl);
}

int get_next_id_csv(FILE *fp) {
    char line[512];
    int max_id = 0;
    rewind(fp);
    while (fgets(line, sizeof(line), fp)) {
        int id;
        if (sscanf(line, "%d,", &id) == 1 && id > max_id) max_id = id;   /* header line is skipped */
    }
    return max_id + 1;
}

int lock_record(int fd, short type, off_t start, off_t len) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type   = type;
    fl.l_whence = SEEK_SET;
    fl.l_start  = start;          /* first byte of the record */
    fl.l_len    = len;            /* length of the record; 0 = to end of file */
    while (fcntl(fd, F_SETLKW, &fl) < 0) {
        if (errno != EINTR) return -1;        /* includes EDEADLK */
    }
    return 0;
}

int unlock_record(int fd, off_t start, off_t len) {
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type   = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start  = start;
    fl.l_len    = len;
    return fcntl(fd, F_SETLK, &fl);
}

/* Reserve the next row. The guard lock (header byte 0) is held only long enough to read the
   file size, lock our own new row and extend the file with a blank row. No fsync happens here. */
long log_reserve(int fd, int row_size) {
    char blank[512];
    struct stat st;

    if (row_size < 2 || row_size > (int)sizeof(blank)) return -1;
    if (lock_record(fd, F_WRLCK, 0, 1) < 0) return -1;              /* guard */

    if (fstat(fd, &st) < 0 || st.st_size < row_size || st.st_size % row_size != 0) {
        unlock_record(fd, 0, 1);
        return -1;
    }

    off_t off = st.st_size;
    long row_no = (long)(off / row_size);

    if (lock_record(fd, F_WRLCK, off, row_size) < 0) {              /* our own new row, beyond EOF */
        unlock_record(fd, 0, 1);
        return -1;
    }

    memset(blank, ' ', row_size);
    blank[row_size - 1] = '\n';
    if (pwrite(fd, blank, row_size, off) != row_size) {             /* extends the file */
        unlock_record(fd, off, row_size);
        unlock_record(fd, 0, 1);
        return -1;
    }

    unlock_record(fd, 0, 1);                                        /* guard released: others may reserve */
    return row_no;
}

/* Caller holds the row lock from log_reserve (or took one itself). */
int log_write_row(int fd, long row_no, int row_size, const char *row) {
    if (pwrite(fd, row, row_size, (off_t)row_no * row_size) != row_size) return -1;
    return (fsync(fd) == 0) ? 0 : -1;
}

int log_release_row(int fd, long row_no, int row_size) {
    return unlock_record(fd, (off_t)row_no * row_size, row_size);
}

long log_row_count(int fd, int row_size) {
    struct stat st;
    if (fstat(fd, &st) < 0) return -1;
    if (st.st_size < row_size || st.st_size % row_size != 0) return -1;
    return (long)(st.st_size / row_size) - 1;
}

int log_validate_file(const char *path, int row_size, const char *header) {
    struct stat st;
    char last;

    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return -1;
    if (fstat(fd, &st) < 0) { close(fd); return -1; }

    if (st.st_size == 0) {                                          /* fresh install */
        int ok = (write(fd, header, row_size) == row_size) && (fsync(fd) == 0);
        close(fd);
        return ok ? 0 : -1;
    }

    int ok = (st.st_size >= row_size && st.st_size % row_size == 0 &&
              pread(fd, &last, 1, row_size - 1) == 1 && last == '\n');
    close(fd);
    return ok ? 0 : -1;
}

/* MIN_PASSWORD_LEN..MAX_PASSWORD_LEN printable ASCII characters, no spaces, no commas
   (a comma would break the CSV row). One policy shared by every role. */
int password_policy_ok(const char *pw) {
    size_t len = strlen(pw);
    if (len < MIN_PASSWORD_LEN || len > MAX_PASSWORD_LEN) return -1;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)pw[i];
        if (c <= 32 || c >= 127 || c == ',') return -1;
    }
    return 0;
}

int parse_id(const char *s, int *out) {
    size_t len = strlen(s);
    if (len == 0 || len > 9) return -1;
    for (size_t i = 0; i < len; i++)
        if (!isdigit((unsigned char)s[i])) return -1;
    int v = atoi(s);
    if (v <= 0) return -1;
    *out = v;
    return 0;
}

int is_valid_name(const char *s) {
    size_t len = strlen(s);
    if (len < 1 || len > CUSTOMER_FIELD_LEN) return 0;
    for (size_t i = 0; i < len; i++)
        if (!isalpha((unsigned char)s[i])) return 0;
    return 1;
}

int is_valid_username(const char *s) {
    size_t len = strlen(s);
    if (len < 3 || len > CUSTOMER_FIELD_LEN) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!islower(c) && !isdigit(c) && c != '_') return 0;
    }
    return 1;
}

int is_valid_password(const char *s) {
    size_t len = strlen(s);
    if (len < MIN_PASSWORD_LEN || len > MAX_PASSWORD_LEN) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c <= 32 || c >= 127 || c == ',') return 0;
    }
    return 1;
}
/* Changes the status byte (second-last byte) of one log row, under that row's lock only. */
int log_set_status(int fd, long row_no, int row_size, char status) {
    off_t off = (off_t)row_no * row_size;
    int rc = -1;

    if (lock_record(fd, F_WRLCK, off, row_size) < 0) return -1;
    if (pwrite(fd, &status, 1, off + row_size - 2) == 1 && fsync(fd) == 0) rc = 0;
    unlock_record(fd, off, row_size);
    return rc;
}   /* rewrite one row's status byte */