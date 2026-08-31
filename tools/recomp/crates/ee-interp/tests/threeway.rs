//! Three-way verification: for every covered straight-line mnemonic,
//! generate edge-case operand vectors and run them through
//!   (a) the reference interpreter (ee-interp, recomp_ops.h via the shim),
//!   (b) a compiled-and-executed emitted snippet (ee-emit's C, built with
//!       gcc into a shared object and dlopen'd),
//! then compare the full R5900Context and guest RAM bit-exactly.
//!
//! ---------------------------------------------------------------------
//! UNCERTAIN-SEMANTICS TODO REGISTRY
//! Decisions below are deterministic and shared by both sides, but PS2
//! hardware behavior is unverified. Calibrate against PCSX2 traces or
//! hardware later; do not silently change recomp_ops.h without rerunning
//! this suite.
//!  1. Rounding: EE FPU / VU FMAC round toward zero on hardware; tier 0
//!     uses host round-to-nearest for add/sub/mul/div/madd.
//!  2. madd/msub: hardware truncates the product before the add; we do two
//!     host roundings (FP contraction disabled everywhere).
//!  3. Exponent-255 bit patterns (host NaN/Inf) entering via loads:
//!     arithmetic inputs read them as +/-FMAX (rc_fin/rc_vu_in); PS2
//!     treats them as even larger finite values (up to ~2^129), so
//!     magnitudes differ. COP1 compares stay plain host compares and
//!     answer false against NaN patterns; PS2 would compare them as huge
//!     finite values.
//!  4. Div-by-zero result: COP1 uses +/-0x7FFFFFFF (documented), VU div
//!     uses +/-FMAX (0x7F7FFFFF) per PCSX2 behavioral reference.
//!  5. VU 0/0 (div and rsqrt) sets I not D, x/0 sets D, Q gets the xor
//!     sign; follows PCSX2.
//!  6. vsqrt: negative input sets I and computes sqrt(|x|); -0 sets no I.
//!  7. ctc2 STATUS: only sticky bits (0xFC0) writable, non-sticky kept.
//!  8. ctc2 to MAC(17)/CLIP(18): treated read-only, routed to rt_vu0_ctc
//!     so a real write fails loudly instead of being dropped.
//!  9. R register held as 0x3F800000 | 23 bits for both ctc2 and cfc2.
//! 10. vmax/vmini: sign-magnitude integer compare (PCSX2 fp_max/fp_min);
//!     -0 vs +0 ordering and NaN patterns unverified.
//! 11. vftoi of NaN/Inf saturates by the input sign bit.
//! 12. vitof of |int| > 2^24 rounds to nearest on host; hardware truncates.
//! 13. R LFSR: taps at bits 4 and 22, shift left (manual reading + PCSX2).
//! 14. -0.0 FMAC results set both Z and S mac bits.
//! 15. Denormal results flush to signed zero and set U (and Z) flags.
//! 16. cfc2 sign-extends bit-31-set values (raw Q/I float bits) to 64.
//! 17. Macro vlqi/vlqd/vsqi address VU0 data RAM through the 0x11004000
//!     window, vi masked to 0xFF quadwords (4 KB wrap).
//! 18. Status non-sticky I/D bits are rewritten by every div-unit op and
//!     untouched by FMAC ops.
//! ---------------------------------------------------------------------

use std::fmt::Write as _;
use std::os::raw::{c_char, c_void};
#[cfg(unix)]
use std::os::raw::c_int;
use std::path::{Path, PathBuf};
use std::process::Command;

use ee_interp::{set_page, step, Ctx};
use r5900_decode::{decode, Insn};

const TEST_VRAM: u32 = 0x0010_0000;
const RAM_BASE: u32 = 0x8000; // inside guest page 0
const VU0_PAGE: u32 = 0x1100; // 0x11004000 >> 16

type Setup = Box<dyn Fn(&mut Ctx)>;

struct Case {
    word: u32,
    name: String,
    setups: Vec<Setup>,
}

fn mk(word: u32, setups: Vec<Setup>) -> Case {
    let insn = decode(word, TEST_VRAM);
    let name = insn
        .mnemonic()
        .unwrap_or_else(|| panic!("word {word:08X} decodes invalid"))
        .to_string();
    Case { word, name, setups }
}

// ---- value pools ---------------------------------------------------------

const U64P: [u64; 16] = [
    0,
    1,
    2,
    0x7F,
    0x80,
    0xFF,
    0x7FFF,
    0x8000,
    0x7FFF_FFFF,
    0x8000_0000,
    0xFFFF_FFFF,
    0x1_0000_0000,
    0x7FFF_FFFF_FFFF_FFFF,
    0x8000_0000_0000_0000,
    0xFFFF_FFFF_FFFF_FFFF,
    0x1234_5678_9ABC_DEF0,
];

const F32P: [u32; 20] = [
    0x0000_0000, // +0
    0x8000_0000, // -0
    0x0000_0001, // min denormal
    0x8000_0001,
    0x007F_FFFF, // max denormal
    0x0080_0000, // min normal
    0x3F80_0000, // 1.0
    0xBF80_0000, // -1.0
    0x3F00_0000, // 0.5
    0x4049_0FDB, // pi
    0x7F7F_FFFF, // +FMAX
    0xFF7F_FFFF, // -FMAX
    0x7F80_0000, // +Inf pattern
    0xFF80_0000, // -Inf pattern
    0x7FC0_0000, // qNaN
    0xFFC0_0000, // -qNaN
    0x7F80_0001, // sNaN pattern
    0x4F00_0000, // 2^31
    0xCF00_0000, // -2^31
    0x4B7F_FFFF, // just under 2^24
];

