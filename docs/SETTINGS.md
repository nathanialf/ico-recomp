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
   or `~/.config/icorecomp` on Linux, `%LOCALAPPDATA%\icorecomp` on Windows,
   `~/Library/Application Support/icorecomp` on macOS. macOS has no
   config/state split, so that one directory is both the config directory and
   the state directory, the same way `%LOCALAPPDATA%\icorecomp` is on Windows
   (`rt_user_config_dir` and `rt_user_state_dir`, `src/runtime/host/portable.h`).
   It is also where a quarantined app bundle ends up keeping everything,
   because macOS then runs the bundle in a container and the folder next to
   the executable is not writable; see docs/MACOS.md.
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
- **cold, restarts**: `debug.console` and `debug.log_file`. Changed from the launcher only; the program restarts to
  apply it.

A hot setting needs no applier when its consumer reads `rt_settings()` fresh
at every use; `settings_apply.cpp` marks those "nothing to push here" rather
than calling them cold.

Two cold keys have a consumer that runs once, before there is anything to
push to: a console cannot be attached to a process that has already decided
it has none, and the log sink is opened once from the same startup peek.
Rather than leave such a change sitting in the file, the runtime applies it
by restarting itself, which is why both controls are offered in the menu
only while the launcher is still up. The commit saves `settings.json`, ends
the run through the shutdown a quit takes, and starts a new process on the
same command line and working directory. Once the game is running the two
controls are disabled, and a commit that moved either key anyway is reverted
with a log naming it.

There were three until 2026-09-05. The GS backend of a run is also built
once, at `rt_hw_init()`, and `display.backend` was the third; it was retired
with the native renderers (below, "Renderer backends"), so nothing loads it
and no control moves it. `RtSettings` carries no field for it either, so its
arm of the restart rule is gone rather than merely unreachable.

`launcher.*` is cold too and restarts nothing: those keys only decide what
the next launch's launcher does, and they are changed in the launcher's own
window, where restarting to apply "show this at startup" would throw away
the screen the user is looking at. They take effect the next time the
program is started, as they always did. So does a cold key edited by hand in
`settings.json` while the game is running: nothing rereads the file, and the
running process keeps the value it started with.

### display

| key | type | allowed / range | default | apply | env override |
|---|---|---|---|---|---|
| mode | enum | `windowed`, `fullscreen_desktop`, `fullscreen_exclusive` | `windowed` | hot | - |
| window_width | int | [320, 16384] | 1280 | hot | - |
| window_height | int | [320, 16384] | 960 | hot | - |
| remember_window_size | bool | - | true | hot | - |
| fit | enum | `letterbox`, `integer`, `stretch` | `letterbox` | hot | - |
| raster | enum | `crt`, `window` | `window` | hot | - |
| widescreen | enum | `off`, `window`, `16_9` | `off` | hot | - |
| filter | enum | `linear`, `nearest` | `linear` | hot | - |
| render_scale | int, one of a set | 1, 4, 8, 16 | 1 | warm | - |
| show_fps | bool | - | false | hot | - |
| screenshot_dir | string | any path; empty means `<base>/screenshots` with the per-user fallback | `""` | hot | - |

### audio

| key | type | allowed / range | default | apply | env override |
|---|---|---|---|---|---|
| master_volume | int | [0, 100] | 100 | hot | - |
| mute | bool | - | false | hot | `ICORECOMP_NO_AUDIO` |
| music_volume | int | [0, 100] | 100 | hot | - |
| effects_volume | int | [0, 100] | 100 | hot | - |
| movie_volume | int | [0, 100] | 100 | hot | - |
| chime_volume | int | [0, 100] | 60 | hot | - |

Every one of these is a host output gain. None of them touches a value the
game supplied: no SPU2 register, no command word and no byte of guest memory
moves, and at 100 each multiply is by 1.0 and the mix is sample for sample
what it was before the key existed.

`master_volume`/`mute` are read fresh on every audio callback (`sdl_submit`,
`host/audio.cpp`) and apply only to that host-side output stage. The WAV
capture path (`ICORECOMP_WAV_CAPTURE`) reads the mix without them and so
never sees them; it is the headless verification baseline and has to stay a
function of the sound engine alone.

#### What each category covers, and how a sample is attributed to it

The port can only separate what the sound service's own command stream
separates. Three categories come out of it cleanly, and they are the three
below. `chime_volume` is a fourth gain on a sound that is not the game's at
all.

| key | covers | attribution |
|---|---|---|
| music_volume | the game's music and its ambience | **measured**: the voices the game opened with `SgStAdpcmOpen` (command `0x3E`), which are stream voices decoding out of the IOP ring. **Inferred**: that what those voices carry is music and ambience. The two arrive on the same voices |
| effects_volume | footsteps, the sword, doors, the birds, the calls between Ico and Yorda | **measured**: every SPU2 voice that is not one of the above, keyed on out of SPU RAM by the ordinary voice commands |
| movie_volume | the attract movie's soundtrack | **measured**: the `SgStPcm` channels (commands `0x46`-`0x4F`, volumes at `0x4A`). The attract movie is the only user of that command block in this binary (`sif/SNDN2_NOTES.md`, from `ito/mpeg/mv_audiodec.c`) |
| chime_volume | the achievement unlock chime the runtime synthesises (`snd/chime.h`) | host-side: not the game's audio, and not in the WAV capture at all |

Music and ambience cannot be separated from each other. Both are streamed
ADPCM on the same two voices the game opens at boot (two voices, stereo,
`sif/SNDN2_NOTES.md`), and the command stream carries nothing that says
which of the two a given stream is: the script asks for a stream by number
(`src/script.c` `scpAdpcmPlayRequestFunc`, `sound/adpcm_init.c` `AdpcmPlay`)
and the number is a data index, not a kind. One slider covers both, and it
is named for the louder of the two rather than pretending to a split the
port cannot make.

