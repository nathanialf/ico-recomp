//! Bit-equivalence gate for the SSE2 VU1 lane helpers in recomp_ops.h.
//!
//! The FMAC family, max/min, ftoi/itof, abs, clip and the masked writes
//! exist twice in that header: the branch-free four-lane SSE2 form under
//! the public `rc_vu1_*` name that generated code and the runtime call,
//! and the original one-lane-at-a-time body under a `_scalar` suffix. The
//! scalar body is the statement of the semantics; the vector form is an
//! optimisation of it and is allowed no divergence at all, including on
//! the NaN and infinity bit patterns that raw loads can leave in a vf
//! register.
//!
//! csrc/shim.c exports both families, so this test drives one compiled
//! copy of each and compares result bits, mac, status, clip and the
//! written register file exactly. Coverage is a structured corpus (signed
//! zeros, both denormal extremes, normals across the exponent range, FMAX,
//! both infinities, quiet and signalling NaNs, the ftoi saturation
//! boundaries) swept over every ordered pair and every kind, selector,
//! shift and destination mask, then tens of millions of random 32-bit
//! patterns per family under a fixed seed.
//!
//! Run it with `--release`: the random phase is sized for optimised code
//! and drops to a smoke-sized count under `cfg(debug_assertions)`. Pass
//! `-- --nocapture` to see the count summary.

use std::os::raw::c_int;

use vu_interp::VuState;

extern "C" {
    fn x_sse2_active() -> c_int;
    fn x_set_ftz_daz() -> u32;
    fn x_set_mxcsr(v: u32);
    fn x_off_acc() -> usize;
    fn x_off_q() -> usize;
    fn x_off_i() -> usize;

    fn x_fmac_calc(
        vu: *const u8,
        kind: c_int,
        fs: c_int,
        ft: c_int,
        sel: c_int,
        mask: c_int,
        mac_out: *mut u32,
        out: *mut u8,
    );
    fn x_fmac_calc_scalar(
        vu: *const u8,
        kind: c_int,
        fs: c_int,
        ft: c_int,
        sel: c_int,
        mask: c_int,
        mac_out: *mut u32,
        out: *mut u8,
    );
    fn x_maxmin_calc(vu: *const u8, is_min: c_int, fs: c_int, ft: c_int, sel: c_int, out: *mut u8);
    fn x_maxmin_calc_scalar(
        vu: *const u8,
        is_min: c_int,
        fs: c_int,
        ft: c_int,
        sel: c_int,
        out: *mut u8,
    );
    fn x_abs_calc(vu: *const u8, fs: c_int, out: *mut u8);
    fn x_abs_calc_scalar(vu: *const u8, fs: c_int, out: *mut u8);
    fn x_ftoi_calc(vu: *const u8, shift: c_int, fs: c_int, out: *mut u8);
    fn x_ftoi_calc_scalar(vu: *const u8, shift: c_int, fs: c_int, out: *mut u8);
    fn x_itof_calc(vu: *const u8, shift: c_int, fs: c_int, out: *mut u8);
    fn x_itof_calc_scalar(vu: *const u8, shift: c_int, fs: c_int, out: *mut u8);
    fn x_clip_calc(vu: *const u8, fs: c_int, ft: c_int) -> u32;
    fn x_clip_calc_scalar(vu: *const u8, fs: c_int, ft: c_int) -> u32;

    fn x_write(vu: *mut u8, fd: c_int, mask: c_int, p: *const u8);
    fn x_write_scalar(vu: *mut u8, fd: c_int, mask: c_int, p: *const u8);
    fn x_commit_vf(vu: *mut u8, fd: c_int, mask: c_int, p: *const u8, mac: u32);
    fn x_commit_vf_scalar(vu: *mut u8, fd: c_int, mask: c_int, p: *const u8, mac: u32);
    fn x_commit_acc(vu: *mut u8, mask: c_int, p: *const u8, mac: u32);
    fn x_commit_acc_scalar(vu: *mut u8, mask: c_int, p: *const u8, mac: u32);
    fn x_sq(vu: *mut u8, fs: c_int, mask: c_int, qw: u32);
    fn x_sq_scalar(vu: *mut u8, fs: c_int, mask: c_int, qw: u32);
}

