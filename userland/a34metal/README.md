# a34metal: HackiOS's Metal driver on the Mali-G68

A port of **HackiOS's Metal driver for the Mali** (OOSEX GANG, MIT;
[hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel), branch `mtk` at `a962798`,
the Infinix Hot 20's Mali-G52), on which CoreAnimation draws through Metal there. Nothing of it
has run on our phone yet. Built by `.github/workflows/a34metal.yml` (branch `metal`, artifact
**`a34metal`**). The plan and how Metal loads a driver: [`docs/metal-plan.md`](../../docs/metal-plan.md).

| here | theirs (`mtk`) | what |
| --- | --- | --- |
| `redwoodmetal.m` | `userland/src/redwoodmetal.m` | the plug-in: `RedwoodMetalDevice` (a `_MTLDevice` subclass) and every object it hands out |
| `metal/*.cpp`, `metal/*.h` | `userland/src/metal/` | the driver's core in C++: queues, command buffers, encoders, textures, IOSurfaces, pipelines, CoreAnimation's pipeline library by label, the worker thread |
| `metal_drv_test.m` | `userland/src/metal_drv_test.m` | the driver's test: device, compute, draws, textures, through Metal's own objects |
| `metal_props.c` | `userland/src/metal_props.c` | puts `MetalPluginName`/`ClassName` on `IOAcceleratorES` at run time (needs `IOAcceleratorES::setProperties`, which our kernel has not; kernel 0920's boot-arg does the same) |
| `../mesa/air/` | `tools/mesa/air/` | the AIR frontend (Metal's shader bitcode into NIR) and `air_metal.c`, the C library the core calls (`air_metal.h`) |
| `../mesa/patches/0006`, `0007` | `air-frontend.patch`, `panfrost-osmesa-air.patch`, `panfrost-darwin.patch`, `panfrost-redwood-perf.patch` | the library built into libgallium; panfrost's counters, pool slack, no CRC, 16-bit UBO loads widened |
| `metal-probe.m`, `a34_watch.c` | ours | Metal's discovery as a program sees it; a console witness linked into the tests |

## What changed in the port

- **Mesa 26.3 instead of 25.0.7.** NIR: `nir_def_instr()`, `mesa_shader_stage`, the builder's
  `fp_math_ctrl` for `exact`, `nir_block_create(impl)`, `nir_shader_create` without `si`.
  Gallium: no `create_surface` (a framebuffer's surfaces are templates, kept with each texture),
  `clear` with masks, `nir_options[]`/`compute_caps` for the old queries, `driParseConfigFiles`
  parameters. The frontend and `air_metal.c` compile against 26.3 on a Linux host (softpipe);
  the panfrost branch only in CI.
- **libgallium instead of libOSMesa** (gone in Mesa 26): `air_metal.c` is built into
  `/a34/libgallium-26.3.0.dylib`, which the plug-in `dlopen`s (`metal/air.cpp`).
- **Valhall v9 instead of Bifrost v7.** Their Bifrost-only build switches are not taken. Their
  compiler passes run in `bifrost_nir.c`, which serves both; of them only the 16-bit UBO widening
  is taken, and only for the Metal driver's shaders. Not taken yet (performance): every UBO pushed
  (`pushubo`) and scratch arrays into registers (`scratchregs`).
- **Our kernel.** Their `RedwoodMali` speaks the same panfrost uAPI as ours; where theirs differs
  (a queued SUBMIT's number in `jc`, waits by `0x7d`, a surface's pending mark `0x7c`), our
  libdrm stand-in answers `air_metal.c` over our 0919 queue (`redwood_drm_last_seq`,
  `redwood_drm_wait_seq`; `redwood_drm_surface_seq` is a no-op). Their synchronous mode (the
  default; `rwm_async=1` is the other) needs nothing more of the kernel.
- **Paths.** `/a34/libgallium-26.3.0.dylib` for their `/redwood/lib/libredwoodair.dylib`; the
  bundle at `/System/Library/Extensions/A34Metal.bundle` (kernel 0920's name) with their class as
  its principal class; QuartzCore's own `default.metallib` (our root has it); `MTLAddDevice` by
  name in `metal_drv_test` (their addresses are 16.5's).

## On the phone (first run)

The tests register the device themselves (`MTLAddDevice`), in their own processes only, so the
kernel needs no `a34metal`. With `a34metal=1` every program that asks Metal loads the plug-in;
it then refuses backboardd unless boot-arg `camtl=1` (CoreAnimation's Metal renderer needs
HackiOS's `camtl` hooks, not ported yet), SpringBoard unless `sbmtl=1`, and chronod/widgets. With the root at `/mnt` and the artifact in
`a34metal/`:

```
tools/root-add-job.py --copy /mnt /System/Library/Extensions/A34Metal.bundle a34metal/A34Metal.bundle
tools/root-add-job.py /mnt com.a34.metalprobe /a34 a34metal/a34 metal-probe --direct --next --compute --draw
tools/root-add-job.py --env /mnt com.a34.metalprobe REDWOOD_DRM_ASYNC=0
tools/root-add-job.py /mnt com.a34.metaldrvtest /a34 a34metal/a34 metal_drv_test --bundle
tools/root-add-job.py --env /mnt com.a34.metaldrvtest REDWOOD_DRM_ASYNC=0
```

The second line also copies `a34/` into `/a34`: Mesa's four dylibs (the same Mesa as a34gl's, plus
patches 0006/0007, which also turn panfrost's CRC off for GL), `mali-tri`, `metal-probe`,
`metal_drv_test`, `metal_props`, `a34metal-tests.metallib` (our own shaders, `a34metal-tests.metal`,
compiled in CI by Xcode's `metal` for iOS 16). Boot-args as today (`a34mali=1`, no `a34metal`), plus
`sweetnull=backboardd,SpringBoard,metal-probe,metal_drv_test`: the kernel then copies what the
tests write to `/dev/null` into its log as `|metal-probe| ...`, a second copy of every line that
no console state can lose (boot 54 lost everything the tests wrote after their first lines).
`REDWOOD_DRM_ASYNC=0` keeps the tests' GPU jobs synchronous, as HackiOS's known-good runs were.

What a good run puts in the kernel log (stdout of both jobs goes to the console):

- `metal-probe`: `registry: ... has no MetalPluginName`, `MTLCreateSystemDefaultDevice() = 0x0`,
  `direct: ... loaded, principal class RedwoodMetalDevice`, `redwoodmetal[metal-probe N]: the
  compiler library Mali-G68 ...`, `direct: registered`, the device's name and families, `PASS:
  Metal's default device is RedwoodMetalDevice "..."`, then `next: newCommandQueue = 0x...` and
  `next: newBufferWithLength:4096 = 0x...`; then `--compute`: `PASS: compute: 1024 words written as
  3i + 7 by the GPU, 3072 canaries untouched`, and `--draw`: `PASS: draw: a clear through Metal into
  an IOSurface, every pixel` and `PASS: draw: a translated vertex and fragment shader drew the
  triangle into the IOSurface` (nine pixels listed). Exit 0; 1 no device, 2 another class, 3 not as
  Metal wants it, 4 a compute or draw check failed. The device calls itself `Mali-G68 MC4
  (Panfrost)` and claims families Apple1-3, Common1-2 (feature profile 4; `metal_profile=N`).
- `metal_drv_test --bundle`: `drvtest: step 4 isSubclassOfClass:_MTLDevice PASSES`, `step 5 gave
  RedwoodMetalDevice`, `MTLAddDevice returned`, `MTLCreateSystemDefaultDevice() =
  RedwoodMetalDevice`, `it calls itself "..."`, what the device still owes, then the eleven
  kernels of `/System/Library/VideoDecoders/AV1SW.metallib` compiled for the Mali (`drvtest: of 11
  functions, N compiled and M were refused`) and the first dispatched. It binds RGBA32Float to
  every texture unless told otherwise, which AV1SW's FilmGrain kernels refuse (they read
  `texture2d<ushort>`): `--texslots 0=2d:rgba16uint,1=2d:rgba16uint` (per slot) or `--texfmt` set
  it; `metal-probe --compute` is the checked end-to-end compute test. Other modes (`--draw`,
  `--draw-all`, `--time`): the top of `metal_drv_test.m`.
- Both (`a34_watch.c`): every line of stdout and stderr -- theirs, the plug-in's, redwood-drm's --
  prefixed `+SECONDS`, written through `/dev/console` opened anew for that line and to `/dev/null`;
  `a34watch: PROGRAM[PID] T s: alive; fd 1 ..., N lines out, M lost` and `main thread: c:0x18...
  < ...` every 5 s (`c:` addresses are unslid dyld-cache addresses, others image+offset); `exit()`
  or `signal N` at the end. Boot 31 and 54: silent for 30-40 s inside or right after
  `MTLCreateSystemDefaultDevice()`; the backtraces say where.

## CoreAnimation on Metal (camtl=1): the first boot

What it takes, ported from HackiOS (`camtl.inc`, from their `inject.c`): CoreAnimation specializes
QuartzCore's uber shaders with function constants, which Metal would send to Apple's compiler
service; the plug-in, in backboardd with `camtl=1`, answers the plain function and keeps the
values on it for the compiler library (`rwa_render_constants`). Nothing else of their `camtl`
is needed here: our root has QuartzCore's own `default.metallib`, and the plug-in itself keeps
backboardd off Metal without `camtl=1`. On the host (Mesa 26.3, AIR frontend to validated NIR):
QuartzCore's `default.metallib` 137 of 141 functions; the 4 others are `tile_downsample_*`
(imageblocks), which CoreAnimation asks for only of a device with tile shaders (Apple4 and up;
this one claims Apple3). RenderBox's 19 of 47 (struct-valued instructions: later). Whether
panfrost compiles them for the G68 is the phone's to say, first with
`metal_drv_test --bundle --draw-all /System/Library/Frameworks/QuartzCore.framework/default.metallib`.

The boot: the artifact installed as above (`A34Metal.bundle`, `a34/`), boot-args as today plus
`a34metal=1 camtl=1 rwm_finish=1`, and `REDWOOD_DRM_ASYNC=0` in `/a34/backboardd.env`
(HackiOS's known-good was synchronous). a34gl stays installed and injected: if CoreAnimation
cannot make its Metal context it falls back to OpenGL ES; if a pipeline fails, backboardd ends
(HackiOS) and the way back is a boot without `camtl=1`. The plug-in's lines are in the kernel log
as `|backboardd| redwoodmetal[backboardd N]: ...` (the devnull tee); `camtl=1: ... hooks` says
the specialization is in place, every pipeline it cannot build is named. With `a34metal=1` the
device is served to backboardd (`camtl=1`), the tests, programs named in
`/a34/A34Metal.clients`, and everyone only with `a34metalall=1`; never to SpringBoard (`sbmtl=1`)
or chronod/widgets.

## Profiling CoreAnimation on Metal (boot-arg camprof=N)

Every N seconds of backboardd's drawing, `|backboardd| redwoodmetal camprof:` lines (HackiOS's
profile, now in our log): command buffers a second, draws, the processor's time per buffer, the
GPU wait per buffer, the worst buffer, pipelines built and their time; surfaces copied in (sampled
surfaces the GPU cannot read in place) and copied back (drawn elsewhere and copied by the
processor), count, KB and ms; the waits (queue, processed, completed); batches submitted and their
time; shader variants compiled at draw time; the CPU time of a draw by stage. Lines HackiOS kept
with their kernel's sysctl come as `redwoodmetal kept: ...` (slow pipelines with their ms, pipelines
it could not build, surfaces copied back). Boot 63's first sign: SpringBoard's 1125x2436 surface 31
(rows of 4512 bytes, not a multiple of 64) is drawn by Metal into a texture of Mesa's and copied
back by the processor, 11 MB, after each command buffer that draws into it.

Pipelines ahead of time: `metal-probe --post-recipes 180` (a job) has every process with the driver
write its pipelines' recipes into the log three minutes into the boot (`rwr ...` lines);
`tools/rwm-recipes.py LOG` makes `build/rwm-recipes.txt` of them; as `/a34/rwm-recipes.txt` the
driver builds those pipelines on a thread of its own at its next start (`rwm_warm=0`: not).

## CoreAnimation's effects on Metal (metal-fx)

Boots 63 and 65 (`camtl=1`) drew blurs black or not at all. QuartzCore's Metal renderer asks the
device for three things this driver did not do. QuartzCore's own selectors list `setLevel:`,
`setMipmapLevelCount:`, `generateMipmapsForTexture:` and `newTextureViewWithPixelFormat:`
(research: `qc_metal_sels.txt`).

- **Drawing into a level or slice.** A blur's smaller copies are drawn into the levels of one
  texture (`MTLRenderPassAttachmentDescriptor.level`; the variable blur reads them back by level).
  Every attachment was drawn at level 0, slice 0, so the copies were written over the picture
  they were made from, and the levels the blur reads stayed empty. Now each colour attachment is
  drawn at its own level and slice (`rwa_draw.color_level`/`color_slice`, render surfaces kept per
  level). A slice of a 2D array or a face of a cube is a 2D target. Boot-arg `rwm_levels=0`:
  level 0 as before. The first four such passes are logged ("a pass draws into level N").
- **Each attachment's own load action.** Only attachment 0's clear was seen, and it cleared every
  attachment to its colour. CoreAnimation draws into two. Now each attachment is cleared or kept
  by its own load action, to its own colour, in a pass that draws and in one that only clears
  (`rwa_render_clear_attachments`). Boot-arg `rwm_clears=0`: as before.
- **Texture views.** `-newTextureViewWithPixelFormat:` (and the `textureType:levels:slices:`
  forms) answered nil. They are now another texture over the same memory, the same size a texel
  in another format, from its own first level and slice (`rwa_texture_view`). It is sampled,
  drawn into, copied and mipmapped as such, and a view of a texture over a surface lies over it
  too. Boot-arg `rwm_views=0`: nil as before. The first eight views are logged ("a texture
  view: pixel format N as M").

Files: `userland/mesa/air/air_metal.[ch]` (the library), `metal/render.cpp`, `draw.cpp`,
`blit.cpp`, `texture.cpp`, `air.cpp`, `rwm.h`, `redwoodmetal.m` (attachments and views only).

## Credits and licence

HackiOS (OOSEX GANG) wrote everything here but `metal-probe.m` and `a34_watch.c`, and the
changes listed above; the AIR sources carry their own header (Copyright 2026 Darwin on redwood
contributors, MIT). HackiOS's userland is under the MIT License:

```
MIT License

Copyright (c) 2026 OOSEX GANG
Copyright (c) 2025-2026 the HakiOS authors
Copyright (c) 2026 Darwin on redwood contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

Mesa (MIT) is fetched at build time; the patches are under Mesa's licence.
