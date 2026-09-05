/* guest/achievements.h: local achievements, observed from the game's own
 * progress bits.
 *
 * The whole module is one read per guest field and a file beside the memory
 * card. It writes nothing into guest memory, ever, and patches no guest
 * code: it reads the progress array the game keeps (RT_ICO_PROGRESS_BITS,
 * RT_ICO_PROGRESS_BYTES bytes, guest/ico_syms.h: 0x32 bytes on this build,
 * measured rather than carried), notices bits going from 0 to 1, and matches
 * those against a table of trophy conditions. Everything a player sees comes
 * out of that plus four host-side counters.
 *
 * The trophy names and descriptions below are the public PS3 "ICO Classics
 * HD" list, authored on the host side. They are not ROM data and no part of
 * this module reads a string, an image or a table out of the game.
 *
 * ---- what is not resolved yet ---------------------------------------------
 *
 * Eleven of the sixteen trophies have an entry in the bit table in
 * guest/ico_syms.h, from the RetroAchievements set for this disc (game 1319,
 * retrieved 2026-09-04). Bench Warmer has no entry, because that set has no
 * achievement for the benches and so names no bit for them. A trophy with no
 * entry stays locked and reports "condition not yet located in this port".
 *
 * Guessing a bit out of a plausible range would unlock on the wrong event
 * and nothing would notice. The remaining indices come from a diagnostic
 * playthrough (the progress-bit transition log, always on at info), and
 * docs/ACHIEVEMENTS.md is the procedure.
 *
 * Two more things are unresolved: the game-over layout id, which is what
 * Unscathed Escape needs, and the bench set.
 *
 * ---- where the entry points are called from --------------------------------
 *
 *   rt_achievements_init      once, after the saves directory is resolved.
 *   rt_achievements_tick      once per guest field, from pad_field_tick
 *                             (sif/pad.cpp), beside rt_guest_menu_tick and
 *                             for the same reason: that is the one call per
 *                             field that already runs before the pad is
 *                             polled, whatever the input provider is.
 *                             Nothing in this module calls itself.
 *   rt_achievements_poll_toast  from the UI tick (ui/ui_achievements_model.cpp),
 *                             on the UI clock.
 *   rt_achievements_shutdown  once, at exit, to flush the counters.
 *
 * Nothing here is fatal. An unmapped guest page, a store that will not parse
 * and a directory that cannot be written all log and carry on, the same
 * contract guest/menu_nav.cpp and host/settings.cpp hold to.
 *
 * ---- the settings keys ----------------------------------------------------
 *
 * rt_achievements_configure takes the first three in this order.
 *
 *   achievements.enabled           bool   default true   hot
 *       Off, this module reads nothing and the tick returns at once.
 *   achievements.toast             bool   default true   hot
 *       Off, an unlock is still recorded and logged; nothing is drawn.
 *   achievements.sound             bool   default false  hot
 *       On, an unlock plays a chime the runtime synthesises itself
 *       (snd/chime.h) and host/audio.cpp sums into the samples handed to
 *       the device. No audio asset ships with this port and the game's own
 *       mix is not altered.
 *   audio.chime_volume             int    default 60     hot
 *       0..100, the gain on that chime (moved from achievements on
 *       2026-09-04). audio.master_volume and audio.mute still apply to
 *       the sum.
 *   (no key) the progress-bit log, on in every run since 2026-09-04
 *       The diagnostic. One info line per progress-bit transition and per
 *       layout id change. It is the fourth parameter of
 *       rt_achievements_configure and the runtime's only caller passes true
 *       (host/settings_apply.cpp); the parameter stays because the selftest
 *       turns the log off for the cases that are not about it.
 *
 * All of them are host-side only in the strict sense: none of them reads or
 * changes a value the game supplied.
 */
#ifndef ICORECOMP_GUEST_ACHIEVEMENTS_H
#define ICORECOMP_GUEST_ACHIEVEMENTS_H

#include <cstdint>
#include <string>

/* ---- the trophy set ------------------------------------------------------
 *
 * The sixteen of the PS3 list, in tier order. The enum value is the index
 * into the table rt_trophy_info() returns and is what RT_ICO_TROPHY_BITS
 * entries name; the string key beside each one is what the store file uses,
 * so reordering this enum never rewrites anybody's unlocks.
 */
enum RtTrophyId {
    RT_TROPHY_RESCUE = 0,
    RT_TROPHY_FAILURE,
    RT_TROPHY_ARMED_AND_READY,
    RT_TROPHY_EAST_GATE,
    RT_TROPHY_WEST_GATE,
    RT_TROPHY_FAREWELL,
    RT_TROPHY_ROYAL_ARMS,
    RT_TROPHY_EMANCIPATION,
    RT_TROPHY_SPLIT_THE_WATERMELON,
    RT_TROPHY_SPIKED_CLUB,
    RT_TROPHY_SHINING_SWORD,
    RT_TROPHY_BENCH_WARMER,
    RT_TROPHY_EXPRESS_JOURNEY,
    RT_TROPHY_UNSCATHED_ESCAPE,
    RT_TROPHY_CASTLE_GUIDE,
    RT_TROPHY_ENLIGHTENMENT,
    RT_TROPHY_COUNT
};

enum RtTrophyTier {
    RT_TROPHY_BRONZE = 0,
    RT_TROPHY_SILVER,
    RT_TROPHY_GOLD,
    RT_TROPHY_PLATINUM
};

