/* guest/achievements.cpp: local achievements, observed from the game's own
 * progress bits.
 *
 * See achievements.h for the entry points, the four settings keys and the
 * statement of what is not resolved yet.
 *
 * ---- what is observed -----------------------------------------------------
 *
 * The addresses and sizes in this comment are SCES_507.60's, from
 * guest/ico_syms.h, which says how each one was measured; the code names
 * none of them and reads the constants. The array length in particular was
 * measured on this build rather than carried: RT_ICO_PROGRESS_BYTES is 0x32
 * bytes, where the decomp's own pair of save routines passes 0x2E.
 *
 * The game keeps one bit array of what the player has done: 0x002A50C0,
 * 0x32 bytes, 400 bits, with three accessors at 0x00181A48, 0x00181A70 and
 * 0x00181AA0 (the disc listing's gflagChk, gflagOn and gflagOff) that test,
 * set and clear a bit by index. The stage scripts drive it: read off the
 * decomp as a behavioural reference, src/st02a.c tests 0x40 and 0x41 and
 * sets 0x6A, src/st08b.c sets 0xF0 through 0xF8, and src/e3.c and
 * src/deja.c set 0x145 through 0x14B on the way out of the game. Those
 * particular bit numbers are not what anything here turns on; the trophy
 * table in guest/ico_syms.h is, and it is measured. It is also what the
 * save carries: the routine
 * at 0x001819A0 writes the 4-byte word 0x0063AA04, then the 4-byte word
 * 0x0063AA00 and then the 0x32 bytes to the card, and the routine at
 * 0x001819F8 reads the same three back.
 *
 * So one read of RT_ICO_PROGRESS_BYTES per guest field is the whole
 * observation. A bit
 * that goes from 0 to 1 is something the player just did. Nothing here
 * writes into guest memory and nothing here patches guest code; the module
 * would be removable without the game noticing.
 *
 * ---- the baseline rule ----------------------------------------------------
 *
 * The array does not only change one bit at a time, and this build says so
 * exactly. Its whole loaded image has six sites that form 0x002A50C0:
 * 0x00181A50, 0x00181A78 and 0x00181AA8 are the three per-bit accessors
 * above; 0x001819E0 is gflagSave passing the array to the card; 0x00181A30
 * is gflagLoad reading it back, which replaces the whole array when a save
 * is loaded; and 0x0018191C is gflagInit clearing all 0x32 bytes, which is
 * what a new game does, along with the 0x14-byte save header at 0x0029B9D0
 * (RT_ICO_SAVE_HEADER) that it clears twenty instructions later. Treating
 * either whole-array write as a burst of achievements would unlock most of
 * the set on the load screen.
 *
 * So a field where more than kBaselineReplaceBits bits change in either
 * direction, or where the array goes to all zeroes, is a replacement: the
 * baseline is re-seeded from the new array and no unlock is evaluated for
 * it. The all-zero case is further read as a new game, which resets the
 * profile's counters and increments its run count, because gflagInit is the
 * only one of those six sites that zeroes the array.
 *
 * kBaselineReplaceBits is provisional. A single scripted moment can
 * legitimately set several bits at once (from the decomp, as a behavioural
 * reference: src/st13c.c:108-109 sets 0x4A and 0x4B back to back and
 * src/access.c:249-250 sets 0x31 and 0x32), so the threshold has to sit
 * above the largest such burst and below the smallest load. The diagnostic
 * log is what sets it: every transition is printed with its field, at info
 * level and always, and the largest burst inside one field of ordinary play
 * is the number this constant has to clear.
 *
 * ---- the counters ---------------------------------------------------------
 *
 * Three host-side numbers per profile, none of which the game keeps anywhere
 * this port could read:
 *
 *   playtime_ms  accumulated on fields where the layout id is
 *                RT_ICO_LAYOUT_GAMEPLAY. That constant is the sentinel:
 *                the gameplay layout id is believed to be 0x36, the entry
 *                whose action function the disc listing calls la_game_loop,
 *                and it is not measured, so nothing is carried and playtime
 *                stays 0 until a run settles it. An earlier reading here
 *                said 0x32 and said it was established; the retail layout
 *                table shows 0x32's action function is la_delete_processing
 *                (guest/ico_syms.h carries the derivation). The clock is
 *                the nominal field period of the video mode the game
 *                programmed, not host wall time, so it counts guest time
 *                and a paused or stalled host does not inflate it.
 *   game_overs   the rising edge of the game-over layout id, which is not
 *                known (guest/ico_syms.h says why, and how to read it off
 *                the diagnostic log). While the sentinel stands this counter
 *                never moves and nothing derived from it can unlock.
 *   runs         incremented on a new game, that is on the all-zero
 *                replacement above.
 *
 * ---- the store -------------------------------------------------------------
 *
 * saves/achievements.json beside the memory card image, version 1, one
 * object per profile, keyed by the memory card slot the player is on
 * (RT_ICO_SAVE_CARD_INDEX) together with the completed-playthrough count
 * the game keeps in its save header (RT_ICO_SAVE_HEADER_CLEAR_COUNT).
 * docs/ACHIEVEMENTS.md carries the format.
 *
 * Why those two and not the header word alone. The header word is the
 * game-clear count, measured: la_save_processing writes it into the save
 * header from RT_ICO_CLEAR_COUNT on every save (guest/ico_syms.h cites the
 * addresses). Keyed on that alone, two separate playthroughs that are both
 * at clear count 0, which is every first playthrough, share one profile.
 * The card slot is the one further discriminator the decomp actually
 * supports: D_0063B550 is the card the memory card screens select and the
 * preview builder reads beside the file index, so it identifies which card
 * a save lives on. The file index is deliberately not in the key: it is the
 * load and save grid's own cursor and means nothing off those screens.
 *
 * What that still cannot separate, said plainly rather than left implied:
 * two playthroughs on the same card slot at the same clear count. Nothing
 * in guest memory that this port can read carries a per-playthrough
 * identity, so that grouping is the limit of what is measurable, not a
 * choice. Nothing can be wrongly unlocked by it either way: an unlock is
 * decided by a progress bit, never by which profile is in play.
 *
 * The clear count moves only when a playthrough finishes, and the card slot
 * only when the player changes cards, so an ordinary field re-keys nothing.
 * A change on a field that was not a replacement re-keys the profile in
 * place and keeps its counters; a change on a replacement field is a
 * different lineage and gets its own profile.
 *
 * Unlocks are permanent: nothing here ever clears one, and a new game resets
 * only the counters. The menu shows the union across profiles, so a trophy
 * earned on one save is earned.
 *
 * A file that will not parse is left alone. The run carries on with
 * everything locked, one warn line says so, and no write happens until an
 * unlock actually needs recording, so a hand-edited file is not clobbered by
 * a parse failure the player has not seen yet.
 */
#include "achievements.h"

#include "gmem.h"
#include "ico_syms.h"
#include "../target.h"

#include "../ee/kernel.h"
#include "../host/audio.h"
#include "../host/json.h"
#include "../host/portable.h"
#include "../runtime.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>

