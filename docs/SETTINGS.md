# Settings, the in-game menu, and the launcher

This document covers the settings file (`settings.json`), the environment
variables that override it, and the in-game overlay menu that edits it. It is
written for both a user running the packaged port and a developer reading the
runtime source. The schema tables below are taken directly from
`src/runtime/host/settings.h` and `settings.cpp`; if the two ever disagree,
the code is right and this file is out of date.

## 1. Where settings live

The file is always named `settings.json`. The runtime looks for it in this
order and stops at the first match:

1. `ICORECOMP_SETTINGS`, if set. A value of `-` or `0` means "run on
   compiled-in defaults and never save" (the same opt-out spelling
   `ICORECOMP_LOG` uses elsewhere in this codebase). Any other value is
   treated as a literal path to the settings file, with no further fallback:
   a bad path here is the user's explicit choice, not something the runtime
   should second-guess.
2. `settings.json` next to the executable (`rt_base_dir()`). This is the
   "portable folder" promise: a settings file dropped beside the packaged
   binary, e.g. on a USB stick, travels with it and wins over any per-user
   copy.
3. `settings.json` in the per-user config directory: `$XDG_CONFIG_HOME/icorecomp`
   or `~/.config/icorecomp` on Linux, `%LOCALAPPDATA%\icorecomp` on Windows.
4. Neither exists: the runtime starts on compiled-in defaults with no file
   loaded yet.

If both the portable copy and the per-user copy exist, the portable one wins
and the runtime logs that the per-user copy is shadowed.

The first successful save picks a target the same way: it tries the folder
next to the executable first, falls back to the per-user config directory if
that folder is not writable, and that choice is then sticky for the rest of
the run. Once a file has been loaded, saves always go back to the same path
it came from.

## 2. Schema

`"version": 1` is required at the top level of the file; a file missing it,
or carrying a non-numeric value, is treated as unparseable (see section 4).

Apply timing:
- **hot**: takes effect immediately, before the next use of the value.
- **warm**: queued and applied at the next field boundary
  (`rt_settings_apply_pending()`), because the subsystem it touches cannot be
  changed mid-frame.
- **cold**: only takes effect on the next run of the executable.

A hot setting needs no applier when its consumer reads `rt_settings()` fresh
at every use; `settings_apply.cpp` marks those "nothing to push here" rather
than calling them cold.

### display

| key | type | allowed / range | default | apply | env override |
|---|---|---|---|---|---|
| mode | enum | `windowed`, `fullscreen_desktop`, `fullscreen_exclusive` | `windowed` | hot | - |
| window_width | int | [320, 16384] | 1280 | hot | - |
| window_height | int | [320, 16384] | 960 | hot | - |
| remember_window_size | bool | - | true | hot | - |
| present | enum | `mailbox`, `fifo`, `immediate` | `mailbox` | warm | `ICORECOMP_GS_PRESENT` |
| fit | enum | `letterbox`, `integer`, `stretch` | `letterbox` | hot | - |
| raster | enum | `crt`, `window` | `window` | hot | - |
| deinterlace | enum | `adaptive`, `bob`, `weave` | `bob` | hot | - |
| filter | enum | `linear`, `nearest` | `linear` | hot | - |
| render_scale | int, one of a set | 1, 4, 8, 16 | 1 | warm | - |
| show_fps | bool | - | false | hot | - |

### audio

| key | type | allowed / range | default | apply | env override |
|---|---|---|---|---|---|
| master_volume | int | [0, 100] | 100 | hot | - |
| mute | bool | - | false | hot | `ICORECOMP_NO_AUDIO` |

`master_volume`/`mute` are read fresh on every audio callback (`sdl_submit`,
`host/audio.cpp`) and apply only to that host-side output stage. The WAV
capture path (`ICORECOMP_WAV_CAPTURE`) reads the unscaled mix and never sees
this gain; it is the headless verification baseline and has to stay a
function of the sound engine alone.

### input

| key | type | allowed / range | default | apply | env override |
|---|---|---|---|---|---|
| keyboard.\<slot\> | string | an `SDL_GetScancodeName` name | see table below | hot | - |
| gamepad.\<slot\> | string | an SDL gamepad button string, or an axis name with a trailing `+`/`-` for direction (e.g. `lefttrigger+`) | see table below | hot | - |
| left_deadzone | float | [0, 0.95] | 0.0 | hot | - |
| right_deadzone | float | [0, 0.95] | 0.0 | hot | - |

An unresolvable binding name (a typo, a name from a different SDL version)
falls back to the compiled-in default for that one slot
(`rt_settings_default_binding()`), with a log line naming the slot and the
name that did not resolve; it never falls back to "no binding".

`host/input.cpp` reads every one of these keys, including
`input.keyboard.menu` and `input.gamepad.menu`: it builds a keyboard table
and a gamepad table from them and rebuilds both whenever
`rt_settings_generation()` moves, so a change made in the menu applies on
the next poll, the same field it is committed. That includes the menu
hotkey itself (`ui/ui.cpp` re-resolves it on the same generation check), so
nothing in this section is cold or warm any more; every row above is hot.

