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

Do **not** boot with `a34metal=1` yet: with the plug-in named on `IOAcceleratorES`, every program
that asks Metal gets this device, backboardd included, and CoreAnimation then needs HackiOS's
`camtl` hooks (their `inject.c`), which are not ported. The test registers the device itself. The second line copies `a34/` (Mesa's dylibs with the
Metal library in libgallium, the tests) into `/a34`, replacing a34gl's Mesa there with the same
Mesa plus patches 0006/0007 (which also turn panfrost's CRC off for GL):

```
tools/root-add-job.py --copy /mnt /System/Library/Extensions/A34Metal.bundle a34metal/A34Metal.bundle
tools/root-add-job.py /mnt com.a34.metaldrvtest /a34 a34metal/a34 metal_drv_test --bundle
tools/root-add-job.py --env /mnt com.a34.metaldrvtest REDWOOD_DRM_ASYNC=0
```

Boot-args as today with `a34mali=1` (no `a34metal`). `metal_drv_test --bundle` asks Metal for a
device, loads the bundle, makes `RedwoodMetalDevice`, registers it with `MTLAddDevice`, lists
what the device still owes, then compiles and runs the functions of
`/System/Library/VideoDecoders/AV1SW.metallib` (other modes: `--draw`, `--draw-all`, `--time`, see
the top of the file). `a34watch:` lines give the main thread's backtrace every 5 s and the exit.
`REDWOOD_DRM_ASYNC=0` keeps every GPU job synchronous, as HackiOS's known-good runs were.

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