namespace {

/* ---- the trophy table ----------------------------------------------------
 *
 * The public PS3 "ICO Classics HD" list, sixteen trophies. Names, tiers and
 * descriptions are the published wording (PlayStation.Blog, 2011-08-15, for
 * names and tiers; TheSixthAxis, 2011-08-12, for the descriptions). No
 * source marks any of them hidden, so none is. Three conditions are
 * evaluated by this module itself: Express Journey (under four hours),
 * Castle Guide (under two hours) and Unscathed Escape (no game over), all
 * three at the Emancipation edge; Enlightenment follows the other fifteen.
 * Every other trophy fires on a progress bit from the table in ico_syms.h.
 *
 * The two time trophies are judged on the game's own frame counter
 * (RT_ICO_TIME_FRAMES) while that word is known, and on host-counted
 * gameplay fields otherwise. Which one a profile was judged on is recorded
 * in the store, see the store section below.
 *
 * None of this is ROM data. No string, image or table here is read out of
 * the game.
 */
const RtTrophyInfo kTrophies[RT_TROPHY_COUNT] = {
    /* key, name, description, tier, hidden */
    {"rescue", "Rescue",
     "Rescue Yorda",
     RT_TROPHY_BRONZE, false},
    {"failure", "Failure",
     "Finish watching the demo scene confronting the Queen at the front gate stage",
     RT_TROPHY_BRONZE, false},
    {"armed_and_ready", "Armed and Ready",
     "Acquire Sword",
     RT_TROPHY_SILVER, false},
    {"east_gate", "East Gate",
     "Open East Gate",
     RT_TROPHY_SILVER, false},
    {"west_gate", "West Gate",
     "Open West Gate",
     RT_TROPHY_SILVER, false},
    {"farewell", "Farewell",
     "Finish watching farewell demo with Yorda at the front gate stage",
     RT_TROPHY_SILVER, false},
    {"royal_arms", "Royal Arms",
     "Acquire Queen's Sword",
     RT_TROPHY_GOLD, false},
    {"emancipation", "Emancipation",
     "Clear the game",
     RT_TROPHY_GOLD, false},
    {"split_the_watermelon", "Split the Watermelon",
     "Bring the watermelon to Yorda upon completing 2nd time through",
     RT_TROPHY_GOLD, false},
    {"spiked_club", "Spiked Club",
     "Acquire spiked club",
     RT_TROPHY_GOLD, false},
    {"shining_sword", "Shining Sword",
     "Acquire shining sword",
     RT_TROPHY_GOLD, false},
    {"bench_warmer", "Bench Warmer",
     "Save at all save points",
     RT_TROPHY_GOLD, false},
    {"express_journey", "Express Journey",
     "Beat the game within 4 hours",
     RT_TROPHY_GOLD, false},
    {"unscathed_escape", "Unscathed Escape",
     "Clear the game without viewing a game over screen",
     RT_TROPHY_GOLD, false},
    {"castle_guide", "Castle Guide",
     "Beat the game within 2 hours",
     RT_TROPHY_GOLD, false},
    {"enlightenment", "Enlightenment",
     "Acquire all trophies",
     RT_TROPHY_PLATINUM, false},
};

/* guest/ico_syms.h cannot include this module's header, so its trophy bit
 * table names the trophies through constants that mirror RtTrophyId. This
 * is where the two are held together: change the enum without changing the
 * mirror and the build stops here rather than repointing a bit at whatever
 * trophy took the index. */
static_assert(RT_ICO_TROPHY_RESCUE == RT_TROPHY_RESCUE, "trophy mirror drifted");
static_assert(RT_ICO_TROPHY_FAILURE == RT_TROPHY_FAILURE, "trophy mirror drifted");
static_assert(RT_ICO_TROPHY_ARMED_AND_READY == RT_TROPHY_ARMED_AND_READY, "trophy mirror drifted");
static_assert(RT_ICO_TROPHY_EAST_GATE == RT_TROPHY_EAST_GATE, "trophy mirror drifted");
static_assert(RT_ICO_TROPHY_WEST_GATE == RT_TROPHY_WEST_GATE, "trophy mirror drifted");
static_assert(RT_ICO_TROPHY_FAREWELL == RT_TROPHY_FAREWELL, "trophy mirror drifted");
static_assert(RT_ICO_TROPHY_ROYAL_ARMS == RT_TROPHY_ROYAL_ARMS, "trophy mirror drifted");
static_assert(RT_ICO_TROPHY_EMANCIPATION == RT_TROPHY_EMANCIPATION, "trophy mirror drifted");
static_assert(RT_ICO_TROPHY_SPLIT_THE_WATERMELON == RT_TROPHY_SPLIT_THE_WATERMELON,
    "trophy mirror drifted");
static_assert(RT_ICO_TROPHY_SPIKED_CLUB == RT_TROPHY_SPIKED_CLUB, "trophy mirror drifted");
static_assert(RT_ICO_TROPHY_SHINING_SWORD == RT_TROPHY_SHINING_SWORD, "trophy mirror drifted");

/* ---- tunables ------------------------------------------------------------ */

/* Bits changing in one field past which the array is read as a whole
 * replacement (a load or a new game) rather than as progress. PROVISIONAL,
 * see the baseline rule in the file header: 8 is a starting value chosen to
 * sit above the two-bit bursts the decomp shows and well below a load, and
 * the diagnostic log is what will replace it with a measured one. */
constexpr int kBaselineReplaceBits = 8;

/* Guest time per field: the field rate of the video mode the game
 * programmed (../video_mode.h), which is what the rest of the runtime
 * paces and times off. 59.94 Hz on NTSC, 50 Hz on PAL, read per field so a
 * PAL run that switches between its two display options keeps counting
 * real time. Counting fields rather than host milliseconds is what keeps
 * playtime a property of the run and not of the machine it ran on. */
double field_ms() { return 1000.0 / rt_field_rate_hz(); }

/* Under four hours, the Express Journey condition, and under two, the
 * Castle Guide condition, in host-counted gameplay fields. This is the
 * fallback clock, used while RT_ICO_TIME_FRAMES is unknown. */
constexpr uint64_t kExpressJourneyMs = 4ull * 60ull * 60ull * 1000ull;
constexpr uint64_t kCastleGuideMs = 2ull * 60ull * 60ull * 1000ull;

/* The same two conditions against the game's own in-game clock, which is
 * the better one where it is known: it is the number the game itself
 * counts, it survives a save and a reload, and it is what "in-game time"
 * means to a player.
 *
 * RT_ICO_TIME_FRAMES counts fields. How many of them make an hour depends
 * on which video mode the game is in, and RT_ICO_VIDEO_MODE is the game's
 * own word for that: 0 NTSC, 1 PAL. The two rates below are the nominal
 * ones, 60 and 50, and they are not this runtime's field rates (NTSC runs
 * at 59.94, video_mode.h). That is deliberate and it is the whole
 * conversion decision: rather than turn frames into milliseconds through
 * some rate and compare against two hours, the comparison stays in frames
 * against frames * a rate, which is exactly the condition the
 * RetroAchievements set for this disc uses (under 432000 frames at mode 0,
 * under 360000 at mode 1). So the trophy fires on the same count the rest
 * of that game's community measures it by, and no value gets rounded on the
 * way. At 59.94 Hz 432000 fields is 7207 seconds of wall time rather than
 * 7200; whether the game's own displayed clock divides by 60 or by 59.94 is
 * not measured here, and this comparison does not need to know. */
constexpr uint32_t kFieldsPerSecondNtsc = 60;
constexpr uint32_t kFieldsPerSecondPal = 50;
constexpr uint32_t kExpressJourneySeconds = 4u * 60u * 60u;
constexpr uint32_t kCastleGuideSeconds = 2u * 60u * 60u;

/* What a profile's two time trophies were judged against. Recorded in the
 * store so a row in that file can be read years later without guessing
 * which build wrote it. */
constexpr const char* kClockGuestFrames = "guest_frames";
constexpr const char* kClockGameplayFields = "gameplay_fields";

/* Gameplay milliseconds between store writes. The store is also written on
 * every unlock and at shutdown; this is only so that a run that ends in a
 * crash does not lose an hour of playtime. */
constexpr uint64_t kFlushIntervalMs = 60ull * 1000ull;

constexpr int kStoreVersion = 1;

/* ---- state --------------------------------------------------------------- */

struct Profile {
    /* The two words the key is made of. clear_count is the game's own
     * completed-playthrough count as of the last save; key_card is the
     * memory card slot, -1 until that word has been read. */
    uint32_t clear_count = 0;
    int32_t key_card = -1;
    uint64_t playtime_ms = 0;
    uint32_t game_overs = 0;
    /* 1, not 0: a profile exists because a playthrough is under way on it,
     * and that playthrough is run 1. Counting from 0 left the whole of a
     * fresh install's first playthrough reading "0 runs", because the array
     * is bss at boot and the New Game the player then picks zeroes an
     * already-zero array, which is not the all-zero edge the new-game rule
     * fires on. A store written by an earlier build keeps whatever number
     * it holds. */
    uint32_t runs = 1;
    int32_t card = -1;
    int32_t file = -1;
    /* Which build of this port wrote this profile's counters
     * (RT_TARGET_NAME). The store lives beside the memory card image and the
     * default saves directory is the same tree across builds, so a key
     * written by an earlier build of this port can collide with this
     * build's while meaning a different run. A profile whose
     * target is not this build's is kept, written back unchanged, and
     * otherwise ignored: it is never selected as the profile in play and
     * never contributes to the unlock union. Empty means the file predates
     * this member, in which case it is adopted as this build's, which is
     * what it was before the member existed. */
    std::string target = RT_TARGET_NAME;
    /* The key it was stored under, so a profile from another build
     * round-trips under exactly the key it arrived on. Empty for a profile
     * this run made. */
    std::string key;
    /* Which clock the two time trophies were judged against, once they have
     * been judged: kClockGuestFrames or kClockGameplayFields. Empty until
     * the run reaches the moment they are evaluated at. */
    std::string clock;
    /* Per trophy, the ISO-8601 UTC time it was unlocked under this profile,
     * or empty. Never cleared. */
    std::string unlocked[RT_TROPHY_COUNT];
};

/* Settings, with achievements.h's stated defaults. */
bool g_enabled = true;
bool g_toast = true;
bool g_sound = false;
/* The progress-bit diagnostic: one info line per progress-bit transition and
 * per layout id change. Always on, and no longer a setting. It is the log
 * that resolves the trophy bit table, it is what a user is asked for when a
 * trophy does not fire, and at info level it costs a line on a transition
 * and nothing at all on a field without one. rt_achievements_configure is
 * still handed the value (host/settings_apply.cpp passes true) so this file
 * keeps one place that decides it. */
bool g_log_bits = true;

bool g_inited = false;
std::string g_store_path;
/* Set when the store would not parse: nothing is written until an unlock
 * actually needs recording, so a hand-edited file survives a parse failure
 * the player has not seen yet. */
bool g_write_blocked = false;
/* Non-empty when the store on disk is a version this build does not know:
 * where store_save moves it before writing its own. See store_load. */
std::string g_version_backup_path;

std::vector<Profile> g_profiles;
int g_cur = -1;

RtTrophyStatus g_status[RT_TROPHY_COUNT];
RtAchievementCounters g_counters;

/* The last array seen, and whether there is one. */
uint8_t g_baseline[RT_ICO_PROGRESS_BYTES];
bool g_have_baseline = false;

uint32_t g_last_layout = 0;
bool g_have_layout = false;

double g_playtime_frac = 0.0;   /* sub-millisecond remainder */
uint64_t g_since_flush_ms = 0;

/* One line for the whole run, not one per field: every field of a boot
 * before the game has read the card takes that path. */
bool g_logged_unkeyed_field = false;

std::deque<int> g_toast_queue;
int g_toast_active = -1;
double g_toast_until = 0.0;

/* The bit table in force. Points at RT_ICO_TROPHY_BITS in a normal build;
 * the selftest swaps in a table of its own (achievements.h says why). */
const RtIcoTrophyBit* g_bits = RT_ICO_TROPHY_BITS;
int g_bit_count = RT_ICO_TROPHY_BIT_COUNT;
uint32_t g_gameover_layout = RT_ICO_LAYOUT_GAMEOVER;

/* ---- small helpers -------------------------------------------------------- */

int popcount8(uint8_t v) {
    int n = 0;
    while (v) {
        n += v & 1;
        v >>= 1;
    }
    return n;
}

/* The two accessors this module needs, with the "never fatal, false when the
 * page is unmapped" contract guest/gmem.h states once for the three observer
 * modules that share it. This module only ever reads. */
using rt_gmem::read_bytes;
using rt_gmem::read_word;

bool read_byte(uint32_t addr, uint8_t* out) {
    return read_bytes(addr, out, sizeof(*out));
}

/* The game's own in-game clock, in fields, and how many fields a second it
 * is counting. False when either of the two addresses is unknown, and on a
 * mode word holding something other than the 0 and 1 the game's own
 * note gives it, which is a fact to report rather than a value to assume a
 * meaning for. */
bool guest_clock(uint32_t* frames_out, uint32_t* rate_out) {
    if (RT_ICO_TIME_FRAMES == RT_ICO_SYM_UNKNOWN) return false;
    if (RT_ICO_VIDEO_MODE == RT_ICO_SYM_UNKNOWN) return false;
    uint32_t frames = 0;
    uint32_t mode = 0;
    if (!read_word(RT_ICO_TIME_FRAMES, &frames)) return false;
    if (!read_word(RT_ICO_VIDEO_MODE, &mode)) return false;
    if (mode > 1) {
        static bool said = false;
        if (!said) {
            said = true;
            rt_log_warn("achievements", "the video mode word at 0x%08x is %u, not the 0 (NTSC)"
                                        " or 1 (PAL) it is known to take; the game's own frame"
                                        " counter cannot be turned into a duration, so the two"
                                        " time trophies fall back to counted gameplay fields"
                                        " (this line is not repeated)",
                (unsigned)RT_ICO_VIDEO_MODE, (unsigned)mode);
        }
        return false;
    }
    if (frames_out) *frames_out = frames;
    if (rate_out) *rate_out = (mode == 1) ? kFieldsPerSecondPal : kFieldsPerSecondNtsc;
    return true;
}

/* Whether an entry's clear-count qualifier holds right now. `ok` is left
 * alone when the byte cannot be read, and the caller skips the entry: one
 * bit standing for two trophies is only usable while the word that
 * separates them can be read. */
bool clear_count_allows(int qualifier, bool* readable) {
    *readable = true;
    if (qualifier == RT_ICO_BIT_CLEAR_ANY) return true;
    if (RT_ICO_CLEAR_COUNT == RT_ICO_SYM_UNKNOWN) {
        *readable = false;
        return false;
    }
    uint8_t clears = 0;
    if (!read_byte(RT_ICO_CLEAR_COUNT, &clears)) {
        *readable = false;
        return false;
    }
    return (qualifier == RT_ICO_BIT_CLEAR_FIRST_RUN) ? (clears == 0) : (clears != 0);
}

/* "YYYY-MM-DDTHH:MM:SSZ". gmtime rather than portable.h's rt_localtime: a
 * timestamp in a file that may be copied between machines has to be UTC, and
 * portable.h carries no UTC helper. Kept local rather than added to that
 * header, which is shared by most of the runtime. */
std::string iso8601_utc_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tmv = {};
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    /* 80, not 32: every std::tm field is an int, so GCC's
     * -Wformat-truncation reasons about the whole int range for each of the
     * six conversions (11 characters each) and 32 is a truncation it can
     * prove. Sizing the buffer for the worst case the types allow is the
     * answer; the alternative would be pretending the range is narrower
     * than the type. */
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", tmv.tm_year + 1900,
        tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

/* The store key for a profile of this build: the card slot and the clear
 * count, prefixed with the build's name so a store shared with a profile
 * written by an earlier build of this port cannot collide on it. A card of
 * -1 is "not seen yet", which is what the fields before the game has
 * touched that word carry. */
std::string profile_key(int32_t card, uint32_t clear_count) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%s:card%d:%u", RT_TARGET_NAME, (int)card,
        (unsigned)clear_count);
    return buf;
}

