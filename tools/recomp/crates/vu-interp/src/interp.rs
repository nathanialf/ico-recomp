//! Reference VU1 interpreter.
//!
//! Every arithmetic and memory operation goes through the shim into the
//! same `rc_vu1_*` helpers the emitter's generated C calls, so the two
//! sides cannot disagree about float semantics. What they *can* disagree
//! about is scheduling, and that is deliberate: this interpreter models
//! the two VU pipeline hazards **dynamically**, at run time, whereas
//! vu-emit models them **statically** by auditing sites ahead of time in
//! analyze.rs (`old_vi_sites`, `q_commit_sites`).
//!
//! A differential failure therefore localises to one of:
//!   - analyze.rs attached an integer-branch hazard to the wrong bundle,
//!     or missed one;
//!   - analyze.rs committed the Q pipeline at the wrong bundle;
//!   - the emitter picked a wrong branch target or ordering.
//!
//! Hazards modelled here:
//!
//! * Integer-branch: a conditional branch or `jr` whose source vi was
//!   written by the immediately preceding bundle reads the OLD value. Held
//!   in `prev_write` as (register, pre-write value), refreshed every
//!   bundle.
//! * Q pipeline: `div`/`sqrt` take 7 cycles, `rsqrt` 13. A Q read inside
//!   that window sees the old Q. Modelled as a countdown decremented once
//!   per bundle; the pending value is committed at `waitq`, at the next
//!   div-unit issue, or at a Q read once the countdown has expired.
//!
//! The op coverage is the same measured census the emitter enforces:
//! anything outside it is an error rather than a guess.

use anyhow::{bail, Result};
use vu_decode::lower::LowerOp;
use vu_decode::upper::{FixedPoint, FmacOp, Rhs, UpperOp};
use vu_decode::{branch_target, Bundle, Comp, Dest, LowerSlot};

use crate::ffi;
use crate::state::VuState;

/// Why a run stopped.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Stop {
    /// E-bit reached; `pc` is where a following MSCNT would resume.
    EndBit { pc: u32 },
    /// Ran past the last bundle.
    FellOffEnd,
    /// Instruction budget exhausted (a loop that does not terminate under
    /// the seeded state).
    Budget,
}

#[derive(Debug, Clone)]
pub struct Outcome {
    pub stop: Stop,
    pub bundles_executed: u64,
    /// Execution count per bundle index. Answers "did this path run at
    /// all", which is what separates a differential that exercises the
    /// suspect code from one that merely passes.
    pub coverage: Vec<u64>,
    /// Ordered bundle offsets as executed, capped at TRACE_CAP. Coverage
    /// says which bundles ran; this says in what order, which is what
    /// locates the first scheduling disagreement with the emitter.
    pub trace: Vec<u32>,
    /// Register-file hash on entry to each traced bundle, so a value
    /// divergence can be localised the same way a path divergence is.
    pub hashes: Vec<u32>,
}

/// Traces exist to name a diverging bundle, not to profile. A run that
/// needs more than this has already diverged.
pub const TRACE_CAP: usize = 200_000;

fn comp_idx(c: Comp) -> i32 {
    match c {
        Comp::X => 0,
        Comp::Y => 1,
        Comp::Z => 2,
        Comp::W => 3,
    }
}

/// (ft register, selector code) matching RC_VU_SRC_* in recomp_ops.h.
fn rhs_sel(rhs: Rhs) -> (i32, i32) {
    match rhs {
        Rhs::Bc(ft, c) => (ft.0 as i32, comp_idx(c)),
        Rhs::Ft(ft) => (ft.0 as i32, 4),
        Rhs::Q => (0, 5),
        Rhs::I => (0, 6),
    }
}

fn fixed_shift(f: FixedPoint) -> i32 {
    match f {
        FixedPoint::F0 => 0,
        FixedPoint::F4 => 4,
        FixedPoint::F12 => 12,
        FixedPoint::F15 => 15,
    }
}

