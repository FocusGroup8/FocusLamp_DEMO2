#pragma once
typedef struct {int fd;} fd_set;
struct timeval {long tv_sec, tv_usec;};
#define FD_ZERO(set) ((set)->fd = -1)
#define FD_SET(value, set) ((set)->fd = (value))
int test_ws_select(int count, fd_set *readable, fd_set *writable, fd_set *error, struct timeval *timeout);
#define select test_ws_select
