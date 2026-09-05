#!/usr/bin/env python3
"""tools/check_shaders_fresh.py: the committed shader blobs against their sources.

Two links in one chain, both checked here.

  GLSL -> SPIR-V, HLSL, MSL
      tools/gen_gs_shaders.sh writes all three from one run over the same
      GLSL, and records the SHA-1 of every source it read in
      src/runtime/rhi/rhi_shaders.h. Editing a shader, or one of the three
      C++ headers the compute shaders include verbatim, without rerunning
      the generator leaves the .spv.inc, the .hlsl and the .metal all stale
      and all agreeing with each other, so nothing downstream can see it.

  HLSL -> DXIL
      tools/gen_gs_shaders_dxil.sh (or the .ps1 on Windows) writes
      src/runtime/rhi/rhi_shaders_dxil.h with the SHA-1 of the HLSL each
      container was compiled from. The D3D12 backend matches a shader by the
      content of its SPIR-V and then looks the DXIL up by name, so nothing at
      run time ties a container to the source it came from: rerunning only
      gen_gs_shaders.sh would leave Vulkan on the new shader and D3D12 on the
      old one, with no line in the log.

Both are silent wrongness, which the project rules say must be loud, so they
are made loud here instead. Runs from the build (CMake wires it to the
runtime target) and in CI.

Exit codes: 0 fresh, 1 stale or malformed, 2 nothing to check (neither index
header is present, which is the state of a checkout that has never generated
anything and is not a failure).
"""
import hashlib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SPV_INDEX = os.path.join(ROOT, "src/runtime/rhi/rhi_shaders.h")
DXIL_INDEX = os.path.join(ROOT, "src/runtime/rhi/rhi_shaders_dxil.h")
HLSL_DIR = os.path.join(ROOT, "src/runtime/gs/render/shaders/hlsl")
BLIT_CPP = os.path.join(ROOT, "src/runtime/rhi/d3d12/rhi_d3d12_shaders.cpp")

DXIL_ROW = re.compile(
    r'\{\s*"([^"]+)"\s*,\s*\w+\s*,\s*sizeof\(\w+\)\s*,\s*"([0-9a-fA-F]*)"\s*\}')
SOURCE_ROW = re.compile(r'\{\s*"([^"]+)"\s*,\s*"([0-9a-fA-F]*)"\s*\}')


def sha1_bytes(data):
    return hashlib.sha1(data).hexdigest()


def blit_source():
    """The kBlitHlsl literal, unescaped exactly as the generator unescapes it."""
    text = open(BLIT_CPP, encoding="utf-8").read()
    m = re.search(r'const char kBlitHlsl\[\] =\s*((?:\s*"(?:[^"\\]|\\.)*"\s*)+);', text)
    if not m:
        return None
    # One pass over the escapes, matching the generator exactly: three passes
    # of str.replace would turn a literal backslash followed by an n into a
    # newline, and the two must agree byte for byte or the digest is noise.
    esc = {"n": "\n", "t": "\t", "r": "\r", "0": "\0"}
    out = []
    for lit in re.finditer(r'"((?:[^"\\]|\\.)*)"', m.group(1)):
        out.append(re.sub(r"\\(.)", lambda e: esc.get(e.group(1), e.group(1)), lit.group(1)))
    return "".join(out).encode("utf-8")


def report(stale, what, regen):
    print("check_shaders_fresh: %s" % what, file=sys.stderr)
    for name, where, want, have in stale:
        print("  %-24s %s\n      built from %s\n      the file is now %s"
              % (name, where, want or "(no digest recorded)", have), file=sys.stderr)
    print("Regenerate with %s." % regen, file=sys.stderr)


def check_sources():
    """The GLSL and its two shared headers against rhi_shaders.h's digests."""
    if not os.path.exists(SPV_INDEX):
        return 2, 0
    text = open(SPV_INDEX, encoding="utf-8").read()
    body = text.split("shader_source_digests", 1)
    if len(body) < 2:
        print("check_shaders_fresh: %s carries no shader_source_digests table. Regenerate it"
              " with tools/gen_gs_shaders.sh."
              % os.path.relpath(SPV_INDEX, ROOT), file=sys.stderr)
        return 1, 0
    rows = SOURCE_ROW.findall(body[1])
    if not rows:
        print("check_shaders_fresh: %s has a shader_source_digests table with no rows."
              " Regenerate it with tools/gen_gs_shaders.sh."
              % os.path.relpath(SPV_INDEX, ROOT), file=sys.stderr)
        return 1, 0

    stale = []
    for rel, digest in rows:
        path = os.path.join(ROOT, rel)
        if not os.path.exists(path):
            print("check_shaders_fresh: %s is missing but %s carries a digest for it"
                  % (rel, os.path.relpath(SPV_INDEX, ROOT)), file=sys.stderr)
            return 1, 0
        have = sha1_bytes(open(path, "rb").read())
        if have != digest.lower():
            stale.append((os.path.basename(rel), rel, digest.lower(), have))
    if stale:
        report(stale,
               "the committed SPIR-V, HLSL and MSL are stale against their GLSL sources.",
               "tools/gen_gs_shaders.sh, then tools/gen_gs_shaders_dxil.sh")
        return 1, 0
    return 0, len(rows)


def check_dxil():
    """The committed DXIL against the committed HLSL."""
    if not os.path.exists(DXIL_INDEX):
        return 2, 0
    text = open(DXIL_INDEX, encoding="utf-8").read()
    rows = DXIL_ROW.findall(text)
    if not rows:
        print("check_shaders_fresh: %s carries no shader rows with a source digest."
              " Regenerate it with tools/gen_gs_shaders_dxil.sh (or the .ps1 on Windows)."
              % os.path.relpath(DXIL_INDEX, ROOT), file=sys.stderr)
        return 1, 0

    stale = []
    for name, digest in rows:
        if name in ("blit.vert", "blit.frag"):
            data = blit_source()
            where = "%s kBlitHlsl" % os.path.relpath(BLIT_CPP, ROOT)
            if data is None:
                print("check_shaders_fresh: the kBlitHlsl literal was not found in %s" % where,
                      file=sys.stderr)
                return 1, 0
        else:
            path = os.path.join(HLSL_DIR, name + ".hlsl")
            where = os.path.relpath(path, ROOT)
            if not os.path.exists(path):
                print("check_shaders_fresh: %s is missing but %s carries DXIL for it"
                      % (where, os.path.relpath(DXIL_INDEX, ROOT)), file=sys.stderr)
                return 1, 0
            data = open(path, "rb").read()
        have = sha1_bytes(data)
        if have != digest.lower():
            stale.append((name, where, digest.lower(), have))

    if stale:
        report(stale, "the committed DXIL is stale against the committed HLSL.",
               "tools/gen_gs_shaders_dxil.sh (Linux) or tools/gen_gs_shaders_dxil.ps1 (Windows)")
        return 1, 0
    return 0, len(rows)


def main():
    src_rc, src_n = check_sources()
    dxil_rc, dxil_n = check_dxil()
    if src_rc == 1 or dxil_rc == 1:
        return 1
    if src_rc == 2 and dxil_rc == 2:
        print("check_shaders_fresh: no generated shader index header; nothing to check")
        return 2
    parts = []
    if src_rc == 0:
        parts.append("%d GLSL source(s) match the SPIR-V, HLSL and MSL built from them" % src_n)
    if dxil_rc == 0:
        parts.append("%d DXIL container(s) match their HLSL" % dxil_n)
    print("check_shaders_fresh: %s" % "; ".join(parts))
    return 0


if __name__ == "__main__":
    sys.exit(main())