fn single_lane(off: u32, dest: Dest) -> Result<i32> {
    match dest.0 {
        8 => Ok(0),
        4 => Ok(1),
        2 => Ok(2),
        1 => Ok(3),
        m => bail!("ilw/ilwr at {off:#x} has a non-single-lane dest mask {m:#x}"),
    }
}

/// The upper half's pending write, held until the lower half has run.
/// The emitter orders calc -> lower -> commit, so the interpreter must too:
/// a lower-slot flag read (fmand, fsand) in the same bundle sees the flags
/// as they were before this bundle's upper op, which is also what the MAC
/// flag pipeline does on hardware.
enum Commit {
    None,
    /// rc_vu1_write: no flag side effect.
    Write { reg: u8, mask: i32, buf: [u8; 16] },
    Vf { fd: u8, mask: i32, buf: [u8; 16], mac: u32 },
    Acc { mask: i32, buf: [u8; 16], mac: u32 },
    Clip(u32),
}

/// What the lower half asks the sequencer to do next.
enum Ctl {
    None,
    Cond { taken: bool, target: u32 },
    Jump { target: u32 },
    Indirect { target: u32 },
}

struct Interp<'a> {
    st: &'a mut VuState,
    /// (vi register, value before this bundle wrote it), for the
    /// integer-branch hazard. Set from the previous bundle.
    prev_write: Option<(u8, u32)>,
    /// Same, being accumulated for the current bundle.
    cur_write: Option<(u8, u32)>,
    /// Bundles remaining before the pending Q becomes readable. 0 = no
    /// outstanding divide.
    q_latency: u32,
    /// MAC and status as they were N bundles ago, oldest first. Only used
    /// when flag_latency > 0. The VU flag pipeline is 4 deep: a lower-slot
    /// flag read sees the flags of the FMAC four bundles back, not the one
    /// that just ran. Neither this interpreter nor the emitter models that
    /// by default; setting VU_FLAG_LATENCY turns it on here so the gate can
    /// measure whether it changes what the programs draw.
    flag_hist: [(u32, u32); 4],
    flag_latency: usize,
}

impl<'a> Interp<'a> {
    /// MAC as a lower-slot flag op sees it.
    fn vis_mac(&self) -> u32 {
        if self.flag_latency == 0 { return self.st.mac(); }
        self.flag_hist[self.flag_hist.len() - self.flag_latency].0
    }
    /// Status as a lower-slot flag op sees it.
    fn vis_status(&self) -> u32 {
        if self.flag_latency == 0 { return self.st.status(); }
        self.flag_hist[self.flag_hist.len() - self.flag_latency].1
    }

    fn vi(&self, n: u8) -> u32 {
        unsafe { ffi::x_vi(self.st.as_ptr(), n as i32) }
    }

    /// vi read for a branch condition: honours the integer-branch hazard.
    fn cond_vi(&self, n: u8) -> u32 {
        match self.prev_write {
            Some((r, old)) if r == n => old,
            _ => self.vi(n),
        }
    }

    fn viset(&mut self, n: u8, v: u32) {
        // Record the pre-write value once per bundle per register, so a
        // branch in the next bundle can see it.
        if self.cur_write.is_none() {
            self.cur_write = Some((n, self.vi(n)));
        }
        unsafe { ffi::x_viset(self.st.as_mut_ptr(), n as i32, v) }
    }

    /// A Q read point. Commits the pending value only once the divider
    /// latency has elapsed, which is what makes this an independent check
    /// on analyze.rs's static q_commit_sites.
    fn q_read(&mut self) {
        if self.q_latency == 0 {
            unsafe { ffi::x_q_commit(self.st.as_mut_ptr()) }
        }
    }

    fn tick_q(&mut self) {
        self.q_latency = self.q_latency.saturating_sub(1);
    }