An axis bind (the `lefttrigger+` / `righttrigger+` defaults for L2 and R2)
counts as pressed when the raw axis reads past 8192 of 32767 in the bound
direction, the same `> 8192` check the pre-settings build hardcoded. The
press point is a compiled-in constant in `sdl_poll()`
(`src/runtime/host/input.cpp`), not a setting: the host pad state
(`RtPadState`, `src/runtime/host/input.h`) carries sixteen digital bits and
the two sticks and no analog trigger channel, so the point only ever chose
where a digital bit flips. The pad HLE quantises that bit to a 0 or 255
pressure byte (`src/runtime/sif/pad.cpp`); whether ICO reads L2 or R2
pressure is not established. The keys
`input.trigger_threshold` and `input.rumble` from earlier builds are
retired; a file that still carries them keeps them, unchanged, and the load
logs each one as retired. Rumble follows the game's own actuator requests,
which the game asks the player about when a new game starts.

Each stick gets a radial deadzone with remainder rescale: `left_deadzone`
and `right_deadzone` are the fraction of the stick's radius (0 to 0.95) that
reads as centered, and everything past that is rescaled so the reported
range still reaches the edge. At `dz == 0`, the shipped default, the
deadzone math is skipped entirely rather than run with a zero threshold,
so a fresh install produces the exact same axis values the pre-settings
build did.

Two rules keep one host key or button from being asked to do two things at
once, both enforced by `validate_binds()` in `settings.cpp` at commit time:
the menu key must not also name a pad slot (the menu consumes that input
before the pad ever sees it), and two ordinary slots on the same device must
not share a name, because one host input cannot press two DS2 buttons.

Both rules revert only the slots that commit changed, whichever side of the
pair they are: binding the menu key onto an existing pad slot reverts the
menu key, binding a pad slot onto the menu key reverts the pad slot, and
changing both reverts both. A revert goes back to the previously committed
name, not the compiled default, logs why, and is also shown inline in the
Input tab that made the change. A collision where neither slot changed is
skipped: it arrived in the settings file, `log_bind_duplicates()` reports it
once at load, and the load path never rewrites the user's own file, so
re-rejecting it on every later commit would only leave a permanent message
in a pane that cannot fix it. Name comparison for both rules is
case-insensitive, matching how `SDL_GetScancodeFromName` itself resolves
names.

Default keyboard bindings (`kKeyboardBinds`, `settings.cpp`):

| slot | key | slot | key |
|---|---|---|---|
| up | Up | rstick_up | I |
| down | Down | rstick_down | K |
| left | Left | rstick_left | J |
| right | Right | rstick_right | L |
| cross | X | l1 | Q |
| circle | C | r1 | E |
| square | Z | l2 | 1 |
| triangle | V | r2 | 3 |
| lstick_up | W | l3 | T |
| lstick_down | S | r3 | Y |
| lstick_left | A | start | Return |
| lstick_right | D | select | Backspace |
| menu | F1 | | |

Default gamepad bindings (`kGamepadBinds`, `settings.cpp`):

| slot | button | slot | button |
|---|---|---|---|
| up | dpup | l1 | leftshoulder |
| down | dpdown | r1 | rightshoulder |
| left | dpleft | l2 | lefttrigger+ |
| right | dpright | r2 | righttrigger+ |
| cross | a | l3 | leftstick |
| circle | b | r3 | rightstick |
| square | x | start | start |
| triangle | y | select | back |
| menu | guide | | |

Sticks are not rebindable slots: they map natively from the gamepad's own
analog axes.

### gameplay

| key | type | allowed / range | default | apply | env override |
|---|---|---|---|---|---|
| run_any_direction | bool | - | false | hot | - |

Off, the default, reproduces retail: a left-stick tilt that is not close to a
cardinal direction makes the player walk rather than run, no matter how far
the stick is pushed. On, a full tilt in any direction runs.

The cause is in the decomp. `iosPadGetStick` (`ios/pad.c`) forms a radius
from the stick's two raw bytes around a centre of 127.5, applies a deadzone
of 48, then divides that radius by `1 + 0.2 * t / 45`, where `t` is the
integer degrees the tilt sits off the nearest cardinal direction (0 at a
cardinal, up to 45 at a diagonal), and saturates the result at a radius of
120. The player code (`src/boyact.c`) only emits a run when that corrected
value is at least 0.99 for four consecutive frames, which needs a corrected
radius of `48 + 0.99 * 72 = 119.28`. The divisor was tuned for the DualShock
2's octagonal gate, whose corners sit past the inscribed circle, so a
diagonal push there still clears 119.28. An SDL gamepad reports a circular
stick instead: radius 127.5 at every angle, with no octagonal overshoot at
the corners. Divided by the same factor, a full circular tilt clears
119.28 only within about 15.5 degrees of a cardinal; at 45 degrees it would
need a raw radius of 143.1, which a circular stick never reaches.

The fix stays on the host side. `host/input.cpp` pre-multiplies the left
stick's byte pair by the same divisor the game is about to apply, before
those bytes are handed to the virtual pad, capped so the pair never leaves
the 0..255 byte square. That cancels the game's own division: a full tilt
reads as a full radius in every direction. Nothing the game computes
changes and no guest code is patched. The right stick and the keyboard
sticks are untouched.

The byte-square cap sets a ceiling, not a hole: near 13 degrees off a
cardinal, the cap limits a full-radius stick to a corrected radius of about
123.8, still above the 119.28 the run gate needs. Below full radius, the
same cap means a pad reporting under about 96 percent of full radius can
still land in the 5 to 25 degree walk band rather than running. The byte
square bounds a physical pad the same way, so that is the game's own limit
carried through, not a hole in the transform. This band is derived from the
formula above, not measured on hardware.

