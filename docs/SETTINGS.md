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
| mouse.\<slot\> | string | one of `left`, `right`, `middle`, `x1`, `x2`, `wheelup`, `wheeldown`, or `""` for unbound | see table below | hot | - |
| left_deadzone | float | [0, 0.95] | 0.0 | hot | - |
| right_deadzone | float | [0, 0.95] | 0.0 | hot | - |
| mouse_look | bool | - | true | hot | - |
| mouse_look_sensitivity | float | [0.05, 20] | 1.0 | hot | - |
| mouse_look_invert_y | bool | - | false | hot | - |

An unresolvable keyboard or gamepad binding name (a typo, a name from a
different SDL version) falls back to the compiled-in default for that one
slot (`rt_settings_default_binding()`), with a log line naming the slot and
the name that did not resolve; it never falls back to "no binding". The
mouse is different: `""` is a legitimate value there, meaning the slot is
unbound, and most mouse slots ship that way. A non-empty mouse name that is
not one of the seven above logs once and leaves the slot unbound, because
there is no default worth substituting.

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
not share a name, because one host input cannot press two DS2 buttons. The
mouse has no menu slot, so only the second rule applies to it, and two
unbound mouse slots never count as sharing a name.

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
| triangle | Space | r2 | 3 |
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

Default mouse bindings (`kMouseBinds`, `settings.cpp`): square = `left`,
r1 = `right`, every other slot unbound. The names are this runtime's own
(`host/mouse_names.h`), not SDL strings, and compare case-insensitively.
Held buttons press their slot for as long as they are held. A wheel bind is
a pulse: each wheel tick presses its slot for one field and releases it for
one field, so two ticks are two presses, which is what the game's
pressed-this-frame detection needs. Nothing is bound to the wheel by
default on purpose: the game turns D-pad bits into stick motion, so a wheel
on the D-pad would walk the player. Mouse binds only fire while the window
has keyboard focus, and they are suspended entirely while the pointer owns
the mouse (section 10), where left click confirms and right click cancels
regardless of what the gameplay slots say. Losing focus also clears the
held state: a button released over another window sends no release event to
this process, so the bit is forgotten rather than left set to read as a
press when focus comes back.

### Mouse look

`input.mouse_look` (on by default) feeds mouse motion to the right stick,
which is ICO's camera stick. While it is on, the window captures the
pointer in relative mode with the cursor hidden whenever the window has
focus and the settings menu and launcher are closed; each transition is
logged as `mouse look: captured` or `mouse look: released` with the reason.
Focus loss and opening the menu release it, and those are the only two
things that do.

The game's own menus do not release it. While the pointer owns the mouse
there (section 10) relative mode stays on and the OS cursor stays hidden;
the field's motion moves a cursor the overlay draws inside the picture
instead of the camera stick, which sees no motion at all until the menu is
gone. The switch each way is logged as `guest menu: pointer takes the
mouse` and `guest menu: pointer hands the mouse back to mouse look`.

The camera stick is position control in the game, not velocity. Traced
through the decomp: the camera's stick reader (`func_00189D68`,
`src/camera-ico2.c`) multiplies the stick's unit vector by the response
`iosPadGetStick` returns and hands that to `func_00194EC0`, which calls
`ActSendMail_WithAdditionalData`. The manual camera is two accumulated
angles that `GetMailAdditionalData` turns into rotations of the camera about
its target, and the stick sets where those angles should be (full deflection
is 120 degrees of yaw around the target, the limit `func_00194E28` installs),
not how fast they should change.
Each camera update moves them toward that target by at most 2.0 degrees
while the response is 0.1 or more and 1.5 degrees below it, and by a tenth
of what remains once inside ten of those steps. So a stick held still holds
the camera still and off centre, and a released stick is a target of zero
that the camera walks back to at 45 degrees per second.

Nothing on that path treats a neutral stick as an event. The immediate snap
back to centre in the code (`ClearMailAdditionalData`) is reached only by a
mode word, by `func_00153FE8` reading zero, or by a camera set that forbids
the manual camera, never by the stick reading neutral. There is no timer, no
release edge and no mode flag: the two angles are the whole state, so a
single neutral field between two deflected fields resets nothing and costs
one step of return.

