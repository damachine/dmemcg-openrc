// SPDX-License-Identifier: MIT
#define _GNU_SOURCE
#include "common.h"
#include "control.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/capability.h>
#include <linux/magic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

static int fail(const char *operation) {
  if (errno == ETIMEDOUT)
    fprintf(stderr, "dmemcg-run: registration timed out after five seconds\n");
  else
    fprintf(stderr, "dmemcg-run: %s: %s\n", operation, strerror(errno));
  return EXIT_FAILURE;
}

static int parse_number(const char *text, long maximum, long *value) {
  char *end;
  errno = 0;
  long number = strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0' || number < 0 ||
      number > maximum) {
    errno = EINVAL;
    return -1;
  }
  *value = number;
  return 0;
}

/* Only this root-owned service can authorize the helper's destination. */
static int check_service(int fd) {
  struct ucred peer;
  socklen_t length = sizeof(peer);
  struct sockaddr_un address = {0};
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) < 0)
    return -1;
  if (length != sizeof(peer) || peer.uid != 0 || peer.pid <= 1) {
    errno = EPERM;
    return -1;
  }
  length = sizeof(address);
  if (getpeername(fd, (struct sockaddr *)&address, &length) < 0)
    return -1;
  if (address.sun_family != AF_UNIX ||
      length < offsetof(struct sockaddr_un, sun_path) + sizeof(DMEMCG_SOCKET) ||
      memcmp(address.sun_path, DMEMCG_SOCKET, sizeof(DMEMCG_SOCKET)) != 0) {
    errno = EPERM;
    return -1;
  }
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 ||
      fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
    return -1;
  return 0;
}

static int receive_group(int fd, const struct timespec *deadline) {
  char reply[16] = {0};
  size_t offset = 0;
  int group = -1;
  for (;;) {
    if (remaining_ms(deadline) < 0)
      goto error;
    union {
      struct cmsghdr alignment;
      char bytes[CMSG_SPACE(4 * sizeof(int))];
    } control = {0};
    struct iovec vector = {.iov_base = reply + offset,
                          .iov_len = sizeof(reply) - 1 - offset};
    struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1,
                            .msg_control = control.bytes,
                            .msg_controllen = sizeof(control.bytes)};
    ssize_t length = recvmsg(fd, &message, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
    if (length < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (wait_ready(fd, POLLIN, deadline) < 0)
          goto error;
        continue;
      }
      goto error;
    }
    int invalid = (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0;
    for (struct cmsghdr *header = CMSG_FIRSTHDR(&message); header != NULL;
         header = CMSG_NXTHDR(&message, header)) {
      if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
          header->cmsg_len < CMSG_LEN(0)) {
        invalid = 1;
        continue;
      }
      size_t bytes = header->cmsg_len - CMSG_LEN(0);
      if (bytes % sizeof(int) != 0)
        invalid = 1;
      for (size_t i = 0; i < bytes / sizeof(int); i++) {
        int received;
        memcpy(&received, CMSG_DATA(header) + i * sizeof(int), sizeof(received));
        if (group >= 0) {
          close(received);
          invalid = 1;
        } else {
          group = received;
        }
      }
    }
    if (invalid) {
      errno = EPROTO;
      goto error;
    }
    offset += (size_t)length;
    if (length == 0 || memchr(reply, '\n', offset) != NULL ||
        offset == sizeof(reply) - 1)
      break;
  }
  if (strcmp(reply, "OK\n") != 0 || group < 0) {
    errno = EPROTO;
    goto error;
  }
  struct stat status;
  struct statfs filesystem;
  if (fstat(group, &status) < 0 || fstatfs(group, &filesystem) < 0)
    goto error;
  if (!S_ISDIR(status.st_mode) || status.st_uid != 0 ||
      (status.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
      filesystem.f_type != CGROUP2_SUPER_MAGIC) {
    errno = EPERM;
    goto error;
  }
  return group;
error:;
  int saved_errno = errno;
  if (group >= 0)
    close(group);
  errno = saved_errno;
  return -1;
}

static int drop_privileges(uid_t uid, gid_t gid) {
  struct __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
  struct __user_cap_data_struct data[2] = {{0}, {0}};
  /* The caller's supplementary groups are inherited, not replaced by root's. */
  if (prctl(PR_SET_KEEPCAPS, 0L, 0L, 0L, 0L) < 0 ||
      setresgid(gid, gid, gid) < 0 || setresuid(uid, uid, uid) < 0 ||
      syscall(SYS_capset, &header, data) < 0 ||
      prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0L, 0L, 0L) < 0)
    return -1;
  return 0;
}

