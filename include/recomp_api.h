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
#define RECOMP_TEXT_LIMIT 0x00280000u /* rounded past .text+.vutext end */
#define RECOMP_FUNCTAB_SLOTS ((RECOMP_TEXT_LIMIT - RECOMP_TEXT_BASE) >> 2)
#define RECOMP_FUNC_IDX(vram) (((vram) - RECOMP_TEXT_BASE) >> 2)

/* Slot = current implementation (generated, HLE override, or interp thunk).
 * g_functab_orig keeps the generated implementation so overrides can chain. */
extern recomp_fn_t g_functab[RECOMP_FUNCTAB_SLOTS];
extern recomp_fn_t g_functab_orig[RECOMP_FUNCTAB_SLOTS];

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