The first field the transform is active, `host/input.cpp` logs
`gameplay.run_any_direction is on` once; the latch resets if the setting
turns off, so a later re-enable logs again.

### debug

| key | type | allowed / range | default | apply | env override |
|---|---|---|---|---|---|
| verbose | string | an `ICORECOMP_VERBOSE` channel spec; empty means the compiled-in default channels | `""` | hot | `ICORECOMP_VERBOSE` |
| log_file | bool | see section 5 | true | cold | `ICORECOMP_LOG` |
| profile_fields | int | [0, 100000]; 0 disables the profiler | 180 | hot | `ICORECOMP_PROFILE` |
| fps_limit_hz | double | 0, or [1, 1000]; 0 disables pacing | 59.94 | hot | `ICORECOMP_FPS_LIMIT` |

Each profile summary ends with a `fields:` line: the longest host field
interval in the window, how many fields ran over 20 ms and over 50 ms, how
many were catch-up fields (fields the limiter did not hold back, because the
audio debt had already put the deadline in the past or because the queue held
less than one field of mix), and the longest single disc read. A `longest
field` line under it breaks that field down by bucket, largest first, which
is what separates a GPU wait (present, gs) from a host read (disc) or a decode
(ipu) when one field stalls. Both lines measure fields between consecutive
profiler boundaries, so a field's own pacing sleep is billed to the field
after it. Where a backend has a present path, a `present flush / scanout / present_frame` line under it
names three spans inside the `present` bucket, so a field lost to the
swapchain and a field lost to the renderer can be told apart. The bucket table
above them is an average over the whole window, which is the wrong shape for a
stutter; these are the extremes in it.

### launcher

| key | type | allowed / range | default | apply | env override |
|---|---|---|---|---|---|
| show_at_startup | bool | - | true | cold | - |
| disc_path | string | a path, resolved relative to the executable's directory | `""` | cold | - |

See section 8: the launcher window sets both of these. `show_at_startup` is
its "show this window next time" checkbox, and `disc_path` is written
whenever a disc image is chosen or cleared there.

## 3. Environment precedence

For every key with an environment-variable twin, the environment always wins
over `settings.json` for the whole run. This is logged at startup as
`"<key> is overridden by <VAR>=<value>"`, and once the in-game menu exists for
a setting with a twin, its control shows disabled with the same "overridden
by" text.

"Set" means the variable is present in the environment, even as an empty
string, because that is exactly what every consumer of these variables tests
(`getenv() != NULL`). An empty `ICORECOMP_VERBOSE=` still counts as set, for
instance.

`ICORECOMP_LOG` is the one exception: it must be non-empty to count as set.
An empty log path names no file, so `log.cpp` reads the variable as
`env && *env` and leaves `debug.log_file` in charge. Reporting the key as
overridden there would say the setting was ignored when it is exactly what
took effect.

The full table (`kEnvTwins`, `settings.cpp`):

| settings key | environment variable |
|---|---|
| display.present | `ICORECOMP_GS_PRESENT` |
| debug.fps_limit_hz | `ICORECOMP_FPS_LIMIT` |
| debug.verbose | `ICORECOMP_VERBOSE` |
| debug.profile_fields | `ICORECOMP_PROFILE` |
| debug.log_file | `ICORECOMP_LOG` |
| audio.mute | `ICORECOMP_NO_AUDIO` |

This is deliberate: it keeps every existing script, CI job, or manual
invocation that already sets one of these variables running exactly as it
did before `settings.json` existed.

### Variables with no settings key

The two cycle-billing knobs are environment only. They set how much virtual
time the guest is charged for running its own code, which is a property of
the hardware model rather than a user preference, so neither has a
`settings.json` key or a menu control. Both are read once by `rt_sched_init`
(`src/runtime/ee/sched.cpp`), which logs the value in force under the
`sched` channel at startup.

| variable | range | default | what it bills |
|---|---|---|---|
| `ICORECOMP_EE_LOOP_CYCLES` | 1 to 64 | 2 | bus cycles per taken backward branch in translated EE code. Higher is a slower emulated EE against the field clock; too low and the game's sound tick runs too often per field and over-refills the sndn2 stream ring. |
| `ICORECOMP_MMIO_CYCLES` | 1 to 4096 | 32 | bus cycles per EE hardware-register access. This is what paces register-driven guest code such as the MPEG player: at the default a field of 2460060 cycles holds 76876 accesses. |

Both are sweep knobs, not tuning the port expects a user to do. A value
outside the range leaves the compiled-in default in place;
`ICORECOMP_MMIO_CYCLES` says so in a log line naming the value and the
range, `ICORECOMP_EE_LOOP_CYCLES` does not and only the startup line shows
which value took effect.

## 4. Bad values and broken files

None of this is ever fatal. A `settings.json` a user or a hand-edit can break
in any number of ways, and none of those ways should cost the rest of a
working file, let alone the run:

- **Unknown keys** anywhere in the document are kept across a load/save round
  trip (the loader retains the whole parsed document, not just the fields it
  recognizes) and each is logged once as `"unknown key \"<dotted.key>\" kept
  as-is"`.
- **A bad value** (wrong JSON type, or a number out of range or not in the
  allowed set) keeps that one key's compiled-in default, and logs the bad
  value and the allowed range or set. The rest of the file still loads
  normally.