/* Environment bytes are interpreted only after permanent privilege removal. */
static char **restore_environment(int fd, size_t size) {
  if (size == 0)
    return calloc(1, sizeof(char *));
  char *bytes = malloc(size + 1);
  if (bytes == NULL)
    return NULL;
  size_t offset = 0;
  while (offset < size) {
    ssize_t length = pread(fd, bytes + offset, size - offset, (off_t)offset);
    if (length < 0 && errno == EINTR)
      continue;
    if (length <= 0) {
      int error = length < 0 ? errno : EIO;
      free(bytes);
      errno = error;
      return NULL;
    }
    offset += (size_t)length;
  }
  bytes[size] = '\0';
  if (bytes[size - 1] != '\0') {
    free(bytes);
    errno = EINVAL;
    return NULL;
  }
  size_t count = 0;
  for (size_t i = 0; i < size; i++)
    if (bytes[i] == '\0')
      count++;
  char **entries = calloc(count + 1, sizeof(*entries));
  if (entries == NULL) {
    free(bytes);
    return NULL;
  }
  entries[0] = bytes;
  count = 1;
  for (size_t i = 0; i + 1 < size; i++)
    if (bytes[i] == '\0')
      entries[count++] = bytes + i + 1;
  /* Both allocations remain owned by the process until exec or exit. */
  return entries;
}

int main(int argc, char **argv) {
  uid_t uid = getuid();
  gid_t gid = getgid();
  if (argc < 7 || uid == 0 || geteuid() != 0) {
    errno = EPERM;
    return fail("start helper requires its installed setuid permissions");
  }
  long values[5];
  const long limits[] = {INT_MAX, INT_MAX, 7, LONG_MAX, 999999999};
  for (size_t i = 0; i < 5; i++)
    if (parse_number(argv[i + 1], limits[i], &values[i]) < 0)
      return fail("invalid start helper arguments");
  int socket_fd = (int)values[0];
  int environment = (int)values[1];
  struct timespec deadline = {.tv_sec = (time_t)values[3], .tv_nsec = values[4]};
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
    return fail("cannot read registration clock");
  if (deadline.tv_sec > now.tv_sec + 5) {
    errno = EINVAL;
    return fail("invalid registration deadline");
  }
  int remaining = remaining_ms(&deadline);
  if (remaining < 0)
    return fail("registration deadline");
  if (remaining > 5000 || socket_fd == environment) {
    errno = EINVAL;
    return fail("invalid start helper arguments");
  }
  struct stat status;
  int seals = fcntl(environment, F_GET_SEALS);
  int required = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
  long maximum = sysconf(_SC_ARG_MAX);
  if (seals < 0 || fstat(environment, &status) < 0)
    return fail("cannot inspect saved environment");
  if ((seals & required) != required || !S_ISREG(status.st_mode) ||
      status.st_size < 0 || maximum <= 0 || status.st_size > maximum) {
    errno = EINVAL;
    return fail("invalid saved environment");
  }
  if (fcntl(environment, F_SETFD, FD_CLOEXEC) < 0 || check_service(socket_fd) < 0)
    return fail("cannot verify registration service");
  int group = receive_group(socket_fd, &deadline);
  if (group < 0) {
    if (errno == EPROTO) {
      fprintf(stderr, "dmemcg-run: service rejected registration\n");
      return EXIT_FAILURE;
    }
    return fail("cannot receive protected cgroup");
  }
  close(socket_fd);

  /* Zero means the writing process itself, never an external numeric PID. */
  int procs = openat(group, "cgroup.procs", O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
  if (procs < 0)
    return fail("cannot open protected cgroup membership");
  close(group);
  ssize_t written;
  do {
    if (remaining_ms(&deadline) < 0)
      return fail("registration deadline");
    written = write(procs, "0\n", 2);
  } while (written < 0 && errno == EINTR);
  if (written != 2) {
    if (written >= 0)
      errno = EIO;
    return fail("cannot join protected cgroup");
  }
  if (close(procs) < 0)
    return fail("cannot close protected cgroup membership");
  if (drop_privileges(uid, gid) < 0)
    return fail("cannot permanently drop start privileges");
  char **saved = restore_environment(environment, (size_t)status.st_size);
  close(environment);
  if (saved == NULL)
    return fail("cannot restore command environment");
  for (int i = 0; i < 3; i++)
    if (((unsigned long)values[2] & (1UL << (unsigned int)i)) == 0)
      close(i);
  environ = saved;
  execvp(argv[6], &argv[6]);
  fprintf(stderr, "dmemcg-run: cannot execute %s: %s\n", argv[6], strerror(errno));
  return 127;
}
