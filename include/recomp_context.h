/* recomp_context.h: guest CPU/VU state ABI shared by generated code, the
 * reference interpreter, and the runtime. This header is the contract.
 * Changes here require regenerating all translated code.
 *
 * C11, compiles as C and C++ under gcc/clang/clang-cl.
 */
#ifndef RECOMP_CONTEXT_H
#define RECOMP_CONTEXT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
#define RC_ALIGN(n) __declspec(align(n))
#else
#define RC_ALIGN(n) __attribute__((aligned(n)))
#endif

typedef union RC_ALIGN(16) rc_u128 {
    uint8_t  u8x[16];
    uint16_t u16x[8];
    uint32_t u32x[4];
    uint64_t u64x[2];
    int8_t   s8x[16];
    int16_t  s16x[8];
    int32_t  s32x[4];
    int64_t  s64x[2];
    float    f32x[4];
} rc_u128;

/* VU0 macro-mode (COP2) state, embedded in R5900Context because macro mode
 * executes synchronously with the EE pipeline. vf[0] holds the (0,0,0,1)
 * invariant: the emitter suppresses writes to it and may read it as constants.
 * vi[0] == 0 always. */
typedef struct RC_ALIGN(16) Vu0State {
    rc_u128  vf[32];
    uint16_t vi[16];
    rc_u128  acc;
    float    q;        /* macro-mode vdiv/vsqrt/vrsqrt complete immediately */
    float    p;        /* unused by ICO; kept for completeness */
    uint32_t r;        /* R register raw float bits (vrinit/vrnext/vrxor/vrget) */
    float    i;        /* I register */
    uint32_t status;   /* sticky status flags */
    uint32_t mac;      /* per-lane sign/zero of last FMAC op */
    uint32_t clip;     /* 24-bit shifting clip judgment (vclipw) */
} Vu0State;

/* VU1 state for statically recompiled microprograms. mem is VU1 data memory
 * (16 KB); the VIF1 HLE UNPACKs into it and sets xtop/itop before MSCAL.
 * pc is the entry address in *bytes* into micro memory (MSCAL target * 8). */
typedef struct RC_ALIGN(16) Vu1State {
    rc_u128  vf[32];
    uint16_t vi[16];
    rc_u128  acc;
    float    q;          /* committed Q */
    float    pending_q;  /* Q pipeline: set by div/sqrt/rsqrt, committed on wait/read point */
    uint32_t q_pending;  /* nonzero while pending_q not yet committed */
    uint32_t r;
    float    i;
    uint32_t status;
    uint32_t mac;
    uint32_t clip;
    uint32_t xtop;       /* quadword address given by VIF1 TOPS at MSCAL */
    uint32_t itop;
    uint32_t pc;         /* entry offset in bytes into micro mem; set by MSCAL/MSCNT */
    uint8_t  mem[16384]; /* VU1 data memory. The struct is 16-byte aligned but
                          * this member sits at offset 604, so mem itself is
                          * only 4-byte aligned; use unaligned loads/stores. */
} Vu1State;

/* Full EE guest context. One per guest thread. Generated code receives a
 * pointer to this as its only parameter. */
typedef struct RC_ALIGN(16) R5900Context {
    rc_u128  r[32];     /* GPRs; r[0] never written (emitter suppresses) */
    rc_u128  lo, hi;    /* 128-bit: pipeline 0 result in u64x[0], pipeline 1 (mult1/div1) in u64x[1] */
    uint32_t sa;        /* shift-amount register, byte-granular (mtsab/mtsa/mfsa/qfsrv) */
    uint32_t fcr31;     /* COP1 control; bit 23 (condition 'C') is the load-bearing bit */
    float    f[32];     /* COP1 registers */
    Vu0State vu0;
    uint32_t pc_hint;   /* debugging only; not maintained on the fast path */
    void*    host;      /* runtime per-thread data (scheduler bookkeeping); opaque to generated code */
} R5900Context;

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_CONTEXT_H */