/// Random patterns per helper family. Sized for `--release`; a debug run
/// is a smoke test, not the gate.
fn random_iters() -> u64 {
    if cfg!(debug_assertions) {
        200_000
    } else {
        10_000_000
    }
}

/// The five FMAC kinds and the seven third-operand selectors the emitter
/// and the interpreter can present. 4 is RC_VU_SRC_VEC, 5 is Q, 6 is I.
const KINDS: [c_int; 5] = [0, 1, 2, 3, 4];
const SELS: [c_int; 7] = [0, 1, 2, 3, 4, 5, 6];
/// vftoi0/vftoi4/vftoi12/vftoi15 and the matching vitof shifts.
const SHIFTS: [c_int; 4] = [0, 4, 12, 15];

/// Lane bit patterns that decide something: both signed zeros, both
/// denormal extremes, the smallest normals, values around 1.0, the ftoi
/// saturation boundaries at +/-2^31, values whose products overflow and
/// underflow, FMAX, both infinities, and quiet and signalling NaNs of both
/// signs. Normals across the whole exponent range are appended so no
/// exponent class is untested.
fn corpus() -> Vec<u32> {
    let mut v: Vec<u32> = vec![
        0x0000_0000, // +0
        0x8000_0000, // -0
        0x0000_0001, // smallest +denormal
        0x8000_0001, // smallest -denormal
        0x0040_0000, // mid denormal
        0x007F_FFFF, // largest +denormal
        0x807F_FFFF, // largest -denormal
        0x0080_0000, // smallest +normal, 2^-126
        0x8080_0000, // smallest -normal
        0x0080_0001,
        0x0100_0000, // 2^-125
        0x0C80_0000, // 2^-102, squares to an underflowing product
        0x1F00_0000, // 2^-65
        0x3380_0000, // 2^-24
        0x3DCC_CCCD, // 0.1
        0x3F7F_FFFF, // largest float below 1.0
        0x3F80_0000, // 1.0
        0x3F80_0001,
        0xBF80_0000, // -1.0
        0x4000_0000, // 2.0
        0x4B00_0000, // 2^23
        0x4EFF_FFFF, // just below 2^31
        0x4F00_0000, // 2^31 exactly, the ftoi saturation edge
        0x4F00_0001,
        0xCEFF_FFFF,
        0xCF00_0000, // -2^31 exactly, converts in range
        0xCF00_0001, // just past -2^31
        0x5F00_0000, // 2^63
        0x7E80_0000, // 2^126, squares to an overflowing product
        0xFE80_0000,
        0x7F7F_FFFF, // FMAX
        0xFF7F_FFFF, // -FMAX
        0x7F80_0000, // +inf
        0xFF80_0000, // -inf
        0x7F80_0001, // +sNaN, smallest payload
        0xFF80_0001, // -sNaN
        0x7FBF_FFFF, // +sNaN, largest payload
        0x7FC0_0000, // +qNaN
        0xFFC0_0000, // -qNaN
        0x7FFF_FFFF, // +qNaN, largest payload
        0xFFFF_FFFF, // -qNaN, largest payload
        0x0BAD_F00D,
        0xDEAD_BEEF,
    ];
    for e in (1u32..=254).step_by(9) {
        v.push((e << 23) | 0x0012_3456);
        v.push(0x8000_0000 | (e << 23) | 0x0065_4321);
    }
    v.sort_unstable();
    v.dedup();
    v
}

struct Rng(u32);
impl Rng {
    fn next(&mut self) -> u32 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 17;
        self.0 ^= self.0 << 5;
        self.0
    }
    /// Mostly uniform 32-bit words, with a share drawn from the corpus so
    /// the interesting encodings keep turning up at a useful rate.
    fn pattern(&mut self, corpus: &[u32]) -> u32 {
        let r = self.next();
        if r & 7 < 3 {
            corpus[(r >> 3) as usize % corpus.len()]
        } else {
            self.next()
        }
    }
}

/// A VU1 state with direct access to the fields the helpers read.
struct Bed {
    st: VuState,
    off_acc: usize,
    off_q: usize,
    off_i: usize,
}

