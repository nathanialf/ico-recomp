#!/usr/bin/env bash
# tools/gen_gs_shaders.sh: compiles the native GS renderer's GLSL (ours, not
# ROM-derived) into committed SPIR-V blobs plus the index header the runtime
# includes.
#
# Inputs   src/runtime/gs/render/shaders/*.comp, *.vert, *.frag
#          and the three headers the compute shaders include verbatim from
#          the C++ side, src/runtime/gs/render/gs_swizzle.h, gs_prim.h and
#          gs_texture.h (GL_GOOGLE_include_directive, -I below)
# Outputs  src/runtime/gs/render/shaders/<name>.spv.inc   one C array each
#          src/runtime/rhi/rhi_shaders.h                  the index, and the
#          SHA-1 of every source this run read, which
#          tools/check_shaders_fresh.py recomputes on every build so a shader
#          edited without rerunning this script cannot ship as a stale blob
#
# Both outputs are committed. This project's standalone build has no runtime
# shader compiler (the same reason tools/gen_overlay_spirv.sh exists for the
# paraLLEl-GS overlay pass), so the SPIR-V is produced ahead of time and
# checked in. Rerun this after changing any shader, gs_swizzle.h, gs_prim.h or
# gs_texture.h.
#
# The HLSL stage below is the second consumer of the same .spv files: it
# cross-compiles them with SPIRV-Cross for the D3D12 backend, writing
#          src/runtime/gs/render/shaders/hlsl/<name>.<stage>.hlsl  committed text
#          src/runtime/rhi/rhi_shaders_hlsl.h                      the index
# It is skipped with a STATUS line when spirv-cross is not installed, because
# a tree without it still has to produce the SPIR-V and still has to build.
#
# The MSL stage is the third consumer, for the Metal backend, writing
#          src/runtime/gs/render/shaders/msl/<name>.<stage>.metal  committed text
#          src/runtime/rhi/rhi_shaders_msl.h                       the index
# It shares the same spirv-cross detection and is skipped with it. Nothing
# about the GLSL has to change for any of the three.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"

SHADER_DIR=src/runtime/gs/render/shaders
INCLUDE_DIR=src/runtime/gs/render
INDEX=src/runtime/rhi/rhi_shaders.h

# name:stage, in the order the index header lists them.
SHADERS=(
    "scanout:comp"
    "raster:comp"
    "shadow:comp"
    "overlay:vert"
    "overlay:frag"
)

# The C++ headers the compute shaders #include verbatim through
# GL_GOOGLE_include_directive, relative to $INCLUDE_DIR. They are inputs to
# every stage below, so they go in the freshness table beside the shaders:
# an edit to one of them with no regeneration leaves the SPIR-V, the HLSL and
# the MSL all stale. gs_prim.h and gs_texture.h both include gs_swizzle.h, so
# these three are the whole set.
SHADER_HEADERS=(
    "gs_swizzle.h"
    "gs_prim.h"
    "gs_texture.h"
)

GLSLANG=""
if command -v glslangValidator >/dev/null 2>&1; then
    GLSLANG="$(command -v glslangValidator)"
    echo "gen_gs_shaders: using glslangValidator from PATH ($GLSLANG)"
else
    echo "gen_gs_shaders: glslangValidator not on PATH, building it from the vendored glslang"
    SRC=third_party/parallel-gs/Granite/third_party/glslang
    BUILD=build/glslang-tools
    GLSLANG="$BUILD/StandAlone/glslangValidator"
    if [[ ! -x "$GLSLANG" ]]; then
        cmake -S "$SRC" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
            -DENABLE_OPT=OFF -DENABLE_HLSL=OFF -DENABLE_GLSLANG_BINARIES=ON \
            -DENABLE_CTEST=OFF -DBUILD_TESTING=OFF
        # The target that produces the glslangValidator binary is named
        # glslang-standalone; see tools/gen_overlay_spirv.sh for the same note.
        cmake --build "$BUILD" --target glslang-standalone -j2
    fi
fi

if [[ ! -x "$GLSLANG" ]]; then
    echo "gen_gs_shaders: no usable glslangValidator at $GLSLANG" >&2
    exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Emits `inline constexpr uint32_t <name>[] = { ... };` from a raw SPIR-V