So the mouse drives a virtual stick, not a speed. `RtMouseLookStick`
(`host/mouse_look.h`, exercised by `icorecomp-mouse-look-selftest`) keeps a
stick vector `V` on the unit circle and is stepped once per field, motion or
not, including the catch-up fields after a long frame:

- Each field's accumulated motion moves it: `V += (dx, dy) * sensitivity /
  160`, scaled back onto the unit circle when that pushes it out, which is
  the stick's gate. 160 px of drag is full deflection at sensitivity 1: an
  eighth of the 1280 px default window and 0.2 in of hand travel on an 800
  dpi mouse, for the 120 degrees of camera offset the game allows.
  Sensitivity divides that distance: 320 px at 0.5, 80 px at 2.
- Motion is integrated, so nothing depends on how the pixels landed across
  field boundaries: 16 fields of 5 px and one field of 80 px reach the same
  deflection.
- Dragging and then holding the mouse still leaves `V` where it is, and the
  camera parks at that offset exactly as it would for a held stick.

The stick vector then becomes the byte pair, shaped for the game's own
stick chain:

- The radius ramps linearly with `|V|` from a floor of 49 to 122. From the
  decomp's stick routine (`ios/pad.c`): a radial dead zone of 48, the
  octagonal-gate divisor `1 + 0.2 * t / 45`, saturation at 120 and a linear
  response `(mag - 48) / 72`. The floor is the first whole unit past the
  dead zone and is the same at every angle, so the smallest deflection a
  drag can leave asks for the same small camera offset (about 0.02 of
  response, under two degrees) whichever way it points.
- The camera's stick reader also computes a square dead zone, zeroing the
  pair when both components are within 0.4 of centre (51.2 units), and then
  never reads the result: only the radial 48 gates the retail camera, which
  is what lets the floor be 49 rather than 52 on a cardinal and 73.5 on a
  diagonal. What remains of that spread is the byte grid, about 1.3 units of
  the 72 the response spans, printed by check 5 of the selftest.
- The top is 122 (120 plus the byte rounding margin measured by the
  selftest, so a fully deflected stick saturates at every angle), and the
  pair is pre-multiplied by the same octagonal divisor the game is about to
  apply, so a diagonal reaches the same deflection as a cardinal. Bytes
  round away from centre; the byte square is the only cap.
- Mouse up (negative SDL `yrel`) gives a byte below 128, the game's "stick
  up", unless `mouse_look_invert_y`.

Letting go is the other half. A hand on a mouse stops moving all the time in
the middle of a drag, so stillness is read in two stages:

- Hold: `RT_MOUSE_LOOK_HOLD_FIELDS`, 15 fields (250 ms), of no motion do not
  touch `V` at all. The stick stays where the drag left it.
- Release: after that, `V` walks linearly to centre over
  `RT_MOUSE_LOOK_RELEASE_FIELDS`, 6 fields (100 ms), and the last of those
  reports a released stick. The game's own return then walks the camera back
  at 45 degrees per second. Motion during the release picks up from where it
  had got to rather than jumping.

So a drag is reported for 20 fields (333 ms) after the last motion. There is
no exponential average of the delta and there should not be: `V` is an
integral, so it already does not care how a hand's pixels landed across
fields, and a filter would only add lag. The stick is centred outright,
with no release ramp, when capture is lost, when the pointer takes the mouse
for the game's own menus, and while the settings menu is up.

Neither constant has a settings key yet. `input.mouse_look_hold_ms` (0 to
1000 ms, default 250) and `input.mouse_look_drag_pixels` (40 to 1000, default
160) are the two to wire.

Precedence per axis is the keyboard right-stick keys while held, then the
mouse while its stick is off centre, then the gamepad stick. The delta is
sampled once per emulated field and discarded while the settings menu is
up, so nothing accumulates behind the menu and flings the camera on close.
Scripted runs (`ICORECOMP_INPUT_SCRIPT`) never see the mouse.

### Last device used

The SDL provider tracks which kind of device the player last used, once per
field, and `rt_input_last_device()` (`host/input.h`) reports it. A held bound
key, a mouse button, a wheel tick or real pointer motion says keyboard and
mouse; a held pad button, an axis bind past its press point (raw 8192 of
32767) or either stick pushed a quarter of the way from centre says
controller. A field that shows both leaves the answer alone, because there is
no honest winner between them, and so does a field that shows neither, so it
names the last device that was unambiguously used rather than the one in a
hand right now.

It boots as the controller, so a run where nobody touches a mouse never draws
a cursor, and a scripted run or a build without SDL leaves it there for the
whole run. Each change is logged as `last device is now the controller` or
`last device is now keyboard and mouse`.

The one consumer is the drawn cursor on the game's own menus (section 10): it
is shown only while the last device is keyboard and mouse, on top of the
conditions that were already there. Nothing else changes with it. A pad press
hides the arrow, the next mouse motion brings it back, and the pointer's own
state is untouched in between, so the arrow reappears where the player left
it rather than where the pad walked to.

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

`menu_hit_editor` was retired when the pointer on the game's menus started
reading the game's own scene objects (section 10): there are no hit boxes to
author. A file that still holds the key keeps it, logs `no longer a setting`
and does nothing with it.

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
real device: no buttons held, both sticks centered, and mouse look releases
the pointer so the menu can be clicked. That is the same report
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
gamepad, mouse), each row a DS2 button or stick direction paired with the
name currently bound to it, plus a Rebind button (and, on the mouse, an
Unbind button that stores `""`). Pressing Rebind arms capture for that one
slot (`ui/ui_rebind.cpp`):

- **Key**: the next key pressed is stored as its `SDL_GetScancodeName`.
- **Gamepad button**: the next button pressed is stored as its SDL button
  string.
- **Mouse button or wheel**: the next mouse button pressed, or the next
  wheel tick, is stored under its name from the table above. A platform
  that reports the wheel already inverted (SDL's flipped direction) is
  un-flipped first, the same rule the gameplay wheel binds are read by, so
  the stored name is the direction the user scrolled. The click that
  pressed Rebind itself is never captured: RmlUi fires the click on the
  release, so the capture only ever sees a fresh press, and the release of
  the captured press is swallowed so the menu does not act on it. While a
  mouse capture is armed the pointer is held: motion, buttons and wheel go
  nowhere else until it ends.
- **Gamepad axis**: the next axis pushed past 60% of full travel is stored
  as its SDL axis string with a trailing `+` or `-` recording which
  direction was pushed (60% is deliberately higher than the 25% press
  point an axis bind fires at, so a resting stick on a worn pad cannot be
  mistaken for a deliberate press).

Escape cancels an armed capture without changing anything. A capture that
receives no input within five seconds times out the same way, and closing
the menu drops one that is still armed. A capture that was already accepted
is not dropped by the menu closing: the name is written at the coming field
boundary whether the menu is still up or not. The two resets and the mouse
Unbind do drop it, because they are rewriting the same table and applying
it afterwards would undo them. A name that
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
into a triangle. The drawn cursor on the game's own menus (section 10) is
the same trick twice, a dark triangle with a lighter one over it.
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

The same image feeds a second one. The moment it is published, the letter I
is cut out of it and turned into the cursor the pointer draws on the game's
own menus (`src/runtime/ui/cursor_image.cpp`, section 10). That happens here,
where the image exists, rather than where the cursor is drawn, so the cursor
is ready before the game boots and is re-cut whenever the wordmark is
re-rasterised for a new window scale. It costs about a millisecond and it is
never fatal: a failure logs and the cursor stays the arrow drawn out of
borders.

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
`ui`, `launcher`, `gs`, `audio`, `input`, `guest`, `json` or `prof` log
components, worth grepping `icorecomp.log` for:

| phrase | meaning |
|---|---|
| `settings: loaded from` | which file the runtime actually read |
| `is overridden by` | an environment variable is winning over settings.json for one key |
| `kept default` | a bad value in the file was rejected; the key stayed at its default |
| `settings.json.bad` | the file failed to parse and was preserved under this name |
| `no longer a setting` | the file still holds a retired key (`display.hires_scanout`, `input.trigger_threshold`, `input.rumble` or `debug.menu_hit_editor`); the line names it and what replaced it |
| `super-sampling` | the render scale the paraLLEl-GS backend was created with, whether super-sampled textures are on, and whether it asked for high-resolution scanout |
| `render scale applied live` | a render scale change from the menu reached the backend |
| `(display.raster)` | which output frame the scanout is built at, `crt` or `window`, and in `window` that the DBX/DBY read offset is ignored (section 6); logged at startup and again on every change from the menu |
| `(display.deinterlace)` | how an interlaced scanout is composed, `adaptive`, `bob` or `weave` (section 6); logged at startup and again on every change from the menu |
| `scanout internal` | the scanout geometry; its `ss=`, `hires=` and `deint=` fields are the super-sampling rate in force, whether the renderer actually scanned out at high resolution, and the `display.deinterlace` mode the field was composed with |
| `profiling on:` | the frame-time profiler is active, and how often it reports |
| `RenderInterface::` | a stylesheet under `ui/` used something the overlay renderer cannot draw |
| `title logo:` | the launcher's title image: a cache hit, or each step of building one from the disc, with timings (section 8) |
| `no title logo from this disc:` | the title image could not be built and the launcher kept its text title; the line says why |
| `menu cursor:` | which cursor the pointer draws on the game's own menus: the letter I cut out of the title image, with its size, hotspot and which end was found to be the point, or `no logo image; drawn arrow` (section 10) |
| `launcher gate:` | which of the two boot orderings this run took, and why (section 8) |
| `rebind ` | a capture starting, ending, being accepted or rejected (section 7) |
| `mouse look: captured` / `mouse look: released` | relative mouse mode was taken or freed, and why (section 2, mouse look); the game's own menus do not free it |
| `last device is now the controller` / `... keyboard and mouse` | the player picked up the other kind of device (section 2, last device used); the drawn menu cursor follows it |
| `guest menu: pointer takes the mouse` / `guest menu: pointer hands the mouse back` | the field's mouse motion switched between the drawn cursor on one of the game's menus and the camera stick; the take line says where the drawn cursor is (section 10) |
| `guest menu: layout ... chain` | the layouts one of the game's menu screens is composed from, and how many selectable items they came to between them; once per screen (section 10) |
| `guest menu: layout ... item ... rect` | where the pointer put each selectable item of one of the game's menu screens, in fractions of the presented picture; one line per item, once per screen (section 10) |
| `guest menu: select` | the pointer made the item under the cursor the selected one by writing the game's own selection word (section 10) |
| `guest menu: the game is swallowing` | the game set its one-frame no-navigation flag `D_00633160` and the pointer deferred its write to the next field; once per run, and expected at a screen change (section 10) |
| `guest menu: click` | the pointer clicked one of the game's menu items: cross on the item named (section 10) |
| `previous run's log kept as` | the last run's `icorecomp.log` was renamed to `icorecomp.prev.log` before this run's log was opened |
| `could not keep the previous run's log` | the rename above failed and why; this run's log overwrote the previous one as before rotation existed |
| `gameplay.run_any_direction is on` | the left stick is being pre-scaled; a full tilt runs in every direction |
| `guest menu: layout ... item ... fade ... mcsel ...` | the game's own menu state (layout id, selected item, fade/transition state, memory card selector index) as read out of guest RAM by the host, logged once per change |

