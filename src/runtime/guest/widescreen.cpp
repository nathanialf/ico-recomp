/* guest/widescreen.cpp: scale the guest's own horizontal projection scale.
 *
 * What the game does, measured on SCES_507.60 (2026-09-05; an earlier
 * reading of this header quoted another build's addresses):
 *   The writer at 0x00114BD8 fills a nine-float projection block at
 *   0x0067BA60 (RT_ICO_PROJ_BLOCK). It forms the base with `lui $16, 0x0068`
 *   at 0x00114CB0 and `addiu $2, $16, -17824` at 0x00114DAC, writes +0x00
 *   at 0x00114CF0 or at 0x00114D20 depending on which branch it took, and
 *   +0x04 through +0x20 at 0x00114E20 through 0x00114E48. It then puts the
 *   block's address in register 8 (`daddu $8, $2, $0` at 0x00114E0C) and
 *   ends with `j 0x001146F0` at 0x00114E4C, a tail jump to the matrix
 *   composer at RT_ICO_MATRIX_COMPOSER. The composer takes that pointer
 *   straight into $16 (`daddu $16, $8, $0` at 0x00114700) and reads +0x00
 *   through +0x20 off it.
 *
 *   That tail jump is the composer's only caller, which is the property
 *   this module rests on: searching the whole loaded image for a `j` or a
 *   `jal` whose target is 0x001146F0 finds exactly one, the `j` at
 *   0x00114E4C. So register 8 at the composer's entry is always the block
 *   the composer is about to compose.
 *
 *   The disc's own listing names the two gsb_SetVSMatrix (the writer, disc
 *   0x00114AD0) and gsb_SetVSMatrixSub (the composer, disc 0x001145E8, 270
 *   words matched against the retail bytes with the relocatable fields
 *   masked out and no mismatch). Only the writer's entry is correlated and
 *   not its body: the two links differ inside it, because the disc build
 *   calls the composer and returns where this one tail-jumps.
 *
 * What this module does there:
 *   reads the block pointer out of $8 and multiplies the X scale word at
 *   +0x04 by the factor the settings layer set. Nothing else is written, and
 *   nothing at all is written while the factor is off.
 *
 * Why the pointer rather than a fixed address: the composer composes
 * whatever block it is handed, so the value to scale is the one in that
 * block, not the one at an address this file decided on in advance. Today
 * the two are the same (RT_ICO_PROJ_BLOCK is the only block any caller
 * builds), and a pointer that is not that block is logged so we learn about
 * it, but the register is what the composer itself reads and so it is what
 * this module reads.
 *
 * Why every block is scaled, whatever the current X scale:
 *   The writer stores (float)a0 / (float)RT_ICO_SCREEN_W at +0x04, where a0
 *   is its first argument: it takes a0 into $s2 at 0x00114BF4
 *   (`daddu $s2, $a0, $zero`), converts both at 0x00114D7C and 0x00114DB8,
 *   divides at 0x00114DE8 and stores at `swc1 $f2, 4($v0)` (0x00114E40).
 *   It has five callers, the disc listing's name for each beside the site
 *   and the argument decoded from the retail words:
 *     0x0010C270  dispPool (pool.c)          passes the literal 204
 *     0x00112764  gsb_Init (GsBase.c)        passes RT_ICO_SCREEN_W -> 1.0
 *     0x001934C4  MakeCameraMatrix           passes RT_ICO_SCREEN_W -> 1.0
 *     0x00194424  SetCameraMatrix            passes RT_ICO_SCREEN_W -> 1.0
 *     0x001F5BA8  drawAreaSetup (puddle.c)   passes the literal 230
 *   The two with a literal are 3D projections into smaller buffers, not a
 *   different kind of value: each is the same projection of the same world,
 *   rendered narrower. They have to widen by the same factor as the main
 *   view or they stop lining up with it. So the rule is to scale the X
 *   scale that is there, not to scale only the X scales that happen to be
 *   1.0. There is no latch and no expectation of a particular value.
 *
 * The reads and the write use the same shape as guest/menu_nav.cpp:
 * rt_gptr plus memcpy, never rt_gwrite32, and never fatal. An unmapped
 * address here means the ELF is not loaded yet, which is a state to return
 * from, not to die on.
 *
 * Every rt_log call below carries the level the leveled logging API should
 * give it, as a trailing comment, so that conversion stays mechanical.
 */
