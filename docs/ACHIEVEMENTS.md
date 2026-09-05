# Achievements

Local achievements for the port. Sixteen trophies, the public PS3 "ICO
Classics HD" set, unlocked by watching the game's own progress bits and kept
in a JSON file beside the memory card image. Nothing leaves the machine and
nothing is uploaded anywhere.

This is host-side observation in the strict sense. The module reads guest
memory once per field and never writes it, patches no guest code, and
changes no value the game supplied. Turning it off changes nothing the game
does.

## 1. What is observed, and where

Everything comes from a handful of reads per guest field, all of them
addresses listed in `src/runtime/guest/ico_syms.h`. They were read off
`SCES_507.60` on 2026-09-04, each address by one of two methods.
**correlation**: an address was located by the code sites that materialize
it, and accepted only when every one of those sites agreed; the count of
agreeing sites is quoted per row below and per constant in the header, in the
form `correlation: N/N sites`. The subcommand that ran that correlation was
removed when this became the only target, so the counts are the record of a
measurement made on 2026-09-04 rather than something a reader can re-run.
**RA**: the RetroAchievements set for this disc (game 1319: its `r=patch`
conditions and `r=codenotes2` notes, retrieved 2026-09-04), whose addresses
were measured on this build by that community.

Some rows below also quote the decomp of this game. It is read as a
behavioural reference, the way PCSX2 is, and never as the deciding source:
where a fact rests on it alone the row says so. `src/runtime/guest/ico_syms.h`
is the record for every address here and carries the same distinction per
constant.

| What | Address | Proved by |
|---|---|---|
| Progress bit array, 0x32 bytes (400 bits) | `0x002A50C0` | the decomp's `src/way_util.c`: three accessors test, set and clear a bit, all of them indexing the array as `[bit >> 3]`. correlation 3/3 sites, and RA names the same address as the flag array. The length is measured here, not carried: the two functions that move the array whole (`0x001819A0` and `0x001819F8`) pass a length of 50, where the earlier reading of the same pair passes 0x2E |
| The word saved beside the array | `0x0063AA04` | the save path writes it and then the array; the load path reads both back. Nothing reads it. Read off this build's save path (`0x001819A0` stores the stage id there at `0x001819B8`), 8 of 8 $gp-relative sites agree. That path then writes a second word, `0x0063AA00`, the clear count, which the earlier reading of this game's save path did not have |
| The stage id that word is copied from | `0x00639D10` | the save path copies it; correlation 39/39 sites, and RA's notes call the same word "Map ID 3". Logged beside every bit, never decided from |
| Live save header | `0x0029B9D0`, `0x14` bytes | zeroed with the array when a new game starts; copied into the preview buffer by `la_mc_confirm_save_file`. correlation 1/1 sites. The length is established at `0x14`: `gflagInit` clears the object for that many bytes at `0x00181984` (read off the decomp), and the save and load paths corroborate it by moving exactly `0x14` bytes in and out of the save record's per-file slot (`la_save_processing` at `0x001BD0C4` through `0x001BD0E8`, `la_load_processing` reading the same back). An earlier reading here said "unknown" because the next datum sits at header+0x18; that is header+0x14 plus four bytes of padding and is not evidence against the length. `guest/ico_syms.h` `RT_ICO_SAVE_HEADER_BYTES` carries it |
| Completed-playthrough count in the header | `+0x04`, measured | it is **not** a save counter, which is what this row said until the word was measured. `la_save_processing` loads `RT_ICO_CLEAR_COUNT` (`0x0063AA00`) $gp-relative at `0x001BD09C` and stores it at header + `0x04` at `0x001BD0B0`, off the base it forms at `0x001BD050`, so the word is the completed-playthrough count as of the last save. `guest/ico_syms.h` `RT_ICO_SAVE_HEADER_CLEAR_COUNT`; `achievements.h` and `achievements.cpp` call it `clear_count` throughout, and it is half of a profile's key |
| Current layout (screen) id | `0x0063B60C` | already in use by the menu pointer, `src/runtime/guest/menu_nav.cpp`. correlation 4/4 sites |
| Save block index | `0x0063B550` | `_la_set_preview_info` reads the product record array with this word as the index. correlation 5/5 sites. The stride is `0x1F0` (496 bytes), not the `0x18C` this row used to give: that number was carried from a reading of a different build's structure. Read off the decomp's card writer and reader, all five index multiplies over the array at `0x0029B5F0` use `0x1F0`, which is also the transfer length those two are given; the two addresses corroborate it without the decomp, since `0x0029B9D0 - 0x0029B5F0` is `0x3E0`, exactly two strides. `guest/ico_syms.h` `RT_ICO_SAVE_SLOTS` and `RT_ICO_SAVE_SLOT_STRIDE`; nothing in the runtime reads either |
| Save file index inside the block, 0..9 | `0x0063B4E4` | `_la_set_preview_info` sets it from the load and save grid's own selection every frame. correlation 1/1 sites |
| Map id | `0x0028FE74` | RA "[32 Bit] Map ID", the word 30 of the set's 31 conditions gate on. Logged, never decided from |
| In-game time, in fields | `0x0029B9D8` | RA "[32 Bit] Internal Time (Frames)". It is save header + 0x08, inside the object the game saves |
| Video mode word, 0 NTSC and 1 PAL | `0x0028F4C0` | RA "[32 Bit] NTSC / PAL Mode". Also the word `RT_ICO_BRIGHTNESS` is an offset from, correlation 143/143 sites |
| Completed playthroughs, one byte | `0x0063AA00` | RA "[8 Bit] Game Clear count". It is what separates the two secret weapons |

