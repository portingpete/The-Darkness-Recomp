# Native XMA bridge

`XmaBridge` connects the original SDK group submission (`828B1BA8`), poll
(`828B1A58`), sample availability (`828B1048`), consumer (`828B10C0`) and reset
(`828B13A0`) to a native CPU decoder. Guest context storage and native decoder
instances belong to `Memory`; release/reset discards the matching stream.

The supported path is a single mono/stereo XMA stream at 24/32/44.1/48 kHz,
2048-byte packets, sequential whole-packet input, and a BE16 interleaved PCM
ring. The bridge decodes synchronously into free 256-byte ring blocks. It owns
padded compressed packet copies before releasing guest input valid bits, retains
partially read decoder frames, and stops when the output ring is full. The read
bit offset describes accepted packet progress, not an emulated hardware cycle.
It never sends EOF when guest input temporarily runs out. Loop/subframe seek,
arbitrary initial offsets and interleaved packet skips fail explicitly.

## Decoder build

`python tools/build_xma_codec.py` downloads and verifies FFmpeg commit
`1c2c67c0b9f7f66ab32c19dcf7f227bcd290aa4c` (n8.1.2), then builds only avcodec and
avutil with XMA1/XMA2 enabled. It requires the existing MSYS2 MinGW toolchain at
`C:/msys64` and LLVM import-library tool at `C:/Program Files/LLVM/bin`.
`tools/build.ps1` invokes it before configuring the native application.

The opt-in public AVOption `darkrecomp_raw_frames=1` bypasses the stock
container FIFO, priming skip and synthesized EOF overlap tail. The engine owns
its own priming/sample extents; upstream packet reservoir and transform math
are retained. An unmodified stock DLL rejects the option, rather than silently
using the wrong contract. DLLs use distinct `-darkxma` names.

Pinned source, archive hash, patch, configure arguments, import libraries and
LGPL notices are retained under `build_native/deps`. MinGW's time functions
also require `libwinpthread-1.dll`; its existing runtime and license are copied
alongside the decoder. CMake stages all three runtime DLLs with executables.
The configuration argument stamp avoids rerunning compiler probes unnecessarily.

## Validation and limits

`XmaRawContract` verifies first-packet PCM, partial-frame reads and overlapping
output against independently loaded, unmodified stock FFmpeg. For the captured
two-packet stream it returns 8192 samples/channel; 7616 overlapping samples match
stock within 1.2e-7 after accounting for stock's 576-sample skip.

`XmaBridgeContract` uses the original SDK functions to verify staged/live input,
split/combined packets, ring fullness, partial reads and wrap, same-address input
reuse, reset/free/reuse, rejected unsupported modes and no synthetic EOF output.
These checks are not a hardware oracle for exact priming, quantization, seeking
or looping, nor proof of playable graphics or controls.

## Context validation under overlapping sounds

The `overlap-refill25` combat capture reproduced audible stutter with detailed
sample tracing disabled and no XAudio2 starvation or device errors. At the
busiest point, whole refill passes averaged 50–72 ms. Most of that time was
inside `XmaBridge::decodeValidated`, while an independent 64-stereo-stream
decoder benchmark needed only about 1.2 ms to produce 512 frames per stream.

The owned context pool previously shared the precommitted 512 MiB title arena.
Every batch starts a fresh permission-validation cache, and the title submits
individual contexts, so `VirtualQuery` repeatedly scanned the large region.
A host reproduction measured about 1.35 ms per arena query versus 0.0003 ms
for the same 20 KiB pool surrounded by reserved memory.

The pool now occupies `60010000..60014FFF`, outside the general heap and title
arena, with reserved gaps on both sides. All ownership, permissions, bounds,
reset, release and decode checks remain in place. `XmaLifecycleContract` checks
the actual Windows region extent and its reserved neighbors, including after
ordinary heap allocations. Evidence and timing tools are in
`build_native/refill-audio-20260912/`.

