#pragma once
#include <stddef.h>
#include "sys/select.h"
#define IPPROTO_TCP 6
#define SOL_SOCKET 1
#define SO_SNDTIMEO 21
#define SO_KEEPALIVE 9
#define MSG_NOSIGNAL 0x4000
#define MSG_DONTWAIT 0x40
#define SHUT_RDWR 2
int test_ws_send(int fd, const void *data, size_t length, int flags);
int test_ws_shutdown(int fd, int how);
int test_ws_setsockopt(int fd, int level, int option, const void *value, size_t size);
#define send test_ws_send
#define shutdown test_ws_shutdown
#define setsockopt test_ws_setsockopt
