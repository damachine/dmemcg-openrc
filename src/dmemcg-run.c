// SPDX-License-Identifier: MIT
#define _GNU_SOURCE
#include "common.h"
#include "control.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static void usage(FILE *stream) {
  fprintf(stream, "Usage: dmemcg-run COMMAND [ARGUMENT ...]\n");
}

static int connect_control(const char *path, const struct timespec *deadline) {
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  int fd;

  if (strlen(path) >= sizeof(address.sun_path)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(address.sun_path, path, strlen(path) + 1);
  fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0)
    return -1;
  for (;;) {
    if (remaining_ms(deadline) < 0)
      break;
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) == 0 ||
        errno == EISCONN)
      return fd;
    if (errno == EINTR)
      continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      /* AF_UNIX returns EAGAIN for a full queue, without a pending connect. */
      int timeout = remaining_ms(deadline);
      if (timeout < 0)
        break;
      if (poll(NULL, 0, timeout < 10 ? timeout : 10) < 0 && errno != EINTR)
        break;
      continue;
    }
    if (errno == EINPROGRESS) {
      int error = 0;
      socklen_t length = sizeof(error);
      if (wait_ready(fd, POLLOUT, deadline) < 0 ||
          getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0)
        break;
      if (error == 0)
        return fd;
      errno = error;
    }
    break;
  }
  int saved_errno = errno;
  close(fd);
  errno = saved_errno;
  return -1;
}

static int send_request(int fd, const struct timespec *deadline) {
  const char request[] = "SPAWN\n";
  size_t offset = 0;
  while (offset < sizeof(request) - 1) {
    if (remaining_ms(deadline) < 0)
      return -1;
    ssize_t written =
        send(fd, request + offset, sizeof(request) - 1 - offset, MSG_NOSIGNAL);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (wait_ready(fd, POLLOUT, deadline) < 0)
          return -1;
        continue;
      }
      return -1;
    }
    if (written == 0) {
      errno = EPIPE;
      return -1;
    }
    offset += (size_t)written;
  }
  return 0;
}

/* The first reply byte follows identity verification. Keep SCM_RIGHTS queued. */
static ssize_t read_reply(int fd, char *reply, size_t size,
                          const struct timespec *deadline) {
  for (;;) {
    if (remaining_ms(deadline) < 0)
      return -1;
    ssize_t length = recv(fd, reply, size - 1, MSG_PEEK);
    if (length < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (wait_ready(fd, POLLIN, deadline) < 0)
          return -1;
        continue;
      }
      return -1;
    }
    reply[length] = '\0';
    return length;
  }
}

static int environment_fd(void) {
  int fd = memfd_create("dmemcg-run environment", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd < 0)
    return -1;
  for (char **entry = environ; *entry != NULL; entry++) {
    size_t length = strlen(*entry) + 1;
    size_t offset = 0;
    while (offset < length) {
      ssize_t written = write(fd, *entry + offset, length - offset);
      if (written < 0 && errno == EINTR)
        continue;
      if (written <= 0) {
        int error = written < 0 ? errno : EIO;
        close(fd);
        errno = error;
        return -1;
      }
      offset += (size_t)written;
    }
  }
  if (fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK |
                            F_SEAL_SEAL) < 0) {
    int error = errno;
    close(fd);
    errno = error;
    return -1;
  }
  return fd;
}

static void registration_error(const char *operation, int error) {
  if (error == ETIMEDOUT)
    fprintf(stderr, "dmemcg-run: registration timed out after five seconds\n");
  else
    fprintf(stderr, "dmemcg-run: %s: %s\n", operation, strerror(error));
}

int main(int argc, char **argv) {
  char reply[16] = {0};
  struct timespec deadline;
  ssize_t length;
  int fd;
  unsigned int stdio = 0;
  for (int i = 0; i < 3; i++) {
    int flags = fcntl(i, F_GETFD);
    if (flags >= 0 && (flags & FD_CLOEXEC) == 0)
      stdio |= 1U << (unsigned int)i;
  }

  if (argc < 2) {
    usage(stderr);
    return EXIT_FAILURE;
  }
  if (clock_gettime(CLOCK_MONOTONIC, &deadline) < 0) {
    registration_error("cannot read registration clock", errno);
    return EXIT_FAILURE;
  }
  deadline.tv_sec += 5;
  fd = connect_control(DMEMCG_SOCKET, &deadline);
  if (fd < 0) {
    registration_error("cannot contact dmemcg-openrcd", errno);
    return EXIT_FAILURE;
  }
  if (send_request(fd, &deadline) < 0) {
    registration_error("cannot send registration", errno);
    close(fd);
    return EXIT_FAILURE;
  }
  length = read_reply(fd, reply, sizeof(reply), &deadline);
  int reply_errno = errno;
  if (length < 0) {
    registration_error("cannot read service reply", reply_errno);
    close(fd);
    return EXIT_FAILURE;
  }
  if (length == 0 || reply[0] != 'O') {
    fprintf(stderr, "dmemcg-run: service rejected registration\n");
    close(fd);
    return EXIT_FAILURE;
  }
  if (remaining_ms(&deadline) < 0) {
    registration_error("cannot complete registration", errno);
    close(fd);
    return EXIT_FAILURE;
  }
  int saved_environment = environment_fd();
  if (saved_environment < 0 || fcntl(fd, F_SETFD, 0) < 0 ||
      fcntl(saved_environment, F_SETFD, 0) < 0) {
    registration_error("cannot prepare start helper", errno);
    close(fd);
    if (saved_environment >= 0)
      close(saved_environment);
    return EXIT_FAILURE;
  }
  char values[5][32];
  snprintf(values[0], sizeof(values[0]), "%d", fd);
  snprintf(values[1], sizeof(values[1]), "%d", saved_environment);
  snprintf(values[2], sizeof(values[2]), "%u", stdio);
  snprintf(values[3], sizeof(values[3]), "%lld", (long long)deadline.tv_sec);
  snprintf(values[4], sizeof(values[4]), "%ld", deadline.tv_nsec);
  char **arguments = calloc((size_t)argc + 6, sizeof(*arguments));
  if (arguments == NULL) {
    registration_error("cannot prepare start helper", errno);
    close(fd);
    close(saved_environment);
    return EXIT_FAILURE;
  }
  arguments[0] = (char *)DMEMCG_HELPER;
  for (size_t i = 0; i < 5; i++)
    arguments[i + 1] = values[i];
  for (int i = 1; i < argc; i++)
    arguments[i + 5] = argv[i];
  char *empty_environment[] = {NULL};
  execve(DMEMCG_HELPER, arguments, empty_environment);
  registration_error("cannot execute start helper", errno);
  free(arguments);
  close(fd);
  close(saved_environment);
  return 127;
}