## 10. The pointer on the game's menus

ICO's own menus (the title screen's continue or new game choice, the memory
card check, the ten-slot load grid, the save flow, the vibration question)
are scene objects driven by the D-pad, cross and triangle; the game has no
pause menu. Nothing is authored on the host side to point at them. The game
already holds, in RAM, which screen is up, which items that screen has, and
where on the picture it draws each of them, so the host reads all three and
makes the item under the cursor the selected one by writing the game's own
selection word. Cross, triangle and the wheel are virtual pad presses, the
same bits a controller would carry. No guest code is patched, and every word
written is one the game's own navigation writes.

### What the host reads

The words and tables in guest RAM listed in `src/runtime/guest/ico_syms.h`
(the second approved address-fact exception in CLAUDE.md): the current layout
id (which menu screen is up), the current item id, the layout fade state, the
memory card check screen's selector index, the layout table those index into,
and the scene objects each layout entry's range names.
`guest/menu_nav.cpp` reads them once per pad field and logs the tuple
whenever it changes:

    guest menu: layout 0x9 item 0xe fade 2 mcsel 0

Fade 2 is the interactive state: `lt_link_layout` draws a highlight only at
2 and `lt_switch_layout` applies cross and triangle only at 2. 3 and 5 are
the two fading states. Gameplay sits at 2 as well, which is why fade alone is
not the predicate.

