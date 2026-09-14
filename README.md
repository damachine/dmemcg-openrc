# dmemcg-openrc

`dmemcg-openrc` gives explicitly launched games DMEM cgroup protection on an
OpenRC system. It does not depend on systemd and does not patch Plasma.

The root daemon moves each `dmem-run` process into a shared game cgroup. The
Unix socket uses Linux `SO_PEERCRED`, and each client can register only its own
PID. The client is moved before it replaces itself with the game, so VRAM
allocations are charged to the correct cgroup from program start.

## Requirements

- Unified cgroup v2 with the `dmem` controller enabled
- A GPU region listed in `/sys/fs/cgroup/dmem.capacity`
- OpenRC's `cgroups` service

## Build and install

```sh
make
make check
sudo make install
```

The Gentoo ebuild in `packaging/gentoo` is intended for a local overlay.

## Use

Enable service:

```sh
rc-update add dmemcg-openrc default
rc-service dmemcg-openrc start
```

Place `dmem-run` directly before `%command%` in a Steam game's launch options:

```text
gamemoderun dmem-run %command%
```

Existing preparation and cleanup commands can remain around this expression.
Successful operation produces no terminal output.

For a running game, verify the assignment with:

```sh
cat /proc/$(pgrep -n -f 'game executable')/cgroup
cat /sys/fs/cgroup/openrc.dmem-games/dmem.low
```

```sh
watch -n1 'cat /sys/fs/cgroup/openrc.dmem-games/dmem.{low,current,peak}'
```

## Related work

This is an independent OpenRC implementation inspired by the
[SteamOS dmemcg booster](https://gitlab.steamos.cloud/holo/dmemcg-booster) and
the foreground-booster work in
[Jovian's kcgroups fork](https://github.com/Jovian-Experiments/kcgroups).
