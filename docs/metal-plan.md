# Metal on the Mali-G68: scope and plan

Written 13 November 2026 from the files alone: the dyld cache of our iOS 16.7.16 root (UUID
`8f970452-c9e3-3e88-b114-feb1864bc9e0`, Metal 306.6.6, QuartzCore 1045.17.2), HackiOS's
sources, and Mesa 26.3.0. **Nothing in this document has run on the phone.** Addresses are in
that cache; "HackiOS main" is `oosexgang/hakios-xnu-kernel@63fc2b4`, "HackiOS mtk" its branch
`mtk@a962798`. How to repeat the readings: [section 9](#9-how-the-readings-were-made).

## Change of approach (14 November 2026): port HackiOS's driver

The milestones below were planned from first principles. Since then the work ports HackiOS's
Metal driver for the Mali wholesale (`userland/a34metal`, `userland/mesa/air`, Mesa patches 0006
and 0007; what changed in the port is in `userland/a34metal/README.md`). The first phone test is
their `metal_drv_test --bundle` with `REDWOOD_DRM_ASYNC=0`, on today's kernel without
`a34metal=1` (with it every program would get the device, backboardd included, and CoreAnimation
on Metal needs their `camtl` hooks in `inject.c`, not ported yet). What the kernel would add for
their asynchronous mode and run-time discovery switch (0x7c, `setProperties`) is a proposed
patch kept outside the repository.

**Boot 31 (metal-probe, kernel 0918):** silent for 30 s inside `MTLCreateSystemDefaultDevice()`,
then the GPU opened and closed and nothing more was printed. Metal 306.6.6 has no timeout on that
path (no semaphore, group wait, sleep or timer is reachable from `MTLDeviceArrayInitialize`); what
it does there that leaves the process is preferences: `[NSUserDefaults standardUserDefaults]`,
`_CFXPreferencesRegisterDefaultValues`, `CFPreferencesGetAppBooleanValue("UseMetalTelemetry",
"com.apple.Metal")` (`initWrapperType`), all of which talk to cfprefsd, and no `exec cfprefsd`
appears anywhere in that boot's log. A guess, not a finding: the tests now carry `a34_watch.c`,
which prints the main thread's backtrace every 5 s on its own console descriptor and says how the
program ends, so the next run shows where it waits and whether it lives.

## Summary

- **Loading is small and fully known.** Metal looks for `IOAcceleratorES` services, reads two
  registry properties, loads `/System/Library/Extensions/<name>.bundle`, and wants a subclass of
  its private `_MTLDevice`. A plug-in that subclasses `_MTLDevice` directly is asked for two
  numbers before it is registered, and nothing opens a user client on `IOAcceleratorES`.
  Our kernel needs two properties behind a boot-arg ([section 8](#8-the-kernel-change-for-m1)).
- **A Metal driver for a Mali on Mesa's panfrost is published**, contrary to what we assumed:
  HackiOS mtk has it under MIT since 6 November (`userland/src/redwoodmetal.m`,
  `userland/src/metal/`, `tools/mesa/air/`), for a Mali-G52 (Bifrost v7) on Mesa 25.0.7 with
  OSMesa. It is the long-run architecture below, already written once.
- **Recommended:** the plug-in in Objective-C over a C core built **inside our Mesa** on
  gallium (`pipe_screen`/`pipe_context`), shaders **AIR to NIR directly** with HackiOS's
  front end. Rejected as the main road: Metal over our OpenGL ES context (no AIR-to-GLSL
  translator exists, and GL's thread and state model is in the way), and Metal on Vulkan
  (PanVK is not built for this GPU's architecture v9).
- **Our root has QuartzCore's `default.metallib`** (141 functions), which HackiOS's 16.5 root
  lacked and had to assemble on a Mac. One library is all CoreAnimation loads.
- **Started:** `userland/a34metal` (milestone 1: the plug-in and `metal-probe`), built by
  `.github/workflows/a34metal.yml` for arm64 iOS 16; not yet run on the phone.

## 1. How Metal finds and loads a driver

All in `/System/Library/Frameworks/Metal.framework/Metal` (306.6.6):

| step | where | what it does |
| --- | --- | --- |
| 1 | `_MTLCreateSystemDefaultDevice` `0x181a37760` | `dispatch_once` on `MTLDeviceArrayInitialize`, then returns the one object of `_deviceArray` (more than one: "doesn't know what to do with more than one device") |
| 2 | `MTLDeviceArrayInitialize` block `0x181a34050` | makes the array and the queue `com.Metal.DeviceArray`, reads the debug/capture/HUD switches, calls `_MTLRegisterDevices` |
| 3 | `_MTLRegisterDevices` `0x181b144c8` | `IOServiceGetMatchingServices(IOServiceMatching("IOAcceleratorES"))`; for each service: `getMetalPluginClassForService`, `[[cls alloc] initWithAcceleratorPort:service]`, and `_MTLAddDevice` if that is not nil |
| 4 | `_getMetalPluginClassForService` `0x181a3c7c8` | property `MetalPluginName` (must be a string) -> `[NSBundle bundleWithPath:@"/System/Library/Extensions/<name>.bundle"]`; property `MetalPluginClassName` -> `[bundle classNamed:]`, else `[bundle principalClass]`; nil unless `[cls isSubclassOfClass:[_MTLDevice class]]` |
| 5 | `_MTLAddDevice` `0x181a1b368` | fatal assertions `[newDevice conformsToProtocol:@protocol(MTLDevice)]` and `...(MTLDeviceSPI)`; `-initLimits`; `-initFeatureQueries`; adds the device to the array (wrapped if a debug layer is on) |

What the base class does for a subclass, and asks of it:

- `-[_MTLDevice initWithAcceleratorPort:]` (`0x181aab968`) is abstract: `doesNotRecognizeSelector:`
  and nil. The subclass implements it and sends `[super init]` (`0x181a3634c`: three dispatch
  queues, the library builder, serialization paths; no IOKit call).
- `-[_MTLDevice initLimits]` (`0x181a1b4bc`) sends `-featureProfile`, `-argumentBuffersSupport`
  (its own) and `-maxBufferLength`. The profile selects a column of limits: 0, 1 and 4...9
  have one; anything else fails the assertion `_limits.maxColorAttachments > 0`.
  `-initFeatureQueries` (`0x181a1bd38`) calls `-initGPUFamilySupport` (`0x181a1ea28`), which
  again reads only `-featureProfile`.
- `_MTLDevice` implements about 400 of the 463 selectors of `MTLDevice` + `MTLDeviceSPI`
  (libraries, functions, descriptors, the family and feature answers). **About 60 required ones it
  does not**: all the resource makers (`newBufferWith...`, `newTextureWithDescriptor:`,
  `newTextureWithDescriptor:iosurface:plane:`, `newSamplerStateWithDescriptor:`,
  `newDepthStencilStateWithDescriptor:`, `newHeapWithDescriptor:`, `newFence`), and the
  device's facts (`featureProfile`, `maxBufferLength`, `registryID`, `hasUnifiedMemory`,
  `isHeadless`/`isLowPower`/`isRemovable`, the memory sizes, `supportsSampleCount:`,
  `supportsVertexAmplificationCount:`, the FP configurations). `-newCommandQueue`
  (`0x181aaf3d0`) forwards to `-newCommandQueueWithDescriptor:`; `-targetDeviceInfo`
  (`0x181aabb14`) answers NULL, which HackiOS found Metal's pipeline-library code reading
  without a check, so the plug-in answers it.
- The classes `_MTLDevice`, `_MTLCommandQueue`, `_MTLCommandBuffer`, `_MTLCommandEncoder`,
  `_MTLResource`, `_MTLRenderPipelineState`, `_MTLComputePipelineState`,
  `_MTLDepthStencilState`, `_MTLSamplerState`, `_MTLLibrary`, `_MTLFunction` are exported
  from Metal (`_OBJC_CLASS_$__MTLDevice` is an external symbol), so a plug-in subclasses them
  in source.

**What Apple's own drivers add, which we do not need.** On this iPhone the driver is
`AGXA11Device : AGXA11FamilyDevice : IOGPUMetalDevice : _MTLDevice` (IOGPU.framework 65.60.1);
Metal also still carries the older `MTLIOAccelDevice : _MTLDevice`. Those two base classes
are what talk to the kernel: `-[MTLIOAccelDevice initWithAcceleratorPort:]` (`0x181abd76c`)
calls `IOAccelDeviceCreateWithAPIProperty`, `IOAccelDeviceGetConfig64`,
`IOAccelDeviceGetSharedMemorySize`, i.e. `IOServiceOpen` on the accelerator and Apple's
IOAccelerator/IOGPU user-client protocol (shared memory, command-buffer storage, resource
tables). A plug-in below `_MTLDevice` makes **no** call on `IOAcceleratorES` at all; Metal
itself only reads the two properties. HackiOS measured the other road shut on theirs
(`IOServiceOpen` type 1 on their `IOAcceleratorES`: `kIOReturnUnsupported`; mtk
`userland/src/redwoodmetal.m:26-30`).

**How HackiOS satisfied it** (same Metal, 306.6.4 on 16.5): boot-arg `metal=1` adds the two
properties to their `IOAcceleratorES` (mtk `xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp:1936-1953`);
the bundle is linked into `/System/Library/Extensions`; the class subclasses `_MTLDevice`,
adds `MTLDeviceSPI` with `class_addProtocol` at `+load`, and answers `-featureProfile`,
`-maxBufferLength`, `-supportsVertexAmplificationCount:` (mtk `redwoodmetal.m:9-25`, `:1005`).
Their Adreno driver's `fake_gpu.cpp` is unrelated to loading: it is a PM4 interpreter for
host tests.

**Every process, not only backboardd.** This discovery runs in each process that calls
`MTLCreateSystemDefaultDevice`: once the service names the plug-in, SpringBoard, CoreImage's
clients and apps load the bundle themselves, with no `DYLD_INSERT_LIBRARIES` (which is how
`liba34gl` reaches backboardd and nothing else). That is the way to a GPU in apps, and it
means three things have to hold before the device is shown to everyone:
`RedwoodMali` must open for any program (our "Sandbox" policy is a stand-in,
`bsd/kern/sweet_amfi.c:400`, so `iokit-open` is not filtered; to verify from an app); the
kernel service must stand many clients (it has one GPU address space for all and runs jobs
one at a time, `iokit/Kernel/arm/A34Mali.cpp:26-40` of the patched tree, and each Mesa screen
takes a tiler heap of which `a34maliheap` = 64 MB gets memory); and a device that cannot do
what a program asks ends that program, so the plug-in decides per program
(`A34Metal.clients`, boot-arg `a34metal=1|2|3`).
HackiOS found two that must be refused even with a working device: SpringBoard (Siri's
set-up makes a Metal Performance Shaders kernel, and MPS has no device for a foreign GPU
below family Apple2) and chronod/widgets (RenderBox wants a drawable) (mtk
`redwoodmetal.m:869-900`). On the CoreImage crash seen in SpringBoard with an `EAGLContext`
that has no driver: given a Metal device, CoreImage is expected to take its Metal renderer
instead (not verified here); UniXNU and HackiOS found it reading `sharedMemorySize` and
`dedicatedMemorySize` first and throwing without them, so the plug-in answers both.

## 2. Prior art

### hakios-metal (HackiOS main, `hakios-metal/`, MIT): Metal on an Adreno without Mesa's driver

| | |
| --- | --- |
| What | a C++ core that writes the Adreno's own command packets, a C interface (`include/hakios-metal/hakios_metal.h`), and the Objective-C plug-in over it (`plugin/HakiosMetal.m`, 1.6k lines; the window server's is `userland/src/HakiosMetalCA.m`, 8.7k) |
| Surface | device; buffers; 2D textures of one level (also as colour target); samplers; libraries/functions (own `.metallib` reader); compute pipelines with buffers, textures, samplers; command queue and buffer with Metal's states and handlers; blit copy/fill; one render pass into one colour target (`README.md`'s table). The window-server plug-in adds what CoreAnimation uses |
| Adreno-specific | `pm4.hpp`, `a6xx.hpp` (4.5k lines of registers), `render.cpp`, `compute.cpp`, `texture.cpp` (fdl6 layouts), `link.cpp` (ir3 linking), `gpu.cpp` (msm DRM requests), `fake_gpu.cpp` (packet interpreter for tests): most of the core |
| Reusable | the plug-in's object model over a C contract; the command-buffer state machine (`command.cpp`); the `.metallib` reader; the test discipline (every refusal a sentence, a fake GPU); what `HakiosMetalCA.m` knows about CoreAnimation (pipeline labels, the pipeline library) |
| Shaders | AIR -> NIR by their front end -> ir3 by Mesa, in process (`src/compiler.cpp`, built with `MESA=1`) |
| Loads | as in section 1; boot-arg `metal=1`, window server only unless `metal_all=1` |
| Presents | no swap chain: CoreAnimation hands IOSurfaces (`newTextureWithDescriptor:iosurface:plane:`), kernel request `0x7e` turns one into a GPU buffer (as ours does), `0x7d` wires a client's own pages |
| Measured | page swipes 100-104 fps on a 120 Hz Adreno 642L; UniXNU's Metal on Vulkan on the same phone 50-57 (`docs/reference/metal.md`) |

### The Mali driver (HackiOS mtk, MIT): what we port

| part | files | lines | what |
| --- | --- | --- | --- |
| AIR front end | `tools/mesa/air/air_to_nir.c`, `air_module.c`, `air_bitcode.c`, `air_intrinsics.c`, `nir_air.h` | 9.4k | `air_to_nir()` beside `spirv_to_nir()`, with its own bitcode reader (no LLVM in the process) |
| Metal on gallium | `tools/mesa/air/air_metal.c`, `air_metal.h` | 7.7k + 1.4k | the contract "AIR in, a pipeline out", about 40 `rwa_*` calls: device, buffers, textures (also from an IOSurface), samplers, depth/stencil, render and compute pipelines from bitcode with function constants, draws, blits, fences. Opens `panfrost_drm_screen_create` itself (`air_metal.c:598`), softpipe for tests (`:614`) |
| Plug-in | `userland/src/redwoodmetal.m`, `userland/src/metal/*.cpp` | 5.9k + 5.8k | device, buffer, texture, sampler, depth/stencil, pipelines, queue, command buffer, render/compute/blit encoders, argument encoder; a worker thread; CoreAnimation's pipeline library by label (`metal/plib.cpp`) |
| Mesa glue | `tools/mesa/air-frontend.patch`, `panfrost-osmesa-air.patch`, `panfrost-darwin.patch`, `panfrost-redwood-perf.patch` | small | builds the two into `libOSMesa` (Mesa 25.0.7) |

State by their own account (`docs/metal-x6826b-plan.md`, a note of 27 October; the branch is
a snapshot of 6 November): CoreAnimation draws through it on the G52, fast (92-96% of frames
within 18 ms) but **not right** when asynchronous (doubled icons, strips); clean only with
the driver fully synchronous. Listed there as empty: MSAA, cull/winding/fill modes, depth
bias, visibility results, events.

## 3. Shaders

A `.metallib` is a container (`MTLB`) of one LLVM bitcode module per function, with Apple's
metadata (`!air.vertex`, `!air.fragment`, `air.*` intrinsics); ours say "Apple metal version
31001.722", `air64-apple-ios16.7.0`, AIR 2.5. Metal gives a plug-in each function's module
through `-[_MTLFunction bitcodeData]`, so the plug-in parses no container for libraries
loaded through the device.

| road | pieces | verdict |
| --- | --- | --- |
| **AIR -> NIR** | HackiOS mtk `tools/mesa/air` (MIT). On their 16.5 corpus: fragment 270/271, vertex 3/3, kernel 40/47 reach the backend; the 135 "visible" (CoreImage stitchable) modules do not (`docs/air-report.md`). Handles function constants (`air_metal.h:1112`), framebuffer fetch (`air_to_nir.c:2024`), argument buffers | **chosen**: the only one that has drawn CoreAnimation on a Mali, no LLVM at run time, ends where panfrost's compiler starts |
| AIR -> SPIR-V -> `spirv_to_nir` | Iridium in [darlinghq/indium](https://github.com/darlinghq/indium) (0BSD; through a `dlopen`ed libLLVM); `metal2vulkan-air` in [steelbrain/metal2vulkan](https://github.com/steelbrain/metal2vulkan) (Rust, own reader, `GPL-2.0-or-later OR LGPL-3.0-or-later`) | Vulkan-flavoured SPIR-V (descriptor sets, physical storage buffers) that still needs a binding model on gallium; libLLVM is 50-100 MB in every process, or a GPL/LGPL Rust runtime. Keep as a **host-side second opinion** for translations |
| LLVM IR -> SPIR-V by [SPIRV-LLVM-Translator](https://github.com/KhronosGroup/SPIRV-LLVM-Translator) | | no: OpenCL-flavour only |
| AIR -> GLSL ES | | nothing exists |
| Metal on Vulkan (metal2vulkan + PanVK) | [UniXNU/AdrenoMetal](https://github.com/UniXNU/AdrenoMetal) does it on Turnip | no: PanVK builds for architectures 6, 7, 10-14 (`src/panfrost/vulkan/meson.build:145`), and Mesa's table has no Vulkan for the G57/G68 (`docs/drivers/panfrost.rst`) |

Useful for reading AIR, not for running it: [YuAo/MetalLibraryArchive](https://github.com/YuAo/MetalLibraryArchive)
(container format), [zhuowei/MetalShaderTools](https://github.com/zhuowei/MetalShaderTools),
[3Shain/dxmt](https://github.com/3Shain/dxmt) `src/airconv` (the conventions, written by a
producer of AIR). Apple documents none of it. Stock `llvm-dis` reads the modules (HackiOS:
all 456 of 16.5 with LLVM 22.1.8).

**On the device or on the host?** On the device, because it has to be: CoreAnimation
specializes its uber shaders with function constants at run time ("Specializing shader
function %s for %s with constant values %s" in `CA::OGL::MetalContext`), and apps bring their
own libraries. The host's part is the test: every function of the libraries below translated
by a local script (Apple's files stay off CI), and shaders of our own drawn on softpipe in CI
and compared. A cache of compiled shaders on disk is a speed matter for M4 (HackiOS saw
13-19 s for one specialized uber shader, `redwoodmetal.m:1051`).

**The libraries.** The root has 143 `.metallib` and 15 `.pipelinelib` (77 under
PrivateFrameworks, 44 under Frameworks). What draws the interface:

| library | functions | who |
| --- | --- | --- |
| `QuartzCore.framework/default.metallib` | 141: 39 vertex, 95 fragment, 7 kernel | the window server's renderer. The only library `CA::OGL::MetalContext` loads (`bundleWithIdentifier:com.apple.QuartzCore`, `URLForResource:@"default" withExtension:@"metallib"`, `newLibraryWithURL:error:`; it asserts `metallib_url != nil`) |
| `QuartzCore.framework/default.pipelinelib` | `functions`: 451 specialized libraries; `pipelines.desc`: 1079 `com.apple.coreanimation.draw.` labels; binaries `pipelines.g10p`... for Apple's GPUs | precompiled pipelines; CoreAnimation logs "Precompiled pipelines disabled on unsupported hardware" when it cannot use them. HackiOS draws from its `functions` by label (`metal/plib.cpp`) |
| `RenderBox.framework/default.metallib` | 47: 18 vertex, 29 fragment | SwiftUI and widgets (chronod), in their own processes |
| `CoreImage.framework/default.metallib`, `ci_stdlib*.metallib`, `ci_filters*.metallib` | 58 kernels + stitchable | CoreImage in any process (SpringBoard's blurs and snapshots among them) |
| `CoreUI.framework`, `MediaCoreUI.framework`, `VideoToolbox.framework` (9), `SiriUICore.framework`, `Metal.framework`'s own four | | their owners, when used |
| `AppleMetalGLRenderer.bundle/default.metallib` | 274 | OpenGL ES on Metal; not on our path |

Which of these SpringBoard and its daemons really open during a boot is not known from the
files; the plug-in logging every `newLibraryWith...` answers it at M4.

**Smallest path to CoreAnimation on Metal:** port the AIR front end and translate the 141
functions (and the 451 specialized ones as the regression corpus). **To apps in general:**
the same front end grown by corpus: RenderBox and CoreImage first, then what apps ship; the
SPIR-V translators above as the reference when a shader disagrees.

## 4. Backend layering

| | (a) on gallium, inside Mesa | (b) over our OpenGL ES 3.1 context | (c) Metal on Vulkan |
| --- | --- | --- | --- |
| Shaders | NIR goes straight into `pipe_shader_state`; the AIR front end is a Mesa front end | GL takes GLSL source only (no `ARB_gl_spirv` on ES): needs an AIR-to-GLSL translator nobody has written, then compiles twice | SPIR-V; no Vulkan driver for v9 |
| Metal's model | maps one to one: explicit load/store actions, texture views, buffer offsets at bind, `[0,1]` depth and Metal's winding as rasterizer state, base vertex/instance, fences | emulated through GL state: default-framebuffer conventions, no texture views on ES 3.1, format gaps (BGRA, 10-bit, sRGB views), state leaking between Metal's free-threaded encoders | |
| Threads | a `pipe_context` per command queue, ours to lock | one more EGL context in backboardd beside liba34gl's, made current on whatever thread Metal's caller is on: the problem patch 0004 exists for, again | |
| Tests without the phone | the same core runs on softpipe on the CI runner and pixels are compared (HackiOS does this, `air_metal.c:614`) | needs a GL on the runner | |
| Cost | code built in Mesa's tree against internal headers that change with every release (we pin 26.3.0); HackiOS's is for 25.0.7 and freedreno/Bifrost first | no Mesa change for a clear or a blit | PanVK for JM Valhall would have to be written |

**Milestone 1 needs no backend**: the device registers without touching the GPU.
**From milestone 2 on, and for the long run: (a).** The decisive reasons are the shader
road (NIR needs gallium) and tests on softpipe in CI, since the phone is scarce. (b) is
rejected as a base: it ends at the first shader. It stays available for one purpose: if the
Mesa-internal port slips, M2's clear can be shown through EGL + `EGL_IOSURFACE_A34` in a day,
as `a34gl-surftest` already does, to test the Objective-C side; that code would be thrown away.

Limits of the GPU road today, whichever layer: OpenGL ES 3.1-class hardware features through
panfrost on v9 (no tessellation or geometry stages in the driver, so Metal tessellation and
layered rendering are out; compute exists; MSAA exists in panfrost but is untested here);
synchronous job submission in the kernel; one GPU address space for all programs.

## 5. What CoreAnimation needs, and what apps need

**CoreAnimation** (read off the `CA::OGL::Metal...` and `CAMetalLayer` functions of QuartzCore,
which send 253 distinct selectors through `objc_msgSend` stubs, Foundation's included. The
lists below are from those; what it calls through cached method pointers (`methodForSelector:`
is used 15 times in QuartzCore; vertex buffer binding and indexed draws do not show up as
stubs) is missing here and needs the plug-in's log):

- *Start-up*: `CAMetalContextCreate` (`0x18838421c`) waits for an `IOAcceleratorES` service
  and calls `MTLCreateSystemDefaultDevice`. Then a command queue (HackiOS: a failure there
  ends backboardd), `minConstantBufferAlignmentBytes`, the library, the pipeline library
  (`newPipelineLibraryWithFilePath:error:`, for `pipelines.<targetDeviceInfo's name>`), and
  the limits:
  `supportsFamily:` (several), `vendorName`, `maxFragmentTextures`, `maxFragmentSamplers`,
  `maxTextureWidth2D`/`Height2D`, `maxColorAttachments`, `supportsRasterOrderGroups`,
  `supportsYCBCRFormats12`, `supportsNativeHardwareFP16`, `supportsTileShaders`,
  `supportsSIMDGroup`, `iosurfaceReadOnlyTextureAlignmentBytes`, `supportsFeatureSet:`,
  `supportsExtendedXR10Formats`.
- *Resources*: `newTextureWithDescriptor:iosurface:plane:` (HackiOS counts five call sites in
  16.5's QuartzCore: the destination, layer surfaces, drawables), `newTextureWithDescriptor:`, `newTextureViewWithPixelFormat:`,
  `newLinearTextureWithDescriptor:offset:bytesPerRow:bytesPerImage:`,
  `newTextureWithBytesNoCopy:...`, `replaceRegion:...`, `getBytes:...`,
  `newBufferWithLength:options:`, `newBufferWithBytesNoCopy:...`, `newHeapWithDescriptor:`,
  `newSamplerStateWithDescriptor:`, `setPurgeableState:`, resource groups with
  `encodeCacheHint...` (Apple GPU hints; can be empty).
- *Pipelines*: `newFunctionWithDescriptor:error:` with constant values,
  `newRenderPipelineStateWithDescriptor:error:`, `newRenderPipelineStateWithName:options:reflection:error:`
  (from the pipeline library), `newComputePipelineStateWithFunction:error:` (the 7 kernels:
  average picture level and tile downsampling), tile pipelines
  (`newRenderPipelineStateWithTileDescriptor:...`; presumably only where `supportsTileShaders`).
  Blending per attachment, write masks, **framebuffer fetch** (the library is compiled with
  `air.compile.framebuffer_fetch_enable`; panfrost has it).
- *Encoding*: render encoder with viewport, scissor, cull, winding, depth bias, depth/stencil
  state, `setFragmentBuffer:offset:atIndex:`,
  `setFragmentTexture:atTextureIndex:samplerState:atSamplerIndex:` (SPI),
  `drawPrimitives:...instanceCount:`, visibility result mode; blit encoder (texture and buffer
  copies, `generateMipmapsForTexture:`); compute encoder; `commit`,
  `commitAndWaitUntilSubmitted`, `waitUntilScheduled`, `waitUntilCompleted`, completed and
  scheduled handlers, `GPUStartTime`/`GPUEndTime`, shared events.
- *Formats*: at least the IOSurface formats the GL path already imports ('BGRA', 'RGBA',
  'l10r', 'RGhA', 'L008', 'L016'; rows aligned to 64 bytes for panfrost). The full list of
  `MTLPixelFormat`s is the table `CA::OGL::MetalContext::MTLPixelFormat_codes`, not yet read out.
- *Drawables*: `CAMetalLayer` is CoreAnimation's own; a drawable is an IOSurface-backed
  texture from the same method, presented by the window server. No swap-chain call reaches
  the driver.
- Other users inside it: Metal Performance Shaders (`initWithDevice:sigma:`, a Gaussian
  blur) and HDR processing. HackiOS: MPS has no device for a GPU below family Apple2.

**Apps and games** need the breadth CoreAnimation skips: vertex descriptors with every
format, indexed and indirect draws, depth and stencil everywhere, MSAA with resolve, mipmapped
and compressed textures (what the GPU's `TEXTURE_FEATURES` say: ASTC and ETC2 on a Mali),
cube/array/3D textures, argument buffers (tier 1 at least), compute with textures, heaps, events,
`MTKView`/`CAMetalLayer` pacing, shader compilation from source (`newLibraryWithSource:` needs
Apple's compiler service; out of scope), MPS and MetalFX (Apple GPUs only). Expect "the
system's own Metal clients, then simple apps"; games built for A-series features (tile
shaders, imageblocks, tessellation, raster order groups) will not run.

## 6. Architecture

```
app / backboardd
  Metal.framework          _MTLDevice, _MTLLibrary, _MTLFunction ... (Apple's, unmodified)
  A34Metal.bundle          userland/a34metal: A34MetalDevice and the objects it hands out,
                           subclasses of Metal's private bases; records encoders, runs them at commit
        | C contract (HackiOS's air_metal.h, kept free of Mesa types)
  liba34mtl in Mesa 26.3   userland/mesa patch: src/compiler/air (AIR -> NIR) and the Metal core
                           on pipe_screen / pipe_context; panfrost (softpipe on the CI runner)
  libdrm.2.dylib           redwood-drm, as today
kernel: RedwoodMali (panfrost uAPI, 0x7e IOSurface import); IOAcceleratorES names the plug-in
```

The core goes into `libgallium-26.3.0.dylib` (OSMesa is gone from 26.3; a second copy of
gallium beside EGL's would double the memory in backboardd), exported as `rwa_*`/`a34mtl_*`
symbols; the plug-in `dlopen`s it, so a device exists without a GPU.

## 7. Milestones

Sizes are guesses from HackiOS's line counts; "cycles" are boots of the phone.

| | build | test on the phone | kernel | size, risk |
| --- | --- | --- | --- | --- |
| **M1** a device | **written; builds in CI, and on the runner's own Metal (macOS 15) the class loads, states both protocols, makes a device and answers an unimplemented message with nil**: `A34Metal.bundle` (a `_MTLDevice` subclass answering the registration and the device's facts, everything else zero and named once), `metal-probe` | `root-add-job.py --copy /mnt /System/Library/Extensions/A34Metal.bundle a34metal/A34Metal.bundle`; job `metal-probe --direct --next` (registers through `MTLAddDevice` on today's kernel); then with `a34metal=1`, plain `metal-probe`. `PASS` and the families each profile claims in the kernel log | section 8 (two properties, boot-arg); none for `--direct` | 0.6k lines, written. Risk: 306.6.6 asks something 306.6.4 did not (1-2 cycles) |
| **M2** clear | queue, command buffer, render encoder (load/store actions only), texture, `newTextureWithDescriptor:iosurface:plane:`, blit read-back, on Metal's base classes; Mesa patch 0005: the core's first calls (`device_open`, `texture_from_surface`, `render_clear`, `texture_read`, `finish`) | `metal-clear`: an IOSurface as `a34gl-surftest` makes it, cleared through Metal, checked in the surface's memory and through `getBytes:` | none expected (the `0x7e` import and client mapping exist); `RedwoodMali` open from a second process while backboardd draws | ~2.5k lines, mostly ported; 1-2 weeks. Risks: Mesa 25.0 -> 26.3 internal API drift; two screens in one process (EGL's and ours) sharing one `io_connect_t` in redwood-drm |
| **M3** triangle | Mesa patch 0006: `src/compiler/air`; `rwa_render_create`/`draw`; pipeline state, vertex buffers, viewport/scissor; a small `.metallib` of our own shaders as test data (built in CI by Xcode's `metal`) | `metal-tri`: pixels checked, as `mali-tri`. Before that, in CI: our own shaders translated and drawn on softpipe; on the host, by a local script (Apple's files stay off CI): all 141 QuartzCore functions through the front end | none | ~12k ported lines; 2-3 weeks. Risks: NIR from the front end on Valhall's compiler (HackiOS patched panfrost for its Bifrost: `scratchregs`, `widen16`, `pushubo`); 16-bit ops on softpipe |
| **M4** CoreAnimation | everything of section 5: formats, samplers, blending, framebuffer fetch, function constants, texture views, compute kernels, the pipeline library by label, handlers and fences; the worker thread; `A34Metal.clients` = backboardd, liba34gl off (`a34gl=0`) | boot to the lock screen on Metal; `glprof`-like counters in the plug-in; compare screenshots with the GL path | `a34metal=1`; likely asynchronous submit with fences in `RedwoodMali` (today a SUBMIT returns when the job is done) and growing tiler heaps | the bulk: 4-8 weeks. Risks: correctness under asynchrony (HackiOS is not right there yet); pipeline build times; we already have ~30 fps on GL, so M4 must beat a working system |
| **M5** breadth | per section 5's app list, driven by what real programs ask (the "named once" log): depth/stencil, MSAA, compressed formats, argument buffers, CoreImage and RenderBox corpora; device for every process (`a34metal=3`) with the per-program refusals | CoreImage in SpringBoard, a SwiftUI widget, a Metal sample app | per-client GPU address spaces (isolation), memory accounting per program | open-ended |

## 8. The kernel change for M1

In `iokit/Kernel/arm/SweetFramebuffer.cpp`, where patch 0907 publishes the service
(`AppleCLCD::start`), as a new patch (off unless the boot-arg is given: with the properties
every process that asks Metal loads the bundle):

```diff
 	if (accel != NULL && accel->init()) {
 		accel->setName("sgx");
+		/*
+		 * A34: boot-arg a34metal names the Metal plug-in. Metal (MTLRegisterDevices)
+		 * reads MetalPluginName and MetalPluginClassName off every IOAcceleratorES,
+		 * loads /System/Library/Extensions/A34Metal.bundle and makes its class
+		 * (userland/a34metal). Which programs the plug-in shows a device to is its
+		 * own decision (a34metal=1: its tests and /a34/A34Metal.clients; 2: all but
+		 * backboardd; 3: all). Without the boot-arg nothing changes.
+		 */
+		uint32_t metalOn = 0;
+		(void)PE_parse_boot_argn("a34metal", &metalOn, sizeof(metalOn));
+		if (metalOn != 0) {
+			accel->setProperty("MetalPluginName", "A34Metal");
+			accel->setProperty("MetalPluginClassName", "A34MetalDevice");
+		}
 		if (accel->attach(this)) {
 			accel->registerService();
-			IOLog("SWEET FB: empty IOAcceleratorES published (no 30 s wait in CAMetalContextCreate)\n");
+			IOLog("SWEET FB: IOAcceleratorES published%s (no 30 s wait in CAMetalContextCreate)\n",
+			    metalOn != 0 ? ", Metal plug-in A34Metal (a34metal)" : ", no Metal plug-in");
 		}
```

Both properties must be strings (Metal checks `CFStringGetTypeID`). Nothing else: no user
client, no `IOAccelerator` method.

## Unknowns

1. Whether Metal 306.6.6's registration is content with what 306.6.4's was (first `metal-probe` run).
2. Which feature profile to claim. 1 (Apple1+2) is the default; CoreAnimation's choices
   (tile shaders, raster order groups, precompiled pipelines) hang on the families.
3. Whether CoreAnimation, given a device, falls back to OpenGL ES when a pipeline fails, or
   ends (HackiOS: it ends). This decides how M4 is staged.
4. Function specialization: whether `-[_MTLFunction bitcodeData]` of a function made with
   constant values is already specialized here, or the front end must do it (HackiOS does it
   in the front end because Apple's compiler service could not read their library).
5. HackiOS's front end on Valhall v9 (theirs ran on Bifrost v7 and ir3) and on Mesa 26.3.
6. Two gallium screens (EGL's and Metal's) in one process over redwood-drm's one connection.
7. Speed against the GL path we have, with synchronous submission.

## 9. How the readings were made

`research/metal/` (ignored by git; nothing of Apple's is in the repository): the root image
mounted read-only, the dyld cache copied, [blacktop/ipsw](https://github.com/blacktop/ipsw)
3.1.745 for `dyld disass`, `dyld extract`, `class-dump`, `macho info`; two scripts there
(`cdsel.py`: selectors a class leaves unimplemented of a protocol; `fnsel.py`: selectors and
strings per function of a whole-image disassembly). HackiOS: both branches cloned under
`research/metal/prior/`. Mesa: the 26.3.0 tree of the mesa-ios build.

## Credits

HackiOS (OOSEX GANG, MIT): the loading scheme, the measurements quoted, and the driver this
plan proposes to port (`hakios-xnu-kernel`, main and `mtk`). UniXNU's AdrenoMetal (MIT) and
steelbrain's metal2vulkan for the Vulkan road. Mesa (MIT).