/* ---- which trophies this port can ever unlock -----------------------------
 *
 * A trophy is "located" when something in this build can actually fire it:
 * an entry in the bit table, or a derived rule whose own inputs are located.
 * The menu shows the note on everything else, because a row that is merely
 * locked and a row that nothing can unlock are different states and the
 * player is owed the difference.
 */
bool bit_for(int trophy, int* bit_out) {
    for (int i = 0; i < g_bit_count; ++i) {
        if (g_bits[i].trophy != trophy) continue;
        /* An entry that needs the clear-count byte is not a condition this
         * build can evaluate unless that byte's address is known. */
        if (g_bits[i].clear_count != RT_ICO_BIT_CLEAR_ANY &&
            RT_ICO_CLEAR_COUNT == RT_ICO_SYM_UNKNOWN) {
            continue;
        }
        if (bit_out) *bit_out = g_bits[i].bit;
        return true;
    }
    return false;
}

void recompute_located() {
    for (int i = 0; i < RT_TROPHY_COUNT; ++i) g_status[i].condition_located = bit_for(i, nullptr);

    /* All three are evaluated at the Emancipation edge, so none can fire
     * before Emancipation itself can. Unscathed Escape needs the game-over
     * counter to mean something as well. */
    const bool emancipation = g_status[RT_TROPHY_EMANCIPATION].condition_located;
    g_status[RT_TROPHY_EXPRESS_JOURNEY].condition_located = emancipation;
    g_status[RT_TROPHY_CASTLE_GUIDE].condition_located = emancipation;
    g_status[RT_TROPHY_UNSCATHED_ESCAPE].condition_located =
        emancipation && g_gameover_layout != RT_ICO_LAYOUT_GAMEOVER_UNKNOWN;

    bool all = true;
    for (int i = 0; i < RT_TROPHY_COUNT; ++i) {
        if (i == RT_TROPHY_ENLIGHTENMENT) continue;
        if (!g_status[i].condition_located) all = false;
    }
    g_status[RT_TROPHY_ENLIGHTENMENT].condition_located = all;
}