The subsequent `overlap-poolfix26` user-controlled combat test was reported as
"No stutter." It saved 50.90 seconds of six-channel PCM and 9,771 refill-pass
records on normal window close. For passes with at least 20 streams, average
refill time fell from 48.30 ms in `overlap-refill25` to 2.35 ms in this run.
These were separate playthroughs, not identical workloads (the observed maxima
were 47 and 32 streams respectively). The previously elevated phase-128 seam
ratios of 1.17–1.70 became 0.97–1.001 across active channels. XAudio2 reported
zero starvation passes/bytes, engine glitches and errors; maximum callback
time was 3.46 ms. PCM contained no nonfinite values, clipped samples or repeated
nonsilent blocks. All 55 native checks passed, including the bounded-region
regression and existing original-SDK decode/lifecycle checks. Original saves
and settings were verified unchanged, and isolated settings were restored.
The harness's early-close receipt is not a completed 110-second pacing test.

The first integration boot exposed a separate mixer barrier issue. Original
thread creation at `828B57F0` passes a logical processor mask in flags[24:29],
but the native thread's PCR.Number at +0x10c was always zero. Mixer barrier
`828B3DC8` therefore waited for processor 4 while its worker marked slot 0.
Creation and affinity changes now publish the selected logical processor in
the thread's PCR. `NativeRuntime` verifies processor 4 and the original barrier.
After this correction a 30-second boot submitted 4620 buffers and received 4617
actual XAudio2 completions with zero device errors. The run ended at its explicit
diagnostic deadline, not at an XMA failure; gameplay remains unverified.

## Streaming refill waits

The game's refill worker (`827D8AF8`) calls the stream update (`827D9188`),
then waits on an event for 5 ms through `821FC5F0` and
`NtWaitForSingleObjectEx`. A Win32 millisecond timeout can stretch that wait
to a system timer tick. On the test host, 30 alternating 5 ms waits averaged
13.24 ms with the ordinary wait and 5.47 ms with the corrected import.

`native_timed_wait.h` preserves the guest's signed 100 ns deadline using a
high-resolution waitable timer alongside the requested object. Signals,
auto-reset consumption, infinite waits and alertable APC completion retain
their behavior. Reentrant APC waits lease separate timers. Hosts without
high-resolution timer support fall back to the ordinary object wait.
`NativeDelayContract` exercises these cases through the import and host helper.

Voice sample comparisons alone miss refill starvation: the original mixer
can emit zeros after consuming the last available input frame. Validation
must also compare each voice's cursor and fractional progress against its
requested output length, excluding starts, ends and loops. These deficits
count missing *voice* frames, not whole-output silence.

The September 12 `wait-after16` isolated checkpoint capture checked 20,527
eligible voice blocks (11,152 at nonzero gain) with no refill deficits. Its
60-second, 48 kHz six-channel PCM capture had no nonfinite samples, clipping,
fully silent blocks or repeated nonsilent blocks. All 55 native tests passed.
Evidence and reproduction scripts are in
`build_native/darkness-audio-20260911/`. The screenshots establish checkpoint
gameplay near the lift; this is not a full-game or sustained-combat validation.
Synchronous screenshots can interrupt playback, so their device starvation
counters must not be interpreted as ordinary playback performance.

An independent full-bank decode matched 29,140 captured edge samples across
17 active voices, including both long music/ambience streams (maximum error
5.62e-6). Other entries with the same sample count were rejected by their PCM
mismatch; extent matching alone does not identify an asset.

The `wait-clean17` follow-up omitted screenshots and detailed mixer tracing,
replayed checkpoint entry plus reload/fire inputs, and captured another full
60 seconds of PCM. Its reported device counters stayed at zero starvation
passes/bytes, engine glitches and errors. That PCM also had no nonfinite
samples, clipping, fully silent blocks or repeated nonsilent blocks. The
owned harness stopped at 159.39 seconds when the active desktop became
unavailable, before its planned 180-second deadline; its receipt is `stopped`,
not a completed pacing run. Both runs verified the original saves, settings
and executable hashes remained unchanged and restored the isolated settings.