    fn upper_calc(&mut self, b: &Bundle) -> Result<Commit> {
        let off = b.offset;
        let mut buf = [0u8; 16];
        match b.upper {
            UpperOp::Nop => Ok(Commit::None),
            UpperOp::Fmac { op, dest, fd, fs, rhs } => {
                let (ft, sel) = rhs_sel(rhs);
                if matches!(rhs, Rhs::Q) {
                    self.q_read();
                }
                let mask = dest.0 as i32;
                let kind = match (op, rhs) {
                    (FmacOp::Add, _) => 0,
                    (FmacOp::Sub, Rhs::Ft(_)) | (FmacOp::Sub, Rhs::Bc(..)) => 1,
                    (FmacOp::Mul, Rhs::Ft(_))
                    | (FmacOp::Mul, Rhs::Bc(..))
                    | (FmacOp::Mul, Rhs::Q) => 2,
                    (FmacOp::Madd, Rhs::Bc(..)) | (FmacOp::Madd, Rhs::I) => 3,
                    (FmacOp::Max, Rhs::Bc(..)) => {
                        unsafe {
                            ffi::x_maxmin_calc(
                                self.st.as_ptr(), 0, fs.0 as i32, ft, sel, buf.as_mut_ptr(),
                            );
                        }
                        return Ok(Commit::Write { reg: fd.0, mask, buf });
                    }
                    (FmacOp::Mini, Rhs::I) => {
                        unsafe {
                            ffi::x_maxmin_calc(
                                self.st.as_ptr(), 1, fs.0 as i32, ft, sel, buf.as_mut_ptr(),
                            );
                        }
                        return Ok(Commit::Write { reg: fd.0, mask, buf });
                    }
                    _ => bail!("upper {op:?}/{rhs:?} at {off:#x} not in the measured census"),
                };
                let mut mac = 0u32;
                unsafe {
                    ffi::x_fmac_calc(
                        self.st.as_ptr(), kind, fs.0 as i32, ft, sel, mask,
                        &mut mac as *mut u32, buf.as_mut_ptr(),
                    );
                }
                Ok(Commit::Vf { fd: fd.0, mask, buf, mac })
            }
            UpperOp::FmacA { op, dest, fs, rhs } => {
                let (ft, sel) = rhs_sel(rhs);
                if matches!(rhs, Rhs::Q) {
                    self.q_read();
                }
                let mask = dest.0 as i32;
                let kind = match (op, rhs) {
                    (FmacOp::Mul, Rhs::Bc(..)) | (FmacOp::Mul, Rhs::I) => 2,
                    (FmacOp::Madd, Rhs::Bc(..)) => 3,
                    _ => bail!("upper acc {op:?}/{rhs:?} at {off:#x} not in the measured census"),
                };
                let mut mac = 0u32;
                unsafe {
                    ffi::x_fmac_calc(
                        self.st.as_ptr(), kind, fs.0 as i32, ft, sel, mask,
                        &mut mac as *mut u32, buf.as_mut_ptr(),
                    );
                }
                Ok(Commit::Acc { mask, buf, mac })
            }
            UpperOp::Ftoi { fixed, dest, ft, fs } => {
                unsafe {
                    ffi::x_ftoi_calc(
                        self.st.as_ptr(), fixed_shift(fixed), fs.0 as i32, buf.as_mut_ptr(),
                    );
                }
                Ok(Commit::Write { reg: ft.0, mask: dest.0 as i32, buf })
            }
            UpperOp::Itof { fixed, dest, ft, fs } => {
                unsafe {
                    ffi::x_itof_calc(
                        self.st.as_ptr(), fixed_shift(fixed), fs.0 as i32, buf.as_mut_ptr(),
                    );
                }
                Ok(Commit::Write { reg: ft.0, mask: dest.0 as i32, buf })
            }
            UpperOp::Abs { dest, ft, fs } => {
                unsafe { ffi::x_abs_calc(self.st.as_ptr(), fs.0 as i32, buf.as_mut_ptr()) };
                Ok(Commit::Write { reg: ft.0, mask: dest.0 as i32, buf })
            }
            UpperOp::Clip { fs, ft } => {
                let c = unsafe { ffi::x_clip_calc(self.st.as_ptr(), fs.0 as i32, ft.0 as i32) };
                Ok(Commit::Clip(c))
            }
            UpperOp::Opmula { .. } | UpperOp::Opmsub { .. } => {
                bail!("outer-product op at {off:#x} not in the measured census")
            }
            UpperOp::Invalid { raw } => bail!("invalid upper {raw:#010x} at {off:#x}"),
        }
    }