- **An unparseable file** (invalid JSON, a non-object top level, a missing or
  non-numeric `"version"`, or a `"version"` other than 1) is copied
  byte-for-byte to `settings.json.bad`, its line:column (1-based) is logged,
  and the run proceeds on compiled-in defaults. Saving is disabled for the
  rest of that run, which is what makes "the broken original is never
  overwritten again" true: the save target is still that file, so a later
  save would replace what the user has to fix with a defaults document.
  `rt_settings_save()` returns false and logs the file and its `.bad` copy
  as the reason. Fix or delete the file and restart.
- **`"version"` greater than 1** means a newer build wrote this file. The run
  proceeds on defaults and the file is left untouched, so downgrading and
  later re-upgrading does not lose anything.

Saving is atomic: the runtime writes to a `.tmp` file next to the target,
`fsync`s it, then renames it over the target. A save that fails partway
leaves whatever was already on disk alone.

## 5. debug.log_file

`debug.log_file` is read before anything else in settings, by
`rt_settings_peek_log_file()`, because `rt_log_init()` has to decide whether
to open a log file before the first line can be logged, and so before
`rt_settings_init()` itself can run. This means the key takes effect at
startup only; changing it in a running instance's file has no effect until
the next launch.

- `false` behaves exactly like `ICORECOMP_LOG=-`: no log file, console only.
- `true` keeps the platform default: Windows always writes `icorecomp.log`
  next to the executable; Linux only writes it when `ICORECOMP_VERBOSE` is
  set or `ICORECOMP_LOG` names a path (the console already keeps the output
  on Linux, so the file sink there is opt-in).
- `ICORECOMP_LOG` always wins over `debug.log_file`. When the two disagree,
  startup logs that the settings value was ignored.
- Before opening `icorecomp.log`, if a log from an earlier run is already
  at that path, it is renamed to `icorecomp.prev.log` (replacing any older
  `icorecomp.prev.log`), so a crash log is not overwritten by the next
  run. A failed rename is not fatal: the run logs why and then overwrites
  the file as before.

## 6. Render scale and display resolution

`display.render_scale` is the single knob for both, and what it selects is
paraLLEl-GS super-sampling: a fixed integer multiple (1/4/8/16) of the game's own
framebuffer resolution. Because it scales the whole framebuffer uniformly,
the aspect ratio is preserved automatically and the game's 4:3 derivation is
untouched. At 1 the picture is the game's own resolution, a 512x448 (or
whatever the game programmed) image stretched to the window. At 4, 8 and 16
the runtime also asks for high-resolution scanout, so the picture is built
from the super-samples at double resolution instead of being resolved back
down; there is no separate setting for that request. That works because 4
and up also turn on super-sampled textures, so the copy the game makes from
its own render target into the buffer the CRTC reads keeps the sub-samples
instead of resolving them away, and the scanout can rebuild the full frame
from them.

2 is not in the allowed set. `SuperSampling::X2` only doubles the vertical
sampling rate, and the renderer drops a high-resolution scanout request when
either axis has no extra samples, so 2x could smooth edges but never scale
the picture. A `settings.json` holding 2 is an out-of-set value like any
other: the key keeps its default of 1 and the load logs the allowed set.

The renderer can still decline the request for some scanout configurations;
the `hires=` field on the `scanout internal` log line reports what it
actually did, per scanout geometry change.

`display.raster` selects the output frame the scanout is built at, which is
a separate question from how many samples go into it. `crt` is
the area paraLLEl-GS models as visible for the video mode: 640 pixels by 224
lines per field for non-overscan NTSC, at DISPLAY clock 636 and line 50. ICO's
gameplay window is exactly that rectangle (DW+1 2560, DH+1 448, MAGH 4). The
attract movie is not: it programs DW+1 2880, DH+1 480, MAGH 3 from the same
corner, which is 720 pixels by 240 lines per field, so 80 columns on the right
and 16 lines per field at the bottom fall outside the frame and are cropped.
How much of them a television would have shown behind its own overscan is a
property of the set, not of this port.

The left and the top of the movie's picture are cropped by the game itself,
not by the frame. Its `DISPFB2` carries DBX 36, DBY 12, so the CRTC starts
reading 36 columns in and 12 lines per field down: the picture's left 36
columns and top 24 rows are never displayed on hardware either. That is the
game's own overscan allowance.

`window`, the default, asks the renderer to grow the frame until it contains every enabled
CRTC window instead of cropping on the right and bottom, and to read each
circuit from DBX 0, DBY 0 rather than from the offset the game programmed. So
the frame is the whole 720x480 buffer the movie's display window points into,
from its own origin. It is presented at the window's own raster aspect,
derived from the registers: DW+1 clocks against the 2560 clocks and DH+1
lines against the 448 lines of the NTSC 4:3 area, which is 4:3 for gameplay
and 1.4 for the movie. That keeps the movie's pixels the same size as
gameplay's (its 642 content columns span 2568 clocks, the width of the
gameplay picture), at the cost of a thin letterbox in a 4:3 window. PCSX2
stretches the same window to 4:3 instead, a 4.8 percent horizontal squeeze.
The picture is then framed by its own black borders: measured off a decoded
frame, 40 blank columns on the left and 38 on the right, 8 blank rows on top
and 17 on the bottom, so the content sits centred within a pixel horizontally
and about four rows above centre vertically.

