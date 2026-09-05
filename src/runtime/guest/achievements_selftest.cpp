/* guest/achievements_selftest.cpp: standalone exercise of the achievement
 * observer (guest/achievements.cpp).
 *
 * Links achievements.cpp and host/json.cpp against a stub rt_log and
 * rt_base_dir and against a g_pages array this file maps the guest words
 * into, the same arrangement guest/menu_nav_selftest.cpp uses. The guest
 * side is not stubbed: the harness writes real bytes at the real addresses
 * (guest/ico_syms.h) and the module reads them through rt_gptr, so every
 * case drives the path the runtime drives.
 *
 * The cases below do not drive the shipped trophy bit table. They install a
 * table of their own through the ICORECOMP_ACHIEVEMENTS_TEST seams, so that
 * no test needs a pairing added to guest/ico_syms.h to pass. The pairings in
 * it are invented for the test and are not claims about the game; what is
 * under test is the edge detector, the baseline rule, the derived rules and
 * the store, none of which cares which bit means what. The shipped table is
 * checked separately, as data, in test_bit_tables.
 *
 *     ./icorecomp-achievements-selftest [scratch-dir]
 *
 * The scratch directory holds achievements.json between cases and is
 * created if missing; it defaults to
 * ICORECOMP_ACHIEVEMENTS_SELFTEST_DIR, then to "./achievements-selftest".
 *
 * Exit code 0 = every check passed; 2 on the first failing CHECK.
 */
#include "guest/achievements.h"
#include "guest/ico_syms.h"
#include "target.h"
#include "host/json.h"
#include "recomp_api.h"
#include "runtime.h"

/* One executable, icorecomp-achievements-selftest (CMakeLists.txt), built
 * against the one set of constants in guest/ico_syms.h. The harness maps its
 * guest pages from those constants and takes the RT_ICO_SYM_UNKNOWN arms for
 * the words that are not resolved. */

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

/* ---- runtime stubs -------------------------------------------------------- */

/* The unlock chime (host/audio.h). The real one synthesises a buffer and
 * hands it to the audio sink; this harness links no audio at all, and what
 * this file tests is which unlocks happen, not what they sound like. */
void rt_audio_play_chime() {}

/* rt_gptr (ee/kernel.h) indexes this; mem.cpp owns it in the real runtime.
 * The harness maps only the pages the observed words fall in and leaves the
 * rest null, which is the "before the ELF is loaded" state the reader has to
 * cope with. The recomp_api.h include declares it inside extern "C"; without
 * that this definition would get C++ linkage and MSVC would not match it. */
uint8_t* g_pages[0x10000];

namespace {
std::string g_base_dir = ".";
/* Every line rt_log has been handed since log_clear(). Half of what this
 * module promises is a log line naming what it did or refused to do, and a
 * test that only checked the kept state would pass with every line
 * deleted. */
std::string g_log;
} // namespace

void rt_log_line(const char* component, const char* fmt, va_list ap) {
    char buf[1024];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    std::printf("[%s] %s\n", component, buf);
    g_log += buf;
    g_log += '\n';
}

void rt_log_error(const char* component, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); rt_log_line(component, fmt, ap); va_end(ap);
}
void rt_log_warn(const char* component, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); rt_log_line(component, fmt, ap); va_end(ap);
}
void rt_log_info(const char* component, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); rt_log_line(component, fmt, ap); va_end(ap);
}
void rt_log_debug(const char* component, const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); rt_log_line(component, fmt, ap); va_end(ap);
}

std::atomic<int> g_rt_log_level{(int)RT_LOG_INFO};

const char* rt_log_level_name(RtLogLevel level) {
    switch (level) {
    case RT_LOG_DEBUG: return "debug";
    case RT_LOG_INFO:  return "info";
    case RT_LOG_WARN:  return "warn";
    case RT_LOG_ERROR: return "error";
    }
    return "warn";
}

const char* rt_base_dir() {
    return g_base_dir.c_str();
}

/* ---- test harness ---------------------------------------------------------- */

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char* what, int line) {
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("FAIL (line %d): %s\n", line, what);
    std::exit(2);
}

#define CHECK(cond) check((cond), #cond, __LINE__)

void log_clear() { g_log.clear(); }
bool log_has(const char* needle) { return g_log.find(needle) != std::string::npos; }

std::string g_dir;
uint64_t g_field = 1;

/* The 64 KB pages every observed address falls in, derived from the
 * constants rather than named, so a change to an address in
 * guest/ico_syms.h moves the harness's pages with it instead of leaving it
 * mapping the wrong ones. Every address guest/achievements.cpp reads is
 * listed in want_pages() below; one that is RT_ICO_SYM_UNKNOWN is skipped,
 * which is the "this build cannot see that word" state the module has to
 * cope with anyway. */
struct PageSlot {
    uint32_t index = 0;
    std::vector<uint8_t> bytes;
};
std::vector<PageSlot> g_page_slots;

void want_page(uint32_t addr) {
    if (addr == RT_ICO_SYM_UNKNOWN) return;
    const uint32_t index = addr >> 16;
    for (const PageSlot& s : g_page_slots) {
        if (s.index == index) return;
    }
    PageSlot slot;
    slot.index = index;
    slot.bytes.assign(0x10000, 0);
    g_page_slots.push_back(std::move(slot));
}

void want_pages() {
    if (!g_page_slots.empty()) return;
    want_page(RT_ICO_PROGRESS_BITS);
    want_page(RT_ICO_PROGRESS_BITS + RT_ICO_PROGRESS_BYTES - 1);
    want_page(RT_ICO_SAVE_HEADER);
    want_page(RT_ICO_SAVE_HEADER_CLEAR_COUNT);
    want_page(RT_ICO_LAYOUT_ID);
    want_page(RT_ICO_SAVE_CARD_INDEX);
    want_page(RT_ICO_SAVE_FILE_INDEX);
    want_page(RT_ICO_CLEAR_COUNT);
    want_page(RT_ICO_TIME_FRAMES);
    want_page(RT_ICO_VIDEO_MODE);
    want_page(RT_ICO_MAP_ID);
    want_page(RT_ICO_STAGE_ID);
}