    /// Apply the upper half's pending write. Runs after the lower half.
    fn apply_commit(&mut self, c: Commit) {
        match c {
            Commit::None => {}
            Commit::Write { reg, mask, buf } => unsafe {
                ffi::x_write(self.st.as_mut_ptr(), reg as i32, mask, buf.as_ptr())
            },
            Commit::Vf { fd, mask, buf, mac } => unsafe {
                ffi::x_commit_vf(self.st.as_mut_ptr(), fd as i32, mask, buf.as_ptr(), mac)
            },
            Commit::Acc { mask, buf, mac } => unsafe {
                ffi::x_commit_acc(self.st.as_mut_ptr(), mask, buf.as_ptr(), mac)
            },
            Commit::Clip(c) => self.st.set_clip(c),
        }
    }

    fn addr(&self, is: u8, imm11: i16) -> u32 {
        (self.vi(is) as i32 + imm11 as i32) as u32
    }

    fn lower(&mut self, b: &Bundle) -> Result<Ctl> {
        let off = b.offset;
        let op = match b.lower {
            LowerSlot::Loi(bits) => {
                // The I bit makes the lower word a float immediate.
                let o = unsafe { ffi::x_off_i() };
                unsafe {
                    std::ptr::write_unaligned(self.st.as_mut_ptr().add(o) as *mut u32, bits)
                };
                return Ok(Ctl::None);
            }
            LowerSlot::Inst(op) => op,
        };
        use LowerOp::*;
        let vu = self.st.as_mut_ptr();
        match op {
            Nop => {}
            Move { dest, ft, fs } => {
                if !dest.is_empty() {
                    unsafe { ffi::x_move(vu, ft.0 as i32, fs.0 as i32, dest.0 as i32) }
                }
            }
            Mr32 { dest, ft, fs } => unsafe {
                ffi::x_mr32(vu, ft.0 as i32, fs.0 as i32, dest.0 as i32)
            },
            Lq { dest, ft, is, imm11 } => {
                let a = self.addr(is.0, imm11);
                let mut buf = [0u8; 16];
                unsafe {
                    ffi::x_lq(self.st.as_ptr(), a, buf.as_mut_ptr());
                    ffi::x_write(self.st.as_mut_ptr(), ft.0 as i32, dest.0 as i32, buf.as_ptr());
                }
            }
            Sq { dest, fs, it, imm11 } => {
                let a = self.addr(it.0, imm11);
                unsafe { ffi::x_sq(self.st.as_mut_ptr(), fs.0 as i32, dest.0 as i32, a) }
            }
            Lqi { dest, ft, is } => {
                let a = self.vi(is.0);
                let mut buf = [0u8; 16];
                unsafe {
                    ffi::x_lq(self.st.as_ptr(), a, buf.as_mut_ptr());
                    ffi::x_write(self.st.as_mut_ptr(), ft.0 as i32, dest.0 as i32, buf.as_ptr());
                }
                self.viset(is.0, a.wrapping_add(1));
            }
            Sqi { dest, fs, it } => {
                let a = self.vi(it.0);
                unsafe { ffi::x_sq(self.st.as_mut_ptr(), fs.0 as i32, dest.0 as i32, a) }
                self.viset(it.0, a.wrapping_add(1));
            }
            Sqd { dest, fs, it } => {
                let a = self.vi(it.0).wrapping_sub(1);
                self.viset(it.0, a);
                unsafe { ffi::x_sq(self.st.as_mut_ptr(), fs.0 as i32, dest.0 as i32, a) }
            }
            Lqd { dest, ft, is } => {
                let a = self.vi(is.0).wrapping_sub(1);
                self.viset(is.0, a);
                let mut buf = [0u8; 16];
                unsafe {
                    ffi::x_lq(self.st.as_ptr(), a, buf.as_mut_ptr());
                    ffi::x_write(self.st.as_mut_ptr(), ft.0 as i32, dest.0 as i32, buf.as_ptr());
                }
            }
            Ilwr { dest, it, is } => {
                let lane = single_lane(off, dest)?;
                let a = self.vi(is.0);
                let v = unsafe { ffi::x_ilw(self.st.as_ptr(), a, lane) };
                self.viset(it.0, v);
            }
            Ilw { dest, it, is, imm11 } => {
                let lane = single_lane(off, dest)?;
                let a = self.addr(is.0, imm11);
                let v = unsafe { ffi::x_ilw(self.st.as_ptr(), a, lane) };
                self.viset(it.0, v);
            }
            Iswr { dest, it, is } => {
                let a = self.vi(is.0);
                let v = self.vi(it.0);
                unsafe { ffi::x_isw(self.st.as_mut_ptr(), a, dest.0 as i32, v) }
            }
            Isw { dest, it, is, imm11 } => {
                let a = self.addr(is.0, imm11);
                let v = self.vi(it.0);
                unsafe { ffi::x_isw(self.st.as_mut_ptr(), a, dest.0 as i32, v) }
            }
            Iadd { id, is, it } => {
                let v = self.vi(is.0).wrapping_add(self.vi(it.0));
                self.viset(id.0, v);
            }
            Isub { id, is, it } => {
                let v = self.vi(is.0).wrapping_sub(self.vi(it.0));
                self.viset(id.0, v);
            }
            Iand { id, is, it } => {
                let v = self.vi(is.0) & self.vi(it.0);
                self.viset(id.0, v);
            }
            Ior { id, is, it } => {
                let v = self.vi(is.0) | self.vi(it.0);
                self.viset(id.0, v);
            }
            Iaddi { it, is, imm5 } => {
                let v = (self.vi(is.0) as i32).wrapping_add(imm5 as i32) as u32;
                self.viset(it.0, v);
            }
            Iaddiu { it, is, imm15 } => {
                let v = self.vi(is.0).wrapping_add(imm15 as u32);
                self.viset(it.0, v);
            }
            Isubiu { it, is, imm15 } => {
                let v = self.vi(is.0).wrapping_sub(imm15 as u32);
                self.viset(it.0, v);
            }
            Mfir { dest, ft, is } => {
                let v = self.vi(is.0);
                unsafe { ffi::x_mfir(self.st.as_mut_ptr(), ft.0 as i32, dest.0 as i32, v) }
            }
            Div { fs, fsf, ft, ftf } => {
                // A new issue commits whatever was outstanding, then starts
                // a fresh 7-cycle window.
                unsafe {
                    ffi::x_q_commit(vu);
                    ffi::x_div(vu, fs.0 as i32, comp_idx(fsf), ft.0 as i32, comp_idx(ftf));
                }
                self.q_latency = 7;
            }
            Rsqrt { fs, fsf, ft, ftf } => {
                unsafe {
                    ffi::x_q_commit(vu);
                    ffi::x_rsqrt(vu, fs.0 as i32, comp_idx(fsf), ft.0 as i32, comp_idx(ftf));
                }
                self.q_latency = 13;
            }
            Waitq => {
                unsafe { ffi::x_q_commit(vu) }
                self.q_latency = 0;
            }
            Rinit { fs, fsf } => {
                let mut buf = [0u8; 16];
                unsafe { ffi::x_vf(self.st.as_ptr(), fs.0 as i32, buf.as_mut_ptr()) };
                let lane = comp_idx(fsf) as usize;
                let bits = u32::from_le_bytes(
                    buf[lane * 4..lane * 4 + 4].try_into().unwrap(),
                );
                unsafe { ffi::x_rinit(self.st.as_mut_ptr(), bits) }
            }
            Rget { dest, ft } => unsafe {
                ffi::x_rget(vu, ft.0 as i32, dest.0 as i32)
            },
            Fsand { it, imm12 } => {
                let v = self.vis_status() & imm12 as u32;
                self.viset(it.0, v);
            }
            Fmand { it, is } => {
                let v = self.vis_mac() & self.vi(is.0);
                self.viset(it.0, v);
            }
            Fcand { imm24 } => {
                let v = if (self.st.clip() & imm24) != 0 { 1 } else { 0 };
                self.viset(1, v);
            }
            Fcor { imm24 } => {
                let v = if ((self.st.clip() | imm24) & 0x00FF_FFFF) == 0x00FF_FFFF {
                    1
                } else {
                    0
                };
                self.viset(1, v);
            }
            Fcget { it } => {
                let v = self.st.clip() & 0xFFF;
                self.viset(it.0, v);
            }
            Fcset { imm24 } => self.st.set_clip(imm24 & 0x00FF_FFFF),
            Xtop { it } => {
                let o = unsafe { ffi::x_off_xtop() };
                let v = unsafe {
                    std::ptr::read_unaligned(self.st.as_ptr().add(o) as *const u32)
                } & 0x3FF;
                self.viset(it.0, v);
            }
            Xitop { it } => {
                let o = unsafe { ffi::x_off_itop() };
                let v = unsafe {
                    std::ptr::read_unaligned(self.st.as_ptr().add(o) as *const u32)
                } & 0x3FF;
                self.viset(it.0, v);
            }
            Xgkick { is } => {
                let a = self.vi(is.0) & 0x3FF;
                unsafe { crate::xgkick(self.st.as_mut_ptr(), a) }
            }
            B { imm11 } => return Ok(Ctl::Jump { target: branch_target(off, imm11) }),
            Bal { it, imm11 } => {
                self.viset(it.0, off / 8 + 2);
                return Ok(Ctl::Jump { target: branch_target(off, imm11) });
            }
            Jr { is } => {
                return Ok(Ctl::Indirect { target: self.cond_vi(is.0) << 3 });
            }
            Ibeq { it, is, imm11 } => {
                let taken = self.cond_vi(it.0) == self.cond_vi(is.0);
                return Ok(Ctl::Cond { taken, target: branch_target(off, imm11) });
            }
            Ibne { it, is, imm11 } => {
                let taken = self.cond_vi(it.0) != self.cond_vi(is.0);
                return Ok(Ctl::Cond { taken, target: branch_target(off, imm11) });
            }
            Ibgtz { is, imm11 } => {
                let taken = ((self.cond_vi(is.0) as u16) as i16) > 0;
                return Ok(Ctl::Cond { taken, target: branch_target(off, imm11) });
            }
            Ibltz { is, imm11 } => {
                let taken = ((self.cond_vi(is.0) as u16) as i16) < 0;
                return Ok(Ctl::Cond { taken, target: branch_target(off, imm11) });
            }
            Iblez { is, imm11 } => {
                let taken = ((self.cond_vi(is.0) as u16) as i16) <= 0;
                return Ok(Ctl::Cond { taken, target: branch_target(off, imm11) });
            }
            Ibgez { is, imm11 } => {
                let taken = ((self.cond_vi(is.0) as u16) as i16) >= 0;
                return Ok(Ctl::Cond { taken, target: branch_target(off, imm11) });
            }
            other => bail!("lower {other:?} at {off:#x} not in the measured census"),
        }
        Ok(Ctl::None)
    }