Ignoring DBX/DBY is the one place any setting in this document overrides a
value the game supplied, and it is a presentation register rather than
anything the game reads back. It is stated here rather than hidden, it is
the default because it was chosen on the running movie against `crt`, which
stays one menu step away, and the `raster window (display.raster)`
startup line names it. Without it, the same content is flush against the top
and the right of the grown frame, because DBX 36 / DBY 12 is the game's own
overscan allowance and a frame that shows the whole buffer has no overscan to
spend it on. Gameplay is unaffected in either mode: its window fits the frame
and its `DISPFB` carries DBX 0, DBY 0.

`window` is a presentation choice and not a correction in the other direction
too: it moves the movie's origin relative to gameplay's, which is what `crt`
keeps.

`display.deinterlace` decides how the two fields of an interlaced scanout
become one output frame. It matters for the attract movie, whose MPEG is
interlaced video: the game splits each decoded 720x480 picture into an
even-row field buffer and an odd-row field buffer and flips `DISPFB2.FBP`
between them once per field, and the two row sets of one decoded picture were
themselves captured about 1/60 s apart (a decoded I frame shows comb teeth on
moving figures inside the single picture). The field pair is two moments, not
one still frame.

- `adaptive` is the renderer's own FastMAD filter: still parts
  are woven with the previous field and give back the full vertical
  resolution, moving parts come from the current field alone. Motion runs at
  the field rate.
- `bob`, the default, presents each field on its own (the movie's fields; see below), stretched to the frame height and
  offset by the half raster line the field sits at, which is what a CRT does
  with it. Full field-rate motion, half the vertical detail, and the shimmer
  bob always has on fine horizontal detail.
- `weave` pairs the two newest fields with no motion test. It is the
  only mode that shows all 480 source rows of a still picture at once, and on
  moving figures it reproduces the source's own comb.

`bob` and `weave` act on the movie's fields only: a field the game's own
display copy produced (gameplay and the title menu at render scale 1) is one
of two resamples of a single rendered frame, and the adaptive filter gives
that whole 448-row frame back, so those fields are always composed
adaptively whatever the setting says. The first field after switching into
`weave` is woven with itself, one frame, because the renderer's field history
was reset while it was skipped.

None of the three applies when the renderer scans out at high resolution
(`display.render_scale` 4 and up on a buffer the game drew into), because
that path is not deinterlaced at all. The mode in force is on the `scanout
internal` log line as `deint=`.

The window or fullscreen display size (`display.mode`,
`display.window_width`/`window_height`) is a separate, independent setting:
it is only the surface the finished scanout is fitted into, using
`display.fit` (letterbox, integer scale, or stretch) and `display.filter`
(linear or nearest). Neither of these settings changes a value the game
itself supplied, and neither does any other setting in this document except
`display.raster=window`, which ignores the DBX/DBY read offset as described
above and says so in its log line. There is no widescreen option and no
game-speed option; both are out of scope for this port by design.

The default is 1280x960, which is 640x480 doubled. Both are 4:3, the aspect
this backend presents at, so the window opens with no letterbox either way.
640x480 keeps a second meaning: it is the size the menu and launcher
documents are laid out against, and the UI's density-independent pixel ratio
is the surface height over 480 (`src/runtime/ui/ui.cpp`), so the default
window shows that layout at exactly 2x.

The menu offers the size as a list rather than as two typed numbers: the
integer multiples of 640x480 up to 3840x2880. A size the file holds that is
not one of them is kept and honored, and the list grows a leading
`custom (WIDTHxHEIGHT)` entry naming it, so the control never shows a size
the settings do not hold. Any value in [320, 16384] is still accepted from a
hand-edited file, and `display.remember_window_size` can still store the
size of a window that was dragged to some other shape.

## 7. The in-game menu

The menu opens and closes with a hotkey: F1 on the keyboard, Guide on a
gamepad by default, both rebindable via `input.keyboard.menu` and
`input.gamepad.menu` (hot: see section 2). The hotkey is consumed before it
reaches RmlUi or the pad, so it can never collide with a gameplay binding.

The game keeps running while the menu is open. `host/input.cpp`'s SDL
provider reports a default-constructed pad state instead of sampling the
real device: no buttons held, both sticks centered. That is the same report
a real, untouched controller would produce, not a fabricated one. Pausing
the simulation while the menu is up is out of scope for v1, because the
frame pacer is locked to the audio device's clock and pausing it cleanly is
its own piece of work.

That lock works off a 100 ms cushion (`RT_AUDIO_CUSHION_FRAMES`, 4800 frames
at 48 kHz): the audio device is primed with that much silence at open, and
the pacer steers the queue back to that depth. After a stall the pacer does
not throw the lost time away. It treats the frames the device is short of
the cushion as a debt and runs fields unpaced until exactly that much audio
has been put back, which is at most six fields of guest time, then resumes
normal pacing. It never waits at all while the queue holds less than one
field of mix and the sound task is still feeding the device. The cushion
is also the audio latency: sound plays 100 ms behind the field that mixed
it, up from 50 ms, a deliberate trade for absorbing stalls up to that long
without a gap. This is host-side pacing only: it decides when a field is
produced and changes no value the game supplied.

Scripted runs (`ICORECOMP_INPUT_SCRIPT`) never bring the menu up at all:
`main.cpp` skips `rt_ui_init()` entirely when that variable is set, so a
scripted run's input stays bit-identical to a build with no UI compiled in.

### Remapping

The menu's Input tab shows one table of slots per device (keyboard,
gamepad), each row a DS2 button or stick direction paired with the name
currently bound to it, plus a Rebind button. Pressing Rebind arms capture
for that one slot (`ui/ui_rebind.cpp`):

