/* guest/menu_nav_selftest.cpp: standalone exercise of the menu pointer
 * (guest/menu_nav.cpp): the screen mapping derived from the game's own
 * scene objects, the "is a menu up" predicate, the selection writes and the
 * pulses.
 *
 * Links menu_nav.cpp against the two runtime externs it calls (rt_log and
 * the g_pages array rt_gptr indexes), stubbed below. It is built with
 * neither ICORECOMP_HAVE_SDL nor ICORECOMP_HAVE_PARALLEL_GS, so there is no
 * window and no GS library to ask for a cursor; ICORECOMP_MENU_NAV_TEST
 * compiles in the injection hook that stands in for them.
 *
 * The guest side is not stubbed: the harness maps real 64 KB pages into
 * g_pages and writes the state words, the layout table entries and the
 * scene objects the module reads, so every test drives the same path the
 * runtime does. That is also how a write is asserted: the module writes the
 * guest word and the test reads it back out of the same page. The game's
 * own frame and its response to a press are modelled here (game_frame and
 * sim_press below) by doing what lt_next_layout and lt_switch_layout do to
 * the same words.
 *
 * One field is run in the order sif/pad.cpp and host/input.cpp run it, which
 * is what puts a click's cross in the click's own field:
 *
 *     the game's frame  ->  rt_guest_menu_tick(field)
 *                       ->  the field's mouse events
 *                       ->  rt_guest_menu_pulse_bits(field)
 *                       ->  the game applies those bits
 *
 * so a test asks for a click with press_button() or wheel_ticks(), which
 * queue the event for the next run_field() to deliver after the tick, rather
 * than calling the module's entry points between fields where no field of
 * the real runtime ever calls them.
 *
 * The two gates on menu_nav.cpp's pointer half are visible here: the SDL
 * provider gate is rt_input_sdl_active(), stubbed below so a test can shut
 * it, and rt_ui_wants_input() is ui/ui.h's inline stub returning false,
 * because this executable is built without ICORECOMP_UI and has no
 * overlay.
 *
 *     ./icorecomp-menu-nav-selftest
 *
 * Exit code 0 = every check passed; 2 on the first failing CHECK.
 */
#include "guest/ico_syms.h"
/* One executable, icorecomp-menu-nav-selftest (CMakeLists.txt), built
 * against the one set of constants in guest/ico_syms.h. Every address,
 * structure offset and flag bit below comes from those constants, so the
 * scene object's 0x70 stride, its fields from +0x24 up and its hidden bit at
 * 0x10 are all under test rather than described. */
#include "guest/menu_nav.h"
#include "host/input.h"
#include "recomp_api.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

/* ---- runtime stubs -------------------------------------------------------- */

/* rt_gptr (ee/kernel.h) indexes this; mem.cpp owns it in the real runtime.
 * The harness maps the pages the tables fall in and leaves the rest null,
 * which is exactly the "before the ELF is loaded" state the reader has to
 * cope with. The recomp_api.h include above is required, not decorative: it
 * declares g_pages inside extern "C", and without it this definition gets
 * plain C++ linkage, which MSVC mangles and GCC does not, leaving
 * add_layout_items unable to find the symbol under MSVC. */
uint8_t* g_pages[0x10000];

namespace {
/* Every line rt_log has been handed since log_clear(). Half of what this
 * module promises is a log line naming what it refused to do, and a test
 * that only checked the kept state would pass with every line deleted. */
std::string g_log;
} // namespace

namespace {
/* host/input.h's provider gate. menu_nav.cpp writes nothing and hovers
 * nothing unless the SDL provider is the live one, so a scripted run stays
 * bit-identical; the harness stands in for it so that gate can be shut. */
bool g_sdl_active = true;
} // namespace

bool rt_input_sdl_active() { return g_sdl_active; }

/* The runtime's four level entry points, all onto one line here: a
 * selftest has one reader and no level to filter by. */
void rt_log_line(const char* component, const char* fmt, va_list ap);

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

void rt_log_line(const char* component, const char* fmt, va_list ap) {
    char buf[1024];
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    std::printf("[%s] %s\n", component, buf);
    g_log += buf;
    g_log += '\n';
}

/* ---- test harness ---------------------------------------------------------- */

