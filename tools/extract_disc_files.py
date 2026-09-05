#!/usr/bin/env python3
"""Extract named files from an ICO disc image into a local directory.

The translator reads two files off the user's own disc: the boot ELF
SCES_507.60 and SRCFILE.TXT, the objdump listing the development build left
behind. This script copies those out of an image the user supplies and
writes them where config/recomp.toml's [inputs].root points, which is
gitignored. Nothing it writes is committable and nothing about the image is
recorded here.

Usage:
    python3 tools/extract_disc_files.py <image> <output dir> [NAME ...]

Handles a plain 2048-byte-sector ISO 9660 image (.iso) and a 2352-byte-sector
raw dump (.bin from a bin/cue rip), which is the same filesystem with a
16-byte sync header and 288 bytes of error correction around each sector.
Only the root directory is searched: every file this project reads is there.

Deterministic, stdlib only.
"""

import os
import struct
import sys

# (sector size, offset of the 2048-byte user data inside a sector). Mode 1
# and mode 2 form 1 both put the user data 16 bytes in on a raw dump.
LAYOUTS = ((2048, 0), (2352, 16), (2352, 24))
DEFAULT_NAMES = ("SCES_507.60", "SRCFILE.TXT")


def read_sector(f, layout, lba):
    size, off = layout
    f.seek(lba * size + off)
    data = f.read(2048)
    if len(data) != 2048:
        raise EOFError(f"sector {lba} is past the end of the image")
    return data


def find_layout(f):
    """The sector layout whose sector 16 holds a primary volume descriptor."""
    for layout in LAYOUTS:
        try:
            pvd = read_sector(f, layout, 16)
        except EOFError:
            continue
        if pvd[1:6] == b"CD001" and pvd[0] == 1:
            return layout, pvd
    raise SystemExit(
        "not an ISO 9660 image: no primary volume descriptor at sector 16 in "
        "any known sector layout"
    )


def root_entries(f, layout, pvd):
    """Yield (name, lba, size) for every file in the root directory."""
    record = pvd[156:190]
    extent = struct.unpack("<I", record[2:6])[0]
    length = struct.unpack("<I", record[10:14])[0]
    data = b"".join(
        read_sector(f, layout, extent + i) for i in range((length + 2047) // 2048)
    )
    i = 0
    while i < length:
        rec_len = data[i]
        if rec_len == 0:
            # The rest of this sector is padding; directory records never
            # straddle a sector boundary.
            i = (i // 2048 + 1) * 2048
            continue
        rec = data[i : i + rec_len]
        name_len = rec[32]
        name = rec[33 : 33 + name_len].decode("ascii", "replace")
        # ";1" is the ISO 9660 version suffix.
        name = name.split(";")[0]
        yield name, struct.unpack("<I", rec[2:6])[0], struct.unpack("<I", rec[10:14])[0]
        i += rec_len


def extract(image_path, out_dir, names):
    with open(image_path, "rb") as f:
        layout, pvd = find_layout(f)
        table = {name: (lba, size) for name, lba, size in root_entries(f, layout, pvd)}
        missing = [n for n in names if n not in table]
        if missing:
            raise SystemExit(
                f"{image_path}: the root directory has no {', '.join(missing)}. "
                "This is not an ICO PAL disc image (SCES-50760)."
            )
        os.makedirs(out_dir, exist_ok=True)
        for name in names:
            lba, size = table[name]
            out_path = os.path.join(out_dir, name)
            written = 0
            with open(out_path, "wb") as out:
                while written < size:
                    chunk = read_sector(f, layout, lba + written // 2048)
                    out.write(chunk[: min(2048, size - written)])
                    written += 2048
            print(f"{out_path}: {size} bytes")


def main(argv):
    if len(argv) < 3:
        raise SystemExit(__doc__.strip())
    names = argv[3:] or list(DEFAULT_NAMES)
    extract(argv[1], argv[2], names)


if __name__ == "__main__":
    main(sys.argv)