- **Key**: the next key pressed is stored as its `SDL_GetScancodeName`.
- **Gamepad button**: the next button pressed is stored as its SDL button
  string.
- **Gamepad axis**: the next axis pushed past 60% of full travel is stored
  as its SDL axis string with a trailing `+` or `-` recording which
  direction was pushed (60% is deliberately higher than the 25% press
  point an axis bind fires at, so a resting stick on a worn pad cannot be
  mistaken for a deliberate press).

Escape cancels an armed capture without changing anything. A capture that
receives no input within five seconds times out the same way. A name that
is already in use is rejected without being stored: the status line names
which slot already holds it, or says it is the menu key, and capture stays
armed so the user can try something else. An accepted capture is committed
and saved on the next field, going through the same `validate_binds()`
rules as any other settings change, so a name that slips past the capture's
own check (a slot that changed on another device mid-capture) is still
caught there and reported in the pane instead of silently applied.

Each device has its own reset, independent of the "reset to defaults"
button that touches the rest of the settings: resetting a device's
bindings restores that device's compiled-in table (section 2) without
touching the other device's bindings or any non-input setting.

### Supported RCSS subset

The overlay renderer (`src/runtime/ui/ui_render.cpp`) implements RmlUi's
`RenderInterface` for solid and translucent boxes, text, scissored regions,
and 2D transforms. It does **not** implement:

- clip masks (`EnableClipMask`, `RenderToClipMask`)
- layers (`PushLayer`, `CompositeLayers`, `PopLayer`, `SaveLayerAsTexture`,
  `SaveLayerAsMaskImage`), which covers `border-radius`, `box-shadow`,
  gradients, and container opacity, all of which RmlUi implements via layers
- filters and shaders (`CompileFilter`, `RenderShader`, and their `Release*`
  counterparts)