/* ---- profiles ------------------------------------------------------------- */

/* This build's profiles only: a key written by another build of this port
 * is about a different run. */
int find_profile(int32_t card, uint32_t clear_count) {
    for (size_t i = 0; i < g_profiles.size(); ++i) {
        if (g_profiles[i].target != RT_TARGET_NAME) continue;
        if (g_profiles[i].key_card != card) continue;
        if (g_profiles[i].clear_count == clear_count) return (int)i;
    }
    return -1;
}

/* The profile in play. With none chosen yet this takes the one keyed on no
 * card and clear count 0, or makes it: never a second profile under a key
 * the store already holds, which would be written back over the first.
 * Which profile is in play is normally decided by the key words in the
 * tick; this is the fallback for the fields before they could be read. */
/* Before the game has said which card slot it is on, the current profile is
 * the last real one in the store (the one the player last played, and the
 * one the menu should list), not a fresh profile keyed on "no card". A
 * placeholder keyed -1 is made only when the store holds nothing else; it
 * is written back only if it earned an unlock (store_text). Measured need,
 * 2026-09-05: a run that stopped at boot left a "pal:card-1:0" profile in
 * the store and the menu listed it, empty, over the real one. */
Profile& current() {
    if (g_cur < 0) {
        int found = find_profile(-1, 0);
        if (found < 0) {
            for (size_t i = g_profiles.size(); i-- > 0;) {
                if (g_profiles[i].target == RT_TARGET_NAME && g_profiles[i].key_card >= 0) {
                    found = (int)i;
                    break;
                }
            }
        }
        if (found >= 0) {
            g_cur = found;
        } else {
            g_profiles.emplace_back();
            g_cur = (int)g_profiles.size() - 1;
        }
    }
    return g_profiles[(size_t)g_cur];
}

/* Zeros before any profile is in play, rather than making one: the UI asks
 * for these from the first field, and a profile made to answer a getter
 * would be written back to the store. */
void publish_counters() {
    if (g_cur < 0) {
        g_counters = RtAchievementCounters{};
        return;
    }
    const Profile& p = g_profiles[(size_t)g_cur];
    g_counters.clear_count = p.clear_count;
    g_counters.playtime_ms = p.playtime_ms;
    g_counters.game_overs = p.game_overs;
    g_counters.runs = p.runs;
    g_counters.card = p.card;
    g_counters.file = p.file;
}

/* The union across profiles: the earliest timestamp any profile recorded.
 * "Earliest" is a string compare, which is what ISO-8601 UTC is for. */
void rebuild_status() {
    for (int t = 0; t < RT_TROPHY_COUNT; ++t) {
        std::string best;
        for (const Profile& p : g_profiles) {
            if (p.target != RT_TARGET_NAME) continue;
            if (p.unlocked[t].empty()) continue;
            if (best.empty() || p.unlocked[t] < best) best = p.unlocked[t];
        }
        g_status[t].unlocked = !best.empty();
        g_status[t].unlocked_at = best;
    }
}

/* ---- the store ------------------------------------------------------------ */

std::string store_text() {
    RtJson root = RtJson::make_object();
    root.set("version", RtJson::make_number(kStoreVersion));
    RtJson profiles = RtJson::make_object();
    for (const Profile& p : g_profiles) {
        /* A placeholder keyed on no card is kept only for what it earned. */
        if (p.key_card < 0) {
            bool any = false;
            for (int t = 0; t < RT_TROPHY_COUNT; ++t) any |= !p.unlocked[t].empty();
            if (!any) continue;
        }
        RtJson o = RtJson::make_object();
        o.set("clear_count", RtJson::make_number((double)p.clear_count));
        o.set("playtime_ms", RtJson::make_number((double)p.playtime_ms));
        o.set("game_overs", RtJson::make_number((double)p.game_overs));
        o.set("runs", RtJson::make_number((double)p.runs));
        o.set("key_card", RtJson::make_number((double)p.key_card));
        o.set("card", RtJson::make_number((double)p.card));
        o.set("file", RtJson::make_number((double)p.file));
        if (!p.clock.empty()) o.set("clock", RtJson::make_string(p.clock));
        RtJson unlocked = RtJson::make_object();
        for (int t = 0; t < RT_TROPHY_COUNT; ++t) {
            if (p.unlocked[t].empty()) continue;
            unlocked.set(kTrophies[t].key, RtJson::make_string(p.unlocked[t]));
        }
        o.set("target", RtJson::make_string(p.target));
        o.set("unlocked", std::move(unlocked));
        profiles.set(p.key.empty() ? profile_key(p.key_card, p.clear_count).c_str()
                                  : p.key.c_str(),
                     std::move(o));
    }
    root.set("profiles", std::move(profiles));
    return rt_json_write(root);
}

