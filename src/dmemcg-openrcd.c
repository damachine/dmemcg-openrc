// SPDX-License-Identifier: MIT
#define _GNU_SOURCE
#include "common.h"
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <syslog.h>
#include <unistd.h>

struct region_list {
  char lines[DMEMCG_MAX_REGIONS][DMEMCG_MAX_LINE];
  size_t count;
};

static volatile sig_atomic_t running = 1;

static void stop_running(int signal_number) {
  (void)signal_number;
  running = 0;
}

static int write_all(int fd, const char *buffer, size_t length) {
  while (length > 0) {
    ssize_t written = write(fd, buffer, length);
    if (written < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    buffer += written;
    length -= (size_t)written;
  }
  return 0;
}

static int write_path(const char *path, const char *value) {
  int fd = open(path, O_WRONLY | O_CLOEXEC);
  int result;
  if (fd < 0)
    return -1;
  result = write_all(fd, value, strlen(value));
  int saved_errno = errno;
  if (close(fd) < 0 && result == 0)
    return -1;
  errno = saved_errno;
  return result;
}

static int join_path(char *output, size_t size, const char *left,
                     const char *right) {
  int length = snprintf(output, size, "%s/%s", left, right);
  if (length < 0 || (size_t)length >= size) {
    errno = ENAMETOOLONG;
    return -1;
  }
  return 0;
}

static int load_regions(struct region_list *regions) {
  FILE *stream = fopen("/sys/fs/cgroup/dmem.capacity", "re");
  char line[DMEMCG_MAX_LINE];
  if (stream == NULL)
    return -1;
  while (fgets(line, sizeof(line), stream) != NULL) {
    char region[DMEMCG_MAX_LINE];
    unsigned long long capacity;
    char extra;
    int length;
    if (sscanf(line, "%4095s %llu %c", region, &capacity, &extra) != 2)
      continue;
    if (regions->count >= DMEMCG_MAX_REGIONS) {
      errno = E2BIG;
      fclose(stream);
      return -1;
    }
    length = snprintf(regions->lines[regions->count], DMEMCG_MAX_LINE,
                      "%s %llu\n", region, capacity);
    if (length < 0 || length >= DMEMCG_MAX_LINE) {
      errno = EOVERFLOW;
      fclose(stream);
      return -1;
    }
    regions->count++;
  }
  {
    int read_error = ferror(stream);
    int close_error = fclose(stream);
    if (read_error != 0 || close_error != 0)
      return -1;
  }
  if (regions->count == 0) {
    errno = ENODEV;
    return -1;
  }
  return 0;
}

static int write_regions(const char *path, const struct region_list *regions) {
  int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0)
    return -1;
  for (size_t i = 0; i < regions->count; i++) {
    const char *line = regions->lines[i];
    if (write_all(fd, line, strlen(line)) < 0) {
      int saved_errno = errno;
      close(fd);
      errno = saved_errno;
      return -1;
    }
  }
  return close(fd);
}

static int reset_regions(const char *path, const struct region_list *regions) {
  int result = 0;
  for (size_t i = 0; i < regions->count; i++) {
    char region[DMEMCG_MAX_LINE];
    char value[DMEMCG_MAX_LINE];
    int length;
    if (sscanf(regions->lines[i], "%4095s", region) != 1) {
      syslog(LOG_WARNING, "cannot reset %s: invalid region at index %zu", path, i);
      result = -1;
      continue;
    }
    length = snprintf(value, sizeof(value), "%s 0\n", region);
    if (length < 0 || (size_t)length >= sizeof(value)) {
      syslog(LOG_WARNING, "cannot reset %s region %s: value too long", path, region);
      result = -1;
      continue;
    }
    if (write_path(path, value) < 0) {
      syslog(LOG_WARNING, "cannot reset %s region %s: %s", path, region,
             strerror(errno));
      result = -1;
    }
  }
  return result;
}