### Where an item is on screen

`lt_link_layout` is handed one scene object per frame and draws it from seven
whole numbers in the object: a width (`+0x48`, or the texture's width at
`+0x58` when it is zero), a height (`+0x44`, or `+0x54`), a centring flag
(`+0x40`), and an x and y (`+0x50` and `+0x4C`). Those live in the game's own
2D layout space, which is 640 by 224 with the origin at the centre of the
picture: x is `+0x50 - 320`, or `-width/2` when the centring flag is set, and
y is `+0x4C - 113`. The height fields count half units, which is the shift by
3 against the width fields' shift by 4.

Those numbers make two boxes half a unit apart. `(x, y)` to
`(x + w, y + h/2)` is the box the highlight is scattered over. The quad the
object itself is drawn as adds 8 to the Y and subtracts 8 from the H, both in
GS 12.4 fixed point, just before `gif_StartPacketPath1`, so it runs from
`(x, y + 0.5)` to `(x + w, y + h/2)`. The pointer reports the drawn one; half
a unit is 0.002 of the picture's height, below the resolution of the hit test
and of the boxes the mapping was calibrated against.

That space maps onto the presented picture without the frame buffer's size
entering into it. `func_0010FF28`, the sprite emitter this path calls, scales
every coordinate by the frame buffer's width over 640 and its height over
224 and adds a base of 2048; the frame's `XYOFFSET` is
`(2048 - width/2, 2048 - height/2)`, which the GS subtracts. The two cancel,
and a layout-space `(x, y)` is at

    nx = 0.5 + x / 640      ny = 0.5 + y / 224