void store_save() {
    if (g_store_path.empty()) return;
    if (g_write_blocked) return;
    if (!g_version_backup_path.empty()) {
        const std::string backup = g_version_backup_path;
        /* Once, whether or not the rename works: a rename that fails must
         * not stop the run's unlocks from being recorded, and must not be
         * retried on every flush. */
        g_version_backup_path.clear();
        std::error_code ec;
        std::filesystem::rename(g_store_path, backup, ec);
        if (ec) {
            rt_log_warn("achievements", "%s could not be renamed to %s (%s); it is written over"
                                        " with this build's own store",
                g_store_path.c_str(), backup.c_str(), ec.message().c_str());
        } else {
            rt_log_info("achievements", "%s was written by a newer build; kept as %s",
                g_store_path.c_str(), backup.c_str());
        }
    }
    if (!rt_json_write_file(g_store_path, store_text())) {
        /* rt_json_write_file has already logged the reason with strerror.
         * One line here names what was lost, and the run carries on. */
        rt_log_warn("achievements", "the store at %s was not written; this run's unlocks and"
                                    " counters are only in memory",
            g_store_path.c_str());
    }
}

/* Reads a non-negative integer member, keeping `fallback` when it is absent
 * or is not a number. A store with a garbled member is not a garbled store:
 * only a parse failure or a version this build does not know blocks the
 * write. */
double number_member(const RtJson& o, const char* key, double fallback) {
    const RtJson* v = o.find(key);
    if (!v || v->type != RtJson::Type::Number) return fallback;
    return v->number;
}

void store_load() {
    g_profiles.clear();
    g_cur = -1;
    if (g_store_path.empty()) return;

    std::FILE* f = rt_fopen_utf8(g_store_path.c_str(), "rb");
    if (!f) {
        /* No file is the ordinary first run. */
        rt_log_info("achievements", "no store at %s yet; starting from nothing unlocked",
            g_store_path.c_str());
        return;
    }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);

    RtJson root;
    std::string err;
    if (!rt_json_parse(text, &root, &err)) {
        g_write_blocked = true;
        rt_log_warn("achievements", "%s does not parse (%s); this run starts with nothing unlocked"
                                    " and the file is left alone until something is unlocked",
            g_store_path.c_str(), err.c_str());
        return;
    }
    const RtJson* version = root.find("version");
    const int v = (version && version->type == RtJson::Type::Number) ? (int)version->number : -1;
    if (v != kStoreVersion) {
        g_write_blocked = true;
        /* Kept, not overwritten. A file this build cannot read because it is
         * older or garbled is the player's own progress and the first unlock
         * replaces it (see unlock()); a file a *later* build wrote holds
         * unlocks this build has no other copy of, and the store is the only
         * place they live. The first write moves it aside under this name
         * rather than destroying it. */
        g_version_backup_path = g_store_path + ".v" + std::to_string(v) + ".bak";
        rt_log_warn("achievements", "%s is version %d, not %d; this run starts with nothing"
                                    " unlocked, and the first unlock renames it to %s rather"
                                    " than writing over it",
            g_store_path.c_str(), v, kStoreVersion, g_version_backup_path.c_str());
        return;
    }

    const RtJson* profiles = root.find("profiles");
    if (!profiles || profiles->type != RtJson::Type::Object) {
        rt_log_warn("achievements", "%s has no \"profiles\" object; starting from nothing unlocked",
            g_store_path.c_str());
        return;
    }
    for (const auto& entry : profiles->obj) {
        if (entry.second.type != RtJson::Type::Object) continue;
        const RtJson& o = entry.second;
        Profile p;
        p.key = entry.first;
        if (const RtJson* target = o.find("target")) {
            if (target->type == RtJson::Type::String) p.target = target->str;
        }
        /* This build's own profiles are re-keyed from their key words on
         * every write, because those words move; only a foreign one keeps
         * the key it arrived under. */
        if (p.target == RT_TARGET_NAME) p.key.clear();
        /* A key from another build carries a prefix this build's strtoul
         * would read as 0; the members are what is keyed on and they are
         * written on every profile. "save_counter" is the name a store
         * written before this word was identified used for the same number,
         * so it is still read: a player's counters survive the rename. */
        p.clear_count = (uint32_t)number_member(o, "clear_count",
            number_member(o, "save_counter", (double)std::strtoul(entry.first.c_str(), nullptr, 10)));
        p.playtime_ms = (uint64_t)number_member(o, "playtime_ms", 0.0);
        p.game_overs = (uint32_t)number_member(o, "game_overs", 0.0);
        p.runs = (uint32_t)number_member(o, "runs", 0.0);
        p.card = (int32_t)number_member(o, "card", -1.0);
        p.file = (int32_t)number_member(o, "file", -1.0);
        /* A store written before the key carried the card has no key_card
         * of its own; take the card it last recorded, which is what the
         * profile would have been keyed on had the key carried it. */
        p.key_card = (int32_t)number_member(o, "key_card", (double)p.card);
        if (const RtJson* clock = o.find("clock")) {
            if (clock->type == RtJson::Type::String) p.clock = clock->str;
        }
        if (const RtJson* unlocked = o.find("unlocked")) {
            if (unlocked->type == RtJson::Type::Object) {
                for (const auto& u : unlocked->obj) {
                    if (u.second.type != RtJson::Type::String) continue;
                    for (int t = 0; t < RT_TROPHY_COUNT; ++t) {
                        if (u.first != kTrophies[t].key) continue;
                        p.unlocked[t] = u.second.str;
                        break;
                    }
                }
            }
        }
        if (p.key_card < 0) {
            bool any = false;
            for (int t = 0; t < RT_TROPHY_COUNT; ++t) any |= !p.unlocked[t].empty();
            if (!any) continue; /* a placeholder an earlier build wrote; drop it */
        }
        g_profiles.push_back(std::move(p));
    }
    rebuild_status();

    int unlocked_count = 0;
    for (int t = 0; t < RT_TROPHY_COUNT; ++t) {
        if (g_status[t].unlocked) ++unlocked_count;
    }
    int mine = 0, foreign = 0;
    for (const Profile& p : g_profiles) {
        if (p.target == RT_TARGET_NAME) ++mine; else ++foreign;
    }
    rt_log_info("achievements", "%s: %d profile(s) for this build (%s), %d of %d unlocked",
        g_store_path.c_str(), mine, RT_TARGET_NAME, unlocked_count, (int)RT_TROPHY_COUNT);
    if (foreign > 0) {
        /* Kept and written back unchanged, not merged: a key from another
         * build of this port is about a different run, and its unlocks are
         * that build's. */
        rt_log_info("achievements", "%s also holds %d profile(s) written by another build of this"
                                    " port; they are left alone and take no part in this run",
            g_store_path.c_str(), foreign);
    }
}

/* ---- unlocking ------------------------------------------------------------ */

void evaluate_derived(bool emancipation_edge);

void unlock(int trophy, const char* reason) {
    if (trophy < 0 || trophy >= RT_TROPHY_COUNT) return;
    if (g_status[trophy].unlocked) return;

    const std::string when = iso8601_utc_now();
    Profile& p = current();
    p.unlocked[trophy] = when;
    g_status[trophy].unlocked = true;
    g_status[trophy].unlocked_at = when;

    rt_log_info("achievements", "unlocked %s (%s): %s", kTrophies[trophy].name,
        rt_trophy_tier_name(kTrophies[trophy].tier), reason);

    if (g_toast) g_toast_queue.push_back(trophy);
    /* The chime is synthesised by the runtime (snd/chime.h) and summed into
     * the host audio output; no audio asset ships with this port and the
     * game's own mix is not touched. */
    if (g_sound) rt_audio_play_chime();

    /* An unlock is what lifts a parse-failure block: there is now something
     * worth recording, and the player's own progress outranks a file this
     * build could not read. */
    g_write_blocked = false;
    evaluate_derived(trophy == RT_TROPHY_EMANCIPATION);
    store_save();
}

