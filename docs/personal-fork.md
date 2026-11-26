# Personal fork: per-network streams, slow-hands alert, hand profile, and small fixes

The `personal` branch is based on upstream `experimental` tag `v0.3.0-exp.30` (`eb7aec97`). Upstream remains [Frametop/frametop](https://github.com/Frametop/frametop); this fork is [LeeroyDing/frametop, branch `personal`](https://github.com/LeeroyDing/frametop/tree/personal).

Previously carried features now maintained upstream (and dropped from the fork diff):

- Visible recenter countdown, geometry-only reset, and controls guide ([PR #67](https://github.com/Frametop/frametop/pull/67))
- KWin 6.6 nested output scaling and layout-scale preservation ([PR #86](https://github.com/Frametop/frametop/pull/86))
- Apple High Performance Screen Sharing backend (`ft-appless`, `ft-apple-vdisplay.swift`, `kb_layout`, `keymap`, 32:9 ultra-wide, H.264 bottom-row crop, and sleep/wake auto-reconnect; [PR #109](https://github.com/Frametop/frametop/pull/109))

## Carried fork features

### Per-network stream settings, direct-address handoff, and `view_only`

A remote display can carry its own `fps` and `bitrate` for the `direct` (Ethernet/dongle) and `network` routes in `~/.config/frametop-layout.json`:

```sh
layout/ft-layout remote set my-display direct.bitrate=35000 network.bitrate=4000
layout/ft-layout remote set my-display direct.fps=60
layout/ft-layout remote set my-display direct.bitrate=   # reset to display default
layout/ft-layout remote set my-display view_only=on|off|none   # ft-appless live input guard
layout/ft-layout remote list
```

- `stream/route_settings.h`: parses per-route `fps` (1–240) and `bitrate` (0–500000 kbit/s), `HostRouteOf`, `RerouteWatch` (two consecutive probes 10 s apart), and `view_only` merged from host and display entries.
- `stream/ft-stream.cpp` and `stream/ft-appless.cpp`: apply route settings once the route is chosen and run `WatchDirect` while on the host's own address with `route=auto`, restarting cleanly onto a direct address when it comes up. `ft-appless` also supports hot-reloaded `view_only=on|off` (releasing held keys/buttons when toggled on) and `--view-only`.
- `layout/ft_layout.py` and `layout/ft_remotekeys.py`: manage `direct.*`, `network.*`, and `view_only=on|off|none`, restarting a running stream when route settings change.
- Offline tests: `stream/test/route-settings-test.cpp` (`scripts/test-route-settings.sh`), `layout/tests/test_remote_routes.py`, and `layout/tests/test_remote_apple.py`.

### Slow-hands alert and head-following notice

`hands/ft-camwatch` watches XRService's log for the post-suspend upper-camera re-sync loop (`Sync done fully for cam`, threshold `CAMWATCH_SYNC_PER_MIN=100`), posts a desktop notification with a **Restart SteamVR** button (which restarts SteamVR and relaunches the desktop), and shows an 8-second head-following notice (`layout/countdown/build/ft-reset-timer --notice ...`, `CAMWATCH_BANNER_S=8`). It waits while Frametop is paused for a VR game. See [rendered-hands.md](../hands/rendered-hands.md#the-re-sync-loop) and [layout/countdown/README.md](../layout/countdown/README.md#notices).

Install or refresh the user unit after changing releases:

```sh
R=~/.local/share/frametop/releases/current
sed "s|@REPO@|$(readlink -f "$R")|g" "$R/hands/frametop-camwatch.service" > ~/.config/systemd/user/frametop-camwatch.service
systemctl --user daemon-reload && systemctl --user enable --now frametop-camwatch.service
$R/hands/ft-camwatch --notify-test
```

### Local hand profile with confirmed-click pinch learning

`ft-hands` keeps what it learns about the wearer's hands in `~/.local/state/frametop/hands/profile` (numbers only, mode 0600, saved every 5 minutes and on exit): hand size (`user_scale`, moving slowly after settling), bone lengths (used as the fit's starting guess and merged halfway on save), CPU search tile weights (outside cameras handled by `ft-npud`'s palm detector), and pinch thresholds trained only from pinches the pointer helper turned into clicks, presses, drags, or scrolls (`hands/include/fh_verdicts.h`). Works alongside both `ft-npud` NPU hand tracking and CPU fallback. See [tracking.md](../hands/tracking.md#hand-profile); `HANDS_PROFILE=0` disables it and `ft-handsctl profile [forget]` inspects or clears it.

### Small reliability fixes

- **Cached tool root (`screens/vr.cpp`):** `ft-screens` resolves its repo root once in `ft_vr_init` instead of calling `realpath("/proc/self/exe")` on every `ft-layout` launch, so `scripts/fast-install --no-restart` does not break reset/scale commands.
- **Late remote room anchor (`layout/ft_layout.py`):** `remote_anchor()` derives the room anchor from Screen 1 first when desktop screens are active and unpinned, falling back to live head pose or `stored_anchor()`, so late remote connections land in their saved layout slots even when looking sideways.

## Verify changes

Run offline checks inside the release container (`just test`) or selectively:

```sh
./scripts/test-route-settings.sh
python3 -m unittest discover -s layout/tests -v
python3 -m unittest discover -s layout/countdown/tests -v
python3 -m unittest hands/tests/test_camwatch.py -v
make -C hands build/profile-test build/skeleton-test && hands/build/profile-test && hands/build/skeleton-test
git diff --check
```

## Keep the fork up to date

Preserve the previous `personal` tip as a backup branch before rebasing onto a newer upstream release. Retire local commits whenever upstream adopts equivalent behavior, rerun the container tests, install the new upstream release, apply `personal` with `scripts/fast-install`, and rewrite `frametop-camwatch.service` to the new release folder.
