// SPDX-License-Identifier: MIT
#define _GNU_SOURCE
#include "common.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void usage(FILE *stream) {
  fprintf(stream, "Usage: dmem-run COMMAND [ARGUMENT ...]\n");
}

static int connect_control(const char *path) {
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  int fd;

  if (strlen(path) >= sizeof(address.sun_path)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  memcpy(address.sun_path, path, strlen(path) + 1);
  fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;
  if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) < 0) {
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return -1;
  }
  return fd;
}

static int send_request(int fd) {
  const char request[] = "REGISTER\n";
  size_t offset = 0;
  while (offset < sizeof(request) - 1) {
    ssize_t written =
        send(fd, request + offset, sizeof(request) - 1 - offset, MSG_NOSIGNAL);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    offset += (size_t)written;
  }
  return 0;
}

static ssize_t read_reply(int fd, char *reply, size_t size) {
  size_t offset = 0;
  while (offset + 1 < size) {
    ssize_t length = read(fd, reply + offset, size - offset - 1);
    if (length < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    if (length == 0)
      break;
    offset += (size_t)length;
    if (memchr(reply, '\n', offset) != NULL)
      break;
  }
  reply[offset] = '\0';
  return (ssize_t)offset;
}

int main(int argc, char **argv) {
  char reply[16] = {0};
  ssize_t length;
  int fd;

  if (argc < 2) {
    usage(stderr);
    return EXIT_FAILURE;
  }
  fd = connect_control(DMEMCG_DEFAULT_SOCKET);
  if (fd < 0) {
    fprintf(stderr, "dmem-run: cannot contact dmemcg-openrcd: %s\n",
            strerror(errno));
    return EXIT_FAILURE;
  }
  if (send_request(fd) < 0) {
    fprintf(stderr, "dmem-run: registration failed: %s\n", strerror(errno));
    close(fd);
    return EXIT_FAILURE;
  }
  length = read_reply(fd, reply, sizeof(reply));
  close(fd);
  if (length < 0 || strncmp(reply, "OK\n", 3) != 0) {
    fprintf(stderr, "dmem-run: service rejected registration\n");
    return EXIT_FAILURE;
  }
  execvp(argv[1], &argv[1]);
  fprintf(stderr, "dmem-run: cannot execute %s: %s\n", argv[1],
          strerror(errno));
  return 127;
}
