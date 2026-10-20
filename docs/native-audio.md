# Native audio backend (experimental)

The shared C++ backend in `src/runtime/native_sound_*` and
`native_sample_mixer.*` is used by the desktop (Linux, macOS, Windows) and
native GXM Vita frontends. It decodes the supplied Daytona93 music/command
data directly and mixes the original samples at 48 kHz. It does not construct
or execute a `SoundBoard`, recompiled 68000, YM generator or MultiPCM device.
The user-supplied, importer-verified ROM set is still required.

This is an opt-in replacement, not a chip-accurate mode. Reference audio stays
the default and can be selected again without replacing game data or saves.

## Select it

- Desktop: enable **Native audio (experimental; applies on reset)** in the
  launcher, then reset/start the game. Alternatively:

  ```sh
  build/daytona --rom /path/to/daytona93.zip --autostart --audio native
  ```

  Use `--audio reference` to return to reference audio. The selection is saved.
- Vita GPU22 and newer: Options → **Audio Engine: Native (Test)**, then
  **Reset Game**. Merely resuming an existing game does not switch its backend.
  To compare, select **Reference** and reset again. Existing volume/mute and
  clock settings still apply; this change does not raise the clocks.

SDL3 on desktop and SDL2 on Vita own the playback callback. A bounded 16 KiB
single-producer/single-consumer queue carries commands from the game thread.
The callback advances sequencing and mixing from the audio-device clock;
graphics updates do not supply its time. Pausing stops audio, whereas muting
continues sequencing. Shutdown/reset joins the callback before releasing its
engine and ROM storage. Queue/engine faults are reported, never silently
replaced by the reference backend.

## Output level

GPU25 calibrates the shared native mixer's master gain to 1.95, up from
GPU24's 0.75: 2.6x amplitude (+8.30 dB) before peak protection at the same
volume setting. The preceding 50% boost was still about 8.25 dB below the
reference RMS in both 6,000-frame attract and race replays.

A stereo-linked limiter protects loud transients at a 0.98 peak ceiling.
Both channels receive the same attenuation; recovery has a 50 ms exponential
time constant, not a 50 ms fixed completion time. It adds no lookahead delay
or heap allocations and retains state across callback blocks. Limiter-active
frames are reported separately from hard-clipped samples. This can reshape
loud attacks, so matching measured RMS is not a claim of identical sound or
perceived loudness on the device.

This applies to Vita and all desktop frontends. Reference audio, saved
volume/mute settings, sample pitch, command timing and voice balance are
unchanged. Routine Vita logging stays off; fault reporting remains enabled.

## Music and effects volumes

Each note event carries whether its channel is music (0-9 and 15, the ones the
driver's "stop music" command stops) or an effect (the rest and the engine
layers). The mixer scales each voice by the launcher's Music or Effects volume;
at 100% and 100% the output is unchanged. The audio callback takes both from
atomics, like the master volume, so only it touches the engine.

## Fidelity and performance limits

The mixer uses interpolated PCM and a short linear ADSR, not the original
envelope/LFO model. The native sequencer retains music, percussion, continuous
engine/effect layers, pitch, volume, panning, hold, fade and stop commands.
Nonzero modulation requests are reported as unsupported.

The note-on comparison covers 6,000-frame attract and race replays, not every
game mode. Attract matched all 2,274 sample/bank/pitch/gain/pan events in order.
Race had the same 4,899 note starts, with 4,889 strict-order matches: eight
startup engine notes and two later effects were reordered. Two initial engine
rates differed by 0.807%. Event times differed by up to 50.6 ms because this
backend does not reproduce UART serialization and sound-driver execution
latency. These are measured limitations, not waveform parity.

FM is not synthesized: a separate reference audit of these two replays found
23,178,664 FM float samples all zero, with no FM key-ons or DAC enable. Native
code supplies the music-event clock formerly derived from the YM timer.

Native audio continuing through a graphics stall does not make game logic or
geometry faster. A speed benefit, sound quality, suspend/resume and long-run
stability on Vita must be measured on the device. Linux host checks and Vita
cross-compilation do not validate Windows/macOS execution or Vita frame rate.

## Reproduce validation

After setup/import/recompilation of the user's ROM set:

```sh
cmake --build build -j2
ctest --test-dir build --output-on-failure
bash scripts/test_native_sound_rom.sh build 6000
bash scripts/test_native_sound_oracle.sh build 6000 build/rom_cache/daytona93 build/native-oracle-race race
bash scripts/test_native_sound_oracle.sh build 6000 build/rom_cache/daytona93 build/native-oracle-attract attract
bash scripts/test_vita_sound_pipeline.sh build 6000
python3 scripts/test_vita_gpu_lifetime.py --sanitize
python3 scripts/test_vita_renderer.py --sanitize
```

The native ROM test runs the real main board with `GameLoop(..., false)` and
asserts there is no reference sound board. It checks finite/nonzero output,
command/sample health, zero callback heap allocations, music and engine voices,
and 48,000 additional audio frames while game frames/instructions/commands are
held still. The reference pipeline test instead checks unchanged screen
hashes, instruction counts, command ordering and bit-identical reference audio.

Vita diagnostics identify `audio=NATIVE_TEST` and
`reference_sound_board=0`. `native_last_ms`/`native_peak_ms` measure callback
wall time, not main-frame work. `native_frames`, `native_notes`, `native_voices`,
`native_queue`, `native_overflows`, `native_failed`, `native_unsupported` and
`native_invalid` make progress and failures observable in diagnostic builds.
GPU23 disables routine logging by default and preserves the existing file;
unexpected faults still append a record. Diagnostic-enabled builds replace
`ux0:data/daytona93/vita-diag.log` on startup, so copy it before relaunching.
See [Vita diagnostics](../platform/vita/PERFORMANCE.md) for the build switch
and the GPU22 device-log findings.
