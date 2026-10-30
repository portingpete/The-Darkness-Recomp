# CPU capture and resource-worker waits

This change brings the accepted capture and synchronization work onto current
main. It does not resolve the dense outdoor scene that remained near 120 FPS.

## Runtime changes

- Retain the lean capture implementation required by the pooled-draw fix:
  guarded binding/geometry validation, cached vertex preparation, bounded pooled
  control blocks, constant dirty ranges, per-thread counters, and ordered draw
  batching. Unsupported or faulted capture returns to the original path.
- On a fault during fast snapshot preparation, mark the pooled draw for a full
  reset. A fault can occur after constants have been copied but before their
  dirty ranges have been recorded; clearing only those ranges leaves stale
  constants for the next draw.
- During exact live-streaming resource accounting, skip the unused conservative
  image mip-size total. Exact retained-image accounting, geometry accounting,
  budgets, resource ownership and retirement remain in place.
- Replace eligible resource-dependency polling delays with per-thread completion
  events. The dependency's original guest predicate still decides completion.

The Rust helper owns the notification generation, armed-slot mask and at most
64 process-lifetime auto-reset events. Registration failure or exhaustion uses
the original delay. Notification follows the original resource job's normal
return, after its busy byte has cleared and its queue lock has been released.
The token predates the guest predicate; arming and generation checks prevent a
completion between that predicate and the host wait from being lost.

Only caller `8222CD30`, a one-millisecond nonalertable delay, and a valid scoped
resource runner are eligible. All other calls retain their original behavior.
The host wait keeps the one-millisecond timeout and executes the original SDK
wrapper afterward to retain its nonvolatile guest ABI.

## Historical route evidence

These measurements describe the preserved local candidates from October 28–29,
2026, before rebasing this PR onto current main. They are not a fresh benchmark
of the combined PR build, and their improvements must not be added together.
FPS here means successful application frame presentations, not physical scanout.

| Change | Paired route evidence | Limit |
| --- | --- | --- |
| Capture candidate with pooled fault reset | Two alternating baseline/candidate pairs: heavy mean 129.28 to 135.60 FPS; 31–33 s mean 110.69 to 114.52 FPS | Performance attribution remains provisional; one subwindow regressed and pixel equivalence was not established. The fault fix has a red/green protected-page regression. |
| Image accounting only | Three AB/BA/AB pairs: control 190.82 to 192.48 FPS; heavy 136.65 to 141.43 FPS; 31–32 s 117.08 to 120.33 FPS | The 32–33 s result was mixed and control p95 increased 0.50%. This is not a universal frame-time improvement. |
| Per-worker resource notifications | Matched early 10–20 s comparison: 277.77 to 486.86 FPS, mean wall time 3.600 to 2.054 ms, p99 9.874 to 4.824 ms; repeated early-scene confirmation was retained | Later route positions and workloads diverged. Dense-section measurements do not establish a causal improvement. |

The historical route used the same recording/checkpoint, display settings and
shader-cache seed within each comparison. The image-accounting runs used
3440×1440 output, 2576×1080 render targets, 1288×540 guest dimensions and 8× MSAA.
Five control-window queued/rendered-count discrepancies occurred on both builds;
the heavy/valley windows in that campaign had none.

Later investigation established that wall-clock input replay can follow different
courses even when its delivered input sequence matches. Full-route comparisons
require matching position, camera and work; matching tape bytes alone is not a
repeatability guarantee. Command-queue replay also diverged spatially and is not
part of this change. The dense outdoor producer continued spending substantial
time running draw-capture work after the dependency-wait improvement.

## Validation and rollback

On the rebased PR, the fresh ClangCL 19.1.5 Release build passed, AOT generation
reported zero semantic diagnostics, all seven selected CTest contracts passed
(including hardware and WARP renderer tests), and all four Rust unit tests
passed. `DarkRuntime` also built with `DARK_RUST_RESOURCE_WAIT=OFF`. No new
full-route performance result is claimed for this combined build.

Relevant targets are `DrawPoolTests`, `FrameResourceSetTests`,
`StoredGeometryTests`, `EngineMeshTests`, `WorldRendererTests` (including its
`--cpu-only` mode), and `ResourceWaitBackoffTests`.

The wait contract checks pre-arm completion, timeout, 1,000 latched cross-thread
signals, a delayed wake, five independently blocked waiters, and exhausted-slot
fallback. Rust unit tests additionally check independent armed bits, invalid
registrations and generation wrap:

```powershell
rustc --edition 2021 --test renderer/rust_resource_wait/src/lib.rs -o build_native/resource_wait_tests.exe
build_native/resource_wait_tests.exe
```

For a runtime comparison, set `DARK_RESOURCE_WAIT=original` before launching.
For build-time fallback, configure `DARK_RUST_RESOURCE_WAIT=OFF`. For capture
comparison, `DARK_NATIVE_LAYER=0` selects the original capture path and
`DARK_NATIVE_LAYER=2` compares fast/original captures while queuing the original.
The default is `1`. Check mode is diagnostic and is not a timing baseline.

The broad Rust gameplay/frame-accounting rewrites, texture-ownership experiments,
dense outlining and CPU-affinity experiments are not enabled by this PR. Raw
captures, saves, game assets and generated guest translation are not distributed.
