// SPDX-License-Identifier: MIT
#ifndef DMEMCG_CONTROL_H
#define DMEMCG_CONTROL_H
#include <errno.h>
#include <poll.h>
#include <time.h>

/* Every operation uses the same deadline, including retries after EINTR. */
static int remaining_ms(const struct timespec *deadline) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
    return -1;
  long long remaining = (long long)(deadline->tv_sec - now.tv_sec) * 1000000000LL +
                        deadline->tv_nsec - now.tv_nsec;
  if (remaining <= 0) {
    errno = ETIMEDOUT;
    return -1;
  }
  return (int)((remaining + 999999LL) / 1000000LL);
}

static int wait_ready(int fd, short events, const struct timespec *deadline) {
  for (;;) {
    int timeout = remaining_ms(deadline);
    if (timeout < 0)
      return -1;
    struct pollfd descriptor = {.fd = fd, .events = events};
    int result = poll(&descriptor, 1, timeout);
    if (result < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (result == 0)
      continue;
    if ((descriptor.revents & POLLNVAL) != 0) {
      errno = EBADF;
      return -1;
    }
    if ((descriptor.revents & (events | POLLERR | POLLHUP)) != 0)
      return 0;
  }
}

#endif
