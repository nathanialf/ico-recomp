//! `ee-emit`: translates the EE `.text` of the ICO retail US boot ELF into
//! C11, one file per code translation unit, plus the function table and
//! support headers. See `body` for the translation model and `census` for
//! the coverage policy.
//!
//! Everything written here is ROM-derived and must land under a gitignored
//! generated/ tree; the CLI enforces that before calling `emit_all`.

mod body;
mod census;
pub mod cop2;
mod flow;
mod names;
mod ops;

use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::fmt;
use std::fmt::Write as _;
use std::path::Path;

use anyhow::{bail, Context, Result};
use rayon::prelude::*;

use ingest::{ElfImage, ProgramDb};
use r5900_decode::{decode, Insn};

pub use census::census;

/// Emit the C statement for one straight-line instruction. Test support for
/// the three-way harness in `ee-interp`; errors if the instruction is
/// outside the measured coverage or is only routed (`rt_unimplemented`).
pub fn emit_insn(insn: &Insn) -> Result<String> {
    let mut st = ops::OpStats::default();
    let s = ops::emit_stmt(insn, &mut st)?;
    if let Some((m, _)) = st.unknown.iter().next() {
        bail!("mnemonic {m} outside the measured coverage");
    }
    if let Some((m, _)) = st.unimplemented.iter().next() {
        bail!("mnemonic {m} is routed to rt_unimplemented, not translated");
    }
    if st.invalid_words != 0 {
        bail!("invalid instruction word");
    }
    Ok(s)
}

use body::{Group, GroupStats, Member, Resolver};
use flow::{classify, Flow};

// ------------------------------------------------------------- union-find

struct UnionFind {
    parent: Vec<usize>,
}

impl UnionFind {
    fn new(n: usize) -> Self {
        UnionFind {
            parent: (0..n).collect(),
        }
    }
    fn find(&mut self, mut x: usize) -> usize {
        while self.parent[x] != x {
            self.parent[x] = self.parent[self.parent[x]];
            x = self.parent[x];
        }
        x
    }
    fn union(&mut self, a: usize, b: usize) {
        let (ra, rb) = (self.find(a), self.find(b));
        if ra != rb {
            self.parent[ra] = rb;
        }
    }
}

// ------------------------------------------------------------------ report

pub struct EmitReport {
    pub tu_files: usize,
    pub functions_total: usize,
    pub functions_translated: usize,
    pub functions_outside_text: usize,
    pub vendor_functions: usize,
    pub groups: usize,
    pub merged_groups: Vec<(u32, usize)>,
    pub cross_tu_groups: usize,
    pub stats: GroupStats,
    pub files_written: usize,
}

impl fmt::Display for EmitReport {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        writeln!(f, "ee: {} TU files, {} files written total", self.tu_files, self.files_written)?;
        writeln!(
            f,
            "functions: {} total, {} translated, {} outside .text, {} vendor",
            self.functions_total,
            self.functions_translated,
            self.functions_outside_text,
            self.vendor_functions
        )?;
        writeln!(
            f,
            "instructions: {} (delay slots {}, likely {}, slot re-emits {})",
            self.stats.insns,
            self.stats.delay_slots,
            self.stats.likely_branches,
            self.stats.slot_dups
        )?;
        writeln!(
            f,
            "calls: {} direct, {} indirect, {} tail, {} boundary fallthrough; returns {}",
            self.stats.direct_calls,
            self.stats.indirect_calls,
            self.stats.tail_calls,
            self.stats.fallthrough_calls,
            self.stats.returns
        )?;
        writeln!(
            f,
            "jump tables: {} switches, {} cases",
            self.stats.jtbl_switches, self.stats.jtbl_cases
        )?;
        writeln!(
            f,
            "function groups: {} total, {} merged (branch into another function's interior), \
             {} spanning multiple TUs",
            self.groups,
            self.merged_groups.len(),
            self.cross_tu_groups
        )?;
        for (vram, n) in &self.merged_groups {
            writeln!(f, "  merged group at 0x{vram:X}: {n} functions")?;
        }
        writeln!(f, "invalid data words in .text: {}", self.stats.ops.invalid_words)?;
        let unimpl_total: usize = self.stats.ops.unimplemented.values().sum();
        writeln!(
            f,
            "rt_unimplemented routing: {} mnemonics, {} sites",
            self.stats.ops.unimplemented.len(),
            unimpl_total
        )?;
        for (m, n) in &self.stats.ops.unimplemented {
            writeln!(f, "  {n:8}  {m}")?;
        }
        Ok(())
    }
}