/* Express Journey, Castle Guide and Unscathed Escape are read off the
 * counters at the Emancipation edge, which is the only moment any of the
 * three is defined: "within 4 hours", "within 2 hours" and "without a game
 * over" are all statements about the run that just finished. Enlightenment
 * follows the other fifteen from anywhere.
 *
 * Two conditions, and both are needed. `emancipation_edge` is true only in
 * the unlock(RT_TROPHY_EMANCIPATION) call, so the counters are read at the
 * moment the run ended and never again. And the profile whose counters are
 * read has to be the one that ended: g_status is the union across every
 * profile in the store (rebuild_status), so gating on it would let any later
 * unlock, on any profile, be read as a finished run. The failure that
 * combination prevents: finish the game slowly with a game over, start a new
 * game, unlock anything ten minutes in, and all three of these fire on the
 * new profile's fresh counters.
 *
 * The consequence, stated rather than hidden: a profile that reaches the
 * ending after some other profile in the same store already recorded
 * Emancipation never sees this edge, because unlock() returns early on a
 * trophy the store already holds. Unlocks are permanent and these three are
 * statements about the run that recorded the ending. */
void evaluate_derived(bool emancipation_edge) {
    static bool in_progress = false;
    if (in_progress) return;
    in_progress = true;

    if (emancipation_edge && !current().unlocked[RT_TROPHY_EMANCIPATION].empty()) {
        Profile& p = current();
        /* Which clock, decided here and recorded on the profile. The game's
         * own frame counter where it is known, because that is the
         * in-game time the published condition is about; the host-counted
         * gameplay fields otherwise. */
        uint32_t frames = 0;
        uint32_t rate = 0;
        const bool by_frames = guest_clock(&frames, &rate);
        p.clock = by_frames ? kClockGuestFrames : kClockGameplayFields;
        const bool express =
            by_frames ? (frames < (uint64_t)kExpressJourneySeconds * rate)
                      : (p.playtime_ms < kExpressJourneyMs);
        const bool castle =
            by_frames ? (frames < (uint64_t)kCastleGuideSeconds * rate)
                      : (p.playtime_ms < kCastleGuideMs);
        char why[192];
        if (g_status[RT_TROPHY_EXPRESS_JOURNEY].condition_located &&
            !g_status[RT_TROPHY_EXPRESS_JOURNEY].unlocked && express) {
            if (by_frames) {
                std::snprintf(why, sizeof(why), "the game was completed in %u of the game's own"
                                                " frames, under the %u a four-hour run takes at"
                                                " %u fields a second",
                    (unsigned)frames, (unsigned)(kExpressJourneySeconds * rate), (unsigned)rate);
            } else {
                std::snprintf(why, sizeof(why), "the game was completed inside four hours of"
                                                " gameplay fields");
            }
            in_progress = false;
            unlock(RT_TROPHY_EXPRESS_JOURNEY, why);
            in_progress = true;
        }
        if (g_status[RT_TROPHY_CASTLE_GUIDE].condition_located &&
            !g_status[RT_TROPHY_CASTLE_GUIDE].unlocked && castle) {
            if (by_frames) {
                std::snprintf(why, sizeof(why), "the game was completed in %u of the game's own"
                                                " frames, under the %u a two-hour run takes at"
                                                " %u fields a second",
                    (unsigned)frames, (unsigned)(kCastleGuideSeconds * rate), (unsigned)rate);
            } else {
                std::snprintf(why, sizeof(why), "the game was completed inside two hours of"
                                                " gameplay fields");
            }
            in_progress = false;
            unlock(RT_TROPHY_CASTLE_GUIDE, why);
            in_progress = true;
        }
        if (g_status[RT_TROPHY_UNSCATHED_ESCAPE].condition_located &&
            !g_status[RT_TROPHY_UNSCATHED_ESCAPE].unlocked && p.game_overs == 0) {
            in_progress = false;
            unlock(RT_TROPHY_UNSCATHED_ESCAPE, "the game was completed with no game over");
            in_progress = true;
        }
    }

    if (!g_status[RT_TROPHY_ENLIGHTENMENT].unlocked) {
        bool all = true;
        for (int t = 0; t < RT_TROPHY_COUNT; ++t) {
            if (t == RT_TROPHY_ENLIGHTENMENT) continue;
            if (!g_status[t].unlocked) all = false;
        }
        if (all) {
            in_progress = false;
            unlock(RT_TROPHY_ENLIGHTENMENT, "every other trophy is unlocked");
            in_progress = true;
        }
    }
    in_progress = false;
}

/* ---- the field ------------------------------------------------------------ */

/* The stage the game is in, and the game's own map id where it is known,
 * for the diagnostic lines. Either may be absent; the line then says so
 * rather than printing a number that was not read. */
std::string where_now() {
    std::string out;
    char buf[64];
    uint32_t v = 0;
    if (RT_ICO_STAGE_ID != RT_ICO_SYM_UNKNOWN && read_word(RT_ICO_STAGE_ID, &v)) {
        std::snprintf(buf, sizeof(buf), ", stage 0x%x", (unsigned)v);
        out += buf;
    }
    if (RT_ICO_MAP_ID != RT_ICO_SYM_UNKNOWN && read_word(RT_ICO_MAP_ID, &v)) {
        std::snprintf(buf, sizeof(buf), ", map %u", (unsigned)v);
        out += buf;
    }
    return out;
}

void handle_rising_bit(uint64_t field, int bit, uint32_t layout, uint32_t clear_count) {
    if (g_log_bits) {
        rt_log_info("achievements", "field %llu: progress bit 0x%x set, layout 0x%x, clear"
                                    " count %u%s",
            (unsigned long long)field, (unsigned)bit, (unsigned)layout, (unsigned)clear_count,
            where_now().c_str());
    }
    for (int i = 0; i < g_bit_count; ++i) {
        if (g_bits[i].bit != bit) continue;
        char reason[160];
        const int qualifier = g_bits[i].clear_count;
        if (qualifier == RT_ICO_BIT_CLEAR_ANY) {
            std::snprintf(reason, sizeof(reason), "the game set progress bit 0x%x",
                (unsigned)bit);
        } else {
            /* One bit standing for two trophies: the completed-playthrough
             * count is what says which. A count that cannot be read decides
             * nothing, so neither trophy fires and the line says why. */
            bool readable = true;
            const bool allowed = clear_count_allows(qualifier, &readable);
            if (!readable) {
                static bool said = false;
                if (!said) {
                    said = true;
                    rt_log_warn("achievements", "progress bit 0x%x stands for two trophies and"
                                                " the completed-playthrough count at 0x%08x could"
                                                " not be read, so neither is unlocked (this line"
                                                " is not repeated)",
                        (unsigned)bit, (unsigned)RT_ICO_CLEAR_COUNT);
                }
                continue;
            }
            if (!allowed) continue;
            std::snprintf(reason, sizeof(reason), "the game set progress bit 0x%x on a run whose"
                                                  " completed-playthrough count is %s",
                (unsigned)bit,
                qualifier == RT_ICO_BIT_CLEAR_FIRST_RUN ? "zero" : "not zero");
        }
        unlock(g_bits[i].trophy, reason);
    }
}

} // namespace

/* ---- public ---------------------------------------------------------------- */

const RtTrophyInfo& rt_trophy_info(int trophy) {
    if (trophy < 0 || trophy >= RT_TROPHY_COUNT) {
        static bool said = false;
        if (!said) {
            said = true;
            rt_log_warn("achievements", "trophy index %d is outside 0..%d (this line is not"
                                        " repeated)",
                trophy, (int)RT_TROPHY_COUNT - 1);
        }
        return kTrophies[0];
    }
    return kTrophies[trophy];
}