#include "widescreen.h"

#include "gmem.h"
#include "ico_syms.h"

#include "../ee/kernel.h"
#include "../runtime.h"

#include <atomic>
#include <cmath>
#include <cstring>

namespace {

/* 1.0 is off, and so is the state before the settings layer has said
 * anything.
 *
 * Atomic because the two ends are different threads: the settings layer
 * writes it from rt_settings_apply_pending on the EE thread, and the native
 * renderer reads it through rt_widescreen_present_aspect on the GS worker
 * thread once the command ring is threaded. Relaxed is enough: the value
 * stands alone, nothing else is published with it, and a present that reads
 * the previous factor is one field behind at worst. The paraLLEl-GS path
 * pushes the same number across its own ABI instead
 * (GsBackend::set_widescreen_aspect), because it cannot link this. */
std::atomic<double> g_factor{1.0};

double factor() { return g_factor.load(std::memory_order_relaxed); }

/* Blocks scaled since the process started. prof.h's counters are
 * exclusive-time zones for the subsystem buckets, not a place to add a call
 * count, so this is a plain counter of its own, read through
 * rt_widescreen_applied_count(). */
uint64_t g_applied = 0;

/* One line each, not one per call: the composer runs several times a field.
 * None of these latch the module off; they only skip the call they describe,
 * so a transient stays transient. */
bool g_logged_bad_pointer = false;
bool g_logged_other_block = false;
bool g_logged_bad_value = false;

/* Bumped by every real factor change, read by hw/gif.cpp. */
uint64_t g_generation = 0;

/* The span the composer reads off the pointer, +0x00 through +0x20
 * inclusive: nine floats. Used to check the whole block is mapped, not just
 * the one word this module writes. */
constexpr uint32_t kBlockSpan = 0x24;

/* The X scale the game can plausibly have computed. The writer at
 * 0x00114BD8 stores a0 / RT_ICO_SCREEN_W there, a ratio of two positive
 * screen-sized numbers, and 4 is well clear of every value its five callers
 * produce (1.0, 204/width and 230/width). Outside this the word is not the
 * X scale, which is a fact to report rather than a number to multiply. */
constexpr float kXScaleMax = 4.0f;

/* The other end of the same test, and the one the failure this design can
 * actually have would trip. Every widening factor is below 1, so a composer
 * entered twice against one block write would multiply the scale by k again
 * and make it smaller, not larger: nothing would notice until it underflowed
 * to zero after roughly a thousand entries. The floor is the smallest scale
 * the five known callers produce (204/512 = 0.199, dispPool's; the other
 * literal caller, drawAreaSetup, passes 230 and so sits above it) times the
 * factor in force, with 1 per cent of slack for the float round trip. A
 * value under it is a scale that has already been scaled, and it is
 * reported rather than scaled again.
 *
 * The bound moves with the factor, so it is computed at the call rather than
 * being a constant. */
constexpr double kXScaleMinCaller = 0xCC / 512.0;
constexpr double kXScaleMinSlack = 0.99;

bool factor_is_off(double k) { return k == 1.0 || k == 0.0; }

double x_scale_min(double k) { return kXScaleMinCaller * k * kXScaleMinSlack; }

/* The two accessors this module needs, with the "never fatal, false when the
 * page is unmapped" contract guest/gmem.h states once for the three observer
 * modules that share it. */
using rt_gmem::read_f32;
using rt_gmem::write_f32;

/* The block is a quadword-aligned object in the 32 MB of EE RAM. A pointer
 * that is neither is not a projection block, whatever else it is. */
bool pointer_is_sane(uint32_t ptr) {
    if ((ptr & 0xF) != 0) return false;
    if (ptr >= RT_RAM_SIZE) return false;
    if (ptr + kBlockSpan > RT_RAM_SIZE) return false;
    return rt_gptr(ptr) != nullptr && rt_gptr(ptr + kBlockSpan - 1) != nullptr;
}

} // namespace

