# dmemcg-openrc

A tiny VRAM protection broker for OpenRC that uses cgroup v2.
This helps prevent VRAM reclaim under memory pressure.
Nothing else needs to run in the background: no focus agent, polling service, or desktop integration.

## Requirements

- Unified cgroup v2 with DMEM enabled (Linux 6.15+; `dmem.peak` requires 7.3+)
- A GPU driver that registers at least one region in `/sys/fs/cgroup/dmem.capacity`
- OpenRC's `cgroups` service

Tested with Linux 7.3-rc3 and NVIDIA 615.71.09.

## Build and install

```sh
make
sudo make install
```

The Gentoo ebuild in `packaging/gentoo` is intended for a local overlay.

To remove a manual installation:

```sh
sudo rc-service dmemcg-openrc stop
sudo rc-update del dmemcg-openrc default
sudo make uninstall
```

## Usage

Enable and start the service:

```sh
rc-update add dmemcg-openrc default
rc-service dmemcg-openrc start
```

Run any command through `dmem-run`. For example, place it directly before `%command%` in a Steam game's launch options:

```text
gamemoderun dmem-run %command%
```

Or run a command directly:

```sh
dmem-run blender
```

For a running process, verify the assignment with:

```sh
xargs -r ps -fp < /sys/fs/cgroup/openrc.dmem-run/cgroup.procs
```

```sh
watch -n1 'cat /sys/fs/cgroup/openrc.dmem-run/dmem.{low,current,peak}'
# dmem.peak requires Linux 7.3+
```

## Inspiration

This is an independent OpenRC implementation inspired by the [SteamOS dmemcg booster](https://gitlab.steamos.cloud/holo/dmemcg-booster) and the foreground-booster work in [Jovian's kcgroups fork](https://github.com/Jovian-Experiments/kcgroups).
