// SPDX-License-Identifier: MIT
#define _GNU_SOURCE
#include "common.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
  if (close(fd) < 0 && result == 0)
    result = -1;
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

static int write_regions(const char *path, const struct region_list *regions,
                         bool enabled) {
  int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0)
    return -1;
  for (size_t i = 0; i < regions->count; i++) {
    const char *line = regions->lines[i];
    char disabled[DMEMCG_MAX_LINE];
    if (!enabled) {
      char region[DMEMCG_MAX_LINE];
      int length;
      if (sscanf(line, "%4095s", region) != 1) {
        close(fd);
        errno = EINVAL;
        return -1;
      }
      length = snprintf(disabled, sizeof(disabled), "%s 0\n", region);
      if (length < 0 || (size_t)length >= sizeof(disabled)) {
        close(fd);
        errno = EOVERFLOW;
        return -1;
      }
      line = disabled;
    }
    if (write_all(fd, line, strlen(line)) < 0) {
      int saved_errno = errno;
      close(fd);
      errno = saved_errno;
      return -1;
    }
  }
  return close(fd);
}

static int prepare_base(const char *base, const struct region_list *regions) {
  char path[PATH_MAX];
  if (mkdir(base, 0755) < 0 && errno != EEXIST)
    return -1;
  if (join_path(path, sizeof(path), base, "cgroup.subtree_control") < 0 ||
      write_path(path, "-dmem\n") < 0)
    return -1;
  if (join_path(path, sizeof(path), base, "dmem.low") < 0 ||
      write_regions(path, regions, true) < 0)
    return -1;
  return 0;
}

static int register_peer(const char *base, const struct ucred *credentials) {
  char file[PATH_MAX];
  char pid[32];
  int length;
  if (credentials->pid <= 1 || credentials->uid == 0) {
    errno = EPERM;
    return -1;
  }
  if (join_path(file, sizeof(file), base, "cgroup.procs") < 0)
    return -1;
  length = snprintf(pid, sizeof(pid), "%ld\n", (long)credentials->pid);
  if (length < 0 || (size_t)length >= sizeof(pid) || write_path(file, pid) < 0)
    return -1;
  return 0;
}

static void handle_client(int client, const char *base) {
  static const char expected[] = "REGISTER\n";
  struct ucred credentials;
  struct timeval timeout = {.tv_usec = 100000};
  socklen_t length = sizeof(credentials);
  char request[sizeof(expected) - 1];
  ssize_t received;
  if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &credentials, &length) < 0)
    return;
  if (setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) <
      0)
    return;
  received = recv(client, request, sizeof(request), MSG_WAITALL);
  if (received != (ssize_t)sizeof(request) ||
      memcmp(request, expected, sizeof(request)) != 0) {
    (void)write_all(client, "ERROR\n", 6);
    return;
  }
  if (register_peer(base, &credentials) < 0) {
    syslog(LOG_WARNING, "cannot register uid %lu pid %ld: %s",
           (unsigned long)credentials.uid, (long)credentials.pid,
           strerror(errno));
    (void)write_all(client, "ERROR\n", 6);
    return;
  }
  (void)write_all(client, "OK\n", 3);
}

static int create_listener(const char *socket_path) {
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  char directory[PATH_MAX];
  char *slash;
  int listener;
  if (strlen(socket_path) >= sizeof(address.sun_path)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  if (snprintf(directory, sizeof(directory), "%s", socket_path) >=
      (int)sizeof(directory)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  slash = strrchr(directory, '/');
  if (slash == NULL || slash == directory) {
    errno = EINVAL;
    return -1;
  }
  *slash = '\0';
  if (mkdir(directory, 0755) < 0 && errno != EEXIST)
    return -1;
  listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listener < 0)
    return -1;
  memcpy(address.sun_path, socket_path, strlen(socket_path) + 1);
  (void)unlink(socket_path);
  if (bind(listener, (const struct sockaddr *)&address, sizeof(address)) < 0 ||
      chmod(socket_path, 0666) < 0 || listen(listener, 16) < 0) {
    int saved_errno = errno;
    close(listener);
    (void)unlink(socket_path);
    errno = saved_errno;
    return -1;
  }
  return listener;
}

static void remove_socket_directory(const char *socket_path) {
  char directory[PATH_MAX];
  char *slash;
  int length = snprintf(directory, sizeof(directory), "%s", socket_path);
  if (length < 0 || (size_t)length >= sizeof(directory))
    return;
  slash = strrchr(directory, '/');
  if (slash == NULL || slash == directory)
    return;
  *slash = '\0';
  (void)rmdir(directory);
}

static void usage(FILE *stream) {
  fprintf(stream, "Usage: dmemcg-openrcd [--cgroup PATH] [--socket PATH]\n");
}

int main(int argc, char **argv) {
  const char *base = DMEMCG_DEFAULT_CGROUP;
  const char *socket_path = DMEMCG_DEFAULT_SOCKET;
  struct region_list regions = {0};
  struct sigaction action = {.sa_handler = stop_running};
  bool base_touched = false;
  bool socket_touched = false;
  int exit_status = EXIT_FAILURE;
  int listener = -1;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--cgroup") == 0 && i + 1 < argc)
      base = argv[++i];
    else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc)
      socket_path = argv[++i];
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
  listener = create_listener(socket_path);
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
  if (socket_touched) {
    (void)unlink(socket_path);
    remove_socket_directory(socket_path);
  }
  if (base_touched) {
    char low[PATH_MAX];
    if (join_path(low, sizeof(low), base, "dmem.low") == 0)
      (void)write_regions(low, &regions, false);
    (void)rmdir(base);
  }
  closelog();
  return exit_status;
}
