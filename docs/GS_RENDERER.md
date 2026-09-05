# The native GS renderer

The clean-room replacement for paraLLEl-GS: this project's own Graphics
Synthesizer implementation, on this project's own render hardware interface,
with no LGPL code in the chain.

Everything under `src/runtime/rhi/` and `src/runtime/gs/render/` is MIT and
written from the GS User's Manual's register and memory layouts plus this
repository's own measurements of the retail disc. No file under
`third_party/parallel-gs/gs/` was read while writing it, and neither was
PCSX2. Where a behaviour is inferred rather than measured, the code says so at
the place it is inferred, and this document repeats the list.

## Why

paraLLEl-GS is LGPLv3+, so it has to stay a shared library, pinned as a
submodule, with local changes carried as patch files (see CLAUDE.md). That
works, and it stays as an option, but it puts the part of the port that
decides what the picture looks like behind a boundary this project cannot
change, cannot instrument freely, and cannot ship as one static executable.
The native renderer removes that boundary.

## Status, 2026-09-05

The native renderers are out of the player build. `display.backend` was
withdrawn from `settings.json` on 2026-09-05 and is a retired key: a file
that still carries it produces one info line and the run uses paraLLEl-GS.
There is no menu control for it either. The code stays, and is still built
and covered by CI, for two reasons: the parity gate below, and the replay
tool. The only ways to reach a native renderer now are the environment
variable `ICORECOMP_GS_BACKEND` and the replay tool's own selection.

Everything below describes the renderer as it stands, not as something a
player can switch on.

### Swapchain usage, both renderers

The present path clears the swapchain image, blits the scanout into it and
copies it back out for a screenshot, so the swapchain has to be created with
`VK_IMAGE_USAGE_TRANSFER_DST_BIT` and `VK_IMAGE_USAGE_TRANSFER_SRC_BIT`
beside the colour attachment bit. The Vulkan RHI has always asked for both
(`rhi/vulkan/rhi_vulkan_device.cpp:1466-1467`). The paraLLEl-GS shim did not
until 2026-09-05: Granite's WSI defaults its extra usage to 0, so its
swapchain was colour attachment only and every clear, blit and copy on it was
a usage violation that no validation layer saw, because the shim turns
validation off unless `ICORECOMP_VVL=1`. `init_windowed` now calls
`WSI::set_extra_usage_flags` before the swapchain is built. One consequence
worth knowing: Granite's DXGI interop presenter refuses a swapchain with extra
usage, so an MSVC Windows build now takes the plain Vulkan swapchain. The
shipped mingw cross build never had the interop path.

### What CI actually gates, as of 2026-09-05

Blocking: the four CPU selftests in the `linux` CI job
(`icorecomp-gs-swizzle-selftest`, `icorecomp-gif-decode-selftest`,
`icorecomp-gs-raster-selftest`, `icorecomp-gs-texture-selftest`), plus every
job that compiles the renderer.

Not covered by CI since 2026-09-05: the software-Vulkan and WARP replay
jobs (`gs-shaders-lavapipe`, `gs-shaders-warp`) and the shader regeneration
job (`shader-blobs`) were removed with the native renderer's withdrawal from
the player build; none had ever run green (ubuntu-latest's lavapipe offers
no Vulkan 1.3 instance), so a green CI never covered a shader, a binding
layout, a barrier or a CRTC through them. What CI does check is the committed
SPIR-V, HLSL, MSL and DXIL against the GLSL they came from:
`tools/check_shaders_fresh.py` is an ALL target of every build with Python
(the `linux` job among them). A GPU replay gate needs a runner with a real
device; when one exists, the removed jobs' steps are in the history of
`.github/workflows/ci.yml` at commit 9096334.

Both backends can be built at once. `ICORECOMP_GS` names the transport
(`gs/gs_select.cpp`): `dump`, `parallel`, `both`, `native`. The unset default
is unchanged. Which renderer a live transport builds, and on which graphics
API, is `ICORECOMP_GS_BACKEND`: `auto`, `parallel-gs`, `vulkan`, `d3d12`,
`metal`.

### What removing paraLLEl-GS still needs

Two of the things that used to make the library structural are done.

- **The window is the executable's.** `src/runtime/host/window_service.cpp`
  creates the one SDL3 window of the run, on the main thread, before any GS
  backend exists, with the flags the resolved backend needs
  (`SDL_WINDOW_VULKAN` for paraLLEl-GS and for the Vulkan RHI backend, none
  for D3D12 and Metal). It owns the event pump's notifications, the surface
  size and minimized cache, fullscreen and window sizing, the
  `SDL_Vulkan_CreateSurface` call, and the present rectangle that maps a
  window position onto a guest pixel. Every GS backend is a consumer:
  paraLLEl-GS adopts the window through `RtPgsCreateOptions::host_window` and
  asks the host for its surface, and the native renderer reads the platform
  handles into `rhi::DeviceDesc`. Nothing outside `host/` calls `rt_pgs_*` any
  more.
- **SDL3 is a direct dependency.** `third_party/SDL`, pinned to the commit
  Granite vendors, built once and shared by the executable and the library.
  Deleting `third_party/parallel-gs` no longer takes the window library with
  it.

What is left before the library could actually be dropped is the parity gate
below: the native renderer has not passed it, so `auto` never picks it.

## Layout

    src/runtime/rhi/rhi.h              the interface: devices, buffers,
                                       textures, pipelines, command lists,
                                       swapchain, readback
    src/runtime/rhi/vulkan/            the Vulkan backend (volk + Khronos
                                       headers, both pinned submodules)
    src/runtime/rhi/d3d12/             the D3D12 backend (system d3d12.dll and
                                       dxgi.dll, loaded at run time), Windows
                                       only
    src/runtime/rhi/rhi_shaders.h      generated index of the compiled shaders

    src/runtime/gs/render/gs_swizzle.h        local memory addressing, shared
                                              verbatim by C++ and GLSL
    src/runtime/gs/render/gs_swizzle_reference.cpp
                                              an independent second
                                              implementation, selftest only
    src/runtime/gs/render/gif_decode.h        A+D, PACKED and REGLIST decoding
    src/runtime/gs/render/gs_regs.h           the register file, both contexts
    src/runtime/gs/render/gs_prim.h           the primitive record and the
                                              coverage rules, shared verbatim
                                              by C++ and GLSL
    src/runtime/gs/render/gs_texture.h        the texture registers and the
                                              texture unit's arithmetic, also
                                              shared verbatim
    src/runtime/gs/render/gs_clut.h/.cpp      the CLUT buffer and the CLD
                                              load rules
    src/runtime/gs/render/gs_draw.h/.cpp      primitive assembly, the vertex
                                              queue, the fixed-point setup,
                                              coarse binning
    src/runtime/gs/render/gs_vram.h/.cpp      4 MiB and the three transfer
                                              directions
    src/runtime/gs/render/gs_shadow.h         the super-sampled shadow of
                                              local memory, and which of its
                                              pages mean anything
    src/runtime/gs/render/gs_crtc.h/.cpp      video mode, circuits, frame,
                                              merge, deinterlace
    src/runtime/gs/render/gs_native.cpp       the GsBackend implementation
    src/runtime/gs/render/gs_dump_parse.h/.cpp our own reader for our own dump
                                              format
    src/runtime/gs/render/shaders/            GLSL, compiled ahead of time

    tools/gen_gs_shaders.sh            GLSL to committed SPIR-V blobs
    tools/gs_gen_dump/                 synthetic dumps from our own content

## The RHI

A floor, not a framework. It exposes device creation (headless, or with a
surface built from the native window handles `host/window_service.h` reads out
of SDL),
buffers in three kinds, 2D textures and storage images, four immutable
samplers, compute pipelines, one graphics pipeline kind, command lists with
dispatch, indirect dispatch, copies both ways, blits and barriers, 128 bytes
of push constants, a timeline fence per submit, a swapchain with fifo,
mailbox and immediate, and synchronous readback.

One descriptor set layout, the same for every pipeline: uniform buffer [4],
storage buffer [16], sampled texture [8], sampler [4] immutable, storage image
[4]. Sets are built per dispatch and per draw from the bindings standing at
that moment; nothing is cached.

Deliberately absent, so that a D3D12 or Metal backend is a port and not a
rewrite: buffer device address, descriptor indexing beyond those fixed arrays,
8- and 16-bit storage, subgroup size control, subgroup operations. If a
milestone needs one, it gets added here first, together with the fallback for
the backends that lack it.

Vulkan 1.3 is required. That is what makes dynamic rendering and
synchronization2 core, and building both pre-1.3 paths would double the
backend for hardware this port does not target. A device below 1.3 is skipped
during selection with a log line naming it; no usable device at all is a
fatal naming what was missing.

The window is wired up. `gs_native.cpp` reads the executable's window out of
`host/window_service.h` (`rt_window_native`, `rt_window_surface_size`) into
`DeviceDesc` and creates a device with a swapchain; with no window it creates
one headless, which is what the replay tool and a run with no display take.
The resize arrives as an atomic the window service's sink sets and the
consumer thread applies at the top of its next present, because only the
consumer may touch the swapchain. The rectangle each present blits the picture
into is published back to the service, which is where the UI, the mouse
pointer and the screenshot cropper read it.

## Milestones

- **M0 (this one).** The RHI floor, the shader pipeline, GIF decoding, the
  register file, the swizzle with its selftest, all three transfer directions,
  the CRTC and its two frame modes, bob and weave, the dump parser and the
  replay path. No primitive is drawn: a drawing kick logs once at warn
  ("primitive drawing not implemented in this milestone") and is dropped.
- **(a) Memory and transport.** Included in M0: local memory as a storage
  buffer, page/block/column swizzle for every format, GIF decode, HOST to
  LOCAL, LOCAL to LOCAL and LOCAL to HOST, the register state POD, and the
  CRTC with `display.raster` and `display.deinterlace`.
- **(b) Rasterisation.** Written. All seven primitive kinds, the vertex
  queue and its retention rules, ADC, PRMODECONT, per-primitive context
  selection, XYOFFSET and the 16.4 window coordinate, the scissor, flat and
  Gouraud shading, fog, the Z test and Z write for all four Z formats, the
  alpha test with all four AFAIL modes, the destination alpha test, the
  blend unit with FIX, PABE and COLCLAMP, FBA, FBMSK, and dithering from
  DIMX on the 16-bit colour formats. Textured primitives are rasterised
  with their vertex colour and say so once; AA1 is deferred to (d) and says
  so once. The rules chosen are written down in "Rasterisation rules" below.
- **(c) Texturing.** Written. TEX0, TEX1, TEX2, TEXCLUT, TEXA, CLAMP and
  MIPTBP1/2 for both contexts; UV and perspective-correct STQ coordinates;
  the four clamp modes; every texture pixel format the swizzle addresses,
  including the palettised ones and the Z formats; CSM1 and CSM2 CLUT loads
  with the whole CLD table and its CBP0/CBP1 comparison; TEXA expansion; TCC;
  the four TFX functions; nearest and bilinear filtering; LOD from Q and the
  six MMIN filters with their mip levels, from MIPTBP1/2 or from MTBA's
  automatic packing; and fog after texturing. There is no texture cache to
  invalidate, because texels are read out of local memory in place; what
  replaces it is the page tracker below, which breaks a batch when a
  primitive reads pages the batch has written. The rules chosen are in
  "Texturing rules".
- **(d) Blending and the frame buffer.** Written. ALPHA, the fixed-function
  blend unit, FBA, FRAME masking, PABE, colour clamping and the 16-bit
  destination formats landed with (b). This milestone added AA1's edge
  coverage, SCANMSK's row rule, the line DDA's sub-pixel case and a FRAME and
  a ZBUF that address the same memory. The rules are in "The milestone (d)
  rules" below. The 16-bit FBMSK mapping is still inferred and still waiting
  on a capture.
- **(e) Accuracy work.** The cases a triangle-per-draw renderer gets wrong:
  overlapping primitives that read the frame buffer they are writing,
  interleaved transfers and draws, and the ordering rules between them.
- **(f) Render scale.** Written. `display.render_scale` 1, 4, 8 and 16 as
  sub-samples per pixel with per-sample interpolation and per-sample depth, a
  super-sampled shadow of local memory to hold them between batches, a
  resolve into native local memory after every batch, and a high-resolution
  scanout on a buffer the game drew into. "Render scale" below is the whole
  of it. `display.deinterlace adaptive` landed with it, because it is the
  other thing that changes what a field looks like without changing what the
  hardware would have drawn.
- **(g) Other backends.** The D3D12 backend is written; see "The D3D12
  backend" below for what it does and the list of what has not been run yet.
  Metal only if the macOS probe ever finds a device.

## Rasterisation rules

Every one of these is a decision this renderer had to make, and the reason it
made that one. They are written once in `gs_prim.h` and used by both the C++
assembler and `shaders/raster.comp`, so there is no second copy to drift.

**Coordinates.** A vertex arrives as unsigned 12.4 in XYZ2 or XYZF2. XYOFFSET
is subtracted at assembly, so every coordinate inside the renderer is a signed
16.4 window coordinate: one unit is a sixteenth of a pixel and the whole range
a vertex can name is plus or minus 65535 units. Nothing is snapped, rounded or
clamped on the way in.

**The sample position.** A pixel is sampled once, at its centre, which in 16.4
is `(px * 16 + 8, py * 16 + 8)`. That is the one full-pixel snap rule in the
renderer and everything else follows from it. It is a decision, not a
measurement: it is the choice that makes the sprite rule and the triangle rule
agree at integer coordinates, which is where this game's own geometry sits. A
corner rule (sampling at `px * 16`) agrees there too and differs only for a
primitive at a fractional position, so one captured frame with a
fractional-position sprite would settle it. `tools/gs_gen_dump`'s
`draw-sprites.gs` is the case built to show the difference.

**Sprites.** An axis-aligned rectangle between the two vertices in either
order. A pixel is covered when `x0 <= px*16+8 < x1` and the same in y, which is
the manual's rule that the left and top edges are inclusive and the right and
bottom edges exclusive. A sprite narrower than the distance between two sample
points covers nothing, which is what a sample rule does. Colour and Z come
from the second vertex, flat, whatever IIP says.

**Triangles.** The assembler orders the three vertices so the signed area is
positive; a swap reverses the sign and changes nothing else. Coverage is the
three edge functions

    E(P) = (Bx - Ax) * (Py - Ay) - (By - Ay) * (Px - Ax)

evaluated at the sample point, all three of which must be non-negative. A
sample exactly on an edge is drawn only when that edge is a left edge (its
`dy` is negative, since window coordinates run down the screen) or a top edge
(`dy` is zero and `dx` is positive). That is the top-left rule, and it is what
makes two triangles sharing an edge cover each shared pixel exactly once. The
selftest proves that on a 4x4 grid with masks written out by hand.

E is exact. The products are up to 2^17 by 2^17, which does not fit a 32-bit
int, so both sides do it in 64 bits: `int64_t` in C++, and `imulExtended` in
the shader. `imulExtended` is core GLSL 450 and needs neither an extension nor
the `shaderInt64` device feature, which the RHI deliberately does not require.
Nothing is done in floating point that decides coverage.

**Points.** The pixel the coordinate falls in, `x >> 4`.

**Lines.** Inferred, and the only rule here that is. The manual says a line is
drawn by a DDA along its longer axis and does not say how the minor coordinate
rounds. What is implemented: the major axis is covered by the same sample rule
a sprite uses, and for each covered major pixel the minor coordinate is the
linear interpolation at that sample point, drawn in the pixel it falls in. The
interpolation is set up on the CPU in 16.16 whole pixels, where the division
is available. Milestone (d) added the endpoint convention and the sub-pixel
case; see "The milestone (d) rules".

**Z.** Interpolated with a 32.32 fixed-point DDA whose reference is the
primitive's clipped bounding box origin, so the pixel deltas the shader
multiplies by are never negative and the accumulation is unsigned. The plane
is solved in double on the CPU, which carries 53 bits of mantissa against Z's
32; the fixed-point step quantises at 2^-32 per pixel, so the error over the
2048 pixels of the widest possible drawing area is under 2^-21 of one Z unit.
Z is compared and stored at the width of the ZBUF format, so a 24-bit buffer
never sees the top byte of a 32-bit Z.

**Colour and fog.** Interpolated from the same exact edge values coverage
already computed, converted to float for the barycentric weights. A channel is
8 bits and the weights carry a relative error near 2^-24, so the rounded
result is the integer the exact rational would give except within about 1e-4
of a half. Rounding is to nearest rather than truncating, because that
reproduces a vertex's own colour at that vertex. Z does not go through this
path, which is why the fixed-point DDA above exists.

**Tiles and bins.** Primitives are binned on the CPU into a fixed 64-pixel
coarse grid, 32 by 32 bins over the GS's 2048 by 2048 drawing area. The fine
pass is one workgroup of 256 threads per 16x16 tile; it loads that tile's
colour and depth into threadgroup memory (2 KiB of the 16 KiB budget), walks
its bin's primitive list in submission order, and stores the tile back. One
thread owns one pixel for the life of the workgroup, so no thread reads a
pixel another writes and there is no barrier in the shader at all.

Binning is on the CPU rather than in a compute pass because submission order
has to survive it: appending while assembling keeps the order for nothing,
where a GPU pass would need either an atomic append and a sort by primitive
index, or a count, a prefix sum and a fill. The bounding box is also already
in hand, since assembly computes it to decide whether the primitive survives
the scissor. `gs_draw.cpp` says what would change the answer.

**Batches.** A batch is a run of primitives that share FRAME, ZBUF, FOGCOL and
DIMX, because the fine pass loads one tile of one pair of buffers. Everything
else the pixel pipeline reads travels per primitive in the record: the
scissor, TEST, ALPHA, COLCLAMP, PABE, FBA, DTHE and the PRIM attribute bits.
A context switch therefore costs one dispatch and changes nothing about the
result. The batch is also flushed at a vsync, at a transfer, and at 32768
primitives.

**Where local memory lives.** The host copy is authoritative for everything
the transfer engine does; the device buffer is authoritative for the words the
rasteriser wrote. `gs_native.cpp` tracks the range of the second and reconciles
in one direction at a time, only when something is about to read across the
seam. A frame buffer and a Z buffer that overlap in memory are not handled and
say so once; nothing in this game configures that.

**Sub-word writes.** A colour or depth pixel narrower than a word is written
with an atomic AND of the complement of its mask followed by an atomic OR of
its value. No two threads share a mask bit, so any interleaving of the two
leaves every thread's bits correct and the pair does not have to be atomic
together. FBMSK folds into the same mask, as does PSMCT24's missing top byte.

## Texturing rules

Same discipline as the rasterisation rules: each of these is a decision, with
the reason for it. They are written once in `gs_texture.h` and used by the
C++ side, `shaders/raster.comp` and the selftest.

**One coordinate representation.** Both paths reach the sampler as a signed
integer in sixteenths of a texel. FST 1 is already in that unit, because the
UV register is 14 bits per axis with 4 fractional bits. FST 0 interpolates S,
T and Q per pixel in 32-bit float, divides, multiplies by 2^TW and 2^TH and
rounds to the same sixteenth. One representation rather than two because
every rule below is an integer rule on a texel coordinate: the clamp modes'
AND and OR, the bilinear neighbourhood, the mip level's shift. Whether the
hardware's STQ path carries more than four fractional bits is not stated by
the manual and is not measured, so it is listed as inferred below.

**Clamping.** The manual's four formulas, per axis, applied at the mip
level's own size: REPEAT is `U & (size - 1)`, CLAMP is `clamp(U, 0, size-1)`,
REGION_CLAMP is `clamp(U, MIN, MAX)`, and REGION_REPEAT is `(U & MIN) | MAX`,
where MIN is a bit mask and MAX a pattern rather than an interval. The wrap
runs on each of a bilinear neighbourhood's four texels separately, which is
what makes CLAMP repeat an edge texel into the blend instead of wrapping one
corner to the far side of the texture.

**Bilinear.** The coordinate less half a texel selects the neighbourhood, so
a sample at a texel's centre returns that texel unchanged; the four texels
are weighted by the coordinate's four fractional bits and the sum is rounded
to nearest. The manual gives the four-texel blend and not its rounding; round
to nearest is the same choice the Gouraud DDA made.

**LOD.** `LOD = (log2(1/|Q|) << L) + K` for LCM 0 and `LOD = K` for LCM 1,
carried in sixteenths, which is K's own unit. A negative LOD is magnification
and selects MMAG, which is NEAREST or LINEAR and never reads a mip level. A
LOD of zero or more selects MMIN and its six filters; the level is LOD's
integer part clamped at MXL and the blend between two levels is its fraction.
The manual gives the formula and not that split. In the UV path there is no Q
at all, so the sampler passes Q of one, which makes LOD equal K: the same
answer LCM 1 gives.