- file images: `LoadTexture` serves exactly one scheme, `logo:` (the
  launcher's title image, section 8), out of memory. Nothing reads an image
  file off disk. Every other `src` logs once, naming the source, and the
  element draws untextured.

Textured geometry itself is implemented, so `<img>` and image decorators do
draw, but only against a texture this renderer can produce: one generated at
runtime (`GenerateTexture`, which is how text and RmlUi's own rasterized
shapes arrive) or the `logo:` image.

Anyone editing a stylesheet under `ui/` should stay inside that subset.
The drop-down arrow is one consequence of it: RmlUi's own samples draw
`selectarrow` with `decorator: image(...)`, which this renderer would draw
as nothing, so `ui/style/base.rcss` makes the arrow out of a zero-sized box
with a coloured top border and transparent side borders, which RmlUi mitres
into a triangle.
Reaching one of the unimplemented functions is not a crash: each one logs a
single line the first time it is hit, naming itself
(`RenderInterface::<Function>`), and the affected element simply draws
without that effect.

Text that this project cannot bound (a settings path, a disc path, a
binding name, the build identity) is clipped with an ellipsis rather than
allowed to run out of its box. RmlUi resolves `text-overflow` against the
element that holds the text, and only when that element's own overflow is
not visible, so the three properties always appear together:

    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;

Everything else wraps. Column widths in `ui/style/base.rcss` are derived
from the longest string a column can hold at the mono face's 0.6em advance,
at a density-independent pixel ratio of 1, which is the 640x480 baseline.

### Fonts

The UI uses two faces, both required: a missing one disables the UI, and the
log names the file that failed.

- Playfair Display (a variable font), `ui/fonts/PlayfairDisplay[wght].ttf`,
  for names and titles: the nav names, the section titles and the launcher
  title.
- JetBrains Mono, `ui/fonts/JetBrainsMono-Regular.ttf`, for everything a
  value is read out of: labels, controls, values, hints, taglines, the
  footer, the footer credit and the field-rate readout. Every column width in
  `ui/style/base.rcss` is derived from this face's 0.6em advance and the
  longest string that column can hold.

Both are licensed under the SIL Open Font License. Each notice ships beside
its font, at `ui/fonts/PlayfairDisplay-OFL.txt` and
`ui/fonts/JetBrainsMono-OFL.txt`.

## 8. The launcher

A window shown before the game boots, letting the user pick a disc image
and reach the settings menu without a running game behind it. `main.cpp`
implements it as a gate: either the launcher runs first and the game boots
only after Start, or the process boots straight into the game the way it
always did.

### The gate

`main.cpp` checks these conditions in order; the first one that matches
decides against showing the launcher, and the rest are not checked:

1. This build has no UI (`ICORECOMP_UI` off at configure time).
2. `ICORECOMP_INPUT_SCRIPT` is set: a scripted run drives the pad from a
   file and must stay reproducible, which a window waiting for a click is
   not.
3. `ICORECOMP_MAX_VBLANKS` is set: a bounded diagnostic run, expected to
   boot and exit on its own.
4. `--no-launcher` was passed.
5. `launcher.show_at_startup` is `false`.
6. `ICORECOMP_GS` selects the dump backend: a dump run has nothing to draw
   the launcher into. This is answered from the variable alone
   (`rt_gs_backend_selects_live()`, `gs/gs_select.cpp`), before memory and
   the GS backend come up, because bringing the backend up opens and
   truncates the `ICORECOMP_GS_DUMP` file, which would happen before the
   cheap failures (no disc, a SHA-1 pin mismatch) get their chance to stop
   the run.
7. After memory and the GS backend come up (needed to answer this one),
   there is no live windowed backend in this run: a live backend that
   opened no window (headless), or a build with no paraLLEl-GS at all, has
   nothing to draw the launcher into.

If none of these stop it, the launcher runs. Either way, startup logs the
outcome and the reason on one line:

    launcher gate: <launcher first | boot straight into the game> (<reason>)

grep `icorecomp.log` for `launcher gate:` to see which path a given run
took and why.

### What the window shows

The launcher window (`ui/launcher.rml`, model and logic in
`src/runtime/ui/ui_launcher.cpp`) shows:

- **Disc path and source**: the image currently resolved to mount, and
  where it came from (`--disc`, `settings.json launcher.disc_path`,
  `config/local.toml [disc].path`, the decomp baserom, or "next to the
  executable"), or "no disc image found" with the full search list.
- **Browse**: opens the OS file picker via `SDL_ShowOpenFileDialog`,
  filtered to `.iso`/`.bin`. When no dialog backend is available on the
  platform (no portal, no zenity, or this build has no SDL), the button
  says so and the launcher falls back to a typed path field with a **Use
  path** button instead.
- **Clear**: forgets the saved `launcher.disc_path`, then re-runs the
  precheck so the screen shows whatever the search order finds next.
- **Start**: runs `rt_boot_precheck()` (`src/runtime/loader.cpp`), the
  same config-load-through-entry-point-lookup sequence a normal boot runs,
  but returning a human-readable error string instead of calling
  `rt_fatal()`. When it fails, the message is shown inline in the window
  instead of on a console the user may never see; when it passes, Start
  closes the launcher and boots the game, reusing the disc mount and ELF
  read the precheck already did.
- **Settings**: opens the same in-game menu described in section 7.
- **Quit**: closes the process without booting anything.
- **show-at-startup**: the on-screen checkbox for `launcher.show_at_startup`.
- **Footer**: the build identity on the left (the running executable's size
  and modification time, `rt_exe_identity()`), and on the right the credit
  "Nathanial Fine" after the defnf mark, which opens `https://defnf.com`
  through `SDL_OpenURL`. A build with no SDL, or a platform `SDL_OpenURL`
  cannot hand off to, logs why and shows that in the status line instead of
  doing nothing. The mark is drawn from plain boxes in the stylesheet,
  since the overlay renderer draws no file images (see the RCSS subset
  above). Third-party license notices ship in the package README, not in
  the window.

### The title image

The title block shows the game's own logo, built from the user's disc at run
time, and falls back to the word "ICO" in Playfair Display whenever it
cannot be. `.title-mark` in `ui/style/base.rcss` has a fixed height, so the
two states are the same size and nothing below moves when the image arrives.

The wordmark is drawn from the game's own art. Four files come out of the
`STGLOG.DF` archive of `DFDATAS/DATA.DF`: the letter meshes `model/I.p2o`,
`model/C.p2o` and `model/O.p2o`, and the title animation
`anim/title_start.bga`. `src/runtime/ui/title_logo.cpp` reads the outer table
of `DATA.DF`, inflates enough of `STGLOG.DF` to reach them
(`src/runtime/host/inflate.cpp`, a raw DEFLATE decoder written for this,
since nothing else in the tree links one), parses the PS2O meshes and the
three node transforms, and reduces them to a triangle list in a box that is
1.0 tall and as wide as the wordmark's own aspect, with a 4 percent border
included. The triangle count, the span and the aspect are whatever the
mounted disc gives; the `ui` log lines name them for the run, and no number
read off a disc is written down in this repository. That triangle list, not
an image, is what the cache holds.

The raster is a separate, cheap step: the list filled at whatever pixel size
is asked for, under one uniform scale, centred. There is no antialiasing. The
GS draws these polygons with it off, so the fill rule here is the hardware's:
a pixel belongs to the triangle containing its centre, and a centre on a
shared edge goes to whichever of the two triangles has it as a top or left
edge. Coverage is binary, so premultiplied alpha is just the colour or
nothing. The vertex colour is applied the way the GS does,
`Cv = Ct * Cf / 128` with saturation; all three meshes carry `0xFFFFFFFF`, so
the letters come out white.

The launcher asks for exactly the pixel box the overlay will draw across:
`.title-logo` is 238 by 56dp, mirrored as `kRtTitleLogoDpWidth`/`Height` in
`src/runtime/ui/title_logo.h`, times the context's dp ratio (the surface
height over 480, clamped to 1..4). A 2560x1920 surface is ratio 4, so 952 by
224 pixels; a box whose aspect does not match the wordmark's leaves a thin
margin rather than stretching it. One texel a pixel means the overlay's
`LinearClamp` sampler,
which is left as it is, has nothing to interpolate. A window-scale change
re-rasterises at the new size rather than scaling the image, which costs
about a millisecond because the geometry is already in memory, and the image
element's `src` gets a new suffix so RmlUi asks for the new texture instead
of reusing the cached one.

The node transforms are used as the file gives them and no fitted constant is
applied anywhere. That was settled with the glow sprites the title screen
draws behind the letters: each `_f` object is a unit quad carrying its
letter's glow, and the glow lands exactly on its letter only at the scale the
file carries. A search of the decomp found no scale applied to these objects
either. The letters are small next to their spacing because the wordmark is
tracked that widely. The glow itself is not drawn: it read as misaligned on a
real window and did not earn its place in a launcher panel. There is no
fallback placement: a title animation that cannot be read fails the build, and
the launcher keeps its text title, because standing in numbers written into
the source would both put disc-derived data in this repository and draw a
wordmark the mounted disc did not describe.

It runs on the launcher's own frame loop, not on a worker: the ISO reader is
a single unlocked file handle and the same loop can remount it from the disc
picker. When a disc is already resolved at startup, from `--disc`,
`settings.json`, `config/local.toml` or the folder next to the executable, the
whole thing happens **before the first frame is presented**
(`launcher_prepare_first_frame`), so the launcher's first painted frame
already carries the image and the panel is never seen to adjust. That holds
the window for about 85 ms on a cold cache and about 2 ms on a warm one. A
disc chosen later, through Browse or the path field, still goes the deferred
way, since the window has to be up for the user to choose it; the swap costs
nothing in layout terms because `.title-mark`, `.title-logo` and `.title-name`
are all the same fixed 238 by 56dp box with no margin, padding or border.

The cold cost is almost all in reaching the animation, which is stored near
the end of `STGLOG.DF`: the meshes are well inside the archive, but the
placement is read rather than hardcoded, so nearly all of it is inflated. The
cache that follows is a few kilobytes of geometry, not an image, so it stays
valid across window sizes.

The cache is `saves/title_logo.cache` next to the executable, falling back
to the per-user state directory when that folder is not writable. `saves/`
is in `.gitignore` and `tools/check_no_rom.sh` refuses it outright, so
decoded game pixels cannot reach a commit. The file is keyed on the disc
image's sector count, the location and size of `DATA.DF` on it, and a
version number that is bumped whenever the geometry or the cache encoding
changes; a key that does not match is rebuilt rather than drawn. The header
also carries a checksum over the payload, so a file of the right length for
the right disc with a torn body is rebuilt instead of being read as
geometry.

Every step logs under `ui` with its timing: `title logo: cache hit`,
`title logo: building from`, the offsets and sizes the run read for the
archive and the three meshes, `title logo: geometry built in`, and `no title
logo from this disc:` with the reason when it fails.

Any disc a user picks, through Browse or the typed path, is validated
(mounted and checked for `SCUS_971.13`) before it is written anywhere: only
a disc that mounts successfully is saved to `settings.json`'s
`launcher.disc_path`. A rejected candidate leaves the previous disc mounted
if it can be re-probed, and its failure reason is shown in the window.

`--disc <path>` locks the picker: `rt_iso_forced_path()` is non-empty, the
window shows that path as already chosen, and Browse, Use path and Clear
are all disabled, since the command line already said which file this run
uses.

`--no-launcher` skips the window entirely and boots straight into the game,
as if condition 4 above had matched (it is that condition).

### Presentation while the launcher is up

The launcher runs its own frame loop (`rt_launcher_run()`), separate from
the scheduler's per-field loop the running game uses. While that loop owns
the window, the present mode is forced to FIFO (mailbox would spin a core
presenting frames nothing is producing, since there is no guest clock
running yet), and the user's present mode from `rt_settings()` is restored
at hand-off, whether the launcher started the game or the window was
simply closed.

## 9. Log lines to look for

These are exact prefixes and phrases, all under the `main`, `settings`,
`ui`, `launcher`, `gs`, `audio`, or `prof` log components, worth grepping
`icorecomp.log` for:

| phrase | meaning |
|---|---|
| `settings: loaded from` | which file the runtime actually read |
| `is overridden by` | an environment variable is winning over settings.json for one key |
| `kept default` | a bad value in the file was rejected; the key stayed at its default |
| `settings.json.bad` | the file failed to parse and was preserved under this name |
| `no longer a setting` | the file still holds a retired key (`display.hires_scanout`, `input.trigger_threshold` or `input.rumble`); the line names it and what replaced it |
| `super-sampling` | the render scale the paraLLEl-GS backend was created with, whether super-sampled textures are on, and whether it asked for high-resolution scanout |
| `render scale applied live` | a render scale change from the menu reached the backend |
| `(display.raster)` | which output frame the scanout is built at, `crt` or `window`, and in `window` that the DBX/DBY read offset is ignored (section 6); logged at startup and again on every change from the menu |
| `(display.deinterlace)` | how an interlaced scanout is composed, `adaptive`, `bob` or `weave` (section 6); logged at startup and again on every change from the menu |
| `scanout internal` | the scanout geometry; its `ss=`, `hires=` and `deint=` fields are the super-sampling rate in force, whether the renderer actually scanned out at high resolution, and the `display.deinterlace` mode the field was composed with |
| `profiling on:` | the frame-time profiler is active, and how often it reports |
| `RenderInterface::` | a stylesheet under `ui/` used something the overlay renderer cannot draw |
| `title logo:` | the launcher's title image: a cache hit, or each step of building one from the disc, with timings (section 8) |
| `no title logo from this disc:` | the title image could not be built and the launcher kept its text title; the line says why |
| `launcher gate:` | which of the two boot orderings this run took, and why (section 8) |
| `rebind ` | a capture starting, ending, being accepted or rejected (section 7) |
| `previous run's log kept as` | the last run's `icorecomp.log` was renamed to `icorecomp.prev.log` before this run's log was opened |
| `could not keep the previous run's log` | the rename above failed and why; this run's log overwrote the previous one as before rotation existed |
| `gameplay.run_any_direction is on` | the left stick is being pre-scaled; a full tilt runs in every direction |