of the picture. `guest/menu_nav.cpp` carries the derivation and the decomp
citations.

Calibration. On the title screen's continue or new game choice (layout 0x9,
items 0xe and 0xf) the two items were measured by hand on the presented
picture as x 0.399..0.607, y 0.595..0.685 and x 0.400..0.598, y 0.725..0.821.
The mapping above, run over that screen's own scene objects, gives
0.400..0.600 / 0.598..0.688 and 0.400..0.600 / 0.732..0.821: the largest
disagreement on either axis of either item is 0.007 of the picture.
`icorecomp-menu-nav-selftest` carries the same check.

Every screen says its rectangles the first time it is interactive, one line
per item (the layout named is the one that owns the item, see the chain
below), so a log from a real run can be held against what was on the screen:

    guest menu: layout 0x9 item 0xe rect 0.4000,0.5982,0.6000,0.6875
    guest menu: layout 0x9 item 0xf rect 0.4000,0.7321,0.6000,0.8214

### A screen is a chain of layouts, not one

`lt_next_layout` does not work on the current layout alone. It walks the
current layout's parent chain by each entry's `+0x30`, and then, in order:
runs each ancestor's action function (`+0x20`) and `lt_switch_layout`,
farthest ancestor first; runs `lt_prev_layout` for the current layout, which
is what draws its objects; and finally runs `lt_link_layout` over every
ancestor's whole object range, nearest ancestor first. So every layout in the
chain is live, every one is drawn, and each keeps its own current item.

