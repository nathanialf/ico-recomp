#!/usr/bin/env python3
"""Headless verification stats for an ICORECOMP_WAV_CAPTURE output.

Prints per-second RMS (dBFS), peak, and a count of distinct audio events
(contiguous regions above a noise gate). Exit 1 if the file is entirely
silent, so CI-style checks can assert audible output.

Usage: wav_stats.py capture.wav [--gate-db -60]
"""
import argparse
import math
import struct
import sys
import wave


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("--gate-db", type=float, default=-60.0,
                    help="event/noise gate in dBFS (default -60)")
    args = ap.parse_args()

    with wave.open(args.wav, "rb") as w:
        ch = w.getnchannels()
        rate = w.getframerate()
        n = w.getnframes()
        assert w.getsampwidth() == 2, "expected s16 WAV"
        raw = w.readframes(n)
    samples = struct.unpack(f"<{n * ch}h", raw)

    gate = 32768.0 * (10.0 ** (args.gate_db / 20.0))
    print(f"{args.wav}: {n} frames, {ch} ch, {rate} Hz, {n / rate:.2f} s")

    peak_all = 0
    any_signal = False
    for sec in range(0, n, rate):
        chunk = samples[sec * ch:(sec + rate) * ch]
        if not chunk:
            break
        acc = sum(s * s for s in chunk)
        rms = math.sqrt(acc / len(chunk))
        peak = max(abs(s) for s in chunk)
        peak_all = max(peak_all, peak)
        db = 20 * math.log10(rms / 32768.0) if rms > 0 else float("-inf")
        mark = " *" if rms > gate else ""
        print(f"  t={sec // rate:4d}s rms={db:7.2f} dBFS peak={peak:6d}{mark}")
        if rms > gate:
            any_signal = True

    # Distinct events: contiguous 50 ms windows above the gate.
    win = rate // 20
    events = 0
    above = False
    for start in range(0, n, win):
        chunk = samples[start * ch:(start + win) * ch]
        if not chunk:
            break
        acc = sum(s * s for s in chunk)
        rms = math.sqrt(acc / len(chunk))
        now = rms > gate
        if now and not above:
            events += 1
        above = now
    print(f"peak={peak_all} ({20 * math.log10(peak_all / 32768.0) if peak_all else float('-inf'):.2f} dBFS), "
          f"distinct events (>{args.gate_db:.0f} dBFS, 50 ms windows): {events}")
    if not any_signal:
        print("SILENT: no second above the gate", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