static int cleanup_cgroup(const char *base, const struct region_list *regions) {
  char low[PATH_MAX];
  int result = 0;
  if (join_path(low, sizeof(low), base, "dmem.low") < 0) {
    syslog(LOG_WARNING, "cannot find dmem.low in %s: %s", base, strerror(errno));
    result = -1;
  } else if (reset_regions(low, regions) < 0) {
    result = -1;
  }
  if (rmdir(base) < 0 && errno != ENOENT) {
    int error = errno;
    if (error == EBUSY || error == ENOTEMPTY)
      syslog(LOG_NOTICE, "cgroup %s remains because it is not empty", base);
    else {
      syslog(LOG_WARNING, "cannot remove cgroup %s: %s", base, strerror(error));
      result = -1;
    }
  }
  return result;
}

static int prepare_base(const char *base, const struct region_list *regions) {
  char path[PATH_MAX];
  if (mkdir(base, 0755) < 0 && errno != EEXIST)
    return -1;
  if (join_path(path, sizeof(path), base, "cgroup.subtree_control") < 0 ||
      write_path(path, "-dmem\n") < 0)
    return -1;
  if (join_path(path, sizeof(path), base, "dmem.low") < 0 ||
      write_regions(path, regions) < 0)
    return -1;
  return 0;
}

static int peer_alive(int pidfd) {
  struct pollfd descriptor = {.fd = pidfd, .events = POLLIN};
  int result;
  do {
    result = poll(&descriptor, 1, 0);
  } while (result < 0 && errno == EINTR);
  if (result < 0)
    return -1;
  if (result != 0) {
    errno = (descriptor.revents & POLLNVAL) != 0 ? EBADF : ESRCH;
    return -1;
  }
  return 0;
}

static int check_peer_identity(const struct ucred *credentials, int pidfd) {
  char path[64];
  char line[DMEMCG_MAX_LINE];
  bool found_pid = false;
  bool found_uid = false;
  int length;
  int directory;
  int fd;
  int error = 0;
  FILE *stream;

  if (credentials->pid <= 1 || credentials->uid == 0) {
    errno = EPERM;
    return -1;
  }
  if (peer_alive(pidfd) < 0)
    return -1;
  length = snprintf(path, sizeof(path), "/proc/%ld", (long)credentials->pid);
  if (length < 0 || (size_t)length >= sizeof(path)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (directory < 0)
    return -1;
  fd = openat(directory, "status", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  error = errno;
  close(directory);
  if (fd < 0) {
    errno = error;
    return -1;
  }
  stream = fdopen(fd, "r");
  if (stream == NULL) {
    error = errno;
    close(fd);
    errno = error;
    return -1;
  }
  error = 0;
  while (fgets(line, sizeof(line), stream) != NULL) {
    char extra;
    if (strncmp(line, "Pid:", 4) == 0) {
      long pid;
      if (sscanf(line + 4, "%ld %c", &pid, &extra) != 1 ||
          pid != (long)credentials->pid) {
        error = EPERM;
        break;
      }
      found_pid = true;
    } else if (strncmp(line, "Uid:", 4) == 0) {
      unsigned long real, effective, saved, filesystem;
      if (sscanf(line + 4, "%lu %lu %lu %lu %c", &real, &effective, &saved,
                 &filesystem, &extra) != 4 ||
          real != (unsigned long)credentials->uid ||
          effective != (unsigned long)credentials->uid ||
          saved != (unsigned long)credentials->uid ||
          filesystem != (unsigned long)credentials->uid) {
        error = EPERM;
        break;
      }
      found_uid = true;
    }
  }
  if (ferror(stream) != 0 && error == 0)
    error = errno != 0 ? errno : EIO;
  if (fclose(stream) < 0 && error == 0)
    error = errno;
  if (error == 0 && (!found_pid || !found_uid))
    error = EPERM;
  if (error != 0) {
    errno = error;
    return -1;
  }
  /* A pidfd from the socket identifies the connector, even if its PID is reused. */
  return peer_alive(pidfd);
}

static int register_peer(const char *base, const struct ucred *credentials,
                         int pidfd) {
  if (check_peer_identity(credentials, pidfd) < 0)
    return -1;
  int fd = open(base, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0)
    return -1;
  if (peer_alive(pidfd) < 0) {
    int error = errno;
    close(fd);
    errno = error;
    return -1;
  }
  return fd;
}

static int send_group(int client, int group) {
  char reply[] = "OK\n";
  struct iovec vector = {.iov_base = reply, .iov_len = sizeof(reply) - 1};
  union {
    struct cmsghdr alignment;
    char bytes[CMSG_SPACE(sizeof(group))];
  } control = {0};
  struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1,
                          .msg_control = control.bytes,
                          .msg_controllen = sizeof(control.bytes)};
  struct cmsghdr *header = CMSG_FIRSTHDR(&message);
  header->cmsg_level = SOL_SOCKET;
  header->cmsg_type = SCM_RIGHTS;
  header->cmsg_len = CMSG_LEN(sizeof(group));
  memcpy(CMSG_DATA(header), &group, sizeof(group));
  ssize_t written;
  do {
    written = sendmsg(client, &message, MSG_NOSIGNAL);
  } while (written < 0 && errno == EINTR);
  if (written <= 0)
    return -1;
  return write_all(client, reply + written, sizeof(reply) - 1 - (size_t)written);
}

static void handle_client(int client, const char *base) {
  static const char expected[] = "SPAWN\n";
  struct ucred credentials = {0};
  struct timeval timeout = {.tv_usec = 100000};
  socklen_t length = sizeof(credentials);
  char request[sizeof(expected) - 1];
  ssize_t received;
  int pidfd = -1;
  if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &credentials, &length) < 0)
    goto rejected;
  if (length != sizeof(credentials)) {
    errno = EPROTO;
    goto rejected;
  }