namespace {

int g_failures = 0;
const char* g_case = "";

void fail(const char* expr, const char* file, int line) {
    std::printf("FAIL %s: %s (%s:%d)\n", g_case, expr, file, line);
    ++g_failures;
    std::exit(2);
}

#define CHECK(expr) do { if (!(expr)) fail(#expr, __FILE__, __LINE__); } while (0)

void log_clear() { g_log.clear(); }
bool log_has(const char* needle) { return g_log.find(needle) != std::string::npos; }

/* How many log lines since log_clear() carry `needle`: "once and not again"
 * is half of what a hover promises, and log_has() cannot say it. */
int log_count(const char* needle) {
    int n = 0;
    for (size_t i = g_log.find(needle); i != std::string::npos;
         i = g_log.find(needle, i + 1)) {
        ++n;
    }
    return n;
}

void begin(const char* name) {
    g_case = name;
    std::printf("---- %s\n", name);
    log_clear();
}

/* ---- guest memory ---------------------------------------------------------- */

/* The pages the state words, the layout table and the scene objects fall
 * in, allocated once and mapped in and out so the "before the ELF is
 * loaded" case can be driven without losing what was written into them. */
std::vector<std::pair<uint32_t, uint8_t*> > g_owned;

void own_page(uint32_t addr) {
    const uint32_t page = addr >> 16;
    for (const auto& e : g_owned) {
        if (e.first == page) return;
    }
    uint8_t* mem = (uint8_t*)std::calloc(0x10000, 1);
    CHECK(mem != nullptr);
    g_owned.push_back(std::make_pair(page, mem));
}

void map_pages() {
    if (g_owned.empty()) {
        own_page(RT_ICO_LAYOUT_ID);
        own_page(RT_ICO_ITEM_ID);
        own_page(RT_ICO_FADE_STATE);
        own_page(RT_ICO_BRIGHTNESS);
        /* Both ends of both tables: neither fits in one page. */
        own_page(RT_ICO_LAYOUT_TABLE);
        own_page(RT_ICO_LAYOUT_TABLE + RT_ICO_LAYOUT_COUNT * RT_ICO_LAYOUT_STRIDE);
        own_page(RT_ICO_KANBAN_ACTIVE);
        own_page(RT_ICO_KANBAN_NODES);
        own_page(RT_ICO_KANBAN_NODES +
                 RT_ICO_KANBAN_NODE_COUNT * RT_ICO_KANBAN_NODE_STRIDE - 1);
        for (uint32_t i = 0; i <= 0x400; ++i) {
            own_page(RT_ICO_SCENE_OBJECTS + i * RT_ICO_SCENE_STRIDE);
        }
    }
    for (const auto& e : g_owned) g_pages[e.first] = e.second;
}

void unmap_pages() {
    for (const auto& e : g_owned) g_pages[e.first] = nullptr;
}

void poke(uint32_t addr, int32_t value) {
    uint8_t* p = g_pages[addr >> 16] + (addr & 0xFFFFu);
    std::memcpy(p, &value, sizeof(value));
}

int32_t peek(uint32_t addr) {
    int32_t v = 0;
    std::memcpy(&v, g_pages[addr >> 16] + (addr & 0xFFFFu), sizeof(v));
    return v;
}

/* ---- the two guest tables --------------------------------------------------
 *
 * One 0x38-byte layout entry per screen id and one 0x6C-byte scene object
 * per item, laid out exactly as guest/ico_syms.h describes them.
 */
uint32_t entry_of(uint32_t layout) {
    return RT_ICO_LAYOUT_TABLE + layout * RT_ICO_LAYOUT_STRIDE;
}

uint32_t object_of(int32_t index) {
    return RT_ICO_SCENE_OBJECTS + (uint32_t)index * RT_ICO_SCENE_STRIDE;
}

/* `parent` is the +0x30 link lt_next_layout walks; -1 ends the chain. The
 * pages are zeroed once and reused, so it is always written: a zero left
 * there would make layout 0 everything's parent. */
void set_entry(uint32_t layout, int32_t first, int32_t end, int32_t dflt, int32_t cur,
               int32_t parent = -1) {
    poke(entry_of(layout) + RT_ICO_LAYOUT_FIRST_OBJ, first);
    poke(entry_of(layout) + RT_ICO_LAYOUT_END_OBJ, end);
    poke(entry_of(layout) + RT_ICO_LAYOUT_DEFAULT_ITEM, dflt);
    poke(entry_of(layout) + RT_ICO_LAYOUT_CUR_ITEM, cur);
    poke(entry_of(layout) + RT_ICO_LAYOUT_PARENT, parent);
}

int32_t entry_item(uint32_t layout) {
    return peek(entry_of(layout) + RT_ICO_LAYOUT_CUR_ITEM);
}

/* A scene object as lt_link_layout reads one. The placement fields are in
 * the game's own 640 by 224 layout space, origin at the centre of the
 * picture, with the height fields counting half units (menu_nav.cpp). */
struct Obj {
    int32_t right = -1, left = -1, down = -1, up = -1;
    int32_t centred = 0;
    int32_t h = 0, w = 0, y = 0, x = 0;
    int32_t tex_h = 0, tex_w = 0;
    int32_t flags = 0;
};

void put_object(int32_t index, const Obj& o) {
    const uint32_t b = object_of(index);
    for (uint32_t k = 0; k < RT_ICO_SCENE_STRIDE; k += 4) poke(b + k, 0);
    poke(b + RT_ICO_OBJ_TRIANGLE_TARGET, -1);
    poke(b + RT_ICO_OBJ_CROSS_TARGET, -1);
    poke(b + RT_ICO_OBJ_RIGHT, o.right);
    poke(b + RT_ICO_OBJ_LEFT, o.left);
    poke(b + RT_ICO_OBJ_DOWN, o.down);
    poke(b + RT_ICO_OBJ_UP, o.up);
    poke(b + RT_ICO_OBJ_CENTRED, o.centred);
    poke(b + RT_ICO_OBJ_H, o.h);
    poke(b + RT_ICO_OBJ_W, o.w);
    poke(b + RT_ICO_OBJ_Y, o.y);
    poke(b + RT_ICO_OBJ_X, o.x);
    poke(b + RT_ICO_OBJ_TEX_H, o.tex_h);
    poke(b + RT_ICO_OBJ_TEX_W, o.tex_w);
    poke(b + RT_ICO_OBJ_FLAGS, o.flags);
}

/* ---- the game's side of a press -------------------------------------------- */

uint32_t g_layout = 0x36;
uint32_t g_fade = 2;
bool g_adjust_screen = false;
uint64_t g_field = 0;
/* The layouts lt_next_layout would run lt_switch_layout for: the current
 * one and its ancestors. The sim applies a press to each, as the game
 * does. */
std::vector<uint32_t> g_sim_chain;

/* What lt_switch_layout does with one pressed-this-frame D-pad bit: follow
 * the current object's link for that direction and store it, when it is not
 * negative. The screen adjustment screen's own handler instead steps its
 * level on LEFT and RIGHT, clamped to 0 and 14. The selection lives in
 * guest memory, where the pointer's own writes land, so the two cannot
 * drift apart here in a way they could not in the runtime. */
void sim_press(uint16_t bits) {
    if (g_adjust_screen) {
        const int32_t v = peek(RT_ICO_BRIGHTNESS);
        if ((bits & RT_PAD_LEFT) && v > 0) poke(RT_ICO_BRIGHTNESS, v - 1);
        else if ((bits & RT_PAD_RIGHT) && v < (int32_t)RT_ICO_ADJUST_COUNT - 1) {
            poke(RT_ICO_BRIGHTNESS, v + 1);
        }
        return;
    }
    for (uint32_t layout : g_sim_chain) {
        const int32_t cur = entry_item(layout);
        if (cur < 0) continue;
        const uint32_t b = object_of(cur);
        int32_t next = -1;
        if (bits & RT_PAD_UP) next = peek(b + RT_ICO_OBJ_UP);
        else if (bits & RT_PAD_DOWN) next = peek(b + RT_ICO_OBJ_DOWN);
        else if (bits & RT_PAD_LEFT) next = peek(b + RT_ICO_OBJ_LEFT);
        else if (bits & RT_PAD_RIGHT) next = peek(b + RT_ICO_OBJ_RIGHT);
        if (next >= 0) poke(entry_of(layout) + RT_ICO_LAYOUT_CUR_ITEM, next);
    }
}

/* ---- the game's frame -------------------------------------------------------
 *
 * What lt_next_layout does to the words this module reads, in the order it
 * does it. Only the parts a test can tell apart are here.
 */

/* Objects the chain's action functions hide for the frame, standing in for
 * func_001B7218(index, 1): it sets +0x68 bit 1 and leaves every other bit
 * alone, after phase 1 has already seeded bit 1 from bit 0. */
std::vector<int32_t> g_hidden;

/* The load grid's empty save slots, for the custom handler below. */
std::vector<int32_t> g_empty_slots;
/* A custom item-select handler in D_00633164, as 0x10's action function
 * (func_001B21F0) installs exec_layout_texture. Within a frame it is used
 * once, replacing the layout's +0x2C with handler(+0x2C) in place of the
 * neighbour walk, and cleared; the action function puts it back on the next
 * frame, which is why this flag stays set once a test sets it. */
bool g_custom_handler = false;

/* The game's one-frame "swallow this frame's navigation" flag D_00633160.
 * lt_next_layout clears it on the way out, so it is only ever non-zero part
 * way through a frame; the pad tick can land there, because sched.cpp runs
 * due events from the guest's own backedges. Held here for as many fields as
 * a test wants the pointer to see it. */
bool g_swallow = false;

bool contains_i32(const std::vector<int32_t>& v, int32_t x) {
    for (int32_t e : v) {
        if (e == x) return true;
    }
    return false;
}

/* exec_layout_texture's stand-in: with no direction bit pressed it returns
 * the item it was given, except that it moves on from an empty save slot to
 * the next occupied one. That is the game rule the pointer's one-write-per-
 * hover exists for. */
int32_t custom_handler(uint32_t layout, int32_t item) {
    const int32_t first = peek(entry_of(layout) + RT_ICO_LAYOUT_FIRST_OBJ);
    const int32_t end = peek(entry_of(layout) + RT_ICO_LAYOUT_END_OBJ);
    for (int32_t i = item; i < end; ++i) {
        if (!contains_i32(g_empty_slots, i)) return i;
    }
    for (int32_t i = first; i < item; ++i) {
        if (!contains_i32(g_empty_slots, i)) return i;
    }
    return item;
}

void game_frame() {
    poke(RT_ICO_LAYOUT_ID, (int32_t)g_layout);
    poke(RT_ICO_FADE_STATE, (int32_t)g_fade);

    /* Phase 1: for every object of every layout in the chain, copy the
     * persistent flag GetRealModelId writes (RT_ICO_OBJ_FLAG_PERSISTENT)
     * into the per-frame "lt_link_layout draws nothing" one
     * (RT_ICO_OBJ_FLAG_HIDDEN). Those are bits 4 and 5 on this build and not
     * the 0 and 1 the decomp's structure has, which is why the constants are
     * used rather than literals. */
    for (uint32_t layout : g_sim_chain) {
        const int32_t first = peek(entry_of(layout) + RT_ICO_LAYOUT_FIRST_OBJ);
        const int32_t end = peek(entry_of(layout) + RT_ICO_LAYOUT_END_OBJ);
        for (int32_t i = first; i < end; ++i) {
            const uint32_t b = object_of(i) + RT_ICO_OBJ_FLAGS;
            int32_t f = peek(b);
            f &= ~(int32_t)RT_ICO_OBJ_FLAG_HIDDEN;
            if (f & (int32_t)RT_ICO_OBJ_FLAG_PERSISTENT) {
                f |= (int32_t)RT_ICO_OBJ_FLAG_HIDDEN;
            }
            /* Then the action function's own func_001B7218 calls. */
            if (contains_i32(g_hidden, i)) f |= (int32_t)RT_ICO_OBJ_FLAG_HIDDEN;
            poke(b, f);
        }
    }

    /* Phases 2 and 3: lt_switch_layout for each layout in the chain, all of
     * them skipped while the swallow flag is up. With a custom handler
     * installed the neighbour walk is replaced by handler(+0x2C); the D-pad
     * side of it is sim_press, which runs after the bits are known. */
    poke(RT_ICO_NAV_SWALLOW, g_swallow ? 1 : 0);
    if (!g_swallow && g_custom_handler) {
        for (uint32_t layout : g_sim_chain) {
            const int32_t cur = entry_item(layout);
            if (cur < 0) continue;
            poke(entry_of(layout) + RT_ICO_LAYOUT_CUR_ITEM, custom_handler(layout, cur));
        }
    }

    /* The item mirror, loaded from each chain layout's +0x2C in turn with the
     * current layout last, which is why it holds that one's field and lags a
     * write by a frame. */
    poke(RT_ICO_ITEM_ID, entry_item(g_layout));
}

/* ---- one field --------------------------------------------------------------
 *
 * The order sif/pad.cpp and host/input.cpp run: the game's frame, then the
 * tick, then the field's mouse events, then the bits, then the game applying
 * them. A click's cross is in the bits of the field the click arrived on
 * because the module starts the pulse inside rt_guest_menu_on_button.
 */
struct PendingEvent {
    enum Kind { Cursor, Button, Wheel } kind = Button;
    uint8_t button = 0;
    bool down = false;
    int wheel = 0;
    bool cursor_valid = false;
    float x = 0.0f, y = 0.0f;
};
std::vector<PendingEvent> g_pending;

void press_button(uint8_t button, bool down) {
    PendingEvent e;
    e.kind = PendingEvent::Button;
    e.button = button;
    e.down = down;
    g_pending.push_back(e);
}

void wheel_ticks(int ticks) {
    PendingEvent e;
    e.kind = PendingEvent::Wheel;
    e.wheel = ticks;
    g_pending.push_back(e);
}

/* The cursor moving inside the field, after the tick has already run, which
 * is where host/input.cpp routes the field's motion. It is what makes a click
 * arrive on an item the tick did not see yet. */
void move_cursor_in_field(bool valid, float x, float y) {
    PendingEvent e;
    e.kind = PendingEvent::Cursor;
    e.cursor_valid = valid;
    e.x = x;
    e.y = y;
    g_pending.push_back(e);
}

void deliver_pending() {
    const std::vector<PendingEvent> events = g_pending;
    g_pending.clear();
    for (const PendingEvent& e : events) {
        switch (e.kind) {
            case PendingEvent::Cursor:
                rt_guest_menu_test_set_cursor(e.cursor_valid, e.x, e.y);
                break;
            case PendingEvent::Button:
                rt_guest_menu_on_button(e.button, e.down);
                break;
            case PendingEvent::Wheel:
                rt_guest_menu_on_wheel(e.wheel);
                break;
        }
    }
}

uint16_t run_field() {
    ++g_field;
    game_frame();
    rt_guest_menu_tick(g_field);
    deliver_pending();
    const uint16_t bits = rt_guest_menu_pulse_bits(g_field);
    if (bits) sim_press(bits);
    return bits;
}

/* A pad field with no game frame in front of it. sif/pad.cpp's catch-up loop
 * runs several of these in a row after a long frame, so the item mirror and
 * everything else the game refreshes stays as the last frame left it while
 * the module ticks. */
uint16_t run_tick_only() {
    ++g_field;
    rt_guest_menu_tick(g_field);
    deliver_pending();
    return rt_guest_menu_pulse_bits(g_field);
}

std::vector<uint16_t> run_fields(int count) {
    std::vector<uint16_t> out;
    for (int i = 0; i < count; ++i) out.push_back(run_field());
    return out;
}

int count_presses(const std::vector<uint16_t>& seq) {
    int n = 0;
    for (uint16_t b : seq) {
        if (b) ++n;
    }
    return n;
}

/* Every press must be followed by a field of no bits: that release is what
 * makes the game's pressed-this-frame word see a second press at all. */
bool cadence_ok(const std::vector<uint16_t>& seq) {
    for (size_t i = 0; i < seq.size(); ++i) {
        if (seq[i] != 0 && i + 1 < seq.size() && seq[i + 1] != 0) return false;
    }
    return true;
}

/* ---- screens ---------------------------------------------------------------
 *
 * Every test screen is written into guest memory as the game's own tables,
 * so the module derives its items the way it does in a real run.
 */

/* The title screen's continue or new game choice: layout 0x0C, two centred
 * items in a column, scene objects 49 and 50. Those are the retail ids (the
 * listing's la_title_continue_or_new), which is what makes this the
 * calibration case menu_nav.cpp's header quotes.
 *
 * The four placement numbers are the retail objects' own: a 20 tall sprite
 * (the height field counts half units, so 40) centred horizontally, at
 * layout-space y 135 and 165. The one number that is not theirs is the
 * second item's texture width, 128 here against 172 in the ELF: this case
 * holds the module's arithmetic against the user's measured boxes, and the
 * shipped 172 does not reproduce the second box. menu_nav.cpp's header
 * records that disagreement and what would settle it; this test is about
 * the arithmetic, not about the object. */
constexpr uint32_t kTitleLayout = 0x0C;
constexpr int32_t kTitleItemA = 49;
constexpr int32_t kTitleItemB = 50;

void screen_title() {
    Obj a;
    a.down = kTitleItemB;
    a.centred = 1;
    a.h = 40;
    a.tex_w = 128;   /* w is zero, so the texture's width stands in */
    a.tex_h = 20;
    a.y = 135;
    put_object(kTitleItemA, a);

    Obj b = a;
    b.down = -1;
    b.up = kTitleItemA;
    b.y = 165;
    put_object(kTitleItemB, b);

    /* The entry's range is exactly the two items, and its current item is
     * the default, which is the state the game leaves an interactive screen
     * in: a layout whose +0x2C is negative is one the game will not
     * navigate, and the pointer offers nothing on it. */
    set_entry(kTitleLayout, kTitleItemA, kTitleItemB + 1, kTitleItemA, kTitleItemA);
}

/* A row: two items side by side, linked left and right, so the wheel has to
 * press LEFT and RIGHT rather than UP and DOWN. */
constexpr uint32_t kRowLayout = 0x0E;
constexpr int32_t kRowItemA = 100;
constexpr int32_t kRowItemB = 101;

void screen_row() {
    Obj a;
    a.right = kRowItemB;
    a.w = 128;
    a.h = 40;
    a.x = 120;
    a.y = 135;
    put_object(kRowItemA, a);

    Obj b = a;
    b.right = -1;
    b.left = kRowItemA;
    b.x = 392;
    put_object(kRowItemB, b);

    set_entry(kRowLayout, kRowItemA, kRowItemB + 1, kRowItemA, kRowItemA);
}

/* A page built out of a chain, the shape the retail load and save file
 * select pages have: the current layout (0x10 there) draws its own
 * decoration and holds no selection, and the items belong to its parent
 * (0xb there, the ten-slot grid). lt_next_layout runs and draws both.
 *
 * Here the parent kGridLayout owns three items in a row and the child
 * kPageLayout owns two unreachable decoration objects. */
constexpr uint32_t kPageLayout = 0x10;
constexpr uint32_t kGridLayout = 0x0B;
constexpr int32_t kGridItemA = 110;
constexpr int32_t kGridItemB = 111;
constexpr int32_t kGridItemC = 112;
constexpr int32_t kPageDecorA = 153;
constexpr int32_t kPageDecorB = 154;

void screen_page() {
    for (int32_t i = 0; i < 3; ++i) {
        Obj g;
        g.left = i > 0 ? kGridItemA + i - 1 : -1;
        g.right = i < 2 ? kGridItemA + i + 1 : -1;
        g.w = 96;
        g.h = 40;
        g.x = 160 + 128 * i;
        g.y = 150;
        put_object(kGridItemA + i, g);
    }
    set_entry(kGridLayout, kGridItemA, kGridItemC + 1, kGridItemA, kGridItemA);

    /* Drawn, but with no links and no seed on its own layout, so it is
     * decoration and never offered. */
    for (int32_t i = 0; i < 2; ++i) {
        Obj d;
        d.w = 200;
        d.h = 30;
        d.x = 220;
        d.y = 60 + 40 * i;
        put_object(kPageDecorA + i, d);
    }
    set_entry(kPageLayout, kPageDecorA, kPageDecorB + 1, -1, -1, (int32_t)kGridLayout);
}

/* The screen adjustment screen: layout 0x3C owns a long range that covers
 * the fifteen bar positions starting at RT_ICO_ADJUST_FIRST_OBJ, and its
 * own default item is a 1 by 1 placeholder at the layout space origin,
 * which lands off the picture and is not an item. 0x3C is the id the retail
 * layout table gives la_adjust_screen; the sim builds its own table, so the
 * id matters only in that it is the one the runtime would see.
 *
 * The range and the placeholder are stated relative to that first object,
 * not as literals: RT_ICO_ADJUST_FIRST_OBJ is 0x18B on this build, and a range
 * written as a literal would stop covering the fifteen positions the moment
 * that constant moved. */
constexpr uint32_t kAdjustLayout = 0x3C;
constexpr int32_t kAdjustFirst = (int32_t)RT_ICO_ADJUST_FIRST_OBJ - 3;
constexpr int32_t kAdjustEnd = (int32_t)RT_ICO_ADJUST_FIRST_OBJ + 0x1A;
constexpr int32_t kAdjustPlaceholder = (int32_t)RT_ICO_ADJUST_FIRST_OBJ + 0x19;

void screen_adjust() {
    for (uint32_t i = 0; i < RT_ICO_ADJUST_COUNT; ++i) {
        Obj c;
        c.w = 32;
        c.h = 30;
        c.x = 80 + 32 * (int32_t)i;
        c.y = 90;
        c.tex_w = 32;
        c.tex_h = 15;
        put_object((int32_t)(RT_ICO_ADJUST_FIRST_OBJ + i), c);
    }
    Obj p;
    p.h = 1;
    p.w = 1;
    put_object(kAdjustPlaceholder, p);
    set_entry(kAdjustLayout, kAdjustFirst, kAdjustEnd, kAdjustPlaceholder, kAdjustPlaceholder);
}

/* One of the kanban system's screens: the 50/60 Hz choice, layout entry 1,
 * two items side by side that the game's own boot state machine reads the
 * video mode out of. Nothing about it is in the layout state words: the node
 * at RT_ICO_KANBAN_ACTIVE is what says it is up, its +0x00 is the layout
 * table entry, and its +0x0C bit 0 would say it is fading out.
 *
 * The two placement numbers are the ones the retail ELF ships for objects
 * 0x21 and 0x22, so the rectangle checks below are against the screen the
 * game actually draws. */
constexpr uint32_t kKanbanLayout = RT_ICO_KANBAN_LAYOUT_VIDEO_MODE;
constexpr int32_t kKanbanFirst = 0x20;
constexpr int32_t kKanbanEnd = 0x26;
constexpr int32_t kKanbanItemA = 0x21;
constexpr int32_t kKanbanItemB = 0x22;
/* Any of the thirty slots; the fourth, so that a node address of zero or of
 * the array's base cannot pass by accident. */
constexpr uint32_t kKanbanNode = RT_ICO_KANBAN_NODES + 3 * RT_ICO_KANBAN_NODE_STRIDE;

void screen_kanban() {
    Obj a;
    a.right = kKanbanItemB;
    a.h = 40;          /* the height field counts half units, so 20 drawn */
    a.x = 190;
    a.y = 65;
    a.tex_w = 80;      /* w is zero, so the texture's width stands in */
    a.tex_h = 20;
    put_object(kKanbanItemA, a);

    Obj b = a;
    b.right = -1;
    b.left = kKanbanItemA;
    b.x = 370;
    put_object(kKanbanItemB, b);

    set_entry(kKanbanLayout, kKanbanFirst, kKanbanEnd, kKanbanItemA, kKanbanItemA);
}

/* The gameplay loop and one of the cinematics: an empty scene object range
 * and no item at all, which is how the retail layout table ships every
 * entry whose action function the disc listing names for one of those
 * states. 0x36 is la_game_loop and 0x37 is la_game_demo; which id the
 * layout id word actually reports while the player is playing is still not
 * measured (guest/ico_syms.h, RT_ICO_LAYOUT_GAMEPLAY), which is exactly why
 * this module excludes them structurally and not by id. */
constexpr uint32_t kGameplayLayout = 0x36;
constexpr uint32_t kCinematicLayout = 0x37;

void screen_empty(uint32_t layout) {
    set_entry(layout, 261, 261, -1, -1);
}

/* Puts the module into a known state with one screen up, its selection at
 * `start` and no cursor. */
void arm(uint32_t layout, int32_t start) {
    rt_guest_menu_test_reset();
    map_pages();
    g_layout = layout;
    g_fade = 2;
    g_adjust_screen = (layout == kAdjustLayout);
    g_field = 0;
    g_pending.clear();
    g_hidden.clear();
    g_empty_slots.clear();
    g_custom_handler = false;
    g_swallow = false;
    g_sdl_active = true;
    poke(RT_ICO_NAV_SWALLOW, 0);

    poke(RT_ICO_BRIGHTNESS, 0);
    /* No kanban screen unless a test asks for one: the pages are reused
     * between tests, so this word has to be put back rather than assumed. */
    poke(RT_ICO_KANBAN_ACTIVE, 0);
    poke(kKanbanNode + RT_ICO_KANBAN_NODE_FLAGS, 0);
    screen_title();
    screen_row();
    screen_page();
    screen_kanban();
    screen_adjust();
    screen_empty(kGameplayLayout);
    screen_empty(kCinematicLayout);

    g_sim_chain.clear();
    g_sim_chain.push_back(layout);
    if (layout == kPageLayout) g_sim_chain.push_back(kGridLayout);

    if (g_adjust_screen) poke(RT_ICO_BRIGHTNESS, start);
    else if (start >= 0) poke(entry_of(layout) + RT_ICO_LAYOUT_CUR_ITEM, start);

    poke(RT_ICO_LAYOUT_ID, (int32_t)layout);
    poke(RT_ICO_FADE_STATE, 2);
    poke(RT_ICO_ITEM_ID, entry_item(layout));
    rt_guest_menu_test_set_cursor(false, 0.0f, 0.0f);
}

/* The same, with one of the kanban system's screens up instead. The layout
 * state words are left saying gameplay, which has no items and holds fade 2,
 * so anything the module then finds came from the node and not from them.
 * `layout_word` puts the layout id word on the kanban screen's own entry id
 * instead, which is the case the item mirror has to be kept out of. */
void arm_kanban(int32_t start, bool layout_word = false) {
    arm(layout_word ? kKanbanLayout : kGameplayLayout, -1);
    if (layout_word) {
        g_sim_chain.clear();
        g_sim_chain.push_back(kKanbanLayout);
        poke(RT_ICO_LAYOUT_ID, (int32_t)kKanbanLayout);
    }
    poke(entry_of(kKanbanLayout) + RT_ICO_LAYOUT_CUR_ITEM, start);
    poke(kKanbanNode + RT_ICO_KANBAN_NODE_LAYOUT, (int32_t)entry_of(kKanbanLayout));
    poke(kKanbanNode + RT_ICO_KANBAN_NODE_FLAGS, 0);
    poke(RT_ICO_KANBAN_ACTIVE, (int32_t)kKanbanNode);
}

/* The derived item with this id, or a failed check. */
const RtGuestMenuItem& item_of(uint32_t id) {
    for (const RtGuestMenuItem& it : rt_guest_menu_items()) {
        if (it.item == id) return it;
    }
    CHECK(false);
    return rt_guest_menu_items()[0];
}

void centre_of(uint32_t id, float* x, float* y) {
    const RtGuestMenuItem& it = item_of(id);
    *x = (it.x0 + it.x1) * 0.5f;
    *y = (it.y0 + it.y1) * 0.5f;
}

void hover(uint32_t id) {
    float x = 0.0f, y = 0.0f;
    centre_of(id, &x, &y);
    rt_guest_menu_test_set_cursor(true, x, y);
}

/* The layout every derived item was attributed to, which is what the chain
 * walk has to get right. */
bool all_items_belong_to(uint32_t layout) {
    for (const RtGuestMenuItem& it : rt_guest_menu_items()) {
        if (it.layout != layout) return false;
    }
    return !rt_guest_menu_items().empty();
}

void unhover() {
    rt_guest_menu_test_set_cursor(false, 0.0f, 0.0f);
}

bool near_enough(float a, float b, float tol) {
    const float d = a > b ? a - b : b - a;
    return d <= tol;
}

/* ---- the tests -------------------------------------------------------------- */

/* The calibration. The user drew these two boxes on the presented picture
 * of the retail game, as fractions of the presented scanout rectangle; the
 * mapping in menu_nav.cpp has to put the same two items inside them. */
constexpr float kUserBoxA[4] = {0.399f, 0.595f, 0.607f, 0.685f};
constexpr float kUserBoxB[4] = {0.400f, 0.725f, 0.598f, 0.821f};
/* A hand-drawn box is worth about this much of the picture. */
constexpr float kCalibrationTol = 0.01f;

void test_title_rects() {
    begin("the title screen's two items land inside the boxes the user drew");
    arm(kTitleLayout, kTitleItemA);
    run_field();

    CHECK(rt_guest_menu_items().size() == 2);
    CHECK(all_items_belong_to(kTitleLayout));
    /* Reached from the entry's default item first, then down the link. */
    CHECK(rt_guest_menu_items()[0].item == (uint32_t)kTitleItemA);
    CHECK(rt_guest_menu_items()[1].item == (uint32_t)kTitleItemB);

    const RtGuestMenuItem& a = item_of((uint32_t)kTitleItemA);
    const RtGuestMenuItem& b = item_of((uint32_t)kTitleItemB);

    /* The arithmetic the module header states, exactly: a centred 128 wide
     * sprite is 0.4 to 0.6 of the picture, and a y of 135 with a height
     * field of 40 is 0.5 + 22/224 to 0.5 + 42/224. */
    CHECK(near_enough(a.x0, 0.4f, 0.0005f));
    CHECK(near_enough(a.x1, 0.6f, 0.0005f));
    CHECK(near_enough(a.y0, 0.5f + 22.0f / 224.0f, 0.0005f));
    CHECK(near_enough(a.y1, 0.5f + 42.0f / 224.0f, 0.0005f));
    CHECK(near_enough(b.y0, 0.5f + 52.0f / 224.0f, 0.0005f));
    CHECK(near_enough(b.y1, 0.5f + 72.0f / 224.0f, 0.0005f));

    /* And that is inside the boxes the user measured on the real picture. */
    CHECK(near_enough(a.x0, kUserBoxA[0], kCalibrationTol));
    CHECK(near_enough(a.y0, kUserBoxA[1], kCalibrationTol));
    CHECK(near_enough(a.x1, kUserBoxA[2], kCalibrationTol));
    CHECK(near_enough(a.y1, kUserBoxA[3], kCalibrationTol));
    CHECK(near_enough(b.x0, kUserBoxB[0], kCalibrationTol));
    CHECK(near_enough(b.y0, kUserBoxB[1], kCalibrationTol));
    CHECK(near_enough(b.x1, kUserBoxB[2], kCalibrationTol));
    CHECK(near_enough(b.y1, kUserBoxB[3], kCalibrationTol));

    /* Every screen says its rectangles once, so a log from a real run can
     * be held against what was on the screen. */
    CHECK(log_has("guest menu: layout 0xc item 0x31 rect 0.4000,0.5982,0.6000,0.6875"));
    CHECK(log_has("guest menu: layout 0xc item 0x32 rect 0.4000,0.7321,0.6000,0.8214"));
    CHECK(log_has("guest menu: layout 0xc chain 0xc: 2 items"));
    CHECK(log_count(" rect 0.") == 2);

    log_clear();
    run_fields(4);
    CHECK(log_count(" rect 0.") == 0);
}

void test_gameplay_and_cinematic_are_not_menus() {
    begin("gameplay and the cinematic hold fade 2 and are still not menus");
    for (uint32_t layout : {kGameplayLayout, kCinematicLayout}) {
        arm(layout, -1);
        run_field();
        CHECK(rt_guest_menu_state().valid);
        CHECK(rt_guest_menu_state().fade == 2);
        CHECK(rt_guest_menu_items().empty());
        CHECK(!rt_guest_menu_active());
        CHECK(!rt_guest_menu_wants_mouse());

        /* A click there presses nothing and writes nothing. */
        rt_guest_menu_test_set_cursor(true, 0.5f, 0.5f);
        press_button(1, true);
        wheel_ticks(3);
        CHECK(count_presses(run_fields(8)) == 0);
        CHECK(entry_item(layout) == -1);
    }
}

void test_fade_gate() {
    begin("only fade 2 is a menu");
    arm(kTitleLayout, kTitleItemA);
    run_field();
    CHECK(rt_guest_menu_active());

    for (uint32_t fade : {0u, 1u, 3u, 5u}) {
        g_fade = fade;
        run_field();
        CHECK(!rt_guest_menu_active());
        CHECK(!rt_guest_menu_wants_mouse());
    }
    g_fade = 2;
    run_field();
    CHECK(rt_guest_menu_active());
}

void test_unmapped_before_the_elf() {
    begin("nothing is read or written before the ELF is loaded");
    arm(kTitleLayout, kTitleItemA);
    run_field();
    CHECK(rt_guest_menu_active());

    unmap_pages();
    rt_guest_menu_tick(++g_field);
    CHECK(!rt_guest_menu_state().valid);
    CHECK(rt_guest_menu_items().empty());
    CHECK(!rt_guest_menu_active());
    map_pages();
}

void test_decoration_is_not_an_item() {
    begin("an object in the range with no links and no seed is not an item");
    arm(kTitleLayout, kTitleItemA);
    /* A third object inside the range, drawn but unreachable. */
    Obj d;
    d.w = 64;
    d.h = 20;
    d.x = 300;
    d.y = 200;
    put_object(kTitleItemB + 1, d);
    set_entry(kTitleLayout, kTitleItemA, kTitleItemB + 2, kTitleItemA, kTitleItemA);
    run_field();

    CHECK(rt_guest_menu_items().size() == 2);
    for (const RtGuestMenuItem& it : rt_guest_menu_items()) {
        CHECK(it.item != (uint32_t)(kTitleItemB + 1));
    }
}

void test_hidden_and_offscreen_are_skipped() {
    begin("a hidden object and one placed off the picture are not items");
    arm(kTitleLayout, kTitleItemA);

    /* Bit 1 is what lt_link_layout returns on, and the game's own frame
     * rewrites it every frame: phase 1 seeds it from bit 0, then the action
     * function's func_001B7218 calls put it where it wants it. g_hidden is
     * that second half, so the flag the module reads is the one a real frame
     * would have left. */
    g_hidden.push_back(kTitleItemB);
    run_field();
    CHECK((peek(object_of(kTitleItemB) + RT_ICO_OBJ_FLAGS) &
           (int32_t)RT_ICO_OBJ_FLAG_HIDDEN) != 0);
    CHECK(rt_guest_menu_items().size() == 1);
    CHECK(rt_guest_menu_items()[0].item == (uint32_t)kTitleItemA);

    /* The persistent bit alone hides it too, because phase 1 copies it into
     * the hidden one. That is the whole of what it means: the retail title
     * screen ships items 0xe and 0xf with it set. */
    g_hidden.clear();
    poke(object_of(kTitleItemB) + RT_ICO_OBJ_FLAGS, (int32_t)RT_ICO_OBJ_FLAG_PERSISTENT);
    run_field();
    CHECK(rt_guest_menu_items().size() == 1);

    /* And with neither, the frame clears bit 1 again and it is an item. */
    poke(object_of(kTitleItemB) + RT_ICO_OBJ_FLAGS, 0);
    run_field();
    CHECK(rt_guest_menu_items().size() == 2);

    /* Off the picture entirely: y far above the top of the 224-tall space. */
    poke(object_of(kTitleItemB) + RT_ICO_OBJ_Y, -400);
    run_field();
    CHECK(rt_guest_menu_items().size() == 1);

    /* A zero-sized object with no texture size to fall back on is nothing
     * on screen either. */
    poke(object_of(kTitleItemB) + RT_ICO_OBJ_Y, 165);
    poke(object_of(kTitleItemB) + RT_ICO_OBJ_H, 0);
    poke(object_of(kTitleItemB) + RT_ICO_OBJ_TEX_H, 0);
    run_field();
    CHECK(rt_guest_menu_items().size() == 1);
}

void test_hover_selects() {
    begin("hovering an item writes the game's selection word, once");
    arm(kTitleLayout, kTitleItemA);
    run_field();
    CHECK(entry_item(kTitleLayout) == kTitleItemA);

    hover((uint32_t)kTitleItemB);
    run_field();
    CHECK(entry_item(kTitleLayout) == kTitleItemB);
    /* The mirror is written alongside, so a reader before the game's own
     * frame refreshes it sees the two agree. */
    CHECK(peek(RT_ICO_ITEM_ID) == kTitleItemB);
    CHECK(log_has("guest menu: select item 0x32 on layout 0xc (was 0x31)"));
    CHECK(log_count("guest menu: select item") == 1);

    /* Staying inside the item does not write again, even after the game
     * moves the selection somewhere else. */
    log_clear();
    poke(entry_of(kTitleLayout) + RT_ICO_LAYOUT_CUR_ITEM, kTitleItemA);
    run_fields(6);
    CHECK(entry_item(kTitleLayout) == kTitleItemA);
    CHECK(log_count("guest menu: select item") == 0);

    /* Leaving and coming back acts again. */
    unhover();
    run_field();
    hover((uint32_t)kTitleItemB);
    run_field();
    CHECK(entry_item(kTitleLayout) == kTitleItemB);
    CHECK(log_count("guest menu: select item") == 1);

    /* Hovering the item that is already selected writes nothing. */
    log_clear();
    unhover();
    run_field();
    hover((uint32_t)kTitleItemB);
    run_fields(3);
    CHECK(log_count("guest menu: select item") == 0);
    CHECK(count_presses(run_fields(4)) == 0);
}

void test_click() {
    begin("a click selects the item under the cursor and presses cross");
    arm(kTitleLayout, kTitleItemA);
    run_field();

    /* The cursor lands on the item and the click arrives in the same field,
     * both after the tick has run: this is the click that does the write
     * itself rather than finding the hover has already done it. */
    float x = 0.0f, y = 0.0f;
    centre_of((uint32_t)kTitleItemB, &x, &y);
    move_cursor_in_field(true, x, y);
    press_button(1, true);

    const std::vector<uint16_t> seq = run_fields(6);
    CHECK(entry_item(kTitleLayout) == kTitleItemB);
    CHECK(log_has("guest menu: select item 0x32 on layout 0xc (was 0x31)"));
    CHECK(log_has("guest menu: click item 0x32 on layout 0xc"));
    CHECK(log_count("guest menu: select item") == 1);

    /* The cross is in the bits of the field the click arrived on, not the
     * next one: lt_switch_layout resolves the object cross applies to from
     * the word that was just written, and a field of delay would give the
     * game a whole frame to move the selection off it first. */
    CHECK(seq[0] == RT_PAD_CROSS);
    CHECK(count_presses(seq) == 1);
    CHECK(cadence_ok(seq));

    /* The click left the item acted on, so the hover does not write it a
     * second time. */
    log_clear();
    run_fields(4);
    CHECK(log_count("guest menu: select item") == 0);

    /* Releasing the button presses nothing more. */
    press_button(1, false);
    CHECK(count_presses(run_fields(6)) == 0);
}

void test_click_over_nothing() {
    begin("a click over no item presses nothing");
    arm(kTitleLayout, kTitleItemA);
    run_field();

    /* Inside the picture, outside every item. */
    rt_guest_menu_test_set_cursor(true, 0.05f, 0.05f);
    log_clear();
    press_button(1, true);
    CHECK(count_presses(run_fields(8)) == 0);
    CHECK(!log_has("guest menu: click"));
    CHECK(!log_has("guest menu: select"));
    CHECK(entry_item(kTitleLayout) == kTitleItemA);

    /* And with no cursor at all. */
    unhover();
    press_button(1, true);
    CHECK(count_presses(run_fields(8)) == 0);
}

void test_right_click() {
    begin("a right click presses triangle wherever it happens");
    arm(kTitleLayout, kTitleItemA);
    run_field();

    unhover();
    press_button(3, true);
    const std::vector<uint16_t> seq = run_fields(6);
    /* Same field as the click, like the cross. */
    CHECK(seq[0] == RT_PAD_TRIANGLE);
    CHECK(count_presses(seq) == 1);
    CHECK(cadence_ok(seq));
    for (uint16_t b : seq) {
        if (b) CHECK(b == RT_PAD_TRIANGLE);
    }
    CHECK(entry_item(kTitleLayout) == kTitleItemA);
}

void test_wheel_follows_the_links() {
    begin("a wheel tick presses the direction the item's own links point");
    arm(kTitleLayout, kTitleItemA);
    run_field();

    /* A column: the selected object links up and down, so the wheel does. */
    wheel_ticks(-1);
    std::vector<uint16_t> seq = run_fields(6);
    CHECK(count_presses(seq) == 1);
    CHECK(cadence_ok(seq));
    for (uint16_t b : seq) {
        if (b) CHECK(b == RT_PAD_DOWN);
    }
    CHECK(entry_item(kTitleLayout) == kTitleItemB);

    wheel_ticks(1);
    seq = run_fields(6);
    for (uint16_t b : seq) {
        if (b) CHECK(b == RT_PAD_UP);
    }
    CHECK(entry_item(kTitleLayout) == kTitleItemA);

    /* A row: the same object fields with left and right links instead. */
    begin("a wheel tick on a row presses left and right");
    arm(kRowLayout, kRowItemA);
    run_field();
    wheel_ticks(-1);
    seq = run_fields(6);
    CHECK(count_presses(seq) == 1);
    for (uint16_t b : seq) {
        if (b) CHECK(b == RT_PAD_RIGHT);
    }
    CHECK(entry_item(kRowLayout) == kRowItemB);

    /* Several ticks are several presses, each with its own release: the
     * first is this field's bits and the rest wait in the queue. */
    wheel_ticks(2);
    seq = run_fields(10);
    CHECK(count_presses(seq) == 2);
    CHECK(cadence_ok(seq));
    CHECK(entry_item(kRowLayout) == kRowItemA);
}

void test_chain_items_from_an_ancestor() {
    begin("a page whose own layout has no items takes them from its parent");
    arm(kPageLayout, -1);
    run_field();

    /* The current layout draws two objects and holds no selection, so it
     * offers nothing; every item comes from the parent. */
    CHECK(rt_guest_menu_items().size() == 3);
    CHECK(all_items_belong_to(kGridLayout));
    CHECK(rt_guest_menu_items()[0].item == (uint32_t)kGridItemA);
    CHECK(rt_guest_menu_items()[2].item == (uint32_t)kGridItemC);
    CHECK(rt_guest_menu_active());
    CHECK(rt_guest_menu_wants_mouse());

    /* The chain line is what says where the items came from. */
    CHECK(log_has("guest menu: layout 0x10 chain 0x10 <- 0xb: 3 items"));
    /* And each rectangle line names the layout that owns the item, not the
     * layout the state word reports. */
    CHECK(log_has("guest menu: layout 0xb item 0x6e rect"));
    /* Three rectangle lines and not one more: the current layout's own
     * objects are drawn but not offered. */
    CHECK(log_count(" rect 0.") == 3);

    /* The decoration on the current layout is not offered. */
    for (const RtGuestMenuItem& it : rt_guest_menu_items()) {
        CHECK(it.item != (uint32_t)kPageDecorA);
        CHECK(it.item != (uint32_t)kPageDecorB);
    }
}

void test_hover_writes_the_ancestors_word() {
    begin("a hover on a parent's item writes that parent's selection word");
    arm(kPageLayout, -1);
    run_field();

    const int32_t page_before = entry_item(kPageLayout);
    const int32_t mirror_before = peek(RT_ICO_ITEM_ID);

    hover((uint32_t)kGridItemB);
    run_field();
    CHECK(entry_item(kGridLayout) == kGridItemB);
    /* The current layout's own word is left alone, and so is the item
     * mirror: lt_next_layout loads the mirror from the current layout last,
     * so a parent's field never reaches it. */
    CHECK(entry_item(kPageLayout) == page_before);
    CHECK(peek(RT_ICO_ITEM_ID) == mirror_before);
    CHECK(log_has("guest menu: select item 0x6f on layout 0xb (was 0x6e)"));
    CHECK(log_count("guest menu: select item") == 1);

    /* One write per hover, on the parent's word as on the current one. */
    log_clear();
    poke(entry_of(kGridLayout) + RT_ICO_LAYOUT_CUR_ITEM, kGridItemA);
    run_fields(6);
    CHECK(entry_item(kGridLayout) == kGridItemA);
    CHECK(log_count("guest menu: select item") == 0);

    /* A click confirms the parent's item: the select goes to the parent's
     * word and the cross goes through the pad. */
    log_clear();
    unhover();
    run_field();
    hover((uint32_t)kGridItemC);
    press_button(1, true);
    const std::vector<uint16_t> seq = run_fields(6);
    CHECK(entry_item(kGridLayout) == kGridItemC);
    CHECK(log_has("guest menu: click item 0x70 on layout 0xb"));
    CHECK(seq[0] == RT_PAD_CROSS);
    CHECK(count_presses(seq) == 1);

    /* The wheel reads its direction off the layout the items came from, so
     * a row of parent items steps left and right. */
    log_clear();
    unhover();
    run_field();
    poke(entry_of(kGridLayout) + RT_ICO_LAYOUT_CUR_ITEM, kGridItemA);
    wheel_ticks(-1);
    const std::vector<uint16_t> wheel = run_fields(6);
    CHECK(count_presses(wheel) == 1);
    for (uint16_t b : wheel) {
        if (b) CHECK(b == RT_PAD_RIGHT);
    }
    CHECK(entry_item(kGridLayout) == kGridItemB);
}

void test_chain_layout_without_a_selection() {
    begin("a chain layout the game will not navigate contributes nothing");
    arm(kPageLayout, -1);
    /* The parent's current item goes negative, which is what lt_switch_layout
     * and lt_link_layout both refuse to act on. */
    poke(entry_of(kGridLayout) + RT_ICO_LAYOUT_CUR_ITEM, -1);
    run_field();

    CHECK(rt_guest_menu_items().empty());
    CHECK(!rt_guest_menu_active());
    CHECK(log_has("guest menu: layout 0x10 chain 0x10 <- 0xb: 0 items"));
    CHECK(log_has("guest menu: layout 0xb is in the chain and has reachable objects,"
                  " but its current item is negative"));

    /* A click there presses nothing and writes nothing. */
    rt_guest_menu_test_set_cursor(true, 0.5f, 0.5f);
    press_button(1, true);
    wheel_ticks(2);
    CHECK(count_presses(run_fields(8)) == 0);
    CHECK(entry_item(kGridLayout) == -1);

    /* Give it a selection back and the page works again. */
    poke(entry_of(kGridLayout) + RT_ICO_LAYOUT_CUR_ITEM, kGridItemA);
    run_field();
    CHECK(rt_guest_menu_items().size() == 3);
    CHECK(rt_guest_menu_active());
}

void test_memory_card_screen() {
    begin("the screen adjustment screen is keyed by its own level");
    arm(kAdjustLayout, 7);
    run_field();

    /* Fifteen positions, in selector order, and the layout's own default
     * item (a 1 by 1 placeholder off the picture) is not among them. */
    CHECK(rt_guest_menu_items().size() == RT_ICO_ADJUST_COUNT);
    for (uint32_t i = 0; i < RT_ICO_ADJUST_COUNT; ++i) {
        CHECK(rt_guest_menu_items()[i].item == i);
    }
    const RtGuestMenuItem& first = rt_guest_menu_items()[0];
    const RtGuestMenuItem& last = rt_guest_menu_items()[RT_ICO_ADJUST_COUNT - 1];
    CHECK(near_enough(first.x0, 0.5f - 240.0f / 640.0f, 0.0005f));
    CHECK(near_enough(last.x1, 0.5f + 240.0f / 640.0f, 0.0005f));
    CHECK(near_enough(first.y0, 0.5f - 23.0f / 224.0f, 0.0005f));
    CHECK(near_enough(first.y1, 0.5f - 8.0f / 224.0f, 0.0005f));
    CHECK(rt_guest_menu_active());

    /* The hover writes the selector, not the layout entry. */
    const int32_t entry_before = entry_item(kAdjustLayout);
    hover(3);
    run_field();
    CHECK(peek(RT_ICO_BRIGHTNESS) == 3);
    CHECK(entry_item(kAdjustLayout) == entry_before);
    CHECK(log_has("guest menu: select item 0x3 on layout 0x3c (was 0x7)"));

    /* Its handler only reads LEFT and RIGHT. */
    unhover();
    run_field();
    wheel_ticks(-1);
    const std::vector<uint16_t> seq = run_fields(6);
    CHECK(count_presses(seq) == 1);
    for (uint16_t b : seq) {
        if (b) CHECK(b == RT_PAD_RIGHT);
    }
    CHECK(peek(RT_ICO_BRIGHTNESS) == 4);

    /* la_adjust_screen calls GetRealModelId to set the persistent flag
     * on all fifteen every frame and clear it on the one its selector names;
     * phase 1 of the next frame copies it into the hidden one, so all but the
     * selected read as hidden. That flag is the general per-object one and
     * this screen is using it as a lit marker, which says nothing about where
     * the fifteen places are, so all fifteen stay hoverable. */
    for (uint32_t i = 0; i < RT_ICO_ADJUST_COUNT; ++i) {
        const int32_t lit = (int32_t)i == peek(RT_ICO_BRIGHTNESS)
                                ? 0 : (int32_t)RT_ICO_OBJ_FLAG_PERSISTENT;
        poke(object_of((int32_t)(RT_ICO_ADJUST_FIRST_OBJ + i)) + RT_ICO_OBJ_FLAGS, lit);
    }
    run_field();
    for (uint32_t i = 0; i < RT_ICO_ADJUST_COUNT; ++i) {
        const int32_t f = peek(object_of((int32_t)(RT_ICO_ADJUST_FIRST_OBJ + i)) +
                               RT_ICO_OBJ_FLAGS);
        const bool selected = (int32_t)i == peek(RT_ICO_BRIGHTNESS);
        CHECK(((f & (int32_t)RT_ICO_OBJ_FLAG_HIDDEN) != 0) != selected);
    }
    CHECK(rt_guest_menu_items().size() == RT_ICO_ADJUST_COUNT);
    hover(11);
    run_field();
    CHECK(peek(RT_ICO_BRIGHTNESS) == 11);
}

void test_item_outside_the_layout() {
    begin("a write outside the layout's range is refused, and takes the cross");
    arm(kTitleLayout, kTitleItemA);
    run_field();
    CHECK(rt_guest_menu_items().size() == 2);

    /* The range shrinks under the items the tick already derived, which is
     * the only way an offered item can stop being writable. */
    hover((uint32_t)kTitleItemB);
    poke(entry_of(kTitleLayout) + RT_ICO_LAYOUT_END_OBJ, kTitleItemB);
    log_clear();
    /* The one place this file calls an entry point outside the field order:
     * the range is shrunk between the tick that derived the items and the
     * click, which no field of the real runtime can do, because pad.cpp runs
     * the tick and the click with no guest code in between. The check is
     * defensive and this is the only way to reach it. */
    rt_guest_menu_on_button(1, true);
    CHECK(log_has("is outside layout 0xc's items"));
    CHECK(!log_has("guest menu: click"));
    CHECK(entry_item(kTitleLayout) == kTitleItemA);
    CHECK(count_presses(run_fields(8)) == 0);
}

void test_pulse_bits_contract() {
    begin("pulse bits belong to one field and are not repeated");
    arm(kTitleLayout, kTitleItemA);
    run_field();

    unhover();
    ++g_field;
    poke(RT_ICO_LAYOUT_ID, (int32_t)g_layout);
    poke(RT_ICO_FADE_STATE, 2);
    rt_guest_menu_tick(g_field);
    /* The provider's order: the tick, then the field's events, then the
     * bits. The press started inside the event is this field's. */
    rt_guest_menu_on_button(3, true);

    /* The same field twice gives the same answer. */
    const uint16_t a = rt_guest_menu_pulse_bits(g_field);
    const uint16_t b = rt_guest_menu_pulse_bits(g_field);
    CHECK(a == RT_PAD_TRIANGLE);
    CHECK(a == b);

    /* Another field's number gives nothing, and says so once. */
    log_clear();
    CHECK(rt_guest_menu_pulse_bits(g_field + 1) == 0);
    CHECK(log_has("pulse bits asked for field"));
    log_clear();
    CHECK(rt_guest_menu_pulse_bits(g_field + 2) == 0);
    CHECK(!log_has("pulse bits asked for field"));
}

void test_without_the_sdl_provider() {
    begin("with no SDL provider the pointer reads and logs but never writes");
    arm(kTitleLayout, kTitleItemA);
    run_field();
    CHECK(rt_guest_menu_active());

    g_sdl_active = false;
    log_clear();
    hover((uint32_t)kTitleItemB);
    press_button(1, true);
    wheel_ticks(-1);
    CHECK(count_presses(run_fields(6)) == 0);
    CHECK(entry_item(kTitleLayout) == kTitleItemA);
    CHECK(log_count("guest menu: select item") == 0);
    CHECK(!log_has("guest menu: click"));

    /* The read half is not gated: the state and the items are still there,
     * which is what the change log is written from. */
    CHECK(rt_guest_menu_state().valid);
    CHECK(rt_guest_menu_items().size() == 2);
    CHECK(rt_guest_menu_active());

    /* And the provider coming back makes the same hover act. */
    g_sdl_active = true;
    run_field();
    CHECK(entry_item(kTitleLayout) == kTitleItemB);
    CHECK(log_count("guest menu: select item") == 1);
}

void test_navigation_swallowed() {
    begin("a hover write waits for a frame the game will navigate");
    arm(kTitleLayout, kTitleItemA);
    run_field();

    /* D_00633160 is up: lt_switch_layout would return at once and
     * lt_link_layout would draw no highlight, so a write now would move a
     * selection the game is not going to act on. */
    g_swallow = true;
    hover((uint32_t)kTitleItemB);
    run_fields(4);
    CHECK(entry_item(kTitleLayout) == kTitleItemA);
    CHECK(log_count("guest menu: select item") == 0);
    CHECK(log_has("swallowing this frame's navigation"));

    /* One line and not one per field. */
    CHECK(log_count("swallowing this frame's navigation") == 1);

    /* The flag clears and the same hover, never having been acted on, is
     * taken now. */
    g_swallow = false;
    run_field();
    CHECK(entry_item(kTitleLayout) == kTitleItemB);
    CHECK(log_count("guest menu: select item") == 1);

    /* Still one write per hover after that. */
    log_clear();
    poke(entry_of(kTitleLayout) + RT_ICO_LAYOUT_CUR_ITEM, kTitleItemA);
    run_fields(4);
    CHECK(log_count("guest menu: select item") == 0);
}

/* The load grid's D_00633164 handler: the pointer writes the empty slot the
 * cursor is on, the game's own frame moves the selection off it to the next
 * occupied one, and the pointer leaves it there. Re-writing every field
 * would fight the game forever. */
void test_custom_handler_moves_off_an_empty_slot() {
    begin("the game moving off an empty slot is not re-written");
    arm(kPageLayout, -1);
    run_field();
    CHECK(rt_guest_menu_items().size() == 3);
    CHECK(entry_item(kGridLayout) == kGridItemA);

    /* The middle slot is empty, and the page's action function installs its
     * handler on every frame from here on. */
    g_empty_slots.push_back(kGridItemB);
    g_custom_handler = true;

    hover((uint32_t)kGridItemB);
    run_field();
    /* The write happened on the field the cursor entered the item. */
    CHECK(log_has("guest menu: select item 0x6f on layout 0xb (was 0x6e)"));
    CHECK(entry_item(kGridLayout) == kGridItemB);

    /* The next game frame runs the handler, which moves on to the next
     * occupied slot. */
    log_clear();
    run_field();
    CHECK(entry_item(kGridLayout) == kGridItemC);
    /* The cursor has not left the item, so nothing writes it back: this is
     * the case the one-write-per-hover rule exists for, and a pointer that
     * re-wrote every field would fight the handler forever. */
    run_fields(6);
    CHECK(entry_item(kGridLayout) == kGridItemC);
    CHECK(log_count("guest menu: select item") == 0);

    /* Leaving and coming back is a new hover and does act again. */
    unhover();
    run_field();
    hover((uint32_t)kGridItemB);
    run_field();
    CHECK(entry_item(kGridLayout) == kGridItemB);
    CHECK(log_count("guest menu: select item") == 1);
}

/* sif/pad.cpp runs several pad fields in a row after a long frame, so the
 * item mirror D_00633150 can be several ticks stale. Every decision is taken
 * from the layout entry's +0x2C, never from the mirror. */
void test_the_mirror_can_be_several_ticks_stale() {
    begin("the selection is read from +0x2C, not from the stale item mirror");
    arm(kTitleLayout, kTitleItemA);
    run_field();

    /* The game's own word says B is selected; the mirror still says A, which
     * is what a frame boundary that has not happened yet leaves behind. */
    poke(entry_of(kTitleLayout) + RT_ICO_LAYOUT_CUR_ITEM, kTitleItemB);
    poke(RT_ICO_ITEM_ID, kTitleItemA);

    log_clear();
    hover((uint32_t)kTitleItemB);
    for (int i = 0; i < 5; ++i) CHECK(run_tick_only() == 0);
    /* Reading the mirror would have made this a move from A to B. */
    CHECK(log_count("guest menu: select item") == 0);
    CHECK(entry_item(kTitleLayout) == kTitleItemB);
    CHECK(peek(RT_ICO_ITEM_ID) == kTitleItemA);

    /* And the other way: the mirror agrees with the hovered item while
     * +0x2C does not, so the write has to happen, once, across as many
     * ticks as the catch-up loop runs. */
    unhover();
    run_tick_only();
    poke(RT_ICO_ITEM_ID, kTitleItemA);
    hover((uint32_t)kTitleItemA);
    for (int i = 0; i < 5; ++i) run_tick_only();
    CHECK(entry_item(kTitleLayout) == kTitleItemA);
    CHECK(log_count("guest menu: select item") == 1);
}

void test_negative_size_is_logged() {
    begin("an object with a negative drawn size is dropped with a line");
    arm(kTitleLayout, kTitleItemA);
    run_field();
    CHECK(rt_guest_menu_items().size() == 2);

    log_clear();
    poke(object_of(kTitleItemB) + RT_ICO_OBJ_TEX_W, -128);
    run_fields(3);
    CHECK(rt_guest_menu_items().size() == 1);
    CHECK(log_has("guest menu: layout 0xc object 0x32 has a negative drawn size"));
    /* Once, whatever the screen does afterwards. */
    CHECK(log_count("has a negative drawn size") == 1);
}

void test_cursor_outside() {
    begin("a cursor outside the presented rectangle hovers nothing");
    arm(kTitleLayout, kTitleItemA);
    run_field();

    rt_guest_menu_test_set_cursor(true, 1.20f, 0.15f);
    float nx = 0.0f, ny = 0.0f;
    CHECK(!rt_guest_menu_cursor(&nx, &ny));
    run_fields(4);
    CHECK(entry_item(kTitleLayout) == kTitleItemA);

    hover((uint32_t)kTitleItemB);
    CHECK(rt_guest_menu_cursor(&nx, &ny));
    run_field();
    CHECK(entry_item(kTitleLayout) == kTitleItemB);
}

/* ---- the drawn cursor ------------------------------------------------------
 *
 * With relative mouse mode on there is no OS cursor to map, so the pointer
 * carries its own position and the field's relative motion moves it. The
 * injected motion scale stands in for the present rectangle: 1/640 and
 * 1/480 of the picture per window pixel is a 640x480 picture filling a
 * 640x480 window, where one mouse pixel is one picture pixel.
 */
constexpr float kScaleX = 1.0f / 640.0f;
constexpr float kScaleY = 1.0f / 480.0f;

/* arm() with relative mode on and a picture to move across. The injected OS
 * cursor is left invalid throughout, so a position that comes back can only
 * be the drawn one. */
void arm_drawn(int32_t start) {
    arm(kTitleLayout, start);
    rt_guest_menu_test_set_cursor(false, 0.0f, 0.0f);
    rt_guest_menu_test_set_motion_scale(kScaleX, kScaleY);
    rt_guest_menu_test_set_captured(true);
}

void test_drawn_cursor_starts_at_centre() {
    begin("the drawn cursor starts at the centre when the pointer takes the mouse");
    arm_drawn(kTitleItemA);

    /* Before the first tick the pointer has never had the mouse. */
    float nx = 0.0f, ny = 0.0f;
    CHECK(!rt_guest_menu_test_drawn_cursor(&nx, &ny));
    CHECK(!rt_guest_menu_cursor(&nx, &ny));

    run_field();
    CHECK(log_has("pointer takes the mouse, drawn cursor at centre"));
    CHECK(rt_guest_menu_cursor(&nx, &ny));
    CHECK(nx > 0.4999f && nx < 0.5001f);
    CHECK(ny > 0.4999f && ny < 0.5001f);
}

void test_drawn_cursor_motion() {
    begin("relative motion moves the drawn cursor by its fraction of the picture");
    arm_drawn(kTitleItemA);
    run_field();

    /* A tenth of the picture right and a tenth of it up. */
    rt_guest_menu_on_motion(64.0f, -48.0f);
    float nx = 0.0f, ny = 0.0f;
    CHECK(rt_guest_menu_cursor(&nx, &ny));
    CHECK(nx > 0.5999f && nx < 0.6001f);
    CHECK(ny > 0.3999f && ny < 0.4001f);

    /* Motion accumulates rather than replacing. */
    rt_guest_menu_on_motion(-32.0f, 24.0f);
    CHECK(rt_guest_menu_cursor(&nx, &ny));
    CHECK(nx > 0.5499f && nx < 0.5501f);
    CHECK(ny > 0.4499f && ny < 0.4501f);
}

void test_drawn_cursor_clamps() {
    begin("the drawn cursor clamps to the picture");
    arm_drawn(kTitleItemA);
    run_field();

    rt_guest_menu_on_motion(-10000.0f, -10000.0f);
    float nx = 1.0f, ny = 1.0f;
    CHECK(rt_guest_menu_cursor(&nx, &ny));
    CHECK(nx == 0.0f && ny == 0.0f);

    rt_guest_menu_on_motion(10000.0f, 10000.0f);
    CHECK(rt_guest_menu_cursor(&nx, &ny));
    CHECK(nx == 1.0f && ny == 1.0f);
}

void test_drawn_cursor_needs_a_picture() {
    begin("motion with nothing presented cannot move the drawn cursor");
    arm(kTitleLayout, kTitleItemA);
    rt_guest_menu_test_set_cursor(false, 0.0f, 0.0f);
    rt_guest_menu_test_set_captured(true);
    /* No motion scale injected: nothing has been presented. */
    run_field();

    rt_guest_menu_on_motion(50.0f, 50.0f);
    CHECK(log_has("nothing presented yet"));
    float nx = 0.0f, ny = 0.0f;
    CHECK(rt_guest_menu_cursor(&nx, &ny));
    CHECK(nx > 0.4999f && nx < 0.5001f);
    CHECK(ny > 0.4999f && ny < 0.5001f);
}

void test_drawn_cursor_hovers() {
    begin("the selection is written from the drawn cursor");
    arm_drawn(kTitleItemA);
    run_field();

    /* From the centre onto the second item, in pixels of the same picture. */
    float x = 0.0f, y = 0.0f;
    centre_of((uint32_t)kTitleItemB, &x, &y);
    rt_guest_menu_on_motion((x - 0.5f) / kScaleX, (y - 0.5f) / kScaleY);

    float nx = 0.0f, ny = 0.0f;
    CHECK(rt_guest_menu_cursor(&nx, &ny));
    CHECK(nx > x - 0.002f && nx < x + 0.002f);
    CHECK(ny > y - 0.002f && ny < y + 0.002f);

    const std::vector<uint16_t> seq = run_fields(8);
    CHECK(count_presses(seq) == 0);
    CHECK(entry_item(kTitleLayout) == kTitleItemB);
}

void test_kanban_boot_screen() {
    begin("a kanban boot screen is found through its node, not the layout id word");
    arm_kanban(kKanbanItemA);
    run_field();

    /* The layout state words say gameplay at fade 2, which has no items at
     * all, so everything below came out of the node. */
    CHECK(rt_guest_menu_state().layout == kGameplayLayout);
    CHECK(rt_guest_menu_state().fade == 2);
    CHECK(rt_guest_menu_active());
    CHECK(rt_guest_menu_state().kanban);
    CHECK(rt_guest_menu_state().kanban_layout == kKanbanLayout);
    CHECK(rt_guest_menu_items().size() == 2);
    CHECK(all_items_belong_to(kKanbanLayout));
    CHECK(rt_guest_menu_items()[0].item == (uint32_t)kKanbanItemA);
    CHECK(rt_guest_menu_items()[1].item == (uint32_t)kKanbanItemB);
    CHECK(log_has("guest menu: kanban layout 0x1 (the 50/60 Hz screen): 2 items"));

    /* This emitter's y bias is 112, not the layout path's 113: a y of 65 and
     * a height field of 40 run from 65 - 112 to 65 - 112 + 20. */
    const RtGuestMenuItem& a = item_of((uint32_t)kKanbanItemA);
    CHECK(near_enough(a.x0, 0.5f + (190.0f - 320.0f) / 640.0f, 0.0005f));
    CHECK(near_enough(a.x1, 0.5f + (190.0f + 80.0f - 320.0f) / 640.0f, 0.0005f));
    CHECK(near_enough(a.y0, 0.5f + (65.0f - 112.0f) / 224.0f, 0.0005f));
    CHECK(near_enough(a.y1, 0.5f + (65.0f - 112.0f + 20.0f) / 224.0f, 0.0005f));

    /* Hovering writes the layout entry's own current item, which is the word
     * func_001B8B10 re-derives the highlight from every frame. */
    hover((uint32_t)kKanbanItemB);
    run_field();
    CHECK(entry_item(kKanbanLayout) == kKanbanItemB);
    CHECK(log_has("guest menu: select item 0x22 on layout 0x1 (was 0x21)"));

    /* A click confirms it with a cross on the virtual pad, in the field the
     * click arrived on, because display_layout reads a pressed-this-frame
     * word exactly as lt_switch_layout does. */
    log_clear();
    float x = 0.0f, y = 0.0f;
    centre_of((uint32_t)kKanbanItemA, &x, &y);
    move_cursor_in_field(true, x, y);
    press_button(1, true);
    const std::vector<uint16_t> seq = run_fields(6);
    CHECK(entry_item(kKanbanLayout) == kKanbanItemA);
    CHECK(log_has("guest menu: click item 0x21 on layout 0x1"));
    CHECK(seq[0] == RT_PAD_CROSS);
    CHECK(count_presses(seq) == 1);
    CHECK(cadence_ok(seq));

    /* The wheel steps along the links this screen has, which is a row. */
    log_clear();
    wheel_ticks(-1);
    const std::vector<uint16_t> wheel = run_fields(4);
    CHECK(wheel[0] == RT_PAD_RIGHT);
}

void test_kanban_screen_ignores_the_layout_words() {
    begin("a kanban screen writes no item mirror and does not wait for the swallow flag");
    arm_kanban(kKanbanItemA, /*layout_word=*/true);
    run_field();
    CHECK(rt_guest_menu_state().kanban);
    CHECK(rt_guest_menu_state().layout == kKanbanLayout);

    /* The layout id word names this same entry, so the only thing keeping
     * the item mirror out of it is that this is a kanban screen:
     * lt_next_layout is what refreshes that word and it does not run here. */
    const int32_t mirror = peek(RT_ICO_ITEM_ID);
    hover((uint32_t)kKanbanItemB);
    run_field();
    CHECK(entry_item(kKanbanLayout) == kKanbanItemB);
    CHECK(peek(RT_ICO_ITEM_ID) == mirror);

    /* And the swallow flag is the layout system's: display_layout never
     * reads it, so a hover here writes on its own field. */
    log_clear();
    unhover();
    run_field();
    poke(entry_of(kKanbanLayout) + RT_ICO_LAYOUT_CUR_ITEM, kKanbanItemA);
    g_swallow = true;
    hover((uint32_t)kKanbanItemB);
    run_field();
    CHECK(peek(RT_ICO_NAV_SWALLOW) != 0);
    CHECK(entry_item(kKanbanLayout) == kKanbanItemB);
    CHECK(log_count("guest menu: select item") == 1);
}

void test_kanban_screen_ends() {
    begin("a kanban screen that is fading out or gone hands the mouse back");
    arm_kanban(kKanbanItemA);
    run_field();
    CHECK(log_has("pointer takes the mouse on kanban layout 0x1 (the 50/60 Hz screen)"));

    /* kanbanReqDelFade sets bit 0 of the node's flag word, and
     * display_layout gives such a node no input at all. */
    log_clear();
    poke(kKanbanNode + RT_ICO_KANBAN_NODE_FLAGS, (int32_t)RT_ICO_KANBAN_FLAG_FADING);
    run_field();
    CHECK(!rt_guest_menu_active());
    CHECK(rt_guest_menu_items().empty());
    CHECK(log_has("pointer hands the mouse back to mouse look on kanban layout 0x1"));

    /* And a word that is not one of the thirty node slots is refused with a
     * line naming it, rather than read as a node. */
    log_clear();
    poke(RT_ICO_KANBAN_ACTIVE, 0x12345678);
    run_field();
    CHECK(!rt_guest_menu_active());
    CHECK(log_has("is not one of the thirty node slots"));

    /* Once, however many fields it stays wrong for. */
    log_clear();
    run_fields(4);
    CHECK(log_count("is not one of the thirty node slots") == 0);

    /* An entry pointer that is not a row of the layout table is refused the
     * same way. */
    log_clear();
    poke(RT_ICO_KANBAN_ACTIVE, (int32_t)kKanbanNode);
    poke(kKanbanNode + RT_ICO_KANBAN_NODE_FLAGS, 0);
    poke(kKanbanNode + RT_ICO_KANBAN_NODE_LAYOUT,
         (int32_t)(RT_ICO_LAYOUT_TABLE + RT_ICO_LAYOUT_STRIDE / 2));
    run_field();
    CHECK(!rt_guest_menu_active());
    CHECK(log_has("is not a row of the layout table"));
}

void test_drawn_cursor_handover() {
    begin("the log carries the pointer taking the mouse and handing it back");
    arm_drawn(kTitleItemA);
    run_field();
    CHECK(log_has("pointer takes the mouse, drawn cursor at centre"));
    rt_guest_menu_on_motion(64.0f, 0.0f);

    /* The fade leaves 2: the menu is no longer interactive, so the motion
     * goes back to mouse look. */
    log_clear();
    g_fade = 3;
    run_field();
    CHECK(log_has("pointer hands the mouse back to mouse look"));

    /* Back on the menu, at the position the cursor was left at rather than
     * at the centre again. */
    log_clear();
    g_fade = 2;
    run_field();
    CHECK(log_has("pointer takes the mouse, drawn cursor at 0.600,0.500"));
    float nx = 0.0f, ny = 0.0f;
    CHECK(rt_guest_menu_cursor(&nx, &ny));
    CHECK(nx > 0.5999f && nx < 0.6001f);
}

void test_drawn_cursor_yields_to_the_os_cursor() {
    begin("with relative mode off the OS cursor answers instead");
    arm_drawn(kTitleItemA);
    run_field();
    rt_guest_menu_on_motion(64.0f, -48.0f);

    rt_guest_menu_test_set_captured(false);
    float nx = 0.0f, ny = 0.0f;
    /* No OS cursor injected yet: the drawn one must not stand in for it. */
    CHECK(!rt_guest_menu_cursor(&nx, &ny));

    rt_guest_menu_test_set_cursor(true, 0.09f, 0.15f);
    CHECK(rt_guest_menu_cursor(&nx, &ny));
    CHECK(nx > 0.089f && nx < 0.091f);

    /* The drawn cursor kept its own position through all of that. */
    CHECK(rt_guest_menu_test_drawn_cursor(&nx, &ny));
    CHECK(nx > 0.5999f && nx < 0.6001f);
}

} // namespace