The bit array is the whole of the achievement signal. Every stage script in
the decomp drives it: `src/st02a.c` tests `0x40` and `0x41` and sets `0x6A`,
`src/st08b.c` sets `0xF0` through `0xF8`, `src/deja.c` and `src/e3.c` set
`0x145` through `0x14B` on the way out of the game, `src/op.c:220` sets
`0x140`, `src/st00a.c:26` sets `0x141`, `src/boyact.c:267` tests `0x15C`, and
the highest index at any call site is `0x166` (`src/access.c:247`).

### The baseline rule

The array does not only move one bit at a time. The load path replaces the
whole array when a save is loaded, and the new-game path zeroes it. A field where more than `kBaselineReplaceBits` bits change in
either direction, or where the array goes to all zeroes, is therefore read as
a replacement: the baseline is re-seeded and no unlock is evaluated. The
all-zero case is further read as a new game, since nothing else in the decomp
zeroes the array.

`kBaselineReplaceBits` is **provisional**, currently 8. It has to sit above
the largest legitimate one-field burst (the decomp shows two-bit bursts, for
example `src/st13c.c:108-109`) and below the smallest load. The diagnostic
log in section 5 is what replaces it with a measured number.

### The counters

Three numbers the game does not keep anywhere this port could read, so they
are counted host-side, per profile:

- `playtime_ms`: accumulated on fields whose layout id is the gameplay
  layout. **That id has not been measured on this build**, so the counter
  stays 0 and the two time trophies use the game's own frame counter instead
  (below). When it is measured, the clock is the nominal field period of the
  video mode the game programmed (`src/runtime/video_mode.h`), so this counts
  guest time and a stalled host does not inflate it. What the diagnostic run
  has to settle is not only the id: whether every playing field holds it, and
  whether the cinematics that hold the neighbouring id should count.
- `game_overs`: the rising edge of the game-over layout id. **That id is not
  known.** While it is unknown this counter never moves, and Unscathed Escape
  cannot unlock.
- `runs`: incremented on the all-zero replacement above, that is on a new
  game.

### The two time trophies and which clock they use

