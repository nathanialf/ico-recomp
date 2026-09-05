# third_party/patches

Local patches for submodules, applied at configure time. Submodule sources
are never edited in place (LGPL compliance for parallel-gs requires the
pinned sources to stay pristine and replaceable).

Naming: `<submodule>-NNNN-short-description.patch`, produced with
`git -C third_party/<submodule> format-patch`.

## How they are applied

The top-level `CMakeLists.txt` defines `icorecomp_apply_submodule_patches`
and calls it for `parallel-gs` just before `add_subdirectory`. It globs
`<submodule>-NNNN-*.patch`, sorts by name, and for each one:

- `git apply --reverse --check` first. If that succeeds the patch is already
  in the working tree and it is skipped, so re-running cmake in an existing
  build directory changes nothing.
- otherwise `git apply`. A failure is `FATAL_ERROR`: a submodule that
  silently lacks a patch would build and run with the bug the patch exists
  to fix.

The patch files are on `CMAKE_CONFIGURE_DEPENDS`, so editing one re-runs
configure.

The submodule working tree therefore shows as dirty in `git status` once a
build directory has been configured. That is expected. The pinned commit and
the committed sources are unchanged; the diff against them is exactly the
patch files here.

## Adding a patch

1. Edit the submodule tree.
2. Commit inside the submodule with the change and a message describing it.
3. `git -C third_party/<submodule> format-patch -1 --no-signature -o /tmp`
   and move the result here under the naming scheme above.
4. `git -C third_party/<submodule> reset --hard <pinned commit>` so the
   submodule is pristine again. The configure step re-applies the patch.
5. Configure a fresh build directory and check the
   `icorecomp: applied <patch>` status line.
6. Keep each patch's hunks at least three unchanged lines away from every
   other patch's hunks. The "already applied" test is a per-patch
   `git apply --reverse --check` against a tree that carries all of them,
   and a later patch that inserts inside an earlier patch's context lines
   makes that check fail, so the next configure in the same build directory
   stops with a fatal instead of skipping. Check by applying all patches and
   running the reverse check for each one.

## Current patches

### parallel-gs-0001-full-pixel-raster-snap.patch

`GSRenderer::dispatch_shading` halves the vertical raster snap mask
(`snap_raster_mask.y >>= 1`) for field aware rendering, so that snapped
primitives (sprites and points) rasterise at half pixel granularity in Y.
The patch skips the halving when the render pass has super-sampled textures
bound.

Why: with super-sampled textures the per-field content difference already
comes from per-sample attribute interpolation in the ubershader (the
`snap_state == SNAP_RASTER_BIT` branch, reached because `triangle_setup`
clears `SNAP_ATTRIBUTE` for per-sample textures), so the halved mask adds
nothing there. What it does add is per-sub-sample coverage, and that breaks
sprites whose extent ends on a pixel boundary: ICO's display copy spans
y in [-0.25, 223.75), which covers every pixel centre but leaves the lower
vertical sub-sample of row 223 outside the primitive. That sub-sample keeps
whatever its layer held, the pixel's sub-samples then differ, the ubershader
stamps the super-sample reference valid for the pixel, and every later field
reloads the uncovered sub-sample from its own stale layer. High resolution
scanout reads sub-samples directly, so the stale value stays on screen.

Scope: only render passes that both use field aware rendering and have a
super-sampled texture bound. Triangles are unaffected either way (they never
carry `SNAP_RASTER`), and rendering without high resolution scanout never
reaches the halving at all.

### parallel-gs-0002-report-scanout-placement.patch

`ScanoutResult` gains `circuit_enabled`, `circuit_x`, `circuit_y`,
`circuit_width` and `circuit_height`: where each CRTC window ended up in the
output image, in the single-sampled domain, after the CRTC shift and the
horizontal adaptation. `GSRenderer::vsync` fills them from `crtc_rects` just
before it assigns `result.internal_width`.

Why: the shim's placement and crop log lines
(`src/runtime/gs/gs_parallel_scanout.cpp`) need to say whether a display
window fits the mode area or is cropped by it, and the horizontal answer
cannot be re-derived from the registers alone. `adapt_to_internal_horizontal_resolution`
folds both circuits' MAGH and both circuit image widths into `mode_width`,
so a second derivation in the shim would drift from the renderer's. ICO
measured: gameplay lands at (0,0) 512x224 in a 512x224 frame, the attract
movie at (0,0) 720x240 in a 640x224 frame from the same DX and DY.