**Mip bases.** MIPTBP1 holds levels 1 to 3 and MIPTBP2 levels 4 to 6, each as
its own base and width. With MTBA set the bases are computed instead: each
level starts where the previous one ended, and the width halves per level and
never falls below one 64-texel unit. That packing is what the manual's
description names and not arithmetic it writes out, so it is inferred.

**The CLUT.** One 1 KB buffer, held on the CPU, with the manual's CBP0 and
CBP1 comparison registers. A TEX0 or TEX2 write applies the CLD table: 0
keeps the buffer, 1 loads, 2 and 3 load and remember CBP in CBP0 or CBP1, and
4 and 5 load only when CBP differs from the one they remember and then store
it there. CLD 6 and 7 are not in the table; they load and say so once. The
buffer is loaded from the host copy of local memory, which means the open
batch is drawn and the device buffer read back first when the source pages
are ones the rasteriser has written. Each batch carries the distinct
snapshots its primitives were assembled under, in a storage buffer of their
own, and each record names the word its snapshot starts at; the CLUT changes
far less often than a primitive, so a batch usually carries one.

One buffer and not one per context: the manual gives the CLUT one buffer and
two comparison registers, and two comparisons for one buffer is what makes
CLD 4 and 5 useful. A buffer per context would leave the second comparison
register with nothing to do. `gs_clut.h` says what changes if a capture ever
shows otherwise.

**The CSM1 arrangement.** A 256-entry CSM1 palette is held as a 16 by 16
arrangement, which exchanges bits 3 and 4 of the index: index 8 is at slot
16, index 16 at slot 8. A 16-entry palette has no bit 4 to exchange, so its
entries are consecutive and CSA picks the group of 16 they start at. CSM2 is
linear and takes no permutation; it reads a line of entries at TEXCLUT's COU
and COV in a buffer CBW wide.

**TEXA.** A 16-bit texel takes TA0 when its alpha bit is clear and TA1 when
it is set, and a 24-bit texel takes TA0 always. With AEM set, a texel whose
colour and alpha bit are all zero takes alpha 0 instead. A 16-bit CLUT entry
goes through the same expansion, because it is a 16-bit colour and its alpha
bit means the same thing.

**TFX.** The manual's table, with alpha's one at 0x80 so the products shift
by seven and every result clamps at 255: MODULATE is `(Ct * Cf) >> 7`, DECAL
is `Ct`, and both HIGHLIGHT modes are `(Ct * Cf) >> 7 + Af`. The alpha is
what separates them: MODULATE gives `(At * Af) >> 7`, DECAL and HIGHLIGHT2
give `At`, HIGHLIGHT gives `At + Af`, and TCC 0 gives `Af` in every mode.
Fog is applied after all of this, on the textured colour.

**The page tracker, and why a tile-serial rasteriser still needs one.**
Textures are read out of local memory in place. Inside one 16x16 tile the
fine pass already orders primitives: it walks that tile's list serially and
keeps the tile's colour and depth in threadgroup memory, so a later primitive
sees the earlier one's pixels. But it sees them in that threadgroup copy, and
a texture fetch reads local memory, where they are not yet. And across tiles
there is no ordering at all: two workgroups run at once and neither waits.

So each batch tracks two sets of 8 KiB pages: the pages its primitives write,
from FRAME and ZBUF over each primitive's clipped bounding box, and the pages
a primitive is about to read, from TEX0's base and size, every mip level the
filter can reach, and the CLUT's source. A read that meets a write already in
the batch flushes the batch first, which is the dependency the dispatch
boundary expresses. That is the rule the game's bloom pass needs, where the
frame buffer is drawn back onto itself. `tools/gs_gen_dump`'s
`draw-texture-feedback.gs` is the case built for it.

Page granularity rather than block granularity because a page is the unit the
swizzle's geometry is expressed in, and because a conservative overlap costs
one extra dispatch while a missed one costs a wrong picture.

**TEXFLUSH.** A no-op here, counted and reported. It exists on the hardware
to invalidate the texture cache before the next kick; this renderer has no
texture cache, and the ordering TEXFLUSH is used for is the page tracker's
job, decided from the addresses rather than from the register.

## The milestone (d) rules

**AA1.** PRIM's AA1 bit turns on the GS's antialiasing, which the manual
describes for the line and triangle families and not for points or sprites: the
coverage of a pixel on the edge of the primitive becomes that pixel's alpha, so
that with alpha blending on, an edge pixel is mixed into what was there in
proportion to how much of it the primitive covers. An interior pixel has full
coverage, which on the GS's alpha scale is 0x80, so it blends exactly as it
would with the bit clear.

The rule implemented, stated exactly: the coverage of a pixel is the smallest,
over the primitive's edges, of the area of that pixel's unit square lying on the
interior side of that edge. The per-edge area is the exact closed form for a
half plane clipped to a unit square, which `gs_prim.h` writes out. Its limits,
and they are limits of the rule and not of the code:

- it is exact for a pixel that one edge alone cuts, which is every pixel of an
  edge away from a corner;
- at a corner, where two edges cut one pixel, the true covered area is smaller
  than either half plane's, so the minimum over-estimates. Reproducing that
  exactly means clipping a polygon per pixel, which is not what a coverage unit
  does;
- the quantisation is this renderer's: coverage rounds to the nearest of the 129
  values 0 to 0x80. The manual gives none.

Two more decisions, both inferred and both named in the code. Coverage replaces
the source alpha as the blend unit's C selector reads it, and leaves the alpha
test, the destination alpha test, PABE, FBA and the alpha actually written to
the frame buffer on the primitive's own alpha. And a pixel whose sample point
the ordinary rule rejects but whose coverage is not zero is drawn only when ABE
is set: with blending off there is nothing for a partial coverage to do, and
drawing it would only make the primitive one pixel bigger. The primitive's
bounding box grows by one pixel on each side when AA1 is set, which is what lets
those pixels be reached at all.

At `display.render_scale` above 1 the sub-samples resolve the edge themselves,
so coverage is left at one and an AA1 primitive is blended exactly as an
unantialiased one is. That is a difference between scale 1 and the scales above
it, and it is deliberate: applying both would count the edge twice.

**SCANMSK.** The manual's table, applied per pixel before anything is
interpolated: 0 draws every row, 2 draws only pixels whose Y address is even, 3
only those whose Y address is odd. The Y address is the frame buffer row, which
is the window coordinate the renderer already carries. Value 1 the manual
reserves; it is treated as no mask and named once rather than guessed at.
SCANMSK is a global register and not part of the batch key, so it travels in the
primitive record like the rest of the pixel pipeline's switches.

**Lines, and the sub-pixel case.** The DDA is unchanged from (b): the major axis
is covered where `p * 16 + 8` lies in `[min, max)` of the two endpoints' major
coordinates, and the minor coordinate is the linear interpolation at that sample
point. Two things are stated now that were not. The endpoints: the first is
inclusive and the second exclusive when it lands exactly on a sample point,
which is the same convention the sprite rule states and which the manual states
for a rectangle and not for a line. And a line whose two endpoints fall inside
one pixel of the major axis, which selects no pixel at all under the span rule,
is drawn as the one pixel its first vertex falls in: a DDA emits its current
pixel and then steps, so a segment that never leaves the pixel it started in
still leaves that pixel. Milestone (b) drew nothing there and counted it. Both
are inferred, both are counted, and ICO draws very few lines, so a capture with
one settles them cheaply.

**FRAME and ZBUF in the same memory.** The GS writes both buffers through local
memory, so a colour and a depth that land on the same bits of the same word are
one storage cell that the pixel pipeline writes twice. The rule: the colour is
written first and the depth second, so where the two overlap the depth is what
the memory holds, and the colour a later primitive reads back is those bits read
as a colour. The fine pass reproduces that per pixel: it tests `frame address ==
z address` at the same addressing width, and after each primitive it re-derives
both views of the word from the depth write. The write order is inferred; the
manual has one pixel operation write both buffers and does not say in which
order the two reach memory.

Two cases are outside that model and say so. Two buffers of different widths
over the same word, where a colour and a depth share bits without sharing all of
them, keep the masked atomics that stop them tearing but not the per-pixel
ordering: that one is a warning naming both formats. And a 16-bit format's two
pixels in one word are not an alias at all, because they share a word without
sharing bits; the atomics already handle it and always did.

## Render scale

`display.render_scale` is one setting with two effects, and docs/SETTINGS.md
section 6 is the user-facing description this reproduces. 1, 4, 8 and 16 are the
allowed values; 2 is deliberately absent there and is absent here.

**The samples.** N sub-samples per pixel, placed on the pixel's own regular
subdivision, each at the centre of its sub-cell: 4 is a 2 by 2 grid, 8 is 4 by
2, and 16 is 4 by 4. Every attribute is interpolated at the sample point and
every sample has its own depth, so coverage, the depth test, the alpha test, the
blend and the texture fetch all run per sample. An ordered grid rather than a
rotated one because it is the pattern a resolve averages with equal weights and
the pattern the high-resolution scanout reads as quadrants; a rotated grid gives
better edges on near-horizontal and near-vertical lines and would be a change to
two functions in `gs_prim.h`.

**Where the samples live.** A super-sampled shadow of local memory: N copies of
the whole 4 MiB, one plane per sample, in exactly the native swizzle, so the
word for sample s of native word w is `s * GS_VRAM_WORDS + w` and every rule in
`gs_swizzle.h` is used unaltered. The mapping is the identity rather than an
allocation table keyed by page: an allocator would save memory only for a game
that renders into a small part of local memory, and ICO is not one, since a 512
by 448 PSMCT32 frame buffer is 112 of the 512 pages and its Z buffer another
112. What is kept per page is one bit. The cost is 16 MiB at scale 4, 32 at 8
and 64 at 16, and the startup log line names it.

**When a page's shadow is dropped.** A page is valid when its planes hold that
page's samples. It becomes valid when a seed pass broadcasts the native page
into every plane, which happens for the pages a batch is about to touch that are
not valid yet. It stops being valid when anything writes the native page other
than a draw at this scale: a transfer of either direction, or any other host
write, drops every page the write covered. A change of scale drops all of them,
because the plane count the addresses are built from changed. A read drops
nothing.

A drop is never a correctness problem. Native local memory is resolved after
every batch, so it always holds a whole picture, and a dropped page is seeded
again from it before anything draws into it. What a drop costs is the
sub-sample detail of that page for one batch.

**The resolve.** After every batch, over the rectangle the batch covered:
colour is the average of the N samples rounded to nearest, and depth is the
sample nearest the pixel centre rather than an average, because a depth is a
position on a plane and the average of two surfaces is a value neither of them
has. FBMSK is applied again there, because a mask that protects part of a
channel leaves those bits equal in every sample and an average of the channel
would still disturb them. The consequence, and it is the point: everything the
game reads back reads native local memory at native resolution. A transfer, a
texture fetch, a CLUT load and the CRTC all see the same addresses and the same
geometry they see at scale 1, so a feedback effect stays exact and differs from
scale 1 only by the antialiasing already resolved into it.

**Which pages get seeded.** Both buffers over the whole rectangle the batch
covers, not only the pages its primitives land on. The fine pass loads and
stores every pixel of every tile it dispatches, and the resolve writes native
memory back over the same rectangle, so a page read unseeded would resolve
into a picture nothing drew on. The Z pages are seeded whether or not the batch
writes them, because the depth test reads them even when ZMSK stops the write.

**The tile.** A workgroup is always 256 threads and always one thread per
sample, so the tile shrinks as the sample count grows and the threadgroup arrays
never do: 16 by 16 pixels at scale 1, 8 by 8 at 4, 8 by 4 at 8, 4 by 4 at 16.
The fine pass decomposes `gl_LocalInvocationIndex` into the tile's pixels and
their samples, and at scale 1 that lands on exactly the mapping a 16 by 16
workgroup had, doing exactly the arithmetic it did before. That is what makes
scale 1 byte-identical rather than approximately so.

**High-resolution scanout.** At scale 4 and up, on a buffer the game drew into,
the output frame is twice the frame on each axis and every output pixel is one
quadrant of a pixel's sub-samples, averaged. Double and not more, because two
columns and two rows of samples is what every allowed sample count has.

The condition "a buffer the game drew into" is exactly "every page the enabled
circuits read has a valid shadow". So gameplay and the title screen, which the
game renders straight into the buffer the CRTC displays, take this path at scale
4 and up, and the attract movie, whose two field buffers a transfer fills from
the IPU, does not: a transfer drops the shadow of the pages it wrote, so there
are no sub-samples to scan out and the picture falls back to the native path
with its deinterlacer.

That path is not deinterlaced, because the buffer holds a whole frame. FFMD
does not decide whether it is taken: `choose_hires` tests only that every page
the enabled circuits read has a valid shadow, and `scanout.comp` has an arm for
each setting of FFMD.

With FFMD 1 the whole frame is the two fields interleaved in the buffer: the
output row selects a field line and a parity, and the two together name the
buffer row. That is a weave with no motion test, and it is not a deinterlace
decision, because both sets of rows came out of one rendered frame.

With FFMD 0 each field reads every line of the buffer from the same origin, so
there is no second set of rows. The circuit's window is measured in field
lines, and for an interlaced raster the output image is four rows tall for each
of them, so the buffer line is the output row divided by four and the two
sub-sample rows of that line take the top and bottom halves of the four. Taking
the line as the output row divided by two ran past the end of the circuit's
window and left the bottom half of the picture at BGCOLOR.

The decision is reported the first time it is made and whenever it changes, as
`scanout hires=yes` or `hires=no` with the reason.

**Adaptive deinterlace.** This renderer's own motion filter, and the one mode
here that is not a rule about hardware, since the hardware has no adaptive mode.
Per pixel: a row the current field owns is that field's own pixel. A row it does
not own is standing from the previous field, one field time ago; this field's
rows above and below it say what a bob would put there, and if the standing
pixel is within a threshold of that, the two fields agree about this pixel and
the standing one is kept, which is a weave and gives the full vertical
resolution back. Where they disagree, something moved between the two fields and
the bobbed value replaces it. The threshold is the largest difference of any of
the three colour channels, and it is 24 out of 255: a chosen constant, not a
measured one, and the one number that trades a still picture's detail against a
moving one's shimmer.

The previous field is kept in a storage buffer rather than read back out of the
scanout image, because the RHI only guarantees stores into a storage image and
reading one would ask a typed UAV load of the D3D12 backend that feature level
12_0 does not guarantee. The first field after a mode or size change has no
history and bobs every row it does not own.

It will not byte-match any other renderer, including paraLLEl-GS's own adaptive
mode, and it is excluded from the parity gate for the same reason that one is.

## The parity gate

The native renderer and paraLLEl-GS must produce byte-identical scanout for
the same dump, at `display.render_scale` 1, in both `display.raster` modes and
in both `display.deinterlace` bob and weave. That comparison is what promotes
a milestone from written to trusted:

    icorecomp-gs-replay <dump> --backend=parallel --screenshot a.ppm
    icorecomp-gs-replay <dump> --backend=native   --screenshot b.ppm
    cmp a.ppm b.ppm

Both write the same P6 PPM the existing screenshot path writes: the raw
scanout with alpha dropped and no aspect correction, so the comparison is a
function of the GS output alone.

Excluded from the gate, and why:

- `display.deinterlace adaptive`. paraLLEl-GS's adaptive mode is its own
  motion-adaptive filter and this renderer's is its own. Reproducing another
  renderer's filter bit for bit would be copying a design decision, not
  reproducing hardware; the hardware has no adaptive mode at all. Judged on
  the picture, not on matching.
- `display.render_scale` above 1. Super-sampling is a host-side choice about
  how many samples go into a pixel the hardware only ever produced one of.
  Two renderers can both be right and differ. What is gated instead is that
  the two backends agree with each other at every scale on the same dump, and
  that scale 1 is byte-identical to the picture before render scale existed.
- A dump containing AA1 primitives. The coverage rule above is stated exactly
  and its quantisation is not the hardware's known quantisation, because
  there is no known one: an AA1 edge is compared by eye and by the two
  backends agreeing, until a capture settles the coverage values.
  `draw-aa1-tri.gs` is the case built for it. Everything else in the same
  dump still has to match.

## What runs where

- **Vulkan on the user's Windows GPU.** The only place this renderer is run
  interactively. Builds are cross-compiled with mingw and installed to
  `dist/windows`.
- **Vulkan on lavapipe, on the GitHub ubuntu runner only.** The headless
  replay path and the selftests. Never on the development machine: no software
  Vulkan is run locally, by standing instruction.
- **D3D12 on WARP.** The backend is written and unrun. WARP is what lets CI
  exercise it on `windows-latest` with no GPU; the section "What is verified
  and what is not" says what that would and would not settle.
- **Metal**, only if a macOS machine ever reports a device. The Display tab's
  `Renderer` and `Feature support` lines say what the active backend created;
  `icorecomp-gs-replay --probe` answers the same question without starting the
  game. This project has no macOS hardware.

The selftests need no GPU at all: `icorecomp-gs-swizzle-selftest`,
`icorecomp-gif-decode-selftest`, `icorecomp-gs-raster-selftest` and
`icorecomp-gs-texture-selftest` are pure CPU and run anywhere. The swizzle selftest reports which check covered which
format and names on its last line the one thing it did not cover. The
rasteriser selftest checks coverage masks written out by hand from the rule
above, the vertex queue's retention rules for every primitive kind, PACKED
against REGLIST assembly of the same geometry, and the Z plane solve. The
texture selftest checks the four clamp formulas, the CSM1 arrangement (its
named slots, that it is a permutation of all 256 and that it is its own
inverse), TEXA for the 16- and 24-bit cases with both AEM values, the four
TFX functions with both TCC values, the CLD table driven as a state machine
against a real palette in local memory, the page tracker on synthetic
buffers, the explicit and automatic mip bases, the LOD formula and the
bilinear blend. Every expected value in it is written out by hand from the
manual's formula rather than recorded from a run.

Milestones (d) and (f) added six more cases to the rasteriser selftest, all of
them still CPU only:

- AA1's coverage on edges whose covered area can be written down: a vertical
  and a horizontal edge at four distances each, a 45 degree edge at the three
  points its piecewise form changes shape, an interior pixel of a triangle
  (which must be exactly full coverage, so AA1 changes nothing about it), a
  pixel the hypotenuse passes exactly through (half), and a pixel outside
  every edge (nothing);
- the line DDA on five segments, each with its covered pixels written out from
  the rule: horizontal with the far endpoint exclusive, 45 degrees, steeper
  than 45 degrees so the major axis is y, the horizontal one submitted right to
  left, and one whose endpoints share a pixel of the major axis, which has to
  come out as the one pixel the first vertex falls in and has to be counted;
- SCANMSK's four values against four rows each, and that the register reaches
  the primitive record;
- the aliased word: which configurations alias at all, and what the word holds
  after a colour write followed by a depth write, for a 32-bit pair, a 24-bit
  depth under a 32-bit colour, a 16-bit pair and the case where ZMSK stops the
  depth write;
- the sub-sample grid for 4, 8 and 16 written out position by position, that
  every sample is inside its pixel and no two share a position, that the tile
  fills a 256-thread workgroup at every scale, the sample the resolve takes a
  depth from, and the memory each scale costs;
- the shadow's page state machine: a first batch seeds, a second over the same
  pages seeds nothing, a transfer drops the page it wrote and only that page,
  the dropped page is seeded again, and a scale change drops everything.

## The dump corpus

Two kinds of dump, and only one of them can live in this repository.

**Synthetic dumps** are generated by `tools/gs_gen_dump` from content this
repository produces: gradients this code computes, and register values that
are address facts recorded in this repository's own comments. The generator is
committed. Its output is not, because a binary blob in the tree is a blob
whether or not it is ROM-derived, and `tools/check_no_rom.sh` is a mechanical
gate that does not read intent. Write them somewhere outside the tree:

    icorecomp-gs-gen-dump ~/gs-dumps/synthetic

The cases it writes are one gradient upload per pixel format, the NTSC
512x448 gameplay display setup, the attract movie's 720x480 setup with its
DBX 36 / DBY 12 read offset, a LOCAL to LOCAL case including an overlapping
copy with TRXPOS DIR reversed, and a two-circuit merge.

Milestone (b) adds seven drawing cases, each of which clears the frame buffer
with a full-screen sprite, draws its geometry, and programs the same gameplay
display setup so the replay tool can screenshot it:

    draw-flat-tri.gs      two flat triangles of the same shape submitted with
                          opposite windings, which must come out identical
    draw-gouraud-tri.gs   one Gouraud triangle, a primary at each vertex
    draw-sprites.gs       sixteen sprites stepped a sixteenth of a pixel at a
                          time, plus eight sprites one sixteenth wide: the
                          case that shows what the sample rule decides
    draw-strip.gs         a zigzag triangle strip, six triangles sharing five
                          edges, so a seam or a doubled pixel is visible
    draw-fan.gs           a triangle fan around one centre
    draw-depth.gs         the same overlapping pair drawn far first on the
                          left and near first on the right, with ZTST GREATER
    draw-fbmsk.gs         one sprite over another with FBMSK protecting the
                          green byte

