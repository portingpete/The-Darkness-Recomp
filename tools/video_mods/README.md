# Video replacement packs

This dependency-free Rust tool imports an authored replacement for an original
WMV3 or Theora video. The optional native loader selects the new file at the
original guest pathname. The game's decoder and playback clock remain in use;
external XWC audio banks are unchanged.

The original clips come from your own game dump. To build a pack, supply
locally authored replacement clips. Players can use a compatible pack shared
by its creator.

Build with Rust 1.74 or newer. The `create` and `create-batch` commands require
`ffprobe` from FFmpeg on PATH, or the executable path in `DARK_FFPROBE`.

```powershell
cargo build --release --locked --manifest-path tools/video_mods/Cargo.toml
tools/video_mods/target/release/darkrecomp-video-pack.exe create K:/Games/Darkness Content/Videos/Theora/IngameMisc/fultontv.ogg C:/Art/fultontv-hd.ogg C:/Packs/FultonHD
$env:DARK_VIDEO_MOD_DIR = 'C:/Packs/FultonHD'
```

Launch a native build containing the video-pack loader with that environment
variable set. Omit or clear it to use original videos. Each pack maps exact
original video paths to payload files; it does not replace whole directories.
Payloads are pinned against writes and deletion until their final runtime
owner closes. Build a new pack for an edit and select it on the next launch.

For several clips, save a UTF-8 mapping file with one target and replacement
path per line. Use a literal tab between the paths. Replacement paths can be
absolute or relative to the mapping file, and can contain spaces.

```text
Content/Videos/Theora/IngameMisc/PaulieConSite.ogg	Paulie HD.ogg
Content/Videos/Theora/IngameMisc/Paulie_idle.ogg	Paulie idle HD.ogg
```

```powershell
tools/video_mods/target/release/darkrecomp-video-pack.exe create-batch K:/Games/Darkness C:/Art/clips.tsv C:/Packs/PaulieHD
$env:DARK_VIDEO_MOD_DIR = 'C:/Packs/PaulieHD'
```

Every clip is validated before the new pack directory is created. Duplicate
targets are rejected, including case and separator variations. Each pack can
contain up to 128 clips. The combined `media.txt` records the probe results
for each target.

The importer checks the original and replacement with bounded `ffprobe` calls.
They must retain the same codec, frame rate, audio stream layout and timeline
(duration and start within one original frame). Video must be 8-bit YUV 4:2:0
with even dimensions no larger than 2048 per axis. An explicit
`--allow-aspect-change` permits a different aspect ratio; otherwise pad or crop
the authored image to the original ratio. The original screen geometry remains
in control, so aspect changes can distort the image on that screen.

The game directory, replacement and output parent must be ordinary paths.
Reparse points, symlinks, parent traversal and existing output directories are
rejected. Inputs are opened read-only. If writing fails, the importer preserves
the incomplete directory for inspection. It publishes `videos.tsv` atomically
after the payloads and report have been synced. On Unix, pack output requires
a filesystem that supports hard links.
No game media is included in this repository or redistributed by the tool.

The native loader validates the complete bounded manifest, original container,
payload containers and exact byte sizes before publishing any entry. An invalid
pack is disabled with a diagnostic and the original videos remain selected.
Container signatures do not certify a media bitstream; use the importer and
test playback for the intended scene, loop and seek behavior.

Current qualification includes ten Rust manifest/media compatibility tests,
native pack sharing/lifetime tests and the original guest file I/O contract.
A byte-identical WMV copy played through the original decoder at 1024x576 and
advanced to subsequent startup movies. The Theora UI observer also passes
synthetic frame ownership, object-type and GPU resize checks up to 1536x1152.
Authored HD replacements, actual Theora scene playback, looping and audio/video
synchronization remain unverified. No frame-rate improvement is claimed.

The manifest is a UTF-8 tab-separated file with complete LF or CRLF lines:

```text
DARKRECOMP_VIDEO_V1
content/videos/theora/ingamemisc/fultontv.ogg	video.ogg	123456
```

There are at most 128 entries and 64 KiB of manifest data. Payloads are plain
basenames of the matching container, at most 2 GiB each. `check MANIFEST`
validates manifest syntax only.
