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