struct RtTrophyInfo {
    /* Stable key in the store file. Never changes once shipped. */
    const char* key;
    const char* name;
    /* The published PS3 description, verbatim; see the table comment in
     * achievements.cpp for the sources. */
    const char* description;
    RtTrophyTier tier;
    /* Withheld in the menu until unlocked. No ICO trophy is hidden on the
     * public list, so this is false for all sixteen; the field stays so the
     * menu model has one rule. */
    bool hidden;
};

/* The sixteen entries, indexed by RtTrophyId. Out of range returns entry 0
 * after one log line; callers are expected to stay in range. */
const RtTrophyInfo& rt_trophy_info(int trophy);
/* "Bronze", "Silver", "Gold", "Platinum"; "?" outside the enum. */
const char* rt_trophy_tier_name(RtTrophyTier tier);

struct RtTrophyStatus {
    bool unlocked = false;
    /* ISO-8601 UTC, "YYYY-MM-DDTHH:MM:SSZ". Empty while locked. */
    std::string unlocked_at;
    /* False while nothing in this port can ever unlock it: no entry in
     * RT_ICO_TROPHY_BITS (guest/ico_syms.h) and no derived rule that can
     * fire. The menu shows the "condition not yet located in this port"
     * note for these, which is the honest state rather than a row that
     * looks merely unearned. */
    bool condition_located = false;
};

const RtTrophyStatus& rt_achievements_status(int trophy);

/* The counters of the profile currently in play. Host-side, per profile in
 * the store; see the file format in docs/ACHIEVEMENTS.md. */
struct RtAchievementCounters {
    /* The game's own completed-playthrough count as of the last save
     * (RT_ICO_SAVE_HEADER_CLEAR_COUNT). With the memory card slot it is
     * what a stored profile is keyed on. */
    uint32_t clear_count = 0;
    /* Accumulated on fields the layout id says are gameplay. Guest field
     * time at the nominal field rate of the programmed video mode, not
     * host wall time. Stays 0 while the gameplay layout id is not known
     * (guest/ico_syms.h), where the time trophies use the game's own
     * frame counter instead. */
    uint64_t playtime_ms = 0;
    uint32_t game_overs = 0;
    uint32_t runs = 0;
    /* The last values of the two guest words that say which save file the
     * player was last on (guest/ico_syms.h). The card slot is half the
     * profile key; the file index is recorded and never keyed on, being the
     * load and save grid's own cursor, which means nothing off those
     * screens. -1 before either has been seen. */
    int32_t card = -1;
    int32_t file = -1;
};

const RtAchievementCounters& rt_achievements_counters();

/* ---- entry points --------------------------------------------------------- */

/* The settings. Safe before init and safe to call again at any time: the
 * settings are hot. The compiled-in defaults of the three keys are true,
 * true and false (host/settings.h); the runtime always passes true for
 * `log_progress_bits`, and only the selftest passes false. */
void rt_achievements_configure(bool enabled, bool toast, bool sound, bool log_progress_bits);

/* Loads the store from `saves_dir`/achievements.json and seeds the unlock
 * state from it. `saves_dir` is the directory holding the virtual memory
 * card (sif/mc.cpp resolves it); null or empty falls back to
 * rt_base_dir()/saves, with one log line saying so. */
void rt_achievements_init(const char* saves_dir);

/* One guest field. Reads the progress array, the progress word, the layout
 * id and the save counter, edge-detects the bits, moves the counters, and
 * queues a toast for anything it unlocks. Read-only on guest memory. */
void rt_achievements_tick(uint64_t field);

/* Flushes the counters and stops. Safe to call without an init. */
void rt_achievements_shutdown();

/* The toast the UI should be showing, if any, on the UI clock in seconds
 * (Rml's own elapsed time; ui/ui_achievements_model.cpp passes it). One at a
 * time, RT_ACHIEVEMENT_TOAST_SECONDS long, the rest queued behind it.
 *
 * Returns true and writes the trophy index while one is up. Returns false
 * with nothing queued, and when achievements.toast is off. The first call
 * that returns true for a given toast is what starts its clock, so a UI that
 * stops polling (the overlay is not rendering) does not burn the toast. */
bool rt_achievements_poll_toast(double ui_time_seconds, int* trophy_out);

inline constexpr double RT_ACHIEVEMENT_TOAST_SECONDS = 4.0;

#ifdef ICORECOMP_ACHIEVEMENTS_TEST
/* Test-only seams, compiled in for guest/achievements_selftest.cpp and for
 * nothing else. They exist so the tests drive a bit table they control
 * rather than the shipped one: the edge detector, the derived rules and the
 * store round trip have to be exercised on pairings a case chooses, and
 * adding a pairing to guest/ico_syms.h to make a test pass would put an
 * unmeasured bit index in the one file that may only carry measured ones. */
struct RtIcoTrophyBit;
void rt_achievements_test_set_bits(const RtIcoTrophyBit* bits, int count);
void rt_achievements_test_set_gameover_layout(uint32_t layout);
/* Forgets everything: unlocks, counters, baseline, toasts and the store
 * path. The next rt_achievements_init starts from a clean slate. */
void rt_achievements_test_reset();
/* The store path in use, for a test that wants to read the file back. */
const char* rt_achievements_test_store_path();
/* Writes the store now, bypassing the flush cadence. */
void rt_achievements_test_flush();
#endif

#endif /* ICORECOMP_GUEST_ACHIEVEMENTS_H */