impl Bed {
    fn new() -> Bed {
        let (off_acc, off_q, off_i) = unsafe { (x_off_acc(), x_off_q(), x_off_i()) };
        Bed {
            st: VuState::new(),
            off_acc,
            off_q,
            off_i,
        }
    }
    fn put(&mut self, off: usize, v: u32) {
        unsafe { std::ptr::write_unaligned(self.st.as_mut_ptr().add(off) as *mut u32, v) }
    }
    fn set_vf(&mut self, reg: usize, l: [u32; 4]) {
        for (k, b) in l.iter().enumerate() {
            self.st.set_vf_lane(reg, k, *b);
        }
    }
    fn set_acc(&mut self, l: [u32; 4]) {
        let off = self.off_acc;
        for (k, b) in l.iter().enumerate() {
            self.put(off + k * 4, *b);
        }
    }
    fn set_q(&mut self, b: u32) {
        let off = self.off_q;
        self.put(off, b);
    }
    fn set_i(&mut self, b: u32) {
        let off = self.off_i;
        self.put(off, b);
    }
    fn ptr(&self) -> *const u8 {
        self.st.as_ptr()
    }
}

fn lanes(c: &[u32], i: usize) -> [u32; 4] {
    let n = c.len();
    [c[i % n], c[(i + 1) % n], c[(i + 2) % n], c[(i + 3) % n]]
}

fn q(v: &[u8; 16]) -> [u32; 4] {
    let mut o = [0u32; 4];
    for k in 0..4 {
        o[k] = u32::from_le_bytes(v[k * 4..k * 4 + 4].try_into().unwrap());
    }
    o
}

struct Counts {
    cases: u64,
    fails: u64,
}

impl Counts {
    fn new() -> Counts {
        Counts { cases: 0, fails: 0 }
    }
    fn check(&mut self, ok: bool, what: impl FnOnce() -> String) {
        self.cases += 1;
        if !ok {
            self.fails += 1;
            if self.fails <= 8 {
                println!("MISMATCH {}", what());
            }
        }
    }
}

/// The six families the corpus sweep drives. Held together because the
/// sweep runs twice, once under the host's default MXCSR and once under
/// the runtime's FTZ+DAZ, and both passes accumulate into the same
/// counters.
struct Families {
    fmac: Counts,
    maxmin: Counts,
    ftoi: Counts,
    itof: Counts,
    abs: Counts,
    clip: Counts,
}

impl Families {
    fn new() -> Families {
        Families {
            fmac: Counts::new(),
            maxmin: Counts::new(),
            ftoi: Counts::new(),
            itof: Counts::new(),
            abs: Counts::new(),
            clip: Counts::new(),
        }
    }
    fn totals(&self) -> (u64, u64) {
        let k = [
            &self.fmac,
            &self.maxmin,
            &self.ftoi,
            &self.itof,
            &self.abs,
            &self.clip,
        ];
        (k.iter().map(|x| x.cases).sum(), k.iter().map(|x| x.fails).sum())
    }
}

/// One FMAC comparison. `seed_mac` is nonzero so the OR-into-*mac
/// contract is exercised, not just the fresh-accumulator case.
#[allow(clippy::too_many_arguments)]
fn cmp_fmac(
    c: &mut Counts,
    bed: &Bed,
    kind: c_int,
    fs: c_int,
    ft: c_int,
    sel: c_int,
    mask: c_int,
    seed_mac: u32,
    tag: &str,
) {
    let (mut ma, mut ms) = (seed_mac, seed_mac);
    let (mut ra, mut rs) = ([0u8; 16], [0u8; 16]);
    unsafe {
        x_fmac_calc(bed.ptr(), kind, fs, ft, sel, mask, &mut ma, ra.as_mut_ptr());
        x_fmac_calc_scalar(bed.ptr(), kind, fs, ft, sel, mask, &mut ms, rs.as_mut_ptr());
    }
    c.check(ra == rs && ma == ms, || {
        format!(
            "fmac {tag} kind={kind} fs={fs} ft={ft} sel={sel} mask={mask:#x}\n  \
             vf1={:08x?}\n  vf2={:08x?}\n  sse r={:08x?} mac={ma:#06x}\n  \
             scalar r={:08x?} mac={ms:#06x}",
            [
                bed.st.vf_lane(1, 0),
                bed.st.vf_lane(1, 1),
                bed.st.vf_lane(1, 2),
                bed.st.vf_lane(1, 3)
            ],
            [
                bed.st.vf_lane(2, 0),
                bed.st.vf_lane(2, 1),
                bed.st.vf_lane(2, 2),
                bed.st.vf_lane(2, 3)
            ],
            q(&ra),
            q(&rs)
        )
    });
}