Where the three category gains apply: at the point in `snd/engine.cpp` where
each category is summed, which is the only place in the port that knows
which category a sample belongs to. A gain scales that category's whole
contribution, its reverb send included, so a category turned down takes its
reverb tail with it (the reverb is linear, so scaling the send is exactly
scaling that category's share of the wet output). The voice volumes, the
envelopes, the reverb depth and the master volume the game programmed are
all still exactly the words the command stream set.

One consequence, said out loud rather than left to be found in a diff: a WAV
capture does carry the three category gains, because they are applied
upstream of the capture. `rt_audio_submit` logs a warn naming all three the
first time it writes a capture with any of them away from 100. Set them to
100 for a baseline capture.

### input

| key | type | allowed / range | default | apply | env override |
|---|---|---|---|---|---|
| keyboard.\<slot\> | string | an `SDL_GetScancodeName` name | see table below | hot | - |
| gamepad.\<slot\> | string | an SDL gamepad button string, or an axis name with a trailing `+`/`-` for direction (e.g. `lefttrigger+`) | see table below | hot | - |
| gamepad2.\<slot\> | string | the same names as `gamepad`, for the second controller; sixteen slots, no `menu` and no `screenshot` | the same defaults as `gamepad` | hot | - |
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

Four rules keep one host key or button from being asked to do two things at
once, all enforced by `validate_binds()` in `settings.cpp` at commit time:

1. The menu key must not also name a pad slot (the menu consumes that input
   before the pad ever sees it).
2. Two ordinary slots on the same device must not share a name, because one
   host input cannot press two DS2 buttons.
3. A chord (below) is only legal in `gamepad.menu`; one that ends up in any
   other slot reverts.
4. A chord whose two parts are the same button (`gamepad.menu = "start+start"`)
   is not two buttons and reverts.

The mouse has no menu slot, so only rule 2 applies to it, and two unbound
mouse slots never count as sharing a name. Rule 1 is skipped entirely when
the menu slot holds a chord: `back+start` over the bound `select`/`start` is
the expected setup for a pad whose PS button the OS or Steam intercepts, and
the guest sees both parts exactly as hardware would until the menu opens and
blanks the pad.

Rules 1 and 3 revert only the slots that commit changed, whichever side of
the collision they are: binding the menu key onto an existing pad slot
reverts the menu key, binding a pad slot onto the menu key reverts the pad
slot, changing both reverts both, and a chord that landed on an ordinary
slot reverts that slot alone. A revert goes back to the previously committed
name, not the compiled default, logs why, and is also shown inline in the
Input tab that made the change. A collision or a misplaced chord where
nothing changed this commit is skipped: it arrived in the settings file,
`log_bind_duplicates()` reports it once at load, and the load path never
rewrites the user's own file, so re-rejecting it on every later commit would
only leave a permanent message in a pane that cannot fix it. Name comparison
for every rule is case-insensitive, matching how `SDL_GetScancodeFromName`
itself resolves names.

**Chords.** `gamepad.menu` is the one slot that also accepts a chord:
`<button>+<button>`, exactly one interior `+`, neither side ending in `+` or
`-` (that suffix is the axis-direction convention, `lefttrigger+`, so it is
never mistaken for a chord and `a+b+c` fails resolution rather than picking
one pair out of it). Order carries no meaning. `rt_settings_split_chord()`
(`settings.cpp`) is the one place this grammar is parsed; keyboard names are
never passed through it (`Keypad +` is a legitimate scancode name). A pad-bit
slot (0-15) refuses a chord outright: `host/input.cpp`'s `resolve_pad_name()`
treats a chord-shaped name there as unresolvable and falls back to the
compiled default, the same way an unknown SDL token does. This is the
documented alternative for a DualSense (or any pad) whose PS button the OS or
a launcher like Steam intercepts before this process ever sees it: bind
`gamepad.menu` to `back+start` (Create + Options on a DualSense) from the
Input tab's Rebind button (see Remapping, section 7) or by hand in
`settings.json`.

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
| menu | F1 | screenshot | F12 |

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
| menu | guide | screenshot | (unbound) |

The default stays `guide` (the PS/Xbox/Home button through SDL's mapping):
most pads and most platforms deliver it. `back+start` (Create + Options on a
DualSense) is the documented alternative, not a second default, for a pad or
a launcher (Steam, in particular) that intercepts the guide button before
this process sees it; see the chord paragraph above.

Sticks are not rebindable slots: they map natively from the gamepad's own
analog axes.

Default mouse bindings (`kMouseBinds`, `settings.cpp`): square = `left`,
r1 = `right`, every other slot unbound, `screenshot` included. The names are this runtime's own
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

Two of the slots on each device are host hotkeys rather than DS2 buttons:
`menu` (consumed by `ui/ui_events.cpp`) and `screenshot` (consumed by
`host/screenshot.cpp`). Neither ever reaches the virtual pad, so neither has
a pad bit; every loop in `host/input.cpp` that maps a slot to a bit stops at
the sixteenth slot for that reason. `menu` exists on the keyboard and player 1's
gamepad only, because the mouse is what drives the pointer the menu draws
and the menu is player 1's; player 2's pad (`input.gamepad2`) has neither
hotkey, so its table is sixteen slots where player 1's is eighteen;
`screenshot` exists on the other three. `validate_binds()` treats them the same
way: a hotkey name that is also an ordinary slot's name is rejected, since
it would be a pad button the game could never see, and so is one shared
between the two hotkeys, since the menu key is resolved first in the pump
and the screenshot would never fire.

### Mouse look

`input.mouse_look` (on by default) feeds mouse motion to the right stick,
which is ICO's camera stick. While it is on, the window captures the
pointer in relative mode with the cursor hidden whenever the window has
focus and the menu and launcher are closed; each transition is
logged as `mouse look: captured` or `mouse look: released` with the reason.
Focus loss and opening the menu release it, and those are the only two
things that do.

The game's own menus do not release it. While the pointer owns the mouse
there (section 10) relative mode stays on and the OS cursor stays hidden;
the field's motion moves a cursor the overlay draws inside the picture
instead of the camera stick, which sees no motion at all until the menu is
gone. The switch each way is logged as `guest menu: pointer takes the
mouse` and `guest menu: pointer hands the mouse back to mouse look`.

The camera stick is position control in the game, not velocity. What follows
was read off the decomp as a behavioural reference, the way PCSX2 is read,
and was **not checked against `SCES_507.60`**: every number in this
subsection is inferred from that reading rather than measured on this
build's own ELF, and `host/mouse_look.h` carries the same derivation over
the same constants. The camera's stick reader (`src/camera-ico2.c`)
multiplies the stick's unit vector by the response `iosPadGetStick` returns
and hands that to the routine that calls `ActSendMail_WithAdditionalData`.
The manual camera is two accumulated angles that `GetMailAdditionalData`
turns into rotations of the camera about its target, and the stick sets
where those angles should be (full deflection is 120 degrees of yaw around
the target, the limit the camera's own installer sets), not how fast they
should change. Each camera update moves them toward that target by at most
2.0 degrees while the response is 0.1 or more and 1.5 degrees below it, and
by a tenth
of what remains once inside ten of those steps. So a stick held still holds
the camera still and off centre, and a released stick is a target of zero
that the camera walks back to at 45 degrees per second.

Nothing on that path treats a neutral stick as an event. The immediate snap
back to centre in the code (`ClearMailAdditionalData`) is reached only by a
mode word, by the enable word the decomp reads there being zero, or by a
camera set that forbids the manual camera, never by the stick reading
neutral. There is no timer, no
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
  decomp's stick routine (`ios/pad.c`), inferred and not checked against
  this build: a radial dead zone of 48, the octagonal-gate divisor
  `1 + 0.2 * t / 45`, saturation at 120 and a linear response
  `(mag - 48) / 72`. The floor is the first whole unit past the
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
for the game's own menus, and while the menu is up.

Neither constant has a settings key yet. `input.mouse_look_hold_ms` (0 to
1000 ms, default 250) and `input.mouse_look_drag_pixels` (40 to 1000, default
160) are the two to wire.

Precedence per axis is the keyboard right-stick keys while held, then the
mouse while its stick is off centre, then the gamepad stick. The delta is
sampled once per emulated field and discarded while the menu is
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

The cause is read off the decomp as a behavioural reference, and is
inferred rather than measured on `SCES_507.60`; `host/stick_shape.h` says the
same. `iosPadGetStick` (`ios/pad.c`) forms a radius
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

### system

The `system` section has no keys. `system.language` was its only one and is
no longer read: this disc asks the player in its own language screen, on the
boot branch that draws it (section 10), and that screen, not a host setting,
is what the player answers. A `system.language` left in an existing
`settings.json` is kept in the file untouched and named once at info, like
every other retired key (section 4).

What the runtime still reports to the game is English, which is also what
this build falls back to when the value it reads is outside the game's own
five-entry table, so a run behaves as the port always did. The rest of this
section is the measurement behind that, kept because it is what
`ee/syscalls.cpp` implements.

The console's configured language is what this game asks for through
`sceScfGetLanguage`, and what a console answers from its OSD settings. This
port has no OSD, so the answer is the compiled-in English.

The path, measured on the decomp's disassembly of the vendor library this
build links (`asm/nonmatchings/src/cod/vendor_272338/sceScfGetLanguage.s`):
that function calls the `GetOsdConfigParam` syscall, reads the version field
at bits 13 to 15 of the word the kernel writes, and for a nonzero version
returns bits 16 to 20 of the same word as the language. A version of zero
sends it down a different branch, `(word >> 4) & 1`, which is a one-bit
Japanese/English field and cannot express the four other languages this disc
carries. `ee/syscalls.cpp`'s `h_GetOsdConfigParam` therefore reports version
1 with the setting's value in bits 16 to 20. Version 1 and not 2, because a
version of 2 or more is what makes the later SDK libraries call
`GetOsdConfigParam2` instead, which this kernel does not implement; this
build calls `GetOsdConfigParam` only.

The values are the SCE OSD numbers, so the setting is the number the syscall
reports rather than a translation of it: 1 English, 2 French, 3 Spanish, 4
German, 5 Italian. The game maps that value through its own five-entry table
(`asm/nonmatchings/src/kanbanBoot/kanbanBootMcCheck.s`, the `sltiu $3, $4,
0x5` over `language - 1`) to pick which set of pre-rendered subtitle images
it loads, and anything outside 1 to 5 keeps its compiled-in default, which
is the English set. Japanese and the two languages the OSD numbering has
past Italian are not offered, because this disc's table has five entries and
nothing behind the other values.

The disc needs nothing per language: the PAL image carries one data file,
`\DFDATAS\DATA.DF`, whose outer table holds all five subtitle sets, so the
disc layer serves the same file by the same name whatever the setting is.

The game reads this once, at boot. Nothing is written back: this build does
not store the language on the memory card, it re-derives it from the syscall
every boot.

### achievements

| key | type | allowed / range | default | apply | env override |
|---|---|---|---|---|---|
| enabled | bool | - | true | hot | - |
| toast | bool | - | true | hot | - |
| sound | bool | - | false | hot | - |

The chime's volume is `audio.chime_volume`, in the audio section with the
rest of the host output gains.

Local achievements, observed from the game's own progress bits. The observer
(`src/runtime/guest/achievements.cpp`) reads the progress array once per
guest field, from `sif/pad.cpp` beside the pointer on the game's menus, and
writes nothing into guest memory and patches no guest code. What it unlocks
is kept in `achievements.json` beside the virtual memory card. The trophy
list, the file format, what is resolved and what is not, and the procedure
that resolves the rest are in `docs/ACHIEVEMENTS.md`.

`enabled` off, the tick returns before it reads anything and no counter
moves; trophies already unlocked stay unlocked. `toast` off, an unlock is
still recorded and logged and nothing is drawn over the picture. `sound` on,
an unlock plays a short chime. No audio asset ships with this port and none
may: the chime is synthesised at run time by `src/runtime/snd/chime.h`
(three sine partials, a 4 ms attack and a 300 ms decay, rendered once into a
PCM buffer at the output rate) and summed into the samples `host/audio.cpp`
hands to the device, with `audio.chime_volume` as its gain. The game's own
mix is not altered: while nothing is queued the mixer stage is skipped and
the samples reach the device untouched, and the WAV capture
(`ICORECOMP_WAV_CAPTURE`, the headless verification baseline) never carries
the chime at all. `audio.master_volume` and `audio.mute` apply to the sum,
so a muted run plays no chime either. All three are hot and all three go
through `settings_apply.cpp`, which pushes them into
`rt_achievements_configure` on every commit and once more at startup,
because a load runs no applier; `audio.chime_volume` needs no applier
because the mixer reads it fresh on every submit, exactly as
`audio.master_volume` is read.

The progress-bit diagnostic is not a setting. One info line per
progress-bit transition and per menu layout id change is written on every
run: it is the log that resolves the trophy bit table, it is what a user is
asked for when a trophy does not fire, and at info level it costs a line
when a bit moves and nothing at all on a field where none does. See
`docs/ACHIEVEMENTS.md`.

### debug

| key | type | allowed / range | default | apply | env override |
|---|---|---|---|---|---|
| log_level | string | `error` / `warn` / `info` / `debug` | `warn` | hot | `ICORECOMP_LOG_LEVEL` |
| console | bool | Windows only; see section 5 | false | cold, restarts | - |
| log_file | bool | see section 5 | true | cold, restarts | `ICORECOMP_LOG` |
| profile_fields | int | [0, 100000]; 0 disables the profiler | 180 | hot | `ICORECOMP_PROFILE` |
| fps_limit_hz | double | `-1` (the video mode's own rate), 0 (no pacing), or [1, 1000] | `-1` | hot | `ICORECOMP_FPS_LIMIT` |

`fps_limit_hz` caps the guest field rate. Its default, `-1`
(`RT_FPS_LIMIT_MODE_RATE`), is not a rate: it means the rate of the video
mode the game programmed through `SetGsCrt`, which is 59.94 Hz on NTSC and
50 Hz on PAL. The game's own display option switches between the two at run
time, so both rates occur on this disc; the pacer follows the mode field by
field.
The menu shows and accepts the word `mode` for it (`auto` too), so
the text box never reads as a rate the player typed. `0` still disables
pacing and any value in [1, 1000] is still a plain cap in fields per second,
whatever the mode. `ICORECOMP_FPS_LIMIT` still wins over the file: `0` and
`off` disable pacing, a number above 1 is that cap, and anything else means
the mode's rate.

`log_level` is the level a line has to reach to be logged, and every line in
the runtime carries one:

| level | what is at it |
|---|---|
| error | the operation did not happen at all: a file that failed to write, and the `FATAL` line and register dump of a run that is ending |
| warn | it happened differently from what was asked: a refusal, a fallback, an out-of-range or unknown value, a missing file, an expired fact, anything that changes what the user should do |
| info | startup facts, applied settings, device and backend identity, one-time summaries, the profiler's reports and counters |
| debug | per-field, per-packet, per-access, flood-controlled and trace lines |

A line shows when its own level is at or above the configured one, so `warn`
(the default) keeps error and warn, `info` adds the startup and summary lines,
and `debug` adds everything. Both sinks obey it, with one asymmetry that
predates levels and is kept: debug lines go to the log file only, never to the
console echo, so turning the level down does not make the console unusable.
Section 9's table names the level of each line worth grepping for.

`log_level` and the verbose channels are two different controls and both
apply. `log_level` sets the floor for every component at once and is a
settings key; the channel spec is not a settings key at all, it is
`ICORECOMP_VERBOSE` (section 3), and it names individual channels whose
debug lines pass whatever that floor is. So `log_level = "warn"` with
`ICORECOMP_VERBOSE=cdvd` is a quiet log with one full disc trace in it.
Setting `log_level = "debug"` turns on every channel this build defines.
`widescreen` is the one that costs real frame time: it is the measurement
report for `display.widescreen` (section 6) and costs a per-field summary
plus, for the first 32 fields after the mode changes, one line per
classified 2D primitive. `present` is the other one worth knowing about.
The geometry checker used to be a channel of that kind, `geom`; it is a
build define now (`ICORECOMP_GEOM_CHECK`, docs/GS_RENDERER.md), because
re-parsing every GIF packet is a cost that does not belong on a switch a
shipped run can flip. All of them are read once, when
`rt_log_init` parses the variable, so a channel is chosen for the run at
startup and not changed inside it.

The channels a channel spec can name are the log components that have debug
lines. Besides the ones above (`cdvd`, `chan`, `input`, `other`,
`sif`, `syscall`, `timer`, `vif1`, `vu0`, `vu1`, `ipu`, `dmac`, `rpc`,
`mmio`, `intc`, `cop0`, `alarm`, `vblank`, `snd`, `sndn2`, `ui`, `gs`,
`guest`), three are worth naming here:

| channel | what it adds |
|---|---|
| `present` | one line per present: a timestamp in microseconds, the scanout serial on screen, and `new` or `repeat`. Read once when the present path is built |
| `screenshot` | the PNG size against the present rectangle, per capture (section 7). The overlay comparison is no longer here: it fires on its own, once per run, at info |

`achievements` and `gsr` are log components, not channels: every line either
one emits is info or warn, so they are governed by `log_level` alone and
naming them in `ICORECOMP_VERBOSE` adds nothing.

A `log_level` outside the four names is rejected like any other bad value: the
compiled-in default stays, and the line names the value and the allowed set
(section 4).

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
swapchain and a field lost to the renderer can be told apart; once the GS
worker thread is up (below) those three spans are that thread's, not the EE
thread's `present` bucket. The bucket table above them is an average over the
whole window, which is the wrong shape for a stutter; these are the extremes
in it.

Once the game boots, the GS command ring is drained by a worker thread, so the
bucket table describes the EE thread alone: `gs` and `present` there hold only
the cost of writing records into the ring, and `gswait` holds the field sync
point where the EE waits for that worker to finish the previous field. The
worker's own budget is the `gs worker` line, in milliseconds per field:
`replay` is packet parsing and privileged writes, `present` is flush, scanout
and swapchain present, and `idle` is how long it sat with an empty ring, which
is its headroom. The two threads run at the same time against the same 16.68 ms
field, so the port holds the field rate only when each of them fits in it on
its own. `ICORECOMP_GS_THREAD=0` bypasses the ring entirely and puts all of it
back on the EE thread; it is a bisect switch for "is the ring responsible for
this?" and has no settings.json twin.

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
`"<key> is overridden by <VAR>=<value>"` at `info`, which is below the
shipped default level, so the line is in the log of a run at
`debug.log_level = "info"` or lower and not in a default `warn` run's. The
in-game menu shows the same fact whatever the level: a control whose key is
overridden is disabled and carries the same "overridden by" text.

"Set" means the variable is present in the environment, even as an empty
string, because that is exactly what every consumer of these variables tests
(`getenv() != NULL`). An empty `ICORECOMP_NO_AUDIO=` still counts as set,
for instance.

`ICORECOMP_LOG` is the one exception: it must be non-empty to count as set.
An empty log path names no file, so `log.cpp` reads the variable as
`env && *env` and leaves `debug.log_file` in charge. Reporting the key as
overridden there would say the setting was ignored when it is exactly what
took effect.

The full table (`kEnvTwins`, `settings.cpp`):

| settings key | environment variable |
|---|---|
| debug.fps_limit_hz | `ICORECOMP_FPS_LIMIT` |
| debug.log_level | `ICORECOMP_LOG_LEVEL` |
| debug.profile_fields | `ICORECOMP_PROFILE` |
| debug.log_file | `ICORECOMP_LOG` |
| audio.mute | `ICORECOMP_NO_AUDIO` |

This is deliberate: it keeps every existing script, CI job, or manual
invocation that already sets one of these variables running exactly as it
did before `settings.json` existed.

### Variables with no settings key

Two of them were twins until the settings key each stood over was removed.
Both variables still do exactly what they always did, at the same call site,
so every script and CI invocation keeps its behaviour; there is simply no
file key left for them to win over, which is why they are not in the table
above and why no "overridden by" line is logged for them.

| variable | what it does | read by |
|---|---|---|
| `ICORECOMP_GS_PRESENT` | the swapchain present mode for the run: `mailbox` (the compiled-in value), `fifo` or `immediate`. Resolved once, when the GS backend is made | `gs/gs_select.cpp` |
| `ICORECOMP_VERBOSE` | the log channel spec: a comma-separated channel list, or `-`/`0` for none. A channel named here passes its debug lines whatever `debug.log_level` is. Parsed once, in `rt_log_init`, before anything can log | `log.cpp` |

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
  trip (the loader retains the whole parsed document, not only the fields it
  recognizes) and each is logged once as `"unknown key \"<dotted.key>\" kept
  as-is"`.
- **A retired key**, one this build no longer reads, is left in the file
  exactly as it was and named once as `"<dotted.key> is no longer read"` at
  `info`, with what stands in its place. It is not an error and nothing is
  refused: it is the ordinary state of a `settings.json` written by an older
  build. The keys removed on 2026-09-04 are `display.present`,
  `display.present_rate`, `display.deinterlace`, `system.language`,
  `debug.verbose`, `achievements.sound_volume` (now `audio.chime_volume`)
  and `achievements.log_progress_bits`. `display.backend` was removed the
  same way on 2026-09-05, when the native renderers were withdrawn from the
  player's build. All eight go through `load_retired` in `settings.cpp`.
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

## 5. debug.log_file and debug.console

`debug.log_file` is read before anything else in settings, by
`rt_settings_peek_log_file()`, because `rt_log_init()` has to decide whether
to open a log file before the first line can be logged, and so before
`rt_settings_init()` itself can run. This means the key takes effect at
startup only: it is cold, so changing it in the menu while the launcher is
up restarts the program to apply it (section 2), and editing it in a running
instance's file has no effect until that instance is next started.

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

`debug.console` is the Windows half of the same question and is read the same
way, by `rt_settings_peek_console()`, before `rt_log_init()`. `ico.exe` is a
GUI-subsystem binary: a double-clicked run has no console at all, so nothing
it prints can flash up and vanish with the window. What it does instead:

- Started from a shell (cmd, PowerShell, a CI job, a `.bat`): the process
  attaches to that shell's console and the echo appears there, exactly as
  before. `debug.console` changes nothing for these runs.
- Double-clicked, `debug.console = false` (the default): no console window.
  The log file is the record. A fatal that would have held a console open
  puts the failure and the log path in a message box instead, so it still
  reaches the user.
- Double-clicked, `debug.console = true`: a console is allocated at startup
  and the echo goes to it, as a console-subsystem build did, and a fatal
  holds it open until Enter.

A process that has already decided it has no console cannot be given one
mid-run, so this key is cold, and so is `debug.log_file` beside it: the sink
is opened once, from the same startup peek. The runtime applies both the way
it applies every restarting cold key: changing one from the launcher
restarts the program (section 2), and the controls are disabled once the
game is running.
Linux and macOS are unaffected; stderr is the echo there and always was.

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
lines per field for non-overscan NTSC, at DISPLAY clock 636 and line 50, and
640 by 256 per field for PAL. ICO's NTSC gameplay window is exactly the NTSC
rectangle (DW+1 2560, DH+1 448, MAGH 4). The attract movie is not: it
programs DW+1 2880, DH+1 480, MAGH 3 from the same corner, which is 720
pixels by 240 lines per field, so 80 columns on the right and 16 lines per
field at the bottom fall outside the frame and are cropped. How much of them
a television would have shown behind its own overscan is a property of the
set, not of this port.

Those two windows are NTSC ones, and this port targets the PAL disc, which
boots in PAL (docs/TARGET.md, "The display option"). What a PAL run programs
has been measured only for the boot screens so far, on the 2026-09-04 22:07
run: `PMODE` EN1 0 EN2 1, `SMODE2` 0x2 (INT 0, FFMD 1), CMOD 3, `DISPLAY2`
DX 656, DY 36, MAGH 4, MAGV 0, DW+1 2560, DH+1 256, and a `DISPFB2` of
512x256, PSM 0, FBW 8, DBX/DBY 0. That is a progressive raster, so the
"per field" of the NTSC numbers above is a whole frame here. The gameplay
window and the movie window on this disc are still unmeasured.

The register values quoted for the movie here were measured on a run whose
movie window is DW+1 2880, DH+1 480. This build's own `mv_disp` passes
`setDispEnv` 720 by 576 (docs/TARGET.md, "The picture"), and no run of this
runtime has reached the movie on this disc, so what its display registers
carry has not been measured. What carries from the numbers below is the rule
they were derived with, not the numbers.

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
lines against the frame lines of the mode's 4:3 area, 448 on NTSC and 512 on
PAL, which is 4:3 for gameplay and 1.4 for the movie. That keeps the movie's pixels the same size as
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

How the two fields of an interlaced scanout become one output frame is
fixed at bob and is not a setting. It matters for the attract movie, whose
MPEG is interlaced video: the game splits each decoded 720x480 picture into
an even-row field buffer and an odd-row field buffer and flips `DISPFB2.FBP`
between them once per field, and the two row sets of one decoded picture were
themselves captured about 1/60 s apart (a decoded I frame shows comb teeth on
moving figures inside the single picture). The field pair is two moments, not
one still frame.

Bob presents each field on its own (the movie's fields; see below), stretched
to the frame height and offset by the half raster line the field sits at,
which is what a CRT does with it: full field-rate motion, half the vertical
detail, and the shimmer bob always has on fine horizontal detail. It is the
mode that shows the movie as the disc holds it, which is why it is the one
the port keeps. The renderer still has the other two, `adaptive` (its FastMAD
filter: still parts woven with the previous field, moving parts from the
current field alone) and `weave` (the two newest fields paired with no motion
test), and `third_party/patches/README.md` records what each looked like on
the running movie; neither is reachable from `settings.json` or the menu.

Bob acts on the movie's fields only: a field the game's own display copy
produced (gameplay and the title menu at render scale 1) is one of two
resamples of a single rendered frame, and the adaptive filter gives that
whole 448-row frame back, so those fields are always composed adaptively.

Nothing is deinterlaced when the renderer scans out at high resolution
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
above and says so in its log line. There is no game-speed option; it is out
of scope for this port by design. `display.widescreen` is the second setting
in this document that changes a value the game supplied, added by the user's
decision on 2026-09-03. `off`, the default, is the retail 4:3 picture and
reproduces every earlier build exactly. `window` widens the 3D projection to
the window's aspect and `16_9` fixes it at 16:9. What is written is one
float, the X scale of the game's own projection block, which this setting
multiplies by `(4/3)` over the aspect. Every other word of that block is left
as the game wrote it, so the widening is horizontal only. 2D elements keep
their 4:3 geometry, scaled about the centre of the picture so they sit over
the wide scene at retail proportions. The scanout aspect follows the setting
and `display.fit` letterboxes the remainder as it does at 4:3. Both facts are
on the `widescreen` log lines, and the next subsection is the whole of it.

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

### display.widescreen

Addresses first. They live in `src/runtime/guest/ico_syms.h` behind
`RT_ICO_WIDESCREEN_KNOWN`: the matrix composer's entry is `0x001146F0` and
its projection block is `0x0067BA60`, both found by the code that builds the
block rather than by name, with the argument in `config/entry_hooks.txt` and
the method in docs/TARGET.md. On a build whose guest addresses are not
known, the setting refuses to do anything: one warn line at startup, and the
picture stays 4:3.

The game builds its projection matrix in one place. A nine-float block is
filled by one routine, which then tail-jumps to the matrix composer with the
block's address; the composer reads the block through that pointer and builds
the matrix from it. The translator emits a call to a runtime hook at the
composer's entry (`config/entry_hooks.txt`, an address fact like the ones in
`src/runtime/guest/ico_syms.h`), and `src/runtime/guest/widescreen.cpp`
multiplies the X scale at `+0x04` of that block by `k = (4/3) / aspect` there.
That is the whole of what reaches
guest memory: one float, at the one instant it is complete and not yet read,
and nothing at all while the setting is `off`. No guest code is patched.

Scaling the projection and not the camera is what makes this a wider view
rather than a stretched one. The block's other eight words, the field of view
among them, are untouched, so the frustum grows sideways and vertical
framing, focal length and everything the game computes from them stay exactly
as the retail game left them.

`window` derives the aspect from the presentation surface and follows it
across a resize, a fullscreen switch and a monitor change: it is recomputed
at every field boundary, not only when a settings key moves. `16_9` holds
16/9 whatever shape the window is, so a window that is not 16:9 letterboxes
or pillarboxes to it through `display.fit`. A window narrower than 4:3 gives
a `k` above 1, which narrows the frustum; that is the honest answer for that
window rather than a case to refuse, and the log line names the number.

2D does not go through that projection, so widening it would leave the
menus, the subtitles, the HUD-like overlays and the fades untouched while the
picture behind them changed shape. `src/runtime/hw/gif.cpp` scales the X
coordinate of 2D vertices back by the same `k`, about the GS window centre at
2048, before the packet reaches any backend. It runs on the host's side of
guest memory, on a copy of the packet, so what the dump writer records is
what was drawn. Coordinates are 12.4 fixed point and each edge rounds on its
own, so a scaled quad can be 1/16 of a pixel off the exact width; that is
below a pixel and is left visible rather than papered over.

Which packets count as 2D is a measurement question and the rule in
`gif.cpp` is provisional. As implemented it transforms a sprite on any path,
and a triangle strip or fan on PATH3 whose PRIM has `FST = 1` and whose
vertices all share one Z, and only when the primitive's X extent lies
strictly inside the current context's scissor. A primitive spanning the whole
scissor width is a full-frame pass (a fade, a post-processing copy, a
frame-sized quad) and is left alone, because widening the frustum did not
move it. PATH1 is VU1's transformed output and its strips are never touched;
its sprites are, because the game's own menu item quad arrives there
(`gif_StartPacketPath1`, `src/runtime/guest/menu_nav.cpp`).

To confirm or correct that rule, turn the `widescreen` channel on
(`ICORECOMP_VERBOSE=widescreen`) and take
a GS dump of five scenes with the mode off and again with it on:

1. the title screen,
2. one page of the game's own menu,
3. a cutscene with subtitles up,
4. a fade,
5. the bloom pass.

Each field logs how many primitives were transformed, how many were left as
full-frame passes and how many were judged 3D, and for the first 32 fields
after the mode changes it logs one line per classified primitive with its
path, primitive type, X extent in pixels and the decision. The same channel
logs the VU1 kick count and the set of microprogram hashes bound that field,
so a pair of dumps taken at the same camera with the mode off and on can be
compared on what the widened frustum stopped culling rather than on a
description of the picture. When the rule is settled, name it and the dumps
it was decided from in `src/runtime/hw/gif.cpp`, where the provisional note
is now.

Presentation follows the setting: the scanout is presented at the target
aspect (the window's, or 16:9) instead of the 4:3 the CRTC registers derive,
and `display.fit` letterboxes what is left over exactly as it does at 4:3.
The override is applied to every scanout, a full-screen 2D one included.
Nothing at the scanout can tell a 3D field from the attract movie, a menu
page or a fade, and a picture whose aspect changed from field to field would
be worse than one presented at a single aspect throughout. So full-screen 2D
is presented at the target aspect too, while its geometry was held at 4:3 by
the transform above. What that looks like depends on how the transform
classified each 2D pass: content it judged inside-frame sits at retail
proportions with the wide picture around it, content it judged full-frame
still fills the picture. The five dumps above are what decides which of those
every 2D pass in this game is.

### The present, and why it is not a setting

The window is refreshed once per finished field. There is no key for the
rate and none for the swapchain mode: the rate is one present per field,
which is what every shipped run does, and the mode is `mailbox` unless
`ICORECOMP_GS_PRESENT` names `fifo` or `immediate` for that run (section 3).

The present is still off the guest's field boundary, which is a property of
the pipeline rather than of a setting. `rt_pgs_vsync` renders the field and
latches it; a separate call, `rt_pgs_present_pump`, is what puts it on
screen, and the GS command ring's worker thread makes that call once
immediately after each field, so a finished picture is never held back. The
EE thread enqueues the field and goes back to the game. Its one per-field
wait, the field sync point, returns as soon as the previous field's scanout
is done, so a present still in flight does not hold it. That does not make
the present free: a worker that needs longer than a field period for
everything it does, present included, still falls behind and the EE meets
that at the same wait. What is gone is the per-field serialization, the EE
waiting for a present to finish before it may run the next field of guest
code.

`ICORECOMP_GS_THREAD=0` bypasses the command ring, and with it the worker:
one present per field from the EE thread. That switch is a developer bisect
tool, not a way to play.

The backend interface still carries a present rate
(`GsBackend::set_present_rate`, implemented by the command ring) and the
ring's own selftest exercises it, including the case that matters here: a
rate of 0 must produce exactly one pump per vsync and never a repeat
(`gs/gs_ring_selftest.cpp`). Nothing in the runtime sets it to anything
else.

None of this is the guest's frame rate. The game draws fields at the rate of
the video mode it programmed (59.94 a second on NTSC, 50 on PAL); the field
edge, the vblank interrupt bits, the EE timers, the SIF and the pad all hang
off the virtual clock (`src/runtime/ee/sched.cpp`), and `pace_field`
(`src/runtime/hw/gspriv.cpp`), steered by `debug.fps_limit_hz`, is the only
thing that sets it.

### Renderer backends

The renderer is paraLLEl-GS. `display.backend` was withdrawn from
settings.json and from the menu on 2026-09-05: the native renderer in
`src/runtime/gs/render` (Vulkan, D3D12, Metal) drew its first game frame that
day and is not close to playable (docs/GS_RENDERER.md, "The first run on real
hardware"), so it is not offered to the player. A `backend` key still in a
settings.json is named once at info as no longer read and otherwise ignored.
`ICORECOMP_GS_BACKEND` (`auto`, `parallel-gs`, `vulkan`, `d3d12`, `metal`)
remains for the replay tool and CI, where the native renderer is still built
and compared; it is not a twin of any settings key.

`ICORECOMP_GS` is a separate question and a different axis: it names the
*transport* (`dump`, `parallel`, `both`, `native`), which is where the GS
command stream goes. `dump` creates no live renderer, so no renderer is
resolved there. Every other value is live, and `ICORECOMP_GS_BACKEND` picks
the renderer and the graphics API for it: `ICORECOMP_GS=native` with
`ICORECOMP_GS_BACKEND=d3d12` means the native renderer on D3D12, and
`ICORECOMP_GS=native` with a resolved backend of `parallel-gs` is a fatal,
because the two contradict each other. When `ICORECOMP_GS` is unset the
platform default decides whether a live backend is created at all: Windows
and macOS builds create one (a packaged build must open a window), Linux
builds the headless dump backend.

`ICORECOMP_GS_HEADLESS=1` skips the window entirely. Every backend then takes
its headless path, which is what CI and the replay tool use.

An `ICORECOMP_GS_BACKEND` naming a backend this build does not have is a
warn and a fall through to `auto`, never a fatal, and so is an unrecognised
spelling: the resolver logs the value and the list of backends this build
does have and uses `auto` (`gs_select.cpp` `resolve_live_backend`). That
rule is older than the retirement and is kept because a fatal there would
stop the run before the launcher window ever existed. An unknown
`ICORECOMP_GS` transport is still fatal: that variable names where the
command stream goes, not which renderer draws it, and there is no sensible
fallback for it.

The menu's Display tab no longer offers a renderer select. It still carries
two read-only lines, `Renderer` and `Feature support`. Both describe the
device the active backend actually created: the backend fills them in once,
just after creating it, and nothing makes a device of its own to answer the
question (which the startup probe they replaced did on every launch, for a
device the run need not have used). For `parallel-gs` the first names the
Vulkan device, its raw driver version and its API version, and the second
says `all required features present` or lists the requirements the device
missed. For a native backend the first names the device and its API level
and the second names the RHI backend. A run with no live backend says `no
renderer device created yet` to both. They are the two facts to send back
from a machine where the picture does not appear; see `docs/MACOS.md`.

## 7. The in-game menu

The overlay is titled "Menu", and the launcher's button that opens it says
"Menu" too. This document and the source call it the settings menu wherever
it has to be told apart from the game's own menus, which
`src/runtime/guest/menu_nav.cpp` drives; nothing on screen says "Settings".
The document is still `ui/menu.rml`, its data model is still `settings`,
and the launcher event is still `open_settings()`: those are bindings
between files, not labels.

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

### The two cold controls

Two controls behave differently from every other one in the menu: the Debug
tab's "Show a console window" and "Write a log file" checkboxes. Both keys
are cold (section 2), and each carries a note in red (`.note-restart` in
`ui/style/base.rcss`) instead of the usual hint colour, because the note is
about what a change does to the run rather than about the value:

- With the launcher still up, the note reads "Changing this restarts the
  program" and the control is live. Changing it writes `settings.json`, ends
  the run through the same shutdown a quit takes, and starts a new process
  on the same command line and working directory, which comes up on the new
  value. There is no apply button and no confirmation step: the commit is
  the restart.
- Once the game is running, the note reads "Change this from the launcher;
  the program restarts to apply it" and the control is disabled. A commit
  that moved one of those keys anyway is reverted with a log naming it, so
  anything else that writes the settings struct meets the same rule.

The menu is reachable from the launcher through its "Menu" button, which is
where a cold key is meant to be changed.

There was a third until 2026-09-05, the Display tab's renderer backend
select. `display.backend` was retired with the native renderers (section 6),
so the control is gone and the key is no longer read.

The log file checkbox is disabled for a second reason while `ICORECOMP_LOG`
is set, and says so in its own "overridden by" note; that rule is older and
unchanged.

A restart that cannot be started (`CreateProcessW` failed, or this
executable's own path could not be resolved) is not fatal: the reason is
logged at error and put in a message box, the run carries on with the value
it started with, and the new value stays in `settings.json` for the next
launch.

### Remapping

The menu's Input tab shows one table of slots per device (keyboard,
gamepad, mouse), each row a DS2 button or stick direction paired with the
name currently bound to it, plus a Rebind button (and, on the mouse, an
Unbind button that stores `""`). Pressing Rebind arms capture for that one
slot (`ui/ui_rebind.cpp`):

- **Key**: the next key pressed is stored as its `SDL_GetScancodeName`.
- **Gamepad button**: the next button pressed is stored as its SDL button
  string, on the press, immediately, for every slot except `gamepad.menu`.
  That one slot is the exception, because it is the one a chord is legal on
  (section 2): its capture prompts "press a button, or hold two together"
  and accepts on release, once nothing is held, from every button pressed
  since the capture armed. One distinct button becomes its own name; two
  become `first+second` in the order they were pressed (order carries no
  meaning once stored, see section 2's chord grammar); a third distinct
  button invalidates the attempt (a status line says so) and the user
  releases everything and starts over. An axis is refused outright for this
  one slot, with its own status line, since a chord is buttons only.
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

### Screenshots

`F12` (`input.keyboard.screenshot`; the gamepad and mouse twins ship
unbound) writes a PNG of the presented picture into `screenshots/` beside
`saves/`, named `ico-YYYYMMDD-HHMMSS.png` in local time, with `-2`, `-3` and
so on appended when a name is already taken. `display.screenshot_dir` names
a different folder; empty, the default, means the folder beside `saves/`,
falling back to the per-user state directory when the installation's own
folder is not writable. A folder that cannot be created logs once and the
capture is skipped; nothing here is ever fatal.

What lands in the file is the window backbuffer over exactly the rectangle
the present published: the picture at presented size, with the display
aspect already applied and the letterbox bars excluded, so it is what the
window showed and not the raw scanout. The copy is taken after the scanout
blit and before the overlay render pass, which is why the menu, the
launcher, the drawn pointer and the fps readout are never in it. The Display
tab's `Take screenshot` button makes the same request, so a capture can be
taken from the menu without the menu being in the picture.

The pixels cross the paraLLEl-GS C ABI as raw bytes and nothing else: the
arm rides the GS command ring like any other call that changes what a
present does (`rt_pgs_request_screenshot`), so it lands on the field the key
was pressed on, and the finished image is read back out of a mutex on the
library side (`rt_pgs_take_screenshot`) rather than through the ring. The
folder, the name, the timestamp and the PNG encoder are all on the
executable's side, in `host/screenshot.cpp`.

The measurement the design rests on fires on its own, on the first capture
of a run and only then: that field is copied twice, once before the overlay
pass and once after it, and the log says how many bytes differ. With the
menu closed they are byte identical; with the menu open they differ, and the
copy taken before the pass is the one written. Only one file is ever
written, whether or not the check was taken: the second copy is compared and
dropped. This was gated on `ICORECOMP_VERBOSE=screenshot` until 2026-09-05,
which meant the claim rested on a run nobody makes.

This is not `ICORECOMP_GS_SCREENSHOT`, which is a rendering diagnostic: that
one writes the raw scanout as a PPM, deliberately without the display
aspect, so it stays a function of the GS output alone and stays comparable
against a `gs-replay` dump. The two answer different questions.

### Controller

#### The second controller

The virtual IOP serves two pad ports, and the game opens both: the decomp's
`iosPadDevInit` opens ports 0 and 1, slot 0 on each, in one loop
(`asm/nonmatchings/ios/pad/iosPadDevInit.s`, the `scePadPortOpen` at :68
inside the `slti $2, $17, 0x2` loop at :88), and its pad device array has
two entries. Port 1 is what the disc's two-player mode reads for Yorda:
`asm/nonmatchings/src/girl_act/func_00177BB8.s` binds the girl's pad struct
to a device index taken from the game's own two-player flag (:83-85) and
reads the stick from it (:87, :99). The flag is toggled from the game's own
in-game options menu (`asm/nonmatchings/src/layout_action/la_game_option.s`
:104-108, the fifth row of that menu), with no clear-count gate on the row.

Host side, pad port 0 is player 1 and is fed by the keyboard, the mouse and
the first gamepad; pad port 1 is player 2 and is fed by the second gamepad
alone, through `input.gamepad2`. Until a second gamepad is attached the game
is told nothing is plugged into port 1, which is what `sif/pad.cpp` sends as
a disconnected frame and what a console would report. Attaching a second pad
fills port 1; unplugging player 1's pad moves player 2's down onto port 0 so
the game is not left with an empty port 0; attaching a third pad logs its
name and leaves it closed, since there are two ports and no multitap.

The two deadzones and `gameplay.run_any_direction` apply to both pads: they
describe how a stick is reported, not which player is holding it. Rumble is
per port, so a jolt the game sends to port 1 goes to player 2's pad. The
mouse, mouse look, the wheel queue and the pointer on the game's own menus
are player 1's only, and so are the two host hotkeys: `input.gamepad2` has
no `menu` slot and no `screenshot` slot.

Only a run with two controllers plugged in can confirm the end of this: that
the game's own two-player option row makes Yorda follow the second pad. The
host side of it is covered by `icorecomp-settings-selftest`.

#### The overlay

The focused control is drawn inverted, black with sand type, so the pad's
selection reads at a glance; a text field, a select and a check row invert
the same way, the field or the whole row going black. The footer's last
line names the controls for the device last used: after a pad input the pad
line, after a key or the mouse the keyboard line. It is bound to `nav_hint`
on both the settings and the launcher models and follows the `last device is
now ...` log line. The two documents get different wording, because their
pad models differ: the menu's pair names the cards, the pane and East, the
launcher's names neither East nor Escape, since neither does anything
there.

Both overlay documents (the menu and the launcher) are navigable end to end
with a gamepad, entirely from `ui/ui_events.cpp`; nothing under `ui/` needs
its own input handling for this. Guide (or the `back+start` chord) opens
and closes the menu from anywhere, and the left stick's four synthetic
d-pad edges use a fixed hysteresis (press above a raw axis value of 16384,
release below 9830) independent of `input.left_deadzone`, which is about
what the game is told the stick reports, not about the menu noticing a
deliberate push; held, a direction repeats after 400 ms and then every
100 ms. Both documents share those two rules; everything else below is
specific to one or the other, because the menu's layout (a column of cards
beside the pane the active one shows) and the launcher's (one flat list of
controls) call for different pad models.

**The launcher** stays flat, the way the whole overlay used to work: South
activates the focused control (press a button, toggle a check row);
Up/Down/Left/Right move the focus ring by RmlUi's own spatial search
(`nav: auto` on every control, `body.launcher { nav: auto; }` as the wrap
target when a search finds nothing, which is also the recovery if nothing
is focused at all). East does nothing here, deliberately, the same way
Escape does not close the launcher: a stray press must not be able to quit
out of it. Quit is a button on the window, reached and pressed like any
other control. The Start button carries `autofocus`, so the pad has
something focused from the first frame.

**The menu** is two levels, decided fresh every time from where the
focus actually is (`current_nav_level()`, `ui/ui_events.cpp`) rather than
from state of its own, so a mouse click into either region is never out of
step with the pad: focus inside one of the five cards in the left column is
level 1, focus inside the pane on the right is level 2, and anywhere else
(the footer buttons, or the body itself, which is where RmlUi leaves the
focus after a click on empty panel) reads as level 1 too. Level 1's own
moves are written against the five cards by id, so from one of those
in-between places any direction key first puts the focus back on the active
tab's card; from there the table below applies.

| pad input | level 1 (a card) | level 2 (the pane) |
|---|---|---|
| Up/Down | move the focus ring among the five cards, wrapping; does not change which pane is showing | RmlUi's own spatial search (native, `dispatch_nav_key`'s usual document-wrap fallback included), vetoed if it would land on a card -- focus stays where it was instead |
| Left/Right | switch the active tab (same as L1/R1), keeping focus on the card | a single native key dispatch, no document-wrap fallback: a range slider adjusts, a text field moves its cursor, anything else moves focus within the pane by RmlUi's own default handling (which is confined to the pane anyway, since the pane is the nearest scroll container) -- vetoed if it lands outside the pane. An open select's session (below) swallows these two rather than dispatching them |
| South | make the focused card's tab active if it was not already, then focus the first focusable control in the now-visible pane (enters level 2) | activate the focused control: press a button, toggle a checkbox, open a closed select, commit a select's highlighted option, or (in a text field) commit the field |
| East | close the menu | back out to level 1: focus the card for the tab still showing, even out of a focused text field (a second East from there closes the menu, since level 1 is what East then sees) |
| L1 / R1 | previous/next tab from either level; from level 2 this also moves focus back to the new card (level 1) | (same) |

A `<select>`'s South/East/Up-Down at level 2 go through their own session
rather than RmlUi's native arrow-key handling, because `WidgetDropDown`
answers an Up/Down while its list is open by moving the selection
immediately, which fires a `change` event on every step, exactly the event
the menu's controls commit on. South on a closed select opens it and
highlights the currently selected option (the `padhl` pseudo-class, styled
identically to `:hover` in `ui/style/base.rcss`); Up/Down move the
highlight only, consumed here and never forwarded to RmlUi; South commits
the highlighted option (one `change`, the same as a mouse click on it);
East closes the list without committing, and the menu stays open (level 2,
not a level-1 East). Left and Right are swallowed while the list is open,
because forwarding them would move the focus off the select and the blur
that follows would close the list behind the session's back. The session
ends on its own if the select loses focus by any other means (a mouse click
elsewhere, Tab), and whenever the document being navigated changes, both of
which `ui_nav_tick()` notices at the next field boundary.

The pad is blanked for the game while the menu is up (`host/input.cpp`, see
the paragraph above this subsection): `rt_ui_wants_input()` reads `g_ui.
visible` fresh every time `rt_input_poll()` runs, and the hotkey that flips
it is consumed and applied synchronously inside the event pump
(`rt_window_pump`), not queued to a later field boundary the way a settings
change is. Whether the guest's own pad read for the field that opened or
closed the menu falls before or after that pump call is guest-scheduling
dependent (`rt_pad_run_due()`'s catch-up loop runs off the guest's virtual
clock, not off `rt_gs_vsync_hook`'s field count) and is not measured here;
what the mechanism guarantees is that no read ever mixes a stale visibility
flag with a stale pad sample, only ever the current pair.

Text fields are reachable and rebindable by pad. Up/Down leave the field at
once (`WidgetTextInput` never claims the vertical keys); Left/Right move the
cursor and only leave the field once it is already at that end
(`WidgetTextInput.cpp`, the `out_of_bounds` check). South commits the same
way Enter does on the keyboard. Three Debug fields (Verbose channels,
Profiler period, Frame limit) and the launcher's disc path field carry a
"type with the keyboard" hint for exactly this reason: a pad can focus and
commit them, but SDL3 only delivers `SDL_EVENT_TEXT_INPUT` from an attached
keyboard, so there is no way to type a character from the pad itself.

Gamepad hot-plug (`host/input.cpp`'s `rt_input_on_sdl_event`, called from
the one event pump ahead of the UI) keeps whichever pad is open current
whether or not a menu is up: attaching a second pad while one is already
open logs and keeps the first; unplugging the open one closes it and
opens whichever other pad SDL still lists, if any.

### Quit

The menu's footer carries a Quit button between Close and the settings
path, labeled `{{quit_label}}`. The first press arms it (`quit_label`
becomes "Press again to quit") for three seconds; the second press within
that window flushes any pending settings write and calls `rt_request_exit
("Quit from the menu")` (`host/window.cpp`), the same shutdown path a
closed window takes. The arm disarms on its own after three seconds with no
second press, and immediately on every way the menu can close (Close,
Escape, gamepad East) or reopen, so a Quit press left hanging never
survives past the moment the menu goes away. There is no settings key for
any of this; `quit_label` and the arm state are UI-only.

The launcher's own Quit button goes through the identical function
(`ui_launcher.cpp`), which is what lets its loop notice the request the same
way it would notice the window's own close button: `rt_request_exit` calls
`rt_pgs_notify_quit`, and the launcher's loop sees `RT_PGS_VSYNC_WINDOW_
CLOSED` on its very next `backend_present_ui()` call and exits through that
one branch, logged as "window closed or exit requested; exiting" either
way. In game the same flag is read by `hw/gspriv.cpp`'s vsync hook, which
logs "paraLLEl-GS: window closed or exit requested, exiting" and unwinds
from there.

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
(`RenderInterface::<Function>`), and the affected element draws
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
and reach the menu without a running game behind it. `main.cpp`
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
`src/runtime/ui/ui_launcher.cpp`) is drawn, like the in-game menu, in the
game's own palette: sand panels with black type and black rules (the
token table at the top of `ui/style/base.rcss`), and the wordmark tinted
black through RmlUi's `image-color`, which the overlay shader multiplies
into the white raster the disc gives. The window shows:

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

Every control here but Browse is reachable by gamepad the same way the
in-game menu is (section 7's Controller subsection): Start, Settings, Quit,
Clear, Use path, the show-at-startup checkbox and the credit link all carry
`tab-index: auto` already. Browse is the one exception: it opens the native
OS file dialog (`SDL_ShowOpenFileDialog`), a separate window this process
does not draw and so cannot feed pad events to; picking a disc from a pad
means typing its path into the field and pressing Use path instead.

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
edge. Coverage is binary, so premultiplied alpha is the colour or
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
(mounted and checked for `SCES_507.60`) before it is written anywhere: only
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
closed with no prompt.

## 9. Log lines to look for

By default the log keeps state changes, first occurrences, errors,
summaries and the ring dumps (SIF DMA, CD stream requests, and so on); it
does not keep a line for every SIF RPC command transfer, every sndn2
volume or pitch command, a timer MODE write that repeats the value already
logged, a CreateThread/StartThread/CreateSema past the first 32 of its
kind, most of the generic syscall trace for WakeupThread, iWakeupThread,
SleepThread, SifDmaStat, SifSetDma and SetSyscall, most rumble changes
past the first 8, or most sceCdStREAD polls. Those are the lines that
were measured to dominate a run's log without telling a reader anything
past the first several. `ICORECOMP_VERBOSE=sif,sndn2,timer,sched,syscall,
input,cdvd` (section 3) turns every one of them back on and reproduces the
old per-call output exactly; `all` (or `1`) turns on every channel this
build defines, not only these seven. See section 3 and `rt_verbose()`
(`log.cpp`) for how a channel spec is parsed.

These are exact prefixes and phrases, all under the `main`, `settings`,
`ui`, `launcher`, `gs`, `audio`, `input`, `screenshot`, `guest`, `json` or
`prof` log components, worth grepping `icorecomp.log` for. The level column
is the lowest `debug.log_level` (section 2) at which each line appears: a
line marked `info` is not in a default `warn` run's log, and a line marked
`debug` needs either `log_level = "debug"` or its own channel named in
`ICORECOMP_VERBOSE`. Warn and error lines are in every run's log. Lines marked
`always` are part of the startup prologue, which no level filters.

| phrase | level | meaning |
|---|---|---|
| `settings: loaded from` | info | which file the runtime actually read |
| `is overridden by` | info | an environment variable is winning over settings.json for one key |
| `kept default` | warn | a bad value in the file was rejected; the key stayed at its default |
| `settings.json.bad` | warn | the file failed to parse and was preserved under this name |
| `no longer a setting` | warn | the file still holds a retired key (`display.hires_scanout`, `input.trigger_threshold`, `input.rumble` or `debug.menu_hit_editor`); the line names it and what replaced it |
| `is no longer read` | info | the file still holds one of the eight retired keys: the seven removed with the settings pass of 2026-09-04 (`display.present`, `display.present_rate`, `display.deinterlace`, `system.language`, `debug.verbose`, `achievements.sound_volume`, `achievements.log_progress_bits`) or `display.backend`, removed on 2026-09-05; the value is left in the file untouched and the line says what stands in its place |
| `super-sampling` | info | the render scale the paraLLEl-GS backend was created with, whether super-sampled textures are on, and whether it asked for high-resolution scanout |
| `render scale applied live` | info | a render scale change from the menu reached the backend |
| `(display.raster)` | info | which output frame the scanout is built at, `crt` or `window`, and in `window` that the DBX/DBY read offset is ignored (section 6); logged at startup and again on every change from the menu |
| `(display.deinterlace)` | info | how an interlaced scanout is composed; always `bob` in a shipped run (section 6), logged at startup |
| `scanout internal` | info | the scanout geometry; its `ss=`, `hires=` and `deint=` fields are the super-sampling rate in force, whether the renderer actually scanned out at high resolution, and the deinterlace mode the field was composed with |
| `widescreen: mode=` | info | the `display.widescreen` mode in force, the aspect it resolved to and the factor `k` the game's own projection X scale is multiplied by; one line per change, and in `window` mode a resize is a change (section 6) |
| `widescreen: 2D held at 4:3` | info | the second half of the same change: 2D X coordinates are scaled back about x=2048 by the same `k` and full-frame passes are left alone |
| `widescreen: presenting at` | info | the scanout is being presented at the widescreen target aspect instead of the one derived from the CRTC registers; once per distinct aspect |
| `widescreen: field` | debug (`widescreen` channel) | per field: how many primitives the 2D transform rewrote, left as full-frame passes or judged 3D, and the VU1 kick count with the set of microprogram hashes bound that field. For the first 32 fields after a mode change the same channel adds one line per classified primitive with its path, type, X extent and decision (section 6) |
| `profiling on:` | info | the frame-time profiler is active, and how often it reports |
| `RenderInterface::` | warn | a stylesheet under `ui/` used something the overlay renderer cannot draw |
| `title logo:` | info, warn on a failure | the launcher's title image: a cache hit, or each step of building one from the disc, with timings (section 8) |
| `SDL gamepad:` | info, warn if the open failed | which gamepad was opened at probe time, or that none was found; `host/input.cpp`, once a run, from whichever probe gets there first: `rt_input_sdl_gamepad_probe()` at UI init, or `sdl_poll`'s own on the first poll if there was no SDL window yet at UI init |
| `SDL gamepad attached:` / `SDL gamepad removed:` | warn | hot-plug: a second pad was seen and ignored (the line names which one stayed open), or the open pad was unplugged and another took over, if any (`rt_input_on_sdl_event`, section 7) |
| `menu hotkey:` | info | the resolved keyboard and gamepad menu hotkeys, the gamepad half spelled `first+second` when `input.gamepad.menu` is a chord; logged at UI init and again whenever the committed settings move |
| `rebind gamepad.menu` | info, warn on a rejection | the outcome of a capture on the one chord-eligible slot: the accepted name (a single button or `first+second`), a rejection, or a timeout (section 7, Remapping) |
| `exit requested:` | info | `rt_request_exit()` was called and named why (`Quit from the menu`, `Quit from the launcher`, or the window's own close); the shutdown that follows is the same one a closed window always takes |
| `window closed or exit requested` | info | the loop is unwinding because the window closed or `rt_request_exit()` was called; both reasons produce this one line, from `hw/gspriv.cpp`'s vsync hook in game and from `rt_launcher_run`'s own loop in the launcher |
| `no title logo from this disc:` | warn | the title image could not be built and the launcher kept its text title; the line says why |
| `menu cursor:` | info, warn if nothing could be cut out | which cursor the pointer draws on the game's own menus: the letter I cut out of the title image, with its size, hotspot and which end was found to be the point, or `no logo image; drawn arrow` (section 10) |
| `launcher gate:` | info | which of the two boot orderings this run took, and why (section 8) |
| `rebind ` | info, warn on a rejection | a capture starting, ending, being accepted or rejected (section 7) |
| `mouse look: captured` / `mouse look: released` | info | relative mouse mode was taken or freed, and why (section 2, mouse look); the game's own menus do not free it |
| `last device is now the controller` / `... keyboard and mouse` | info | the player picked up the other kind of device (section 2, last device used); the drawn menu cursor follows it |
| `guest menu: pointer takes the mouse` / `guest menu: pointer hands the mouse back` | info | the field's mouse motion switched between the drawn cursor on one of the game's menus and the camera stick; the take line says where the drawn cursor is, and both name the boot screen when it is one of those (section 10) |
| `guest menu: layout ... chain` | info | the layouts one of the game's menu screens is composed from, and how many selectable items they came to between them; once per screen (section 10) |
| `guest menu: kanban layout ...` | info | one of the boot screens the kanban system draws (the language choice, the 50/60 Hz choice) is up, with its item count and what the layout state words said at the same time; once per screen (section 10) |
| `guest menu: the kanban screen word ...` / `guest menu: kanban node ... names layout entry ...` | warn | the word that says which boot screen is up did not hold one of the thirty node slots, or the node did not name a row of the layout table; the value is in the line and the pointer leaves those screens alone (section 10) |
| `guest menu: layout ... item ... rect` | debug | where the pointer put each selectable item of one of the game's menu screens, in fractions of the presented picture; one line per item, once per screen (section 10) |
| `guest menu: select` | info | the pointer made the item under the cursor the selected one by writing the game's own selection word (section 10) |
| `guest menu: the game is swallowing` | info | the game set its one-frame no-navigation flag `0x0063B620` and the pointer deferred its write to the next field; once per run, and expected at a screen change (section 10) |
| `guest menu: click` | info | the pointer clicked one of the game's menu items: cross on the item named (section 10) |
| `previous run's log kept as` | always (startup prologue) | the last run's `icorecomp.log` was renamed to `icorecomp.prev.log` before this run's log was opened |
| `could not keep the previous run's log` | always (startup prologue) | the rename above failed and why; this run's log overwrote the previous one as before rotation existed |
| `screenshot` (the log component) | info, warn on a refusal, debug for the size lines | the screenshot path: which folder captures go to, the resolved hotkeys, and a `wrote <path> (WxH, N ms)` line per capture. One info line per run carries the overlay check, the byte comparison of the two copies of the first captured field (section 7, Screenshots); the PNG size against the present rectangle is at debug |
| `gameplay.run_any_direction is on` | info | the left stick is being pre-scaled; a full tilt runs in every direction |
| `guest menu: layout ... item ... fade ... mcsel ...` | debug | the game's own menu state (layout id, selected item, fade/transition state, memory card selector index) as read out of guest RAM by the host, logged once per change |
| `GS backend:` | info | which live GS renderer this run built, on which graphics API, and where the choice came from (the compiled-in default, or `ICORECOMP_GS_BACKEND`), with the list of the backends the build has. A native backend adds that it has not passed its parity gate. On the dump transport a separate line says no live backend is being created (section 6) |
| `restarting to apply` | info | a cold key (`debug.console` or `debug.log_file`) was changed from the menu while the launcher was up, so the run is ending and a new process is starting on the same command line to apply it (section 2). `settings.json` is written first, then the ordinary shutdown runs (`exit requested: restarting to apply <key>`, the GS backend's teardown and pipeline cache write, the log drain), and the successor is started last |
| `could not restart to apply` | error | the successor process could not be prepared (`CreateProcessW` failed, or this executable's own path could not be resolved or is no longer executable). The reason is in the line and in a message box; the run carries on with the value it started with, and the new value is in `settings.json` for the next launch |
| `settings: <key> can only be changed` | warn | a commit tried to move a cold key (`debug.console` or `debug.log_file`) with the game already running, and it was reverted. The menu disables both controls, so this line means something else wrote the settings struct. The guard carried a dead `display.backend` arm until 2026-09-05; `RtSettings` has no field for that key any more, so the arm is gone with it |
| `window created for` | info | the one window of the run, opened by the executable for the backend it is about to build: which backend asked, the size, and whether it carries `SDL_WINDOW_VULKAN`. Absent when the run is headless, where `SDL_Init(VIDEO) failed` or `SDL_CreateWindow failed` says why instead |
| `Renderer:` | info | the device the backend this run created is on, with its driver and API version, or `no renderer device created yet` when no live backend was built. Logged once, right after the backend exists; the menu's Display tab shows the same string (section 6) |
| `Feature support:` | info | for `parallel-gs`, `all required features present` or `missing:` and the requirements this device failed; for a native backend, which RHI backend it came up on. The other half of the pair above, and the first thing to read on a machine that shows no picture (`docs/MACOS.md`) |
| `achievements` (the log component) | info, warn on a store that could not be written | the local achievement observer: which store file it loaded and how much was unlocked, each unlock as it happens, a new game resetting the counters, and a load re-seeding the baseline. It also carries, on every run, one line per progress-bit transition and per menu layout change, which is how the trophy bit table gets resolved (`docs/ACHIEVEMENTS.md`) |
| `present t=` | debug (the `present` channel) | one line per present: a `steady_clock` timestamp in microseconds, the scanout serial that is on screen, and `new` or `repeat`. The measurement the decoupled present rate is checked with; see below |

### How a run ended, and the field watchdog

Every run writes one end-of-run block, whichever of the many exits the
process took: `main` returning, a fatal, a crash handler, `abort`, the
window closing, the restart that applies a cold key, or the `atexit` chain
catching a path that named no reason at all. The block is written exactly
once, by whichever exit gets there first, and its level says what kind of
ending it was: **info when the player asked for it, warn when nobody did.**
A warn-level block is in every default run's log.

The rule this exists to enforce: the log always says how a run ended and
why. It was written because three runs, on three different graphics
backends, ended shortly after the guest booted with an error on screen, no
dialog, and a log whose last line was an ordinary HLE warning.

The block looks like this, with `[icorecomp][run]` for the info form and
`[icorecomp][run][warn]` for the warn form:

```
[icorecomp][run][warn] ---- end of run ----
[icorecomp][run][warn] reason: quit requested by SDL_EVENT_QUIT with no window close behind it...
[icorecomp][run][warn] ended in phase: gameplay (of 9; the run got this far and no further)
[icorecomp][run][warn] state: phase=gameplay fields=734 presents=731 gif packets=61208; GS worker parked on an empty ring, last record Vsync, 0 bytes queued, 90114 records replayed; RHI idle; last syscall WaitSema(4); last RPC cdvdman fno=0x6; window on screen
[icorecomp][run][warn] wall time: 14.322 s; ending thread: EE (main)
[icorecomp][run][warn] log: C:\Games\ico\icorecomp.log
[icorecomp][run][warn] ---- end of run ----
```

Grep `---- end of run ----` first in any log. The `reason` line is the
answer to "why did it stop"; the `ended in phase` line is the answer to "how
far did it get".

**The phases**, in order. Each one is logged as it is reached
(`phase: <name>`, info), and the block names the last one the run got to:

| phase | what it means |
|---|---|
| `start` | nothing has happened yet; a run that ends here died before the log sink opened |
| `log init` | the log file is open and the crash handlers are installed |
| `settings` | `settings.json` was read and applied |
| `backend created` | the GS backend object exists, so device and swapchain setup returned |
| `window created` | the executable's one window is open |
| `launcher shown` | the launcher is drawing |
| `guest booted` | the scheduler is running translated code |
| `first field` | the first field boundary was reached |
| `first present` | something reached the swapchain |
| `gameplay` | 120 fields have completed, which is past every boot hazard |

**The exit-reason and watchdog lines.** All under the `run`, `window`,
`gs`, `sched` and `crash` components.

| phrase | level | meaning |
|---|---|---|
| `---- end of run ----` | warn, info on a user quit | the end-of-run block, one per run. The five lines between the two markers are the reason, the phase, the counters, the wall time and the log path |
| `phase:` | info | one phase transition. The order they appear in is the order the table above lists |
| `quit requested by` | warn, info when the player asked | what asked the run to end, named at the moment it arrived: `Quit from the menu`, `Quit from the launcher`, `restarting to apply <key>`, `the window's close button (SDL_EVENT_WINDOW_CLOSE_REQUESTED)`, or `SDL_EVENT_QUIT with no window close behind it`. The last one is at warn because nothing the player did asked for it, and SDL carries no source on that event |
| `the run is ending at a field boundary because the window is gone` | warn | the field boundary saw the close and is exiting, and the player did not ask. The `quit requested by` line above it says what did |
| `window closed or exit requested, exiting` | info | the same exit, when the player did ask |
| `the guest ended the run itself (Exit)` | warn | translated code called the Exit syscall. Not a player quit: every player quit comes through `rt_request_exit` instead, so this is the game deciding to stop, which on this port is more often a boot that went wrong than a feature |
| `the guest returned out of its entry function` | warn | translated code returned through the boot sentinel. Same class as the line above |
| `no field has completed for` | warn | the field watchdog: five seconds with no field boundary. The line carries the phase, the GS worker's state and last record, the bytes queued in the command ring, the last submitted RHI command list, the last syscall and RPC, and whether the window is minimised. Repeated every 30 s while the stall lasts. The run is never ended by this: a very slow field and a hung one look the same from outside, and killing the first would be its own bug |
| `fields are completing again after the stall above` | warn | the stall ended, with the same state line |
| `fields are still advancing but the guest has submitted no GIF traffic` | warn | ten seconds of field boundaries with nothing drawn: the guest is looping with nothing to submit. The state line carries the guest's last syscall and RPC |
| `the GS consumer spent` | warn | one present or device wait took two seconds or more. That is a driver or display stall, not a slow frame. Said once per run |
| `the EE has waited` | warn | the field sync point has waited five seconds for the GS consumer on a live window. Said once per run; the EE keeps waiting, because the consumer may still be inside a driver call that returns |
| `field watchdog started` | info | the watchdog is running, with its two thresholds |

**The crash block.** A fault writes this before it does anything else, on
the faulting thread, straight to the log file and flushed, bypassing the log
writer thread entirely. That matters because the writer may be the thread
that died, or may be waiting on a lock the faulting thread holds: a crash
line handed to another thread is a crash line that may never be written.

```
[icorecomp][crash][error] ---- crash ----
[icorecomp][crash][error] ACCESS_VIOLATION (code 0xc0000005) on thread "GS ring worker"
[icorecomp][crash][error] detail: the faulting access was reading
[icorecomp][crash][error] faulting instruction: 00007FF... in icorecomp-parallel-gs.dll+0x1a2f40
[icorecomp][crash][error] address touched: 0000000000000018 (not in any loaded module)
[icorecomp][crash][error] guest pc_hint: no guest context on this thread
[icorecomp][crash][error] registers: rip=... rsp=... rbp=...
[icorecomp][crash][error] backtrace (23 frames; the first few are this handler):
[icorecomp][crash][error]   #00 00007FF...  ico.exe+0x4b120
...
[icorecomp][crash][error] ---- crash ----
```

Module and offset come from `GetModuleHandleExW` on the address itself, and
the frames from `CaptureStackBackTrace`. Both are kernel32, so a crash dump
never depends on DbgHelp being installed or on symbols being present; the
cost is `module+offset` rather than function names, which a map file turns
back into a function. On POSIX the same two come from `dladdr` and
`backtrace`. After the block the queued lines are drained out behind it, so
the file reads "the crash, then whatever the run had queued before it".

A fatal (`rt_fatal`) writes its own `FATAL:` line the same synchronous way,
before the drain, before the message box and before the exit, and the
message box a run with no console shows carries the end-of-run reason and
the log path as well as the failure text.

### Checking the present

`ICORECOMP_VERBOSE=present` turns on one line per present: a timestamp in
microseconds, the scanout serial on screen, and `new` or `repeat`.

With the rate fixed at one present per field there is exactly one thing to
check, and it is the shipped behaviour: over a 60 second run the line count
is about 3600 on NTSC (3000 on PAL), every line carries a distinct scanout
serial, and none of them says `repeat`. The profiler's `present` sub-line
says the same thing with its `presents`, `fields` and `repeats` counts, the
last of which has to be 0.

A `repeat` in that log, or a `presents` count above `fields`, means
something is pumping the present path off the field boundary.

## 10. The pointer on the game's menus

ICO's own menus (the two PAL boot screens, the title screen's continue or new
game choice, the screen adjustment bar, the ten-slot load grid, the save
flow, the vibration question) are scene objects driven by the D-pad, cross and
triangle; the game has no pause menu. Nothing is authored on the host side to point at them. The game
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
screen adjustment screen's own level, the layout table those index into,
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
whole numbers in the object: a width (`+0x4C`, or the texture's width at
`+0x64` when it is zero), a height (`+0x48`, or `+0x60`), a centring flag
(`+0x44`), and an x and y (`+0x54` and `+0x50`). Those live in the game's own
2D layout space, which is 640 by 224 with the origin at the centre of the
picture: x is `+0x54 - 320`, or `-width/2` when the centring flag is set, and
y is `+0x50 - 113`. The height fields count half units, which is the shift by
3 against the width fields' shift by 4.

Those numbers make two boxes half a unit apart. `(x, y)` to
`(x + w, y + h/2)` is the box the highlight is scattered over. The quad the
object itself is drawn as adds 8 to the Y and subtracts 8 from the H, both in
GS 12.4 fixed point, just before `gif_StartPacketPath1`, so it runs from
`(x, y + 0.5)` to `(x + w, y + h/2)`. The pointer reports the drawn one; half
a unit is 0.002 of the picture's height, below the resolution of the hit test
and of the boxes the mapping was calibrated against.

That space maps onto the presented picture without the frame buffer's size
entering into it. The sprite emitter this path calls scales
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
selection word lives and where the custom item-select handler
(`exec_layout_texture`, put there by 0x10's own action function) moves it.
The word that handler is installed in has no address in
`src/runtime/guest/ico_syms.h`: it is not established on this build, and the
pointer neither reads nor writes it.
The save file select page 0x1d has the same chain. Several more screens in
the load and save flow are the same shape: 0x19, 0x2d and 0x2f take their two
items from 0x17, 0x1f and 0x20 from 0x18, 0x25 from 0x26, 0x15 from 0x17.

The pointer writes the current item of the layout that owns the item under
the cursor. The item-id mirror `0x0063B610` is written only when that layout
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
(`+0x2C`) and follow the object's four neighbour links (`+0x30` right,
`+0x34` left, `+0x38` down, `+0x3C` up), staying inside the range. An object
whose flag word at `+0x6C` has the hidden bit (0x10) set is skipped, because
`lt_link_layout` returns without drawing it, and so is one whose rectangle
lands entirely off the picture. The hidden bit is the per-frame half of that
field: `lt_next_layout` seeds it from the persistent bit (0x20) for every
object in the chain at the top of each frame, and a screen's own handler may
overwrite it for one object mid-frame. A hidden object is still walked
through, since it can sit between two visible ones.

A layout whose `+0x2C` is negative contributes nothing, even when objects are
reachable from its default item. That is the game's own gate:
`lt_next_layout` skips `lt_switch_layout` for such a layout,
`lt_switch_layout` returns immediately on one, and `lt_link_layout`
highlights nothing, so writing an item into it would hand it a highlight and
a navigable selection the game did not have. The chain log line is followed
by a line naming any layout skipped this way.

The screen adjustment screen is keyed differently, for the same reason its
selection word is different: its fifteen bar positions are scene objects
0x18B..0x199, reached by `la_adjust_screen`'s own loop rather than by any
link, and an item's value there is its level 0..14. The screen is recognised
by its object range covering all fifteen of them, which in the retail layout
table only layout `0x3C` does (its range is objects 392..420, and no other
entry's range contains 395..409). The hidden bit of `+0x6C` is not tested on
those fifteen: `la_adjust_screen` sets the persistent bit on all of them and
clears it on the one the level names, and `lt_next_layout` copies the
persistent bit into the hidden one, so all but the selected read as hidden.
The persistent bit itself is general and not that screen's:
`GetRealModelId(index, flag)` sets it to `flag & 1` on any scene object and
leaves every other bit alone, and the retail title screen ships its own two
items, scene objects 49 and 50, with it set. Lighting one of fifteen card positions is one use of it, and
on that screen it says nothing about where the fifteen places are.

### The two boot screens, which are not layout screens

The language choice and the 50/60 Hz choice are not in any of the above.
Neither is unconditional, either: they belong to the branch
`kanbanBootMcCheck` takes when the memory card's product block does not
load. Its state machine dispatches on states 0x60, 0x61, 0x64, 0x65 and 0xBE
(`asm/nonmatchings/src/kanbanBoot/kanbanBootMcCheck.s`, the compare chain at
001B9358 through 001B93CC); the two screens are the 0x64 and 0x65 states,
and a boot that reads the block runs 0x60, 0x61 and 0xBE with neither screen
drawn. No layout id word names them,
`lt_next_layout` does not run them, and the fade state is not what gates
them: they are drawn by the kanban system (`kanban.c`, `kanbanBoot.c`), which
keeps a list of its own with one node per screen and hands each node a layout
table entry to take its objects and its selection word from. A log from a run
of this port before they were handled shows what that looked like from the
outside: the pointer reported `layout 0x0 chain 0x0: 0 items` and never took
the mouse, while the player's 50/60 choice arrived as `SetGsCrt` PAL/NTSC
flips, so the game was reading the pad on a screen the pointer could not see.

What the pointer reads for them, all of it in
`src/runtime/guest/ico_syms.h` with the instruction each address was decoded
from:

* `0x0063C39C`, the kanban node the game is navigating. `kanbanReqAdd` sets
  it when it puts a screen up whose entry has a default item, and
  `kanbanReqDelFade` clears it when that screen is faded out. Six code sites
  touch the word in the whole ELF and those are all six.
* the node's `+0x00`, the layout table entry the screen is built from, which
  gives the screen's object range, its item links and its selection word; and
  the node's `+0x0C` bit 0, the fading-out flag `display_layout` gives a node
  no input at all through.

So the word the pointer writes is the same layout entry `+0x2C` it writes on
every other screen, the items come from the same scene object fields, and a
click is the same cross on the virtual pad: `display_layout` reads the same
pressed-this-frame word, follows the same four neighbour links (taking one
only when it is greater than zero), and stores 1 into the node's `+0x08` on
cross, which is what the boot state machine waits for. Triangle stores 2
there and the boot state machine ignores it.

Three things differ, each of them what the game does rather than a
simplification. Which screen is up comes from the node, not from the layout
id word, which holds an unrelated value while these screens are up: the run
that produced the line below reported `the layout state word says layout 0x7
fade 2` on the language screen. The item mirror `0x0063B610` is
not written, because `lt_next_layout` is what refreshes that word and it does
not run these screens. And the one-frame swallow flag `0x0063B620` is not
read, because neither `display_layout` nor the sprite emitter it calls looks
at it, so a hover writes on the field it happens on.

The two entries are layout 0, the language choice, five items in a column,
and layout 1, the 50/60 Hz choice, two items in a row whose values the boot
state machine turns into the NTSC/PAL word. Those ids are used for the log
line and nothing else: the screen is found through the node, so the one other
kanban screen that has selectable items, the memory card message on layout 5,
is pointed at by the same code without being named.

    guest menu: kanban layout 0x0 (the language screen): 5 items; the layout state word says layout 0x7 fade 2

The placement is the same seven fields with one number changed. The kanban
system's sprite emitter is `lt_link_layout`'s twin: the same shifts, the same
`-320` on x, and a y bias of -112 where `lt_link_layout`'s is -113. That is
one line of 224, 0.004 of the picture's height. Each path is given the bias
its own emitter applies rather than one averaged number.

### When the pointer owns the mouse

`rt_guest_menu_active()` is true when the reads are valid and there is at
least one selectable item with a rectangle on the picture: for a layout
screen, on some layout in the current screen's chain with the fade state at
2; for one of the kanban system's screens, on the entry the navigable node
names, where the fade state means nothing. Gameplay and the pre-title
cinematic both hold fade 2, and the item term excludes them structurally
rather than by name: the retail layout table gives each an empty scene object
range, a default and current item of -1 and no parent, so the chain is one
layout with nothing to reach and no rectangle to derive.

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

One frame belongs to the game whatever the cursor is doing. `0x0063B620` is
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
menu opened over a game menu, there is no hidden cursor to stand
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

- On every screen but the screen adjustment one, the word is the current-item
  field of the layout table entry of the layout that owns the item
  (`0x00533FE8 + layout * 0x38 + 0x2C`), which is the current layout or one
  of its ancestors in the chain above. The game re-derives the highlight from
  that field on every frame:
  `lt_prev_layout` calls `lt_link_layout` once per scene object the layout
  owns, and `lt_link_layout` draws the highlight sprite for the object whose
  index equals that field. A D-pad press does nothing more than write it, in
  `lt_switch_layout`. The item-id mirror `0x0063B610` is written alongside,
  because the game refreshes it from the same field at the top of every frame
  and a reader in between should see the two agree.
- On the screen adjustment screen the word is the level at
  `0x0028F4C0 + 0x2C`. That screen's handler `la_adjust_screen` (retail
  `0x001BE058`, layout `0x3C`) steps it with LEFT and RIGHT, clamped to 0 and
  14, and calls `GetRealModelId` for all fifteen positions every frame to
  light the selected one, so the level alone decides the highlight and the
  layout entry is left alone. It is the same word `gsb_controlBrightness`
  reads every frame as the alpha of a white full-frame sprite, so writing it
  is both the selection and the brightness. This screen was called the memory
  card check here until 2026-09-05; the derivation of what it really is lives
  in `guest/ico_syms.h`.

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

## 11. Which code reads each key

The schema tables in section 2 say what a key is. This one says which line
actually reads it once the loader, the commit and the applier are done with
it, so a key can be checked end to end rather than trusted. It was written
by walking every key in section 2 through loader, validation, serializer,
applier and consumer, and it is the audit's permanent form: a key added
without a row here is a key nothing has been shown to read.

"Class" is the apply timing from section 2. "Consumer" is the code that
reads the applied value on the path a player exercises, never the applier:
`host/settings_apply.cpp` pushes a value, it does not use one. Where the two
GS backends differ the row names both.

| key | consumer | class |
|---|---|---|
| display.mode | `host/window.cpp` `rt_window_apply_mode`; `gs/gs_select.cpp` at startup | hot |
| display.window_width / window_height | `host/window.cpp` `rt_window_apply_mode`; `host/window_service.cpp` at creation | hot |
| display.remember_window_size | `host/window.cpp` `record_window_size` | hot |
| display.fit | `gs_parallel_present.cpp`; `gs/render/gs_native.cpp` `present_rect` | hot |
| display.filter | `gs_parallel_present.cpp` (VkFilter); `gs/render/gs_native.cpp` `blit_texture` | hot |
| display.raster | `gs_parallel_scanout.cpp`; `gs/render/gs_crtc.cpp` `crtc_plan` | hot |
| display.widescreen | projection: `guest/widescreen.cpp` `rt_widescreen_on_composer_entry`; 2D: `hw/gif.cpp` `ws_transform`; presentation: `gs_parallel_scanout.cpp`, and `gs/render/gs_native.cpp` through `rt_widescreen_present_aspect` | hot |
| display.render_scale | `gs_parallel_present.cpp`; `gs/render/gs_native.cpp` `set_render_scale` and `choose_hires` | warm |
| display.show_fps | `ui/ui_settings_model.cpp` `sync_fps_document`, drawn through `GsBackend::overlay_set_frame` on either backend | hot |
| display.screenshot_dir | `host/screenshot.cpp` `capture_dir` | hot |
| audio.master_volume | `host/audio.cpp` `sdl_submit` | hot |
| audio.mute | `host/audio.cpp` `sdl_submit`; `ICORECOMP_NO_AUDIO` instead stops the stream opening | hot |
| audio.music_volume | `snd/engine.cpp` `render`, on the stream voices as they are summed | hot |
| audio.effects_volume | `snd/engine.cpp` `render`, on every other voice as it is summed | hot |
| audio.movie_volume | `snd/engine.cpp` `mix_pcm`, on the `SgStPcm` channels | hot |
| audio.chime_volume | `host/audio.cpp` `chime_mix`, read fresh on every submit | hot |
| input.keyboard.\<slot\> | `host/input.cpp` `rebuild_tables` and the press loop | hot |
| input.keyboard.menu | `ui/ui_events.cpp` | hot |
| input.keyboard.screenshot | `host/screenshot.cpp` `rebuild_hotkeys` | hot |
| input.gamepad.\<slot\> | `host/input.cpp`, port 0 | hot |
| input.gamepad.menu | `ui/ui_events.cpp`, chord included | hot |
| input.gamepad.screenshot | `host/screenshot.cpp`, button and axis | hot |
| input.gamepad2.\<slot\> | `host/input.cpp`, port 1: the second SDL pad, reported on guest pad port 1 through `sif/pad.cpp` | hot |
| input.mouse.\<slot\> | `host/input.cpp`, held buttons and wheel pulses | hot |
| input.mouse.screenshot | `host/screenshot.cpp`, button and wheel | hot |
| input.left_deadzone / right_deadzone | `host/input.cpp` `apply_deadzone` | hot |
| input.mouse_look | `host/input.cpp` (the stick) and `host/mouse.cpp` (the capture) | hot |
| input.mouse_look_sensitivity | `host/mouse_look.h` `RtMouseLookStick::step` | hot |
| input.mouse_look_invert_y | `host/mouse_look.h` `rt_mouse_look_map` | hot |
| gameplay.run_any_direction | `host/input.cpp` `rt_stick_gate_expand` (`host/stick_shape.h`), both ports | hot |
| achievements.enabled | `guest/achievements.cpp` `rt_achievements_tick` | hot |
| achievements.toast | `guest/achievements.cpp` unlock queue and `rt_achievements_poll_toast`, which is what shows and hides the toast document in `ui/ui_achievements_model.cpp` | hot |
| achievements.sound | `guest/achievements.cpp` on an unlock, which queues the synthesised chime (`snd/chime.h`) through `rt_audio_play_chime` | hot |
| debug.log_level | `main.cpp` at startup, then the level gate in `log.cpp` | hot |
| debug.console | `rt_settings_peek_boot` -> `log.cpp` `rt_console_init`. The struct field itself is never read; the peek is the consumer (section 5) | cold, restarts |
| debug.log_file | `rt_settings_peek_boot` -> `log.cpp`, same shape | cold, restarts |
| debug.profile_fields | `prof.h` `rt_prof_field`, every field | hot |
| debug.fps_limit_hz | `hw/gspriv.cpp` `pace_period_seconds`; the `-1` sentinel resolves through `video_mode.cpp` `rt_field_rate_hz` | hot |
| launcher.show_at_startup | `main.cpp`, which decides whether the launcher opens | cold |
| launcher.disc_path | `iso/iso9660.cpp` `probe_and_mount`, resolved against `rt_base_dir()` | cold |

Two rows are worth reading twice.

`debug.console` and `debug.log_file` are the only keys whose consumer never
reads `rt_settings()`: `rt_log_init` has to decide about the console and the
log sink before `rt_settings_init` can run, so both come from
`rt_settings_peek_boot`, which parses the file on its own. The struct fields
exist so the menu can show them and a save can round-trip them.

The presentation words (`fit`, `filter`, `raster`, the fixed deinterlace
mode and `render_scale`) are pushed by `settings_apply.cpp` against the
words the backend was last given, not against the previous settings struct. A load
runs no applier, so a diff against the previous struct only ever carried an
edit, and a value that arrived in `settings.json` and was never touched
reached only a backend that read the settings itself when it was made.
paraLLEl-GS does that; the native renderer does not, and used its own
compiled-in defaults for the whole run until this was changed.