int main() {
    map_pages();

    test_title_rects();
    test_gameplay_and_cinematic_are_not_menus();
    test_fade_gate();
    test_unmapped_before_the_elf();
    test_decoration_is_not_an_item();
    test_hidden_and_offscreen_are_skipped();
    test_hover_selects();
    test_click();
    test_click_over_nothing();
    test_right_click();
    test_wheel_follows_the_links();
    test_chain_items_from_an_ancestor();
    test_hover_writes_the_ancestors_word();
    test_chain_layout_without_a_selection();
    test_memory_card_screen();
    test_kanban_boot_screen();
    test_kanban_screen_ignores_the_layout_words();
    test_kanban_screen_ends();
    test_item_outside_the_layout();
    test_pulse_bits_contract();
    test_without_the_sdl_provider();
    test_navigation_swallowed();
    test_custom_handler_moves_off_an_empty_slot();
    test_the_mirror_can_be_several_ticks_stale();
    test_negative_size_is_logged();
    test_cursor_outside();
    test_drawn_cursor_starts_at_centre();
    test_drawn_cursor_motion();
    test_drawn_cursor_clamps();
    test_drawn_cursor_needs_a_picture();
    test_drawn_cursor_hovers();
    test_drawn_cursor_handover();
    test_drawn_cursor_yields_to_the_os_cursor();

    std::printf("\nmenu-nav selftest: %s\n", g_failures == 0 ? "all checks passed" : "FAILED");
    return g_failures == 0 ? 0 : 2;
}