Milestone (c) adds the texture cases, each with its own texture uploaded into
a page clear of the frame and Z buffers:

    draw-texture-<fmt>.gs one 64x64 texture of that pixel format, magnified
                          with nearest filtering; one file for each of
                          PSMCT32, PSMCT24, PSMCT16, PSMCT16S, PSMT8, PSMT4,
                          PSMT8H, PSMT4HL and PSMT4HH
    draw-texture-csm2.gs  the same 8-bit texture through a CSM2 palette: a
                          line of 16-bit entries at TEXCLUT's own offset
    draw-texture-bilinear.gs
                          an 8x8 texture magnified forty times, nearest on the
                          left and linear on the right
    draw-texture-clamp.gs four squares of one texture at four times its size,
                          one per WMS/WMT mode
    draw-texture-mip.gs   one triangle whose Q runs from 1 to 1/8, so LOD runs
                          from 0 to 3 and each level's own colour is a band
    draw-texture-highlight2.gs
                          the four TFX functions side by side
    draw-texture-feedback.gs
                          a sprite that reads the frame buffer pages it draws
                          into, which is what the page tracker breaks the
                          batch for

Milestones (d) and (f) add five more:

    draw-aa1-tri.gs       the same triangle twice, without AA1 on the left and
                          with it on the right, over a flat background and
                          with the blend the coverage alpha feeds, plus one
                          very shallow triangle where a nearly horizontal edge
                          covers many columns per pixel of coverage
    draw-lines.gs         a fan of sixteen lines through both major axes, one
                          line whose endpoints share a pixel, and the same fan
                          again with AA1
    draw-scanmsk.gs       three bands of one sprite each, at SCANMSK 0, 2 and 3
    draw-fbz-alias.gs     a frame buffer and a Z buffer at the same base, with
                          the same triangle drawn once with the depth written
                          and once with ZMSK set
    draw-scale-texture.gs a textured triangle whose three edges are all at
                          awkward angles, over a gradient: the case to compare
                          between display.render_scale 1 and 16

**Captured dumps** are ROM-derived: they are the game's own GIF traffic. They
live outside the repository, at a path the user keeps, and nothing about their
contents is recorded here. What this repository holds is a manifest of names
only, with no hashes and no sizes, so a report can say which capture it used:

    boot-first-fields.gs        the fields between the ELF entry and the first
                                display enable
    attract-movie.gs            a slice of the attract movie, interlaced video
                                through the two field buffers
    title-menu.gs               the title screen with the menu up
    castle-gate.gs              gameplay, the first loading zone either side
    idol-room.gs                gameplay, the save room

## Shaders

GLSL 450 under `src/runtime/gs/render/shaders/`, compiled ahead of time by
`tools/gen_gs_shaders.sh` into committed `.spv.inc` blobs and an index header
`src/runtime/rhi/rhi_shaders.h`. This project's standalone build has no
runtime shader compiler, the same reason `tools/gen_overlay_spirv.sh` exists
for the paraLLEl-GS overlay pass.

    ./tools/gen_gs_shaders.sh

The shaders are `scanout.comp` (the CRTC, the deinterlacers and the
high-resolution path), `raster.comp` (the fine rasteriser), `shadow.comp` (the
seed and resolve passes render scale needs) and the overlay's vertex and
fragment pair.

Rerun it after changing any shader or `gs_swizzle.h`, which the scanout shader
includes. Until it has been run, CMake reports the native renderer as disabled
and names the script; it does not fail the configure, so a fresh tree still
builds everything else.

The index header also carries `shader_name_table()`, the name of each SPIR-V
array keyed by the address of the array. The D3D12 and Metal backends identify
a shader by that address and look its HLSL or MSL up under the name, so the
table has to list every shader the generator emitted. It is generated for that
reason: each backend used to keep its own copy by hand and both copies were
missing `shadow.comp`, which made the renderer fatal at construction on either
of them at any render scale. The script also writes
`src/runtime/gs/render/shaders/shaders.manifest`, which
`tools/gen_gs_shaders_dxil.ps1` reads instead of keeping a fifth list.

The same script has a second stage that cross-compiles the SPIR-V to HLSL for
the D3D12 backend and a third that cross-compiles it to MSL for the Metal
backend. Both are optional and both are skipped together: without
`spirv-cross` on `PATH` the script prints a `STATUS` line naming the two index
headers it did not write, and everything else still runs.

### The drift gate

Every one of those outputs is committed, and nothing that builds this project
regenerates them, so a shader edited without rerunning the generator would
ship three backends' worth of binaries that no longer match their own GLSL,
and nothing would say so. `tools/check_shaders_fresh.py` is what says so: it
runs as an ALL target of every build with a Python interpreter (the `linux`
CI job included) and fails when a source SHA-1 recorded in the index headers
no longer matches the GLSL under `src/runtime/gs/render/shaders`.

The output is bytes, so every tool it uses is pinned. glslang and SPIRV-Cross
are the copies vendored under the paraLLEl-GS submodule, which pins them by
commit; the job deliberately does not `apt-get install glslang-tools`, because
a distribution's version would emit different SPIR-V from the same source.
`dxc` is the Linux asset of release `v1.9.2607`, the release that produced the
seven containers in the tree, looked up through the GitHub release API because
the asset's file name carries a build date rather than the version. Both
optional stages exit zero when their tool is missing, so the job also greps
the generator's own output for the two skip lines and fails on either:
without that, a run that found neither tool would check the SPIR-V alone and
pass.

The job is `continue-on-error` until its first green run. That those pins
reproduce the committed blobs byte for byte is exactly what has not been
measured yet, and the first green run is the measurement.

## The D3D12 backend

`src/runtime/rhi/d3d12/`, built only on Windows and only where CMake finds
`d3d12.h`, `dxgi1_6.h`, `d3d12sdklayers.h` and `dxcapi.h`. Nothing is linked:
`d3d12.dll` and `dxgi.dll` are resolved with `LoadLibrary` and
`GetProcAddress`, for the three reasons volk is used on the Vulkan side. There
is no Agility SDK, so no `D3D12Core.dll` is redistributed and every entry
point used is in the system library on Windows 10 1809 and later.

Feature level 12_0 is the floor. That is what guarantees resource binding
tier 2, and one descriptor table here holds twenty UAVs (sixteen storage
buffers plus four storage images) where tier 1 on a feature level 11.0 device
guarantees eight. Tier 2 is checked and named rather than assumed. Wave
operations, 64-bit atomics and typed UAV loads of the additional formats are
not required: the storage images are written and never read, and a typed UAV
store to `R8G8B8A8_UNORM` is guaranteed at every accepted feature level.

`DeviceDesc::prefer_software` selects the adapter with
`DXGI_ADAPTER_FLAG_SOFTWARE` (WARP), falling back to
`IDXGIFactory4::EnumWarpAdapter` on the Windows versions that do not enumerate
it. It is never a fallback from a failed hardware device: a machine with a
driver that failed says so, because a software rasteriser at a frame a second
looks like a hang rather than a missing driver. `DeviceDesc::validation` turns
on the debug layer, and its message queue is drained into the runtime log
after every submit, which is this backend's equivalent of the Vulkan debug
messenger.

### Binding model

One root signature for every pipeline, mirroring the one Vulkan pipeline
layout. Vulkan has a single descriptor namespace; HLSL has four register
classes with independent numbering, so the mapping is fixed by
`src/runtime/rhi/d3d12/rhi_d3d12_bindings.h` and the HLSL generator rewrites
each declaration's register to match. All in `space0`:

| rhi.h binding | count | HLSL | where |
| --- | --- | --- | --- |
| 0 uniform buffer | 4 | `b0`-`b3` CBV | table offset 0 |
| 1 storage buffer | 16 | `u0`-`u15` raw UAV | table offset 4 |
| 2 sampled texture | 8 | `t0`-`t7` SRV | table offset 20 |
| 3 sampler | 4 | `s0`-`s3` | sampler table, its own heap |
| 4 storage image | 4 | `u16`-`u19` typed UAV | table offset 28 |
| push constants, 128 bytes | 32 DWORDs | `b4` | root parameter 0 |

The storage buffers are UAVs even where a pass only reads them, because
`raster.comp` declares all sixteen slots as one GLSL block array (an array of
blocks cannot mix member layouts) and writes slot 0 through atomics. One HLSL
array is one register class, so the whole array is `RWByteAddressBuffer`.
Push constants sit at `b4` so the root constants and the uniform-buffer CBVs
never share a register.

The four samplers are a descriptor table of their own over a four descriptor
`SAMPLER` heap, written once at device creation and never rewritten, which is
what rhi.h calls immutable. Their filters and address modes mirror the Vulkan
set one for one: nearest/clamp, linear/clamp, nearest/repeat, linear/repeat,
mip point, no LOD clamp.

They were four D3D12 static samplers first, and that is why every graphics
pipeline failed to build with `E_INVALIDARG`. The generated pixel shader
declares one array, `SamplerState g_samplers[4] : register(s0)`, which is a
shader sampler range of four registers, and D3D12 binds a shader resource
range wider than one register from a descriptor table range only: it does not
merge adjacent static samplers into a range that can cover it. `dxc`, which
runs the same check the runtime runs inside
`CreateGraphicsPipelineState`, rejects the pair with

```
Shader sampler descriptor range (RegisterSpace=0, NumDescriptors=4,
BaseShaderRegister=0) is not fully bound in root signature.
```

and accepts it once the four static samplers are one table range. A single
`SamplerState s : register(s0)` is a range of one and a static sampler does
bind it, which is why the compute shaders, which declare no sampler, and the
present blit, which declares two single samplers, never showed it. The
sampler heap is separate from the CBV/SRV/UAV heap because D3D12 has no heap
type that holds both; both are bound for the life of a command list and the
sampler table, being constant, is set once when the list opens.

A table of 32 descriptors is written per dispatch and per draw into the
frame's slice of one shader-visible heap, from the bindings standing at that
moment. Nothing is cached, and every slot is written, including the ones
nothing was bound to: a table with a hole is undefined on tiers 1 and 2, so
the unbound slots take dummy resources, as the Vulkan backend fills its
arrays. There are two dummy buffers rather than one, because a single
resource cannot be in `VERTEX_AND_CONSTANT_BUFFER` for the CBV slots and
`UNORDERED_ACCESS` for the raw UAV slots at the same time.

### Queue, states and copies

One direct queue. Compute runs on it too: every dispatch, draw and copy is
already ordered against the one before it by rhi.h's single recording thread
and single command list, so a second queue would add ownership transfers for
work with no parallelism to find. One allocator and one command list per frame
in flight, two frames. `ID3D12Fence` is the timeline, one value per submit,
with the same semantics rhi.h defines for the Vulkan timeline semaphore.

Barriers map as follows. A transition between two different states is a
transition barrier; a dependency between two shader accesses of the same
resource is a UAV barrier, which is D3D12's spelling of the same edge. There
is no equivalent of Vulkan's `GENERAL` layout: a storage image sits in
`UNORDERED_ACCESS` and a sampled one in the two shader-resource states.
Upload and readback heap resources have a state D3D12 fixes for their whole
life, so a request to move one is a fatal naming it rather than an illegal
barrier.

Two places where D3D12's rules force work Vulkan does not need, both
documented at the code:

- A host-visible storage buffer lives twice. D3D12 forbids
  `ALLOW_UNORDERED_ACCESS` on an upload heap, and the storage buffers are
  UAVs. Such a buffer gets an upload resource the CPU writes and a
  default-heap shadow the shader reads, and `bind_storage_buffer` copies the
  bound range into the shadow. The consequence to know: the shader sees what
  the CPU had written at the moment of the bind, so a write after the bind and
  before the submit would be seen by Vulkan and not by D3D12. Nothing in the
  renderer does that.
- Texture copies are padded. D3D12 pads each row to 256 bytes and places the
  footprint on a 512-byte boundary, while rhi.h's contract is a tightly packed
  buffer. Where the two agree the copy is direct; otherwise it goes through a
  device scratch buffer and is packed or unpacked one row at a time. That
  lands on the overlay texture upload and the screenshot path, neither of
  which is per field. `read_texture` unpacks on the CPU instead, since it is
  already synchronous.

`dispatch_indirect` uses a command signature with one dispatch argument and a
12-byte stride, which is the same three words a `VkDispatchIndirectCommand`
holds, so the caller's buffer is laid out identically for both backends.

`blit_texture` is a draw and not a copy: `CopyTextureRegion` neither scales nor
filters, so the present blit is one full-screen triangle from `SV_VertexID`,
placed by the viewport, with the source in sampled texture slot 0 and the
filter chosen by a root constant. Its shader is the backend's own, written in
HLSL inside `rhi_d3d12_shaders.cpp`, because there is no GLSL twin to
cross-compile: Vulkan does this with `vkCmdBlitImage`.

### Present modes

DXGI has no present modes, so rhi.h's three become a sync interval and a
present flag on a flip-discard swapchain with three buffers, created through
`IDXGIFactory2::CreateSwapChainForHwnd`. None of the three is exactly its
Vulkan namesake:

| rhi.h | DXGI | what it actually does |
| --- | --- | --- |
| `fifo` | `SyncInterval 1`, no flag | one present per vertical blank, `Present` blocks when the queue is full. Matches Vulkan FIFO. |
| `mailbox` | `SyncInterval 0`, no flag | the compositor takes the most recently completed present and discards the ones behind it. Not identical: DXGI still ties the change to a vertical blank, Vulkan's mailbox need not. |
| `immediate` | `SyncInterval 0` plus `DXGI_PRESENT_ALLOW_TEARING` | tearing, where the adapter reports `DXGI_FEATURE_PRESENT_ALLOW_TEARING` and the swapchain carries `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`. Where that is unsupported it falls back to mailbox's flags with a log line, because tearing is the only thing immediate adds. |

Because all three are Present arguments, `set_present_mode` does not rebuild
the swapchain. That is a difference from the Vulkan backend, where the mode is
baked into the swapchain object. The swapchain also carries
`DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` and
`acquire_backbuffer` waits on it: that is the flip model's back pressure, and
without it a fast renderer queues frames the compositor has not shown and the
present latency grows without bound. D3D12 has no acquire semaphore, so
`acquire_backbuffer` also waits on the fence value of the submit that last
wrote that buffer.

The swapchain format is `R8G8B8A8_UNORM` rather than the `B8G8R8A8` a Vulkan
surface usually hands back. Both are flip-model formats, and RGBA is the RHI's
own `Format` order, so readback needs no channel swap.

### Shader path

Three sources, in the order the backend tries them, with the one it used
logged at info the first time a pipeline is built:

1. `src/runtime/rhi/rhi_shaders_dxil.h`, committed signed DXIL. This is the
   shipping path: the executable then loads no shader compiler at all.
   `tools/gen_gs_shaders_dxil.sh` writes it on Linux and
   `tools/gen_gs_shaders_dxil.ps1` on Windows, and `tools/gen_gs_shaders.sh`
   calls the first one at the end of its run when a `dxc` is on the machine.

   A driver rejects unsigned DXIL outside developer mode, so this only works
   if what the generator writes is signed. It is, on both platforms: a
   DirectX Shader Compiler release carries the hashing code that produces the
   container signature (`libLLVMDxilHash`, and `lib/libdxil.so` beside
   `lib/libdxcompiler.so`). The script does not take that on trust. It reads
   the sixteen bytes after the `DXBC` magic of each container it wrote, prints
   them, and fails the run if they are all zero. The seven containers in the
   tree today were produced by v1.9.2607 on Linux and all seven carry a
   non-zero hash.
2. `src/runtime/rhi/rhi_shaders_hlsl.h`, committed HLSL from
   `tools/gen_gs_shaders.sh`, compiled at run time through `dxcompiler.dll`.
   A fallback for a tree with no compiled-in DXIL, or a shader changed since
   the DXIL was written. `rhi_d3d12_loader.cpp` loads `dxil.dll` first and
   `dxcompiler.dll` second, both by full path from the executable's own
   directory with `LOAD_WITH_ALTERED_SEARCH_PATH`; `dxcompiler.dll` then finds
   the validator by base name in the process and signs what it compiles.
   `rhi_d3d12_shaders.cpp` checks the container hash of the result and warns
   when it is zero, because the failure that follows an unsigned blob is
   `CreateComputePipelineState` returning `E_INVALIDARG` and saying nothing.

   Both DLLs are installed beside `ico.exe` by CMake when `ICORECOMP_DXC_DIR`
   names a directory holding them (it defaults to `.cache/dxc/bin/x64`, and a
   configure with neither says so at STATUS). Both import `MSVCP140.dll`,
   `VCRUNTIME140.dll` and `VCRUNTIME140_1.dll`, so a machine with no Visual
   C++ redistributable fails their load with `ERROR_MOD_NOT_FOUND` (126) even
   with the files in place. The loader puts the path it tried and the Win32
   error into the message for exactly that reason.
3. Neither present: a fatal naming both commands. Both headers are pulled in
   with `__has_include`, so a tree that has run no generator still builds.

A shader is identified by the SPIR-V array the caller passed. Every one of
those pointers comes from an accessor in the generated `rhi_shaders.h`, so the
array address is a stable identity that costs nothing in rhi.h; a blob from
anywhere else is a fatal naming the pipeline rather than a silent guess.

Two flags in the cross-compile are decisions and not defaults.
`--flip-vert-y` on the vertex stages: Vulkan clip space has +Y down and D3D
has +Y up, and `overlay.vert` is written for Vulkan's, so SPIRV-Cross negates
the clip Y and both backends put the picture the same way up.
`--shader-model 60` because that is what `dxc` consumes, and nothing here
needs a later model.

The vertex input semantics are `TEXCOORD0`, `TEXCOORD1` and `TEXCOORD2` and
not `POSITION`/`TEXCOORD`/`COLOR`, because SPIRV-Cross names a location
decoration `TEXCOORDN`. The generator leaves that default alone, so the input
layout in `rhi_d3d12_device.cpp` and the generated vertex shader agree by
construction.

### GLSL portability

Every construct the four shaders use, and how it crosses:

- `imulExtended`, `umulExtended` and `uaddCarry` (`gs_prim.h`): the one place
  the cross-compile needs help, and measured, not guessed. SPIRV-Cross emits
  calls to all three **by name**, and HLSL has no intrinsic for any of them, so
  `raster.comp.hlsl` would not compile. The generator prepends portable
  definitions (a 16-bit-halves 32x32 to 64 product, with the two's complement
  correction for the signed case) when a shader references them. Nothing in
  the GLSL changes: `gs_prim.h` needs an exact 64-bit product for the edge
  function and `imulExtended` is how it gets one without requiring
  `shaderInt64` of any device.
- `atomicAnd` and `atomicOr` on storage buffer words (`raster.comp`):
  `InterlockedAnd` and `InterlockedOr` on `RWByteAddressBuffer`.
- `shared uint s_color[256]`, `s_depth[256]`: `groupshared`.
- Separate `texture2D` and `sampler` combined at the use site with
  `sampler2D(...)` (`overlay.frag`): HLSL's `Texture2D` and `SamplerState` are
  already separate, so this is the shape HLSL wants.
- `writeonly image2D` with the `rgba8` format qualifier (`scanout.comp`):
  `RWTexture2D<unorm float4>`. Stores only, so no typed UAV load support is
  involved.
- No subgroup or wave operations, no 8- or 16-bit storage, no 64-bit types, no
  buffer device address, no descriptor indexing beyond the fixed arrays. rhi.h
  excludes all of them on purpose.

Two further rewrites the generator has to do, both found by running it:

- SPIRV-Cross emits a push constant block as a bare `cbuffer Push` with **no**
  register clause, which HLSL then defaults to `b0`, colliding with the
  uniform buffer CBVs. The clause is written in rather than left to the
  default.
- `scanout.comp` declares local memory `readonly`, so SPIRV-Cross emits a
  read-only `ByteAddressBuffer`, which HLSL requires at a `t` register. The
  storage buffers are one UAV range in the root signature, so the generator
  gives the read-only slot the `RW` type and it never writes.

`overlay.frag` indexes a sampler array (`g_samplers[1]`). That was the one
construct with a caveat, and the caveat turned out to be real: see the
sampler note above. It is checked on this host now, because the DXC Linux
release under `.cache/dxc-linux` compiles the generated HLSL with the root
signature attached (`[RootSignature(...)]`), which runs the same
root-signature-versus-shader validation the D3D12 runtime runs at pipeline
creation.

### What is verified and what is not

Compiled, on a Linux host with the mingw-w64 headers and no D3D12 device. By
standing instruction no GS renderer is run on this machine, so nothing here
has executed.

Checked:

- The mingw-w64 package provides every header the backend includes. One
  wrinkle: `dxcapi.h` includes no Windows header of its own and does not
  compile standalone, so CMake checks it after `windows.h`.
- The backend cross-compiles clean, with no warnings, into both the runtime
  and the replay tool.
- A Linux `linux-gcc-release` build of the replay tool contains no D3D12
  object, which is the exclusion working.
- SPIRV-Cross translates all four shaders with no diagnostic, and the output
  is byte identical across repeated runs.
- The generated HLSL has no call left undefined after the prelude above.

Not verified until someone runs it on Windows:

- Every run-time behaviour of `dxc`-produced DXIL on a real driver. The
  compile itself, the storage buffer array as `RWByteAddressBuffer[16]`, the
  sampler array and the arithmetic prelude are all settled on this host by
  `tools/gen_gs_shaders_dxil.sh`.
- Every run-time behaviour: adapter selection, the root signature, the
  descriptor ring, the copies with their padding, the present blit and all
  three present modes.

WARP is what CI can settle without a GPU: the replay tool on
`windows-latest`, running the dump corpus through the D3D12 backend and
comparing the picture against the Vulkan reference, verifies the binding
model, the shader path, the dispatches, the copies and readback. It does not
verify the swapchain, the present modes or the present blit's filtering,
because a headless replay never presents. Those need the user's machine.

Run on hardware once, on 2026-09-04, on an RTX 3090. The device came up, the
shaders compiled, the dispatches ran, the swapchain presented and the overlay
was visible through it; the game picture was black. Nothing above is settled
by that run beyond "it does not crash and it does present". See "The first run
on real hardware" below for what it reported, what has been added to localise
the fault, and the two defects the reading of the present path did find.

## The first run on real hardware, 2026-09-04

> **Removed 2026-09-05.** The diagnostic machinery this section and the next
> two describe is gone from the tree: the stage a, b and c content measures,
> the scanout probe and its `ScanoutPush::probe` word, the shader's two
> diagnostic modes, the one-shot batch and texture probes, the cross check,
> and the range-blind transfer check. Every one of them is kept below as the
> record of how the black picture was localised, and the arithmetic and the
> conclusions they produced still stand; what is gone is the code.
>
> Why. The sampling ran on the first three picture fields and then one field
> in 64 for the rest of the run, and every sampled field cost a full device
> stall and about 5 MB of readback. The renderer's next milestone is the
> parity gate, and part of that gate is timing, so a permanent 1-in-64 device
> stall is noise in the measurement it would be judged by. Dev-only code does
> not stall the GPU unconditionally. The cross check went with the sampling
> because it had no cheap trigger of its own: both halves of its comparison
> are readbacks. The transfer check went because it was range blind, counting
> the nonzero words of a destination's min-to-max span, which says nothing
> about a transfer into a narrow rectangle of a wide buffer, and it said it at
> warn.
>
> What is left, because it costs an integer increment when idle: the VRAM
> reconcile counters, the block ownership tracker, and the per-field timing in
> the end-of-run stats block. If the black-picture question returns, the
> measurement is written again against that run, fired from the condition
> being investigated, and removed with it.

An RTX 3090 on D3D12, Windows. The run itself was fine and the same run under
paraLLEl-GS shows the picture, so the guest side is not in question. The
launcher overlay, drawn through the same swapchain by the same graphics
pipeline machinery, was visible. The game picture was black for the whole run.

What the renderer reported: 559 fields, 26 million register writes, 4.7
million kicks, 2.6 million primitives in 18555 batches of which 3.5 million
textured, 211 million pixels of host to local transfer, 9078 readbacks of
local memory totalling 3830906880 words, 555 presents with none skipped for
want of a backbuffer, a 516x512 scanout frame, PAL progressive, `scanout
hires=no (render scale is 1)`, and `present flush 0.000 ms/field, scanout
0.310 ms/field, present_frame 0.218 ms/present`. No gsr warn or error other
than one local to local transfer with SPSM 0x30 against DPSM 0x00 and the
SMODE1 CMOD notes.

So every stage said it had run and none of them said what it had produced.
That is the gap this section closes.

### Content measures on a sampled field

`gs_native.cpp` now takes three measurements on the first three fields that
produce a picture and then on one field in 64, all at info, all naming the
field they belong to:

- **stage a**, in `vsync` after the scanout's wait: for each enabled circuit,
  the word range of local memory the CRTC is about to read, derived from
  DISPFB's block, FBW, PSM and DBY through `gs_buffer_word_range`, copied off
  the *device* buffer the scanout shader reads and counted for nonzero words.
  Not `sync_from_device`: that one reconciles the host and device copies and
  moves `m_gpu_first`/`m_gpu_last`, and a measurement must not change what it
  measures.
- **stage b**, immediately after: the scanout image read back with
  `read_texture`, counted for pixels with a nonzero colour, with the first
  such pixel's position and value, and with `hires` and `samples` on the same
  line.
- **stage c**, inside the present command list and read after its submit but
  before the present: the backbuffer copied twice, once after the blit and
  once after the overlay, each counted for nonzero pixels inside the present
  rectangle and outside it. Two copies because a picture the overlay covered
  and a picture the blit never wrote are the same black window and different
  faults. The line also carries the backbuffer size, the present rectangle,
  the aspect, and the fit and filter settings.

Each sampled field says what it cost in milliseconds, and the stats block
carries the count of sampled fields with the run total and the mean. The cost
is a full device stall plus about five megabytes of readback, so it is kept
out of the scanout and present accumulators: the profiler's per-field numbers
stay the numbers of a field that was not sampled.

How to read the result. A stage with a large count followed by a stage with
zero puts the fault in the step between them. All three zero is the game
drawing somewhere the CRTC is not reading, which is a question for the draw
path and the FRAME and DISPFB registers, not for the present path. Stage c
nonzero after the blit and zero after the overlay is the overlay covering the
picture. Stage c zero in the rectangle and nonzero outside it is a present
rectangle in the wrong place.

### Two things the reading found

**The Vulkan present barrier's source scope did not cover the writes it was
meant to follow.** `record_present` ends with `texture_barrier(backbuffer,
Graphics, Write, Present, Read)`. On the Vulkan backend that produced
`srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT`, while the writes still pending
on that image are the overlay's `COLOR_ATTACHMENT_WRITE` and, when no overlay
is up, the blit's `TRANSFER_WRITE` in the `BLIT` stage. A layout transition is
ordered only against its own barrier's scopes, so the move into
`PRESENT_SRC_KHR` was allowed to run ahead of the drawing, and what is
presented is then undefined. Fixed in two places in `rhi_vulkan_cmd.cpp`:
`access_bits` for a graphics-stage write now carries
`COLOR_ATTACHMENT_WRITE` as well as `SHADER_WRITE`, and a barrier whose
destination is `Stage::Present` takes `ALL_COMMANDS`/`MEMORY_WRITE` as its
source scope, which is the same over-broad pair the backend's internal
`transition()` helper already uses. This is a Vulkan-only fault: a D3D12
transition barrier is expressed as states and not as scopes, and the D3D12
backend derives `StateBefore` from the state it tracked, so the same call is
correct there. The Vulkan backend has never run on hardware, so this was not
what made the D3D12 run black.

**`read_texture` leaves its texture in the transfer-source state or layout,
and nothing said so.** That is harmless for the scanout image, whose next use
transitions out of the tracked state, and it is a broken present for a
swapchain buffer: D3D12 requires `D3D12_RESOURCE_STATE_PRESENT` at
`IDXGISwapChain::Present` and Vulkan requires `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR`
at `vkQueuePresentKHR`. Both backends now refuse `read_texture` of a swapchain
buffer with a fatal that names the alternative, which is
`copy_texture_to_buffer` inside the present command list, and both check the
state or layout of the buffer they are about to present and log an error
naming it if it is not the one the specification requires. The present still
goes ahead after that error, because dropping it would turn one wrong frame
into a frozen window.

### What the stages measured, and the two defects they found

The diagnostics ran on the same RTX 3090 on the Vulkan backend and localised
it in one run. Every sampled field from 64 on:

    stage a: DISPFB2 block 0 FBW 8 PSM 0x00 DBX/DBY 0,0 512x256 is words
             0..278528 of device local memory, 258369 of 278528 nonzero
    stage b: the 516x512 scanout image is entirely black after its dispatch
             (hires=0 samples=1 deinterlace=1 field=1)
    stage c: 0 nonzero in the rect after the blit, 1028 after the overlay

Fields 4 to 6 had stage a all zero, which is the boot before anything is
drawn. The mode was `SMODE1=0x00006100 SMODE2=0x2`, PAL with INT 0 and FFMD 1,
and only DISPFB2 ever appeared in a stage a line, so PMODE had EN1 0 and
EN2 1. So local memory holds the picture and the scanout dispatch produces
black. The same symptoms on D3D12.

**The merge returned BGCOLOR for every pixel.** With EN1 0, `merge_at` falls
straight through to `return back`, and `back` was
`(slbg == 0u && in2) ? texel2 : bgcolor`. Circuit 2's window is 4,0 512x256
inside a 516x512 frame, so `in2` is true for 131072 pixels and the words those
pixels read are 92 per cent nonzero: of the two arms only the BGCOLOR one can
produce a uniformly black frame, and it can only be taken with SLBG 1. That
the frame is *exactly* black rather than a flat colour is the same fact
twice, because BGCOLOR is 0. SLBG 1 with EN1 0 is what a display environment
set up in the usual way leaves behind, and it is a setting under which the
hardware displays circuit 2: paraLLEl-GS shows this game's picture from the
same register writes on the same run. Fixed in `scanout.comp`: SLBG is read
only when circuit 1 is enabled. The value of SLBG itself is a deduction from
the measurement and the shader source, not a number read off a log, which is
why the stage b lines below now print PMODE.

This defect is not PAL specific and not backend specific. It blanks any frame
whose PMODE enables circuit 2 alone with SLBG 1, which is why the native
renderer had never drawn a game frame on any target.

**The frame was twice as tall as the raster.** `mode_area` read
`lines >> (interlaced ? 1 : 0)`, giving a non-interlaced raster the whole
interlaced line count. `kModeLinesPal` is 512, so a progressive PAL frame came
out 512 lines against the 256 line window ICO PAL actually programs (DISPLAY2
DH+1 256, MAGV 0), which is the measured `scanout frame is now 516x512`
against a 512x256 DISPFB2. On its own this is a picture in the top half of the
frame and BGCOLOR in the bottom, not a black frame, so it was hidden behind
the merge. The shift is now unconditional: what interlace decides is only
whether `frame_h` is `lines_per_field` or twice it. `circuit_rect` is
unchanged and stays right, because DH and DY are in interlaced raster lines
only when INT is 1. The aspect derivation at the end of `crtc_plan` already
counted a non-interlaced DH twice against the same mode line total, which is
the same statement made in the other direction, and the aspect is unchanged at
4:3.

After both fixes the frame for this mode is 516x256, circuit 2 covers all of
it, and the picture is read with `line = pix.y` straight out of buffer rows
0..255 with no field arm, which is what INT 0 means.

Two things this did **not** turn out to be, both checked against the shader
and the push constants and both left alone. The FFMD row rule
(`sy = sy * 2u + fld`) is guarded by `pc.interlaced != 0u` and so does not run
here, which is correct: FFMD selects between FIELD and FRAME reading in
interlace mode, and a non-interlaced raster reads every line. And the display
window arithmetic places circuit 2 at x 4 inside a 516 wide frame, which is
`(DX - 636) / 5` against a 2560 clock visible area at MAGH 4, so the window
lands inside the frame and not outside it.

### The registers behind the arithmetic

The stage b measurement is now three lines, so the next log shows the window
arithmetic in numbers rather than only in its result:

- `stage b regs`: PMODE raw and decoded (EN1, EN2, CRTMD, MMOD, AMOD, SLBG,
  ALP), SMODE2 raw with INT and FFMD, CMOD, both DISPLAY registers as DX, DY,
  MAGH, MAGV, DW, DH, and the mode area in pixels by lines per field.
- `stage b push`: all 31 words of `ScanoutPush` as the dispatch received them,
  named.
- `stage b`: the content measure as before.

`ScanoutPlan` carries `pmode_raw`, `smode2_raw` and the two decoded `Display`
structures for this and nothing else reads them; the push block is full at 31
words and none of this belongs in it.

### The shader blobs have to be regenerated

`scanout.comp` changed, and every backend reads a committed blob rather than
the GLSL. Nothing here regenerates them. Two commands, in this order, from the
repository root:

    ./tools/gen_gs_shaders.sh        # SPIR-V, HLSL, MSL and their indexes
    ./tools/gen_gs_shaders_dxil.sh   # DXIL for D3D12, signed, and its index

The first rewrites `shaders/scanout.comp.spv.inc`, `shaders/hlsl/scanout.comp.hlsl`,
`shaders/msl/scanout.comp.metal` and the three index headers; the second reads
the new HLSL and rewrites `shaders/dxil/scanout.comp.dxil.inc` and
`rhi_shaders_dxil.h`. A D3D12 build made without the second command runs the
old merge with none of the others changed and stays black, and the drift gate
above is what catches that.

`gs_crtc.cpp`'s frame height changes for any non-interlaced dump in the
corpus, so the parity gate's stored pictures for those dumps have to be taken
again.

### The SLBG deduction was wrong, and the scanout probe

The fixed build ran on the same RTX 3090 on Vulkan. `mode_area` was right:
`scanout frame is now 516x256`. The picture was still black, and the new
register line settled the deduction against it:

    stage b regs: PMODE=0x66 (EN1=0 EN2=1 CRTMD=1 MMOD=1 AMOD=1 SLBG=0 ALP=0)
                  SMODE2=0x2 (INT=0 FFMD=1) CMOD=3;
                  DISPLAY2 DX=656 DY=36 MAGH=4 MAGV=0 DW=2559 DH=255;
                  mode area 512x256 lines per field
    stage b push: frame 516x256 field=1 deint=1 interlaced=0 ffmd=1
                  bgcolor=0x000000 merge=0x0000000e;
                  c2 en=1 block=0 fbw=8 psm=0x00 dbx=0 dby=0 at 4,0 size 512x256;
                  hires=0 samples=1 hist_valid=1
    stage a:      258369 of 278528 words nonzero
    stage b:      the 516x256 scanout image is entirely black after its dispatch

**SLBG is 0.** The merge change is therefore inert here, and it stays because
the rule it states is still the right one; it is now listed under "Measured
versus inferred" as inferred, which is what it always was. The deduction that
named SLBG was wrong because it assumed the only way to reach a uniformly
black frame was through the BGCOLOR arm. With SLBG 0, EN1 0, a circuit window
of 4,0 512x256 inside a 516x256 frame, `interlaced` 0 and a buffer that is 92
per cent nonzero, every arm of the shader that these values select writes a
nonzero colour. The dispatch produces nothing from inputs that are all
correct.

Everything between the register file and the dispatch was then read again, and
all of it checks out:

- **The buffer.** Stage a copies out of the same `rhi::Buffer` that
  `record_scanout` binds at storage slot 0 and slot 1; there is one
  `VkBuffer`, and the Vulkan backend has no host-visible shadow (that is a
  D3D12 mechanism, and `m_vram` is `DeviceLocal` on both). Every element of
  `g_vram[16]` is written, with the device's dummy buffer in the thirteen
  slots nothing is bound to.
- **The grid.** 516 by 256 at a local size of 8x8x1 is 65 by 32 by 1
  workgroups. Nothing is zero, and `dispatch_checked` logged no refusal.
- **The barriers.** `access_bits(Stage::Compute, Access::Write)` is
  `VK_ACCESS_2_SHADER_WRITE_BIT`, and `build_descriptor_set` moves every bound
  storage image to `VK_IMAGE_LAYOUT_GENERAL` before the dispatch, which is the
  layout its descriptor declares.
- **The descriptor.** The storage image descriptor carries the frame image's
  own view at `GENERAL`. The only early return that can skip every invocation
  is the `pix.x >= out_w || pix.y >= out_h` bound, and `out_w`/`out_h` are 516
  and 256 on the host.
- **The command list.** `upload_dirty_vram` and `record_scanout` are recorded
  on the one list `vsync` opens, and the readback happens after that list's
  submit has been waited on.
- **The pipeline layout.** One push constant range of 128 bytes at offset 0
  with `VK_SHADER_STAGE_ALL`, attached to the layout, and `vkCmdPushConstants`
  writes the whole 128 bytes through it. This was checked because a layout
  with no range would leave the push block reading zero, which would make
  `out_w` and `out_h` zero and every invocation return, and that is exactly
  the measured symptom. It is not the cause.

So the next step is a measurement and not another hypothesis.

**The probe.** It is built into the executable and it fires on its own. There
is no setting: a diagnostic that has to be asked for is a diagnostic nobody
has when the fault appears, and this project's rule is that the executable
carries them.

The trigger is the **measured black condition**, tested on a field the
existing stage a and stage b measurements already sampled, so it costs nothing
until it happens:

> the display buffer the CRTC is about to read is at least **one eighth**
> nonzero, and the scanout image built from it has **no** pixel with a nonzero
> colour, and the readback that established that succeeded.

One eighth rather than "any nonzero word", because a boot field with a few
words of stale content in an otherwise empty buffer is a legitimately black
picture and must not spend two fields on a measurement. The run this exists
for measured 258369 of 278528, which is 0.93; fields 4 to 6 of the same run
measured 0 of 278528 and would correctly not have fired. The fraction is a
chosen threshold, not a measured one, and it is the only number that decides
whether the probe runs.

When it fires, the next field runs the **solid** probe and the field after it
runs **raw**, each read back and each answered with one line at error, and
then the normal scanout resumes for good. **At most once in a run.** If the
condition never occurs, none of this executes and nothing is logged beyond the
ordinary stage lines.

Both arms sit at the top of `scanout.comp`'s `main`, ahead of everything that
reads a push value, and both take their bounds from `imageSize(g_images[0])`
rather than from the push block:

- **solid** writes `vec4(x & 255, y & 255, frame_w & 255, 255) / 255` at every
  invocation. It depends on nothing but the dispatch running and the storage
  image descriptor naming the frame image. Its blue channel carries `frame_w`,
  so a gradient also says whether the rest of the push block arrived.
- **raw** writes storage buffer slot 0 read at the output pixel's own linear
  index, with no swizzle, no window and no circuit.

On a probe field the host also pre-fills the frame image with `0xFF3B2A11`
through `copy_buffer_to_texture` on the same command list, immediately before
the dispatch. That is what makes the answer three-way instead of two-way, and
it depends on no push constant at all.

The frame image therefore carries `TextureUsage::CopyDst` on **every** run,
not only a probe run: the RHI fixes a texture's usage at creation and neither
D3D12 nor Vulkan can widen it afterwards, and a probe that only fires on a
fault cannot ask for a differently created image before the fault happens. The
cost is nothing on D3D12 and Metal, where a copy destination is a resource
state and not a creation flag, and one `VK_IMAGE_USAGE_TRANSFER_DST_BIT` on
Vulkan on an image that already carries `STORAGE`, `SAMPLED` and
`TRANSFER_SRC`, so there is no compression mode left for it to give up.

### What the log says

Nothing at all, unless the condition fires. When it does:

    [warn]  field N: the display buffer the CRTC reads is 258369 of 278528 words
            nonzero and the 516x256 scanout image built from it is entirely
            black. That is the measured black condition. The next two fields'
            pictures are replaced by a built-in measurement, solid then raw, and
            the normal scanout resumes after them. This happens at most once in
            a run.
    [info]  the graphics debug layer was off for this run, so no driver message
            accompanies this. A run with debug.verbose = rhi adds the D3D12 debug
            layer's or the Vulkan validation layer's own messages, which would say
            whether the driver rejected anything in the dispatch. A hint, not a
            requirement.

That info line appears only when the layers were in fact off. Then one error
line per probe field, one of these:

| verdict | what it means, and what to look at next |
|---|---|
| every pixel still carries the host pre-fill `0xFF3B2A11` | the host copy landed and the dispatch wrote nothing: either it is not executing, or the probe word did not reach the shader. Look at the compute pipeline, its descriptor set, and `vkCmdPushConstants` against the layout's push constant range |
| the image is entirely zero | neither the host copy nor the dispatch reached the image. The fault is the image, its view or the readback, not the shader: `create_texture`, the view the storage descriptor carries, `copy_texture_to_buffer` |
| solid: the gradient, blue equal to `frame_w & 255` | the dispatch, the storage image descriptor, the readback and the push block all work. The fault is in the data path the normal scanout takes, and the raw field that follows splits it |
| solid: the gradient, blue not `frame_w & 255` | the dispatch runs and the push block does not reach the shader past the probe word. `out_w` and `out_h` then read 0 and every invocation of the normal scanout returns at its bounds check, which is the whole black picture |
| raw: every pixel black | the buffer the dispatch reads is not the buffer stage a reads. The fault is the storage buffer descriptor: `build_descriptor_set`'s slot 0 and the range it binds |
| raw: nonzero pixels of local memory | the storage buffer binding is good and the dispatch reads the memory stage a reads. The fault is above it: `gs_pixel_addr`'s addressing, or the circuit window arithmetic in `merge_at` |

    [warn]  the scanout probe is finished; the normal scanout resumes on the next
            field and the probe does not run again in this run.

