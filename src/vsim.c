// SPDX-FileCopyrightText: 2007-2016 The original vcontrold authors (cf. doc/original_authors.txt)
//
// SPDX-License-Identifier: GPL-3.0-or-later

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <syslog.h>
#include <unistd.h>
#include <termios.h>
#include <string.h>
#include <time.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <sys/time.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <stdarg.h>
#include "socket.h"
#include "vclient.h"

#define SERVERPORT 6578
int makeDaemon = 0;
int inetversion = 0;
int readCmdFile(char *filename, char *result, int *resultLen, char *device );
int interactive(int socketfd, char *device);
void printHelp(int socketfd);
int rawModus (int socketfd, char *device);
static void sigPipeHandler(int signo);

void logIT (int class, char *string, ...)
{
    va_list arguments;
    char *print_buffer;

    va_start(arguments, string);
    vasprintf(&print_buffer, string, arguments);
    va_end(arguments);
    printf("%s\n", print_buffer);
    free(print_buffer);
}

static void sigPipeHandler(int signo)
{
    logIT(LOG_ERR, "Received SIGPIPE");
}

static void dump(char *buf, int len, char *txt)
{
    int i = 0;
    printf("%s:\n", txt);
    for (i = 0; i < len; i++) {
        printf(" %02x", (unsigned char)buf[i]);
    }
    printf("\n");
}

typedef struct {
    int cmdlen;
    char cmd[20];
    int rsplen;
    char rsp[20];
} ctable;

ctable cmds[] = {
    { 1, { 0x04 }, 1, { 0x05 } },
    { 5, { 0x01, 0xF7, 0x08, 0x00, 0x02 }, 2, { 0xE2, 0xFF } },
    { 5, { 0x01, 0xF7, 0x08, 0x02, 0x02 }, 2, { 0x94, 0x02 } },
    { 5, { 0x01, 0xF7, 0x08, 0x04, 0x02 }, 2, { 0xCC, 0x01 } },
    { 5, { 0x01, 0xF7, 0x23, 0x06, 0x01 }, 1, { 0x14 } },
    { 5, { 0x01, 0xF7, 0x23, 0x07, 0x01 }, 1, { 0x10 } },
    { 5, { 0x01, 0xF7, 0x23, 0x08, 0x01 }, 1, { 0x16 } },
    { 5, { 0x01, 0xF7, 0x63, 0x00, 0x01 }, 1, { 0x32 } },
    { 5, { 0x01, 0xF7, 0x08, 0x10, 0x02 }, 2, { 0xC2, 0x01 } },
    { 5, { 0x01, 0xF7, 0x08, 0x12, 0x02 }, 2, { 0xC2, 0x01 } },
    { 5, { 0x01, 0xF7, 0x08, 0x42, 0x01 }, 1, { 0x01 } },
    { 5, { 0x01, 0xF7, 0x23, 0x23, 0x01 }, 1, { 0x02 } },
    { 5, { 0x01, 0xF7, 0x27, 0xD4, 0x01 }, 1, { 0x00 } },
    { 5, { 0x01, 0xF7, 0x27, 0xD3, 0x01 }, 1, { 0x0E } },
    { 5, { 0x01, 0xF7, 0xCF, 0x30, 0x04 }, 4, { 0xE8, 0x03, 0x00, 0x00 } },
    { 5, { 0x01, 0xF7, 0x08, 0x8A, 0x04 }, 4, { 0x2D, 0x49, 0x01, 0x00 } },
    { 5, { 0x01, 0xF7, 0x08, 0x96, 0x02 }, 2, { 0xCF, 0x00 } },
    { 5, { 0x01, 0xF7, 0x00, 0xF8, 0x02 }, 2, { 0x20, 0x9F } }
};

// The number of commands is derived from the table itself, so it can't get out of sync
int cmdc = sizeof(cmds) / sizeof(cmds[0]);

char input[100];
int inpidx = 0;
static double enq_interval = 2.3;
static int activity_only = 0;

static double monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}