    /// One bundle: upper calc/commit around the lower half, then the
    /// per-bundle hazard bookkeeping.
    fn bundle(&mut self, b: &Bundle) -> Result<Ctl> {
        self.cur_write = None;
        // calc -> lower -> commit, the same order the emitter uses. The
        // upper half's result and its MAC flags are not visible to this
        // bundle's lower half, which matters for the lower-slot flag reads
        // (fmand, fsand) that feed the next bundle's branch.
        // LOI is not an instruction: the lower word is a float immediate
        // for this pair's upper half, so I must be live before it runs.
        if let LowerSlot::Loi(bits) = b.lower {
            let o = unsafe { ffi::x_off_i() };
            unsafe { std::ptr::write_unaligned(self.st.as_mut_ptr().add(o) as *mut u32, bits) };
        }
        // WAITQ stalls the whole instruction pair until the divider has
        // written Q, so a Q reader in this bundle's upper half sees the new
        // value. The programs pair `mulq ... | waitq` precisely for that.
        if matches!(b.lower, LowerSlot::Inst(LowerOp::Waitq)) {
            self.q_latency = 0;
            unsafe { ffi::x_q_commit(self.st.as_mut_ptr()) };
        }
        let commit = self.upper_calc(b)?;
        let ctl = self.lower(b)?;
        self.apply_commit(commit);
        self.tick_q();
        // Shift the flag history after this bundle's commit has landed.
        for k in 0..self.flag_hist.len() - 1 {
            self.flag_hist[k] = self.flag_hist[k + 1];
        }
        let last = self.flag_hist.len() - 1;
        self.flag_hist[last] = (self.st.mac(), self.st.status());
        self.prev_write = self.cur_write.take();
        Ok(ctl)
    }
}

