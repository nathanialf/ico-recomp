/* hooks.cpp: privileged/control hooks called by generated code and the
 * reference interpreter (see recomp_api.h). Policy: log everything,
 * prefer loud failure over silent wrongness, per CLAUDE.md.
 *
 * rt_syscall lives in ee/syscalls.cpp (P2 kernel HLE); ei/di and the COP0
 * Status/Count reads route into ee/intc.cpp and the virtual clock.
 */
#include "runtime.h"

#include "ee/kernel.h"
#include "guest/ico_syms.h"
#include "guest/widescreen.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {

bool is_pow2(uint64_t v) { return v != 0 && (v & (v - 1)) == 0; }

bool strict_mode() {
    const char* v = std::getenv("ICORECOMP_STRICT");
    return v && std::strcmp(v, "1") == 0;
}

/* Generic per-key access counter for flood control (cop0/vu0 register polls). */
std::unordered_map<uint32_t, uint64_t> g_cop0_read_counts;
std::unordered_map<uint32_t, uint64_t> g_cop0_write_counts;
std::unordered_map<uint32_t, uint64_t> g_vu0_cfc_counts;

std::unordered_set<std::string> g_unimplemented_seen;

/* Entry hook addresses the generated code called that this file has no
 * dispatch for. One line each, because a config file and a runtime that
 * disagree is a bug and the log has to say which address. */
std::unordered_set<uint32_t> g_entry_hook_unknown;

bool g_ei_logged = false;
bool g_di_logged = false;

} // namespace

void rt_break(R5900Context* ctx, uint32_t code) {
    rt_fatal("break", ctx, "BREAK code=0x%x (%u)", code, code);
}

uint32_t rt_cop0_read(R5900Context* ctx, int reg) {
    uint32_t value = 0;
    switch (reg) {
        case 9:  /* Count: CPU clock is 2x BUSCLK */
            value = (uint32_t)(rt_clock_now() * 2);
            break;
        case 12: /* Status: EIE/IE per intc state */
            value = rt_cop0_status_word();
            break;
        default:
            break;
    }
    uint64_t& n = g_cop0_read_counts[uint32_t(reg)];
    ++n;
    if (is_pow2(n)) {
        rt_log_debug("cop0", "read $%d -> 0x%08x [access #%llu] pc_hint=0x%08x",
            reg, value, (unsigned long long)n, ctx->pc_hint);
    }
    return value;
}

void rt_cop0_write(R5900Context* ctx, int reg, uint32_t v) {
    (void)ctx;
    uint64_t& n = g_cop0_write_counts[uint32_t(reg)];
    ++n;
    /* Every write is discarded: this runtime holds no COP0 register file,
     * and the one piece of COP0 state it does model, the interrupt enable
     * in Status, is driven by rt_ei/rt_di instead. So a guest write to
     * Status (12), Cause (13) or any other register changes nothing here,
     * and that is a divergence, not a no-op. Warn the first time each
     * register is written, at the default level, then fall back to the
     * power-of-two debug line. Nothing in SCES_507.60 is known to do this;
     * if the warn ever appears, the register it names is the one to model. */
    if (n == 1) {
        rt_log_warn("cop0", "write $%d = 0x%08x DISCARDED: this runtime models no COP0 register "
            "file, and its interrupt-enable state comes from ei/di rather than from Status. "
            "Further writes to $%d are logged at debug on powers of two",
            reg, v, reg);
        return;
    }
    if (is_pow2(n)) {
        rt_log_debug("cop0", "write $%d = 0x%08x [access #%llu]", reg, v, (unsigned long long)n);
    }
}

void rt_ei(void) {
    if (!g_ei_logged) {
        rt_log_info("intc", "EI by thread %d (interrupts enabled for that thread; the state is per"
            " thread, see EEThread::eie) -- further EI calls not logged", rt_thread_current_id());
        g_ei_logged = true;
    }
    rt_intc_set_eie(true); /* delivers any pending interrupts */
}

void rt_di(void) {
    if (!g_di_logged) {
        rt_log_info("intc", "DI by thread %d (interrupts disabled for that thread only; every other"
            " thread and the scheduler keep taking them) -- further DI calls not logged",
            rt_thread_current_id());
        g_di_logged = true;
    }
    rt_intc_set_eie(false);
}