The end-of-run stats block carries one line either way, so a log that never
tripped the condition still says the probe existed and did not fire.

`stage a` also names the `rhi::Buffer` and its size, and `stage b grid` names
the workgroup counts, the image size, the device maximum and the probe word,
so the first two candidates above are answered in the log rather than by
reading.

### What the probe measured: the scanout is innocent

The automatic probe fired on the user's Vulkan run at field 64 and answered in
two fields.

    field 65 probe solid: the 1032x512 frame image carries the probe's gradient
      (528384 of 528384 pixels nonzero, centre 0xff040004) and its blue channel
      is 4, which is frame_w 516 and 255.
    field 66 probe raw: the dispatch overwrote the pre-fill, so it ran, and
      every pixel it wrote from storage buffer slot 0 read at that pixel's own
      linear index is black.

**Measured, and it retires the whole line of investigation.** The solid verdict
proves the dispatch executes, the storage image descriptor names the frame
image, the push block arrives (the blue channel carries `frame_w & 255`, and
516 & 255 is 4), the readback carries the result and the blit and the present
show it. The raw verdict proves the dispatch reads storage slot 0 and that
every colour byte in the first 528384 words of it is zero, while stage a read
269376 nonzero words of 278528 from the same `rhi::Buffer` at the same moment.
Both facts together say one thing: **the display buffer holds words that are
nonzero and carry nothing in bits 0..23.** It is alpha or depth, not colour.

A black scanout is therefore the correct picture for that content. The scanout
pass, `gs_pixel_addr`, the circuit window arithmetic, the storage buffer
binding, the descriptor set, the push constants and both RHI backends are all
exonerated by measurement. That is also why D3D12 shows the same picture: the
fault is upstream of the backend, in what puts colour into local memory, and
that is shared.

Two of the normal stage b readings say the same thing independently.
`0,0 = 0x80000000` is `BGCOLOR | 0x80000000`, which is what `merge_at` returns
outside circuit 2's window, and circuit 2 starts at x 4, so the first pixels of
each row are correctly background. `centre 516,256 = 0x7f000000` is inside the
window: the merge read a texel, and the texel had no colour in it.

**What doubled the image.** `display.render_scale 8` was set for that run. The
log carries it: `display.render_scale 8: 8 samples per pixel, 32 MiB of
super-sampled shadow`, then `scanout hires=no at display.render_scale 8 (the
displayed buffer holds no sub-samples: nothing drew it at this scale)` while
the buffer was still empty, and from field 64 `scanout hires=yes: 1032x512
from 8 samples per pixel, not deinterlaced` once the shadow had samples for
the displayed buffer. A high-resolution scanout is twice the frame on each
axis, so 516x256 becomes 1032x512 and the grid becomes 129x64. Nothing is
wrong with it; the earlier `hires=0` lines are the same run before the shadow
had anything in it. Note that the hires arm reads the **shadow** at storage
slot 1, and it is black too, so the shadow holds no colour either. A run at
`display.render_scale 1` takes the shadow out of the picture entirely and is
worth doing once for that reason alone.

### The measurement moved upstream by one stage

The probe's own verdict text drew the wrong conclusion from a correct
measurement: it said "the buffer the dispatch reads is not the buffer stage a
reads", because it compared a count of pixels with a nonzero **colour**
against a count of **nonzero words**, and a word can be nonzero with no colour
in it. That is exactly the case it was looking at. Fixed, and the distinction
is now made for free on every sampled field rather than by spending two fields
on a probe:

- **stage a** counts two things over the words the CRTC reads: how many are
  nonzero, and how many carry anything in bits 0..23. Both are on the line.
- **The black condition** that arms the probe now requires **colour**, not
  merely nonzero words. A buffer full of alpha is a correctly black picture
  and must not spend two fields proving it.
- **A new verdict**, at error and said once, fires when the buffer has
  substantial nonzero words and almost no colour: it says the scanout is not
  the fault, and it names the last batch's `FRAME` block, FBW, PSM and
  **FBMSK** and its `ZBUF`, against the DISPFB block, FBW and PSM the CRTC
  reads. Those are the three things that decide whether drawing reaches the
  buffer the CRTC looks at, and the log has never carried any of them.
- **The raw probe verdict** now splits the same two cases with the same
  counts, so if the probe ever does run it cannot repeat the mistake.

What to look at when that line appears: whether `FRAME` block equals the
DISPFB block (the drawing and the display are the same buffer, or a transfer
is supposed to move one to the other), whether `FBMSK` is protecting the
colour bits (`frame_store` writes `mask & ~pc.frame_mask`, so an FBMSK of
0x00FFFFFF writes alpha only), and the local to local transfer that fills the
display buffer, which this run performs 79429632 pixels of and which already
warns once about an `SPSM 0x30` against a `DPSM 0x00`.

### The three raster-path candidates, read

**(1) Non-uniform descriptor indexing: retired.** Every index into the storage
buffer array in all three compute shaders is a compile-time constant.
`raster.comp:105-116` declares `g_buf[16]` with `VRAM 0, PRIMS 1, BINIDX 2,
BINRNG 3, CLUTS 4, SHADOW 5`, and the only variable selection in the file is
`mem_load`'s `g_shadow ? g_buf[SHADOW].data[i] : g_buf[VRAM].data[i]`
(`raster.comp:161-164`), an `if` on `pc.shadow`, which is a push constant and
therefore dynamically uniform. `store_masked` (`:173-192`) does the same. The
super-sample selection is **not** a descriptor index: `g_plane = smp *
GS_VRAM_WORDS` (`:876`) is an element offset inside one buffer, and
`mem_index` adds it (`:158`). The texture fetch does not go through
`mem_load` at all: it reads `g_buf[VRAM].data[...]` directly at `:267` and
`:272`, and the CLUT reads `g_buf[CLUTS]` at `:282` and `:284`. `scanout.comp`
uses `VRAM 0`, `SHADOW 1`, `HISTORY 2`, all constant. `shadow.comp` has no
variable index either.

So nothing needs `nonuniformEXT`, nothing needs
`shaderStorageBufferArrayNonUniformIndexing` or the D3D12
`NonUniformResourceIndex`, and none is missing. This is not the difference
between NVIDIA and WARP.

**(2) The frame store: colour and alpha are written as one word.**
`frame_store` (`raster.comp:219-230`) is `store_masked(g_faddr, rgba,
mask & ~pc.frame_mask)` with `mask` `0xFFFFFFFF` for PSMCT32, and
`store_masked` writes the whole word in one plain store when the mask is
`0xFFFFFFFF` and one `atomicAnd` plus one `atomicOr` otherwise. There is no
path in which the alpha byte reaches memory and the colour bytes do not,
**except through FBMSK**. `decode_frame` (`gs_regs.h:295-302`) takes FBMSK
from bits 32..63 and the sense is right: a set bit protects, and the shader
writes `~pc.frame_mask`.

That leaves two ways to get a word with alpha and no colour, and both are the
shading rather than the store: an FBMSK of the shape `0x00FFFFFF`, or a
shaded colour that is already black. The second is what a modulated textured
primitive produces when its texel comes back zero, because with TCC 0 the
alpha comes from the vertex and only the RGB comes from the texture. This run
drew 1544268 textured primitives.

**(3) The local to local transfer runs on the host.**
`TransferEngine::local_to_local` (`gs_vram.cpp:158-206`) reads and writes
`LocalMemory& mem`, which is the host store, one pixel at a time through
`read_pixel`/`write_pixel`. Nothing about it touches the device. Its result
reaches the device only through `upload_dirty_vram`, which uploads the
**conservative min/max span** of the host's dirty range, and that upload is
guarded by `host_current_for_upload()` calling `sync_if_overlaps`, which reads
the device's own written range back into the host first. So the transfer can
erase device-written colour only if `m_gpu_first..m_gpu_last` fails to cover
something the device wrote. It also drops the super-sampled shadow's samples
over the same conservative span (`m_shadow.invalidate_words`), which is what
`34337 dropped by a native write` of `34660 pages seeded` is.

The `SPSM 0x30` against `DPSM 0x00` warn is a PSMZ32 source and a PSMCT32
destination. Those two share the GS's page and block layout, so a verbatim
copy of the stored values is the right answer and the warn is a note, not a
fault. It is also not a route to alpha-only content: a depth value copied into
a colour buffer carries bits everywhere, not only in the top byte.

None of the three is established as the cause by reading. What is established
is where the cause cannot be, and that is worth as much: the descriptor model,
the store's mask sense and the transfer's mechanism are all correct.

### Stage 0, the batch probe

The scanout measurements leave exactly one question open, and it has two
answers: either the rasteriser writes no colour anywhere, or it writes colour
somewhere the CRTC does not read. One readback of a batch's own FRAME range,
taken right after that batch's dispatch and its resolve, settles it.

It is armed by the no-colour verdict and fires on the first batch after it,
once in a run, so it costs nothing until the fault appears. It logs, at error:
the batch's primitive count and tile grid, whether it drew into the shadow and
at how many samples, its `FRAME` block, FBW, PSM and **FBMSK**, its `ZBUF`,
the word range it wrote, and then one of two verdicts.

| verdict | what it means |
|---|---|
| the range has nonzero words **and** words carrying colour | the rasteriser does put colour into local memory. The fault is between there and the display buffer: the drawing goes to a buffer the CRTC does not read, or the transfer that should move it does not, or the host upload writes over it. Compare this `FRAME` block against the DISPFB block on the stage a line |
| the range has nonzero words and **none** carrying colour | the rasteriser wrote alpha and no colour, so the fault is the shading or the frame store and nothing downstream. Check `FBMSK` first, then the texture fetch, which reads native local memory directly and would shade a modulated primitive black with its vertex alpha if it came back zero |

### What to look for in the next log

In order:

1. `field N: the display buffer the CRTC reads holds ... only M of them carry
   anything in bits 0..23` and the `FRAME`/`FBMSK`/`ZBUF`/DISPFB numbers on
   it. If `FBMSK` has bits 0..23 set, that is the whole answer. If the `FRAME`
   block is not the DISPFB block, the drawing and the display are different
   buffers and the transfer between them is next.
2. The `batch probe` line and its verdict, which splits shading from
   transport.
3. `stage a` now carries both counts on every sampled field, so the moment
   colour appears in the display buffer is visible without any probe.

### The render scale path is eliminated

`display.render_scale 8` was set for the Vulkan runs, and its counters
(`34337 dropped by a native write` of `34660 seeded`) say the shadow is
invalidated almost as fast as it is filled, which made it an obvious suspect.
It is not the cause. The **first** native run, on D3D12 at 21:20 UTC on
2026-09-04, had `display.render_scale = 1` in the user's settings.json, read
off the file at the time, and was black in the same way with the same 4.7
million kicks and 2.6 million primitives. So a run at scale 1 has already been
made and there is no reason to ask for another.

That elimination narrows the search usefully, because at scale 1 there is no
shadow anywhere in the chain: `use_shadow` is false, so `pc.shadow` is 0,
`mem_load` and `store_masked` both address `g_buf[VRAM]` directly
(`raster.comp:161-192`), there is no seed pass and no resolve, and the scanout
reads the same `g_buf[VRAM]` at plane -1. **The rasteriser writes and the
texture unit reads the same buffer, and the CRTC reads that buffer**, with
nothing between them. Every theory that needs the shadow, the seed, the
resolve, or a difference between the buffer the drawing goes to and the buffer
the texture fetch reads, is dead.

What survives is exactly three things, and the two new log lines split all
three:

- `FRAME`'s **FBMSK** protecting bits 0..23, which `frame_store` would honour
  by writing alpha only.
- The shaded colour already being black, which a modulated textured primitive
  produces when its texel comes back zero, since TCC 0 takes alpha from the
  vertex and only RGB from the texture.
- The drawing going to a `FRAME` the CRTC does not read, with the transfer
  that should move it into the display buffer not doing so.

### The fault is the texture fetch

The batch probe ran on D3D12 and narrowed it to one stage.

    field 64: ... 269376 nonzero words of 278528, and only 0 of them carry
      anything in bits 0..23. The last batch drew with FRAME block 2048 FBW 8
      PSM 0x00 FBMSK 0x00000000 and ZBUF block 6144 PSM 0x30 write=0, against a
      DISPFB at block 0 FBW 8 PSM 0x00
    batch probe: 100 primitives over a 64x64 tile grid, shadow=1 samples=8;
      FRAME block 0 FBW 8 PSM 0x00 FBMSK 0x00000000 ... after this batch,
      121920 of 131072 words in its own FRAME range are nonzero and none of
      them carries a colour

From field 128 on, stage a reads `259269 of 278528 nonzero and 7418 of those
carry a colour`: a few thousand coloured words, which is what vertex-coloured
geometry produces and what the untextured part of the frame is.

So, measured: **FBMSK is 0**, so nothing is masking the colour bits. **The
drawing goes to the buffer the CRTC reads**, double-buffered at blocks 0 and
2048 with the DISPFB at 0. **The rasteriser puts alpha in the right places**,
121920 words of it in a batch's own FRAME range. And **untextured drawing does
produce colour** while textured drawing does not. A modulated textured
primitive shades to exactly that when its texel comes back zero, because TCC 0
takes the alpha from the vertex and only the colour from the texture.

The fault is the texture fetch, on both NVIDIA D3D12 and NVIDIA Vulkan, while
the WARP parity run passes.

**Two candidates are eliminated by that pair of backends alone.** The
SPIRV-Cross HLSL cannot be the cause, because the Vulkan backend never sees
any HLSL and consumes the SPIR-V directly, and it shows the same picture. The
D3D12 storage shadow (the upload heap and the default heap a host-visible
storage buffer lives in twice) cannot be the sole cause either, for the same
reason: Vulkan has one buffer and no shadow. Whatever it is, it is common to
both backends: the GLSL, the data not being in local memory, or a dependency
the RHI's shared barrier contract expresses the same wrong way on both.

That WARP passes says the dump corpus does not exercise it. Worth noting
against that: this run drew 1544268 textured primitives of which **829308 were
mipmapped**, so more than half the textured drawing takes the LOD and
MIPTBP/MTBA path.

### Stage 0 now measures the texture source

The batch probe reads two more things off the device, after the probed batch,
and compares each against the host mirror of the same words. Both at error.

- **The texture.** The first primitive in the batch with `GSP_F_TME` set gives
  its TEX0: the probe logs TBP0, TBW, PSM, the size from TW and TH, TCC, TFX
  and the whole CLUT descriptor (CBP, CPSM, CSM, CSA, CLD), then reads the
  texture's word range off the device buffer the shader reads and counts the
  nonzero words there against the count in `m_mem` for the same range.
- **The CLUT.** The batch's CLUT buffer, read back off the device and counted
  against the host copy of it. On D3D12 those are two different resources and
  only the first is what the shader reads, which is what makes this worth
  measuring rather than assuming.

`diag_count_buffer` is what does it: the same readback the display buffer's
counts use, generalised to any storage buffer this renderer owns, so both new
numbers come from the words the device holds and not from the host's copy of
them.

| what comes back | what it means |
|---|---|
| device nonzero, host nonzero | the texels are on the device and the shader is given them. The fault is the addressing that reads them: `gs_pixel_addr`, the CLUT lookup, or the texture unit's arithmetic |
| device zero, host nonzero | the upload or the barrier before this dispatch is the fault, not the shader |
| both zero | nothing ever put the texture into local memory, and the fault is upstream of this pass, in the transfer that should have uploaded it |
| CLUT device zero, host nonzero | on D3D12 the storage shadow never received its copy; on Vulkan a missing host write dependency |

If no primitive in the probed batch is textured, the probe says so rather than
reporting nothing, and names `GSP_F_TME` as what to wait for. It still fires
only once in a run.

### Render scale 1 retires the shadow, and what the log actually says

A scale 1 run on D3D12: `display.render_scale 1: 1 samples per pixel, 0 MiB of
super-sampled shadow`, `scanout hires=no (render scale is 1)`. Field 128 stage
a reads `257379 of 278528 nonzero and 5667 of those carry a colour`, and stage
b on the same field says `the 516x256 scanout image is entirely black`. With no
shadow anywhere in the chain, the scanout reads the very buffer that holds
those 5667 coloured words and writes none of that colour. **The shadow theory
is retired.**

Three numbers that were reported from the earlier runs do not match the logs,
and the logs are what this section works from.

