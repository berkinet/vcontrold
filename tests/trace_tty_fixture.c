/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Test-only serial opener: pseudo-terminals do not implement adapter DTR ioctls. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include "trace.h"
static int opened = -1;
int openDevice(char *device)
{
    struct termios config;
    opened = open(device, O_RDWR | O_NOCTTY);
    if (opened < 0 || tcgetattr(opened, &config)) return -1;
    cfmakeraw(&config);
    if (tcsetattr(opened, TCSANOW, &config)) return -1;
    return opened;
}
int main(int argc, char **argv)
{
    char go, reply[2];
    int session;
    if (argc != 3 || optical_trace_start(argv[2], argv[1])) return 2;
    puts("READY"); fflush(stdout);
    /* Deliberate fixture pause lets the test queue bytes for pre-TX draining. */
    if (read(STDIN_FILENO, &go, 1) != 1) return 3;
    session = optical_trace_device(argv[1]);
    optical_trace_flush(session);
    puts("FLUSHED"); fflush(stdout);
    if (read(session, reply, 2) != 2) return 4;
    optical_trace_record("RX-REPLY", reply, 2);
    close(session);
    if ((unsigned char)reply[0] != 0x12 || (unsigned char)reply[1] != 0x01) return 5;
    puts("IDLE"); fflush(stdout);
    /* Closing the session dup must leave continuous capture alive. */
    if (optical_trace_wait(STDIN_FILENO)) return 6;
    return 0;
}