Scope: reporting only. No existing caller renders anything differently, and
nothing outside the new assignments is touched. Applies on top of 0001.

### parallel-gs-0003-weave-only-deinterlace.patch

`VSyncInfo` gains `weave_only`. When set, `GSRenderer::fastmad_deinterlace`
binds the two most recent fields to both texture pairs
(`vsync_last_fields[i & 1]` instead of `vsync_last_fields[i]`).

Why: the ICO attract movie decodes one picture and splits it on the GS into
an even-row field buffer and an odd-row field buffer, displayed on alternate
fields. Measured, in the retail ELF: `setDispEnv` (0x0025C0B8) puts the two
buffers at pages 0 and 108, one 720x288 PSMCT32 buffer apart, and
`vblankHandler` (0x0025C830) switches DISPFB2's FBP between them on
CSR.FIELD. That the two upload sprites are point sampled 2:1 vertical sprites
with V origins exactly one source row apart is inferred, not measured: their
UV values are computed from `dispSetTags`'s arguments rather than being
literals. See `src/runtime/gs/gs_parallel_scanout.cpp`. A pure weave gives
that pair back as the picture the IPU decoded. Measured afterwards: the movie
is interlaced video (a decoded I frame shows comb teeth on moving figures
inside one picture), so the weave reproduces that comb; the runtime's
deinterlace mode is compiled in as bob (the `display.deinterlace` key was
removed on 2026-09-04), and weave stays in the code for comparison builds.

Scope: one texture binding index. With `uField2 == uField0` and
`uField3 == uField1` every luma difference in `weave.frag` is zero,
`bob_factor` is 0, and the mix returns the previous field verbatim, so the
shipped shader weaves without change and the SPIR-V bank
(`shaders/slangmosh.hpp`) needs no regeneration. Ignored unless deinterlacing
runs. Applies on top of 0002.

### parallel-gs-0004-grow-mode-area-to-circuits.patch

`VSyncInfo` gains `grow_mode_area_to_circuits` and
`ignore_display_buffer_offset`. With the first, `GSRenderer::vsync` grows
`mode_width` and `mode_height` to contain every enabled CRTC window, just
after the horizontal adaptation and before the merged image is created. With
the second, the two `sample_crtc_circuit` calls receive a copy of DISPFB with
DBX and DBY zeroed, so each circuit is read from its buffer's origin; the
register state itself is untouched.

Why: a display window larger than the mode area is cropped to it. The ICO
attract movie programs a 720x240-per-field window from the same DX 636 /
DY 50 as gameplay's 640x224 area, so 80 columns and 16 lines per field are
cut. With the flag the frame becomes 720x240 and the whole window survives.

ICO reads the movie's 720x480 picture from DBX 36 / DBY 12, which puts the
picture's own black borders (40/38 columns, 8/17 rows, measured from a decoded
I frame) flush against the top and right of the window; read from the origin
the picture sits centred inside its borders. Together the two flags are the
runtime's `display.raster = window` mode.

Scope: the merged image, `result.mode_*`, `result.internal_*` and the
deinterlace output, all of which derive from `mode_width` and `mode_height`
after the insertion point, plus the DISPFB copy handed to the sampler. `real_mode_width`, captured before the adaptation
and used for extwrite, is left alone. Windows starting left of or above the
mode area are not handled; they still crop. Applies on top of 0003.

### parallel-gs-0005-skip-deinterlace-dst-layout.patch

`GSRenderer::vsync` adds the barrier the `skip_deinterlace` path was
missing: after the circuit merge the merged field sits in
`READ_ONLY_OPTIMAL` waiting for a deinterlace pass, and only
`fastmad_deinterlace` moved its output to the caller's `dst_layout`. With
`skip_deinterlace` set the pass does not run, so `ScanoutResult.image` came
back in the wrong layout and scope while the result promised `dst_layout`.

Why: the runtime's `display.deinterlace = bob` presents the raw field itself
and blits it as a transfer source, which is what `dst_layout` asks for.
Blitting from an image left in `READ_ONLY_OPTIMAL` is invalid Vulkan usage.

Scope: one `image_barrier` in the else branch of the deinterlace decision,
taken only when deinterlacing would have run. The non-interlaced path already
did the equivalent. Applies on top of 0004.
