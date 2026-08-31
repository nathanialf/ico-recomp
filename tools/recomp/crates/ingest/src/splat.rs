//! Parser for the splat `config/ico.us.yaml` config: walks the single `cod`
//! segment's `subsegments` list and turns it into `TranslationUnit` records.
//!
//! Subsegments come in two shapes:
//!   - array form: `[rom_offset, type, path]`, e.g. `[0x1C80, c, src/delayFreeManager]`
//!   - dict form (bss/sbss only): `{ type: sbss, vram: 0x..., name: ..., align: 0x... }`
//!
//! A type name prefixed with `.` (e.g. `.rodata`) marks a *carved* chunk
//! attributed to a real TU; the bare form (`rodata`) marks an unattributed
//! resume blob owned by a `src/cod/<offset>` placeholder path. Comments are
//! plain YAML `#` comments and need no special handling beyond using a YAML
//! parser.

use anyhow::{bail, Context, Result};
use serde_yaml::Value;
use std::path::Path;

use crate::model::{SubsegKind, TranslationUnit};

struct RawSubseg {
    rom_offset: u32,
    raw_kind: String,
    name: String,
}

struct Segment {
    start: u32,
    vram: u32,
    bss_size: Option<u32>,
    subsegs: Vec<RawSubseg>,
}

pub fn load_translation_units(path: &Path) -> Result<Vec<TranslationUnit>> {
    let text = std::fs::read_to_string(path)
        .with_context(|| format!("reading splat yaml at {}", path.display()))?;
    let doc: Value = serde_yaml::from_str(&text)
        .with_context(|| format!("parsing splat yaml at {}", path.display()))?;

    let segments_val = doc
        .get("segments")
        .with_context(|| format!("{}: no top-level `segments` key", path.display()))?
        .as_sequence()
        .with_context(|| format!("{}: `segments` is not a sequence", path.display()))?;

    let mut segments: Vec<Segment> = Vec::new();
    let mut end_markers: Vec<(usize, u32)> = Vec::new(); // (segment index it follows, offset)

    for item in segments_val {
        match item {
            Value::Mapping(map) => {
                let start =
                    as_u32(map.get(Value::String("start".into())).with_context(|| {
                        format!("{}: segment missing `start`", path.display())
                    })?)?;
                let vram = as_u32(
                    map.get(Value::String("vram".into()))
                        .with_context(|| format!("{}: segment missing `vram`", path.display()))?,
                )?;
                let bss_size = map
                    .get(Value::String("bss_size".into()))
                    .map(as_u32)
                    .transpose()?;
                let subsegs_val = map
                    .get(Value::String("subsegments".into()))
                    .with_context(|| format!("{}: segment missing `subsegments`", path.display()))?
                    .as_sequence()
                    .with_context(|| {
                        format!("{}: `subsegments` is not a sequence", path.display())
                    })?;

                let mut subsegs = Vec::with_capacity(subsegs_val.len());
                for sub in subsegs_val {
                    subsegs.push(parse_subseg(sub, start, vram, path)?);
                }
                segments.push(Segment {
                    start,
                    vram,
                    bss_size,
                    subsegs,
                });
            }
            Value::Sequence(seq) if seq.len() == 1 => {
                // Bare `[offset]` end-of-segment marker.
                let offset = as_u32(&seq[0])?;
                if let Some(idx) = segments.len().checked_sub(1) {
                    end_markers.push((idx, offset));
                }
            }
            other => bail!(
                "{}: unrecognized `segments` entry shape: {other:?}",
                path.display()
            ),
        }
    }

    if segments.is_empty() {
        bail!("{}: no segments parsed", path.display());
    }

    let mut tus = Vec::new();
    for (seg_idx, seg) in segments.iter().enumerate() {
        let segment_end_marker = end_markers
            .iter()
            .find(|(idx, _)| *idx == seg_idx)
            .map(|(_, off)| *off);

        // Sort defensively; splat yaml is already offset-ordered.
        let mut ordered: Vec<&RawSubseg> = seg.subsegs.iter().collect();
        ordered.sort_by_key(|s| s.rom_offset);

        for (i, sub) in ordered.iter().enumerate() {
            let rom_start = sub.rom_offset;
            let rom_end = if let Some(next) = ordered.get(i + 1) {
                next.rom_offset
            } else if let Some(marker) = segment_end_marker.filter(|m| *m > rom_start) {
                marker
            } else if let Some(bss_size) = seg.bss_size {
                rom_start + bss_size
            } else {
                eprintln!(
                    "warning: {}: subsegment {} ({:#x}) has no successor and no \
                     segment end marker or bss_size to bound it; recording zero size",
                    path.display(),
                    sub.name,
                    rom_start
                );
                rom_start
            };

            let vram_start = seg.vram + (rom_start - seg.start);
            let vram_end = seg.vram + (rom_end - seg.start);
            let (kind, carved) = classify(&sub.raw_kind);

            tus.push(TranslationUnit {
                name: sub.name.clone(),
                kind,
                raw_kind: sub.raw_kind.clone(),
                carved,
                rom_start,
                rom_end,
                vram_start,
                vram_end,
            });
        }
    }

    Ok(tus)
}

