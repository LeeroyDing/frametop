# Personal fork: KWin 6.6 scaling and per-network streams

The `personal` branch is based on `kwin-6.6-output-scale` (`3b039d0`): upstream `experimental` (`fdfd96e`, the `v0.3.0` development line) plus the KWin 6.6 nested output scaling commit. Upstream remains [Frametop/frametop](https://github.com/Frametop/frametop); this fork is [LeeroyDing/frametop, branch personal](https://github.com/LeeroyDing/frametop/tree/personal).

Upstream already includes the controls guide, `layout/countdown/`, and geometry-only screen reset through PR #67 (`7fab7e1`). They are inherited from the base. The old IDR throttle and its revert cancel out and are omitted from this history. The commits above `3b039d0` carry per-network stream settings, moving a running stream to an available direct address, a sturdier way for ft-screens to find its tools, two fixes to the KWin 6.6 scaling (also on its pull request), and this fork reference.

## KWin 6.6 nested output scaling

The base commit reconciles KWin 6.6's nested output scaling with ft-screens. It advertises fractional scaling, configures the nested output in logical dimensions, and keeps layout positions consistent with the requested scale while KWin applies changes. Floating-window metadata uses the same scale convention. The implementation and SteamOS update checks are documented in `docs/design.md` and `scripts/update-check.py`; offline coverage lives in `screens/tests/screen-scale-test.c` and `layout/tests/test_scales.py`.

From KWin 6.6, `kscreen-doctor` shows every nested output at scale 1, whatever its scale. ft-layout used to read that back and hand it to ft-screens, so a desktop set to 200% went back to 100% a few seconds after it started. `23d2c3f` stops that, and `6a0a06c` makes ft-floatd and `ft-layout remote-view` take screens' scales from the layout as well. [PR #86](https://github.com/Frametop/frametop/pull/86) carries both (`28bdcbd`, `42d0059`). To see the scale an output really has, use `qdbus6 org.kde.KWin /KWin supportInformation`.

## Per-network stream settings

A remote display can have its own frame rate and bitrate for each way its stream reaches the host. ft-stream picks the way each time it connects: one of the host's `direct` addresses if one answers (a dongle, or a second link such as the host's Ethernet), otherwise the host's own address. These are the `direct` and `network` routes.

```sh
layout/ft-layout remote set mac-desktop direct.bitrate=15000 network.bitrate=4000
layout/ft-layout remote set mac-desktop direct.fps=30    # the frame rate too
layout/ft-layout remote set mac-desktop direct.bitrate=  # back to the display's own
layout/ft-layout remote list                                         # each route's settings
```

They are stored in the display's entry in `~/.config/frametop-layout.json` as `"routes": {"direct": {...}, "network": {...}}`. A route uses the display's own `fps` and `bitrate` for anything it leaves out, and 0 kbit/s is Frametop's default for the size and frame rate. Frame rates run from 1 to 240 and bitrates from 0 to 500000 kbit/s. Changing a route's settings starts a running stream over, with its panel where it is. The stream log names the route whose settings it used (`stream: the direct route's settings, 60 fps, 15000 kbit/s`), and the stream's state says which way it went (`live via dongle` for a direct address, `live via network`).

A stream that goes through the host's own address with route `auto` keeps checking the host's direct addresses, every 10 seconds on its own thread. Once one answers two checks in a row, such as the host's Ethernet being plugged in, the stream ends with `route: ADDRESS answers now, so the stream starts again through it` in its log and the state `lost moving to a direct address`. ft-screens starts it again 2 seconds later through that address, with that route's settings and its panel where it was. Over a direct address nothing is checked: if that link goes, the stream ends and comes back through the host's own address. Routes `network` and `dongle` never switch. Remote Displays still edits only the display's own settings.

- `stream/route_settings.h`: a display's settings for a route, a host's route and direct addresses, and the two-in-a-row rule for switching; `stream/tests/route_settings_test.cpp` tests them (`scripts/test-route-settings.sh`).
- `stream/ft-stream.cpp`: applies the settings once the route is chosen, before working out the default bitrate, and watches for a direct address while on the host's own (`WatchDirect`).
- `layout/ft_layout.py`: `remote set ROUTE.fps=F ROUTE.bitrate=KBPS` and `remote list`; `layout/tests/test_remote_routes.py` tests them.

## Finding ft-screens' tools

ft-screens starts `layout/ft-layout` (the reset button, `scale` after a move) from its own install folder. It now finds that folder once, at start. Before, it looked again for every run through `/proc/self/exe`, and once an update replaced the program on disk while it ran (`scripts/fast-install --no-restart`), that path named a deleted file. Every reset button press then did nothing, with nothing logged, until the desktop restarted.

## Upstream integration

Per-route settings and direct-address checks work with upstream's desktop-free streaming (DEE-340): a running ft-screens stream does not need KWin or Plasma to change routes. `remote list` retains upstream's microphone and host-input flags alongside each route's settings. `WatchDirect` reports the handoff before connection teardown, retaining upstream's exit watchdog, host-input release, microphone cleanup, and gamepad release.

The controls guide is available from the desktop application menu and Frametop Input Settings. Screen reset runs the head-following countdown before sampling the head pose, then moves existing unpinned panels while preserving apps and streams. Explicit Open profile restores saved apps and window positions. These behaviors and their tests are maintained upstream.

## Source and installation

Clone this branch explicitly; upstream release downloads do not install this fork:

```sh
git clone --branch personal https://github.com/LeeroyDing/frametop.git
cd frametop
```

Follow the repository's installation prerequisites and normal `./install.sh` workflow. Build components in the dev container; from a PC, edit locally and use `scripts/sync.sh` and `scripts/frame.sh`. This fork does not currently publish prebuilt release assets.

## Verify changes

Run these offline checks from the checkout. The route-settings test requires a C++17 compiler and nlohmann/json, supplied by the dev container. The countdown lifecycle tests require the pinned STB headers from `layout/countdown/deps.sh` and an installed TrueType font.

```sh
./scripts/test-screen-scale.sh
./scripts/test-route-settings.sh
python3 -m unittest discover -s layout/tests -v
python3 -m unittest discover -s layout/countdown/tests -v
git diff --check
```

`just test` runs the layout and countdown suites in `test-python`, and both screen-scale and route-settings checks in `test-c`. The C/C++ test scripts compile locally wherever they run, so they work inside the image without SSH or a headset.

Live checks should cover scaled output sizes and placement on KWin 6.6, route-specific frame rate and bitrate, and a network stream moving to a newly available direct address. Repeat route checks while streaming without the desktop. Arrange any live headset checks with its wearer first.

## Keep the fork up to date

Keep `upstream` pointing to `https://github.com/Frametop/frametop.git`. Review changes against the exact experimental base before rebasing; upstream `main` can lag it. Retire local changes when upstream provides equivalent behavior, and preserve upstream's stream lifecycle and cleanup fixes when resolving conflicts.

After an update, rerun the offline tests and check the relevant SteamOS package hints in `scripts/update-check.py`. For rollback, preserve the previous `personal` tip before rebasing and restore its matching source and built components through the normal workflow.