#ifdef SO_PEERPIDFD
  length = sizeof(pidfd);
  if (getsockopt(client, SOL_SOCKET, SO_PEERPIDFD, &pidfd, &length) < 0)
    goto rejected;
  if (length != sizeof(pidfd) || pidfd < 0) {
    errno = EPROTO;
    goto rejected;
  }
#else
  errno = EOPNOTSUPP;
  goto rejected;
#endif
  if (peer_alive(pidfd) < 0 ||
      setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
    goto rejected;
  received = recv(client, request, sizeof(request), MSG_WAITALL);
  if (received < 0)
    goto rejected;
  if (received != (ssize_t)sizeof(request) ||
      memcmp(request, expected, sizeof(request)) != 0) {
    errno = EPROTO;
    goto rejected;
  }
  int group = register_peer(base, &credentials, pidfd);
  if (group < 0)
    goto rejected;
  if (send_group(client, group) < 0)
    syslog(LOG_WARNING, "cannot send cgroup handle to uid %lu pid %ld: %s",
           (unsigned long)credentials.uid, (long)credentials.pid, strerror(errno));
  close(group);
  close(pidfd);
  return;
rejected:
  syslog(LOG_WARNING, "cannot register uid %lu pid %ld: %s",
         (unsigned long)credentials.uid, (long)credentials.pid, strerror(errno));
  (void)write_all(client, "ERROR\n", 6);
  if (pidfd >= 0)
    close(pidfd);
}

static int acquire_instance_lock(void) {
  int fd;
  if (mkdir(DMEMCG_RUNTIME_DIR, 0755) < 0 && errno != EEXIST)
    return -1;
  fd = open(DMEMCG_LOCK, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0)
    return -1;
  if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return -1;
  }
  return fd;
}

static int create_listener(gid_t socket_gid) {
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  int listener;
  if (sizeof(DMEMCG_SOCKET) > sizeof(address.sun_path)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listener < 0)
    return -1;
  memcpy(address.sun_path, DMEMCG_SOCKET, sizeof(DMEMCG_SOCKET));
  (void)unlink(DMEMCG_SOCKET);
  if (bind(listener, (const struct sockaddr *)&address, sizeof(address)) < 0 ||
      chown(DMEMCG_SOCKET, 0, socket_gid) < 0 ||
      chmod(DMEMCG_SOCKET, 0660) < 0 ||
      listen(listener, 16) < 0) {
    int saved_errno = errno;
    close(listener);
    (void)unlink(DMEMCG_SOCKET);
    errno = saved_errno;
    return -1;
  }
  return listener;
}