A page's items therefore need not belong to the layout id the state word
reports. The load file select page is the case that forced this: the current
layout is 0x10, whose own nine objects have no default and no current item,
and its chain is `0x10 <- 0xb <- 0xd <- 0xc`. The ten save slots (objects
0x1b..0x24) belong to 0xb (`_la_set_preview_info`), which is where their
selection word lives and where the custom handler installed in `D_00633164`
(`exec_layout_texture`, put there by 0x10's own action function) moves it.
The save file select page 0x1d has the same chain. Several more screens in
the load and save flow are the same shape: 0x19, 0x2d and 0x2f take their two
items from 0x17, 0x1f and 0x20 from 0x18, 0x25 from 0x26, 0x15 from 0x17.

The pointer writes the current item of the layout that owns the item under
the cursor. The item-id mirror `D_00633150` is written only when that layout
is the current one, because the game loads the mirror from each chain layout
in turn and the current one is last, so after a frame it holds that one's
field and nothing else's.

Each screen logs the chain it resolved, once, the first time it is
interactive, and each rectangle line names the layout that owns the item:

    guest menu: layout 0x10 chain 0x10 <- 0xb <- 0xd <- 0xc: 10 items
    guest menu: layout 0xb item 0x1b rect 0.3625,0.3080,0.3875,0.3795

### Which items a layout has

A layout entry's scene object range (`+0x00` up to `+0x04`) is everything it
draws, decoration included. The items are the ones the game's own navigation
can reach: start from the entry's default item (`+0x28`) and its current item
(`+0x2C`) and follow the four neighbour links (`+0x2C` right, `+0x30` left,
`+0x34` down, `+0x38` up), staying inside the range. An object whose `+0x68`
has bit 1 set is skipped, because `lt_link_layout` returns without drawing
it, and so is one whose rectangle lands entirely off the picture. Bit 1 is
the per-frame half of that field: `lt_next_layout` seeds it from bit 0 for
every object in the chain at the top of each frame, and `func_001B7218` may
overwrite it for one object mid-frame. A hidden object is still walked
through, since it can sit between two visible ones.

A layout whose `+0x2C` is negative contributes nothing, even when objects are
reachable from its default item. That is the game's own gate:
`lt_next_layout` skips `lt_switch_layout` for such a layout,
`lt_switch_layout` returns immediately on one, and `lt_link_layout`
highlights nothing, so writing an item into it would hand it a highlight and
a navigable selection the game did not have. The chain log line is followed
by a line naming any layout skipped this way.

The memory card check screen is keyed differently, for the same reason its
selection word is different: its fifteen card positions are scene objects
0x158..0x166, reached by `_la_memory_card_check`'s own loop rather than by
any link, and an item's value there is its selector position 0..14. The
screen is recognised by its object range covering all fifteen of them, which
in the retail layout table only layout 0x38 does; it is reached by cross on
object 0x118 of layout 0x36 (`_la_mcard_error_check`), not from the retail
load or save flow. Bit 1 of `+0x68` is not tested on those fifteen:
`_la_memory_card_check` sets bit 0 on all of them and clears it on the one
its selector names, and `lt_next_layout` copies bit 0 into bit 1, so all but
the selected read as hidden. Bit 0 itself is general and not that screen's:
`GetRealModelId(index, flag)` sets it to `flag & 1` on any scene object and
leaves every other bit alone, and the retail title screen ships items 0xe and
0xf with it set. Lighting one of fifteen card positions is one use of it, and
on that screen it says nothing about where the fifteen places are.

### When the pointer owns the mouse

`rt_guest_menu_active()` is true when the reads are valid, the fade state is
2, and some layout in the current screen's chain has at least one selectable
item with a rectangle on the picture. Gameplay (layout 0x32) and the pre-title
cinematic (0x33) both hold fade 2, and the second term excludes them
structurally rather than by name: the retail layout table gives each an empty
scene object range, a default and current item of -1 and no parent, so the
chain is one layout with nothing to reach and no rectangle to derive.

While the predicate is true the pointer owns the mouse: the field's motion,
buttons and wheel go to the pointer, the camera stick sees no motion, and the
gameplay mouse bindings are suspended.

Two host-side conditions have to hold as well before the pointer writes or
hovers anything, and neither of them is about the game. The SDL input
provider has to be the live one: a run driven by `ICORECOMP_INPUT_SCRIPT` is
required to stay bit-identical, and one with a window would otherwise have
the OS cursor moving the game's selection out from under the script. And the
settings overlay has to be closed: it releases relative mouse mode, so the OS
cursor lies over the picture, and a drag across the overlay would move the
selection on the game's menu underneath it. With either shut the field writes
nothing, hovers nothing, starts no press and drops whatever was queued, the
same way `host/input.cpp` neutralises the pad while the overlay is up. The
state read and the change log are not gated, because they write nothing.

One frame belongs to the game whatever the cursor is doing. `D_00633160` is
a one-frame flag that swallows navigation: while it is non-zero
`lt_switch_layout` returns at once, `lt_next_layout` skips `lt_switch_layout`
for every layout in the chain, and `lt_link_layout` draws no highlight;
`lt_next_layout` clears it on the way out. It is set on the frame a fade
completes into an interactive screen (`lt_current_property_item`), on a load
or save page whose preview info is not ready (`_la_set_preview_info`), and by
the title's `kanbanBoot` setup when there is no save to continue from. The
pointer's write does not go through `lt_switch_layout`, so it defers for that
field and takes the hover again on the next one; the one-write-per-hover rule
keys on the write actually happening.

Relative mouse mode does not change across that boundary. It is on whenever
mouse look is on, the window has focus and the overlay is closed, which means
the OS cursor is hidden on the game's menus too, so the pointer carries a
cursor of its own: a position in the presented picture, moved by the same
relative motion the camera stick would otherwise have had, clamped to the
picture so it cannot leave it, and drawn by the overlay (`ui/cursor.rml`)
with its point on the position the hit test uses. It starts at the centre of
the picture the first time the pointer takes the mouse and keeps its position
after that, so a menu left and returned to has the cursor where it was left.
The two log lines say which happened:

    guest menu: pointer takes the mouse, drawn cursor at centre
    guest menu: pointer hands the mouse back to mouse look

What it draws is the game's own letter I. `src/runtime/ui/cursor_image.cpp`
takes the title image the launcher built from the disc (section 8),
thresholds its alpha, labels the connected components and picks the leftmost
one that is at least half as tall as the tallest, which on the wordmark is
the I and never a speck. It finds which end of that glyph is the pointed one
by comparing the mean number of opaque pixels per row over the top fifth of
it with the same over the bottom fifth, taking the narrower end, and taking
the top when the two are within 15 percent of each other. On the retail disc
the narrow end is the bottom: the wordmark's I is a horn, wide at the top and
tapering to a point. That end is turned upwards and the glyph's long axis put
about 22 degrees off vertical, so the point is at the top left and the body
falls away below it and to the right, which is where a desktop arrow cursor
sits. The rotation and the scale are one bilinear resample of the
premultiplied pixels, and one pixel of dilated alpha in `#0a0a0a` goes
underneath as an outline, so a white glyph reads over a light picture; a disc
whose letters came out dark gets a light outline instead, decided from the
mean luminance of the glyph's own pixels.

The result is cut down to what was actually drawn and published in memory
under the `cursor:` scheme, alongside the title image's `logo:`. Its hotspot,
the point the tip landed on, is exact rather than rounded: the resample's
translation is nudged by the fraction of a pixel that puts the tip on a whole
one. The document offsets the image by minus that hotspot, in dp, so the
point sits on the cursor position. The glyph's own long axis is 32dp, so with
the tilt and the outline the image comes out about 30 by 20dp. It is cut at
the window's density, like the title image, and re-cut when that moves, so it
is never a scaled copy of a cut made for another window size. One `ui` line
says what a run is using:

    menu cursor: the logo's I, 82x120 px, hotspot (2,2), tip at the bottom

The arrow built out of borders in `ui/style/base.rcss`, a light triangle over
a dark one, is what is drawn whenever there is no such image: a disc that
gave no title image, a GPU upload that failed, or a run that never showed the
launcher, since that is the only place the title image is built. `menu
cursor: no logo image; drawn arrow` is the line for it.

Whenever relative mode is not on, which is mouse look turned off and the
settings menu opened over a game menu, there is no hidden cursor to stand
in for: the pointer follows the OS cursor's position in the picture instead
and nothing is drawn over it, so there is never a second arrow next to the
system one. With mouse look off that is the state for the whole menu, and
the log says so (`guest menu: pointer takes the mouse, following the system
cursor`). Everything downstream, the hover and the click, reads whichever
of the two is answering and does not care which.

### How a click happens

Hovering an item that is not the selected one makes it the selected one at
once, in a single write, and the game carries on from there as if its own
navigation had landed on that item. That is a decision of the user's, taken
on 2026-09-03, and it is the one host feature that writes a value the game
itself computed; CLAUDE.md records it. What is written, and why that is the
whole of it:

- On every screen but the memory card check, the word is the current-item
  field of the layout table entry of the layout that owns the item
  (`D_0053C020 + layout * 0x38 + 0x2C`), which is the current layout or one
  of its ancestors in the chain above. The game re-derives the highlight from
  that field on every frame:
  `lt_prev_layout` calls `lt_link_layout` once per scene object the layout
  owns, and `lt_link_layout` draws the highlight sprite for the object whose
  index equals that field. A D-pad press does nothing more than write it, in
  `lt_switch_layout`. The item-id mirror `D_00633150` is written alongside,
  because the game refreshes it from the same field at the top of every frame
  and a reader in between should see the two agree.
- On the memory card check screen the word is the selector `D_00274EC0 +
  0x2C`. That screen's handler `_la_memory_card_check` steps it with LEFT and
  RIGHT, clamped to 0 and 14, and calls `GetRealModelId` for all fifteen
  positions every frame to light the selected one, so the selector alone
  decides the highlight and the layout entry is left alone.

The write happens on the field the cursor enters an item and not again while
it stays inside it, whatever the game then does with the selection. The load
grid moving on from an empty save slot to the next occupied one is that case:
it is the game's own rule, and a pointer that re-wrote every field would
fight it forever. The line in the log is

    guest menu: select item 0xf on layout 0x9 (was 0xe)

A write that cannot be shown correct does not happen. An item outside the
layout's own scene object range can never be the highlighted one, so it is
refused with `guest menu: item 0x25 is outside layout 0xb's items
0x1b..0x24; not written`, and a selector position outside 0..14 the same
way. A click that
needed such a write presses nothing rather than confirming the item the game
still has selected.

A left click selects the item under the cursor if it is not already selected,
then presses cross (`guest menu: click item 0x1b on layout 0xb`). The two
land in the same field, which is the right order: `lt_switch_layout` resolves
the object cross applies to from the current-item field on entry, before it
looks at that frame's pad bits, and the pad tick that writes the selection
runs before the pad frame the game reads. The cross is in that field's bits
because the click starts the press itself when no press is already in flight,
rather than leaving it to the next field's tick: the SDL provider runs the
tick, then the button events, then reads the press bits, all for one field. A
field of delay would put a whole game frame between the write and the cross,
and on the load grid the game moves the selection off an empty slot inside
that frame. A click over no item presses nothing at all: there is no item to
confirm, and a cross would confirm whatever the game happens to have selected
elsewhere on the screen.

A right click presses triangle wherever it happens. Each wheel tick presses
one D-pad step, in the direction the selected object's own links point: LEFT
and RIGHT when it has a left or a right link, UP and DOWN when it has only an
up or a down one, and nothing at all when it has none. The direction is read
off the first layout in the chain that contributed items, which is the one
the player is steering; the press itself reaches every layout in the chain,
because the game runs `lt_switch_layout` for each of them. The memory card
check screen's handler only reads LEFT and RIGHT.

Every press is one field of bits followed by one field of zero, which is the
edge the game's pressed-this-frame word needs, and goes through the same
virtual pad as a real controller (`rt_guest_menu_pulse_bits`, OR'ed in by the
SDL provider only; scripted runs never see it).

One difference from a player's D-pad move: `lt_switch_layout` plays the
cursor sound only when it applied the move itself, so a hover is silent.
Playing it would mean calling guest code.