uint32_t rt_vu0_cfc(R5900Context* ctx, int creg) {
    (void)ctx;
    uint64_t& n = g_vu0_cfc_counts[uint32_t(creg)];
    ++n;
    /* Same shape as rt_cop0_write above: every VU0 control register reads
     * back as 0 because there is no VU0 control register file here, so the
     * first read of each register says so at the default level. */
    if (n == 1) {
        rt_log_warn("vu0", "cfc $%d -> 0: this runtime models no VU0 control registers, so the "
            "guest is told zero whatever the real one would hold. Further reads of $%d are "
            "logged at debug on powers of two", creg, creg);
        return 0;
    }
    if (is_pow2(n)) {
        rt_log_debug("vu0", "cfc $%d -> 0 [access #%llu]", creg, (unsigned long long)n);
    }
    return 0;
}

void rt_vu0_ctc(R5900Context* ctx, int creg, uint32_t v) {
    (void)ctx;
    rt_log_debug("vu0", "ctc $%d = 0x%08x", creg, v);
}

void rt_unimplemented(const char* what, uint32_t vram) {
    std::string key = std::string(what ? what : "(null)") + "@" + std::to_string(vram);
    bool first = g_unimplemented_seen.insert(key).second;
    if (first) {
        rt_log_warn("unimpl", "%s at vram=0x%08x", what ? what : "(null)", vram);
    }
    if (strict_mode()) {
        rt_fatal("unimpl", nullptr, "unimplemented op '%s' at vram=0x%08x (ICORECOMP_STRICT=1)", what ? what : "(null)", vram);
    }
}

/* A guest string for a log line: up to `cap` bytes at `addr`, stopped at
 * the first NUL, or a note when the address is unmapped. Never touches a
 * byte past the mapping. */
static void guest_cstr(uint32_t addr, char* out, size_t cap) {
    if (cap == 0) return;
    size_t n = 0;
    while (n + 1 < cap) {
        const uint8_t* p = rt_gptr(addr + (uint32_t)n);
        if (!p || *p == 0) break;
        out[n++] = (char)((*p >= 0x20 && *p < 0x7F) ? *p : '?');
    }
    out[n] = 0;
    if (n == 0 && !rt_gptr(addr)) std::snprintf(out, cap, "(unmapped 0x%08x)", addr);
}

void rt_entry_hook(R5900Context* ctx, uint32_t pc) {
    switch (pc) {
        case RT_ICO_MATRIX_COMPOSER:
            rt_widescreen_on_composer_entry(ctx);
            return;
        case RT_ICO_DEBUG_ASSERT: {
            /* debug_assertMessage(file, line, message): the game's assert
             * macro (decomp ios/cdvd func_00133250 at 0x00133324 is one
             * caller: sprintf into a stack buffer, then this, then
             * __assert). The retail body loops for ever, so the run is over
             * either way; this is the one place the reason can be said. */
            const uint32_t file = (uint32_t)ctx->r[4].u64x[0];
            const uint32_t line = (uint32_t)ctx->r[5].u64x[0];
            const uint32_t msg = (uint32_t)ctx->r[6].u64x[0];
            char fbuf[128], mbuf[512];
            guest_cstr(file, fbuf, sizeof(fbuf));
            guest_cstr(msg, mbuf, sizeof(mbuf));
            rt_log_error("guest", "the game asserted: %s:%u: %s (thread %d, ra=0x%08x)",
                fbuf, line, mbuf, rt_thread_current_id(), (uint32_t)ctx->r[31].u64x[0]);
            rt_fatal("guest", ctx, "guest assert at %s:%u: %s. debug_assertMessage (0x%08x) is a "
                "branch to itself in the retail build, so the game would hang here on the console",
                fbuf, line, mbuf, pc);
        }
        default:
            break;
    }
    /* Not fatal: the generated code is correct, it called what the config
     * file asked for. What is wrong is that this switch has no arm for it,
     * which is a mismatch between the target's entry-hook config and this
     * runtime. A fatal here would turn a stale config line into a dead
     * port. */
    if (g_entry_hook_unknown.insert(pc).second) {
        rt_log_warn("hooks",
            "rt_entry_hook called for 0x%08x, which config/entry_hooks.txt asks for but "
            "this runtime has no dispatch arm for. The hook does nothing for this "
            "address.",
            pc);
    }
}

/* An indirect call to an address the function table does not cover: the
 * guest jumped somewhere this translation has no code for, which is the end
 * of the run.
 *
 * Written through rt_log_error rather than fprintf(stderr), and ended
 * through rt_fatal rather than std::exit. stderr is the log file on a
 * packaged run, so the old form did reach the file, but it reached it
 * untagged, outside the level scheme, absent from the failure text a run
 * with no console shows in its message box, and with no end-of-run summary
 * behind it. A run that died here therefore ended with a block of text no
 * grep in docs/SETTINGS.md matches and no line saying the run was over. */