fn cmp_pair16(
    c: &mut Counts,
    ra: [u8; 16],
    rs: [u8; 16],
    tag: impl FnOnce() -> String + Copy,
) {
    c.check(ra == rs, || {
        format!("{}\n  sse={:08x?}\n  scalar={:08x?}", tag(), q(&ra), q(&rs))
    });
}

/// The exhaustive corpus sweep, run once per MXCSR mode. Lane 0 of the two
/// operands walks every ordered corpus pair; lanes 1..3 carry rotations of
/// the same corpus, so a single call tests four pairs and the whole sweep
/// covers the cross product.
fn corpus_sweep(bed: &mut Bed, c: &[u32], f: &mut Families) {
    let n = c.len();

    for i in 0..n {
        bed.set_vf(1, lanes(c, i));
        for j in 0..n {
            bed.set_vf(2, lanes(c, j));
            bed.set_acc(lanes(c, i + j));
            bed.set_q(c[(i + 5) % n]);
            bed.set_i(c[(j + 11) % n]);
            for &kind in KINDS.iter() {
                // every destination mask, third operand the full vector
                for mask in 0..16 {
                    cmp_fmac(&mut f.fmac, bed, kind, 1, 2, 4, mask, 0, "corpus");
                }
                // every third-operand selector, full destination mask, and
                // the vf00 = (0,0,0,1) invariant on either operand
                for &sel in SELS.iter() {
                    cmp_fmac(&mut f.fmac, bed, kind, 1, 2, sel, 0xF, 0xA5A5, "corpus-sel");
                    cmp_fmac(&mut f.fmac, bed, kind, 0, 2, sel, 0xF, 0, "corpus-vf00-fs");
                    cmp_fmac(&mut f.fmac, bed, kind, 1, 0, sel, 0xF, 0, "corpus-vf00-ft");
                }
            }
        }
}

for i in 0..n {
    bed.set_vf(1, lanes(c, i));
    bed.set_q(c[(i + 7) % n]);
    bed.set_i(c[(i + 3) % n]);
    for &shift in SHIFTS.iter() {
        let (mut ra, mut rs) = ([0u8; 16], [0u8; 16]);
        unsafe {
            x_ftoi_calc(bed.ptr(), shift, 1, ra.as_mut_ptr());
            x_ftoi_calc_scalar(bed.ptr(), shift, 1, rs.as_mut_ptr());
        }
        cmp_pair16(&mut f.ftoi, ra, rs, || format!("ftoi shift={shift} corpus i={i}"));
        unsafe {
            x_itof_calc(bed.ptr(), shift, 1, ra.as_mut_ptr());
            x_itof_calc_scalar(bed.ptr(), shift, 1, rs.as_mut_ptr());
        }
        cmp_pair16(&mut f.itof, ra, rs, || format!("itof shift={shift} corpus i={i}"));
    }
    let (mut ra, mut rs) = ([0u8; 16], [0u8; 16]);
    unsafe {
        x_abs_calc(bed.ptr(), 1, ra.as_mut_ptr());
        x_abs_calc_scalar(bed.ptr(), 1, rs.as_mut_ptr());
    }
    cmp_pair16(&mut f.abs, ra, rs, || format!("abs corpus i={i}"));

    for j in 0..n {
        bed.set_vf(2, lanes(c, j));
        for &sel in SELS.iter() {
            for is_min in 0..2 {
                unsafe {
                    x_maxmin_calc(bed.ptr(), is_min, 1, 2, sel, ra.as_mut_ptr());
                    x_maxmin_calc_scalar(bed.ptr(), is_min, 1, 2, sel, rs.as_mut_ptr());
                }
                cmp_pair16(&mut f.maxmin, ra, rs, || {
                    format!("maxmin is_min={is_min} sel={sel} i={i} j={j}")
                });
            }
        }
        for &prev in [0u32, 0x00FF_FFFF, 0x0055_5555, 0x00AA_AAAA].iter() {
            bed.st.set_clip(prev);
            let (a, b) = unsafe { (x_clip_calc(bed.ptr(), 1, 2), x_clip_calc_scalar(bed.ptr(), 1, 2)) };
            f.clip.check(a == b, || {
                format!("clip prev={prev:#08x} i={i} j={j} sse={a:#08x} scalar={b:#08x}")
            });
        }
    }
}
}

