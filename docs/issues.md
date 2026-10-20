# Open issues: assessment and plans

The open GitHub issues, what the code shows about each, and what is planned.
Reviewed on 2026-10-06 against `main` at `6817bff`. Nothing here is changed
yet. Line numbers drift; the files and functions named are the reference.

| Issue | Summary | Status |
| --- | --- | --- |
| [#4](#4-logitech-driving-force-cannot-be-bound-on-linux) | Logitech Driving Force (PS2) cannot be bound on Linux | Confirmed: works with SDL's Logitech driver off. "Legacy Logitech wheel support" setting added |
| [#7](#7-stutter-and-no-refresh-rate-options) | Stutter; no resolution or refresh-rate options | Four settings added, all off by default (native speed) |
| [#9](#9-force-feedback-only-rumbles-on-a-direct-drive-wheel) | Force feedback only rumbles on a direct-drive wheel | Commands were decoded wrongly (the centring spring played as a shake). Fixed, confirmed by the reporter on a Simagic Alpha Mini. Enhancements documented, not started |
| [#16](#16-force-feedback-on-a-driving-force-under-linux-no-centring-on-bends) | Force feedback on a Driving Force under Linux: no centring on bends; a failing test | "Centring driver override" (Linux) added; the test fixed |
| [#10](#10-sound-effects-too-loud-compared-with-the-music) | Sound effects too loud compared with the music | Music and Effects volumes added, both audio modes |
| [#6](#6-android-version) | Android version | Android and iOS builds with touch controls merged (PR #14); tested on devices, working |

## #4: Logitech Driving Force cannot be bound on Linux

**Report:** the wheel (USB `046d:c294`) is listed in the controls menu as
"Driving Force EX", but no button, pedal or wheel movement registers when
binding. `evtest` sees all of them.

**Cause (likely):** SDL, not the game. "Driving Force EX" is the name SDL's
HIDAPI Logitech wheel driver gives `c294`
([SDL_hidapi_lg4ff.c](https://github.com/libsdl-org/SDL/blob/release-3.4.16/src/joystick/hidapi/SDL_hidapi_lg4ff.c),
SDL 3.4.16, which setup puts in `extern/sdl3`).
That driver takes the device over hidraw, so SDL's evdev backend (what
`evtest` reads) skips it. For `c294` the driver reads 27-byte reports and
drops any report of another length (`HIDAPI_DriverLg4ff_UpdateDevice`). If the
original PS2 Driving Force sends shorter reports, no input ever arrives. Not
yet confirmed: the PS2 wheel's report length.

**The game's side is fine:** every joystick is opened when it appears
([controls.cpp](../src/app/controls.cpp), `Devices::handle_event`), and the
capture in [launcher.cpp](../src/app/launcher.cpp) handles buttons, hats and
axes from their rest values, so pedals resting at either end work. One weakness
to fix later: while an axis is being tracked, buttons and other axes are
ignored, so a noisy axis can block a bind until Esc.

**Confirmed:** with `SDL_JOYSTICK_HIDAPI_LG4FF=0` the wheel works.

**Done:** a setting rather than a change for
everyone, since SDL's driver works for the Logitech wheels it was written for
and only this older one is affected. Controls tab: **Legacy Logitech wheel
support (Restart Required)** (`legacy_logitech_wheels`; on by default on Linux, whose kernel
driver gives Logitech wheels force feedback; off on macOS, where only SDL's
driver does). On, it
sets `SDL_HINT_JOYSTICK_HIDAPI_LG4FF` to `"0"` before `SDL_Init`
([main.cpp](../src/app/main.cpp)), as the Graphics API setting does with its
hint. Hidden on Windows: SDL's Logitech driver is off there by default
(`hid.dll` cannot send its reports). SDL gives the wheel another GUID under
the other driver, so its controls are bound again after switching.

**Next:** the reporter has been told about the setting and asked whether
**Invert steering** is still needed after rebinding; if it is, find why the
kernel driver's axis runs the other way from SDL's. Report the 27-byte report
check to SDL.

## #7: Stutter and no refresh-rate options

**Report:** stutter at the arcade's 57.524 Hz with a custom display mode; no
resolution or refresh options. A second user: VRR does not help, frame pacing
is uneven. Triple buffering in the driver hid it for the reporter, at the cost
of input lag.

**Cause:** in [main.cpp](../src/app/main.cpp), game frames are timed by the wall
clock (`pending` / `frame_ns`), run in whole frames, and presented under the
display's vsync. Nothing sleeps, and the present mode and frames in flight are
SDL's defaults. So:

- **60 Hz:** about every 24th refresh repeats a frame (a hitch about every
  0.4 s). This follows from running at 57.52 frames/s on a 60 Hz screen.
- **57.524 Hz:** still uneven. The clock is read after the vsync wait, so its
  noise flips some refreshes between 0 and 2 game frames.
- **144 Hz:** each frame is shown for 2 or 3 refreshes (13.9 or 20.8 ms against
  17.4 ms). A slight shimmer, much less visible than the 60 Hz hitch, but seen
  in steady panning. Clock noise can make the 2-3-2-3 pattern irregular.
- **VRR:** no help, because the game presents at the display's maximum rate,
  not at 57.52 Hz.

The only display setting is fullscreen on or off (borderless, at the desktop's
mode).

### Plan

**Rule: by default the game runs at the arcade's own speed, 57.52 frames/s, on
any display.** Every change below is its own setting, off by default. With all
of them off the game behaves as it does now. A faster display may show frames
more often; it never makes the game faster unless the player turns on Sync to
display.

**Done** (launcher, Game tab, under Fullscreen; `src/app/pacing.h`): all four,
off by default. The table is as built.

| Setting | Default | When turned on |
| --- | --- | --- |
| Sync to display | Off | The game runs at a rate that divides evenly into the display's refresh and is close to the arcade's: 60 frames/s on 60, 120, 180 and 240 Hz, 57.5 on 115 Hz. No effect where no such rate exists (144 Hz, 165 Hz). |
| Smooth pacing on a 57.52 Hz display | Off | One game frame per refresh (or two, at 115.05 Hz) when the display is within 1% of a multiple of the arcade's rate; no doubled or skipped frames. Speed within 1%. |
| VRR pacing | Off | Each frame held to 1/57.52 s by a precise sleep, one game frame per present, so a G-Sync or FreeSync display refreshes at the game's rate. Vsync stays on (the usual way to drive VRR: no tearing), not the immediate present mode first planned. Speed unchanged. |
| Fullscreen mode | Desktop | A list of the display's modes: exclusive fullscreen at the chosen resolution and refresh (a custom 57.52 Hz mode made in the driver among them) instead of borderless at the desktop's. |

Description for Sync to display in the options:

> **Sync to display** (off by default)
> Runs the game at a rate that divides evenly into your screen's refresh rate,
> so every frame is shown for the same time and motion is perfectly smooth. On
> 60, 120, 180 and 240 Hz screens the game runs at 60 frames/s, about 4% faster
> than the arcade (57.52), and with reference audio the sound plays slightly
> faster (native audio keeps the music's own tempo). It has no
> effect on screens like 144 Hz or 165 Hz, which can't evenly fit a rate close
> to the arcade's; on those, use VRR pacing if your monitor supports G-Sync or
> FreeSync. Off: the game always runs at the arcade's own speed.

Why only even divisions: locking to 60 on a 144 Hz display still shows frames
for 2 or 3 refreshes (2.4 refreshes each), no smoother than 57.52, and 4% fast.

**Checked:** `test_app_pacing` (the mode for 0, 50, 57.52, 59.94, 60, 115, 120,
144, 165, 180 and 240 Hz; the default's frame count against the old loop's
arithmetic over 20,000 random passes; one frame per refresh at 60 Hz with 2 ms
of jitter; every 2nd at 120 Hz; held to its rate with vsync forced off; VRR
frames exactly 1/57.52 s apart, no burst after a stall). In the game, frames
counted over 15 s: default 57.9, Sync to display on a 60 Hz display 59.8, VRR
57.53 frames/s. With two displays (60 and 144 Hz) the pacing follows the
window: Sync to display went back to the arcade's speed when the window moved
to the 144 Hz one. Not yet tried on a VRR display, nor listened to.

**Notes for the implementation:**

- **Audio:** reference audio adjusts its stream speed by at most ±0.5% to keep
  about 60 ms queued, and clears the streams past 240 ms. At 60 frames/s that
  limit must scale by the rate over 57.52, or audio drops out every few seconds.
  Native audio has no such adjustment and needs one.
- **Refresh rate:** read it from `SDL_GetCurrentDisplayMode`, or better, measure
  it from the presents; reported rates are rounded.
- **Exclusive fullscreen:** `SDL_GetClosestFullscreenDisplayMode` with 57.524,
  then `SDL_SetWindowFullscreenMode`.
- **Input lag:** `SDL_SetGPUAllowedFramesInFlight(device, 1)` would cut it;
  possibly a further option.

## #9: Force feedback only rumbles on a direct-drive wheel

**Report:** Simagic Alpha Mini, Windows 10. Force feedback sometimes starts only
after a while, and feels like constant crash rumble; no centring, grip, kerb or
contact effects.

**How it works now:** the game's drive-board command bytes are decoded in
[drive_board.h](../src/runtime/drive_board.h) (high nibble: 0x1 centring, 0x2
friction, 0x3 vibration, 0x5/0x6 force right/left, 0x8/0xC reset) and played
by [ffb.cpp](../src/app/ffb.cpp) as SDL haptic effects: spring, friction, a
60 ms sine and a constant force, each infinite, each only if the device
supports it. Those meanings were taken from Supermodel's notes on Sega's later
drive boards. **For Daytona most of them are wrong** (below).

### What Daytona's drive board does (checked 2026-10-06)

Checked against the drive board's own program, EPR-16488A (Z80, 2.8 KB), in
two ways:

- **Read:** disassembled with MAME 0.289's debugger (`dasm` from a Lua
  `-autoboot_script` with `-debug -debugger none`).
- **Run:** in MAME, with Lua taps on the drive CPU's I/O space. The game's
  commands were logged during a race (`race_basic`'s inputs), and then chosen
  commands and wheel positions were fed to the board directly (command port
  `$27`, wheel ADC `$80`), recording the motor output (`$46`) for each.

The board reads the wheel's position itself (a serial ADC on port `$80`) and
drives the motor in a closed loop: power 0 to 63, in one of two directions,
ramped by one step per interrupt (about 57 a second). The game only sends a
command when it changes something: 42 in a whole race, the same 42 in MAME and
in our runtime (`m2run`). Each command stays in force until the next.

| Command | What the board does | Our decoder today |
| --- | --- | --- |
| `0x00`-`0x04`, `0x08`, `0x09`, `0x0B`, `0x0C` | Motor disabled: no force at all until enabled | Ignored |
| `0x05`-`0x07`, `0x0A`, `0x0D`-`0x0F` | Motor enabled (the game sends `0x07` when a game starts) | Ignored |
| `0x10`-`0x1F` | No force | Centring spring, strength n/15 |
| `0x20`-`0x27` | Power with neither direction: most likely resistance (a brake), strength n | Friction, n/15 (close) |
| `0x30`-`0x37` | **Centring spring**: pushes back towards the centre, harder the further the wheel is turned; strength n | **Vibration** (60 ms sine), n/15 |
| `0x38`-`0x3F` | **Centring spring with a dead zone** around the centre; strength n | **Vibration**, n/15 |
| `0x40`-`0x47` | Uncentring: pushes away from the centre; strength n | Ignored |
| `0x50`-`0x57` | Constant force, direction 1; strength n | Force right, (n+1)/16 |
| `0x60`-`0x67` | Constant force, direction 2; strength n | Force left, (n+1)/16 |
| `0x28`-`0x2F`, `0x48`-`0x4F`, `0x58`-`0x5F`, `0x68`-`0x6F` | No force | As their high nibble |
| `0x70`-`0x7F` | Sets a parameter of the spring's slope and minimum (the game sends `0x71` once) | Ignored |
| `0x80`-`0x83` | Status queries (wheel position, `0xFF`, DIP switches): the motor is unchanged | `0x80`: reset everything |
| `0x84`-`0xFF` | Nothing: the motor is unchanged | `0x88`, `0xC0`-`0xCF`: reset everything |

n is the low 3 bits. Direction 1 moves the wheel's position reading down,
direction 2 up; which of them is left on a cabinet is not known from the
board alone (our decoder calls `0x5-` right; "Invert force" swaps it).

There is **no vibration** on this board: nothing makes it shake. In the race,
the game sends `0x07` (motor on) and `0x71` when the race starts, then mostly
`0x39`-`0x3C` (centring spring with a dead zone, strength 1 to 4), with
stretches of `0x54` (constant force, direction 1, strength 4) of 15 to 300
frames between them.

### Causes, most likely first

1. **The commands are decoded wrongly** (confirmed above; every wheel, not only
   the Simagic). In a race the game's centring spring (`0x39`-`0x3C`) is played
   as a 60 ms sine at 60 to 80% strength, all race long. Our spring is set
   only from `0x1-`, which means "no force" and which the game sends at most
   once in a race, so no centring spring is played. That is the
   report exactly: constant shaking like a crash, no centre pull. Commands the
   game sends that we ignore: motor on/off (`0x0-`) and uncentring (`0x4-`).
2. **Fallen back to rumble.** If SDL cannot open the wheel as a force feedback
   device, `ForceFeedback::open` quietly uses joystick rumble instead, because
   the wheel also reports rumble. On Windows SDL opens wheels through
   DirectInput (its default; RawInput and Windows.Gaming.Input are off), and
   its DirectInput rumble is a sine effect on the wheel's motor
   (`SDL_DINPUT_JoystickRumble`): shaking, and never a spring. `open` is called
   only when the steering device changes, so it then stays on rumble for the
   session. Not confirmed; one way it happens is "Couldn't find joystick in
   haptic device list" in `SDL_DINPUT_HapticOpenFromJoystick`. **Can be checked
   today:** under the force feedback slider the launcher shows "Now: wheel
   (force feedback)" or "Now: gamepad (rumble)".
3. **Lost updates.** No return value of `SDL_UpdateHapticEffect` or
   `SDL_RunHapticEffect` is checked, and `changed()` records a level as sent
   before sending it. If an early call fails (on Windows the device may not be
   acquired yet), an effect the game sets rarely stays off until its level
   changes. This could explain "starts after a while".

### Plan

Fixing cause 1 changes how force feedback feels on every wheel. That is
deliberate: today's behaviour is wrong on all of them.

**Fixes:**

- **Decode the commands as the board does** (cause 1), in `drive_board.h`, per
  the table: motor on/off; centring spring with and without the dead zone;
  resistance; uncentring; constant force in either direction; "no force" for
  the rest; status queries and unknown commands leave the state as it is. The
  vibration (sine) effect goes: the board has none.
- **Play them with SDL effects:** the centring springs as SDL springs (the dead
  zone as the spring's `deadband`), resistance as SDL friction or damper,
  uncentring as a spring with a negative coefficient (or a constant force away
  from the centre, from the wheel's position), constant force as now. Map
  strength 0-7 onto the SDL levels so that 7 is close to full.
- **Gamepads keep rumble** (the default when steering is bound to a pad, or no
  wheel is bound; keyboard only has no force feedback, as now). Today the pad's
  small motor follows "vibration", which is really the centring spring, so pads
  buzz at 60 to 80% all race too. After the fix the rumble comes only from
  forces that push the car: the large motor from the constant force
  (`0x5-`/`0x6-`), the small motor from uncentring (`0x4-`), each by strength.
  The centring spring (on all race) and resistance give no rumble. The strength
  slider still scales it, 0 off.
- **No rumble on a force feedback device** (cause 2). When
  `SDL_IsJoystickHaptic` says the device has force feedback but the haptic open
  fails, do not fall back to rumble: log `SDL_GetError` and try the open again
  every few seconds, with no force until it succeeds. Rumble stays for
  gamepads, which have no haptic device.
- **Retry failed calls** (cause 3). Check the result of every haptic call.
  Record a level as sent only if its call succeeded, and set `running_` only
  once every effect has started, so a failed call is retried on the next frame.
- **Launcher text:** it lists "kerb rumble"; the board has no rumble.

**New setting.** It does not exist yet. Today the launcher has only "Force
feedback (Experimental)", a strength slider where 0 is off, and "Invert force"
([launcher.cpp](../src/app/launcher.cpp)). The "Vibration" setting planned
earlier is no longer needed: with the commands decoded correctly there is no
vibration to turn off.

| Setting | Default | When changed |
| --- | --- | --- |
| Force feedback log | Off | Writes force feedback lines to the game's log: the haptic device's name and supported effects when it is opened, each effect created or not, every failed haptic call with `SDL_GetError`, and the game's drive-board command bytes with the frame number. |

**Where the log goes:** the game's existing log, `daytona.log`, in the same
folder as `launcher.ini` (`open_log` in [main.cpp](../src/app/main.cpp)):

- Windows: `%APPDATA%\daytona-recomp\daytona93\daytona.log` (`daytona`
  instead of `daytona93` for Revision A), as in getting-started.md. Started
  from a command window, the game prints to that window instead and writes no
  file.
- macOS and Linux: printed to the terminal the game was started from.

On Windows the log is overwritten each time the game starts, so it has to be
copied after the run that shows the problem, before the game is started again.

**Still open:**

- Which direction is left. A cabinet would settle it; failing that, the
  game's own use: which constant force it sends when the car is pushed one
  way (a wall or a car on one side, in an input script).
- Exactly what `0x2-` feels like: power with neither direction selected is
  most likely a brake, but that depends on the motor driver circuit.
- Both sets (`daytona93` and `daytona`) list the same two drive board ROMs:
  EPR-16488A (MAME's default, the one checked) and the older EPR-16488 (not
  checked).

**Confirmed (2026-10-06):** the reporter tried the build on the Simagic Alpha
Mini: no more shaking, centring works, and force feedback did not cut out in
his testing.

### Enhancements (documented, not started)

**Force feedback cutting out (a safeguard).** The reporter's second problem,
force feedback stopping after 10-20 s or at full lock until a restart, did not
happen in Daytona but still happens in another recomp (Namco System 22). A
likely mechanism, read from SDL's code, not measured: when Windows takes the
wheel away (a direct-drive base resetting at full lock, say), SDL's next
`SDL_UpdateHapticEffect` reacquires the device and retries, and succeeds
(`SDL_DINPUT_HapticUpdateEffect`, `src/haptic/windows/SDL_dinputhaptic.c`).
But a reacquired DirectInput device has its effects stopped, and SDL does not
start them again, so the game believes force feedback is playing and the wheel
stays slack. Our retries do not catch it either: the update reports success.
Daytona probably escapes because the game sends so few commands. The
safeguard: about once a second, ask `SDL_GetHapticEffectStatus` (where the
device reports `SDL_HAPTIC_STATUS`) and `SDL_RunHapticEffect` any effect that
should be playing and is not. No change on a wheel that keeps its effects.

**More force feedback settings,** asked for by the reporter after M2Emulator
with Boomslangz's FFBPlugin. Enhancements: the defaults keep today's force
feedback exactly.

| Setting | Default | What it adds |
| --- | --- | --- |
| Force feedback strength | 70% (as now) | Up to 200%, clamped at the device's maximum, for weak wheels or a stronger feel |
| Centring spring | 100% | Scales the game's centring spring (0x3-) |
| Resistance | 100% | Scales the board's resistance (0x2-, SDL friction) |
| Damper | 0% | A constant SDL damper, which the arcade board does not have: weight when turning, steadier at speed |
| Minimum force | 0% | Lifts small forces above a direct-drive wheel's dead zone |

**Next:** the enhancements above, when wanted.

## #10: Sound effects too loud compared with the music

**Report:** on the arcade the music is quiet but clear; here the sound effects
drown it out.

**Reference audio** (the default): the mix matches MAME's Model 1 sound board:
the YM3438 at 0.30 and each MultiPCM at 0.5
([sound_board.cpp](../src/runtime/sound_board.cpp)). The MultiPCM code matches
MAME's, and the output was checked against MAME's recordings (HANDOFF.md). So
MAME has the same balance, and MAME's gains may not match a real cabinet. Music
and effects share both MultiPCM chips, and the driver hands out voices as they
are needed, so a voice cannot be told apart by its chip or slot.

Measured in `race_basic`'s race (reference audio, RMS of the mixed output):
the effects alone 0.195, the music alone 0.059, about 10 dB apart.

**Native audio:** a real problem.

- The master gain is 1.95 (`kDefaultMasterGain` in
  [native_sample_mixer.h](../src/runtime/native_sample_mixer.h)) against MAME's
  0.5 per chip, so the peak limiter turns the whole mix down, music included, on
  every loud effect.
- Notes never get their sample's envelope
  ([native_sound_engine.cpp](../src/runtime/native_sound_engine.cpp), `NoteOn`),
  so effects do not fade the way the game intends.
- A music/effects balance setting would be easy: the sequencer already tells
  them apart by channel
  ([native_sound_sequencer.cpp](../src/runtime/native_sound_sequencer.cpp)).

### Done: Music and Effects volumes

A new **Audio** tab in the launcher has Volume, Mute, **Music** and
**Effects** (both 100% by default) and the Native audio switch.

- **Which voice is which (reference audio).** At each key-on the sound board
  finds the driver's voice for that chip and slot in the 68000's RAM and reads
  its channel; music is channels 0-9 and 15, as the driver's own "stop music"
  command has it. From the driver's code: two voice pools of 28 at 0xf01500
  and 0xf01618, ten bytes each (byte 0 nonzero when in use, bit 3 the chip;
  byte 1 the slot code; byte 3 the channel); records with byte 6 0xff are the
  engine layers' reserved ones and are skipped (they carry the wrong chip
  bit). The engine's fixed slots on the second chip have no voice record and
  count as effects.
- **Checked** against the native sequencer's class for the same notes (the
  oracle's pairing): race 4,899 of 4,899 notes agree, attract 2,274 of 2,274;
  Revision A race 4,892 of 4,892, attract 2,095 of 2,095. Exactly one voice
  record per key-on.
- **At 100% and 100% nothing changes:** `race_basic`'s reference audio WAV is
  byte-identical before and after (20 MB), and the screen hash and instruction
  counts are unchanged. Music alone plus effects alone equals the full mix in
  all but 0.097% of samples, the ones where a chip clips at 16 bits in the full
  mix.
- **Native audio:** each note carries the class from its channel; the mixer
  scales each voice by its volume.

**Next:** ask the reporter which audio mode they use, and whether Music and
Effects let them get the balance they remember. Native audio's master gain
and envelopes are still open.

## #16: Force feedback on a Driving Force under Linux: no centring on bends

**Report (mikefairbank, the #4 reporter):** with the PS2 Driving Force on
Linux, force feedback is felt on crashes but not round ordinary bends; Invert
force was needed; the wheel seems to twitch just before a crash; and the
`app_controls` test failed.

**No centring on bends.** In Daytona the feel through a corner is the board's
centring spring (0x3-), on all race. A game cannot drive the motor directly:
it asks the driver for effects, and the Linux kernel's Logitech driver offers
this wheel, as far as known (from memory of `hid-lg4ff`, not checked here), a
constant force and its own autocentre, but no spring. The spring is then not
created, and autocentre is off because the game centres the wheel itself, so
nothing centres it on bends; crashes come through as constant force. The
force feedback log's first line (`spring no/yes`) confirms it.

**Done: "Centring driver override"** (Controls tab, Linux only,
`ffb_centring_override`, off). On, the game's springs are worked out from the
wheel's position the game sees (`rt::DriveBoard::motor`: towards the centre,
rising over 8% of the travel beyond the board's dead zone of 2%, or 6% for
0x38-0x3F, to the spring's level; uncentring away) and sent in the constant
force; the spring effect is left at nothing. Off, nothing changes, and on
other systems it is not offered (main ignores the setting outside Linux), so
the Simagic on Windows (#9) is untouched. Invert force flips the centring with
the crash pushes, so once crashes push the right way, so does the centring.

**Invert force needed:** left as it is: the Simagic (#9) did not need it, and
which of 0x5-/0x6- is left on a cabinet is still unknown.

**The twitch before a crash:** each command is played on the frame the game
sends it, with nothing predicted here, so it comes from the game (its
collision checks); likely the same on a cabinet.

**The failing test:** `app_controls` counted every joystick, so a wheel plugged
in while the tests ran made "both virtual devices opened" fail. It now counts
on top of the devices already there.

**Next:** the reporter to try the override and send the log's first line.

## #6: Android version

Done in `platform/mobile` (PR #14, from boucymatt): Android (arm64) and iOS
builds of the desktop runtime and renderer, with on-screen touch controls
(analogue steering, gas and brake, gears, views, coin and start) that work
alongside Bluetooth or USB controllers. Tested on devices (2026-10-06):
Android and iOS work with no issues reported (the PS Vita port too). See
`platform/mobile/README.md` and its HANDOFF. The issue can be closed.