# binary. inline and not static: the name table below takes the address of
# each array, and a static array in a header gives every translation unit its
# own, which makes the inline table function differ between them.
# SPIR-V is a stream of little-endian 32-bit words regardless of host
# endianness (Vulkan spec, 2.2.1), so it is decoded as such rather than by
# host byte order.
emit_array() {
    local name="$1" spv="$2"
    echo "inline constexpr uint32_t ${name}[] = {"
    python3 - "$spv" <<'PY'
import struct
import sys

data = open(sys.argv[1], "rb").read()
assert len(data) % 4 == 0, "SPIR-V binary size is not a multiple of 4"
words = struct.unpack("<%uI" % (len(data) // 4), data)
for i in range(0, len(words), 8):
    chunk = words[i:i + 8]
    print("    " + ", ".join("0x%08xu" % w for w in chunk) + ",")
PY
    echo "};"
}

symbol_of() {
    # scanout:comp -> kScanoutCompSpv
    local name="$1" stage="$2"
    printf 'k%s%sSpv' \
        "$(tr '[:lower:]' '[:upper:]' <<< "${name:0:1}")${name:1}" \
        "$(tr '[:lower:]' '[:upper:]' <<< "${stage:0:1}")${stage:1}"
}

for entry in "${SHADERS[@]}"; do
    name="${entry%%:*}"
    stage="${entry##*:}"
    src="$SHADER_DIR/$name.$stage"
    out="$SHADER_DIR/$name.$stage.spv.inc"
    if [[ ! -f "$src" ]]; then
        echo "gen_gs_shaders: missing $src" >&2
        exit 1
    fi
    # Vulkan 1.3 is what the RHI requires of a device (rhi/vulkan/rhi_vulkan.h),
    # so that is the environment the SPIR-V targets.
    "$GLSLANG" -V --target-env vulkan1.3 -I"$INCLUDE_DIR" \
        -o "$TMP/$name.$stage.spv" "$src"
    {
        echo "/* $out: SPIR-V for $src, generated by tools/gen_gs_shaders.sh."
        echo " * Ours (MIT), our own shader source, not ROM-derived. Do not edit by hand;"
        echo " * rerun the script after changing the shader, gs_swizzle.h, gs_prim.h or"
        echo " * gs_texture.h."
        echo " */"
        emit_array "$(symbol_of "$name" "$stage")" "$TMP/$name.$stage.spv"
    } > "$out"
    echo "gen_gs_shaders: wrote $out ($(wc -c < "$out") bytes)"
done

# ---- HLSL for the D3D12 backend --------------------------------------------
#
# The same SPIR-V, cross-compiled to shader model 6.0 HLSL and committed as
# plain text under $HLSL_DIR so it can be read in review, plus an index header
# the D3D12 backend includes. Optional: a tree with no spirv-cross still
# produces the SPIR-V and still builds, and the D3D12 backend says which
# command to run when it finds no HLSL and no DXIL.
#
# Only the tool is invoked. Nothing under third_party/parallel-gs is read.
#
# Two flags matter and both are decisions, not defaults:
#
#   --flip-vert-y      on the vertex stages. Vulkan clip space has +Y down and
#                      D3D has +Y up, and the overlay's vertex shader is
#                      written for Vulkan's (see overlay.vert). This makes
#                      SPIRV-Cross negate the clip Y so the two backends put
#                      the picture the same way up.
#   --shader-model 60  DXIL, which is what dxc consumes. Nothing here needs a
#                      later model: no wave operations, no 16-bit types, no
#                      64-bit atomics.
#
# The register rewrite that follows is explained in
# src/runtime/rhi/d3d12/rhi_d3d12_bindings.h, which is the authority for the
# table below.

HLSL_DIR="$SHADER_DIR/hlsl"
HLSL_INDEX=src/runtime/rhi/rhi_shaders_hlsl.h

SPIRV_CROSS=""
if command -v spirv-cross >/dev/null 2>&1; then
    SPIRV_CROSS="$(command -v spirv-cross)"
elif [[ -x build/spirv-cross-tools/spirv-cross ]]; then
    SPIRV_CROSS="build/spirv-cross-tools/spirv-cross"
elif [[ -x third_party/parallel-gs/Granite/third_party/spirv-cross/build/spirv-cross ]]; then
    SPIRV_CROSS="third_party/parallel-gs/Granite/third_party/spirv-cross/build/spirv-cross"
fi

MSL_DIR="$SHADER_DIR/msl"
MSL_INDEX=src/runtime/rhi/rhi_shaders_msl.h

if [[ -z "$SPIRV_CROSS" ]]; then
    echo "gen_gs_shaders: STATUS spirv-cross not found, skipping the HLSL and MSL stages."
    echo "gen_gs_shaders: STATUS the D3D12 backend needs $HLSL_INDEX and the Metal backend"
    echo "gen_gs_shaders: STATUS needs $MSL_INDEX; install spirv-cross"
    echo "gen_gs_shaders: STATUS or build third_party/parallel-gs/Granite/third_party/SPIRV-Cross"
    echo "gen_gs_shaders: STATUS into build/spirv-cross-tools, then rerun."
else
    echo "gen_gs_shaders: using spirv-cross ($SPIRV_CROSS)"
    mkdir -p "$HLSL_DIR"
    for entry in "${SHADERS[@]}"; do
        name="${entry%%:*}"
        stage="${entry##*:}"
        flip=()
        if [[ "$stage" == "vert" ]]; then flip=(--flip-vert-y); fi
        "$SPIRV_CROSS" --hlsl --shader-model 60 "${flip[@]}" \
            --output "$TMP/$name.$stage.hlsl" "$TMP/$name.$stage.spv"
        python3 - "$TMP/$name.$stage.hlsl" "$HLSL_DIR/$name.$stage.hlsl" \
                  "$name.$stage" <<'PY'
import re
import sys

src, dst, ident = sys.argv[1], sys.argv[2], sys.argv[3]

# The authority for this table is
# src/runtime/rhi/d3d12/rhi_d3d12_bindings.h. A declaration whose name is not
# here is an error and not a guess: a resource that lands on the wrong
# register binds nothing and says nothing at run time.
TABLE = {
    "Push":        "b4",   # push constants, the root constants at parameter 0
    "g_buf":       "u0",   # storage buffer array, raw UAVs
    "g_vram":      "u0",
    "g_textures":  "t0",
    "g_samplers":  "s0",
    "g_images":    "u16",  # storage image array, typed UAVs
}

DECL = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[0-9]*\])?\s*:\s*register\(")
REG = re.compile(r"register\(\s*[bstu][0-9]+\s*(?:,\s*space[0-9]+\s*)?\)")
# SPIRV-Cross emits a push constant block as a bare `cbuffer <Block>` with no
# register clause at all, which HLSL then defaults to b0 and which would
# collide with the uniform buffer CBVs at b0-b3. The clause is added here
# rather than left to the default.
CBUFFER = re.compile(r"^cbuffer\s+([A-Za-z_][A-Za-z0-9_]*)\s*(?::\s*register\([^)]*\))?\s*$")


def fail(what):
    sys.stderr.write(
        "gen_gs_shaders: %s declares '%s' at a register the D3D12 root signature "
        "has no place for. Add it to rhi_d3d12_bindings.h and to this table, in "
        "that order.\n" % (ident, what))
    sys.exit(1)


out = []
for line in open(src).read().splitlines():
    cb = CBUFFER.match(line)
    if cb:
        if cb.group(1) not in TABLE:
            fail(cb.group(1))
        line = "cbuffer %s : register(%s, space0)" % (cb.group(1), TABLE[cb.group(1)])
        out.append(line)
        continue
    m = DECL.search(line)
    if m:
        decl = m.group(1)
        if decl not in TABLE:
            fail(decl)
        # A `readonly buffer` in GLSL becomes a read-only ByteAddressBuffer,
        # which HLSL requires to sit at a t register. The storage buffers are
        # one UAV range in the root signature (rhi_d3d12_bindings.h says why),
        # so the read-only slot takes the RW type and never writes.
        if TABLE[decl].startswith("u") and line.lstrip().startswith("ByteAddressBuffer "):
            line = line.replace("ByteAddressBuffer ", "RWByteAddressBuffer ", 1)
        line = REG.sub("register(%s, space0)" % TABLE[decl], line, count=1)
    out.append(line)

# GLSL's extended-arithmetic builtins. SPIRV-Cross emits calls to them by
# name, and HLSL has no intrinsic for any of the three, so the definitions are
# prepended here when a shader uses them. This is the only construct in these
# shaders the cross-compile cannot carry on its own; docs/GS_RENDERER.md
# records it. Nothing in the GLSL changes: gs_prim.h needs a 64-bit product to
# keep the edge function exact, and imulExtended is how it gets one without
# requiring shaderInt64 of any device.
PRELUDE = """
void umulExtended(uint x, uint y, out uint msb, out uint lsb)
{
    uint x0 = x & 0xffffu;
    uint x1 = x >> 16;
    uint y0 = y & 0xffffu;
    uint y1 = y >> 16;
    uint p00 = x0 * y0;
    uint p01 = x0 * y1;
    uint p10 = x1 * y0;
    uint p11 = x1 * y1;
    uint mid = p01 + p10;
    // The bit mid lost on overflow has weight 2 ** 32, which is 2 ** 16 once
    // the middle partial products are shifted into place.
    uint mid_carry = (mid < p01) ? 1u : 0u;
    uint lo = p00 + (mid << 16);
    uint lo_carry = (lo < p00) ? 1u : 0u;
    lsb = lo;
    msb = p11 + (mid >> 16) + (mid_carry << 16) + lo_carry;
}

void imulExtended(int x, int y, out int msb, out int lsb)
{
    uint hi;
    uint lo;
    umulExtended(asuint(x), asuint(y), hi, lo);
    // Two's complement correction: the unsigned product of the two bit
    // patterns exceeds the signed product by y * 2 ** 32 when x is negative
    // and by x * 2 ** 32 when y is negative.
    if (x < 0) hi -= asuint(y);
    if (y < 0) hi -= asuint(x);
    msb = asint(hi);
    lsb = asint(lo);
}

uint uaddCarry(uint x, uint y, out uint carry)
{
    uint sum = x + y;
    carry = (sum < x) ? 1u : 0u;
    return sum;
}
"""

body = "\n".join(out)
prelude = []
if any(name + "(" in body for name in ("umulExtended", "imulExtended", "uaddCarry")):
    prelude = PRELUDE.splitlines()

header = [
    "// %s: HLSL for src/runtime/gs/render/shaders/%s, generated by" % (dst, ident),
    "// tools/gen_gs_shaders.sh from the committed SPIR-V through SPIRV-Cross.",
    "// Ours (MIT), our own shader source, not ROM-derived. Do not edit by hand.",
    "// The registers were rewritten to the convention in",
    "// src/runtime/rhi/d3d12/rhi_d3d12_bindings.h; that header says why.",
    "",
]
open(dst, "w").write("\n".join(header + prelude + out) + "\n")
PY
        echo "gen_gs_shaders: wrote $HLSL_DIR/$name.$stage.hlsl"
    done

    python3 - "$HLSL_INDEX" "$HLSL_DIR" "${SHADERS[@]}" <<'PY'
import sys

index, hlsl_dir = sys.argv[1], sys.argv[2]
entries = [e.split(":") for e in sys.argv[3:]]

TARGET = {"comp": "cs_6_0", "vert": "vs_6_0", "frag": "ps_6_0"}


def literal(text):
    out = []
    for line in text.split("\n"):
        esc = line.replace("\\", "\\\\").replace('"', '\\"')
        out.append('    "%s\\n"' % esc)
    return "\n".join(out)


parts = [
    "/* rhi/rhi_shaders_hlsl.h: the native GS renderer's shaders as HLSL.",
    " *",
    " * Generated by tools/gen_gs_shaders.sh from the committed SPIR-V through",
    " * SPIRV-Cross. Ours (MIT). Do not edit by hand.",
    " *",
    " * The D3D12 backend uses this only when no DXIL is compiled in: dxcompiler.dll",
    " * builds it at run time. See src/runtime/rhi/d3d12/rhi_d3d12_shaders.h.",
    " */",
    "#ifndef ICORECOMP_RHI_SHADERS_HLSL_H",
    "#define ICORECOMP_RHI_SHADERS_HLSL_H",
    "",
    "#include <cstddef>",
    "",
    "namespace rhi {",
    "",
    "struct ShaderHlsl {",
    "    const char* name;",
    "    const char* source;",
    "    const char* entry;",
    "    const char* target;",
    "};",
    "",
]
for name, stage in entries:
    text = open("%s/%s.%s.hlsl" % (hlsl_dir, name, stage)).read()
    parts.append("inline constexpr char kHlsl_%s_%s[] =" % (name, stage))
    parts.append(literal(text.rstrip("\n")) + ";")
    parts.append("")

parts.append("inline const ShaderHlsl* shader_hlsl_table(size_t* count) {")
parts.append("    static const ShaderHlsl kTable[] = {")
for name, stage in entries:
    parts.append('        { "%s.%s", kHlsl_%s_%s, "main", "%s" },'
                 % (name, stage, name, stage, TARGET[stage]))
parts.append("    };")
parts.append("    *count = sizeof(kTable) / sizeof(kTable[0]);")
parts.append("    return kTable;")
parts.append("}")
parts.append("")
parts.append("} // namespace rhi")
parts.append("")
parts.append("#endif /* ICORECOMP_RHI_SHADERS_HLSL_H */")
open(index, "w").write("\n".join(parts) + "\n")
PY
    echo "gen_gs_shaders: wrote $HLSL_INDEX"

    # ---- MSL for the Metal backend -----------------------------------------
    #
    # The same SPIR-V again, cross-compiled to Metal Shading Language 2.3 and
    # committed as plain text under $MSL_DIR so it can be read in review, plus
    # an index header the Metal backend includes. Optional in exactly the way
    # the HLSL stage is.
    #
    # Only the tool is invoked. Nothing under third_party/parallel-gs is read.
    #
    # Two flags matter and both are decisions, not defaults:
    #
    #   --flip-vert-y      on the vertex stages. Metal's clip space is D3D's:
    #                      +Y is up, where Vulkan's is down, and overlay.vert
    #                      is written for Vulkan's. This makes SPIRV-Cross
    #                      negate the clip Y so all three backends put the
    #                      picture the same way up.
    #   --msl-version      20300, which is MSL 2.3. That is what macOS 11 and
    #                      later provide, so it is available on every machine
    #                      this port targets (macOS 14, docs/MACOS.md), and it
    #                      is what the backend pins MTLCompileOptions to. The
    #                      two have to agree or a shader could compile here
    #                      and not on the machine that runs it.
    #
    # No --msl-argument-buffers: the layout fits Metal's direct binding limits
    # and src/runtime/rhi/metal/rhi_metal_bindings.h says why that is the
    # choice. The index rewrite that follows is explained in the same header,
    # which is the authority for the table below.

    mkdir -p "$MSL_DIR"
    for entry in "${SHADERS[@]}"; do
        name="${entry%%:*}"
        stage="${entry##*:}"
        flip=()
        if [[ "$stage" == "vert" ]]; then flip=(--flip-vert-y); fi
        "$SPIRV_CROSS" --msl --msl-version 20300 "${flip[@]}" \
            --output "$TMP/$name.$stage.metal" "$TMP/$name.$stage.spv"
        python3 - "$TMP/$name.$stage.metal" "$MSL_DIR/$name.$stage.metal" \
                  "$name.$stage" <<'PY'
import re
import sys

src, dst, ident = sys.argv[1], sys.argv[2], sys.argv[3]

# The authority for this table is
# src/runtime/rhi/metal/rhi_metal_bindings.h. A resource whose name is not
# here is an error and not a guess: an argument that lands on the wrong index
# reads whatever the backend put there and says nothing at run time.
#
# Each entry is (which of Metal's three argument tables, the base index).
TABLE = {
    "pc":         ("buffer", 20),   # the push constant block's instance name
    "g_buf":      ("buffer", 4),    # storage buffer array (raster.comp)
    "g_vram":     ("buffer", 4),    # storage buffer array (scanout.comp)
    "g_textures": ("texture", 0),
    "g_images":   ("texture", 8),   # storage image array
    "g_samplers": ("sampler", 0),
}

# Metal has no arrays of buffers, so SPIRV-Cross splits one into that many
# entry point arguments named <base>_<element> at consecutive indices and
# rebuilds a local pointer array from them. Texture and sampler arrays stay
# native and take one argument covering a run of indices, so only these two
# need the suffix taken apart.
FLATTENED = ("g_buf", "g_vram")

ARG = re.compile(
    r"([A-Za-z_][A-Za-z0-9_]*)(\s*)\[\[\s*(buffer|texture|sampler)\(\s*(\d+)\s*\)\s*\]\]")
SUFFIX = re.compile(r"^(.*)_([0-9]+)$")


def fail(message):
    sys.stderr.write("gen_gs_shaders: %s: %s\n" % (ident, message))
    sys.exit(1)


seen = {}


def rewrite(m):
    arg, sep, table, old = m.group(1), m.group(2), m.group(3), int(m.group(4))
    base, element = arg, 0
    if base not in TABLE:
        tail = SUFFIX.match(arg)
        if tail and tail.group(1) in FLATTENED:
            base, element = tail.group(1), int(tail.group(2))
    if base not in TABLE:
        fail("argument '%s' sits at %s(%d) and rhi_metal_bindings.h has no index for it. "
             "Add it to that header and to this table, in that order." % (arg, table, old))
    want_table, want_base = TABLE[base]
    if want_table != table:
        fail("argument '%s' came out in Metal's %s table and the convention puts it in %s. "
             "That is a SPIRV-Cross change, not a table entry to edit."
             % (arg, table, want_table))
    index = want_base + element
    key = (table, index)
    if seen.get(key, arg) != arg:
        fail("'%s' and '%s' both want %s(%d)" % (seen[key], arg, table, index))
    seen[key] = arg
    return "%s%s[[%s(%d)]]" % (arg, sep, table, index)


body = ARG.sub(rewrite, open(src).read())

header = [
    "// %s: MSL for src/runtime/gs/render/shaders/%s, generated by" % (dst, ident),
    "// tools/gen_gs_shaders.sh from the committed SPIR-V through SPIRV-Cross.",
    "// Ours (MIT), our own shader source, not ROM-derived. Do not edit by hand.",
    "// The argument indices were rewritten to the convention in",
    "// src/runtime/rhi/metal/rhi_metal_bindings.h; that header says why.",
    "",
]
open(dst, "w").write("\n".join(header) + body.rstrip("\n") + "\n")
PY
        echo "gen_gs_shaders: wrote $MSL_DIR/$name.$stage.metal"
    done

    python3 - "$MSL_INDEX" "$MSL_DIR" "$SHADER_DIR" "${SHADERS[@]}" <<'PY'
import re
import sys

index, msl_dir, shader_dir = sys.argv[1], sys.argv[2], sys.argv[3]
entries = [e.split(":") for e in sys.argv[4:]]

# The threadgroup size. Metal takes it at the dispatch where Vulkan bakes it
# into the shader, so it has to travel with the source. The GLSL's own
# layout(local_size_*) line is the authority; reading it back out of the
# generated MSL would be reading a restatement.
LOCAL = re.compile(
    r"layout\s*\(\s*local_size_x\s*=\s*(\d+)\s*"
    r"(?:,\s*local_size_y\s*=\s*(\d+)\s*)?"
    r"(?:,\s*local_size_z\s*=\s*(\d+)\s*)?\)\s*in\s*;")

# SPIRV-Cross renames the SPIR-V entry point (main becomes main0), so the name
# is read out of the generated source rather than assumed.
ENTRY = re.compile(r"^(?:kernel|vertex|fragment)\b([^(]*)\(")


def fail(message):
    sys.stderr.write("gen_gs_shaders: %s\n" % message)
    sys.exit(1)


def entry_name(path, text):
    for line in text.split("\n"):
        m = ENTRY.match(line)
        if not m:
            continue
        tokens = m.group(1).split()
        if not tokens:
            continue
        return tokens[-1]
    fail("%s has no kernel, vertex or fragment entry point" % path)


def local_size(name, stage):
    if stage != "comp":
        return (0, 0, 0)
    path = "%s/%s.%s" % (shader_dir, name, stage)
    m = LOCAL.search(open(path).read())
    if not m:
        fail("%s declares no layout(local_size_x = ...) and Metal needs the threadgroup "
             "size at the dispatch" % path)
    return (int(m.group(1)), int(m.group(2) or 1), int(m.group(3) or 1))


def literal(text):
    out = []
    for line in text.split("\n"):
        esc = line.replace("\\", "\\\\").replace('"', '\\"')
        out.append('    "%s\\n"' % esc)
    return "\n".join(out)


parts = [
    "/* rhi/rhi_shaders_msl.h: the native GS renderer's shaders as MSL.",
    " *",
    " * Generated by tools/gen_gs_shaders.sh from the committed SPIR-V through",
    " * SPIRV-Cross. Ours (MIT). Do not edit by hand.",
    " *",
    " * The Metal backend compiles these with newLibraryWithSource at run time;",
    " * there is no ahead-of-time .metallib. See",
    " * src/runtime/rhi/metal/rhi_metal_shaders.h for why.",
    " *",
    " * local_size_* is the shader's own threadgroup size, zero for the stages",
    " * that have none. Metal takes it at the dispatch rather than from the",
    " * function, so it travels with the source.",
    " */",
    "#ifndef ICORECOMP_RHI_SHADERS_MSL_H",
    "#define ICORECOMP_RHI_SHADERS_MSL_H",
    "",
    "#include <cstddef>",
    "#include <cstdint>",
    "",
    "namespace rhi {",
    "",
    "struct ShaderMsl {",
    "    const char* name;",
    "    const char* source;",
    "    const char* entry;",
    "    uint32_t local_size_x;",
    "    uint32_t local_size_y;",
    "    uint32_t local_size_z;",
    "};",
    "",
]

table = []
for name, stage in entries:
    path = "%s/%s.%s.metal" % (msl_dir, name, stage)
    text = open(path).read()
    parts.append("inline constexpr char kMsl_%s_%s[] =" % (name, stage))
    parts.append(literal(text.rstrip("\n")) + ";")
    parts.append("")
    sx, sy, sz = local_size(name, stage)
    table.append((name, stage, entry_name(path, text), sx, sy, sz))

parts.append("inline const ShaderMsl* shader_msl_table(size_t* count) {")
parts.append("    static const ShaderMsl kTable[] = {")
for name, stage, entry, sx, sy, sz in table:
    parts.append('        { "%s.%s", kMsl_%s_%s, "%s", %du, %du, %du },'
                 % (name, stage, name, stage, entry, sx, sy, sz))
parts.append("    };")
parts.append("    *count = sizeof(kTable) / sizeof(kTable[0]);")
parts.append("    return kTable;")
parts.append("}")
parts.append("")
parts.append("} // namespace rhi")
parts.append("")
parts.append("#endif /* ICORECOMP_RHI_SHADERS_MSL_H */")
open(index, "w").write("\n".join(parts) + "\n")
PY
    echo "gen_gs_shaders: wrote $MSL_INDEX"
fi

{
    echo "/* rhi/rhi_shaders.h: the native GS renderer's compiled shaders."
    echo " *"
    echo " * Generated by tools/gen_gs_shaders.sh from src/runtime/gs/render/shaders."
    echo " * Ours (MIT). Do not edit by hand."
    echo " *"
    echo " * Each entry is a pointer and a word count, which is what"
    echo " * rhi::Device::create_compute_pipeline and create_graphics_pipeline take."
    echo " */"
    echo "#ifndef ICORECOMP_RHI_SHADERS_H"
    echo "#define ICORECOMP_RHI_SHADERS_H"
    echo
    echo "#include <cstddef>"
    echo "#include <cstdint>"
    echo
    for entry in "${SHADERS[@]}"; do
        name="${entry%%:*}"
        stage="${entry##*:}"
        echo "#include \"../gs/render/shaders/$name.$stage.spv.inc\""
    done
    echo
    echo "namespace rhi {"
    echo
    echo "struct ShaderBlob {"
    echo "    const uint32_t* words;"
    echo "    size_t word_count;"
    echo "};"
    echo
    for entry in "${SHADERS[@]}"; do
        name="${entry%%:*}"
        stage="${entry##*:}"
        sym="$(symbol_of "$name" "$stage")"
        echo "inline ShaderBlob shader_${name}_${stage}() { return ShaderBlob{ $sym, sizeof($sym) / sizeof(uint32_t) }; }"
    done
    echo
    echo "/* The name of each blob, keyed by its SPIR-V words."
    echo " *"
    echo " * The D3D12 and Metal backends identify a shader by comparing the"
    echo " * blob they were handed against these arrays word for word, never by"
    echo " * address: the arrays are static in a header, so every translation"
    echo " * unit holds its own copy at its own address, and the first D3D12 run"
    echo " * failed on exactly that. The table is generated from the"
    echo " * same list the arrays, the HLSL index and the MSL index come from,"
    echo " * so a shader added to the generator reaches all three in one run."
    echo " * Each backend used to keep its own copy of this table by hand, and"
    echo " * both copies were missing shadow.comp, which made those backends"
    echo " * fatal at renderer construction."
    echo " */"
    echo "struct ShaderName {"
    echo "    const uint32_t* spirv;"
    echo "    size_t words;"
    echo "    const char* name;"
    echo "};"
    echo
    echo "inline const ShaderName* shader_name_table(size_t* count) {"
    echo "    static const ShaderName kTable[] = {"
    for entry in "${SHADERS[@]}"; do
        name="${entry%%:*}"
        stage="${entry##*:}"
        sym="$(symbol_of "$name" "$stage")"
        echo "        { $sym, sizeof($sym) / sizeof(uint32_t), \"$name.$stage\" },"
    done
    echo "    };"
    echo "    *count = sizeof(kTable) / sizeof(kTable[0]);"
    echo "    return kTable;"
    echo "}"
    echo
    echo "/* The SHA-1 of every source this run compiled from: each GLSL shader"
    echo " * and the three C++ headers the compute shaders #include verbatim."
    echo " *"
    echo " * Nothing at run time reads these. They exist so that"
    echo " * tools/check_shaders_fresh.py can recompute them and fail the build"
    echo " * when a source has moved on without the generator being rerun. The"
    echo " * SPIR-V here, the HLSL in rhi_shaders_hlsl.h and the MSL in"
    echo " * rhi_shaders_msl.h are all written by one run of the generator, so"
    echo " * one digest per source covers all three: an edit with no"
    echo " * regeneration leaves every one of them stale and mutually"
    echo " * consistent, which is the state nothing else in the tree can see."
    echo " * rhi_shaders_dxil.h carries its own digests of the HLSL, which is"
    echo " * the next link in the same chain; the checker verifies both."
    echo " */"
    echo "struct ShaderSourceDigest {"
    echo "    const char* path; /* relative to the repository root */"
    echo "    const char* sha1;"
    echo "};"
    echo
    echo "inline const ShaderSourceDigest* shader_source_digests(size_t* count) {"
    echo "    static const ShaderSourceDigest kTable[] = {"
    for entry in "${SHADERS[@]}"; do
        name="${entry%%:*}"
        stage="${entry##*:}"
        src="$SHADER_DIR/$name.$stage"
        echo "        { \"$src\", \"$(sha1sum "$src" | cut -d" " -f1)\" },"
    done
    for hdr in "${SHADER_HEADERS[@]}"; do
        echo "        { \"$INCLUDE_DIR/$hdr\", \"$(sha1sum "$INCLUDE_DIR/$hdr" | cut -d" " -f1)\" },"
    done
    echo "    };"
    echo "    *count = sizeof(kTable) / sizeof(kTable[0]);"
    echo "    return kTable;"
    echo "}"
    echo
    echo "} // namespace rhi"
    echo
    echo "#endif /* ICORECOMP_RHI_SHADERS_H */"
} > "$INDEX"

echo "gen_gs_shaders: wrote $INDEX"

# The same list once more, as plain text, for the two DXIL stages
# (tools/gen_gs_shaders_dxil.sh and its Windows twin .ps1). Neither sources
# this script, and a hand-kept second list is how shadow.comp came to be
# missing from the DXIL stage. One `name stage` pair per line, in the order
# the index header lists them.
MANIFEST="$SHADER_DIR/shaders.manifest"
{
    echo "# src/runtime/gs/render/shaders/shaders.manifest: the shader list,"
    echo "# generated by tools/gen_gs_shaders.sh. Do not edit by hand."
    echo "#"
    echo "# tools/gen_gs_shaders_dxil.sh and tools/gen_gs_shaders_dxil.ps1 read it so"
    echo "# the DXIL stage compiles exactly the shaders this script emitted, in the"
    echo "# same order."
    echo "# One 'name stage' pair per line; the shader model follows from the"
    echo "# stage."
    for entry in "${SHADERS[@]}"; do
        echo "${entry%%:*} ${entry##*:}"
    done
} > "$MANIFEST"

echo "gen_gs_shaders: wrote $MANIFEST"

# The DXIL stage, for the D3D12 backend. Skipped with a STATUS line when no
# dxc is on this machine, exactly as the HLSL and MSL stages are skipped
# without spirv-cross: a tree without it still has to produce the SPIR-V and
# still has to build, and the backend still starts by compiling the committed
# HLSL at run time. See tools/gen_gs_shaders_dxil.sh for where dxc is looked
# for and why the ahead-of-time path is the one the package wants.
if [[ -x "${ICORECOMP_DXC:-}" || -x .cache/dxc-linux/bin/dxc ]] \
   || command -v dxc >/dev/null 2>&1; then
    tools/gen_gs_shaders_dxil.sh
else
    echo "gen_gs_shaders: no dxc found, skipping the DXIL stage." \
         "src/runtime/rhi/rhi_shaders_dxil.h is left as it is."
fi