#[test]
fn vu1_helpers_sse2_matches_scalar() {
    let sse2 = unsafe { x_sse2_active() } != 0;
    let c = corpus();
    let n = c.len();
    let iters = random_iters();
    println!(
        "vu1 helper equivalence: SSE2 path {}, corpus {} patterns, {} random iterations per family",
        if sse2 { "active" } else { "absent (scalar vs scalar)" },
        n,
        iters
    );

    let mut bed = Bed::new();
    let mut tot = Counts::new();

    // ---- exhaustive corpus sweeps ---------------------------------------
    // Run twice: once under the MXCSR a test process starts with, and once
    // under the FTZ+DAZ the runtime sets on every guest-executing thread
    // (src/runtime/main.cpp). Both forms run under the same MXCSR within a
    // pass, so each pass is a like-for-like comparison; running both modes
    // is what shows the vector form does not depend on denormal handling
    // the production build has turned off.
    let mut f = Families::new();
    corpus_sweep(&mut bed, &c, &mut f);
    let (cases_default, fails_default) = f.totals();
    let prev_mxcsr = unsafe { x_set_ftz_daz() };
    corpus_sweep(&mut bed, &c, &mut f);
    unsafe { x_set_mxcsr(prev_mxcsr) };
    let (cases_both, fails_both) = f.totals();
    println!(
        "  corpus pass 1 (MXCSR 0x{prev_mxcsr:04x}: FTZ {}, DAZ {}) {cases_default} cases, {fails_default} mismatches",
        if prev_mxcsr & 0x8000 != 0 { "on" } else { "off" },
        if prev_mxcsr & 0x0040 != 0 { "on" } else { "off" },
    );
    println!(
        "  corpus pass 2 (FTZ on, DAZ on: the runtime's mode)  {} cases, {} mismatches",
        cases_both - cases_default,
        fails_both - fails_default
    );

    // ---- masked writes and commits --------------------------------------
    // Two states, the same starting bytes, one helper each; the whole
    // state must stay byte-identical, which covers vf, acc, status and mac
    // together.
    let mut stores = Counts::new();
    {
        let mut a = Bed::new();
        for r in 0..32 {
            a.set_vf(r, lanes(&c, r * 3));
        }
        a.set_acc(lanes(&c, 17));
        for qw in 0..1024 {
            let l = lanes(&c, qw + 1);
            let off = qw * 16;
            let mem = a.st.mem_mut();
            for k in 0..4 {
                mem[off + k * 4..off + k * 4 + 4].copy_from_slice(&l[k].to_le_bytes());
            }
        }
        for i in (0..n).step_by(7) {
            let mut payload = [0u8; 16];
            for (k, w) in lanes(&c, i).iter().enumerate() {
                payload[k * 4..k * 4 + 4].copy_from_slice(&w.to_le_bytes());
            }
            for &fd in [0usize, 1, 5, 31].iter() {
                for mask in 0..16 {
                    for &mac in [0u32, 0xFFFF, 0x1234].iter() {
                        let (mut sa, mut sb) = (a.st.clone(), a.st.clone());
                        unsafe {
                            x_write(sa.as_mut_ptr(), fd as c_int, mask, payload.as_ptr());
                            x_write_scalar(sb.as_mut_ptr(), fd as c_int, mask, payload.as_ptr());
                        }
                        stores.check(sa.bytes() == sb.bytes(), || {
                            format!("write fd={fd} mask={mask:#x}")
                        });

                        let (mut sa, mut sb) = (a.st.clone(), a.st.clone());
                        unsafe {
                            x_commit_vf(sa.as_mut_ptr(), fd as c_int, mask, payload.as_ptr(), mac);
                            x_commit_vf_scalar(
                                sb.as_mut_ptr(),
                                fd as c_int,
                                mask,
                                payload.as_ptr(),
                                mac,
                            );
                        }
                        stores.check(sa.bytes() == sb.bytes(), || {
                            format!("commit_vf fd={fd} mask={mask:#x} mac={mac:#x}")
                        });

                        let (mut sa, mut sb) = (a.st.clone(), a.st.clone());
                        unsafe {
                            x_commit_acc(sa.as_mut_ptr(), mask, payload.as_ptr(), mac);
                            x_commit_acc_scalar(sb.as_mut_ptr(), mask, payload.as_ptr(), mac);
                        }
                        stores.check(sa.bytes() == sb.bytes(), || {
                            format!("commit_acc mask={mask:#x} mac={mac:#x}")
                        });

                        let (mut sa, mut sb) = (a.st.clone(), a.st.clone());
                        let qw = (i as u32).wrapping_mul(37).wrapping_add(mask as u32);
                        unsafe {
                            x_sq(sa.as_mut_ptr(), fd.max(1) as c_int, mask, qw);
                            x_sq_scalar(sb.as_mut_ptr(), fd.max(1) as c_int, mask, qw);
                        }
                        stores.check(sa.bytes() == sb.bytes(), || {
                            format!("sq fs={fd} mask={mask:#x} qw={qw}")
                        });
                    }
                }
            }
        }
    }

    // ---- random sweeps ---------------------------------------------------
    let mut rng = Rng(0x1234_5678);
    for _ in 0..iters {
        let a = [
            rng.pattern(&c),
            rng.pattern(&c),
            rng.pattern(&c),
            rng.pattern(&c),
        ];
        let b = [
            rng.pattern(&c),
            rng.pattern(&c),
            rng.pattern(&c),
            rng.pattern(&c),
        ];
        let acc = [
            rng.pattern(&c),
            rng.pattern(&c),
            rng.pattern(&c),
            rng.pattern(&c),
        ];
        bed.set_vf(1, a);
        bed.set_vf(2, b);
        bed.set_acc(acc);
        bed.set_q(rng.pattern(&c));
        bed.set_i(rng.pattern(&c));
        let w = rng.next();
        let kind = KINDS[(w % 5) as usize];
        let sel = SELS[((w >> 3) % 7) as usize];
        let mask = ((w >> 8) & 0xF) as c_int;
        let fs = ((w >> 12) % 3) as c_int;
        let ft = ((w >> 16) % 3) as c_int;
        cmp_fmac(&mut f.fmac, &bed, kind, fs, ft, sel, mask, w, "random");
    }

    let mut rng = Rng(0x9E37_79B9);
    for _ in 0..iters {
        bed.set_vf(
            1,
            [
                rng.pattern(&c),
                rng.pattern(&c),
                rng.pattern(&c),
                rng.pattern(&c),
            ],
        );
        bed.set_vf(
            2,
            [
                rng.pattern(&c),
                rng.pattern(&c),
                rng.pattern(&c),
                rng.pattern(&c),
            ],
        );
        bed.set_q(rng.pattern(&c));
        bed.set_i(rng.pattern(&c));
        let w = rng.next();
        let sel = SELS[(w % 7) as usize];
        let is_min = ((w >> 3) & 1) as c_int;
        let (mut ra, mut rs) = ([0u8; 16], [0u8; 16]);
        unsafe {
            x_maxmin_calc(bed.ptr(), is_min, 1, 2, sel, ra.as_mut_ptr());
            x_maxmin_calc_scalar(bed.ptr(), is_min, 1, 2, sel, rs.as_mut_ptr());
        }
        cmp_pair16(&mut f.maxmin, ra, rs, || format!("maxmin random is_min={is_min} sel={sel}"));
    }

    let mut rng = Rng(0x85EB_CA6B);
    for _ in 0..iters {
        bed.set_vf(
            1,
            [
                rng.pattern(&c),
                rng.pattern(&c),
                rng.pattern(&c),
                rng.pattern(&c),
            ],
        );
        let shift = SHIFTS[(rng.next() % 4) as usize];
        let (mut ra, mut rs) = ([0u8; 16], [0u8; 16]);
        unsafe {
            x_ftoi_calc(bed.ptr(), shift, 1, ra.as_mut_ptr());
            x_ftoi_calc_scalar(bed.ptr(), shift, 1, rs.as_mut_ptr());
        }
        cmp_pair16(&mut f.ftoi, ra, rs, || format!("ftoi random shift={shift}"));
        unsafe {
            x_itof_calc(bed.ptr(), shift, 1, ra.as_mut_ptr());
            x_itof_calc_scalar(bed.ptr(), shift, 1, rs.as_mut_ptr());
        }
        cmp_pair16(&mut f.itof, ra, rs, || format!("itof random shift={shift}"));
        unsafe {
            x_abs_calc(bed.ptr(), 1, ra.as_mut_ptr());
            x_abs_calc_scalar(bed.ptr(), 1, rs.as_mut_ptr());
        }
        cmp_pair16(&mut f.abs, ra, rs, || "abs random".to_string());
    }

    let mut rng = Rng(0xC2B2_AE35);
    for _ in 0..iters {
        bed.set_vf(
            1,
            [
                rng.pattern(&c),
                rng.pattern(&c),
                rng.pattern(&c),
                rng.pattern(&c),
            ],
        );
        bed.set_vf(
            2,
            [
                rng.pattern(&c),
                rng.pattern(&c),
                rng.pattern(&c),
                rng.pattern(&c),
            ],
        );
        bed.st.set_clip(rng.next() & 0x00FF_FFFF);
        let (a, b) = unsafe { (x_clip_calc(bed.ptr(), 1, 2), x_clip_calc_scalar(bed.ptr(), 1, 2)) };
        f.clip.check(a == b, || format!("clip random sse={a:#08x} scalar={b:#08x}"));
    }

    let mut rng = Rng(0x27D4_EB2F);
    {
        let mut base = Bed::new();
        for r in 0..32 {
            base.set_vf(
                r,
                [
                    rng.pattern(&c),
                    rng.pattern(&c),
                    rng.pattern(&c),
                    rng.pattern(&c),
                ],
            );
        }
        let (mut sa, mut sb) = (base.st.clone(), base.st.clone());
        for _ in 0..iters {
            let mut payload = [0u8; 16];
            for k in 0..4 {
                payload[k * 4..k * 4 + 4].copy_from_slice(&rng.pattern(&c).to_le_bytes());
            }
            let w = rng.next();
            let fd = ((w >> 2) % 32) as c_int;
            let mask = (w & 0xF) as c_int;
            let mac = rng.next();
            let qw = rng.next();
            // The two states start equal at every step and are compared on
            // the fields the helpers touch: vf[fd], acc, status, mac, and
            // the quadword of data memory the store addresses.
            unsafe {
                x_commit_vf(sa.as_mut_ptr(), fd, mask, payload.as_ptr(), mac);
                x_commit_vf_scalar(sb.as_mut_ptr(), fd, mask, payload.as_ptr(), mac);
                x_commit_acc(sa.as_mut_ptr(), mask, payload.as_ptr(), mac);
                x_commit_acc_scalar(sb.as_mut_ptr(), mask, payload.as_ptr(), mac);
                x_sq(sa.as_mut_ptr(), fd.max(1), mask, qw);
                x_sq_scalar(sb.as_mut_ptr(), fd.max(1), mask, qw);
            }
            let off = ((qw & 0x3FF) * 16) as usize;
            let same = (0..4).all(|k| sa.vf_lane(fd as usize, k) == sb.vf_lane(fd as usize, k))
                && sa.status() == sb.status()
                && sa.mac() == sb.mac()
                && sa.mem()[off..off + 16] == sb.mem()[off..off + 16];
            stores.check(same, || {
                format!("store random fd={fd} mask={mask:#x} mac={mac:#x} qw={qw:#x}")
            });
            if !same {
                break;
            }
        }
        // Whatever the sequence did, the two states must still be equal
        // everywhere, not just in the fields checked step by step.
        stores.check(sa.bytes() == sb.bytes(), || {
            "store random: full state diverged".to_string()
        });
    }

    for (name, k) in [
        ("fmac", &f.fmac),
        ("maxmin", &f.maxmin),
        ("ftoi", &f.ftoi),
        ("itof", &f.itof),
        ("abs", &f.abs),
        ("clip", &f.clip),
        ("write/commit/sq", &stores),
    ] {
        println!("  {name:<16} {:>12} cases, {} mismatches", k.cases, k.fails);
        tot.cases += k.cases;
        tot.fails += k.fails;
    }
    println!(
        "  {:<16} {:>12} cases, {} mismatches",
        "total", tot.cases, tot.fails
    );
    assert_eq!(tot.fails, 0, "SSE2 VU1 helpers diverge from the scalar reference");
    assert!(
        tot.cases > 1_000_000,
        "coverage collapsed: only {} cases ran",
        tot.cases
    );
}
