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
| filter | enum | `linear`, `nearest` | `linear` | hot | - |
| render_scale | int, one of a set | 1, 2, 4, 8, 16 | 1 | warm | - |
| hires_scanout | bool | needs `render_scale >= 4`; below that it logs and stays off | false | warm | - |
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
| trigger_threshold | float | (0, 1] | 0.25 | hot | - |
| rumble | bool | - | true | hot | - |

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

The trigger threshold gates an axis bind's pressed state: the axis reads
past `trigger_threshold` of full scale, in the bound direction, to count as
pressed. The default 0.25 gives a raw threshold of 8191.75, and with the
`>` comparison the code uses, a trigger now counts as pressed at a raw axis
value of 8192, one unit below where the pre-settings build's hardcoded
`> 8192` fired (8193). That one-unit difference, out of 32767, is called
out in the comment above `sdl_poll()`'s trigger check in
`src/runtime/host/input.cpp` rather than hidden: reproducing 8193 exactly
would need a threshold of 8192/32767 = 0.2500076, not a number a user can
type into the menu.

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

### debug

| key | type | allowed / range | default | apply | env override |
|---|---|---|---|---|---|
| verbose | string | an `ICORECOMP_VERBOSE` channel spec; empty means the compiled-in default channels | `""` | hot | `ICORECOMP_VERBOSE` |
| log_file | bool | see section 5 | true | cold | `ICORECOMP_LOG` |
| profile_fields | int | [0, 100000]; 0 disables the profiler | 180 | hot | `ICORECOMP_PROFILE` |
| fps_limit_hz | double | 0, or [1, 1000]; 0 disables pacing | 59.94 | hot | `ICORECOMP_FPS_LIMIT` |

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

## 6. Render scale and display resolution

`display.render_scale` is paraLLEl-GS super-sampling: a fixed integer
multiple (1/2/4/8/16) of the game's own framebuffer resolution. Because it
scales the whole framebuffer uniformly, the aspect ratio is preserved
automatically and the game's 4:3 derivation is untouched. `hires_scanout`
asks the renderer for a higher-resolution scanout on top of that, which only
does anything at `render_scale >= 4`; below that it is logged and stays
inert rather than silently doing nothing.

The window or fullscreen display size (`display.mode`,
`display.window_width`/`window_height`) is a separate, independent setting:
it is only the surface the finished scanout is fitted into, using
`display.fit` (letterbox, integer scale, or stretch) and `display.filter`
(linear or nearest). Neither of these settings, nor any other setting in
this document, changes a value the game itself supplied. There is no
widescreen option and no game-speed option; both are out of scope for this
port by design.

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
  direction was pushed (60% is deliberately higher than
  `trigger_threshold`, so a resting stick on a worn pad cannot be
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
- file images (`LoadTexture`): only textures generated at runtime
  (`GenerateTexture`, i.e. text and RmlUi's own rasterized shapes) are drawn

Anyone editing a stylesheet under `ui/` should stay inside that subset.
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
  for names and titles: the nav names, the section titles, the launcher
  title and the credits byline.
- JetBrains Mono, `ui/fonts/JetBrainsMono-Regular.ttf`, for everything a
  value is read out of: labels, controls, values, hints, taglines, the
  footer and the field-rate readout. Every column width in
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
- **Credits**: shows the credits screen (below).
- **Quit**: closes the process without booting anything.
- **show-at-startup**: the on-screen checkbox for `launcher.show_at_startup`.

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

### Credits

Reachable from the launcher's Credits button and from the in-game menu's
About pane (`ui/credits.rml`, shown as its own document; the same text is
inline in the About pane). It lists "Developed by Nathanial Fine", a link
to `https://defnf.com`, and the "Built with" credits for paraLLEl-GS
(Arntzen Software, LGPLv3+, loaded as a separate shared library), SDL3
(zlib license), RmlUi (MIT), FreeType (FreeType License), Playfair Display
(SIL Open Font License, notice in `ui/fonts/PlayfairDisplay-OFL.txt`) and
JetBrains Mono (SIL Open Font License, notice in
`ui/fonts/JetBrainsMono-OFL.txt`). The site link opens through
`SDL_OpenURL`, the same call the About pane's link uses;
a build with no SDL, or a platform `SDL_OpenURL` cannot hand off to, logs
why and shows that in the status line instead of doing nothing.

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
| `hires scanout stays inert` | `hires_scanout` is set but `render_scale` is below 4 |
| `profiling on:` | the frame-time profiler is active, and how often it reports |
| `RenderInterface::` | a stylesheet under `ui/` used something the overlay renderer cannot draw |
| `launcher gate:` | which of the two boot orderings this run took, and why (section 8) |
| `rebind ` | a capture starting, ending, being accepted or rejected (section 7) |