/// Run a decoded program from `st.pc()` until the E bit, the end of the
/// program, or `budget` bundles.
pub fn run(bundles: &[Bundle], st: &mut VuState, budget: u64) -> Result<Outcome> {
    let n = bundles.len();
    let mut it = Interp {
        st,
        prev_write: None,
        cur_write: None,
        q_latency: 0,
        flag_hist: [(0, 0); 4],
        // The VU flag pipeline is four deep. Both this interpreter and the
        // emitter model it; VU_FLAG_LATENCY=0 turns it off here to measure
        // what it changes.
        flag_latency: std::env::var("VU_FLAG_LATENCY")
            .ok()
            .and_then(|v| v.parse::<usize>().ok())
            .map(|v| v.min(4))
            .unwrap_or(4),
    };
    let mut idx = (it.st.pc() / 8) as usize;
    let mut executed = 0u64;
    let mut cov = vec![0u64; n];
    let mut tr: Vec<u32> = Vec::new();
    let mut hs: Vec<u32> = Vec::new();

    loop {
        if idx >= n {
            return Ok(Outcome { stop: Stop::FellOffEnd, bundles_executed: executed, coverage: cov, trace: tr, hashes: hs });
        }
        if executed >= budget {
            return Ok(Outcome { stop: Stop::Budget, bundles_executed: executed, coverage: cov, trace: tr, hashes: hs });
        }
        let b = &bundles[idx];
        let e_bit = b.flags.e;
        cov[idx] += 1;
        if tr.len() < TRACE_CAP {
            tr.push(b.offset);
            hs.push(it.st.reg_hash());
        }
        let ctl = it.bundle(b)?;
        executed += 1;

        // Only a branch or the E bit has a delay slot; everything else
        // simply falls through to the next bundle.
        let has_delay = e_bit || !matches!(ctl, Ctl::None);
        if !has_delay {
            idx += 1;
            continue;
        }

        let delay_idx = idx + 1;
        if delay_idx >= n {
            return Ok(Outcome { stop: Stop::FellOffEnd, bundles_executed: executed, coverage: cov, trace: tr, hashes: hs });
        }
        let db = &bundles[delay_idx];
        cov[delay_idx] += 1;
        if tr.len() < TRACE_CAP {
            tr.push(db.offset);
            hs.push(it.st.reg_hash());
        }
        let d_ctl = it.bundle(db)?;
        executed += 1;
        // A branch in a delay slot is not in the census; treat it loudly.
        if !matches!(d_ctl, Ctl::None) {
            bail!("branch in the delay slot at {:#x}", db.offset);
        }

        if e_bit {
            let pc = (delay_idx as u32 + 1) * 8;
            it.st.set_pc(pc);
            return Ok(Outcome { stop: Stop::EndBit { pc }, bundles_executed: executed, coverage: cov, trace: tr, hashes: hs });
        }

        idx = match ctl {
            Ctl::None => unreachable!("no delay slot without control transfer"),
            Ctl::Cond { taken, target } => {
                if taken {
                    (target / 8) as usize
                } else {
                    delay_idx + 1
                }
            }
            Ctl::Jump { target } => (target / 8) as usize,
            Ctl::Indirect { target } => (target / 8) as usize,
        };
    }
}
