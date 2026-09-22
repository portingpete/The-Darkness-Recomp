# The Darkness Recomp

A native Windows x64 port of **The Darkness (Xbox 360)**. The 360 executable is
translated to C++ ahead of time and built into `DarkRecomp.exe` with a D3D11
renderer — no emulation at runtime.

> **Bring your own game dump.** This repo contains no game code, assets, or
> binaries. Dump your Xbox 360 copy and drop the files into `Darkness/`
> (step 1 below).

Playable today: full gameplay, mouse look + controller input, video settings
(resolution, FOV, gamma, bloom, frame cap, VSync), XMA audio, and saves.

## Download and play

**[Download the Windows release](https://github.com/portingpete/The-Darkness-Recomp/releases).**
Choose `The-Darkness-Recomp-v...-windows-x64.zip` under **Assets**.
The GitHub **Source code** downloads are for building the project yourself.

1. Right-click the Windows ZIP and choose **Extract All**.
2. Copy your own extracted Xbox 360 game dump into the included **Darkness**
   folder: `_uncrypted.xex`, `basefile.exe`, `default.xex`, `Content`, `System`,
   and all the other files and folders from your dump. If the first two files
   are missing, follow **Preparing the game files** below to generate them.
3. Double-click **Launch.cmd** to play with sound.

No compiler, Python, or separate audio setup is needed for the Windows release.
Use 64-bit Windows 10/11 with a Direct3D 11-capable graphics device.
See [START_HERE.txt](START_HERE.txt) for the folder layout and troubleshooting.
An ISO alone is not enough; the dump must include the decrypted executable
images for the supported game revision.

Click the game window to capture the mouse. **F1** shows controls, **F2** toggles
capture, and **Esc** releases it. Graphics options are in **Options > Video Settings**.

## Preparing the game files

`_uncrypted.xex` and `basefile.exe` are generated from your own `default.xex`;
they are not normally present in a disc extraction. `basefile.exe` is a raw
Xbox 360 memory image read by the port, not a Windows application to run.

1. Obtain **xorloser's XexTool** separately; **v6.3** is the version tested here.
   It is not included with this release.
2. Place `xextool.exe` beside `default.xex` in your **Darkness** folder.
3. Open that folder in File Explorer, type `powershell` into its address bar,
   and press Enter. Run these commands one at a time:

   ```powershell
   .\xextool.exe -e u -c u -o _uncrypted.xex default.xex
   .\xextool.exe -b basefile.exe default.xex
   ```

   Confirm that each command reports success. These commands preserve
   `default.xex`. Keep the `-o _uncrypted.xex` option on the first command;
   do not rename files or apply region/devkit patches. If you already have
   generated files, back them up before regenerating them.
4. Keep both generated files in **Darkness**, together with the original dump,
   then double-click `Launch.cmd` in the parent folder.

### Checking the supported game revision

For **v0.1.1**, run this in the same PowerShell window:

```powershell
Get-FileHash .\_uncrypted.xex, .\basefile.exe -Algorithm SHA256 | Format-List
```

The generated SHA-256 hashes must match these values (case does not matter):

```text
_uncrypted.xex  a7ccd87860889fa082d62dcc24382467d59c8031f6e99d2ed9f3dbddaa7d535e
basefile.exe   180b7fc8f57462f6bac3404ecab061a8d79914c7a3e9a12c238bd3449e72d049
```

These commands were tested against the project's dump and reproduced both
release inputs exactly. Its XEX metadata reports Title ID `545407EE`, Media ID
`0F213645`, version `0.0.0.1`, and **All Regions**. That does not establish
compatibility with every USA/Canadian/international disc revision; the generated
hashes are the decisive check for this release.

If the hashes differ, report the hashes and the Title ID, Media ID, and version
shown by `.\xextool.exe -l default.xex`. Do not post
the game files or the full tool output, which includes unrelated key fields.
If the game closes after launch, also attach the newest
`build_native/run/desktop-*/runtime.log`. A different result can indicate a
different revision, a modified dump, or a preparation problem.

## Build from source

**1. Game files** — from your own dumped copy, fill in `Darkness/`:

Use **Preparing the game files** above to generate the two required images.

```
Darkness/
  _uncrypted.xex            # decrypted executable
  basefile.exe              # raw extracted image used by analysis/tests
  default.xex
  Content/  Content_Eng/ .../ ExtraContent/
  System/                   # engine shaders and system data
```

`darkness_switch_tables.toml` already ships with the repo — keep it and add
the rest from your dump. Nothing else under `Darkness/` is committed.

**2. Build** — one step (dependency setup + generator + translation + XMA codec + compile + tests):

```powershell
powershell -ExecutionPolicy Bypass -File tools\build.ps1
```

The build downloads the pinned XenonRecomp source and applies the bundled
Darkness patches automatically. Existing checkouts from the old setup instructions
are supported too. The first build needs an internet connection.

You'll need 64-bit Windows, Git, Visual Studio 2022 with the **ClangCL** toolset,
CMake 3.24+, Python 3.11+, and ~15 GB free. Output lands in
`build_native/Release/`.

The XMA audio build also requires MSYS2 at `C:\msys64` with MinGW64 GCC and
`make`, plus standalone LLVM at `C:\Program Files\LLVM` (for `llvm-lib.exe`).

**3. Play** — double-click `Launch.cmd`:

| Command | What it does |
|---|---|
| `Launch.cmd` | Play with sound |
| `Launch.cmd mute` | Play muted |
| `Launch.cmd preview` | Engine preview build (muted by default) |
| `Launch.cmd stutter` | Play with sound while recording slow-frame timings |
| `Launch.cmd performance` / `render-profile` / `steady-60` | Diagnostic recording runs |
| `Launch.cmd help` | Full usage |

Extra arguments reach the game untouched, e.g. `Launch.cmd play --fps 120`.

Click the game window to capture the mouse (**F1** controls guide, **F2**
toggle capture, **Esc** release).

## Docs

- [CONTROLS.md](CONTROLS.md) — every binding, launcher option, and setting
- [RENDERING.md](RENDERING.md) — how the renderer and frame pacing work

## Updating a downloaded release

Close the game, extract the new Windows ZIP into your existing game folder,
and replace the included program files. Keep **Darkness/**, **saves/**, and
**DarkRecomp.settings.ini** to preserve your game files, progress, and settings.
You do not need to rebuild.

## Updating a source build

Pull the latest version and run the build again:

```powershell
git pull --ff-only
powershell -ExecutionPolicy Bypass -File tools\build.ps1
```

Keep your game dump and existing build directory. Unchanged generated files
retain their timestamps so the native build can reuse previous compilation work.

If an older setup stopped with `AOT gate: Generator failed (0)`, these same
commands install the missing generator patches and regenerate the output.

## Packaging a release

Maintainers can build, test, and package a Windows ZIP with:

```powershell
python tools/package_release.py --version v0.1.1
```

Run from a clean, committed checkout with the build prerequisites installed.
The ZIP and SHA-256 checksum land in `build_native/releases/`. The packager
includes the launcher, runtime dependencies, notices, and audio-library source;
game dumps, saves, settings, build tools, and diagnostic logs are excluded.

## Tests

The build already runs the full suite. To re-run it later:

```powershell
ctest --test-dir build_native -C Release --output-on-failure
```

## Project layout

| Path | Contents |
|---|---|
| `app/` | Windows entry points and display settings |
| `assets/` | Button-prompt artwork bundled with the game |
| `cmake/` | CMake helpers |
| `config/darkness.toml` | Translation configuration |
| `renderer/` | D3D11 backend and engine scene reconstruction |
| `runtime/` | Native kernel, audio, input, XMA, and filesystem bridges |
| `tools/` | Translator driver, `build.ps1`, shader/menu/XMA codegen |
| `tests/` | Native and contract tests run by CTest |

## License

GPLv3 — see [COPYING](COPYING), matching the upstream
[UnleashedRecomp](https://github.com/hedge-dev/UnleashedRecomp) /
[XenonRecomp](https://github.com/hedge-dev/XenonRecomp) toolchain this port builds on.
