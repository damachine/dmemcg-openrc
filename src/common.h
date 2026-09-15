// SPDX-License-Identifier: MIT
#ifndef DMEMCG_OPENRC_COMMON_H
#define DMEMCG_OPENRC_COMMON_H

#define DMEMCG_DEFAULT_CGROUP "/sys/fs/cgroup/openrc.dmem-run"
#define DMEMCG_RUNTIME_DIR "/run/dmemcg-openrc"
#define DMEMCG_SOCKET DMEMCG_RUNTIME_DIR "/control.sock"
#define DMEMCG_SOCKET_GROUP "video"
#define DMEMCG_MAX_LINE 4096
#define DMEMCG_MAX_REGIONS 32

#endif
