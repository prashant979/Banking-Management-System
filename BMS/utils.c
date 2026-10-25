#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include "utils.h"

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
