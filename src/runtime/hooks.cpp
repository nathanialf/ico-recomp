/* hooks.cpp: privileged/control hooks called by generated code and the
 * reference interpreter (see recomp_api.h). Policy: log everything,
 * prefer loud failure over silent wrongness, per CLAUDE.md.
 *
 * rt_syscall lives in ee/syscalls.cpp (P2 kernel HLE); ei/di and the COP0
 * Status/Count reads route into ee/intc.cpp and the virtual clock.
 */
#include "runtime.h"

#include "ee/kernel.h"

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
        rt_log("cop0", "read $%d -> 0x%08x [access #%llu] pc_hint=0x%08x",
            reg, value, (unsigned long long)n, ctx->pc_hint);
    }
    return value;
}

void rt_cop0_write(R5900Context* ctx, int reg, uint32_t v) {
    (void)ctx;
    uint64_t& n = g_cop0_write_counts[uint32_t(reg)];
    ++n;
    if (is_pow2(n)) {
        rt_log("cop0", "write $%d = 0x%08x [access #%llu]", reg, v, (unsigned long long)n);
    }
}

void rt_ei(void) {
    if (!g_ei_logged) {
        rt_log("intc", "EI (interrupts enabled) -- further EI calls not logged");
        g_ei_logged = true;
    }
    rt_intc_set_eie(true); /* delivers any pending interrupts */
}

void rt_di(void) {
    if (!g_di_logged) {
        rt_log("intc", "DI (interrupts disabled) -- further DI calls not logged");
        g_di_logged = true;
    }
    rt_intc_set_eie(false);
}

uint32_t rt_vu0_cfc(R5900Context* ctx, int creg) {
    (void)ctx;
    uint64_t& n = g_vu0_cfc_counts[uint32_t(creg)];
    ++n;
    if (is_pow2(n)) {
        rt_log("vu0", "cfc $%d -> 0 [access #%llu]", creg, (unsigned long long)n);
    }
    return 0;
}

void rt_vu0_ctc(R5900Context* ctx, int creg, uint32_t v) {
    (void)ctx;
    rt_log("vu0", "ctc $%d = 0x%08x", creg, v);
}

void rt_unimplemented(const char* what, uint32_t vram) {
    std::string key = std::string(what ? what : "(null)") + "@" + std::to_string(vram);
    bool first = g_unimplemented_seen.insert(key).second;
    if (first) {
        rt_log("unimpl", "%s at vram=0x%08x", what ? what : "(null)", vram);
    }
    if (strict_mode()) {
        rt_fatal("unimpl", nullptr, "unimplemented op '%s' at vram=0x%08x (ICORECOMP_STRICT=1)", what ? what : "(null)", vram);
    }
}

void rt_bad_indirect(uint32_t target, uint32_t caller_vram) {
    std::fprintf(stderr, "[icorecomp][functab] FATAL: bad indirect call: target=0x%08x caller=0x%08x\n", target, caller_vram);
    if (target < RECOMP_TEXT_BASE || target >= RECOMP_TEXT_LIMIT) {
        std::fprintf(stderr, "  target 0x%08x is outside the function table's vram range [0x%08x, 0x%08x)\n",
            target, RECOMP_TEXT_BASE, RECOMP_TEXT_LIMIT);
    } else {
        uint32_t idx = RECOMP_FUNC_IDX(target);
        std::fprintf(stderr, "  g_functab neighborhood (idx=%u, slot vram = base + idx*4):\n", idx);
        int32_t lo = int32_t(idx) - 4;
        int32_t hi = int32_t(idx) + 4;
        if (lo < 0) lo = 0;
        if (uint32_t(hi) >= RECOMP_FUNCTAB_SLOTS) hi = int32_t(RECOMP_FUNCTAB_SLOTS) - 1;
        for (int32_t i = lo; i <= hi; ++i) {
            uint32_t vram = RECOMP_TEXT_BASE + uint32_t(i) * 4;
            std::fprintf(stderr, "    %s idx=%-8d vram=0x%08x functab=%p functab_orig=%p\n",
                (uint32_t(i) == idx) ? "->" : "  ", i, vram,
                (void*)g_functab[i], (void*)g_functab_orig[i]);
        }
    }
    std::fflush(stderr);
    std::exit(1);
}

void rt_xgkick(Vu1State* vu, uint32_t qw_addr) {
    rt_log("vu1", "xgkick stub: vu=%p qw_addr=0x%x (GIF packet not parsed in P1)", (void*)vu, qw_addr);
}

void rt_vu1_register(uint32_t hash, uint32_t size_bytes, void (*entry)(Vu1State* vu)) {
    rt_log("vu1", "register stub: hash=0x%08x size=%u entry=%p (not dispatched in P1)", hash, size_bytes, (void*)entry);
}
