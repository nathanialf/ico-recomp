/* recomp_api.h: the runtime surface generated code links against, plus the
 * guest memory access layer. This header is the translator/runtime contract.
 *
 * Memory model: software page table with 64 KB pages. The runtime populates
 * g_pages at boot: 32 MB EE RAM at guest 0x00000000 with aliases at the
 * 0x2/0x3 (uncached) and 0x8/0xA (kseg) prefixes, scratchpad at 0x70000000,
 * VU micro/data memory windows at 0x11000000. Pages left NULL (0x10xxxxxx
 * device registers, 0x12xxxxxx GS privileged) fall through to rt_mmio_*,
 * which is fatal with a state dump for any other NULL-page address
 * (mmio.cpp; the two windows above are the only ones this title touches).
 */
#ifndef RECOMP_API_H
#define RECOMP_API_H

#include "recomp_context.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- function dispatch ------------------------------------------------- */

typedef void (*recomp_fn_t)(R5900Context* ctx);

#define RECOMP_TEXT_BASE 0x00100000u
/* Rounded past the boot ELF's .text + .vutext end. Measured on
 * SCES_507.60: .text is 0x00100000..0x00289BC4 and .vutext is
 * 0x00289BD0..0x0028ECB0, so the table has to cover 0x0028ECB0. The value
 * was 0x00280000 while this port targeted SCUS_971.13, whose .vutext ends
 * at 0x002746C0; on the PAL ELF that limit is 100 functions short of the
 * end of .text, and since the generated funcs_table.c indexes
 * g_functab_orig by RECOMP_FUNC_IDX without a bound test, those 100 were
 * writes past the end of the array rather than functions the dispatch
 * merely refused. The guard is at build time: the translator writes a
 * #error into generated/ee/funcs_table.c keyed on the highest translated
 * entry, so a window below it fails the compile. src/runtime/loader.cpp
 * logs the window a built binary actually carries. */
#define RECOMP_TEXT_LIMIT 0x00290000u
#define RECOMP_FUNCTAB_SLOTS ((RECOMP_TEXT_LIMIT - RECOMP_TEXT_BASE) >> 2)
#define RECOMP_FUNC_IDX(vram) (((vram) - RECOMP_TEXT_BASE) >> 2)

/* Slot = current implementation (generated, HLE override, or interp thunk).
 * g_functab_orig keeps the generated implementation so overrides can chain. */
extern recomp_fn_t g_functab[RECOMP_FUNCTAB_SLOTS];
extern recomp_fn_t g_functab_orig[RECOMP_FUNCTAB_SLOTS];

/* The name of every translated function, sorted by vram, and defined by the
 * generated funcs_table.c. The names come from the objdump listing the disc
 * carries, correlated onto this build by the translator's ingest, and are
 * the provisional func_XXXXXXXX where the correlation placed no donor
 * function. The table exists for one message: when an indirect call lands on
 * an address no translated function covers, the runtime names the function
 * whose body the address is inside. It is not a dispatch path and nothing on
 * a hot path reads it.
 *
 * Only a build that links generated code defines these
 * (ICORECOMP_HAVE_GENERATED); a stub build must not reference them. */
typedef struct {
    uint32_t vram;
    const char* name;
} recomp_func_name_t;
extern const recomp_func_name_t g_func_names[];
extern const unsigned g_func_names_count;

void       rt_override(uint32_t vram, recomp_fn_t fn);
recomp_fn_t rt_original(uint32_t vram);

/* Indirect-call helper used at jalr sites and function-pointer dispatch. */
void rt_call_indirect(R5900Context* ctx, uint32_t target, uint32_t caller_vram);

/* ---- memory ------------------------------------------------------------ */

extern uint8_t* g_pages[0x10000]; /* host pointer per 64 KB guest page; NULL => MMIO */

uint8_t  rt_mmio_read8(uint32_t addr);
uint16_t rt_mmio_read16(uint32_t addr);
uint32_t rt_mmio_read32(uint32_t addr);
uint64_t rt_mmio_read64(uint32_t addr);
rc_u128  rt_mmio_read128(uint32_t addr);
void rt_mmio_write8(uint32_t addr, uint8_t v);
void rt_mmio_write16(uint32_t addr, uint16_t v);
void rt_mmio_write32(uint32_t addr, uint32_t v);
void rt_mmio_write64(uint32_t addr, uint64_t v);
void rt_mmio_write128(uint32_t addr, rc_u128 v);

/* ---- control / privileged hooks ---------------------------------------- */

void rt_syscall(R5900Context* ctx);                 /* number in ctx->r[3] ($v1) */
void rt_break(R5900Context* ctx, uint32_t code);    /* default: fatal log (div guards are unreachable) */
uint32_t rt_cop0_read(R5900Context* ctx, int reg);
void rt_cop0_write(R5900Context* ctx, int reg, uint32_t v);
void rt_ei(void);
void rt_di(void);
uint32_t rt_vu0_cfc(R5900Context* ctx, int creg);   /* vi28/vi29 (FBRST/CMSAR0) only */
void rt_vu0_ctc(R5900Context* ctx, int creg, uint32_t v);
void rt_unimplemented(const char* what, uint32_t vram);

/* Entry hook. Generated code calls this as the first statement of the body
 * of every function whose entry address is listed in config/entry_hooks.txt,
 * before that function's first translated instruction, with the function's
 * own entry vram in `pc`. Nothing else calls it: the reference interpreter
 * (tools/recomp/crates/ee-interp) executes one instruction at a time and has
 * no notion of a function entry, so there is no second call site to keep in
 * step, and the per-op three-way tests compare single instructions and never
 * run an emitted function body.
 *
 * The runtime dispatches on `pc` (src/runtime/hooks.cpp). A `pc` it does not
 * know is a mismatch between the config file and the runtime, which is a
 * bug: it logs loudly and returns. */
void rt_entry_hook(R5900Context* ctx, uint32_t pc);
void rt_bad_indirect(uint32_t target, uint32_t caller_vram);

/* Called by generated code on taken backward branches (target vram <= branch
 * vram), counter-gated cheap. Gives RAM-only spin loops a periodic runtime
 * trap point: every Nth call advances the virtual clock and runs the same
 * pending-delivery path MMIO reads use. */
void rt_backedge(void);

/* ---- VU1 --------------------------------------------------------------- */

/* XGKICK: qw_addr is a quadword address into vu->mem; the runtime parses the
 * GIF packet (handling 16 KB wrap) and forwards it to the GS on PATH1.
 * Semantically synchronous: the packet is fully consumed before return. */
void rt_xgkick(Vu1State* vu, uint32_t qw_addr);

/* Generated init code registers each recompiled microprogram by the hash of
 * its instruction bytes; the VIF1 HLE resolves uploads to entries via hash. */
void rt_vu1_register(uint32_t hash, uint32_t size_bytes, void (*entry)(Vu1State* vu));

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_API_H */