static void handle(int fd)
{
    inpidx = 0;
    char buf[1] = "\0";
    ssize_t len;
    double deadline = monotonic_seconds() + enq_interval;

    while ( 1 ) {
        fd_set readable;
        struct timeval timeout, *wait = NULL;
        FD_ZERO(&readable);
        FD_SET(fd, &readable);
        /* Never inject synchronization into a partially received request. */
        if (enq_interval > 0 && inpidx == 0) {
            double remaining = deadline - monotonic_seconds();
            if (remaining < 0) remaining = 0;
            timeout.tv_sec = (time_t)remaining;
            timeout.tv_usec = (suseconds_t)((remaining - timeout.tv_sec) * 1000000);
            wait = &timeout;
        }
        int ready = select(fd + 1, &readable, NULL, NULL, wait);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return;
        }
        if (ready == 0) {
            char enq = 0x05;
            if (writen(fd, &enq, 1) != 1) return;
            deadline = monotonic_seconds() + enq_interval;
            continue;
        }
        len = readn(fd, buf, 1);
        deadline = monotonic_seconds() + enq_interval;
        if  (len < 0) {
            perror("read eror\n");
            exit(-1);
        } else if (len == 0) {
            printf("eof read\n");
            return;
        } else {
            int i = 0;
            int j = 0;
            while ( i < len ) {
                if (inpidx >= (int)sizeof(input)) { return; }
                input[inpidx] = buf[i];
                dump(&input[inpidx], 1, "received char:");
                inpidx++;
                /* Synthetic KW2 single-byte target writes: update matching read fixture. */
                if (inpidx == 6 && (unsigned char)input[0] == 0x01 &&
                    (unsigned char)input[1] == 0xf4 && (unsigned char)input[4] == 1) {
                    unsigned int addr = ((unsigned char)input[2] << 8) | (unsigned char)input[3];
                    if (addr == 0x2306 || addr == 0x2307 || addr == 0x2308 || addr == 0x6300) {
                        for (j = 0; j < cmdc; j++) {
                            if (cmds[j].cmdlen == 5 && (unsigned char)cmds[j].cmd[1] == 0xf7 &&
                                cmds[j].cmd[2] == input[2] && cmds[j].cmd[3] == input[3]) {
                                if (!activity_only) cmds[j].rsp[0] = input[5];
                            }
                        }
                        char ack = 0;
                        if (!activity_only) writen(fd, &ack, 1);
                        inpidx = 0;
                        i++;
                        continue;
                    }
                }
                for ( j = 0 ; j < cmdc; j++) {
                    if ( cmds[j].cmd[0] == input[0] ) {
                        if (( cmds[j].cmdlen == inpidx) && (!memcmp(cmds[j].cmd, input, inpidx))) {
                            dump(input, inpidx, "received cmd:");
                            dump(cmds[j].rsp, cmds[j].rsplen, "answer:");
                            if ((!activity_only || (unsigned char)input[0] == 0x04) &&
                                writen(fd, cmds[j].rsp, cmds[j].rsplen) != cmds[j].rsplen) {
                                printf("not completely written\n");
                            }
                            inpidx = 0;
                        }
                    }
                }
                i++;
            }
        }
    }
}

int main(int argc, char *argv[])
{
    int sockfd = -1;
    int listenfd = -1;

    for (int arg = 1; arg < argc; arg++) {
        if (!strcmp(argv[arg], "--activity-only")) {
            activity_only = 1;
        } else if (!strcmp(argv[arg], "--enq-interval") && arg + 1 < argc) {
            char *end;
            enq_interval = strtod(argv[++arg], &end);
            if (!*argv[arg] || *end || !(enq_interval >= 0 && enq_interval <= 86400)) {
                fprintf(stderr, "Invalid ENQ interval (0..86400 seconds)\n");
                return 2;
            }
        } else {
            fprintf(stderr, "Usage: vsim [--enq-interval SECONDS] [--activity-only]\n");
            return !strcmp(argv[arg], "--help") ? 0 : 2;
        }
    }
    listenfd = openSocket2(SERVERPORT, "127.0.0.1");
    while (1) {
        sockfd = listenToSocket(listenfd, makeDaemon);
        if (signal(SIGPIPE, sigPipeHandler) == SIG_ERR) {
            logIT(LOG_ERR, "Signal error");
            exit(1);
        }
        if (sockfd >= 0) {
            // Socket returned fd, the rest is done interactively
            logIT(LOG_INFO, "\nvcontrold connected");
            handle(sockfd);
            logIT(LOG_INFO, "\nvcontrold disconnected");
            closeSocket(sockfd);
        } else {
            logIT(LOG_ERR, "Error during connection setup");
        }
    }
}
