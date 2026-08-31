#!/usr/bin/env python3
"""Compare the runtime's VAG decoder against the decomp repo's reference.

Usage:
  1. Boot the runtime with ICORECOMP_SND_SELFTEST=/tmp/vagtest (any
     untracked prefix; the dumps are ROM-derived and must never be
     committed). The engine writes, at the first key-on:
       /tmp/vagtest.vag  raw VAG bytes from fake SPU RAM
       /tmp/vagtest.s16  our decode of those bytes, mono s16le
  2. python3 src/runtime/snd/tests/vag_compare.py /tmp/vagtest \
       --ref /path/to/ico/tools/decode_vag.py

Runs the reference decoder (MIT, lives in the decomp repo; not copied here)
on the .vag bytes and requires an exact sample-for-sample match with the
.s16 output. Exit 0 on match, 1 on any mismatch.
"""
import argparse
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix", help="selftest dump prefix (expects .vag and .s16)")
    ap.add_argument("--ref", required=True, help="path to the decomp repo's tools/decode_vag.py")
    args = ap.parse_args()

    vag = Path(args.prefix + ".vag")
    ours_path = Path(args.prefix + ".s16")
    if not vag.is_file() or not ours_path.is_file():
        print(f"missing {vag} or {ours_path}; run the runtime with "
              "ICORECOMP_SND_SELFTEST first", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as td:
        # Rename to .bd so the reference treats it as raw blocks
        # (stop_on_end=False), matching the engine's linear selftest decode.
        bd = Path(td) / "sample.bd"
        bd.write_bytes(vag.read_bytes())
        ref_wav = Path(td) / "ref.wav"
        subprocess.run([sys.executable, args.ref, str(bd), "-o", str(ref_wav),
                        "--quiet"], check=True)
        with wave.open(str(ref_wav), "rb") as w:
            assert w.getnchannels() == 1 and w.getsampwidth() == 2
            ref = struct.unpack(f"<{w.getnframes()}h", w.readframes(w.getnframes()))

    raw = ours_path.read_bytes()
    ours = struct.unpack(f"<{len(raw) // 2}h", raw)

    if len(ref) != len(ours):
        print(f"length mismatch: ref {len(ref)} samples, ours {len(ours)}")
        return 1
    bad = [i for i, (a, b) in enumerate(zip(ref, ours)) if a != b]
    if bad:
        i = bad[0]
        print(f"{len(bad)} mismatching samples of {len(ref)}; first at "
              f"#{i}: ref={ref[i]} ours={ours[i]}")
        return 1
    print(f"OK: {len(ref)} samples identical")
    return 0


if __name__ == "__main__":
    sys.exit(main())