void map_pages() {
    want_pages();
    std::memset(g_pages, 0, sizeof(g_pages));
    for (PageSlot& s : g_page_slots) g_pages[s.index] = s.bytes.data();
}

/* Every mapped page back to zero, the state a fresh boot leaves bss in. */
void zero_pages() {
    want_pages();
    for (PageSlot& s : g_page_slots) s.bytes.assign(0x10000, 0);
}

void unmap_pages() { std::memset(g_pages, 0, sizeof(g_pages)); }

uint8_t* gptr(uint32_t addr) {
    uint8_t* page = g_pages[addr >> 16];
    return page ? page + (addr & 0xFFFFu) : nullptr;
}

void poke32(uint32_t addr, uint32_t v) {
    uint8_t* p = gptr(addr);
    CHECK(p != nullptr);
    std::memcpy(p, &v, sizeof(v));
}

void bit_set(int index) {
    uint8_t* p = gptr(RT_ICO_PROGRESS_BITS);
    CHECK(p != nullptr);
    p[index >> 3] = (uint8_t)(p[index >> 3] | (1 << (index & 7)));
}

void bit_clear(int index) {
    uint8_t* p = gptr(RT_ICO_PROGRESS_BITS);
    CHECK(p != nullptr);
    p[index >> 3] = (uint8_t)(p[index >> 3] & ~(1 << (index & 7)));
}

void bits_zero() {
    uint8_t* p = gptr(RT_ICO_PROGRESS_BITS);
    CHECK(p != nullptr);
    std::memset(p, 0, RT_ICO_PROGRESS_BYTES);
}

/* The layout the harness parks on so playtime accrues. While the gameplay
 * layout id is not known RT_ICO_LAYOUT_GAMEPLAY is the
 * RT_ICO_SYM_UNKNOWN sentinel, which is not a layout: writing it into the
 * guest word would make the observer count playtime on the strength of a
 * value that means "not known". The harness parks on an ordinary id there
 * instead, and every check that reads playtime is gated on
 * kHaveGameplayLayout. */
constexpr bool kHaveGameplayLayout = RT_ICO_LAYOUT_GAMEPLAY != RT_ICO_SYM_UNKNOWN;
constexpr uint32_t kPlayLayout = kHaveGameplayLayout ? RT_ICO_LAYOUT_GAMEPLAY : 0x12u;

void set_layout(uint32_t layout) { poke32(RT_ICO_LAYOUT_ID, layout); }
void set_clear_count(uint32_t n) { poke32(RT_ICO_SAVE_HEADER_CLEAR_COUNT, n); }
void set_card(uint32_t n) { poke32(RT_ICO_SAVE_CARD_INDEX, n); }

void field() { rt_achievements_tick(g_field++); }

/* The fake pairing. Thirteen trophies whose condition is a progress bit in
 * the shipped design, given thirteen distinct bit indices here. Invented for
 * the test; see the file header. */
const RtIcoTrophyBit kTestBits[] = {
    {RT_TROPHY_RESCUE, 0x20},
    {RT_TROPHY_FAILURE, 0x21},
    {RT_TROPHY_ARMED_AND_READY, 0x22},
    {RT_TROPHY_EAST_GATE, 0x23},
    {RT_TROPHY_WEST_GATE, 0x24},
    {RT_TROPHY_FAREWELL, 0x25},
    {RT_TROPHY_ROYAL_ARMS, 0x26},
    {RT_TROPHY_SPLIT_THE_WATERMELON, 0x27},
    {RT_TROPHY_SPIKED_CLUB, 0x28},
    {RT_TROPHY_SHINING_SWORD, 0x29},
    {RT_TROPHY_BENCH_WARMER, 0x2A},
    /* Last on purpose: the derived rules fire at this one's edge. */
    {RT_TROPHY_EMANCIPATION, 0x2C},
};
constexpr int kTestBitCount = (int)(sizeof(kTestBits) / sizeof(kTestBits[0]));

/* A layout id the harness uses as the game-over screen. The real one is not
 * known (guest/ico_syms.h); what is under test is the edge, not the id. */
constexpr uint32_t kTestGameOverLayout = 0x40;

/* The same table with one entry that needs the completed-playthrough byte.
 * Used by the clear-count case below. */
const RtIcoTrophyBit kTestBitsQualified[] = {
    {RT_TROPHY_RESCUE, 0x20},
    {RT_TROPHY_SPIKED_CLUB, 0x30, RT_ICO_BIT_CLEAR_FIRST_RUN},
    {RT_TROPHY_SHINING_SWORD, 0x30, RT_ICO_BIT_CLEAR_NEW_GAME_PLUS},
};
constexpr int kTestBitsQualifiedCount =
    (int)(sizeof(kTestBitsQualified) / sizeof(kTestBitsQualified[0]));

std::string store_path() { return g_dir + "/achievements.json"; }

void remove_store() {
    std::error_code ec;
    std::filesystem::remove(store_path(), ec);
}

