/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Single-process trace mode. The same owner reads both idle and solicited bytes.
 * Time stamps are host read times, not hardware arrival times. No threads/forks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/select.h>
#include "io.h"
#include "trace.h"

static int optical_fd = -1;
static FILE *output;
static char *optical_device;

int optical_trace_enabled(void) { return output != NULL; }

void optical_trace_record(const char *kind, const void *data, size_t length)
{
    struct timespec wall, mono;
    struct tm utc;
    char timestamp[40];
    const unsigned char *bytes = data;
    size_t i;
    if (!output) return;
    clock_gettime(CLOCK_REALTIME, &wall);
    clock_gettime(CLOCK_MONOTONIC, &mono);
    gmtime_r(&wall.tv_sec, &utc);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &utc);
    fprintf(output, "%s.%09ldZ mono=%lld.%09ld %s len=%zu", timestamp,
            wall.tv_nsec, (long long)mono.tv_sec, mono.tv_nsec, kind, length);
    for (i = 0; i < length; ++i) fprintf(output, " %02X", bytes[i]);
    fputc('\n', output);
    /* A failed trace must not silently masquerade as a complete capture. */
    if (fflush(output) || ferror(output)) {
        perror("optical trace write failed");
        exit(2);
    }
}

int optical_trace_start(const char *path, char *device)
{
    int fd, flags = O_WRONLY | O_APPEND | O_CREAT;
    struct stat st;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    fd = open(path, flags, 0600);
    if (fd < 0) return -1;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        close(fd); errno = EINVAL; return -1;
    }
    /* Open once before enabling the dup path in openDevice. */
    optical_fd = openDevice(device);
    if (optical_fd < 0) { close(fd); return -1; }
    if (optical_fd >= FD_SETSIZE) {
        close(fd); close(optical_fd); optical_fd = -1;
        errno = EMFILE; return -1;
    }
    output = fdopen(fd, "a");
    if (!output) { close(fd); close(optical_fd); optical_fd = -1; return -1; }
    optical_device = strdup(device);
    if (!optical_device) { perror("trace device allocation"); exit(2); }
    optical_trace_record("START", NULL, 0);
    return 0;
}

int optical_trace_device(const char *device)
{
    if (!output || strcmp(device, optical_device)) { errno = EINVAL; return -1; }
    optical_trace_record("SESSION-OPEN", NULL, 0);
    return dup(optical_fd);
}

/* Drain only bytes currently queued. On a TTY the old code threw these away;
 * capture them first. Keep a bounded batch so a noisy source cannot starve TCP.
 */
static int drain_idle(const char *kind)
{
    int batch;
    for (batch = 0; batch < 64; ++batch) {
        fd_set ready;
        struct timeval zero = {0, 0};
        unsigned char data[256];
        ssize_t n;
        int rc;
        FD_ZERO(&ready); FD_SET(optical_fd, &ready);
        rc = select(optical_fd + 1, &ready, NULL, NULL, &zero);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) return rc;
        n = read(optical_fd, data, sizeof(data));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            optical_trace_record("OPTICAL-EOF-OR-ERROR", NULL, 0);
            if (!n) errno = EIO;
            return -1;
        }
        optical_trace_record(kind, data, (size_t)n);
    }
    return 1;
}

/* Wait for TCP input/accept while continuously draining/logging optical input.
 * Idle bytes are explicitly labelled, not decoded or forwarded as TCP replies.
 */
int optical_trace_wait(int socketfd)
{
    if (socketfd >= FD_SETSIZE) { errno = EMFILE; return -1; }
    for (;;) {
        fd_set ready;
        int rc, top = optical_fd > socketfd ? optical_fd : socketfd;
        FD_ZERO(&ready); FD_SET(socketfd, &ready); FD_SET(optical_fd, &ready);
        rc = select(top + 1, &ready, NULL, NULL, NULL);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0) return -1;
        if (FD_ISSET(optical_fd, &ready) && drain_idle("RX-IDLE") < 0) return -1;
        if (FD_ISSET(socketfd, &ready)) return 0;
    }
}

ssize_t optical_trace_line(int socketfd, void *buffer, size_t length)
{
    char *line = buffer;
    size_t used = 0;
    if (!length) { errno = EINVAL; return -1; }
    optical_trace_record("WAIT-COMMAND", NULL, 0);
    while (used + 1 < length) {
        ssize_t n;
        if (optical_trace_wait(socketfd)) return -1;
        n = read(socketfd, line + used, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        if (!n) break;
        if (line[used++] == '\n') break;
    }
    line[used] = 0;
    if (used) optical_trace_record("COMMAND", line, used);
    else optical_trace_record("CLIENT-EOF", NULL, 0);
    return (ssize_t)used;
}

void optical_trace_flush(int fd)
{
    if (!output) { tcflush(fd, TCIOFLUSH); return; }
    optical_trace_record("PRE-TX-DRAIN", NULL, 0);
    if (drain_idle("RX-PRE-TX-DISCARD") < 0) {
        perror("optical trace drain failed"); exit(2);
    }
    /* Never flush unread RX bytes in trace mode. Bytes racing the drain remain
     * available to the protocol reader and will be logged there. */
    tcflush(fd, TCOFLUSH);
}