- The scale 8 run's stats say `44180 pages seeded, 43857 dropped by a native
  write, 2 whole-shadow drops, **10537 resolves**` against `10537 batches`,
  which is one resolve per batch. It was not 0, and the shadow was being
  resolved on every batch.
- The same run says `local memory read back from the device **5119** times,
  2159812608 words` over 360 fields, which is 14.2 readbacks per field of 1.7
  MB each, not 16158.
- The scale 1 run's stage b pixels line reads `centre **258,128**` for a
  516x256 image, which is the centre. The sample point is correct.

The cost model for the readbacks stands on the real numbers: 14.2 full
pipeline stalls per field, each a submit, a wait and a 1.7 MB copy across PCIe,
which is what fills `gswait 17.364 ms/field`. They come from `sync_if_overlaps`
being reached from `clut_written`, `sync_for_transfer` and
`host_current_for_upload`, and from `sync_from_device` reading the **whole**
`m_gpu_first..m_gpu_last` span every time because the span is a min and a max
and cannot represent a partial reconciliation. Cutting it needs a page
granular tracker for both the host dirty set and the device written set, which
is a real change and not one to make blind while the picture is still wrong.

### The pixel level cross check

The one thing every measurement so far leaves open: whether the shader's
addressing lands on the coloured words at all. It now runs on any sampled
field where the buffer carries colour and the image carries none, which is
exactly the failing case and nothing else.

Both directions, on the host, through the same `gs_pixel_addr` the shader
calls, so the two cannot disagree about the swizzle:

- **Forward.** The circuit's word range is read off the device and walked; for
  each of the first eight words carrying colour, the check prints the word, its
  offset, the buffer pixel `(x, y)` that addresses it, whether that pixel is
  inside the circuit's window, the output pixel it should be showing, and the
  value that output pixel actually has. The `(x, y)` map is built by walking
  the buffer once with `gs_pixel_addr` itself.
- **Reverse.** Three output pixels, the buffer pixel and word address the
  shader reads for each, and the word the device holds there.

**A forward line whose `y` falls outside the circuit's window is the whole
answer on its own**: the picture is in a part of the buffer this scanout never
reads. That is a live possibility here and the check is aimed at it. Stage a
measures `dby + h * (ffmd ? 2 : 1)` buffer rows, which is **512** for this
mode, while the scanout with `INT` 0 reads `line = pix.y`, which is rows
**0..255**. If the coloured words are in rows 256..511, the scanout is looking
at the wrong half of the buffer and `SMODE2` FFMD 1 with INT 0 is the reason
to look at. The forward lines say which rows the colour is in, in numbers.

### The probe now fires on a text screen

The black condition required an eighth of the buffer to carry colour. The
language screen is text on black: 5667 coloured words of 278528, two per cent,
so an eighth would never fire on the one screen that shows the fault most
clearly. The colour test is now an absolute floor of **256 words**, a quarter
of one page and more than any stray write, and the fraction is kept only for
the "nonzero words but no colour" verdict, which does want a substantially
written buffer. The sequence may now run **twice** in a run rather than once,
because its first firing happened on a field whose buffer had no colour at all
and therefore tested nothing about a coloured one. Both thresholds are chosen,
not measured, and the code says so.

### The PSMCT32 swizzle read against the shader

Read for the shared expression both hardware backends would get wrong while
WARP and lavapipe do not. `gs_pixel_addr` (`gs_swizzle.h:314-330`) for FBW 8,
DBX/DBY 0, PSMCT32: `pages_across = 512 / 64 = 8`, `page = (y / 32) * 8 +
(x / 64)`, `bx = (x % 64) / 8`, `by = (y % 32) / 8`, the block number comes out
of `kBlock32` at `by * 8 + bx`, and the address is
`block * 64 + gs_block_offset(...)` with `gs_block_offset` for FAM_32 being
`(ly >> 1) * 16 + kColumn32[(ly & 1) * 8 + (lx & 7)]`. Every quantity is
`uint`, every shift is by a literal 1 or 3, there is no shift by 32, no signed
arithmetic, no 64 bit intermediate, and no division by a value that can be
zero (`pages_across` is guarded). `read_circuit` then reads
`g_vram[VRAM].data[addr & (GS_VRAM_WORDS - 1u)]`, which cannot be out of range
whatever `addr` is. Nothing here is a candidate for a translation difference.

The two constant tables are the remaining possibility: `kBlock32[i]` with
`i = by * 8 + bx` is in range for any `bx < 8, by < 4`, and an index past the
end would be robust-access zero on hardware and something else on a software
implementation. The cross check answers that too, because a wrong block table
shows up as a forward line whose output pixel is not the buffer word beside it.

### Found: the FFMD row rule was gated on INT

The cross check answered it in one run.

    field 128 cross check 0: word 169479 = 0x0f1d1d1d is buffer pixel 163,337;
      the circuit window is 4,0 512x256 at DBX/DBY 0,0, so it is OUTSIDE the
      window, so this scanout never reads it

All eight coloured words came back the same way, at buffer rows 337 to 339 and
x 163 to 166. The reverse checks agreed with the device: output 260,128 reads
buffer 256,128 and the device holds `0x7f000000` there, which is what the
output pixel had. So the addressing was right and it was pointed at the wrong
half of the buffer.

ICO PAL programs `SMODE2` INT 0 with FFMD 1, `DISPLAY2` DH+1 256 and MAGV 0,
and draws a 512 row frame. `scanout.comp`'s `merge_at` applied FFMD's row rule
under `pc.interlaced != 0u && pc.ffmd != 0u`, so with INT 0 it read buffer rows
0..255 and never saw the text.

**The rule, and its source.** With FFMD 1 the CRTC reads every other line of
the buffer: the buffer row of output line L is `(DBY + L) * 2 + field`. That is
already what `gs_crtc.cpp`'s file comment records as measured, from the attract
movie's DISPFB2 DBY 12 skipping 24 buffer rows, which is what fixes it as
`(DBY + line) * 2 + field` rather than `DBY + line * 2`. **The rule does not
depend on INT.** `gs/gs_parallel_scanout.cpp` states the same for the other
backend, from the library's own behaviour and without conditioning on INT: the
CRTC "reads each as a packed field and lays buffer A's row n on display line 2n
and buffer B's row n on 2n+1", and at 1x the weave "puts the current field on
rows where `(y & 1) == phase`". Neither statement is guarded by the raster
being interlaced, and paraLLEl-GS shows this screen correctly from these same
registers.

So an output line y reads buffer row `(DBY + y) * 2 + parity`, and the odd rows
are the other parity's: half the buffer reaches the screen per field, which is
what FRAME mode means. The frame height is unchanged at 256 output lines, which
is what `mode_area` gives for this raster after the earlier fix, and the buffer
behind it is 512 rows.

**What is not measured: the absolute sense of the parity when INT is 0.** The
reference derives it from the field counter as `(field & 1) ^ 1` and then
refines it with a display copy snoop that this renderer does not have, so the
field the vsync carries is used directly. Getting it the wrong way round is a
one buffer row offset, not a missing picture. That is the open question left
behind by this fix.

**Changed:** the guard in `scanout.comp`'s `merge_at` is now `pc.ffmd != 0u`
for both circuits, `choose_hires`'s `ffmd` local drops the same INT term so the
pages an FFMD circuit reads are counted twice as tall whatever the raster is,
and the cross check applies the same row rule in both directions so its next
report is consistent with what the shader does.

The hires arm needed no change. It selects a field line and leaves the row
doubling to `merge_at`, so it follows the corrected rule for a progressive
raster as well.

**Not changed, because they were already right.** Stage a measures
`dby + h * (ffmd ? 2 : 1)` buffer rows, which is 512 for this mode and is
exactly what the corrected scanout reads. The aspect derivation counts a non
interlaced DH twice against the mode's line total, `(DH + 1) * 2` against 512,
which gives 4:3 for this display environment and matches `mode_area`'s own
unconditional halving. Both were consistent with the corrected rule and neither
needed touching.

## The first native picture, 2026-09-04

The FFMD fix produced the first game frame this renderer has drawn: the PAL
language screen, 1280x960 at 4:3. It is wrong in two independent ways, and
they are separated here because one is placement and the other is the texture
unit.

**Fault 1, vertical placement.** Everything drawn sits in the lower third: the
first row of marks at about y 655 of 960, a block at 790 to 870, more at 890
to 960, and the top two thirds black. The pointer agent measured the five
language items at 31, 42, 54, 65 and 76 per cent of the picture height from the
game's own object data, so the first item belongs a third of the way down. The
cross check independently found the topmost coloured word at buffer row 337 of
512, which is 66 per cent and is where the first marks appear. So the buffer
itself has nothing above row 337: this is not the CRTC showing the wrong rows,
it is the drawing landing too low, or being given a y that is already too low.

**Fault 2, the glyphs.** Each glyph is a set of vertical bars of varying
brightness rather than letters, and the highlighted item is a block of
random-looking bright pixels. Bars that are constant down y mean the texel's
row does not advance with the output row while its column does.

The sprite arm of the texture coordinate interpolation is not the cause and
was read to rule it out: `raster.comp:603-610` takes
`ty = (syp - y0) / (y1 - y0)` and `fv = mix(v0, v1, ty)`, with
`syp = py * 16 + gs_sample_y(...)` at `:887` and the sprite's `y0`/`y1` in the
same 16.4 window units, so `ty` advances per output row exactly as `tx`
advances per column.

What the swizzle selftest already settles, and what it does not:

- **PSMT8 is verified** against a transcribed table (`gs_swizzle_selftest.cpp`
  check 1), so an 8-bit font's texel addressing is not in question.
- **PSMT4's word placement is verified** against the PSMT8 table (check 4),
  because a PSMT4 column and a PSMT8 column are the same 64 bytes.
- **PSMT4's nibble order inside that word is not verified and is a stated gap**
  in `gs_swizzle_reference.cpp`: the manual's 32x4 column diagram is not
  transcribed, and writing one from inference would be a plausible guess this
  project refuses. `gs_swizzle.h` currently takes the nibble as
  `((lx >> 4) & 1) * 4 + sub`, where `sub` is the row's own 0..3 order, so two
  bits of the nibble index come from y and one from x. If the hardware divides
  them differently, rows collapse in pairs, which is the shape of the fault.

So the format the font is in decides which of those it is, and the log has
never carried it. That is what the two measurements below add.

### Two measurements for the two faults

**The batch trace**, for the placement fault. Armed on the first sampled field
and bounded to the twelve batches after it, at info. Each line carries the
batch's primitive count, the pixel grid it covers **after** XYOFFSET has been
subtracted, its `FRAME` block, FBW, PSM and FBMSK, the raw `XYOFFSET1` and
`XYOFFSET2` OFX and OFY so the subtraction itself can be checked, both
`SCISSOR` rectangles, and the sample count. That is the y the rasteriser was
actually given, against the 512 row buffer and the 256 line window, which is
the only thing that separates "the drawing lands too low" from "the CRTC shows
the wrong rows".

**The texture probe now finds the font.** It used to take the first textured
primitive of the batch, which on the probed batch was a frame-to-frame copy in
PSMCT32 and said nothing about the format drawn wrong. It now prefers the first
primitive whose TEX0 PSM is palettised (PSMT8, PSMT4, PSMT8H, PSMT4HL,
PSMT4HH) and falls back to any textured one. It is also armed differently: the
no-colour verdict no longer fires now that the picture has colour, so the probe
runs once on the first batch of the run that draws with a CLUT format, which is
the batch that draws the text. One readback per run.

**The coordinates as the rasteriser received them.** The shader's own
interpolation cannot produce a constant V down a sprite, but the record can:
`mix(v0, v1, ty)` is constant when `v0` equals `v1`, and that is a question
about GIF assembly rather than about the shader. The register order of UV or ST
against XYZ2 for a sprite, the FST select, and a per-vertex coordinate stored
only on the kick vertex all reach it. The font probe therefore also prints the
primitive's kind, its FST bit, its three window XY pairs in sixteenths and in
pixels, and its three UV pairs (or its nine STQ floats), with the verdict
spelled out: `v0` equal to `v1` on a sprite says the fault is in assembly and
not in the shader, and two corners carrying different coordinates says assembly
gave the shader a real gradient and the fault is below it, in the texel
addressing or the CLUT lookup.

Between them the next log says the font's TBP0, TBW, PSM, TW, TH, TCC, TFX,
CBP, CPSM, CSM, CSA and CLD, whether its pages hold colour on the device and on
the host, whether its CLUT reaches the shader, the coordinates its two corners
carry, and where in the 512 row buffer each batch actually drew.

**If the font is PSMT4**, the unverified nibble order is the first suspect and
settling it needs the manual page or a measured upload from the running game,
not another inference. **If it is PSMT8**, the addressing is already verified
against a transcribed table and the fault is the CLUT lookup instead: CSA, the
CPSM 16 against 32 expansion, or the CLD load rules.

### The font probe found a composite, not a glyph

    TEX0 TBP0 10240 (a block, not a page) TBW 8 PSM 0x1b 512x512, TCC=1 TFX=0,
      CBP 16000 CPSM 0x00 CSM=0 CSA=0 CLD=1
    kind 2 (sprite) with FST=1, window XY0 0,0 XY1 8192,8192
      (0.00,0.00 to 512.00,512.00 in pixels)
    UV is u0=8 v0=0, u1=8200 v1=0 ... v0 equals v1
    the texture occupies words 655360..933888; the device buffer has 0 of 278528
      nonzero there ... the host mirror has 0
    CLUT 256/256 both sides

That is not a glyph. It is a **full-screen 512x512 sprite** compositing a
PSMT8H layer at block 10240, a region past both frame buffers and past the Z
buffer at block 6144, and the region is **empty on both the host and the
device**. PSMT8H indices live in the high byte of a 32-bit word, which is the
same shape as the alpha-only frame buffer seen earlier in this investigation:
a buffer written by a draw whose FBMSK protects the low 24 bits, or by a
transfer. Nothing wrote it in this run.

Its `u` runs 8 to 8200 sixteenths, which is 0.5 to 512.5 texels across 512
pixels, an ordinary one to one mapping with the half-texel offset. Its `v` is 0
at both corners. Whether that is the game asking for one texel row or assembly
losing the second corner's V cannot be told from one primitive that samples an
empty region, so no conclusion is drawn from it here; the second probe line
below is what will settle it on a primitive that samples something.

The batch trace missed the text entirely: armed on the first sampled field, it
ran on fields 9 to 15 and recorded seven batches of screen clears into blocks 0
and 2048, long before any menu existed.

### Three changes, all host side

**The trace is armed where the text is.** It now starts on the field of the
first palettised batch rather than on the first sampled field, and each line
names the batch's first texture as well: TBP0, TBW and PSM beside the `FRAME`
block, FBW, PSM and FBMSK it draws into. So the sequence that builds the menu
is visible as a list of "this batch drew into that block reading this texture".

**The font probe reports two primitives.** The first palettised one, as before,
and, when it is a different primitive, **the first palettised one whose texture
range holds anything on the host mirror**. A range with data in it is a texture
something has actually uploaded, which is what a glyph sprite is; the first
palettised primitive of a batch turned out to be a composite of an empty layer
and said nothing about a glyph. When no palettised primitive of the batch
samples a range with anything in it, the probe says that instead of reporting
nothing.

**Every destination block is listed at the end of a run.** Two new stats lines,
one for the base blocks the rasteriser drew into with their FBW, PSM, FBMSK and
batch count, and one for the base blocks transfers wrote to with their FBW,
PSM and count, both capped at 24 distinct entries. "Who was supposed to write
block 10240" had no answer anywhere in the log, and asking it should not need a
hard-coded address: a block nothing writes is now visible by its absence from
both lists.

Between them the next log says which batch writes the PSMT8H layer, with what
FRAME PSM and FBMSK and where in y, or that nothing does; what a real glyph
sprite's TEX0 and per-vertex coordinates are; and whether the picture's
placement fault is the y the rasteriser is given.

### The block lists, and the contradiction they raised

    drawn into base blocks: 2048(bw 8 psm 0x00, 14100), 0(bw 8, 386),
      10240(bw 8 psm 0x00, 311), 10304(bw 8, 311), 14400(bw 4, 311),
      15424(bw 2, 311), 15680(bw 1, 311), 15872(bw 4, 131), 11776(bw 8, 926),
      10240(bw 4 psm 0x00, 3721), 10752(bw 4, 3408), 12288(bw 4 ...
    transfer destinations, base blocks: 16000(bw 1 psm 0x00, 1856),
      10240(bw 8 psm 0x00, 311), 10240(bw 2 psm 0x14, 2789),
      10496(bw 2 psm 0x14, 1244), 10752 ... 14080(bw 2 psm 0x14, 524)

That is the shape of the menu: a run of **PSMT4 uploads at BW 2 to blocks
10240 to 14080 in 256 block steps**, which is the font and the menu art, plus
**PSMCT32 at 16000 BW 1**, which is the CLUTs. The field 103 batch trace shows
the build: a 256x64 draw into 15872, then 512x512 into 11776 reading 2048, then
a ping-pong of 256x128 draws between 10240 and 10752 at BW 4 each reading the
other, with XYOFFSET1 30720/31744 and SCISSOR1 0..255 by 0..127. That is a
downscale or blur chain over a 256x128 layer, and the composite sprite reads
the result of it as PSMT8H.

**The apparent contradiction is timing, not a fault.** The ranges do overlap:
`gs_buffer_word_range` starts every view at `base_block * 64`, which is word
655360 for block 10240 whatever the buffer width is, and the PSMT8H BW 8
512x512 span of 278528 words is a superset of the PSMCT32 BW 4 256x128 span of
40960 words at the same start. So the probe's range does cover where the
ping-pong draws. What it does not cover is when: the probe fired on the
**first** palettised batch of the run, and the menu builds its layer late in
the field and composites the one from before it, so on the first occurrence the
layer really was empty. The probe measured a composite of something not yet
drawn and reported it as an empty texture, which reads as a fault and is not
one.

### Three measurements, all host side

**The font probe waits for a texture something has written.** A palettised
batch now only arms the probe when one of its palettised primitives samples a
range that either holds something on the host store or overlaps the range the
device has written. Both tests are host side and cost nothing per batch. That
is what stops it measuring a layer before the layer exists.

**Every traced batch reports what it left behind.** Each of the twelve trace
lines is now followed by the nonzero and colour word counts of that batch's own
write range, read off the device after its dispatch and its resolve. A batch
that draws into a block and leaves its own range empty is the whole picture
fault for whatever reads that block next, and the menu build is a chain of
exactly that shape: 15872, then 11776, then 10240 and 10752 in turn. Twelve
readbacks, once in a run.

**Every transfer reports what it left behind.** The first twelve transfers log
the nonzero word count of their own destination range on the host store they
write. A transfer whose range is still empty afterwards has landed its texels
somewhere other than where its own BITBLTBUF says, which garbles every glyph
that reads them, and the PSMT4 BW 2 uploads are the ones to watch. The count is
taken one transfer late, and again at each vsync, because a host to local
transfer's data arrives after TRXDIR in a separate image packet and is not
there yet at the moment the destination is decoded.

### What the game builds at block 10240

From the two lists and the trace, without inference beyond them: the font and
menu art arrive as PSMT4 uploads at BW 2 across blocks 10240 to 14080, the
CLUTs arrive as PSMCT32 at 16000, a 256x64 element is drawn into 15872, a
512x512 pass into 11776 reads the frame buffer at 2048, and a 256x128 layer is
worked between 10240 and 10752 at BW 4 with each pass reading the other. The
composite sprite then reads block 10240 as PSMT8H 512x512 over the whole
screen. So block 10240 is one region under three views: a PSMT4 upload target,
a PSMCT32 render target for the 256x128 chain, and a PSMT8H source for the
composite. Whether the text is rendered into that layer, and where in y, is
what the twelve trace lines with their content counts will say.

### The PSMT4 uploads land nothing, and where that is not

Measured, from the transfer check:

    a transfer to DBP 16000 DBW 1 DPSM 0x00 covers host words 1024000..1026048
      and 256 of 2048 are nonzero after it
    a transfer to DBP 10240 DBW 2 DPSM 0x14 covers host words 655360..673792
      and 0 of 18432 are nonzero after it

The 16x16 CLUT lands. **Every PSMT4 transfer checked leaves its destination
empty**, at 10240, 10496 and 10752. The field 105 trace shows the text drawn
into FRAME block 2048 by batches whose textures are TBP0 11600, 12096, 12256,
12288, 12320, 12448, 12456, 12488 and 12496 at TBW 2 PSM 0x14, which are those
same uploads, so the glyph sprites read whatever is left in those pages as
indices. That is the bars and the noise. The later probe reading TBP0 10240 as
PSMT8 512x512 with CLD 2 finds 73727 coloured words on both sides and is
consistent, because that layer is written by the render chain and not by a
transfer.

**What is not the cause, by measurement.** The swizzle selftest gained a fifth
check: every texel of a 64x64 region gets a distinct value written through
`LocalMemory::write_pixel` and read back through `read_pixel`, which exercises
the shift and the mask for the sub-word formats and `gs_word_mask` and
`gs_word_shift` for the three H formats. Those are a different piece of
arithmetic from the address, and the three older checks say nothing about them.
It was built and run:

    PSMT4     ok   placement + bijective + word + store/load
    PSMT8     ok   address + placement + bijective + store/load
    PSMCT16   ok   address + placement + bijective + store/load

all thirteen formats pass. So the per-texel store and load path is right for
PSMT4, PSMT8, PSMT8H, PSMT4HL, PSMT4HH and PSMCT16, and so is the address. The
format switch is not it either: `gs_transfer_bits(GS_PSMT4)` is 4, the "no
defined pixel width" warn never appears in the log, `stray image qwords` is 0
so no image packet is being discarded, and the run reports 118997232
host-to-local pixels written, which is the write loop running.

So the texels are written through a path that is verified correct, and the
range reads empty one transfer later. **Something zeroes them in between**, and
there is one candidate: `sync_from_device` copies device words over the host
store across `[m_gpu_first, m_gpu_last)` without asking whether the host has
written those words since. A readback whose range overlaps a fresh upload
overwrites it with the device's older copy, and the host's dirty span then
carries the clobbered values back to the device.

That is stated as the candidate and not as the finding, because the elimination
above is by exhaustion and the next log can settle it directly.

### The transfer check is now a warn, and says what reconciled over it

A transfer that leaves its range empty is never a game fault: the guest asked
for texels to be somewhere and this renderer put them nowhere. The line is a
**warn**, and it now carries how many device readbacks ran while the transfer
was in flight, the word range they covered, and whether that range overlaps the
destination. Two verdicts on the same line:

- **overlaps** names `sync_from_device` overwriting the upload with the
  device's older copy, which makes the fix the reconciliation and not the
  transfer.
- **does not overlap** says nothing reconciled over it and the writes never
  landed there at all, which puts the fault back in the transfer's own
  arithmetic.

Either way the next log answers it without another elimination. If it is the
first, the fix is the page granular tracker for the host dirty set and the
device written set that is already queued for the readback cost, because a min
and a max span cannot express "the host owns these words and the device owns
those".

## The block ownership tracker, 2026-09-05

Local memory has two copies in this renderer, the host store and the device
buffer, and until now each side's changes were tracked as one min and max
span. A span cannot express "the host owns these words and the device owns
those", and that cost two faults at once: a device readback overwrote a fresh
PSMT4 font upload with the device's older copy, so every glyph sprite read
leftovers as indices, and every reconciliation moved the whole span, 1.7 MB at
a time and about fourteen times a field, which is the 17 ms of gswait.

The unit is the **GS block**: 256 bytes, 64 words, 16384 of them in 4 MiB. It
is the smallest unit the swizzle keeps contiguous, so a run of blocks is a run
of words and one run is one copy. `LocalMemory` carries two bitmaps of 16384
bits, with the spans kept beside them purely as a fast reject.

    host_dirty      the host store holds words the device does not
    device_written  the device holds words the host store does not

A block in neither is one both copies agree about.

### The five rules as implemented

1. **A host write reconciles first.** `sync_for_transfer` calls
   `reconcile_from_device` over the union of the transfer's source and
   destination before any texel is written, and that reads back **only the
   blocks the device owns**, as contiguous runs. The write then lands and
   `write_pixel` marks its own block host-dirty, one bit per word written.
2. **A readback never overwrites a host-owned block.** `reconcile_from_device`
   walks the device bitmap over the range asked for, splits each device run on
   the host-dirty blocks inside it, and reads only the pieces that are
   device-owned and not host-owned. The runs are packed consecutively into the
   readback buffer, recorded on one command list and waited on once, so a
   range with nothing to reconcile costs **no submit at all**.
3. **The upload writes exactly the host-dirty blocks.** `upload_dirty_vram`
   walks the host bitmap as runs, copies each run, and clears them. After it
   both copies agree over every block it wrote, so nothing is marked either
   way and no readback is needed to make the upload safe.
   `host_current_for_upload` is now a named no-op that says why.
4. **A batch marks what it drew.** `gsr_flush_draws` calls
   `note_device_words` over the batch's own FRAME and ZBUF write range, which
   sets device-written and leaves host-dirty alone for those blocks.
5. **The shadow follows the runs.** `m_shadow.invalidate_words` is called once
   per uploaded run rather than once over the whole span, so a transfer no
   longer drops the samples of every page between its lowest and highest word.

### Where two owners can still meet

Inside one block. A block is 256 bytes and carries one bit per side, so if the
host writes part of it and the device writes another part with no
reconciliation in between, one side's bytes are lost and which side depends on
the order the marks arrived in.

**The ordering prevents it, and the counter proves it.** `gsr_flush_draws`
uploads every host-dirty block before it dispatches, so rule 4 never runs over
a host-owned block; and rule 1 reconciles before a host write lands on a
device-owned one. Where the two marks meet anyway,
`LocalMemory::both_owners()` counts it and the end-of-run summary says so at
**warn**, naming it as a fault of this renderer. A run with that line at zero
is a run where no block had two owners.

### The counters

The readback line now reads count, runs, blocks, words and megabytes, and
there is a second line for the uploads with the same shape. The next log
should show the readback volume fall by orders of magnitude: the old path
moved 2159812608 words in 5119 readbacks, which is the whole span every time.

### The selftest

`gs_swizzle_selftest` gained a sixth check, `check_ownership`, which needs no
device because the bitmaps, the spans and the run walk are all host logic. It
covers: a host write marking its own block and no other; the correct order
(upload, then draw) leaving a block in exactly one bitmap; the incorrect order
being counted by `both_owners`; run coalescing over adjacent blocks, a gap
splitting two runs, and the walk stopping at the end it was given; a range
straddling a block boundary marking both blocks and no third; and
`mark_all_dirty` marking the whole store host-side only. Built and run:

    tracker   ok       block ownership: marks, order, runs, boundaries
    gs-swizzle-selftest: pass.

### The shader blobs need regenerating

`scanout.comp` carries the merge fix, the two probe arms and the FFMD row rule
fix, and `ScanoutPush` is now 32 words, which is the whole 128 byte budget. The same two commands, in
this order, from the repository root, neither of them run here:

    ./tools/gen_gs_shaders.sh
    ./tools/gen_gs_shaders_dxil.sh

One run of each covers every shader change of this session. Neither making the
probe automatic nor splitting stage a's counts touched the GLSL: the trigger,
the pre-fill, the counts and the verdicts are all host side in
`gs_native.cpp`.

### The debug layer and the validation layer

Both already exist and both are opt-in at build time: configuring with
`-DCMAKE_CXX_FLAGS=-DICORECOMP_RHI_VALIDATION` sets `DeviceDesc::validation`
(`gs_native.cpp`), and the same flag spelt `-DICORECOMP_GEOM_CHECK` turns on
the per-packet geometry diagnostic in `hw/geomcheck.cpp`. Both were run-time
verbose channels until 2026-09-05 and are build defines now, because a
run-time switch for either is a cost on every call of a path that ships. On D3D12 that calls `EnableDebugLayer` before device
creation, adds `DXGI_CREATE_FACTORY_DEBUG`, and drains `ID3D12InfoQueue` after
every submit into `rt_log_error`/`rt_log_warn` under the `rhi` component. On
Vulkan it enables `VK_LAYER_KHRONOS_validation` with `VK_EXT_debug_utils` and
routes error and warning severities to the same place. Neither is on by
default, both say at startup whether they came up, and the value is latched
when the device is created, so it has to be set before the run starts.

Note that Vulkan's messenger subscribes to warning and error severities only,
so synchronization validation's informational output does not appear. Turning
on `VK_LAYER_KHRONOS_validation`'s synchronization validation feature is a
layer setting, not a code path this backend has.

### The 9078 readbacks

They come from `sync_from_device`, reached three ways: a CLUT load whose
source overlaps words the rasteriser wrote (`clut_written`), a transfer whose
source or destination overlaps them (`sync_for_transfer`), and the upload at
the top of a batch or a field (`host_current_for_upload`). Sixteen a field is
the game's own texture and CLUT traffic against a frame buffer the GPU is
writing, not a bug. The path cannot leave the scanout source stale: every
batch and every field calls `host_current_for_upload` and then
`upload_dirty_vram` before anything is dispatched, so a transfer that lands
mid-field reaches the device before the next batch reads it. It cannot be out
of order with the present either: there is one queue, and every submit in
this renderer is followed by a wait on its own timeline value.

## The Metal backend

`src/runtime/rhi/metal/`, built only on Apple platforms and only where CMake
finds the Metal, QuartzCore, Cocoa and Foundation frameworks. Objective-C++
(`.mm`) with ARC, not metal-cpp: the window path needs Core Animation and
AppKit whatever the Metal binding is, since SDL hands back an `NSWindow`, the
layer is a `CAMetalLayer` on its content view, and the backing scale comes off
`NSWindow`. metal-cpp would cover the `MTL*` half and leave the rest in
Objective-C anyway. Nothing is loaded at run time: unlike Vulkan, which
arrives through MoltenVK and so needs volk, Metal and Core Animation are on
every macOS this port targets.

**Nothing here has been compiled.** There is no macOS toolchain on the
machines this project is developed on, so every line of this backend ships
uncompiled: the CI job that built it (`runtime-macos-arm64-pgs`, the
`macos-arm64-release` preset with Homebrew's MoltenVK) never ran green and
was removed on 2026-09-05 with the native renderer's withdrawal; its steps
are in the history of `.github/workflows/ci.yml` at commit 9096334. The
`macos` CI job builds the stub preset only.

The device comes from `MTLCreateSystemDefaultDevice`. `MTLGPUFamilyApple7`
(Apple silicon, M1 and later) or `MTLGPUFamilyMac2` is the floor, the highest
family the device reports is logged, and a device below both is a loud fatal
naming it. Apple7 is the floor because this port targets arm64 macOS 14 and
nothing older; Mac2 is accepted so an Intel Mac reports a device rather than a
refusal, which is a better failure report than silence. One command queue, for
the reason the D3D12 backend gives for one direct queue.

Two `DeviceDesc` fields land differently here than on the other two backends,
and both are loud rather than quiet:

- `prefer_software` is a fatal. Metal has no software rasteriser, there being
  no equivalent of WARP or lavapipe, and handing back a GPU to a caller that
  asked for the reference would be the worst of the three answers.
- `validation` is a log line. Metal's API validation is a loader setting and
  not something a process can switch on for itself, unlike the Vulkan
  validation layer and the D3D12 debug layer, so the backend says which
  environment variable turns it on and carries on.

Memory is `MTLStorageModeShared` for everything the CPU touches and
`MTLStorageModePrivate` for the rest, with no `didModifyRange` anywhere. That
is correct on Apple silicon, where Shared is coherent, and it is the reason
the backend is arm64 only in practice: an Intel Mac reached through Mac2 would
need `MTLStorageModeManaged` for the same buffers, and that path is not
written.

### Binding model

Vulkan has one descriptor namespace; Metal has three independent argument
tables (buffer, texture, sampler) and SPIRV-Cross picks an index in each by
walking whichever resources a shader happens to declare, so two shaders using
different subsets of rhi.h's layout would disagree about where a slot is. The
mapping is therefore fixed by `src/runtime/rhi/metal/rhi_metal_bindings.h`,
and the MSL generator rewrites every index to match:

| rhi.h binding | count | Metal table | index |
| --- | --- | --- | --- |
| 0 uniform buffer | 4 | buffer | 0-3 |
| 1 storage buffer | 16 | buffer | 4-19 |
| 2 sampled texture | 8 | texture | 0-7 |
| 3 sampler | 4 | sampler | 0-3 |
| 4 storage image | 4 | texture | 8-11 |
| push constants, 128 bytes | 1 | buffer | 20 |
| the vertex buffer | 1 | buffer | 21 |

A storage buffer array costs sixteen buffer indices and not one. Metal has no
arrays of buffers, so SPIRV-Cross flattens `buffer Block { } g_buf[16]` into
sixteen entry point arguments at consecutive indices and rebuilds a local
pointer array from them. Texture and sampler arrays are native:
`array<texture2d<float>, 8> g_textures [[texture(0)]]` is one argument
covering eight consecutive indices.

**No argument buffers.** The whole layout is 22 of the 31 buffer indices, 12
of the 128 texture indices and 4 of the 16 sampler indices a macOS device
provides, so every slot fits the direct binding limits. An argument buffer
would add an encoder, a residency call per submit and a second place for the
indices to be written down, for nothing this layout needs.

The vertex buffer's index is ours and not SPIRV-Cross's: a vertex stage takes
its attributes through `[[stage_in]]`, and which buffer index feeds them is
set by the `MTLVertexDescriptor` the pipeline is built with.

Indices 22 and up are left free. SPIRV-Cross reserves the high end of the
buffer table for auxiliary buffers it can emit on its own (swizzle constants
at 30, buffer sizes at 25, tessellation and multiview buffers between them).
None of these shaders makes it emit one, and the generator's rewrite errors on
any argument it cannot name, so one appearing later is a build failure rather
than a silent collision.

The four samplers are `MTLSamplerState` objects created once at device
creation and bound into every encoder, which is what rhi.h calls immutable.
Their filters and address modes mirror the Vulkan set one for one:
nearest/clamp, linear/clamp, nearest/repeat, linear/repeat, mip point, no LOD
clamp.

One difference worth writing down because it is invisible until it matters: a
Metal buffer argument carries an offset and no length, so
`bind_storage_buffer`'s `range` is recorded and not enforced. A shader that
reads past it reads the rest of the buffer, where the Vulkan backend's
`VkDescriptorBufferInfo.range` and its validation layer would catch it.

### Encoders, barriers and copies

One `MTLCommandBuffer` per rhi.h submit. Metal has one encoder open at a time
and no way to interleave two kinds, so the command list keeps whichever
encoder it has open and closes it when an operation needs a different one:
compute for dispatches, blit for copies, render for the present blit and the
overlay. Bindings stand until changed and are written into the encoder at each
dispatch and draw, the same rule as the other two backends, so nothing cached
can leak between two dispatches.

Barriers map in two cases:

- **Inside a compute encoder**, which is where every dependency the renderer
  states actually lands, `buffer_barrier` and `texture_barrier` become a
  `memoryBarrierWithScope:` with `MTLBarrierScopeBuffers` or
  `MTLBarrierScopeTextures`.
- **Anywhere else** there is nothing to do. Metal orders the work of two
  encoders on one command buffer and makes the first's writes visible to the
  second for every resource with hazard tracking on, which is every resource
  this backend creates, so the encoder switch the next operation forces is
  already the dependency.

The compute encoders are created with `MTLDispatchTypeConcurrent`, and that is
the riskiest decision in the backend. Concurrent means two dispatches in one
encoder may overlap unless the caller ordered them, which is exactly rhi.h's
contract and exactly what the Vulkan backend enforces with
`vkCmdPipelineBarrier2`. `MTLDispatchTypeSerial` would order every dispatch
whether the caller asked or not, papering over a missing barrier that Vulkan
would report as a race. If a dispatch pair ever turns out to need an order
rhi.h was never told about, the one-line change is the dispatch type in
`compute_encoder()` and the bug is in the caller.

The rhi.h `Stage` and `Access` arguments carry no further information here.
Metal's barrier scope is the resource kind and not the pipeline stage, and
there is no layout to move: a texture has no Vulkan-style layout and no D3D12
resource state, so there is no `transition` helper in this backend at all.

Copies are the one place Metal is simpler than D3D12. `MTLBlitCommandEncoder`
takes a row pitch of the caller's choosing and places the footprint at any
offset that is a multiple of the pixel size, so rhi.h's tightly packed
contract needs no padding, no scratch buffer and no row-by-row repack. An
offset that is not a multiple of the pixel size is a fatal naming it rather
than a rounded value.

`dispatch_indirect` uses `dispatchThreadgroupsWithIndirectBuffer:`, which
reads three 32-bit group counts with a 12-byte stride: the same three words a
`VkDispatchIndirectCommand` holds and the same layout the D3D12 command
signature declares, so the caller's buffer is identical for all three.

Metal takes the threadgroup size at the dispatch where Vulkan bakes it into
the shader, so it travels with the source: the generator reads each compute
shader's own `layout(local_size_*)` line and puts it in the MSL index, and the
device checks it against `maxTotalThreadsPerThreadgroup` when the pipeline is
built.

`blit_texture` is a draw and not a copy, for the same reason it is on D3D12:
`MTLBlitCommandEncoder`'s texture copies neither scale nor filter, so the
present blit is one full-screen triangle from the vertex id, placed by the
viewport, with the source in sampled texture slot 0 and the filter chosen by a
push constant. Its shader is the backend's own, written in MSL inside
`rhi_metal_shaders.mm`, because there is no GLSL twin to cross-compile.

Two smaller differences from the Vulkan backend, both fatal or logged rather
than silent:

- A uniform buffer's bind offset must be a multiple of 256, not 4. Mac2
  requires that of Metal's constant address space and Apple silicon requires
  only 4; the strict rule is enforced on both so a binding that works on one
  accepted family works on the other.
- Metal rejects a scissor rectangle that leaves the attachment where Vulkan
  clamps one silently. The rectangle is clamped here with a warning naming it
  and the attachment, because a clamp is a divergence from what the caller
  asked for. Nothing in the renderer or the UI is known to hit it.

`MTLSharedEvent` is the timeline, one value per submit, with the same
semantics rhi.h defines for the Vulkan timeline semaphore. `wait` uses
`waitUntilSignaledValue:timeoutMS:` with a ten second timeout, and a timeout
is a fatal: it is long enough that no frame this renderer draws can reach it
and short enough that a hung GPU is reported rather than hanging the process
for good. Readback is a shared-storage buffer read after the submit that
filled it has been waited on, exactly as on the other two.

### Present modes

Core Animation has no present modes, so rhi.h's three become one property and
a drawable count on a `CAMetalLayer` attached to the `NSWindow`'s content
view. **Metal has no tearing mode**, so one of the three cannot mean what it
means on Vulkan:

| rhi.h | CAMetalLayer | what it actually does |
| --- | --- | --- |
| `fifo` | `displaySyncEnabled = YES` | one present per vertical blank, `nextDrawable` blocks when every drawable is in use. Matches Vulkan FIFO. |
| `mailbox` | `displaySyncEnabled = NO`, `maximumDrawableCount = 3` | the compositor takes the most recently completed present and discards the ones behind it. Not identical: Core Animation still hands the picture over at a vertical blank, where Vulkan's mailbox need not. |
| `immediate` | the same as `mailbox` | there is nothing in Core Animation that tears, so this is not immediate's Vulkan meaning. The backend logs that, rather than letting a setting claim an effect it has not got. |

Because both are layer properties, `set_present_mode` rebuilds nothing. That
matches D3D12 and differs from Vulkan, where the mode is baked into the
swapchain object.

The layer is `BGRA8Unorm`, the only format every macOS display path takes
without a conversion, and `framebufferOnly = NO` rather than the default
`YES`: the screenshot path reads a backbuffer back, and a framebuffer-only
drawable texture can be an attachment and nothing else.

`present()` puts `presentDrawable:` on a command buffer of its own with
nothing else in it. Metal schedules a present on a command buffer, and rhi.h
calls `present` after `submit` rather than as part of it; command buffers run
in the order they were committed on one queue, so this one lands after the
submit that drew. The cost is one empty command buffer per frame. Attaching
the present inside `submit` would have made those two calls mean something
different here than in the other two backends.

Layer properties are mutated inside an explicit `CATransaction` with actions
disabled. The renderer runs on the GS worker thread, and Core Animation
batches property changes into the transaction of whichever thread makes them;
on the main thread the run loop commits it, on any other thread nothing does.
Whether that is enough on a real Mac is untested, and it is the first thing to
check if the picture is the wrong size or never appears.

### Shader path

One source, logged at info the first time a pipeline is built: the committed
MSL in `src/runtime/rhi/rhi_shaders_msl.h`, produced from the same SPIR-V as
the other two backends by `tools/gen_gs_shaders.sh`, compiled at run time with
`newLibraryWithSource:options:error:`. The committed text also lives under
`src/runtime/gs/render/shaders/msl/` so it can be read in review.

There is no ahead-of-time `.metallib`, which is the one place this backend
differs from D3D12's two-path shader loader. Producing one needs Xcode's metal
compiler, which runs on macOS only and cannot be redistributed through this
repository, so nothing on the machines this project is developed on could
build it; and a `.metallib` is bound to the Metal toolchain that wrote it,
where DXIL is bound only to being signed.

What replaces it is an `MTLBinaryArchive` under the executable's `cache/`
directory, the same place the paraLLEl-GS backend keeps its Vulkan pipeline
cache. It is a cache and not a build product, and it is worth being exact
about what it saves: the pipeline compile, not the MSL to AIR compile. A cold
start still parses four MSL translation units. A missing, unreadable or
rejected archive is a log line and an empty archive, never a failure, which is
the rule the Vulkan cache follows for the same reason.

Two compile options are decisions and not defaults. `languageVersion` is
pinned to MSL 2.3, the version the generator asked SPIRV-Cross for, because a
newer toolchain would otherwise accept source the pinned generator would not
produce and the difference would only show up on someone else's Mac.
`fastMathEnabled` is turned **off**, against its default: it lets the compiler
reassociate and contract float arithmetic, and the rasteriser and the CRTC are
being checked against the hardware bit for bit, so a reassociation the other
two backends do not perform would read as this backend disagreeing with them
and nothing would say why.

Like D3D12, a shader is identified by the SPIR-V array the caller passed:
every one of those pointers comes from an accessor in the generated
`rhi_shaders.h`, so the array address is a stable identity that costs nothing
in rhi.h, and a blob from anywhere else is a fatal naming the pipeline rather
than a silent guess. The entry point name is read out of the generated MSL
rather than assumed, because SPIRV-Cross renames `main`.

`--flip-vert-y` is passed on the vertex stages for the same reason it is for
HLSL: Metal's clip space is D3D's, with +Y up, and `overlay.vert` is written
for Vulkan's, so SPIRV-Cross negates the clip Y and all three backends put the
picture the same way up.

### GLSL portability

Every construct the four shaders use, and how it crosses. All of this is read
off the SPIRV-Cross MSL backend
(`third_party/parallel-gs/Granite/third_party/spirv-cross/spirv_msl.cpp`,
which is the tool and not Granite's renderer), not inferred from the MSL
specification.

- `imulExtended` and `umulExtended` (`gs_prim.h`): **no prelude needed**, and
  this is where Metal is better off than HLSL. `OpSMulExtended` and
  `OpUMulExtended` become a call to a `spvMulExtended` helper SPIRV-Cross
  emits itself, whose body is `return T{U(l * r), U(mulhi(l, r))};`
  (`spirv_msl.cpp`, `SPVFuncImplMulExtended`). `mulhi` is a Metal standard
  library function, so the exact 64-bit product the edge function needs is
  native. The HLSL stage has to prepend hand-written definitions for the same
  three builtins because HLSL has no intrinsic for any of them; the MSL stage
  prepends nothing.
  The one thing to know: SPIRV-Cross marks that helper `[[clang::optnone]]`,
  with the comment that the compiler "may hit an internal error with mulhi,
  but doesn't when encapsulated". So the rasteriser's hot path calls an
  unoptimised function per extended multiply. That is a performance note, not
  a correctness one, and it is the first thing to look at if this backend
  turns out slower than the other two on the same picture.
- `uaddCarry` (`gs_prim.h`): also no helper. `OpIAddCarry` is emitted inline
  as `sum = a + b` and `carry = select(1, 0, sum >= max(a, b))`
  (`spirv_msl.cpp`, `case OpIAddCarry`), which is the correct unsigned carry.
- `atomicAnd` and `atomicOr` on storage buffer words (`raster.comp`): become
  `atomic_fetch_and_explicit((device atomic_uint*)&..., memory_order_relaxed)`
  and the `or` twin. The cast from a plain `uint` in the buffer to
  `device atomic_uint*` is SPIRV-Cross's own pattern, not ours.
- `buffer Block { } g_buf[16]`: flattened into sixteen buffer arguments, as
  the binding model above says. This is the one construct whose Metal shape is
  structurally different from both other backends.
- `shared uint s_color[256]`, `s_depth[256]`: `threadgroup` arrays, moved into
  the kernel function and passed to the helpers, which SPIRV-Cross does on its
  own. `raster.comp` has no `barrier()` in it at all: no thread ever reads a
  word another thread wrote, so there is nothing for the threadgroup memory
  model to differ about.
- Separate `texture2D` and `sampler` combined at the use site with
  `sampler2D(...)` (`overlay.frag`): `g_textures[0].sample(g_samplers[1], uv)`.
  Metal's textures and samplers are already separate, so this is the shape it
  wants. The index is a constant, which matters: a dynamic index into a
  sampler array is an argument buffer feature.
- `writeonly image2D` with the `rgba8` qualifier (`scanout.comp`):
  `array<texture2d<float, access::write>, 4>`, and `imageStore` becomes
  `.write(value, coord)`. Stores only.
- 64-bit integers: none reach MSL. `gs_prim.h` uses `int64_t` and `uint64_t`
  only in its C++ branch; the GLSL branch is written in two 32-bit words
  through `imulExtended` and `uaddCarry` precisely so no device has to offer
  `shaderInt64`. Metal has 64-bit integers, so this would have translated, but
  it never arises.
- No subgroup operations, no 8- or 16-bit storage, no buffer device address,
  no descriptor indexing beyond the fixed arrays. rhi.h excludes all of them
  on purpose, and every one of them is an argument buffer or a Metal 3 feature
  that would have needed a floor higher than Apple7.

One rewrite the generator has to do, and it is the whole of the MSL stage's
own work: reassign every `[[buffer(N)]]`, `[[texture(N)]]` and
`[[sampler(N)]]` to the table above. SPIRV-Cross assigns them by walking the
resources a shader declares, so `scanout.comp`, which declares no sampled
textures, would otherwise put its storage images where `overlay.frag` puts its
textures. The rewrite is by argument name and errors on a name the table does
not hold, which is also what catches an auxiliary buffer SPIRV-Cross emitted
on its own.

### What is verified and what is not

Nothing. This backend has never been compiled, by anyone, anywhere. There is
no macOS toolchain on the machines this project is developed on and, by
standing instruction, no GS renderer is run on them either.

What the macOS CI job settles, and it is the whole list:

- That CMake finds the four frameworks and reports
  `icorecomp: Metal backend enabled`.
- That the three Objective-C++ translation units compile under Apple clang for
  arm64 with ARC and the macOS 14 SDK, and link into both
  `icorecomp-runtime` and `icorecomp-gs-replay`.
- That the renderer's disc-free selftests still pass on arm64. Those exercise
  the shared C++ half and open no device, so they say nothing about Metal.

Not verified until someone runs it on a Mac, which is everything else:

- That the Metal compiler accepts the generated MSL. In particular the
  flattened sixteen-buffer storage array, the `array<sampler, 4>` argument,
  the `spvMulExtended` helper and the atomic casts.
- Every run-time behaviour: device selection, the argument tables, the
  concurrent compute encoder with its explicit barriers, the copies, the
  binary archive, the layer, all three present modes and the present blit.
- Whether `CATransaction` from the GS worker thread is enough to publish the
  layer's geometry, which is the single most likely thing to be wrong.
- Whether a single frame is correct. No picture from this backend has been
  looked at.

There is no equivalent of the WARP replay that gives D3D12 a CI-side parity
check without a GPU: `macos-14` runners do expose a Metal device, but the
replay tool would have to be taught to select this backend first, and
`ICORECOMP_GS_BACKEND` is not wired to `rhi::Backend` yet. That is the next step
and the one that would turn most of the list above into a measurement.

## Measured versus inferred

Stated here as well as in the code, because this is the list that has to be
settled before the renderer can be called accurate.

Measured, on ICO, and recorded in this repository:

- NTSC's visible area is 2560 video clocks from clock 636 and 448 raster lines
  from line 50. Gameplay programs exactly that window (DW+1 2560, DH+1 448,
  DX 636, DY 50, MAGH 4, so 512 pixels by 224 lines per field). The attract
  movie programs DW+1 2880, DH+1 480, MAGH 3 from the same corner, which is
  720 by 240.
- The movie's DISPFB2 carries DBX 36, DBY 12.
- The game renders its frame at 512x448 into local memory at byte 0x80000.

Contested, not settled:

- **The FFMD row rule**: which buffer row a circuit reads for output field
  line L when SMODE2 FFMD is 1. Two rules are on the table.
  - *stride 1*: `row = DBY + (L - c_y)`, the same read as FFMD 0. This is
    what paraLLEl-GS does: `compute_circuit_rect` sets `phase_stride` to 2
    only for `alternative_sampling`, which is `INT && !FFMD`
    (`third_party/parallel-gs/gs/gs_renderer.cpp:4176-4178`), so FFMD takes
    stride 1, and
    `third_party/parallel-gs/gs/shaders/sample_circuit.frag:108-109` computes
    `coord = single_sampled_coord * uvec2(1, phase_stride) + uvec2(dbx, dby + phase)`,
    which never scales DBY. PCSX2 halves the frame rect for FFMD, which is
    the same statement.
  - *stride 2*: `row = DBY + (L - c_y) * 2 + field`. This is what
    `gs/render/shaders/scanout.comp` runs today.

  The argument that used to settle this in favour of stride 2, that the
  movie's DBY 12 skips 24 buffer rows because the decoded picture carries 8
  and 17 row black borders, is an inference from a picture and not a
  measurement of the CRTC, so it is withdrawn as the deciding evidence. The
  2026-09-04 cross check that motivated dropping the old `INT && FFMD` gate
  measured this renderer's own local memory, that is, its draw path: it shows
  the window was wrong, not which of the two rules is right. DBY is no longer
  doubled under either rule; scaling a register the game supplied has no
  support in any hardware description or reference implementation.

  The measurement that settles it: replay one captured gameplay field through
  both renderers and compare the top output row against buffer row 0 and
  against buffer row 1.
- **The PAL visible area and its origin.** The mode itself is exercised (the
  project retargeted to `SCES_507.60` on 2026-09-04 and a PAL run programs
  CMOD 3), but `kModeLinesPal = 512` and the NTSC origin the PAL arm of
  `gs/render/gs_crtc.cpp` reuses have not been checked against a PAL raster,
  so the placement inside the frame is unverified. The renderer warns once
  per run saying so.

Inferred, not measured, and each one says so where it is written:

- **The sample position**, `px * 16 + 8`. See "Rasterisation rules". One
  captured frame containing a sprite at a fractional position settles it.
- **The line DDA.** The major axis rule and the minor rounding are this
  renderer's own model; `gs_draw.cpp` says so once per run. ICO draws very
  few lines, so this is low value to settle and cheap to correct once a
  capture shows one. Three parts of it: the minor rounding, the endpoint
  convention (first inclusive, second exclusive, taken from the sprite rule),
  and what a line whose endpoints share one pixel of the major axis draws
  (one pixel, at the first vertex).
- **AA1's coverage.** That coverage is the covered area of the pixel square,
  that a corner takes the smallest of its edges' areas rather than their
  intersection, that it quantises to the 129 values 0 to 0x80, that it
  replaces the source alpha only where the blend unit's C selector reads it,
  and that a pixel outside the sample rule is drawn only with ABE set. The
  manual gives the sentence and not the arithmetic. `draw-aa1-tri.gs` is the
  case a capture would be compared against.
- **The AA1 line width.** That an AA1 line's coverage is a tent one pixel wide
  on the minor axis, so at most two minor pixels of each major step are drawn
  and their coverages sum to one. The manual gives neither the width nor the
  falloff.
- **The write order of an aliased FRAME and ZBUF.** Colour first, depth
  second, so the depth is what the memory holds where the two overlap. The
  manual has one pixel operation write both and does not order the two.
- **The adaptive deinterlacer.** All of it, by construction: the hardware has
  no adaptive mode. The threshold of 24 out of 255 is a chosen constant.
- **Everything render scale does above 1.** The sub-sample positions, that
  colour resolves by averaging and depth by taking the sample nearest the
  pixel centre, that AA1's coverage is left to the sub-samples above scale 1,
  and that the high-resolution scanout is double resolution and not
  deinterlaced. None of it is a claim about hardware: the hardware produced
  one sample per pixel, and these are host-side choices about a picture it
  never drew. They are listed here so that nothing above scale 1 is mistaken
  for an accuracy claim.
- **Colour and fog rounding.** Round to nearest. The manual gives the fog
  formula and not its rounding, and gives no rounding at all for the Gouraud
  DDA.
- **The blend unit's shift.** `((A - B) * C) >> 7 + D` with an arithmetic
  shift, so a negative intermediate floors. That is what a shift-and-add unit
  does; the manual does not say.
- **ZTE clear.** Treated as ZTST ALWAYS, with ZMSK left as the only thing that
  stops a Z write. The manual describes ZTE as the test's enable and says
  nothing about it disabling the write.
- **The 16-bit FBMSK mapping.** FBMSK is 32 bits wide whatever the frame
  buffer format is; for the 16-bit formats this renderer takes bits 3..7,
  11..15, 19..23 and 31, which are the positions the 5-5-5-1 fields occupy in
  the expanded 32-bit pixel. Derived from the field positions, not measured.
- **The texture coordinate's fractional width.** Four bits on both paths. The
  UV register carries four; whether the STQ path's internal DDA carries more
  is not stated. It decides how finely a bilinear blend's weights step, so a
  magnified sprite is where a capture would show it.
- **The LOD split.** That LOD's integer part selects the mip level and its
  fraction blends between two levels. The manual gives the formula and not
  the split.
- **MTBA's automatic mip bases.** Each level packed immediately after the
  previous one, with the width halving and never falling below one. The
  manual describes the packing and does not write the arithmetic out.
- **The CSM1 CLUT arrangement.** That the 16 by 16 arrangement exchanges bits
  3 and 4 of the index. Read off the manual's CLUT diagram rather than a
  formula. One captured 8-bit palette upload settles it: with the wrong rule
  a palette comes out permuted in blocks of eight, which is loud rather than
  subtle. `draw-texture-psmt8.gs` exercises the whole path but cannot settle
  this on its own, because the generator writes the palette through the same
  function the renderer reads it with.
- **REGION_CLAMP and REGION_REPEAT across mip levels.** MINU, MAXU, MINV and
  MAXV are applied unscaled at every level. Whether the region bounds follow
  a level's halving is not stated.
- **The Z formats as textures.** TEX0's PSM field is six bits and can name
  PSMZ32, PSMZ24, PSMZ16 and PSMZ16S. This renderer reads them as the colour
  format of the same width. The manual does not list them as texture formats
  at all.
- **CSM2 with a 32-bit CLUT.** Loaded as the same linear line at 32 bits,
  with one log line. The manual describes CSM2 for 16-bit CLUTs.
- **CLD 6 and 7.** Not in the manual's table. They load, which is what CLD 1
  does, and say so once.
- **The bilinear blend's rounding.** Round to nearest, the same choice the
  Gouraud DDA made.
- **DIMX's index order.** Which of DIMX's two indices the manual calls the row
  is inferred; this renderer indexes by the low two bits of y then of x.
- **The top-left tie rule.** That a sample point exactly on an edge is drawn
  only when that edge is a left or a top edge. The manual gives the drawing
  kick and the sample position and does not state a fill rule in those words,
  so `gs_prim.h`'s `gs_edge_tie` is this renderer's choice. It is the same
  capture that settles the sample position: one measured sprite or triangle
  pair at a fractional position. The raster selftest's shared-edge pair proves
  the two halves tile exactly, which any consistent tie rule gives, so it
  cannot tell top-left from bottom-right.
- **CSA on an 8-bit texture.** Ignored: a 256-entry palette starts at CLUT
  entry 0 whatever CSA says, on both the load side (`gs_clut.cpp`) and the
  lookup side (`gs_texture.h`'s `gs_clut_entry`). With a 32-bit CLUT the 256
  entries fill the buffer and CSA has nowhere to point, but with a 16-bit CLUT
  they fill half of the 512 half-word entries and CSA 16 would name the other
  half. One captured 8-bit texture with a 16-bit CLUT at a nonzero CSA settles
  it, and gets loudly wrong colours if this is the wrong rule. No case in
  `gs_gen_dump`'s corpus uses a nonzero CSA.
- **The LOD substituted for a Q of zero or a denormal.** `gs_tex_lod` returns
  `MXL * 16`, so the sample lands on the smallest level this texture has
  rather than on a NaN. Nothing read here says what the hardware does with a Q
  that sends log2 to infinity, and a shader cannot log the substitution. The
  pin is derived from MXL rather than written as a literal, so it cannot drift
  from the clamp `raster.comp` applies to the level.
- **The circuit merge's rounding.** `scanout.comp`'s `blend_channel` uses the
  same arithmetic shift the pixel blend unit uses, `b + (((f - b) * a) >> 7)`,
  so a negative intermediate floors in both. It used to divide by 128, which
  truncates toward zero and differs by one for every negative product that is
  not a multiple of 128. There is one alpha unit on the GS and this is now one
  rounding, but which rounding the merge circuit performs is not measured; ICO
  enables one circuit, so no frame in this game exercises a partial ALP.

- The PAL visible area and its origin. The game's own display option can
  program CMOD 3 (docs/TARGET.md, "The picture"), but no run of this
  renderer has been made on that setting, so there is nothing measured to
  take the origin and clock count from; `hw/gspriv.cpp` fatals on any mode
  that is neither NTSC nor PAL, so the two are all `gs_crtc.cpp` has to
  cover. It keeps the NTSC origin and clock count with 512 raster lines and
  logs the substitution once.
- The circuit merge's blend unit. The GS treats 0x80 as one for alpha, so the
  merge shifts by 7 and clamps, the same arithmetic the pixel blend unit uses.
  See "The circuit merge's rounding" above. ICO enables one circuit, so no
  frame in this game exercises a partial ALP.
- **What the merge outputs when EN1 is 0.** SLBG says what output circuit 1 is
  blended against, so it is a property of that blend, and `scanout.comp` now
  reads it only when circuit 1 is enabled: with EN1 0 the display is circuit
  2's output where circuit 2's window covers the pixel and BGCOLOR everywhere
  else, whatever SLBG says. Inferred, not measured on hardware. It is
  corroborated two ways: paraLLEl-GS shows this game's picture from the same
  register writes on the same run, and a GS that blanked the screen for EN1 0
  with SLBG 1 would blank it for every title that leaves its display
  environment at that setting. Reading SLBG unconditionally is what made the
  2026-09-04 run black; see "The first run on real hardware".
- **The visible line count of a non-interlaced raster.** `mode_area` halves
  the mode's line count for every raster, not only an interlaced one, because
  `kModeLinesNtsc` and `kModeLinesPal` are the interlaced raster's visible
  lines, which is the unit DISPLAY DH and DY carry when INT is 1, and a
  non-interlaced raster has half as many lines to begin with. The NTSC half is
  measured through ICO's own interlaced gameplay window; the non-interlaced
  half is corroborated by ICO PAL programming DH+1 256 with MAGV 0 against a
  PAL mode line count of 512, and by the aspect derivation already counting a
  non-interlaced DH twice against the same total.
- **Known gap: which of the eight nibbles inside a word a PSMT4 pixel
  occupies.** The manual's 32x4 PSMT4 column diagram is not transcribed into
  `gs_swizzle_reference.cpp`, and writing 512 entries this project cannot
  vouch for would be a plausible guess, which is worse than a stated gap. What
  the selftest does establish for PSMT4:

  - its block and its 64-byte column match the transcribed page diagram, at
    four buffer bases and three widths;
  - its addresses over one whole page are a permutation of that page's 16384
    nibbles, so nothing collides and nothing is left unaddressed;
  - for x below 16 its *word* inside the block matches the word the literal
    PSMT8 column table puts the same block-local pixel in, which is a
    transcribed table and not the header's own rule.

  What is left is the 3-bit sub-position inside that word, and the extra
  nibble bit x >= 16 contributes. The rule `gs_swizzle.h` uses for it is the
  8-bit column rule with one more x bit, and check 1 verifies that 8-bit rule
  against the transcribed `columnTable8` exactly, at every coordinate and
  every base. Settling the remaining bit needs the manual page or one measured
  PSMT4 upload from the running game.
- Bob's half-line offset is applied as one output row. That is as close as a
  frame-sized image gets to half a raster line; a resampling bob is a filter
  choice this project has not measured against the movie.

## First run on hardware

Nothing in the native backends had ever executed on a GPU before this section
was written. What follows is what to set, what the log has to show, in order,
and what each early failure means. Every line below is a real log line from
the code; none of it is a description of intent.

### What to set

The environment:

    ICORECOMP_GS_BACKEND=d3d12

or `vulkan`, `metal`, `parallel-gs`, or `auto` (which is paraLLEl-GS wherever
that backend is built, because the native renderer has not passed its parity
gate). There is no settings key: `display.backend` was retired on 2026-09-05,
so putting `"display": { "backend": "d3d12" }` in `settings.json` now
produces one info line and a run on paraLLEl-GS.

`ICORECOMP_GS=native` is a separate switch and is not needed:
`ICORECOMP_GS_BACKEND` alone picks the renderer as well as the API.

Turning on the API's own validation is `ICORECOMP_VERBOSE=rhi`. On D3D12 that
is the debug layer, whose messages are drained into the log after every
submit; on Vulkan it is `VK_LAYER_KHRONOS_validation` and the debug messenger.
Both are checked for before they are asked for, so a machine without them logs
a warning and runs on.

### What the log must show, in order

    [gs ] GS backend: the native renderer on D3D12, from ICORECOMP_GS_BACKEND = d3d12 ...
    [rhi] D3D12 device: NVIDIA GeForce RTX 3090 (D3D12 feature level 12_0,
          resource binding tier 3), swapchain
    [rhi] D3D12 shaders come from the committed DXIL (rhi_shaders_dxil.h)
    [gsr] native GS renderer active on NVIDIA GeForce RTX 3090 (...), d3d12 backend

and on Vulkan the same four with

    [rhi] Vulkan device: NVIDIA GeForce RTX 3090 (Vulkan 1.3.x), swapchain

The order is fixed by construction: the backend is resolved and the window
opened before the device exists, the device line is the last thing the device
constructor does, the shader path line is printed when the first pipeline is
built, and the renderer line follows the three pipelines.

The first present has no line of its own. What says it happened is
`display.overlay` showing anything at all, and the run's end:

    [gsr]   presents=N of which repeats=M

`presents=0` with a window on screen means no present ever completed. Three
lines are warnings that say why, and one of them will be there:

    [gsr] the run has a window but the d3d12 device was created headless ...
    [gsr] present_pump: this run has a window but the d3d12 device has no swapchain ...
    [rhi] the window's client area is 0x0, so there is nothing to build a swapchain on

### What each early fatal means

`Direct3D 12 is not available on this system (...)`. `d3d12.dll` or `dxgi.dll`
is missing or too old. Use `ICORECOMP_GS_BACKEND=vulkan` or the paraLLEl-GS
backend.

`no Direct3D 12 adapter on this system meets the renderer's requirements
(feature level 12_0)`. Every adapter was skipped, and each skip logged its own
warning naming the adapter. The renderer needs feature level 12_0 for resource
binding tier 2: it binds twenty UAVs in one descriptor table.

`no DXIL is compiled in for <shader> and the committed HLSL could not be
compiled at run time: ...`. This build has no `rhi_shaders_dxil.h` and the
run-time compiler could not be loaded. The message carries the full path that
was tried and the Win32 error. Error 126 is `ERROR_MOD_NOT_FOUND`: the file is
there and something it imports is not, which for these two DLLs means the
Visual C++ redistributable. Error 2 is the file itself being absent.

`compute pipeline <name> failed to build (HRESULT 0x80070057)`. `E_INVALIDARG`
from `CreateComputePipelineState`, which is what a driver returns for DXIL it
will not accept. The usual cause is an unsigned container, and the warning
`the DXIL dxcompiler.dll produced is unsigned` will be above it.

`the Vulkan loader on this system reports X.Y.Z and this renderer needs 1.3`.
The driver is too old. The renderer uses dynamic rendering,
`synchronization2` and timeline semaphores, and asks for exactly those three.

`the Vulkan loader offers no VK_KHR_win32_surface`. No window system
integration for this platform, so nothing can be presented to.

`this surface offers no B8G8R8A8_UNORM or R8G8B8A8_UNORM in the sRGB
non-linear colour space`. The formats the surface did offer are listed. The
overlay pipeline is built for the backbuffer's format, and rhi::Format carries
only those two eight-bit orders.

`a window was requested but no HWND was given to the D3D12 backend`. SDL
created a window and reported no native handle for it. The window service logs
its own line above this one.

## Building and running

    # once, and after any shader change
    ./tools/gen_gs_shaders.sh

    # Linux, the selftests and the replay tool
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build -j2 --target icorecomp-gs-swizzle-selftest \
                                     icorecomp-gif-decode-selftest \
                                     icorecomp-gs-raster-selftest \
                                     icorecomp-gs-texture-selftest \
                                     icorecomp-gs-gen-dump \
                                     icorecomp-gs-replay
    ./build/icorecomp-gs-swizzle-selftest
    ./build/icorecomp-gif-decode-selftest
    ./build/icorecomp-gs-raster-selftest
    ./build/icorecomp-gs-texture-selftest

    # Windows, cross compiled, the shipping configuration
    cmake --preset windows
    cmake --build --preset windows -j2