fn parse_subseg(v: &Value, seg_start: u32, seg_vram: u32, path: &Path) -> Result<RawSubseg> {
    match v {
        Value::Sequence(seq) => {
            if seq.len() < 3 {
                bail!(
                    "{}: subsegment array {:?} has fewer than 3 elements",
                    path.display(),
                    seq
                );
            }
            let rom_offset = as_u32(&seq[0])?;
            let raw_kind = as_str(&seq[1])?.to_string();
            let name = as_str(&seq[2])?.to_string();
            Ok(RawSubseg {
                rom_offset,
                raw_kind,
                name,
            })
        }
        Value::Mapping(map) => {
            // bss/sbss dict form: { type, vram, name, align }
            let raw_kind =
                as_str(map.get(Value::String("type".into())).with_context(|| {
                    format!("{}: dict subsegment missing `type`", path.display())
                })?)?
                .to_string();
            let vram =
                as_u32(map.get(Value::String("vram".into())).with_context(|| {
                    format!("{}: dict subsegment missing `vram`", path.display())
                })?)?;
            let name =
                as_str(map.get(Value::String("name".into())).with_context(|| {
                    format!("{}: dict subsegment missing `name`", path.display())
                })?)?
                .to_string();
            let rom_offset = seg_start + (vram - seg_vram);
            Ok(RawSubseg {
                rom_offset,
                raw_kind,
                name,
            })
        }
        other => bail!(
            "{}: unrecognized subsegment shape: {other:?}",
            path.display()
        ),
    }
}

fn classify(raw_kind: &str) -> (SubsegKind, bool) {
    let carved = raw_kind.starts_with('.');
    let bare = raw_kind.trim_start_matches('.');
    let kind = match bare {
        "c" => SubsegKind::Code,
        "asm" => SubsegKind::Asm,
        "hasm" => SubsegKind::HandAsm,
        "data" => SubsegKind::Data,
        "rodata" => SubsegKind::RoData,
        "lit4" => SubsegKind::Lit4,
        "sdata" => SubsegKind::SData,
        "sbss" => SubsegKind::SBss,
        "bss" => SubsegKind::Bss,
        "textbin" => SubsegKind::TextBin,
        _ => SubsegKind::Other,
    };
    (kind, carved)
}

fn as_u32(v: &Value) -> Result<u32> {
    match v {
        Value::Number(n) => {
            if let Some(u) = n.as_u64() {
                u32::try_from(u).with_context(|| format!("{v:?} does not fit in u32"))
            } else if let Some(i) = n.as_i64() {
                u32::try_from(i).with_context(|| format!("{v:?} does not fit in u32"))
            } else {
                bail!("{v:?} is not an integer")
            }
        }
        Value::String(s) => parse_hex_or_dec(s),
        other => bail!("expected an integer, got {other:?}"),
    }
}

fn parse_hex_or_dec(s: &str) -> Result<u32> {
    let s = s.trim();
    if let Some(hex) = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
        u32::from_str_radix(hex, 16).with_context(|| format!("bad hex integer {s:?}"))
    } else {
        s.parse::<u32>()
            .with_context(|| format!("bad integer {s:?}"))
    }
}

fn as_str(v: &Value) -> Result<&str> {
    v.as_str()
        .with_context(|| format!("expected a string, got {v:?}"))
}