std::string read_store() {
    std::FILE* f = std::fopen(store_path().c_str(), "rb");
    if (!f) return std::string();
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

void write_store(const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(g_dir, ec);
    std::FILE* f = std::fopen(store_path().c_str(), "wb");
    CHECK(f != nullptr);
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
}

/* A clean module on a clean page set, with the fake table installed and the
 * baseline already seeded from an all-zero array. Leaves the layout on
 * gameplay so playtime accumulates and the game-over edge is not armed. */
void fresh(bool keep_store) {
    rt_achievements_test_reset();
    rt_achievements_test_set_bits(kTestBits, kTestBitCount);
    rt_achievements_test_set_gameover_layout(kTestGameOverLayout);
    rt_achievements_configure(true, true, false, false);
    zero_pages();
    map_pages();
    set_layout(kPlayLayout);
    set_clear_count(1);
    if (!keep_store) remove_store();
    rt_achievements_init(g_dir.c_str());
    field(); /* seeds the baseline */
}

int unlocked_count() {
    int n = 0;
    for (int t = 0; t < RT_TROPHY_COUNT; ++t) {
        if (rt_achievements_status(t).unlocked) ++n;
    }
    return n;
}

/* ---- cases ----------------------------------------------------------------- */

/* The table itself: sixteen entries, unique keys, and the two the public
 * list hides. */
void test_table() {
    std::printf("\n== trophy table ==\n");
    CHECK(RT_TROPHY_COUNT == 16);
    for (int t = 0; t < RT_TROPHY_COUNT; ++t) {
        const RtTrophyInfo& info = rt_trophy_info(t);
        CHECK(info.key != nullptr && info.key[0] != 0);
        CHECK(info.name != nullptr && info.name[0] != 0);
        CHECK(info.description != nullptr && info.description[0] != 0);
        for (int u = 0; u < t; ++u) {
            CHECK(std::strcmp(rt_trophy_info(u).key, info.key) != 0);
        }
    }
    /* No source marks any ICO trophy hidden. */
    CHECK(!rt_trophy_info(RT_TROPHY_FAILURE).hidden);
    CHECK(!rt_trophy_info(RT_TROPHY_SPLIT_THE_WATERMELON).hidden);
    CHECK(!rt_trophy_info(RT_TROPHY_RESCUE).hidden);
    CHECK(rt_trophy_info(RT_TROPHY_ENLIGHTENMENT).tier == RT_TROPHY_PLATINUM);
    CHECK(std::strcmp(rt_trophy_tier_name(RT_TROPHY_GOLD), "Gold") == 0);

    /* Eleven entries, from the RetroAchievements set for this disc. If this
     * fails the table changed, at which point docs/ACHIEVEMENTS.md's
     * resolution table is what has to change with it. */
    CHECK(RT_ICO_TROPHY_BIT_COUNT == 11);
    /* The progress array is 0x32 bytes on this build, measured rather than
     * carried, and every bit index in the table has to fall inside it. That
     * is checked entry by entry in test_bit_tables below; this is the length
     * itself. */
    CHECK(RT_ICO_PROGRESS_BYTES == 0x32);
}

/* The shipped table: every entry has to name a trophy the enum has and a
 * bit the array holds, and two entries may share a bit only when a
 * qualifier separates them. A table is data, and a table that cannot be
 * true is worth catching at the desk rather than on a playthrough. */
template <typename Bit>
void check_bit_table(const Bit* bits, int count, int bit_limit, const char* which) {
    std::printf("  %s: %d entr%s\n", which, count, count == 1 ? "y" : "ies");
    for (int i = 0; i < count; ++i) {
        CHECK(bits[i].trophy >= 0 && bits[i].trophy < RT_TROPHY_COUNT);
        CHECK(bits[i].bit >= 0 && bits[i].bit < bit_limit);
        CHECK(bits[i].clear_count == RT_ICO_BIT_CLEAR_ANY ||
              bits[i].clear_count == RT_ICO_BIT_CLEAR_FIRST_RUN ||
              bits[i].clear_count == RT_ICO_BIT_CLEAR_NEW_GAME_PLUS);
        for (int j = 0; j < i; ++j) {
            /* One trophy, one entry. */
            CHECK(bits[j].trophy != bits[i].trophy);
            /* One bit, one trophy, unless the two are told apart by the
             * completed-playthrough count, which is how the game records
             * both secret weapons with the one bit. */
            if (bits[j].bit == bits[i].bit) {
                CHECK(bits[j].clear_count != RT_ICO_BIT_CLEAR_ANY);
                CHECK(bits[i].clear_count != RT_ICO_BIT_CLEAR_ANY);
                CHECK(bits[j].clear_count != bits[i].clear_count);
            }
        }
    }
}

void test_bit_tables() {
    std::printf("\n== shipped bit table ==\n");
    check_bit_table(RT_ICO_TROPHY_BITS, RT_ICO_TROPHY_BIT_COUNT,
        (int)(RT_ICO_PROGRESS_BYTES * 8), "this build");
    /* The two secret weapons share one bit, and the qualifier is the whole
     * of the difference. */
    for (int i = 0; i < RT_ICO_TROPHY_BIT_COUNT; ++i) {
        if (RT_ICO_TROPHY_BITS[i].trophy != RT_TROPHY_SPIKED_CLUB) continue;
        bool found_pair = false;
        for (int j = 0; j < RT_ICO_TROPHY_BIT_COUNT; ++j) {
            if (RT_ICO_TROPHY_BITS[j].trophy != RT_TROPHY_SHINING_SWORD) continue;
            CHECK(RT_ICO_TROPHY_BITS[j].bit == RT_ICO_TROPHY_BITS[i].bit);
            found_pair = true;
        }
        CHECK(found_pair);
    }
}

/* One bit going 0 to 1 unlocks exactly the trophy paired with it, once. */
void test_edge_detection() {
    std::printf("\n== edge detection ==\n");
    fresh(false);
    CHECK(unlocked_count() == 0);

    bit_set(0x20);
    field();
    CHECK(rt_achievements_status(RT_TROPHY_RESCUE).unlocked);
    CHECK(!rt_achievements_status(RT_TROPHY_RESCUE).unlocked_at.empty());
    CHECK(unlocked_count() == 1);

    /* Held high is not another edge. */
    field();
    field();
    CHECK(unlocked_count() == 1);

    /* A bit going back to 0 unlocks nothing and un-unlocks nothing: the game
     * clears bits itself (func_0017B288, src/access.c:247). */
    bit_clear(0x20);
    field();
    CHECK(rt_achievements_status(RT_TROPHY_RESCUE).unlocked);
    CHECK(unlocked_count() == 1);

    /* And setting it again does not double-unlock. */
    bit_set(0x20);
    field();
    CHECK(unlocked_count() == 1);

    /* A bit with no entry in the table is progress and nothing more. */
    bit_set(0x100);
    field();
    CHECK(unlocked_count() == 1);
}

/* A whole-array replacement re-seeds and unlocks nothing. */
void test_baseline_replacement() {
    std::printf("\n== baseline replacement ==\n");
    fresh(false);

    /* A load: every trophy bit at once, well past the threshold. */
    for (int i = 0; i < kTestBitCount; ++i) bit_set(kTestBits[i].bit);
    bit_set(0x100);
    bit_set(0x101);
    field();
    CHECK(unlocked_count() == 0);
    CHECK(log_has("which is a load rather than progress"));

    /* And the new state is now the baseline, so nothing fires later for
     * bits the load brought in. */
    field();
    CHECK(unlocked_count() == 0);

    /* One more bit on top of the loaded array is ordinary progress again. */
    log_clear();
    bit_set(0x102);
    field();
    CHECK(unlocked_count() == 0);
    CHECK(!log_has("which is a load rather than progress"));

    /* A burst at or below the threshold is progress, not a replacement:
     * kBaselineReplaceBits is 8 and the decomp shows real two-bit bursts
     * (src/st13c.c:108-109). Four here, one of them a trophy bit. */
    fresh(false);
    bit_set(0x20);
    bit_set(0x110);
    bit_set(0x111);
    bit_set(0x112);
    field();
    CHECK(rt_achievements_status(RT_TROPHY_RESCUE).unlocked);
}

/* The array going to zero is func_0017B110, that is a new game: counters
 * reset, run count up, unlocks untouched. */
void test_new_game() {
    std::printf("\n== new game ==\n");
    fresh(false);
    bit_set(0x20);
    field();
    CHECK(rt_achievements_status(RT_TROPHY_RESCUE).unlocked);
    for (int i = 0; i < 200; ++i) field(); /* gameplay layout: playtime */
    /* Only where the gameplay layout id is known. Where it is not, playtime
     * is never counted, by design, and there is nothing here to observe. */
    if (kHaveGameplayLayout) CHECK(rt_achievements_counters().playtime_ms > 0);

    log_clear();
    bits_zero();
    set_clear_count(0); /* func_0017B110 zeroes the header too */
    field();
    CHECK(log_has("which is a new game"));
    CHECK(rt_achievements_counters().playtime_ms == 0);
    CHECK(rt_achievements_counters().game_overs == 0);
    /* The zeroed header takes the clear count to 0 with it, so this is a
     * different profile from the one the run was on, on its first run. */
    CHECK(rt_achievements_counters().runs == 1);
    /* The unlock survives: unlocks are permanent across the file. */
    CHECK(rt_achievements_status(RT_TROPHY_RESCUE).unlocked);
}

/* Playtime only accrues on the gameplay layout, and a game-over layout edge
 * is counted once per visit. */
void test_counters() {
    std::printf("\n== counters ==\n");
    fresh(false);
    for (int i = 0; i < 100; ++i) field();
    const uint64_t played = rt_achievements_counters().playtime_ms;
    /* Where the gameplay layout id is not known the clock never runs at all,
     * which is the design and is what the tab is told to say. Both halves of
     * the claim are checked: it runs on the gameplay layout where that id is
     * known, and it stays at zero throughout where it is not. */
    CHECK(kHaveGameplayLayout ? (played > 0) : (played == 0));

    /* A menu layout stops the clock. */
    set_layout(0x10);
    for (int i = 0; i < 100; ++i) field();
    CHECK(rt_achievements_counters().playtime_ms == played);

    /* One game-over screen is one game over, however long it is up. */
    CHECK(rt_achievements_counters().game_overs == 0);
    set_layout(kTestGameOverLayout);
    for (int i = 0; i < 10; ++i) field();
    CHECK(rt_achievements_counters().game_overs == 1);
    set_layout(kPlayLayout);
    field();
    set_layout(kTestGameOverLayout);
    field();
    CHECK(rt_achievements_counters().game_overs == 2);
}

/* A boot before the game has read the memory card must not touch the
 * profile the last playthrough left in the store.
 *
 * The game's card word is negative until it has read the card, and the same
 * first fields of a boot are where gflagInit zeroes the progress array. The
 * observer used to key on the card word but then act on whatever current()
 * returned, which with no profile chosen is the last card-keyed profile in
 * the store: the all-zero edge reset that profile's playtime and game-over
 * counts, bumped its run count and wrote it back.
 *
 * The sequence below is that boot. It fails before the fix (runs goes up by
 * one and game_overs drops to 0) and passes after. */
void test_unkeyed_boot_touches_no_profile() {
    std::printf("\n== a boot with no card slot writes to no profile ==\n");

    /* A real playthrough on card slot 0: one unlock and two game overs. */
    fresh(false);
    set_card(0);
    set_clear_count(1);
    field();
    bit_set(0x20);
    field();
    set_layout(kTestGameOverLayout);
    field();
    set_layout(kPlayLayout);
    field();
    set_layout(kTestGameOverLayout);
    field();
    set_layout(kPlayLayout);
    field();
    const uint32_t runs_before = rt_achievements_counters().runs;
    const uint32_t overs_before = rt_achievements_counters().game_overs;
    CHECK(overs_before == 2);
    CHECK(runs_before >= 1);
    rt_achievements_shutdown();

    /* The next boot. The store is read back, no profile is current yet, and
     * the card word is the -1 the game leaves there until it has read the
     * card. The progress array carries a bit and is then zeroed, which is
     * the new-game edge. */
    rt_achievements_test_reset();
    rt_achievements_test_set_bits(kTestBits, kTestBitCount);
    rt_achievements_test_set_gameover_layout(kTestGameOverLayout);
    rt_achievements_configure(true, true, false, false);
    zero_pages();
    map_pages();
    set_layout(kPlayLayout);
    set_clear_count(1);
    set_card(0xFFFFFFFFu);
    rt_achievements_init(g_dir.c_str());

    bit_set(0x20);
    field();
    bits_zero();
    field();
    for (int i = 0; i < 20; ++i) field();

    /* Nothing was published from a profile, because none is current. */
    CHECK(rt_achievements_counters().runs == 0);
    CHECK(rt_achievements_counters().game_overs == 0);

    /* And nothing reached the store: the earlier profile is byte for byte
     * the one the first half of this case left. */
    rt_achievements_shutdown();
    RtJson root;
    std::string err;
    CHECK(rt_json_parse(read_store(), &root, &err));
    const RtJson* profiles = root.find("profiles");
    CHECK(profiles != nullptr && profiles->type == RtJson::Type::Object);
    CHECK(profiles->obj.size() == 1);
    bool checked = false;
    for (const auto& e : profiles->obj) {
        const RtJson* kc = e.second.find("key_card");
        CHECK(kc != nullptr && kc->type == RtJson::Type::Number);
        CHECK((int)kc->number == 0);
        const RtJson* runs = e.second.find("runs");
        const RtJson* overs = e.second.find("game_overs");
        CHECK(runs != nullptr && overs != nullptr);
        CHECK((uint32_t)runs->number == runs_before);
        CHECK((uint32_t)overs->number == overs_before);
        checked = true;
    }
    CHECK(checked);
}

/* An unmapped guest page is the pre-ELF state: no reads, no crash, and the
 * baseline is dropped so the next mapped field re-seeds rather than
 * treating the whole array as new progress. */
void test_unmapped() {
    std::printf("\n== unmapped guest memory ==\n");
    fresh(false);
    bit_set(0x20);
    field();
    CHECK(unlocked_count() == 1);

    unmap_pages();
    field();
    field();
    map_pages();
    /* Every trophy bit is set while the module was blind. The first mapped
     * field re-seeds and unlocks nothing. */
    for (int i = 0; i < kTestBitCount; ++i) bit_set(kTestBits[i].bit);
    field();
    CHECK(unlocked_count() == 1);
}

/* The store: written on an unlock, read back on the next init. */
void test_store_round_trip() {
    std::printf("\n== store round trip ==\n");
    fresh(false);
    bit_set(0x20);
    field();
    bit_set(0x22);
    field();
    for (int i = 0; i < 500; ++i) field();
    rt_achievements_shutdown();

    const std::string text = read_store();
    CHECK(!text.empty());
    RtJson root;
    std::string err;
    CHECK(rt_json_parse(text, &root, &err));
    const RtJson* version = root.find("version");
    CHECK(version != nullptr && version->type == RtJson::Type::Number);
    CHECK((int)version->number == 1);
    const RtJson* profiles = root.find("profiles");
    CHECK(profiles != nullptr && profiles->type == RtJson::Type::Object);
    CHECK(profiles->obj.size() == 1);
    const RtJson& p = profiles->obj[0].second;
    CHECK(p.find("playtime_ms") != nullptr);
    CHECK(p.find("game_overs") != nullptr);
    CHECK(p.find("runs") != nullptr);
    const RtJson* unlocked = p.find("unlocked");
    CHECK(unlocked != nullptr && unlocked->type == RtJson::Type::Object);
    CHECK(unlocked->find("rescue") != nullptr);
    CHECK(unlocked->find("armed_and_ready") != nullptr);

    const std::string rescue_at = rt_achievements_status(RT_TROPHY_RESCUE).unlocked_at;
    const uint64_t played = rt_achievements_counters().playtime_ms;
    CHECK(kHaveGameplayLayout ? (played > 0) : (played == 0));
    const RtJson* stored_play = p.find("playtime_ms");
    CHECK(stored_play != nullptr && (uint64_t)stored_play->number == played);

    /* Read back: same unlocks, same timestamps, and the playtime carried
     * forward. fresh() runs one gameplay field of its own to seed the
     * baseline, so the reloaded total is at or above what was stored, never
     * back at zero. */
    fresh(true);
    CHECK(rt_achievements_status(RT_TROPHY_RESCUE).unlocked);
    CHECK(rt_achievements_status(RT_TROPHY_ARMED_AND_READY).unlocked);
    CHECK(rt_achievements_status(RT_TROPHY_RESCUE).unlocked_at == rescue_at);
    CHECK(rt_achievements_counters().playtime_ms >= played);
    CHECK(unlocked_count() == 2);
}

/* The profile key is the memory card slot together with the game's own
 * completed-playthrough count, not the count alone. Two first playthroughs
 * are both at clear count 0, so keyed on the count alone they would share
 * one profile and add their counters together. Keyed on both, the card the
 * player is on separates them.
 *
 * What this cannot check, and what is written down at the store section of
 * achievements.cpp rather than pretended away: two playthroughs on the same
 * card slot at the same count still share a profile. Nothing in guest
 * memory carries a per-playthrough identity. */
void test_profile_key_carries_the_card() {
    std::printf("\n== profile key: card slot and clear count ==\n");
    fresh(false);
    set_card(0);
    set_clear_count(0);
    field();
    bit_set(0x20);
    field();
    const uint32_t runs_on_card0 = rt_achievements_counters().runs;
    CHECK(rt_achievements_counters().clear_count == 0);

    /* Same count, other card. A replacement field (the array going to zero)
     * is what makes this a different lineage rather than a re-key in
     * place. */
    log_clear();
    bits_zero();
    set_card(1);
    field();
    CHECK(log_has("profile switched") || log_has("which is a new game"));
    CHECK(rt_achievements_counters().clear_count == 0);
    CHECK(rt_achievements_counters().card == 1);
    CHECK(rt_achievements_counters().game_overs == 0);
    CHECK(rt_achievements_counters().playtime_ms == 0);

    rt_achievements_shutdown();
    RtJson root;
    std::string err;
    CHECK(rt_json_parse(read_store(), &root, &err));
    const RtJson* profiles = root.find("profiles");
    CHECK(profiles != nullptr && profiles->type == RtJson::Type::Object);
    /* Two profiles, both at clear count 0, distinguished only by the card. */
    CHECK(profiles->obj.size() == 2);
    bool saw_card0 = false, saw_card1 = false;
    for (const auto& e : profiles->obj) {
        const RtJson* kc = e.second.find("key_card");
        CHECK(kc != nullptr && kc->type == RtJson::Type::Number);
        if ((int)kc->number == 0) saw_card0 = true;
        if ((int)kc->number == 1) saw_card1 = true;
        CHECK(e.first.find("card") != std::string::npos);
    }
    CHECK(saw_card0 && saw_card1);
    CHECK(runs_on_card0 >= 1);

    /* A store written before the rename keys on "save_counter" and carries
     * no key_card. Both are read back so a player's counters survive. */
    write_store(
        "{\n"
        "  \"version\": 1,\n"
        "  \"profiles\": {\n"
        "    \"pal:7\": {\n"
        "      \"target\": \"" RT_TARGET_NAME "\",\n"
        "      \"save_counter\": 7,\n"
        "      \"playtime_ms\": 1234,\n"
        "      \"runs\": 3,\n"
        "      \"card\": 1,\n"
        "      \"file\": 2,\n"
        "      \"unlocked\": { \"rescue\": \"2026-01-01T00:00:00Z\" }\n"
        "    }\n"
        "  }\n"
        "}\n");
    rt_achievements_test_reset();
    rt_achievements_test_set_bits(kTestBits, kTestBitCount);
    rt_achievements_configure(true, true, false, false);
    zero_pages();
    map_pages();
    set_layout(kPlayLayout);
    set_card(1);
    set_clear_count(7);
    rt_achievements_init(g_dir.c_str());
    field();
    CHECK(rt_achievements_status(RT_TROPHY_RESCUE).unlocked);
    CHECK(rt_achievements_counters().clear_count == 7);
    CHECK(rt_achievements_counters().runs == 3);
    CHECK(rt_achievements_counters().playtime_ms >= 1234);
}

/* A file that will not parse: run on defaults, say so, and do not overwrite
 * it until there is an unlock worth recording. */
void test_unparsable_store() {
    std::printf("\n== unparsable store ==\n");
    const std::string garbage = "{ this is not json, and a player edited it }\n";
    write_store(garbage);
    log_clear();
    fresh(true);
    CHECK(log_has("does not parse"));
    CHECK(unlocked_count() == 0);

    /* Counters alone do not rewrite it. */
    for (int i = 0; i < 500; ++i) field();
    rt_achievements_shutdown();
    CHECK(read_store() == garbage);

    /* The first unlock does. */
    fresh(true);
    bit_set(0x20);
    field();
    CHECK(rt_achievements_status(RT_TROPHY_RESCUE).unlocked);
    const std::string text = read_store();
    CHECK(text != garbage);
    RtJson root;
    std::string err;
    CHECK(rt_json_parse(text, &root, &err));

    /* A version this build does not know blocks the write in the same way,
     * but it is not overwritten when the block lifts: a store a later build
     * wrote holds unlocks this build has no other copy of, so the first
     * unlock renames it aside and writes its own beside it. */
    const std::string newer = "{\n  \"version\": 99,\n  \"profiles\": {}\n}\n";
    write_store(newer);
    std::error_code backup_ec;
    std::filesystem::remove(store_path() + ".v99.bak", backup_ec);
    log_clear();
    fresh(true);
    CHECK(log_has("is version 99"));
    CHECK(log_has(".v99.bak"));
    for (int i = 0; i < 500; ++i) field();
    rt_achievements_shutdown();
    CHECK(read_store().find("\"version\": 99") != std::string::npos);

    /* The first unlock moves it aside rather than destroying it. */
    fresh(true);
    bit_set(0x20);
    field();
    rt_achievements_shutdown();
    const std::string after = read_store();
    CHECK(after.find("\"version\": 99") == std::string::npos);
    CHECK(after.find("rescue") != std::string::npos);
    std::FILE* bak = std::fopen((store_path() + ".v99.bak").c_str(), "rb");
    CHECK(bak != nullptr);
    std::string kept;
    char bbuf[256];
    size_t bn;
    while ((bn = std::fread(bbuf, 1, sizeof(bbuf), bak)) > 0) kept.append(bbuf, bn);
    std::fclose(bak);
    CHECK(kept == newer);
    std::filesystem::remove(store_path() + ".v99.bak", backup_ec);
}

/* Express Journey, Castle Guide, Unscathed Escape and Enlightenment, all
 * four off the Emancipation edge and the counters. */
void test_derived_unlocks() {
    std::printf("\n== derived unlocks ==\n");
    fresh(false);

    /* Everything but Emancipation, one bit per field so no burst crosses
     * the replacement threshold. */
    for (int i = 0; i < kTestBitCount; ++i) {
        if (kTestBits[i].trophy == RT_TROPHY_EMANCIPATION) continue;
        bit_set(kTestBits[i].bit);
        field();
    }
    CHECK(unlocked_count() == kTestBitCount - 1);
    CHECK(!rt_achievements_status(RT_TROPHY_EXPRESS_JOURNEY).unlocked);
    CHECK(!rt_achievements_status(RT_TROPHY_CASTLE_GUIDE).unlocked);
    CHECK(!rt_achievements_status(RT_TROPHY_ENLIGHTENMENT).unlocked);

    bit_set(0x2C);
    field();
    CHECK(rt_achievements_status(RT_TROPHY_EMANCIPATION).unlocked);
    /* Well under two hours of fields, and no game over on this profile. */
    CHECK(rt_achievements_status(RT_TROPHY_EXPRESS_JOURNEY).unlocked);
    CHECK(rt_achievements_status(RT_TROPHY_CASTLE_GUIDE).unlocked);
    CHECK(rt_achievements_status(RT_TROPHY_UNSCATHED_ESCAPE).unlocked);
    CHECK(rt_achievements_status(RT_TROPHY_ENLIGHTENMENT).unlocked);
    CHECK(unlocked_count() == RT_TROPHY_COUNT);

    /* With a game over on the profile, Unscathed Escape does not fire, and
     * Enlightenment cannot either. */
    fresh(false);
    set_layout(kTestGameOverLayout);
    field();
    set_layout(kPlayLayout);
    field();
    CHECK(rt_achievements_counters().game_overs == 1);
    for (int i = 0; i < kTestBitCount; ++i) {
        bit_set(kTestBits[i].bit);
        field();
    }
    CHECK(rt_achievements_status(RT_TROPHY_EMANCIPATION).unlocked);
    CHECK(rt_achievements_status(RT_TROPHY_EXPRESS_JOURNEY).unlocked);
    CHECK(!rt_achievements_status(RT_TROPHY_UNSCATHED_ESCAPE).unlocked);
    CHECK(!rt_achievements_status(RT_TROPHY_ENLIGHTENMENT).unlocked);
}

/* The regression the derived rules exist to avoid: a finished run whose
 * counters ruled all three out, then a new game, then any unlock at all.
 *
 * The store is seeded with a profile that finished the game slowly and with
 * a game over, so Emancipation is unlocked and none of Express Journey,
 * Castle Guide or Unscathed Escape is. The player then starts a new game,
 * which makes a second profile with an empty clock and no game overs, and
 * unlocks one ordinary trophy ten minutes in. Read off the union across
 * profiles, that unlock looks like a finished run on fresh counters and
 * awards three gold trophies for ten minutes of play. The three are gated
 * on the Emancipation edge of the profile in play instead, so none of them
 * fires here. */
void test_derived_not_on_a_later_run() {
    std::printf("\n== derived unlocks do not follow a later run ==\n");

    /* Slow enough to fail both time conditions on either clock: over four
     * hours of counted gameplay milliseconds where that is the clock, and
     * the guest frame counter poked past four hours below where it is. */
    write_store(
        "{\n"
        "  \"version\": 1,\n"
        "  \"profiles\": {\n"
        "    \"1\": {\n"
        "      \"clear_count\": 1,\n"
        "      \"playtime_ms\": 21600000,\n"
        "      \"game_overs\": 1,\n"
        "      \"runs\": 1,\n"
        "      \"card\": 0,\n"
        "      \"file\": 0,\n"
        "      \"unlocked\": { \"emancipation\": \"2026-01-01T00:00:00Z\" }\n"
        "    }\n"
        "  }\n"
        "}\n");
    fresh(true);
    CHECK(rt_achievements_status(RT_TROPHY_EMANCIPATION).unlocked);
    CHECK(!rt_achievements_status(RT_TROPHY_EXPRESS_JOURNEY).unlocked);
    CHECK(!rt_achievements_status(RT_TROPHY_CASTLE_GUIDE).unlocked);
    CHECK(!rt_achievements_status(RT_TROPHY_UNSCATHED_ESCAPE).unlocked);
    const int before = unlocked_count();

    /* Some progress on that profile, so the array is not already all zero
     * when the new game clears it: an all-zero array replacing an all-zero
     * one is not an edge. */
    bit_set(0x100);
    field();

    /* New game (func_0017B110): the array and the save header go to zero,
     * which makes a second profile with its own empty counters. */
    log_clear();
    bits_zero();
    set_clear_count(0);
    field();
    CHECK(log_has("which is a new game"));
    CHECK(rt_achievements_counters().playtime_ms == 0);
    CHECK(rt_achievements_counters().game_overs == 0);

    /* Ten minutes in, one ordinary unlock. Nothing derived may follow it. */
    bit_set(0x20);
    field();
    CHECK(rt_achievements_status(RT_TROPHY_RESCUE).unlocked);
    CHECK(!rt_achievements_status(RT_TROPHY_EXPRESS_JOURNEY).unlocked);
    CHECK(!rt_achievements_status(RT_TROPHY_CASTLE_GUIDE).unlocked);
    CHECK(!rt_achievements_status(RT_TROPHY_UNSCATHED_ESCAPE).unlocked);
    CHECK(!rt_achievements_status(RT_TROPHY_ENLIGHTENMENT).unlocked);
    CHECK(unlocked_count() == before + 1);
}

/* Nothing in this port can unlock a trophy whose bit is not in the table,
 * and the status says so rather than looking merely unearned. */
void test_unresolved_conditions() {
    std::printf("\n== unresolved conditions ==\n");
    rt_achievements_test_reset();
    /* An empty table explicitly, not the shipped one: what is under test is
     * that a trophy with no entry cannot unlock and says so, which is a
     * statement about the module and not about the table. The shipped table
     * is checked as data in test_bit_tables. */
    rt_achievements_test_set_bits(nullptr, 0);
    rt_achievements_test_set_gameover_layout(RT_ICO_LAYOUT_GAMEOVER_UNKNOWN);
    rt_achievements_configure(true, true, false, false);
    zero_pages();
    map_pages();
    set_layout(kPlayLayout);
    remove_store();
    log_clear();
    rt_achievements_init(g_dir.c_str());
    CHECK(log_has("the trophy bit table is empty"));
    CHECK(log_has("the game-over layout id is not known"));
    for (int t = 0; t < RT_TROPHY_COUNT; ++t) {
        CHECK(!rt_achievements_status(t).condition_located);
    }
    /* Every bit in the array set, one per field, unlocks nothing. */
    field();
    for (int i = 0; i < 20; ++i) {
        bit_set(i);
        field();
    }
    CHECK(unlocked_count() == 0);

    /* With the table installed, the located flags follow it. */
    rt_achievements_test_set_bits(kTestBits, kTestBitCount);
    CHECK(rt_achievements_status(RT_TROPHY_RESCUE).condition_located);
    CHECK(rt_achievements_status(RT_TROPHY_EMANCIPATION).condition_located);
    CHECK(rt_achievements_status(RT_TROPHY_EXPRESS_JOURNEY).condition_located);
    /* Still not, while the game-over layout id is the sentinel. */
    CHECK(!rt_achievements_status(RT_TROPHY_UNSCATHED_ESCAPE).condition_located);
    CHECK(!rt_achievements_status(RT_TROPHY_ENLIGHTENMENT).condition_located);
    rt_achievements_test_set_gameover_layout(kTestGameOverLayout);
    CHECK(rt_achievements_status(RT_TROPHY_UNSCATHED_ESCAPE).condition_located);
    CHECK(rt_achievements_status(RT_TROPHY_ENLIGHTENMENT).condition_located);
}

/* One toast at a time, four seconds each on the UI clock, the rest queued. */
void test_toasts() {
    std::printf("\n== toasts ==\n");
    fresh(false);
    int trophy = -1;
    double now = 100.0;
    CHECK(!rt_achievements_poll_toast(now, &trophy));

    bit_set(0x20);
    field();
    bit_set(0x22);
    field();

    CHECK(rt_achievements_poll_toast(now, &trophy));
    CHECK(trophy == RT_TROPHY_RESCUE);
    now += RT_ACHIEVEMENT_TOAST_SECONDS - 0.1;
    CHECK(rt_achievements_poll_toast(now, &trophy));
    CHECK(trophy == RT_TROPHY_RESCUE);
    now += 0.2;
    CHECK(rt_achievements_poll_toast(now, &trophy));
    CHECK(trophy == RT_TROPHY_ARMED_AND_READY);
    now += RT_ACHIEVEMENT_TOAST_SECONDS;
    CHECK(!rt_achievements_poll_toast(now, &trophy));

    /* Turning toasts off drops the queue rather than showing a backlog when
     * they come back on. */
    bit_set(0x23);
    field();
    rt_achievements_configure(true, false, false, false);
    CHECK(!rt_achievements_poll_toast(now, &trophy));
    rt_achievements_configure(true, true, false, false);
    CHECK(!rt_achievements_poll_toast(now, &trophy));
}

/* achievements.enabled off reads nothing at all. */
void test_disabled() {
    std::printf("\n== disabled ==\n");
    fresh(false);
    const uint64_t played = rt_achievements_counters().playtime_ms;
    rt_achievements_configure(false, true, false, false);
    bit_set(0x20);
    for (int i = 0; i < 50; ++i) field();
    CHECK(unlocked_count() == 0);
    CHECK(rt_achievements_counters().playtime_ms == played);

    rt_achievements_configure(true, true, false, false);
    field(); /* re-seeds nothing: the baseline is still the pre-bit array */
    CHECK(rt_achievements_status(RT_TROPHY_RESCUE).unlocked);
}

/* A bit that needs the completed-playthrough byte on a build that does not
 * know where that byte is. The pair cannot be told apart, so neither of the
 * two trophies is located and neither fires; the unqualified entry beside
 * them is unaffected. On a build that does know the address this case
 * checks the other half, that the count decides which of the two unlocks. */
void test_clear_count_qualifier() {
    std::printf("\n== clear-count qualifier ==\n");
    rt_achievements_test_reset();
    rt_achievements_test_set_bits(kTestBitsQualified, kTestBitsQualifiedCount);
    rt_achievements_configure(true, true, false, false);
    zero_pages();
    map_pages();
    set_layout(kPlayLayout);
    set_clear_count(1);
    remove_store();
    rt_achievements_init(g_dir.c_str());
    field();

    CHECK(rt_achievements_status(RT_TROPHY_RESCUE).condition_located);
    const bool have_count = RT_ICO_CLEAR_COUNT != RT_ICO_SYM_UNKNOWN;
    CHECK(rt_achievements_status(RT_TROPHY_SPIKED_CLUB).condition_located == have_count);
    CHECK(rt_achievements_status(RT_TROPHY_SHINING_SWORD).condition_located == have_count);

    log_clear();
    bit_set(0x30);
    field();
    if (!have_count) {
        CHECK(!rt_achievements_status(RT_TROPHY_SPIKED_CLUB).unlocked);
        CHECK(!rt_achievements_status(RT_TROPHY_SHINING_SWORD).unlocked);
        CHECK(log_has("stands for two trophies"));
    } else {
        /* The byte is zero on this fresh page set, which is a first run. */
        CHECK(rt_achievements_status(RT_TROPHY_SPIKED_CLUB).unlocked);
        CHECK(!rt_achievements_status(RT_TROPHY_SHINING_SWORD).unlocked);
    }

    /* The unqualified entry beside them is unaffected either way. */
    bit_set(0x20);
    field();
    CHECK(rt_achievements_status(RT_TROPHY_RESCUE).unlocked);
}

/* The diagnostic: one line per transition and per layout change. */
void test_diagnostic_log() {
    std::printf("\n== diagnostic log ==\n");
    fresh(false);
    rt_achievements_configure(true, true, false, true);
    log_clear();
    bit_set(0x100);
    /* A second bit, left set for the rest of the case: clearing the only
     * set bit takes the whole array to zero, which is the new-game rule
     * (func_0017B110) and not a cleared-bit transition. */
    bit_set(0x101);
    field();
    CHECK(log_has("progress bit 0x100 set"));
    bit_clear(0x100);
    field();
    CHECK(log_has("progress bit 0x100 cleared"));
    log_clear();
    set_layout(0x11);
    field();
    char want[64];
    std::snprintf(want, sizeof(want), "layout 0x%x -> 0x11", (unsigned)kPlayLayout);
    CHECK(log_has(want));
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1) {
        g_dir = argv[1];
    } else if (const char* env = std::getenv("ICORECOMP_ACHIEVEMENTS_SELFTEST_DIR")) {
        g_dir = env;
    } else {
        g_dir = "achievements-selftest";
    }
    std::error_code ec;
    std::filesystem::create_directories(g_dir, ec);
    g_base_dir = g_dir;
    std::printf("achievements selftest, scratch directory %s\n", g_dir.c_str());

    test_table();
    test_bit_tables();
    test_edge_detection();
    test_baseline_replacement();
    test_new_game();
    test_counters();
    test_unkeyed_boot_touches_no_profile();
    test_unmapped();
    test_store_round_trip();
    test_profile_key_carries_the_card();
    test_unparsable_store();
    test_derived_unlocks();
    test_derived_not_on_a_later_run();
    test_unresolved_conditions();
    test_toasts();
    test_disabled();
    test_clear_count_qualifier();
    test_diagnostic_log();

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 2;
}