Express Journey ("Beat the game within 4 hours") and Castle Guide ("within 2
hours") are evaluated at the Emancipation edge, which is the only moment
either is defined.

The game's own frame counter is what they are judged on, because it is the
in-game time the published condition is about:
it is the number the game itself counts and it survives a save and a reload.
The counter counts fields, and how many make an hour depends on the video
mode, which the game's own word at `RT_ICO_VIDEO_MODE` gives: 0 NTSC, 1 PAL.
The comparison stays in frames against `seconds * rate` with rate 60 or 50,
rather than converting frames to milliseconds through some rate, so the
trophy fires on exactly the condition the RetroAchievements set for this
disc uses (under 432000 frames at mode 0, under 360000 at mode 1). Those two
rates are the nominal ones and deliberately not this runtime's field rates:
NTSC really runs at 59.94, so 432000 fields is 7207 seconds of wall time
rather than 7200. Whether the game's own displayed clock divides by 60 or by
59.94 is **not measured**, and this comparison does not need to know, because
it never leaves frames.

The fallback, if that counter is not known, is `playtime_ms`, the
host-counted gameplay fields above. Which of the two a profile was judged on
is written into its store row as `clock`, so a file can be read later without
guessing.

## 2. What is not resolved

Eleven of the sixteen trophies have an entry in the trophy bit table in
`src/runtime/guest/ico_syms.h`, taken from the RetroAchievements set for this
disc (game 1319, `r=patch` retrieved 2026-09-04, with the `r=codenotes2`
notes naming what each flag is for). Each RA condition names a byte of the
flag array and one of RA's bit prefixes (`M` = bit 0 through `T` = bit 7),
which is the same numbering the game's own accessors use, so the bit index is
`(byte address - array base) * 8 + bit`. Every index in the header was
recomputed from that JSON. What is measured is the bit index and the event
RA's code note names for it; which PS3 trophy each event is comes from
matching RA's description against the published PS3 wording, which is a
reading of two English sentences and is written out pair by pair in the
header so it can be checked.

A trophy with no entry stays locked and the menu says "condition not yet
located in this port" on its row rather than showing it as merely unearned.
Guessing a bit out of a plausible range was rejected: it would unlock on the
wrong event and nothing would notice.

| Trophy | Tier | Bit | The decisive measurement for what is left |
|---|---|---|---|
| Rescue | Bronze | 27 (`0x03`, bit 3), RA "Moonlit Girl", note "Cage - Yorda pick up" | the diagnostic playthrough |
| Failure | Bronze | 137 (`0x11`, bit 1), RA "End of all Hope", note "1st Gate - Escape False Hope" | the diagnostic playthrough |
| Armed and Ready | Silver | 200 (`0x19`, bit 0), RA "Riddle of Steel", note "Crest (Left 1) - Sword obtained" | the diagnostic playthrough |
| East Gate | Silver | 138 (`0x11`, bit 2), RA "Light in the Dark", note "First Beacon Lit" (map 50) | the diagnostic playthrough |
| West Gate | Silver | 139 (`0x11`, bit 3), RA "The Path is Open", note "Second Beacon Lit" (map 51) | the diagnostic playthrough |
| Farewell | Silver | 140 (`0x11`, bit 4), RA "Road to Freedom", note "2nd Gate - Road to Freedom" | the diagnostic playthrough |
| Royal Arms | Gold | 329 (`0x29`, bit 1), RA "The Blade Key", note "Jetty - Queen Sword Get" | the diagnostic playthrough |
| Emancipation | Gold | 355 (`0x2C`, bit 3), RA "You Were There", note "Beach - Ending Regular" | the diagnostic playthrough |
| Split the Watermelon | Gold | 354 (`0x2C`, bit 2), RA "Watermelon Sharing", note "Beach - Ending Watermelon" | the diagnostic playthrough, on a second run |
| Spiked Club | Gold | 114 (`0x0E`, bit 2) with the clear count 0 | the diagnostic playthrough |
| Shining Sword | Gold | 114, the same bit, with the clear count not 0 | the diagnostic playthrough, on a second run |
| Bench Warmer | Gold | **none**: the RA set has no achievement for the benches, so no source names a bit for them | the diagnostic playthrough: whether there is one bit per bench or one summary bit |
| Express Journey | Gold | evaluated at the Emancipation edge, on the game's own frame counter | nothing of its own |
| Unscathed Escape | Gold | **blocked**: the game-over layout id is not known | the layout id the picture is on when the player dies, read off the diagnostic log's layout change lines |
| Castle Guide | Gold | as Express Journey, at two hours | as Express Journey |
| Enlightenment | Platinum | depends on all fifteen above | none of its own |

The diagnostic playthrough is section 5: one run of this disc with the
log kept (the progress-bit log is always on, at info). For a trophy that already has a bit, it
is what confirms the bit fires at the moment the trophy names rather than at
some other one.

Where the decomp puts those bits. The ending scripts set the `0x140..0x15D`
range (`src/op.c:220` sets `0x140`, `src/st00a.c:26` sets `0x141`,
`src/deja.c` and `src/e3.c` set `0x145..0x14B`), and Royal Arms' index,
329 = `0x149`, falls inside the `src/e3.c` run, which is one independent
reason to think that index is right. The others are outside any range the
decomp names: Emancipation's 355 = `0x163` sits between `0x15D` and the
highest index at any call site, `0x166` (`src/access.c:247`), and Farewell's
140 = `0x8C` is in the stage-script range rather than the endgame one. That
is not a disagreement, since the decomp names only the call sites it happens
to name, but it is why the run in section 5 is still what settles them.

### Sources

- The trophy names, tiers and descriptions in
  `src/runtime/guest/achievements.cpp` are the published wording of the PS3
  list: names and tiers from the PlayStation.Blog announcement of
  2011-08-15, descriptions from TheSixthAxis, 2011-08-12. No source marks
  any ICO trophy hidden, so none is withheld, and no condition in the module
  is decided by a string.
- The bit indices, the map id, the in-game frame counter, the video mode
  word and the completed-playthrough byte come from the RetroAchievements
  set for game 1319 (PlayStation 2, "ICO"): the `r=patch` achievement
  conditions and the `r=codenotes2` code notes, both retrieved 2026-09-04.
  The conditions are the measurement; the notes are what says which event
  each flag stands for.
- Everything read out of the game's own code comes from the decomp, cited
  per fact above.

Two facts that were looked for and not found in the decomp: a guest playtime
counter, and any flag distinguishing a second playthrough. Two of the
addresses in section 1 answer both, and both come from the RA set: the
in-game frame counter at `0x0029B9D8` and the completed-playthrough byte at
`0x0063AA00`.

## 3. The file

`saves/achievements.json`, beside the memory card image. Version 1.

```json
{
  "version": 1,
  "profiles": {
    "pal:card0:7": {
      "clear_count": 7,
      "playtime_ms": 5231000,
      "game_overs": 0,
      "runs": 1,
      "key_card": 0,
      "card": 0,
      "file": 2,
      "clock": "guest_frames",
      "target": "pal",
      "unlocked": {
        "rescue": "2026-09-04T11:02:17Z"
      }
    }
  }
}
```

- One object per profile, keyed `<target>:card<slot>:<clear count>`: the
  memory card slot the player is on and the game's own completed-playthrough
  count, behind the name of the build that wrote it. `key_card` and
  `clear_count` carry the same two numbers as members, which is what is
  actually keyed on; the key string is rewritten from them on every save.
  `key_card` is -1 before the game has touched the card word.
- Why those two and not the count alone. The count is measured: the game
  writes it into its save header from its own game-clear word on every save
  (`guest/ico_syms.h` names the addresses). Keyed on that alone, two
  separate playthroughs both at count 0, which is every first playthrough,
  would share one profile and add their counters together. The card slot is
  the one further discriminator the decomp supports, being the card the
  memory card screens select and the preview builder reads.
- What it still cannot separate: two playthroughs on the same card slot at
  the same clear count. Nothing in guest memory that this port reads carries
  a per-playthrough identity, so that grouping is the limit of what is
  measurable. Nothing is unlocked wrongly by it either way: an unlock is
  decided by a progress bit, never by which profile is in play.
- Neither key word moves on an ordinary field. A change on a field that was
  not a replacement re-keys the profile in place and keeps its counters; a
  change on a replacement field (a load or a new game) is a different
  lineage and gets its own profile.
- `save_counter`, the member name a store written before this word was
  identified used for the same number, is still read, so a player's counters
  survive the rename. It is written back as `clear_count`.
- `target` is the build that wrote the profile, `pal` for this one. The store
  lives beside the memory card image and the default `saves` directory does
  not change with the build, so a key written by some other build can mean a
  different run at the same numbers. A profile whose `target` is
  not this build's, which now means one an older US build wrote, is kept and
  written back unchanged and takes no part in the run: it is never the
  profile in play and never contributes to the unlock union. One info line at
  startup says how many were left alone. A profile with no `target` member
  was written before the member existed and is adopted as this build's.
- `card` and `file` are the last values of the two guest save indices seen.
  `card` is half the key, recorded here as well so the file it wrote is
  readable on its own; `file` is recorded and never keyed on, being the load
  and save grid's own cursor, which means nothing off those screens.
- `clock` is which clock the two time trophies were judged against when the
  run reached the moment they are evaluated at: `guest_frames`, the game's
  own frame counter, or `gameplay_fields`, the host-counted `playtime_ms`.
  Absent until that moment.
- `unlocked` maps the trophy key to an ISO-8601 UTC timestamp. Keys are
  stable for the life of the file and never change when the enum is
  reordered.
- `runs` counts the playthroughs started on that profile and starts at 1: a
  profile exists because a playthrough is under way on it. A new game on a
  profile already in play adds one; a new game that lands on a fresh profile
  leaves it at 1, which is that profile's first run.
- Unlocks are permanent. Nothing clears one, and a new game resets only the
  counters. The menu shows the union across profiles, so a trophy earned on
  one save stays earned. Express Journey, Castle Guide and Unscathed Escape
  are read off the counters at the moment Emancipation is unlocked on the
  profile in play, and at no other moment: they are statements about the run
  that has just ended, and evaluating them on any later unlock would award them
  for a fresh profile's empty counters.
- Writes are atomic: `host/json.h`'s `rt_json_write_file` writes
  `achievements.json.tmp`, flushes and fsyncs it, then renames it over the
  target. The file is written on every unlock, on a new game, at shutdown,
  and once a minute of gameplay so a crash costs at most a minute.

**A file that will not parse is left alone.** The run starts with nothing
unlocked, one `warn` line names the parse error, and no write happens until
an unlock actually needs recording. That way a hand-edited file is not
clobbered by a parse failure the player has not seen yet. The first unlock
lifts the block and writes the file, because the player's own progress
outranks a file this build could not read.

**A `version` this build does not know is kept.** The run starts the same
way, with nothing unlocked and no write, but that file was written by a
later build and holds unlocks this build has no other copy of, so the first
unlock does not write over it: it is renamed to
`achievements.json.v<version>.bak` first, and both the warn line at startup
and an info line at the rename name the file it went to. A rename that fails
is logged and the write goes ahead, because the run's own unlocks still have
to be recorded somewhere.

## 4. Settings

Three keys, all host-side, all applied hot, all through
`rt_achievements_configure`. The chime's gain is `audio.chime_volume`
(docs/SETTINGS.md, sound), read fresh by the chime mixer in
`src/runtime/host/audio.cpp`. The progress-bit log has no key: it is always
on, at info, since 2026-09-04.

| key | type | default | apply | what it does |
|---|---|---|---|---|
| `achievements.enabled` | bool | `true` | hot | off, the per-field tick returns at once and nothing is read |
| `achievements.toast` | bool | `true` | hot | off, an unlock is still recorded and logged; nothing is drawn. Turning it off drops whatever is queued |
| `achievements.sound` | bool | `false` | hot | on, an unlock plays a chime the runtime synthesises itself (`src/runtime/snd/chime.h`); no audio asset ships with this port and the game's own mix is not altered |

None of them reads or changes a value the game supplied.

## 5. The playthrough that resolves the bit table

This is the one measurement that turns the feature on. It needs a full run of
the game with the log kept; the progress-bit log is always on at info.

1. Nothing to set: every progress-bit transition and every layout id change
   is logged at info by default.

   and set `debug.log_level` to `"info"` so the lines are kept. Leave
   `achievements.enabled` on.
2. Start a **new game**, so the array starts at all zeroes and every bit the
   run sets is a transition in the log rather than something a load brought
   in.
3. Play the game through to the credits. Along the way, note the wall-clock
   or in-game moment of anything a trophy is named after: freeing Yorda,
   picking up each weapon, each gate, each bench sat on, each save. A written
   list of moments is what pairs with the log; the log alone gives bit
   indices with no meaning attached.
4. Die at least once on purpose, and let the game-over screen come up fully.
   That is what puts the game-over layout id in the log.
5. Send `icorecomp.log`, plus the list of moments.

The log carries three line shapes, all on the `achievements` component:

```
field 123456: progress bit 0x4b set, layout 0x32, clear count 3, stage 0x7
field 123456: progress bit 0x4b cleared, layout 0x32, clear count 3, stage 0x7
field 123460: layout 0x32 -> 0x2e, stage 0x7
```

Every line carries the stage id and the map id (section 1), the latter as
`, map 23`. A bit index with nothing beside it is half a fact: the RA
conditions for this disc all gate on the map, so a line that names both can
be compared against a condition directly.

From those three:

- the bit set at each noted moment fills the trophy bit table in
  `src/runtime/guest/ico_syms.h`;
- the layout id the picture sits on after a death is the game-over layout id,
  which replaces the sentinel in that same header;
- the largest number of bits set in one field during ordinary play is what
  `kBaselineReplaceBits` in `src/runtime/guest/achievements.cpp` has to
  clear.

A second run is needed for Split the Watermelon and Shining Sword, both of
which are second-playthrough items.

## 6. Where the code is

| File | What |
|---|---|
| `src/runtime/guest/ico_syms.h` | the guest addresses, the trophy bit table, and the game-over layout sentinel |
| `src/runtime/guest/achievements.h` | the entry points, the trophy enum, the settings contract |
| `src/runtime/guest/achievements.cpp` | the per-field observation, the baseline rule, the counters, the store, the trophy text |
| `src/runtime/guest/achievements_selftest.cpp` | the standalone exercise, target `icorecomp-achievements-selftest` |
| `src/runtime/ui/ui_achievements_model.cpp` | the `achievements` data model, the tab's rows and the toast |
| `ui/menu.rml` | the Achievements tab |
| `ui/achievement_toast.rml`, `ui/style/achievements.rcss` | the unlock toast and the styles for both views |