const I32P: [i32; 10] = [
    0,
    1,
    -1,
    16,
    -16,
    i32::MAX,
    i32::MIN,
    0x7FFF_FFF0,
    123_456_789,
    -123_456_789,
];

// ---- word encoders -------------------------------------------------------

fn rt3(op: u32, rs: u32, rt: u32, imm: u32) -> u32 {
    (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)
}
fn sp(rs: u32, rt: u32, rd: u32, sa: u32, funct: u32) -> u32 {
    (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct
}
fn mmi(rs: u32, rt: u32, rd: u32, sa: u32, funct: u32) -> u32 {
    (0x1C << 26) | (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct
}
fn cop1s(ft: u32, fs: u32, fd: u32, funct: u32) -> u32 {
    (0x11 << 26) | (0x10 << 21) | (ft << 16) | (fs << 11) | (fd << 6) | funct
}
/// COP2 special (bit 25 set).
fn c2(dest: u32, ft: u32, fs: u32, fd: u32, funct: u32) -> u32 {
    (0x12 << 26) | (1 << 25) | (dest << 21) | (ft << 16) | (fs << 11) | (fd << 6) | funct
}
/// COP2 transfer (qmfc2 1, cfc2 2, qmtc2 5, ctc2 6), .ni form.
fn c2xfer(sub: u32, rt: u32, rd: u32) -> u32 {
    (0x12 << 26) | (sub << 21) | (rt << 16) | (rd << 11)
}

// ---- setup builders ------------------------------------------------------

fn pairs64(f: impl Fn(&mut Ctx, u64, u64) + Copy + 'static) -> Vec<Setup> {
    let mut v: Vec<Setup> = Vec::new();
    for &a in &U64P {
        for &b in &U64P {
            v.push(Box::new(move |c: &mut Ctx| f(c, a, b)));
        }
    }
    v
}

fn singles64(f: impl Fn(&mut Ctx, u64) + Copy + 'static) -> Vec<Setup> {
    U64P
        .iter()
        .map(|&a| Box::new(move |c: &mut Ctx| f(c, a)) as Setup)
        .collect()
}

fn pairs_f32(f: impl Fn(&mut Ctx, u32, u32) + Copy + 'static) -> Vec<Setup> {
    let mut v: Vec<Setup> = Vec::new();
    for &a in &F32P {
        for &b in &F32P {
            v.push(Box::new(move |c: &mut Ctx| f(c, a, b)));
        }
    }
    v
}

/// Rotating quadword seeds for the VU register file.
fn vu_seed(c: &mut Ctx, i: usize) {
    let n = F32P.len();
    let lane = |k: usize| F32P[(i + k) % n];
    c.set_vf(4, [lane(0), lane(1), lane(2), lane(3)]);
    c.set_vf(6, [lane(5), lane(6), lane(7), lane(8)]);
    c.set_vf(2, [lane(9), lane(10), lane(11), lane(12)]);
    c.set_acc([lane(13), lane(14), lane(15), lane(16)]);
    c.set_vu_q_bits(lane(4));
    c.set_vu_i_bits(lane(17));
    // Vary the pre-existing flag state so sticky behavior is exercised.
    c.set_vu_status(match i % 3 {
        0 => 0,
        1 => 0xFC0,
        _ => 0x030,
    });
    c.set_vu_mac(0xA5A5 & 0xFFFF);
    c.set_vu_clip(0x00AB_CDEF & 0xFF_FFFF);
}

fn vu_setups() -> Vec<Setup> {
    (0..12)
        .map(|i| Box::new(move |c: &mut Ctx| vu_seed(c, i)) as Setup)
        .collect()
}

// ---- case groups ---------------------------------------------------------

fn int_cases(cases: &mut Vec<Case>) {
    // Three-register ALU (rd=3, rs=4, rt=5) plus alias and $zero forms.
    for funct in [0x21u32, 0x23, 0x2D, 0x2F, 0x24, 0x25, 0x26, 0x27, 0x2A, 0x2B, 0x0A, 0x0B] {
        for (rs, rt, rd) in [(4u32, 5u32, 3u32), (4, 5, 4), (0, 5, 3), (4, 5, 0)] {
            cases.push(mk(
                sp(rs, rt, rd, 0, funct),
                pairs64(|c, a, b| {
                    c.set_r64(4, a);
                    c.set_r64(5, b);
                    c.set_r64(3, 0xDEAD_BEEF_0BAD_F00D);
                }),
            ));
        }
    }
    // Immediates.
    for op in [0x08u32, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x19] {
        for imm in [0x8000u32, 0xFFFF, 0, 1, 0x7FFF] {
            for (rs, rt) in [(4u32, 3u32), (4, 4), (0, 3)] {
                cases.push(mk(
                    rt3(op, rs, rt, imm),
                    singles64(|c, a| {
                        c.set_r64(4, a);
                        c.set_r64(3, 0x0123_4567_89AB_CDEF);
                    }),
                ));
            }
        }
    }
    // lui.
    for imm in [0u32, 1, 0x8000, 0xFFFF] {
        cases.push(mk(rt3(0x0F, 0, 3, imm), vec![Box::new(|_c| {})]));
    }
    // Shift immediates.
    for funct in [0x00u32, 0x02, 0x03, 0x38, 0x3A, 0x3B, 0x3C, 0x3E, 0x3F] {
        for sa in [0u32, 1, 15, 31] {
            if funct == 0 && sa == 0 {
                continue; // nop; covered separately
            }
            cases.push(mk(
                sp(0, 5, 3, sa, funct),
                singles64(|c, a| {
                    c.set_r64(5, a);
                    c.set_r64(3, 0x1111_2222_3333_4444);
                }),
            ));
        }
    }
    cases.push(mk(0, vec![Box::new(|_c| {})])); // nop
    // Variable shifts (rd=3, rt=5, rs=4 holds the amount).
    for funct in [0x04u32, 0x06, 0x07, 0x14, 0x16, 0x17] {
        cases.push(mk(
            sp(4, 5, 3, 0, funct),
            pairs64(|c, a, b| {
                c.set_r64(4, a);
                c.set_r64(5, b);
            }),
        ));
    }
}

fn mem_cases(cases: &mut Vec<Case>) {
    let base_setup = |extra: u64| {
        singles64(move |c: &mut Ctx, a| {
            c.set_r64(4, RAM_BASE as u64 + extra);
            c.set_r64(5, a);
        })
    };
    // Aligned-width loads and stores at assorted offsets and misalignments.
    for (op, align) in [
        (0x20u32, 1u32), // lb
        (0x24, 1),       // lbu
        (0x21, 2),       // lh
        (0x25, 2),       // lhu
        (0x23, 4),       // lw
        (0x27, 4),       // lwu
        (0x37, 8),       // ld
        (0x1E, 16),      // lq
        (0x28, 1),       // sb
        (0x29, 2),       // sh
        (0x2B, 4),       // sw
        (0x3F, 8),       // sd
        (0x1F, 16),      // sq
    ] {
        for off in [0u32, 8, 0xFFF8, 0xFFF0] {
            for mis in 0..align.min(4) as u64 {
                // rt = 5 (also the store source), plus a load-to-$zero form.
                cases.push(mk(rt3(op, 4, 5, off), base_setup(mis)));
            }
        }
        if op < 0x28 || op == 0x1E {
            cases.push(mk(rt3(op, 4, 0, 4), base_setup(0)));
        }
    }
    // Unaligned pair ops across every byte offset.
    for op in [0x22u32, 0x26, 0x1A, 0x1B, 0x2A, 0x2E, 0x2C, 0x2D] {
        for off in 0u32..8 {
            cases.push(mk(rt3(op, 4, 5, off), base_setup(0)));
        }
    }
}

fn muldiv_cases(cases: &mut Vec<Case>) {
    let seed = |c: &mut Ctx, a: u64, b: u64| {
        c.set_r64(4, a);
        c.set_r64(5, b);
        c.set_lohi(false, 0, 0x1111_2222_3333_4444);
        c.set_lohi(true, 0, 0x5555_6666_7777_8888);
        c.set_lohi(false, 1, 0x9999_AAAA_BBBB_CCCC);
        c.set_lohi(true, 1, 0xDDDD_EEEE_FFFF_0001);
    };
    // mult/madd with and without rd, both pipelines.
    for (word, _) in [
        (sp(4, 5, 3, 0, 0x18), "mult"),
        (sp(4, 5, 0, 0, 0x18), "mult0"),
        (mmi(4, 5, 3, 0, 0x18), "mult1"),
        (mmi(4, 5, 3, 0, 0x00), "madd"),
        (mmi(4, 5, 0, 0, 0x00), "madd0"),
        (mmi(4, 5, 3, 0, 0x20), "madd1"),
        (sp(4, 5, 0, 0, 0x19), "multu"),
        (sp(4, 5, 0, 0, 0x1A), "div"),
        (mmi(4, 5, 0, 0, 0x1A), "div1"),
        (sp(4, 5, 0, 0, 0x1B), "divu"),
    ] {
        cases.push(mk(word, pairs64(seed)));
    }
    // mfhi/mflo/mthi/mtlo, both pipelines.
    for word in [
        sp(0, 0, 3, 0, 0x10),
        sp(0, 0, 3, 0, 0x12),
        sp(4, 0, 0, 0, 0x11),
        sp(4, 0, 0, 0, 0x13),
        mmi(0, 0, 3, 0, 0x10),
        mmi(0, 0, 3, 0, 0x12),
        mmi(4, 0, 0, 0, 0x11),
        mmi(4, 0, 0, 0, 0x13),
    ] {
        cases.push(mk(word, pairs64(seed)));
    }
    // SA register and qfsrv.
    cases.push(mk(sp(0, 0, 3, 0, 0x28), singles64(|c, a| c.set_sa(a as u32 & 31)))); // mfsa
    cases.push(mk(sp(4, 0, 0, 0, 0x29), singles64(|c, a| c.set_r64(4, a)))); // mtsa
    for imm in [0u32, 1, 7, 15, 0xFFFF] {
        cases.push(mk(
            (0x01 << 26) | (4 << 21) | (0x18 << 16) | imm,
            singles64(|c, a| c.set_r64(4, a)),
        )); // mtsab
    }
    for sa in [0u32, 1, 7, 15, 16, 31] {
        for rd in [3u32, 0] {
            cases.push(mk(
                mmi(4, 5, rd, 0x1B, 0x28), // qfsrv
                pairs64(move |c, a, b| {
                    c.set_r128(4, splat128(a));
                    c.set_r128(5, splat128(b));
                    c.set_sa(sa);
                }),
            ));
        }
    }
}

fn splat128(a: u64) -> [u8; 16] {
    let mut b = [0u8; 16];
    b[0..8].copy_from_slice(&a.to_le_bytes());
    b[8..16].copy_from_slice(&a.wrapping_mul(0x9E37_79B9_7F4A_7C15).to_le_bytes());
    b
}

fn mmi_cases(cases: &mut Vec<Case>) {
    let seed = |c: &mut Ctx, a: u64, b: u64| {
        c.set_r128(4, splat128(a));
        c.set_r128(5, splat128(b));
        c.set_r128(3, splat128(0xC0DE_C0DE_C0DE_C0DE));
    };
    let words = [
        mmi(4, 5, 3, 0x04, 0x08), // paddh (mmi0 sa 0x04)
        mmi(4, 5, 3, 0x09, 0x08), // psubb
        mmi(4, 5, 3, 0x01, 0x08), // psubw
        mmi(4, 5, 3, 0x06, 0x08), // pcgth
        mmi(4, 5, 3, 0x07, 0x08), // pmaxh
        mmi(4, 5, 3, 0x07, 0x28), // pminh (mmi1)
        mmi(4, 5, 3, 0x1A, 0x08), // pextlb
        mmi(4, 5, 3, 0x1A, 0x28), // pextub
        mmi(4, 5, 3, 0x12, 0x08), // pextlw
        mmi(4, 5, 3, 0x12, 0x28), // pextuw
        mmi(4, 5, 3, 0x0E, 0x09), // pcpyld (mmi2)
        mmi(4, 5, 3, 0x0E, 0x29), // pcpyud (mmi3)
        mmi(4, 5, 3, 0x1B, 0x08), // ppacb
        mmi(4, 5, 3, 0x12, 0x09), // pand
        mmi(4, 5, 3, 0x12, 0x29), // por
        mmi(4, 5, 3, 0x13, 0x09), // pxor
        mmi(4, 5, 3, 0x13, 0x29), // pnor
        mmi(0, 5, 3, 0x1B, 0x29), // pcpyh
    ];
    for w in words {
        cases.push(mk(w, pairs64(seed)));
        // rd = $zero form: fully suppressed.
        cases.push(mk(w & !(31 << 11), pairs64(seed)));
    }
    for funct in [0x34u32, 0x36, 0x37] {
        for sa in [0u32, 1, 15] {
            cases.push(mk(mmi(0, 5, 3, sa, funct), pairs64(seed))); // psllh/psrlh/psrah
        }
    }
}

fn cop1_cases(cases: &mut Vec<Case>) {
    let fseed = |c: &mut Ctx, a: u32, b: u32| {
        c.set_f_bits(4, a);
        c.set_f_bits(6, b);
        c.set_f_bits(2, 0x1234_5678);
    };
    // lwc1/swc1.
    for off in [0u32, 4, 0xFFFC] {
        cases.push(mk(
            rt3(0x31, 4, 6, off),
            singles64(|c, a| {
                c.set_r64(4, RAM_BASE as u64);
                c.set_f_bits(6, a as u32);
            }),
        ));
        cases.push(mk(
            rt3(0x39, 4, 6, off),
            F32P
                .iter()
                .map(|&v| {
                    Box::new(move |c: &mut Ctx| {
                        c.set_r64(4, RAM_BASE as u64);
                        c.set_f_bits(6, v);
                    }) as Setup
                })
                .collect(),
        ));
    }
    // mfc1 (incl to $zero) and mtc1.
    let fs_pool: Vec<Setup> = F32P
        .iter()
        .map(|&v| {
            Box::new(move |c: &mut Ctx| {
                c.set_f_bits(4, v);
                c.set_r64(5, 0xAAAA_BBBB_CCCC_DDDD);
            }) as Setup
        })
        .collect();
    let refresh = || -> Vec<Setup> {
        F32P
            .iter()
            .map(|&v| {
                Box::new(move |c: &mut Ctx| {
                    c.set_f_bits(4, v);
                    c.set_r64(5, v as u64 | 0x1111_0000_0000_0000);
                }) as Setup
            })
            .collect()
    };
    cases.push(mk((0x11 << 26) | (5 << 16) | (4 << 11), fs_pool)); // mfc1 $a1, $f4
    cases.push(mk((0x11 << 26) | (4 << 11), refresh())); // mfc1 $zero, $f4
    cases.push(mk((0x11 << 26) | (0x04 << 21) | (5 << 16) | (4 << 11), refresh())); // mtc1
    // Unary and binary arithmetic.
    for funct in [0x05u32, 0x06, 0x07] {
        // abs.s / mov.s / neg.s: fd in sa field, fs in rd field.
        cases.push(mk(
            cop1s(0, 4, 2, funct),
            F32P
                .iter()
                .map(|&v| Box::new(move |c: &mut Ctx| fseed(c, v, 0)) as Setup)
                .collect(),
        ));
    }
    for funct in [0x00u32, 0x01, 0x02, 0x03] {
        cases.push(mk(cop1s(6, 4, 2, funct), pairs_f32(fseed)));
        cases.push(mk(cop1s(6, 4, 4, funct), pairs_f32(fseed))); // fd == fs
    }
    // cvt.w.s and cvt.s.w.
    cases.push(mk(
        cop1s(0, 4, 2, 0x24),
        F32P
            .iter()
            .map(|&v| Box::new(move |c: &mut Ctx| fseed(c, v, 0)) as Setup)
            .collect(),
    ));
    cases.push(mk(
        (0x11 << 26) | (0x14 << 21) | (4 << 11) | (2 << 6) | 0x20,
        I32P
            .iter()
            .map(|&v| Box::new(move |c: &mut Ctx| fseed(c, v as u32, 0)) as Setup)
            .collect(),
    ));
    // Compares, from both fcr31 condition states.
    for funct in [0x32u32, 0x34, 0x36] {
        for pre in [0u32, 0x0080_0000] {
            cases.push(mk(
                cop1s(6, 4, 0, funct),
                pairs_f32(move |c, a, b| {
                    fseed(c, a, b);
                    c.set_fcr31(pre);
                }),
            ));
        }
    }
}

fn cop2_fmac_cases(cases: &mut Vec<Case>) {
    let masks = [0xFu32, 0x8, 0x6, 0x1];
    let regsets = [(2u32, 4u32, 6u32), (4, 4, 6), (2, 0, 6), (0, 4, 6)];
    // Broadcast forms: funct = family*4 + bc.
    for fam in [0u32, 1, 2, 3, 4, 5, 6] {
        for bc in 0u32..4 {
            for &mask in &masks {
                for &(fd, fs, ft) in &regsets {
                    cases.push(mk(c2(mask, ft, fs, fd, fam * 4 + bc), vu_setups()));
                }
            }
        }
    }
    // Vector forms censused: vadd 0x28, vmadd 0x29, vmul 0x2A, vsub 0x2C.
    for funct in [0x28u32, 0x29, 0x2A, 0x2C] {
        for &mask in &masks {
            for &(fd, fs, ft) in &regsets {
                cases.push(mk(c2(mask, ft, fs, fd, funct), vu_setups()));
            }
        }
    }
    // Empty dest mask (writes nothing, still sets flags).
    cases.push(mk(c2(0, 6, 4, 2, 0x28), vu_setups()));
    // Q forms: vaddq 0x20, vmulq 0x1C (ft must be 0).
    for funct in [0x20u32, 0x1C] {
        for &mask in &masks {
            for fd in [2u32, 0] {
                cases.push(mk(c2(mask, 0, 4, fd, funct), vu_setups()));
            }
        }
    }
    // ACC broadcast forms: special2 sub 0x00 adda, 0x02 madda, 0x06 mula.
    for sub in [0x00u32, 0x02, 0x06] {
        for bc in 0u32..4 {
            for &mask in &[0xFu32, 0x6] {
                cases.push(mk(c2(mask, 6, 4, sub, 0x3C | bc), vu_setups()));
            }
        }
    }
    // vopmula.xyz / vopmsub.xyz.
    cases.push(mk(c2(0xE, 6, 4, 0x0B, 0x3E), vu_setups()));
    cases.push(mk(c2(0xE, 6, 4, 2, 0x2E), vu_setups()));
    cases.push(mk(c2(0xE, 6, 4, 0, 0x2E), vu_setups()));
}

fn cop2_misc_cases(cases: &mut Vec<Case>) {
    // vmove / vmr32 across masks and vf00 source/dest.
    for (sa, funct) in [(0x0Cu32, 0x3Cu32), (0x0C, 0x3D)] {
        for mask in [0xFu32, 0x8, 0x4, 0x2, 0x1, 0x9, 0x6] {
            for (ft, fs) in [(2u32, 4u32), (0, 4), (2, 0)] {
                cases.push(mk(c2(mask, ft, fs, sa, funct), vu_setups()));
            }
        }
    }
    // vitof0/4 (sa 0x04), vftoi0/4 (sa 0x05).
    for sa in [0x04u32, 0x05] {
        for bc in [0u32, 1] {
            for mask in [0xFu32, 0x6] {
                cases.push(mk(c2(mask, 2, 4, sa, 0x3C | bc), vu_setups()));
            }
        }
    }
    // Integer-lane sources for itof.
    let iseed: Vec<Setup> = (0..I32P.len())
        .map(|i| {
            Box::new(move |c: &mut Ctx| {
                let l = |k: usize| I32P[(i + k) % I32P.len()] as u32;
                c.set_vf(4, [l(0), l(1), l(2), l(3)]);
            }) as Setup
        })
        .collect();
    cases.push(mk(c2(0xF, 2, 4, 0x04, 0x3C), iseed));
    // vdiv / vsqrt / vrsqrt with lane selector coverage.
    for (fsf, ftf) in [(0u32, 0u32), (1, 3), (3, 1), (2, 2)] {
        let dest = fsf | (ftf << 2);
        cases.push(mk(c2(dest, 6, 4, 0x0E, 0x3C), vu_setups())); // vdiv
        cases.push(mk(c2(dest, 6, 4, 0x0E, 0x3E), vu_setups())); // vrsqrt
    }
    for ftf in 0u32..4 {
        cases.push(mk(c2(ftf << 2, 6, 0, 0x0E, 0x3D), vu_setups())); // vsqrt
    }
    // Zero-heavy operand grid for the div unit special cases.
    let zpool: [u32; 6] = [0, 0x8000_0000, 0x3F80_0000, 0xBF80_0000, 0x0000_0001, 0x7FC0_0000];
    let mut zsetups: Vec<Setup> = Vec::new();
    for &a in &zpool {
        for &b in &zpool {
            zsetups.push(Box::new(move |c: &mut Ctx| {
                c.set_vf(4, [a, a, a, a]);
                c.set_vf(6, [b, b, b, b]);
                c.set_vu_status(0x030);
            }));
        }
    }
    cases.push(mk(c2(0, 6, 4, 0x0E, 0x3C), zsetups)); // vdiv x/x lanes
    // vwaitq / vnop.
    cases.push(mk(c2(0, 0, 0, 0x0E, 0x3F), vu_setups())); // vwaitq
    cases.push(mk(c2(0, 0, 0, 0x0B, 0x3F), vu_setups())); // vnop
    // vclipw.
    for dest in [0xEu32, 0xF] {
        cases.push(mk(c2(dest, 6, 4, 0x07, 0x3F), vu_setups()));
    }
    // vlqi / vsqi / vlqd with pointer wrap and vi0.
    let vptr = |vi: u16| -> Vec<Setup> {
        (0..6)
            .map(move |i| {
                Box::new(move |c: &mut Ctx| {
                    vu_seed(c, i);
                    c.set_vi(7, vi);
                }) as Setup
            })
            .collect()
    };
    for vi in [0u16, 1, 0xFF, 0x100, 0xFFFF] {
        for mask in [0xFu32, 0x6] {
            cases.push(mk(c2(mask, 2, 7, 0x0D, 0x3C), vptr(vi))); // vlqi vf2, (vi7++)
            cases.push(mk(c2(mask, 7, 4, 0x0D, 0x3D), vptr(vi))); // vsqi vf4, (vi7++)
            cases.push(mk(c2(mask, 2, 7, 0x0D, 0x3E), vptr(vi))); // vlqd vf2, (--vi7)
        }
    }
    cases.push(mk(c2(0xF, 2, 0, 0x0D, 0x3C), vptr(9))); // vlqi via vi0
    cases.push(mk(c2(0xF, 0, 4, 0x0D, 0x3D), vptr(9))); // vsqi via vi0
    // viaddi.
    for imm5 in [0u32, 1, 15, 16, 31] {
        for (it, is) in [(3u32, 7u32), (3, 0), (0, 7), (7, 7)] {
            cases.push(mk(
                c2(0, it, is, imm5, 0x32),
                [0u16, 1, 0x7FFF, 0xFFFF]
                    .iter()
                    .map(|&v| {
                        Box::new(move |c: &mut Ctx| {
                            c.set_vi(7, v);
                            c.set_vi(3, 0xBEEF);
                        }) as Setup
                    })
                    .collect(),
            ));
        }
    }
    // R register ops.
    let rpool: [u32; 5] = [0, 0x3F80_0000, 0x3FFF_FFFF, 0x3F80_0001, 0x3FAA_AAAA];
    let rset = |mask: u32, ft: u32, fsf: u32, funct: u32| -> Case {
        mk(
            c2(if funct == 0x3C { mask } else { fsf }, ft, 4, 0x10, funct),
            rpool
                .iter()
                .map(|&r| {
                    Box::new(move |c: &mut Ctx| {
                        vu_seed(c, 3);
                        c.set_vu_r(r);
                    }) as Setup
                })
                .collect(),
        )
    };
    for mask in [0xFu32, 0x2] {
        cases.push(rset(mask, 2, 0, 0x3C)); // vrnext
    }
    for fsf in 0u32..4 {
        cases.push(rset(0, 0, fsf, 0x3E)); // vrinit
        cases.push(rset(0, 0, fsf, 0x3F)); // vrxor
    }
    // qmfc2 / qmtc2 (.ni).
    for (rt, fs) in [(5u32, 4u32), (0, 4), (5, 0)] {
        cases.push(mk(c2xfer(1, rt, fs), vu_setups()));
    }
    for (rt, fd) in [(5u32, 4u32), (5, 0)] {
        cases.push(mk(
            c2xfer(5, rt, fd),
            pairs64(|c, a, b| {
                c.set_r128(5, splat128(a ^ b));
            }),
        ));
    }
    // cfc2 / ctc2 over the inline-handled control registers.
    for creg in [0u32, 1, 7, 15, 16, 17, 18, 20, 21, 22] {
        for rt in [5u32, 0] {
            cases.push(mk(c2xfer(2, rt, creg), vu_setups()));
        }
    }
    for creg in [0u32, 1, 7, 15, 16, 20, 21, 22] {
        cases.push(mk(
            c2xfer(6, 5, creg),
            singles64(|c, a| {
                vu_seed(c, 5);
                c.set_r64(5, a);
            }),
        ));
    }
    // lqc2 / sqc2 (EE addresses, 16-byte masking).
    for off in [0u32, 16, 8, 0xFFF0] {
        for ft in [6u32, 0] {
            cases.push(mk(
                rt3(0x36, 4, ft, off),
                vec![Box::new(|c: &mut Ctx| {
                    vu_seed(c, 2);
                    c.set_r64(4, RAM_BASE as u64);
                })],
            ));
            cases.push(mk(
                rt3(0x3E, 4, ft, off),
                vec![Box::new(|c: &mut Ctx| {
                    vu_seed(c, 4);
                    c.set_r64(4, RAM_BASE as u64);
                })],
            ));
        }
    }
}

// ---- snippet .so ---------------------------------------------------------

#[cfg(unix)]
extern "C" {
    fn dlopen(path: *const c_char, flag: c_int) -> *mut c_void;
    fn dlsym(h: *mut c_void, name: *const c_char) -> *mut c_void;
    fn dlerror() -> *mut c_char;
}

#[cfg(unix)]
const RTLD_NOW: c_int = 2;

#[cfg(windows)]
#[link(name = "kernel32")]
extern "system" {
    fn LoadLibraryA(path: *const c_char) -> *mut c_void;
    fn GetProcAddress(h: *mut c_void, name: *const c_char) -> *mut c_void;
}

fn dso_open(path: &Path) -> *mut c_void {
    let cpath = std::ffi::CString::new(path.to_str().unwrap()).unwrap();
    #[cfg(unix)]
    {
        let h = unsafe { dlopen(cpath.as_ptr(), RTLD_NOW) };
        if h.is_null() {
            let err = unsafe { std::ffi::CStr::from_ptr(dlerror()) };
            panic!("dlopen failed: {}", err.to_string_lossy());
        }
        h
    }
    #[cfg(windows)]
    {
        let h = unsafe { LoadLibraryA(cpath.as_ptr()) };
        assert!(!h.is_null(), "LoadLibraryA failed for {}", path.display());
        h
    }
}

fn dso_sym(h: *mut c_void, name: &str) -> *mut c_void {
    let cname = std::ffi::CString::new(name).unwrap();
    #[cfg(unix)]
    let p = unsafe { dlsym(h, cname.as_ptr()) };
    #[cfg(windows)]
    let p = unsafe { GetProcAddress(h, cname.as_ptr()) };
    assert!(!p.is_null(), "resolving symbol {name} failed");
    p
}

fn repo_include() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .ancestors()
        .nth(4)
        .unwrap()
        .join("include")
}

fn build_so(cases: &[Case]) -> PathBuf {
    let mut src = String::new();
    src.push_str(
        "#include <stdio.h>\n#include <stdlib.h>\n#include \"recomp_ops.h\"\n\
         uint8_t* g_pages[0x10000];\n\
         void so_set_page(uint32_t i, uint8_t* p) { g_pages[i & 0xFFFFu] = p; }\n\
         static void die(const char* w, uint32_t a) {\n\
             fprintf(stderr, \"snippet: unexpected %s (0x%X)\\n\", w, a); abort(); }\n\
         uint8_t rt_mmio_read8(uint32_t a) { die(\"r8\", a); return 0; }\n\
         uint16_t rt_mmio_read16(uint32_t a) { die(\"r16\", a); return 0; }\n\
         uint32_t rt_mmio_read32(uint32_t a) { die(\"r32\", a); return 0; }\n\
         uint64_t rt_mmio_read64(uint32_t a) { die(\"r64\", a); return 0; }\n\
         rc_u128 rt_mmio_read128(uint32_t a) { die(\"r128\", a); return (rc_u128){{0}}; }\n\
         void rt_mmio_write8(uint32_t a, uint8_t v) { (void)v; die(\"w8\", a); }\n\
         void rt_mmio_write16(uint32_t a, uint16_t v) { (void)v; die(\"w16\", a); }\n\
         void rt_mmio_write32(uint32_t a, uint32_t v) { (void)v; die(\"w32\", a); }\n\
         void rt_mmio_write64(uint32_t a, uint64_t v) { (void)v; die(\"w64\", a); }\n\
         void rt_mmio_write128(uint32_t a, rc_u128 v) { (void)v; die(\"w128\", a); }\n\
         uint32_t rt_vu0_cfc(R5900Context* c, int r) { (void)c; die(\"cfc\", (uint32_t)r); return 0; }\n\
         void rt_vu0_ctc(R5900Context* c, int r, uint32_t v) { (void)c; (void)v; die(\"ctc\", (uint32_t)r); }\n\n",
    );
    for (i, case) in cases.iter().enumerate() {
        let insn = decode(case.word, TEST_VRAM);
        let stmt = ee_emit::emit_insn(&insn)
            .unwrap_or_else(|e| panic!("emit failed for {} ({:08X}): {e:#}", case.name, case.word));
        let _ = writeln!(src, "/* {} */\nvoid t_{i}(R5900Context* ctx) {{ (void)ctx; {stmt} }}", case.name);
    }
    let dir = std::env::temp_dir().join(format!("icorecomp-threeway-{}", std::process::id()));
    std::fs::create_dir_all(&dir).unwrap();
    let c_path = dir.join("snippets.c");
    // A mingw gcc on PATH is expected on Windows (what CI's runners have);
    // -fPIC stays unix-only because mingw warns it away under -Werror.
    let so_path = dir.join(if cfg!(windows) { "snippets.dll" } else { "snippets.so" });
    std::fs::write(&c_path, src).unwrap();
    let mut cmd = Command::new("gcc");
    if cfg!(unix) {
        cmd.arg("-fPIC");
    }
    let status = cmd
        .args([
            "-std=c11",
            "-O1",
            "-shared",
            "-fno-strict-aliasing",
            "-ffp-contract=off",
            "-Wall",
            "-Werror",
            "-Wl,--no-undefined",
        ])
        .arg("-I")
        .arg(repo_include())
        .arg(&c_path)
        .arg("-o")
        .arg(&so_path)
        .arg("-lm")
        .status()
        .expect("running gcc");
    assert!(status.success(), "snippet compile failed ({})", c_path.display());
    so_path
}

struct Snippets {
    funcs: Vec<unsafe extern "C" fn(*mut u8)>,
    set_page: unsafe extern "C" fn(u32, *mut u8),
}

fn load_so(path: &Path, n: usize) -> Snippets {
    let h = dso_open(path);
    let sym = |name: &str| -> *mut c_void { dso_sym(h, name) };
    let set_page =
        unsafe { std::mem::transmute::<*mut c_void, unsafe extern "C" fn(u32, *mut u8)>(sym("so_set_page")) };
    let funcs = (0..n)
        .map(|i| unsafe {
            std::mem::transmute::<*mut c_void, unsafe extern "C" fn(*mut u8)>(sym(&format!("t_{i}")))
        })
        .collect();
    Snippets { funcs, set_page }
}

// ---- environment ---------------------------------------------------------

struct Env {
    ram: Vec<u8>,
    vumem: Vec<u8>,
}

impl Env {
    fn new() -> Env {
        Env { ram: vec![0u8; 0x10000], vumem: vec![0u8; 0x10000] }
    }
    fn reset(&mut self) {
        for (i, b) in self.ram.iter_mut().enumerate() {
            *b = (i as u8).wrapping_mul(31).wrapping_add(7);
        }
        for (i, b) in self.vumem.iter_mut().enumerate() {
            *b = (i as u8).wrapping_mul(13).wrapping_add(101);
        }
    }
    fn install(&mut self, so: &Snippets) {
        unsafe {
            set_page(0, self.ram.as_mut_ptr());
            set_page(VU0_PAGE, self.vumem.as_mut_ptr());
            (so.set_page)(0, self.ram.as_mut_ptr());
            (so.set_page)(VU0_PAGE, self.vumem.as_mut_ptr());
        }
    }
}

fn template_ctx() -> Ctx {
    let mut c = Ctx::new();
    for n in 1u8..32 {
        c.set_r64(n, (n as u64).wrapping_mul(0x0101_0101_0101_0101) ^ 0x00FF_00FF_0F0F_5A5A);
    }
    for n in 0u8..32 {
        c.set_f_bits(n, 0x3F80_0000 | (n as u32) << 8);
    }
    for n in 1u8..32 {
        for lane in 0..4 {
            c.set_vf_lane(n, lane, 0x3E80_0000 | ((n as u32) << 12) | (lane as u32) << 2);
        }
    }
    for n in 1u8..16 {
        c.set_vi(n, (n as u16).wrapping_mul(0x213));
    }
    c.set_acc([0x3F00_0000, 0xBF00_0000, 0x0000_0000, 0x4000_0000]);
    c.set_vu_q_bits(0x3F80_0000);
    c.set_vu_i_bits(0x4080_0000);
    c.set_vu_r(0x3F92_1FB5);
    c
}

// ---- runner --------------------------------------------------------------

#[test]
fn threeway() {
    let mut cases: Vec<Case> = Vec::new();
    int_cases(&mut cases);
    mem_cases(&mut cases);
    muldiv_cases(&mut cases);
    mmi_cases(&mut cases);
    cop1_cases(&mut cases);
    cop2_fmac_cases(&mut cases);
    cop2_misc_cases(&mut cases);

    let so_path = build_so(&cases);
    let so = load_so(&so_path, cases.len());
    let mut env = Env::new();
    env.reset();
    env.install(&so);

    let template = template_ctx();
    let mut vectors = 0usize;
    let mut failures: Vec<String> = Vec::new();
    let mut covered: std::collections::BTreeSet<String> = Default::default();

    for (i, case) in cases.iter().enumerate() {
        let insn: Insn = decode(case.word, TEST_VRAM);
        covered.insert(case.name.clone());
        for (si, setup) in case.setups.iter().enumerate() {
            vectors += 1;
            // Common initial state.
            env.reset();
            let mut base = template.clone();
            setup(&mut base);
            let ram0 = env.ram.clone();
            let vu0 = env.vumem.clone();

            // (a) interpreter.
            let mut ci = base.clone();
            step(&mut ci, &insn).unwrap_or_else(|e| {
                panic!("interp failed on {} ({:08X}): {e:#}", case.name, case.word)
            });
            let ictx = ci.bytes().to_vec();
            let iram = env.ram.clone();
            let ivu = env.vumem.clone();

            // (b) compiled emitted snippet, from the same initial state.
            env.ram.copy_from_slice(&ram0);
            env.vumem.copy_from_slice(&vu0);
            let mut cn = base.clone();
            unsafe { (so.funcs[i])(cn.as_mut_ptr()) };

            let nctx = cn.bytes();
            let differs = nctx != ictx.as_slice() || env.ram != iram || env.vumem != ivu;
            if differs && failures.len() < 20 {
                    let mut msg = format!(
                        "{} (word {:08X}, case {i}, setup {si}):",
                        case.name, case.word
                    );
                    for (off, (a, b)) in ictx.iter().zip(nctx.iter()).enumerate() {
                        if a != b {
                            let _ = write!(msg, " ctx[{off:#x}] interp {a:02X} native {b:02X};");
                        }
                    }
                    if env.ram != iram {
                        msg.push_str(" ram differs;");
                    }
                    if env.vumem != ivu {
                        msg.push_str(" vumem differs;");
                    }
                    failures.push(msg);
            }
        }
    }

    println!(
        "three-way: {} instruction words, {} distinct mnemonics, {} vectors",
        cases.len(),
        covered.len(),
        vectors
    );
    if !failures.is_empty() {
        for f in &failures {
            eprintln!("MISMATCH {f}");
        }
        panic!("{} mismatching vectors (first 20 shown)", failures.len());
    }
}
