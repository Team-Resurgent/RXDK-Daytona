# DIAG02: direct native diagnostics

This revision addresses insufficient diagnostics, not the unmeasured performance
bottleneck. A two-line log ending at `renderer: VITA gxm` does not establish
whether a later call stalled, output failed, or the executable was different.
The prior performance marker was after texture/audio/settings initialization.

## Use

Apply the accompanying `apply_vita_diagnostics.py` from the repository root.
It accepts the original Vita port, that port with the startup correction, or
that port with both the startup and performance patches. It validates all
replaced files before writing, refuses unknown local edits, and saves originals
under `build/vita-diag-backup-*`. Re-running it is safe. No Git operations,
network calls, dependency installation or ROM operations are performed.

Then use the existing build driver:

```
python3 scripts/build_vita.py --build-dir build/vita-diag --jobs 4
```

Install `build/vita-diag/daytona_vita.vpk`. The menu must say **DIAG02**.
No re-import/recompilation of the ROM-derived C++ is needed.

Collect **`ux0:data/daytona93/vita-diag.log`**, not only `vita.log`.
The diagnostic file is replaced at each launch; copy it before relaunching.
`vita.log` remains the legacy stderr destination for shared-runtime output.

## Changes

* Native `sceIoOpen`/`sceIoWrite`/`sceIoSyncByFd`/`sceIoClose` writes, independent
  of `stderr`. The first literal is written before the heap probe or SDL setup.
* Stage markers around video, texture, audio, settings, import, board creation
  and the first two game frames. A last `begin` without its matching `done`
  narrows where execution stopped; it does not identify the underlying cause.
* DIAG02 build date and time in the diagnostic log and a DIAG02 menu title.
* A visible logger status, sticky write error code, separate sync warning and
  bounded log capacity (1 MiB). Partial writes are completed. Zero/error writes
  stop that record; file handles are still closed. Later records may retry.
* Native process-time microseconds used consistently for the profiler and host
  pacing. SDL's Vita timer uses the same native counter and 1 MHz frequency.
* Existing performance summaries now go to the native diagnostic file every
  two seconds of a progressing main loop. A small screen overlay gives the
  latest simulation FPS and core/video/sound times even if file output fails.
* Both earlier startup/performance fixes are included: 192 MiB up-front heap,
  safe startup failures, one complete game frame per presentation, batched text
  and optional shared-runtime profiling. No game-logic or clock-speed changes.

The direct logger is main-thread-only. It is NOT installed as an SDL callback
or called from the audio thread. The fixed native-write buffers do not require
heap allocation; formatted messages use vsnprintf after the heap probe.
Filesystem sync/close requests are checked, not a guarantee against power loss.
Startup and first-frame writes add diagnostic I/O overhead; use later summaries
for performance investigation. There is no background heartbeat: a blocked
main loop cannot emit periodic FPS, but its earlier stage markers remain useful.
The overlay covers the bottom 36 display pixels in this diagnostic build.

## Validation performed for this revision

Host C++ tests passed under AddressSanitizer/UndefinedBehaviorSanitizer:
native logger successful/partial/zero/negative/overreported writes, failed open,
failed sync, failed close, truncation, capacity and reset; input/pacing;
profiling/overlay; batched text rendering. Eight build-driver tests passed.
Frontend syntax was checked with host-only API declaration shims; that is NOT
VitaSDK compilation or a link test. Installer tests are recorded in the
accompanying validation report. Full ARM compilation/linking, actual device
logging, hardware performance and gameplay parity still require verification.

Primary API references consulted (30 September 2026):
https://docs.vitasdk.org/group__SceFcntlUser.html
https://docs.vitasdk.org/group__SceProcessmgrUser.html
https://github.com/libsdl-org/SDL/blob/SDL2/src/timer/vita/SDL_systimer.c
No upstream implementation was copied into the logger.
