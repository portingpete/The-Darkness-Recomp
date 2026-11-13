# Issue 124: CPU-sensitive freezes

The room-entry freeze could be reproduced in game by restricting the process to
two E-cores. The old v0.1.7 build visibly stopped progressing on the Chinatown
stairs/street route and logged a 6.7-second accepted-frame interval. Current main
completed the stairs, corner and crosswalk. [Watch the comparison](old-vs-main.mp4).
The clip contains real-time excerpts with approximate replay alignment; the
paths diverge once the old build stops processing movement.

The stronger control used **the same current executable**. Restoring both old
scheduling behaviors (`DARK_POOL_PRIORITY=guest`, `DARK_APP_POLL=0`) brought back
four stalls longer than one second entirely within the traversal phase, with a
maximum of **6,007.493 ms**. Two runs with current defaults peaked at **120.514 ms**
without video and **119.614 ms** with video, with no intervals of one second or
longer in either phase. This supports the combined scheduling fixes in
[#163](https://github.com/portingpete/The-Darkness-Recomp/pull/163) and
[#164](https://github.com/portingpete/The-Darkness-Recomp/pull/164). It does not
separate their individual effects. No additional runtime patch was necessary.

These fixes are on main at `cf4120722d8e1d704efce244790b158a85f44988`.
**The v0.1.10 release does not include them.**

## Test and evidence

6.1 Terra Ultra ran the game on native Windows with an i9-14900K and RTX 5080,
restricted to logical processors 16 and 17 (`0x30000`, two E-cores), normal
process priority, 1280×720, 60 FPS, vsync off, original FOV, original/unlimited
draw distance and original movement. Both builds loaded System32 D3D11/DXGI.
No high-priority stall watcher was enabled. The reporter's workaround of raising
process priority and disabling CPU 0 was **not** tested directly.

The mission was `NY1_Chinatown:ny1+layer1`. Developer mission loading was setup,
excluded from measurement. The 25-second controller phase started from
approximately `(-1848, 3440, -120)` and replayed W for 4 seconds, W for 3 seconds,
D for 1.6 seconds and W for 3 seconds. An external controller sampled the owned
process's position read-only and sent input through the game's test interface.
Video captured only the owned game window, with the recorder kept off its two
cores. No process memory was written.

The [Rust replay controller](replay-controller.rs) is included for inspection.
It builds on Windows with `rustc --edition 2021 -O replay-controller.rs` and takes
the private case directory, owned game PID, expected executable path and bootstrap
milliseconds as arguments. It checks the process identity before reading it and
closes only that process's visible window. A completed controller is not itself
proof of completed gameplay; the video, positions and frame trace establish that.

[proof.json](proof.json) records identities, boundaries, counts and limitations.
The three complete ordered numeric timelines are [default without video](default-no-video.csv),
[default with video](default-video.csv) and [restored scheduling](legacy-scheduling.csv).
All report zero lost rows. Statistics select `world=1` rows whose `t_ms` endpoint
lies inside each phase specified in the JSON. The restored-scheduling population
has five intervals above one second; one crosses the start boundary, leaving
four wholly within the phase. The old build only has a rate-limited trace;
its two large recorded intervals are retained in [old-build-outliers.txt](old-build-outliers.txt).

The tests used a private game-data copy and fresh isolated settings, saves and
application shader caches. Real settings and saves were unchanged, verified by
file membership, sizes and SHA256. Original game binaries and extracted assets
are not included here.

This closes the reproduced scheduling-sensitive freeze, not every possible
stutter. The reporter did not identify their hardware or a particular room.
OS/driver caches were uncontrolled and observer overhead was not measured.
Paths differ after the freeze, so this is not a matched full-route FPS benchmark.
The timeline measures accepted application frames, not physical scanout. Shorter
hitches remain; no Proton or other-hardware validation is claimed.