const char* rt_trophy_tier_name(RtTrophyTier tier) {
    switch (tier) {
    case RT_TROPHY_BRONZE: return "Bronze";
    case RT_TROPHY_SILVER: return "Silver";
    case RT_TROPHY_GOLD: return "Gold";
    case RT_TROPHY_PLATINUM: return "Platinum";
    }
    return "?";
}

const RtTrophyStatus& rt_achievements_status(int trophy) {
    if (trophy < 0 || trophy >= RT_TROPHY_COUNT) return g_status[0];
    return g_status[trophy];
}

const RtAchievementCounters& rt_achievements_counters() {
    return g_counters;
}

void rt_achievements_configure(bool enabled, bool toast, bool sound, bool log_progress_bits) {
    const bool was_toast = g_toast;
    g_enabled = enabled;
    g_toast = toast;
    g_sound = sound;
    g_log_bits = log_progress_bits;
    /* Turning toasts off drops whatever is queued rather than showing the
     * backlog the moment they are turned back on. */
    if (was_toast && !g_toast) {
        g_toast_queue.clear();
        g_toast_active = -1;
    }
}

void rt_achievements_init(const char* saves_dir) {
    /* The observer is reads of the progress bit array and the save header,
     * both at addresses from guest/ico_syms.h. Unknown addresses mean the
     * observer never starts: it stays uninitialized, so rt_achievements_tick
     * returns at once and nothing is read or unlocked. */
    if (!RT_ICO_ACHIEVEMENTS_KNOWN) {
        rt_log_warn("achievements", "the achievement observer is off: this build does not have"
            " the address of the game's own progress bit array (guest/ico_syms.h names what is"
            " missing). Nothing is read and nothing can unlock.");
        return;
    }
    std::string dir = (saves_dir && saves_dir[0]) ? std::string(saves_dir) : std::string();
    if (dir.empty()) {
        dir = std::string(rt_base_dir()) + "/saves";
        rt_log_info("achievements", "no saves directory was given; the store goes in %s",
            dir.c_str());
    }
    g_store_path = dir + "/achievements.json";

    recompute_located();
    store_load();
    publish_counters();
    g_inited = true;

    if (g_bit_count == 0) {
        rt_log_info("achievements", "the trophy bit table is empty: nothing can unlock from the"
                                    " game's progress bits in this build. The progress-bit lines"
                                    " below are what fills it in; see docs/ACHIEVEMENTS.md");
    }
    if (g_gameover_layout == RT_ICO_LAYOUT_GAMEOVER_UNKNOWN) {
        rt_log_info("achievements", "the game-over layout id is not known, so game overs are not"
                                    " counted and Unscathed Escape cannot unlock");
    }
    if (RT_ICO_LAYOUT_GAMEPLAY == RT_ICO_SYM_UNKNOWN) {
        rt_log_info("achievements", "the gameplay layout id is not known, so playtime is not"
                                    " counted here; the two time trophies use the game's own"
                                    " frame counter instead");
    }
    rt_log_info("achievements", "profiles in %s are keyed on the memory card slot and the"
                                " game's own completed-playthrough count; two playthroughs on"
                                " one card slot at the same count share a profile, which is the"
                                " limit of what guest memory can separate. Nothing unlocks from"
                                " the key: an unlock is a progress bit.",
        g_store_path.c_str());
    if (RT_ICO_TIME_FRAMES != RT_ICO_SYM_UNKNOWN) {
        uint32_t frames = 0, rate = 0;
        if (guest_clock(&frames, &rate)) {
            rt_log_info("achievements", "the game's own frame counter is at 0x%08x and the video"
                                        " mode word at 0x%08x reads %u fields a second; the two"
                                        " time trophies are judged on that clock",
                (unsigned)RT_ICO_TIME_FRAMES, (unsigned)RT_ICO_VIDEO_MODE, (unsigned)rate);
        }
    }
}