void rt_widescreen_set_factor(double k, const char* mode) {
    if (!mode) mode = "?";
    /* Widescreen writes one float in the game's own projection block, and
     * where that block is comes from guest/ico_syms.h. Unknown addresses
     * mean no write: the picture stays 4:3 rather than the runtime storing
     * a float somewhere it has not identified. */
    if (!RT_ICO_WIDESCREEN_KNOWN) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            rt_log_warn("widescreen", "display.widescreen is off: this build does not have the"
                " projection block's address (guest/ico_syms.h names what is missing, per"
                " target). The picture stays 4:3.");
        }
        return;
    }
    if (!std::isfinite(k) || k <= 0.0) {
        rt_log_warn("widescreen", "factor %.6f is not a finite positive number; leaving widescreen off",
            k);
        k = 1.0;
    }
    if (k == factor()) return;
    g_factor.store(k, std::memory_order_relaxed);
    ++g_generation;
    /* A new factor is a new question: give the skipped cases another chance
     * to report themselves. */
    g_logged_bad_pointer = false;
    g_logged_other_block = false;
    g_logged_bad_value = false;
    if (factor_is_off(k)) {
        rt_log_info("widescreen", "mode=%s k=%.4f (the retail 4:3 picture; nothing in guest memory"
            " is written)", mode, k);
        return;
    }
    /* k = (4/3) / aspect, so the aspect it came from is (4/3) / k. Two
     * lines, because the two halves of this setting are separate mechanisms
     * on separate sides of guest memory and a reader has to be able to tell
     * from the log which one produced what they are looking at. */
    rt_log_info("widescreen", "mode=%s aspect=%.4f k=%.4f (3D projection X scale at the composer entry)",
        mode, (4.0 / 3.0) / k, k);
    /* 2048 is the GS window centre the game's own XYOFFSET values are
     * placed around (2048 - width/2, 2048 - height/2), so it is the fixed
     * point hw/gif.cpp scales 2D X coordinates about. */
    rt_log_info("widescreen", "2D held at 4:3, scaled about x=2048 by %.4f; full-frame passes untouched",
        k);
}

double rt_widescreen_factor() { return factor(); }

double rt_widescreen_target_aspect() {
    const double k = factor();
    if (factor_is_off(k)) return 0.0;
    return (4.0 / 3.0) / k;
}

uint64_t rt_widescreen_generation() { return g_generation; }

uint64_t rt_widescreen_applied_count() { return g_applied; }

void rt_widescreen_on_composer_entry(R5900Context* ctx) {
    const double k = factor();
    if (!ctx || factor_is_off(k)) return;

    /* $8 (register 8): the block pointer the writer at 0x00114BD8 leaves
     * for the composer, and the pointer the composer itself reads the block
     * from. */
    const uint32_t ptr = ctx->r[8].u32x[0];

    if (!pointer_is_sane(ptr)) {
        if (!g_logged_bad_pointer) {
            rt_log_warn("widescreen",
                "the composer's block pointer ($8) is 0x%08x, which is not a quadword-aligned "
                "address with %u mapped bytes in EE RAM; skipping this call. Further calls with "
                "an unusable pointer are not logged.",
                ptr, (unsigned)kBlockSpan);
            g_logged_bad_pointer = true;
        }
        return;
    }

    if (ptr != RT_ICO_PROJ_BLOCK) {
        if (!g_logged_other_block) {
            rt_log_info("widescreen",
                "the composer was handed a block at 0x%08x, not RT_ICO_PROJ_BLOCK (0x%08x). "
                "Only the known block is scaled, so this call is skipped. This is a fact worth "
                "having, not a failure. Further calls with another block are not logged.",
                ptr, RT_ICO_PROJ_BLOCK);
            g_logged_other_block = true;
        }
        return;
    }

    const uint32_t addr = ptr + RT_ICO_PROJ_X_SCALE;
    float x = 0.0f;
    if (!read_f32(addr, &x)) return;

    const double x_min = x_scale_min(k);
    if (!std::isfinite(x) || x <= 0.0f || x > kXScaleMax || (double)x < x_min) {
        if (!g_logged_bad_value) {
            rt_log_warn("widescreen",
                "the X scale at 0x%08x (block 0x%08x + 0x%02X) is %.6f, which is outside the "
                "[%.6f, %.1f] a projection X scale can take at factor %.6f; skipping this call. "
                "A value under the floor means the scale has already been scaled once, which is "
                "this hook running twice against one block write. Further calls with an unusable "
                "value are not logged.",
                addr, ptr, RT_ICO_PROJ_X_SCALE, (double)x, x_min, (double)kXScaleMax, k);
            g_logged_bad_value = true;
        }
        return;
    }

    if (!write_f32(addr, (float)((double)x * k))) return;
    ++g_applied;
}