/* The function whose body covers `vram`, from the generated name table, or
 * nullptr when nothing does. The names are the disc listing's own, and the
 * provisional func_XXXXXXXX where the translator's correlation placed no
 * donor function. A stub build (no generated code linked) has no table and
 * gets no name. Declared in runtime.h: the thread inventory in ee/sched.cpp
 * names each guest thread's entry through it, so a blocked thread reads as a
 * function rather than as an address. It was called rt_decomp_func_name
 * until 2026-09-05, from when the names came from the decomp; they come
 * from the disc listing now, so the name says what the table is instead. */
const char* rt_guest_func_name(uint32_t vram, uint32_t* entry_out) {
#ifdef ICORECOMP_HAVE_GENERATED
    /* Outside the translated address range there is no containing function
     * to name, and the table's last entry would otherwise be reported for
     * any address above it. */
    if (vram < RECOMP_TEXT_BASE || vram >= RECOMP_TEXT_LIMIT) return nullptr;
    if (g_func_names_count == 0) return nullptr;
    /* Greatest entry <= vram. The table is sorted by vram. */
    unsigned lo = 0, hi = g_func_names_count;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (g_func_names[mid].vram <= vram) lo = mid + 1; else hi = mid;
    }
    if (lo == 0) return nullptr;
    if (entry_out) *entry_out = g_func_names[lo - 1].vram;
    return g_func_names[lo - 1].name;
#else
    (void)vram;
    (void)entry_out;
    return nullptr;
#endif
}

void rt_bad_indirect(uint32_t target, uint32_t caller_vram) {
    rt_log_error("functab", "bad indirect call: target=0x%08x caller=0x%08x", target, caller_vram);
    /* Which function each address is inside. The target's is the actionable
     * half: an address with no function of its own that sits inside one the
     * translator did establish is an entry no entry proof reached, which is
     * what the translator's report exists to collect. Rather than leave that
     * inference to whoever reads the log, say it. */
    uint32_t target_entry = 0, caller_entry = 0;
    const char* target_fn = rt_guest_func_name(target, &target_entry);
    const char* caller_fn = rt_guest_func_name(caller_vram, &caller_entry);
    if (caller_fn) {
        rt_log_error("functab", "  the call site 0x%08x is in %s (0x%08x + 0x%x)",
            caller_vram, caller_fn, caller_entry, caller_vram - caller_entry);
    }
    if (target_fn) {
        rt_log_error("functab", "  the target 0x%08x is inside %s (0x%08x + 0x%x), which the"
            " translator established as one function",
            target, target_fn, target_entry, target - target_entry);
        rt_log_error("functab", "  no entry proof reached 0x%08x, so it is either data or a"
            " function entry the translator's proofs miss; generated/ee/entry_gaps.txt lists"
            " what the ingest measured and ends with the unresolved-pointer sweep, which is"
            " where an address like this one belongs",
            target);
    }
    if (target < RECOMP_TEXT_BASE || target >= RECOMP_TEXT_LIMIT) {
        rt_log_error("functab", "  target 0x%08x is outside the function table's vram range [0x%08x, 0x%08x)",
            target, RECOMP_TEXT_BASE, RECOMP_TEXT_LIMIT);
    } else {
        uint32_t idx = RECOMP_FUNC_IDX(target);
        rt_log_error("functab", "  g_functab neighborhood (idx=%u, slot vram = base + idx*4):", idx);
        int32_t lo = int32_t(idx) - 4;
        int32_t hi = int32_t(idx) + 4;
        if (lo < 0) lo = 0;
        if (uint32_t(hi) >= RECOMP_FUNCTAB_SLOTS) hi = int32_t(RECOMP_FUNCTAB_SLOTS) - 1;
        for (int32_t i = lo; i <= hi; ++i) {
            uint32_t vram = RECOMP_TEXT_BASE + uint32_t(i) * 4;
            rt_log_error("functab", "    %s idx=%-8d vram=0x%08x functab=%p functab_orig=%p",
                (uint32_t(i) == idx) ? "->" : "  ", i, vram,
                (void*)g_functab[i], (void*)g_functab_orig[i]);
        }
    }
    /* rt_fatal for the register dump, the failure text, the end-of-run
     * summary and the message box, all of which this path used to skip. */
    rt_fatal("functab", rt_sched_current_ctx(),
        "bad indirect call: the guest called 0x%08x from 0x%08x and no translated function"
        " covers that address", target, caller_vram);
}

/* rt_xgkick and rt_vu1_register moved to hw/vu1rt.cpp (P3): they now parse
 * GIF packets onto PATH1 and feed the microprogram registry. */
