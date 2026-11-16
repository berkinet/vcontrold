/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OPTICAL_TRACE_H
#define OPTICAL_TRACE_H
#include <stddef.h>
#include <sys/types.h>
int optical_trace_start(const char *path, char *device);
int optical_trace_enabled(void);
int optical_trace_device(const char *device);
void optical_trace_record(const char *kind, const void *data, size_t length);
int optical_trace_wait(int socketfd);
ssize_t optical_trace_line(int socketfd, void *buffer, size_t length);
void optical_trace_flush(int fd);
#endif