static void usage(FILE *stream) {
  fprintf(stream, "Usage: dmemcg-openrcd [--cgroup PATH]\n");
}

int main(int argc, char **argv) {
  const char *base = DMEMCG_DEFAULT_CGROUP;
  struct region_list regions = {0};
  struct sigaction action = {.sa_handler = stop_running};
  bool base_touched = false;
  bool socket_touched = false;
  int exit_status = EXIT_FAILURE;
  int listener = -1;
  int lock_fd = -1;
  gid_t socket_gid = 0;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--cgroup") == 0 && i + 1 < argc)
      base = argv[++i];
    else {
      usage(stderr);
      return EXIT_FAILURE;
    }
  }
  if (geteuid() != 0) {
    fprintf(stderr, "dmemcg-openrcd must run as root\n");
    return EXIT_FAILURE;
  }
  openlog("dmemcg-openrcd", LOG_PID, LOG_DAEMON);
  {
    const struct group *group = getgrnam(DMEMCG_SOCKET_GROUP);
    if (group == NULL) {
      syslog(LOG_ERR, "required group %s does not exist",
             DMEMCG_SOCKET_GROUP);
      goto cleanup;
    }
    socket_gid = group->gr_gid;
  }
  lock_fd = acquire_instance_lock();
  if (lock_fd < 0) {
    syslog(LOG_ERR, "cannot acquire instance lock: %s", strerror(errno));
    goto cleanup;
  }
  if (load_regions(&regions) < 0) {
    syslog(LOG_ERR, "cannot prepare dmem cgroups: %s", strerror(errno));
    goto cleanup;
  }
  base_touched = true;
  if (prepare_base(base, &regions) < 0) {
    syslog(LOG_ERR, "cannot prepare dmem cgroups: %s", strerror(errno));
    goto cleanup;
  }
  socket_touched = true;
  listener = create_listener(socket_gid);
  if (listener < 0) {
    syslog(LOG_ERR, "cannot create control socket: %s", strerror(errno));
    goto cleanup;
  }
  sigemptyset(&action.sa_mask);
  (void)sigaction(SIGINT, &action, NULL);
  (void)sigaction(SIGTERM, &action, NULL);
  (void)signal(SIGPIPE, SIG_IGN);
  exit_status = EXIT_SUCCESS;
  while (running != 0) {
    struct pollfd descriptor = {.fd = listener, .events = POLLIN};
    int result = poll(&descriptor, 1, 1000);
    if (result < 0) {
      if (errno == EINTR)
        continue;
      syslog(LOG_ERR, "poll failed: %s", strerror(errno));
      exit_status = EXIT_FAILURE;
      break;
    }
    if (result > 0 &&
        (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      syslog(LOG_ERR, "control socket poll failed: events %#x",
             (unsigned int)descriptor.revents);
      exit_status = EXIT_FAILURE;
      break;
    }
    if (result > 0 && (descriptor.revents & POLLIN) != 0) {
      int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
      if (client >= 0) {
        handle_client(client, base);
        close(client);
      }
    }
  }
cleanup:
  if (listener >= 0)
    close(listener);
  if (socket_touched && unlink(DMEMCG_SOCKET) < 0 && errno != ENOENT) {
    syslog(LOG_WARNING, "cannot remove socket %s: %s", DMEMCG_SOCKET,
           strerror(errno));
    exit_status = EXIT_FAILURE;
  }
  if (base_touched && cleanup_cgroup(base, &regions) < 0)
    exit_status = EXIT_FAILURE;
  if (lock_fd >= 0)
    close(lock_fd);
  closelog();
  return exit_status;
}