// ------------------------------------------------------------------ driver

pub fn emit_all(db: &ProgramDb, image: &ElfImage, out_dir: &Path) -> Result<EmitReport> {
    let text = db
        .sections
        .iter()
        .find(|s| s.name == ".text")
        .ok_or_else(|| anyhow::anyhow!("no .text section in ELF"))?;
    let text_end = text.vram_end();

    // ---- function identity -------------------------------------------------
    let nfuncs = db.functions.len();
    let in_text: Vec<bool> = db
        .functions
        .iter()
        .map(|f| text.contains_vram(f.vram))
        .collect();

    let mut used: HashSet<String> = HashSet::new();
    let mut fn_ident: Vec<String> = Vec::with_capacity(nfuncs);
    for f in &db.functions {
        let mut id = names::c_ident(&f.name);
        if !used.insert(id.clone()) {
            id = format!("{id}_{:08X}", f.vram);
            if !used.insert(id.clone()) {
                bail!("function identifier collision for {}", f.name);
            }
        }
        fn_ident.push(id);
    }

    let mut entry_by_vram: HashMap<u32, usize> = HashMap::new();
    for (i, f) in db.functions.iter().enumerate() {
        if in_text[i] {
            if f.size == 0 {
                bail!("function {} at 0x{:X} has zero size", f.name, f.vram);
            }
            if entry_by_vram.insert(f.vram, i).is_some() {
                bail!("two functions share entry vram 0x{:X}", f.vram);
            }
        }
    }

    // ---- decode everything once -------------------------------------------
    let decoded: Vec<Vec<Insn>> = db
        .functions
        .par_iter()
        .enumerate()
        .map(|(i, f)| -> Result<Vec<Insn>> {
            if !in_text[i] {
                return Ok(Vec::new());
            }
            let end = (f.vram + f.size).min(text_end);
            let mut v = Vec::with_capacity(((end - f.vram) / 4) as usize);
            let mut vram = f.vram;
            while vram < end {
                let word = image
                    .read_u32(vram)
                    .ok_or_else(|| anyhow::anyhow!("unreadable word at 0x{vram:X}"))?;
                v.push(decode(word, vram));
                vram += 4;
            }
            Ok(v)
        })
        .collect::<Result<Vec<_>>>()?;

    // ---- group functions (branches into another function's interior) ------
    // db.functions is sorted by vram; index lookup by containing range.
    let idx_at = |vram: u32| -> Option<usize> {
        let idx = db.functions.partition_point(|f| f.vram <= vram);
        if idx == 0 {
            return None;
        }
        let f = &db.functions[idx - 1];
        if vram >= f.vram && vram < f.vram + f.size {
            Some(idx - 1)
        } else {
            None
        }
    };

    let mut uf = UnionFind::new(nfuncs);
    for (i, insns) in decoded.iter().enumerate() {
        for insn in insns {
            if let Flow::Branch { target, .. } = classify(insn) {
                if target < text.vram || target >= text_end {
                    bail!(
                        "branch at 0x{:X} in {} targets 0x{target:X}, outside .text",
                        insn.vram,
                        db.functions[i].name
                    );
                }
                let ti = idx_at(target).ok_or_else(|| {
                    anyhow::anyhow!(
                        "branch at 0x{:X} targets 0x{target:X}, not inside any function",
                        insn.vram
                    )
                })?;
                if ti != i && db.functions[ti].vram != target {
                    uf.union(i, ti);
                }
            }
        }
    }

    let mut group_members: BTreeMap<usize, Vec<usize>> = BTreeMap::new();
    for (i, &in_t) in in_text.iter().enumerate().take(nfuncs) {
        if in_t {
            group_members.entry(uf.find(i)).or_default().push(i);
        }
    }

    // ---- jump table ownership ---------------------------------------------
    let name_to_idx: HashMap<&str, usize> = db
        .functions
        .iter()
        .enumerate()
        .map(|(i, f)| (f.name.as_str(), i))
        .collect();
    let mut jtbl_by_fn: HashMap<usize, BTreeSet<u32>> = HashMap::new();
    for jt in &db.jump_tables {
        let &fi = name_to_idx
            .get(jt.owner.as_str())
            .ok_or_else(|| anyhow::anyhow!("jump table {} owner {} unknown", jt.name, jt.owner))?;
        let set = jtbl_by_fn.entry(fi).or_default();
        set.extend(jt.targets.iter().copied().filter(|&t| t != 0));
    }

    // ---- build and emit groups --------------------------------------------
    struct GroupOut {
        tu_index: usize,
        leader_vram: u32,
        member_count: usize,
        cross_tu: bool,
        code: String,
        stats: GroupStats,
    }

    let resolver = Resolver {
        entry_by_vram: &entry_by_vram,
        fn_ident: &fn_ident,
    };

    let groups: Vec<&Vec<usize>> = group_members.values().collect();
    let outputs: Vec<GroupOut> = groups
        .par_iter()
        .map(|members| -> Result<GroupOut> {
            let mut idxs: Vec<usize> = (*members).clone();
            idxs.sort_by_key(|&i| db.functions[i].vram);
            let mut jtbl_targets = BTreeSet::new();
            let g = Group {
                members: idxs
                    .iter()
                    .map(|&i| {
                        let f = &db.functions[i];
                        if let Some(set) = jtbl_by_fn.get(&i) {
                            jtbl_targets.extend(set.iter().copied());
                        }
                        Member {
                            ident: fn_ident[i].clone(),
                            vram: f.vram,
                            end: (f.vram + f.size).min(text_end),
                            insns: &decoded[i],
                        }
                    })
                    .collect(),
                jtbl_targets,
            };
            let leader = idxs[0];
            let tu_index = db.functions[leader].tu_index;
            let cross_tu = idxs.iter().any(|&i| db.functions[i].tu_index != tu_index);
            let (code, stats) = body::emit_group(&g, &resolver).with_context(|| {
                format!("emitting group led by {}", db.functions[leader].name)
            })?;
            Ok(GroupOut {
                tu_index,
                leader_vram: db.functions[leader].vram,
                member_count: idxs.len(),
                cross_tu,
                code,
                stats,
            })
        })
        .collect::<Result<Vec<_>>>()?;

    // ---- coverage gate -----------------------------------------------------
    let mut stats = GroupStats::default();
    for o in &outputs {
        stats.merge(&o.stats);
    }
    if !stats.ops.unknown.is_empty() {
        let list: Vec<String> = stats
            .ops
            .unknown
            .iter()
            .map(|(m, n)| format!("{m} (x{n})"))
            .collect();
        bail!(
            "translation aborted: {} mnemonic(s) outside the measured integer allowlist: {}",
            list.len(),
            list.join(", ")
        );
    }

    // ---- assemble TU files --------------------------------------------------
    let mut by_tu: BTreeMap<usize, Vec<(u32, &str)>> = BTreeMap::new();
    for o in &outputs {
        by_tu
            .entry(o.tu_index)
            .or_default()
            .push((o.leader_vram, o.code.as_str()));
    }

    std::fs::create_dir_all(out_dir)
        .with_context(|| format!("creating {}", out_dir.display()))?;
    // Clear stale generated sources so removed TUs do not linger.
    for entry in std::fs::read_dir(out_dir)? {
        let p = entry?.path();
        if p.extension().is_some_and(|e| e == "c" || e == "h") {
            std::fs::remove_file(&p)?;
        }
    }

    let banner = "/* Generated by icorecomp ee from the SCUS_971.13 boot ELF. Do not edit.\n \
                  * ROM-derived output: this file must never be committed. */\n";

    let files: Vec<(String, String)> = by_tu
        .par_iter()
        .map(|(&tu_index, bodies)| {
            let tu = &db.translation_units[tu_index];
            let mut bodies = bodies.clone();
            bodies.sort_by_key(|(v, _)| *v);
            let mut s = String::new();
            s.push_str(banner);
            let _ = writeln!(
                s,
                "/* TU: {} ({:?}, vram 0x{:X}..0x{:X}) */",
                tu.name, tu.kind, tu.vram_start, tu.vram_end
            );
            s.push_str("#include \"ee/funcs.h\"\n\n");
            for (_, code) in bodies {
                s.push_str(code);
                s.push('\n');
            }
            (format!("{}.c", names::c_ident(&tu.name)), s)
        })
        .collect();

    // ---- funcs.h ------------------------------------------------------------
    let mut funcs_h = String::new();
    funcs_h.push_str(banner);
    funcs_h.push_str(
        "#ifndef GEN_EE_FUNCS_H\n#define GEN_EE_FUNCS_H\n\n#include \"recomp_ops.h\"\n\n\
         #ifdef __cplusplus\nextern \"C\" {\n#endif\n\n\
         /* Fills g_functab_orig with every translated function and copies it\n \
          * into g_functab. The arrays themselves are defined by the runtime\n \
          * (declared extern in recomp_api.h). */\n\
         void g_functab_init(void);\n\n\
         /* One declaration and one CF_ call macro per translated function.\n \
          * Call sites always dispatch through g_functab so rt_override takes\n \
          * effect at every call site. */\n",
    );
    let mut translated: Vec<usize> = (0..nfuncs).filter(|&i| in_text[i]).collect();
    translated.sort_by_key(|&i| db.functions[i].vram);
    for &i in &translated {
        let f = &db.functions[i];
        let _ = writeln!(
            funcs_h,
            "void F_{}(R5900Context* ctx);\n#define CF_{} (g_functab[RECOMP_FUNC_IDX(0x{:X}u)])",
            fn_ident[i], fn_ident[i], f.vram
        );
    }
    funcs_h.push_str("\n#ifdef __cplusplus\n}\n#endif\n\n#endif /* GEN_EE_FUNCS_H */\n");

    // ---- funcs_table.c ------------------------------------------------------
    let mut table_c = String::new();
    table_c.push_str(banner);
    table_c.push_str("#include <string.h>\n#include \"ee/funcs.h\"\n\n");
    let _ = writeln!(
        table_c,
        "/* {} translated functions ({} functions sit outside .text and have\n \
         * no EE translation: .vutext microprogram symbols). */",
        translated.len(),
        nfuncs - translated.len()
    );
    table_c.push_str("void g_functab_init(void) {\n");
    table_c.push_str(
        "    memset(g_functab_orig, 0, sizeof(recomp_fn_t) * RECOMP_FUNCTAB_SLOTS);\n",
    );
    for &i in &translated {
        let f = &db.functions[i];
        let _ = writeln!(
            table_c,
            "    g_functab_orig[RECOMP_FUNC_IDX(0x{:X}u)] = F_{};",
            f.vram, fn_ident[i]
        );
    }
    table_c.push_str(
        "    memcpy(g_functab, g_functab_orig, sizeof(recomp_fn_t) * RECOMP_FUNCTAB_SLOTS);\n}\n",
    );

    // ---- vendor_funcs.h -----------------------------------------------------
    let mut vendor_h = String::new();
    vendor_h.push_str(banner);
    vendor_h.push_str(
        "#ifndef GEN_EE_VENDOR_FUNCS_H\n#define GEN_EE_VENDOR_FUNCS_H\n\n\
         /* Vendor (Sony SDK) functions from ProgramDb's vendor flags, for the\n \
          * runtime team's HLE override list: X(name, vram). */\n",
    );
    let vendor: Vec<usize> = translated
        .iter()
        .copied()
        .filter(|&i| db.functions[i].vendor)
        .collect();
    vendor_h.push_str("#define RECOMP_VENDOR_FUNC_LIST(X) \\\n");
    for (k, &i) in vendor.iter().enumerate() {
        let f = &db.functions[i];
        let sep = if k + 1 == vendor.len() { "" } else { " \\" };
        let _ = writeln!(vendor_h, "    X(\"{}\", 0x{:X}u){sep}", f.name, f.vram);
    }
    let _ = writeln!(vendor_h, "\n#define RECOMP_VENDOR_FUNC_COUNT {}", vendor.len());
    vendor_h.push_str("\n#endif /* GEN_EE_VENDOR_FUNCS_H */\n");

    // ---- write everything ---------------------------------------------------
    let mut files_written = 0usize;
    for (name, content) in files
        .iter()
        .map(|(n, c)| (n.as_str(), c.as_str()))
        .chain([
            ("funcs.h", funcs_h.as_str()),
            ("funcs_table.c", table_c.as_str()),
            ("vendor_funcs.h", vendor_h.as_str()),
        ])
    {
        std::fs::write(out_dir.join(name), content)
            .with_context(|| format!("writing {name}"))?;
        files_written += 1;
    }

    let merged_groups: Vec<(u32, usize)> = outputs
        .iter()
        .filter(|o| o.member_count > 1)
        .map(|o| (o.leader_vram, o.member_count))
        .collect();

    Ok(EmitReport {
        tu_files: by_tu.len(),
        functions_total: nfuncs,
        functions_translated: translated.len(),
        functions_outside_text: nfuncs - translated.len(),
        vendor_functions: vendor.len(),
        groups: outputs.len(),
        merged_groups,
        cross_tu_groups: outputs.iter().filter(|o| o.cross_tu).count(),
        stats,
        files_written,
    })
}