void rt_achievements_tick(uint64_t field) {
    if (!g_enabled || !g_inited) return;

    uint32_t layout = 0;
    if (read_word(RT_ICO_LAYOUT_ID, &layout)) {
        if (!g_have_layout || layout != g_last_layout) {
            if (g_log_bits) {
                rt_log_info("achievements", "field %llu: layout 0x%x -> 0x%x%s",
                    (unsigned long long)field, g_have_layout ? (unsigned)g_last_layout : 0xFFFFFFFFu,
                    (unsigned)layout, where_now().c_str());
            }
            /* The game-over edge, once the id is known. While the sentinel
             * stands this branch never runs. */
            if (g_gameover_layout != RT_ICO_LAYOUT_GAMEOVER_UNKNOWN && layout == g_gameover_layout &&
                (!g_have_layout || g_last_layout != g_gameover_layout)) {
                Profile& p = current();
                ++p.game_overs;
                rt_log_info("achievements", "game over %u on this profile", (unsigned)p.game_overs);
            }
            g_last_layout = layout;
            g_have_layout = true;
        }
    }

    /* The two words the profile key is made of, read before the array so a
     * field that can read one can read the other: they are in the same 64 KB
     * page as nothing else this depends on. */
    uint32_t clear_count = 0, card_word = 0;
    const bool have_clear = read_word(RT_ICO_SAVE_HEADER_CLEAR_COUNT, &clear_count);
    const bool have_card = read_word(RT_ICO_SAVE_CARD_INDEX, &card_word);
    const int32_t key_card = have_card ? (int32_t)card_word : -1;

    uint8_t bits[RT_ICO_PROGRESS_BYTES];
    if (!read_bytes(RT_ICO_PROGRESS_BITS, bits, RT_ICO_PROGRESS_BYTES)) {
        /* Before the ELF is loaded. Drop the baseline so the first mapped
         * field re-seeds instead of diffing against nothing. */
        g_have_baseline = false;
        return;
    }

    /* What changed, and whether it is progress or a replacement. */
    int changed = 0;
    bool all_zero = true;
    bool was_all_zero = true;
    for (uint32_t i = 0; i < RT_ICO_PROGRESS_BYTES; ++i) {
        if (bits[i] != 0) all_zero = false;
        if (g_have_baseline) {
            if (g_baseline[i] != 0) was_all_zero = false;
            changed += popcount8((uint8_t)(bits[i] ^ g_baseline[i]));
        }
    }
    const bool new_game = g_have_baseline && all_zero && !was_all_zero;
    const bool replacement = g_have_baseline && (new_game || changed > kBaselineReplaceBits);

    /* Which profile this field belongs to. Neither key word moves on an
     * ordinary field, so a change on a field that was not a replacement
     * re-keys the profile in place; one on a replacement field is a
     * different lineage. */
    /* Whether the profile this field lands on was made by this field. A new
     * profile already counts its own first run (Profile::runs starts at 1),
     * so the new-game branch below must not increment it as well. */
    const size_t profiles_before = g_profiles.size();
    /* No card known yet (the game's card word is negative until it has
     * read the card): neither select nor create a profile, and never write
     * -1 into the current one's key. */
    if (have_clear && key_card >= 0) {
        const auto make_profile = [&] {
            g_profiles.emplace_back();
            g_profiles.back().clear_count = clear_count;
            g_profiles.back().key_card = key_card;
            g_cur = (int)g_profiles.size() - 1;
        };
        if (g_cur < 0) {
            const int found = find_profile(key_card, clear_count);
            if (found >= 0) {
                g_cur = found;
            } else {
                make_profile();
            }
        } else if (g_profiles[(size_t)g_cur].clear_count != clear_count ||
                   g_profiles[(size_t)g_cur].key_card != key_card) {
            const int found = find_profile(key_card, clear_count);
            if (replacement || found >= 0) {
                if (found >= 0) {
                    g_cur = found;
                } else {
                    make_profile();
                }
                rt_log_info("achievements", "card slot %d, clear count %u: profile switched",
                    (int)key_card, (unsigned)clear_count);
            } else {
                g_profiles[(size_t)g_cur].clear_count = clear_count;
                g_profiles[(size_t)g_cur].key_card = key_card;
            }
        }
    }

    /* Set by the new-game branch below: that field resets the counters and
     * must not then add its own 16.68 ms to them. */
    bool skip_playtime = false;

    /* A field whose profile is not identified changes nothing at all.
     *
     * The block above already refuses to key or create a profile while the
     * card slot is unknown, but everything below it acts on whatever
     * current() lands on, and with no profile chosen current() returns the
     * last card-keyed profile in the store, which is the previous
     * playthrough's. The game's card word is negative until it has read the
     * card, and the first fields of a boot are also where gflagInit zeroes
     * the progress array, so without this the new-game branch below would
     * reset that earlier profile's playtime and game-over counts, bump its
     * run count and write it back, and handle_rising_bit would record
     * unlocks against it.
     *
     * The field is therefore observed and dropped. The baseline is left
     * exactly as it was, so progress made across such a window is not lost:
     * it is seen as one diff on the first field that does identify a
     * profile, and if the baseline was never seeded then that field seeds
     * it instead. */
    if (!have_clear || key_card < 0) {
        if (!g_logged_unkeyed_field) {
            g_logged_unkeyed_field = true;
            rt_log_info("achievements", "field %llu: the card slot is not readable yet, so this"
                                        " field is observed and nothing is written to any profile"
                                        " (this line is not repeated)",
                (unsigned long long)field);
        }
        publish_counters();
        return;
    }

    /* From here on there is a profile, and the keying above is what chose
     * it: both key words were readable, so current() returns the profile
     * that block settled on rather than a fallback. */
    Profile& profile = current();
    const bool profile_is_new = g_profiles.size() != profiles_before;

    uint32_t file = 0;
    profile.card = key_card;
    if (read_word(RT_ICO_SAVE_FILE_INDEX, &file)) profile.file = (int32_t)file;

    if (!g_have_baseline) {
        std::memcpy(g_baseline, bits, sizeof(g_baseline));
        g_have_baseline = true;
        int set = 0;
        for (uint32_t i = 0; i < RT_ICO_PROGRESS_BYTES; ++i) set += popcount8(bits[i]);
        rt_log_info("achievements", "field %llu: progress baseline seeded, %d bit(s) set",
            (unsigned long long)field, set);
    } else if (replacement) {
        if (new_game) {
            /* The field the array was zeroed on is the transition into the
             * new run, not a field of it, so the playtime block below skips
             * it: otherwise the counters this branch just reset are already
             * one field old by the time anything can read them. */
            skip_playtime = true;
            profile.playtime_ms = 0;
            profile.game_overs = 0;
            /* A profile made by this same field is already on its run 1; only
             * a new game on a profile that was already in play starts
             * another. */
            if (!profile_is_new) ++profile.runs;
            g_playtime_frac = 0.0;
            rt_log_info("achievements", "field %llu: the progress array was zeroed, which is a new"
                                        " game; counters reset, run %u",
                (unsigned long long)field, (unsigned)profile.runs);
            store_save();
        } else {
            rt_log_info("achievements", "field %llu: %d progress bits changed at once, which is a"
                                        " load rather than progress; baseline re-seeded",
                (unsigned long long)field, changed);
        }
        std::memcpy(g_baseline, bits, sizeof(g_baseline));
    } else if (changed != 0) {
        for (uint32_t byte = 0; byte < RT_ICO_PROGRESS_BYTES; ++byte) {
            const uint8_t diff = (uint8_t)(bits[byte] ^ g_baseline[byte]);
            if (diff == 0) continue;
            for (int b = 0; b < 8; ++b) {
                if (!((diff >> b) & 1)) continue;
                const int index = (int)byte * 8 + b;
                if ((bits[byte] >> b) & 1) {
                    handle_rising_bit(field, index, layout, clear_count);
                } else if (g_log_bits) {
                    rt_log_info("achievements", "field %llu: progress bit 0x%x cleared, layout"
                                                " 0x%x, clear count %u%s",
                        (unsigned long long)field, (unsigned)index, (unsigned)layout,
                        (unsigned)clear_count, where_now().c_str());
                }
            }
        }
        std::memcpy(g_baseline, bits, sizeof(g_baseline));
    }

    /* Playtime. See the file header on why this predicate is provisional. */
    /* The sentinel first. While the gameplay layout id is not known
     * RT_ICO_LAYOUT_GAMEPLAY is RT_ICO_SYM_UNKNOWN, and the intent there is
     * "never": without this test it would hold only by the accident that no
     * guest word ever reads 0xFFFFFFFF. The where_now, game-over and
     * guest_clock paths all test the sentinel explicitly; this one now does
     * too. */
    if (RT_ICO_LAYOUT_GAMEPLAY != RT_ICO_SYM_UNKNOWN && layout == RT_ICO_LAYOUT_GAMEPLAY
        && !skip_playtime) {
        g_playtime_frac += field_ms();
        if (g_playtime_frac >= 1.0) {
            const uint64_t whole = (uint64_t)g_playtime_frac;
            g_playtime_frac -= (double)whole;
            profile.playtime_ms += whole;
            g_since_flush_ms += whole;
        }
    }

    if (g_since_flush_ms >= kFlushIntervalMs) {
        g_since_flush_ms = 0;
        store_save();
    }

    publish_counters();
}

void rt_achievements_shutdown() {
    if (!g_inited) return;
    store_save();
    g_inited = false;
}

bool rt_achievements_poll_toast(double ui_time_seconds, int* trophy_out) {
    if (!g_enabled || !g_toast) return false;
    if (g_toast_active >= 0 && ui_time_seconds >= g_toast_until) g_toast_active = -1;
    if (g_toast_active < 0 && !g_toast_queue.empty()) {
        g_toast_active = g_toast_queue.front();
        g_toast_queue.pop_front();
        g_toast_until = ui_time_seconds + RT_ACHIEVEMENT_TOAST_SECONDS;
    }
    if (g_toast_active < 0) return false;
    if (trophy_out) *trophy_out = g_toast_active;
    return true;
}

#ifdef ICORECOMP_ACHIEVEMENTS_TEST
void rt_achievements_test_set_bits(const RtIcoTrophyBit* bits, int count) {
    g_bits = bits;
    g_bit_count = count;
    recompute_located();
}

void rt_achievements_test_set_gameover_layout(uint32_t layout) {
    g_gameover_layout = layout;
    recompute_located();
}

void rt_achievements_test_reset() {
    g_profiles.clear();
    g_cur = -1;
    for (int t = 0; t < RT_TROPHY_COUNT; ++t) g_status[t] = RtTrophyStatus{};
    g_counters = RtAchievementCounters{};
    g_have_baseline = false;
    g_have_layout = false;
    g_last_layout = 0;
    g_playtime_frac = 0.0;
    g_since_flush_ms = 0;
    g_logged_unkeyed_field = false;
    g_toast_queue.clear();
    g_toast_active = -1;
    g_toast_until = 0.0;
    g_write_blocked = false;
    g_version_backup_path.clear();
    g_store_path.clear();
    g_inited = false;
    recompute_located();
}

const char* rt_achievements_test_store_path() {
    return g_store_path.c_str();
}

void rt_achievements_test_flush() {
    store_save();
}
#endif
