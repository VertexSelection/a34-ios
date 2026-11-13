# Mesa panfrost for the iOS 16 user space (Mali-G68)

Mesa's **panfrost** Gallium driver, built for the iOS 16.7 (iPhone X, `arm64`, not `arm64e`)
user space that runs on the Galaxy A34's Mali-G68 MC4 (`GPU_ID 0x92041010`, Valhall v9, job
manager). The GPU is reached through the kernel's IOKit service **`RedwoodMali`**, whose user
client speaks Linux's panfrost DRM uAPI. Built by `.github/workflows/mesa-ios.yml`
(artifact `mesa-ios`), or on a Mac with `build.sh`.

| file | what |
| --- | --- |
| `build.sh` | the whole build: host tools, libdrm stand-in, Mesa, `mali-tri`, packaging |
| `patches/0001-meson-darwin-drm.patch` | Darwin + panfrost builds as a DRM system (drm DRI platform, EGL, libdrm via pkg-config) |
| `patches/0002-panfrost-darwin-bo-map.patch` | BO mmap/munmap through the user client; no `sched_setscheduler` on Darwin |
| `patches/0005-darwin-panfrost-keep-cached-bo-mappings.patch` | a BO keeps its CPU mapping while it sits in panfrost's BO cache (Linux unmaps it there because its kernel may purge a cached BO; RedwoodMali never purges): no `IOConnectMapMemory64`/`UnmapMemory64` pair every time a pool or texture BO is reused |
| `patches/0004-darwin-egl-context-takeover.patch` | EAGL's thread semantics for EGL contexts: the last thread to make a context current has it, instead of `EGL_BAD_ACCESS` (see `../a34gl/README.md`, "Threads") |
| `patches/0003-darwin-iosurface-egl-image.patch` | IOSurfaces as EGLImages: target `EGL_IOSURFACE_A34` (buffer = IOSurface ID), imported as a dma-buf whose "fd" is the ID; `DRM_CAP_PRIME` asked on Darwin; the BO size of an import from selector `0x7e` (see `../a34gl/README.md`) |
| `redwood-drm/` | `libdrm.2.dylib`: the libdrm functions Mesa calls, carried to `RedwoodMali` |
| `mali-tri/mali-tri.c` | test: 256x256 pbuffer, clear, one triangle, `glReadPixels`, check |

## Mesa version and frontend

**Mesa 26.3.0** (the first release with `PAN_PROD_ID(9, 2, 4)`, "G68"), fetched from
archive.mesa3d.org in CI and checked against a pinned SHA-256; only the two small patches
above are kept here.

The frontend is **EGL with the surfaceless platform** (`EGL_PLATFORM_SURFACELESS_MESA`,
pbuffers and FBOs) and **OpenGL ES 2/3** through `libGLESv2`. ios-topaz used OSMesa with an
ES-profile patch, but OSMesa was removed from Mesa in 25.1, so it is not in 26.3. EGL
surfaceless is the supported offscreen path that is left: it needs no window system, and it
reaches panfrost exactly as on Linux (EGL device list from `drmGetDevices2`, the DRI loader
picks the driver from `drmGetVersion`, `panfrost_drm_screen_create` on the descriptor).
Reviving OSMesa would have meant carrying the whole removed frontend as a patch against
Mesa's moving internal APIs.

Output, for `/a34` on the phone (`INSTALL_DIR`): every library is named `/a34/<file>` and names
the others so, nothing has an `LC_RPATH` -- iOS's dyld refuses `@rpath`, `@loader_path` and
`@executable_path` for our binaries ("security policy does not allow @ path expansion") -- and
everything is ad-hoc signed after the last change to a load command (a signature broken
afterwards made launchd panic: "unexpected exec of non-platform binary"):

- `libEGL.1.dylib`, `libGLESv2.2.dylib`: the API libraries a program links;
- `libgallium-26.3.0.dylib`: Mesa itself (GL state tracker, DRI frontend, panfrost);
- `libdrm.2.dylib`: the stand-in below (linked as `/a34/libdrm.2.dylib` from the start);
- `mali-tri`: the test, linked against those (it runs from anywhere once they are in `/a34`);
- `include/`: EGL (with `eglmesaext.h`: `EGL_IOSURFACE_A34`), GLES2, GLES3, KHR headers and
  `redwood_drm.h`; `BUILD_INFO.txt`.

## The libdrm stand-in (`redwood-drm/`)

Built against libdrm's own `xf86drm.h` (fetched from the libdrm 2.4.134 release) and Mesa's
`include/drm-uapi`, so every structure is the Linux one. One `io_connect_t` per process,
opened on first need with `IOServiceGetMatchingService(IOServiceMatching("RedwoodMali"))` and
`IOServiceOpen(type 0)`. Older iPhoneOS SDKs ship `IOKit.tbd` without IOKit headers (the
18.5 SDK of Xcode 16.4 has `IOKitLib.h`): `redwood_iokit.h` uses the SDK's header when there
is one and otherwise declares the eight calls used, as ios-topaz relied on linking IOKit
directly. The build checks those declarations and values against the macOS SDK's real
headers (`iokit_check.c`) and compiles that branch for iOS as well.

| libdrm call | here |
| --- | --- |
| `drmIoctl(fd, DRM_IOCTL_PANFROST_*)`, `drmCommand*` | `IOConnectCallStructMethod(selector = request & 0xff, in = out = the struct, IOCPARM_LEN(request) bytes)` |
| `DRM_IOCTL_PANFROST_SUBMIT` | as above, with `in_syncs`/`in_sync_count`/`out_sync` cleared; `out_sync` is signalled when the call returns |
| `drmCloseBufferHandle`, `DRM_IOCTL_GEM_CLOSE` | selector `0x09` |
| `drmGetVersion`, `DRM_IOCTL_VERSION` | answered here: `panfrost` 1.1 (`REDWOOD_DRM_MINOR` overrides the minor) |
| `drmGetCap` | answered here: `SYNCOBJ` 1, `SYNCOBJ_TIMELINE` 0, `PRIME` `IMPORT` (IOSurfaces only), `TIMESTAMP_MONOTONIC` 1 |
| `drmGetDevices2`, `drmGetDevice2`, ... | one platform-bus device, render node `/dev/null` (no service: no device) |
| `drmOpen*`, the EGL loader's `open()` | `/dev/null`: any descriptor in the process means the one GPU |
| `PANFROST_WAIT_BO` | answered here ("ready") when none of the process's threads is inside a SUBMIT and the BO is not an IOSurface's: the kernel runs a job to its end inside SUBMIT, so the process's own BOs are idle then. Otherwise the kernel is asked |
| `PANFROST_MADVISE` | answered here (`retained` 1) for BOs the kernel has declared write-combined (`REDWOOD_DRM_CREATE_BO_WC`, bit 0 of the `pad` that comes back from `CREATE_BO`): it does no CPU cache upkeep for them, so `DONTNEED` tells it nothing. For all others the kernel is asked: `DONTNEED` is what keeps a cacheable BO out of its per-job upkeep while Mesa has no use for it |
| queued jobs (kernel 0919) | asked for at open (GET_PARAM `0x1000` = 1, then SET_MODE `0x7a` `{async = 1}`; `REDWOOD_DRM_ASYNC=0` stays synchronous; one stderr line says which). A queued SUBMIT returns at once with the job's number in `in_syncs`: `out_sync` gets that number as its fence, and every BO in `bo_handles` notes it. A fence is signalled once WAIT_SEQ (`0x7b`) has said every job up to its number is done (`drmSyncobjWait`: for all, the last; for any, the first -- a client's jobs end in the order queued). `in_syncs` need no wait (same order). WAIT_BO: answered here when the last job naming the BO is known done, else WAIT_SEQ for it, and the kernel's WAIT_BO for IOSurface BOs (other programs' jobs). Sync files carry the fence (pipe inode -> number); a failed job is said once on stderr. Host-tested against a fake kernel: out-of-order ends, a failed job, blocking waits, WAIT_FOR_SUBMIT, sync files |
| `drmSyncobj*` | user space only: create/destroy/reset/signal/wait (`WAIT_FOR_SUBMIT` blocks until a SUBMIT signals), sync files are signalled pipes |
| BO mmap / munmap (patched in Mesa) | `redwood_drm_mmap` = `IOConnectMapMemory64(type = GEM handle, kIOMapAnywhere)`, `redwood_drm_munmap` = `IOConnectUnmapMemory64` |
| `drmPrimeFDToHandle(fd, IOSurfaceID)` | selector `0x7e` (ios-topaz's IOSurface request, 40 bytes, `handle` = 0): the GEM handle of the surface's memory |
| `redwood_drm_surface()` (not libdrm's) | selector `0x7e` with `handle` = `0xffffffff`: only describe the surface (buffer size, pixel offset, row bytes, width and height, format, planes) |
| `drmPrimeHandleToFD` | `EOPNOTSUPP` (no export) |

Environment: `REDWOOD_DRM_DEBUG=1` logs every request; `REDWOOD_DRM_SERVICE`,
`REDWOOD_DRM_NODE`, `REDWOOD_DRM_MINOR`. `REDWOOD_DRM_ASYNC=0`: synchronous SUBMITs even
where the kernel queues. `REDWOOD_DRM_LOCAL` (default 1): 0 sends
every `WAIT_BO` and `MADVISE` to the kernel; 2 takes every BO made here to be write-combined
(all `MADVISE` answered here -- to measure, with a kernel that does not say: a BO the kernel
made cacheable then stays in its per-job cache upkeep while cached, slower but never wrong).
The kernel makes a program's BOs write-combined (0918, `a34maliwc` not 0) but does not report it in
CREATE_BO's `pad`, so every `MADVISE` went to the kernel: about 110 a frame in Control Center,
1.7 ms of backboardd's drawing thread. With a kernel that queues and no `a34maliwc=0` in the
boot-args, redwood-drm now takes every BO that is neither a heap nor `PANFROST_BO_WB_MMAP` to be
write-combined. Its `MADVISE` is then answered here; the line at open says so.

### What the kernel user client must implement

Selectors and structure sizes (Mesa 26.3's `panfrost_drm.h`, all little-endian, arm64):

| selector | request | bytes | used by Mesa at driver version 1.1 |
| --- | --- | --- | --- |
| `0x09` | `GEM_CLOSE` `{u32 handle, pad}` | 8 | yes |
| `0x40` | `PANFROST_SUBMIT` | **48** (`jm_ctx_handle`, `pad` after `requirements`) | yes; `jc` = first job, `bo_handles` = user pointer to `bo_handle_count` u32s, `requirements` bit 0 `FS`; syncobj fields always 0 |
| `0x41` | `PANFROST_WAIT_BO` `{u32 handle, pad; s64 timeout_ns}` | 16 | yes (answer at once: jobs are synchronous); redwood-drm sends it only for IOSurface BOs and while another thread's SUBMIT runs |
| `0x42` | `PANFROST_CREATE_BO` `{u32 size, flags, handle, pad; u64 offset}` | 24 | yes; flags `NOEXEC` 1, `HEAP` 2 (the tiler heap, one 128 MiB BO per screen: grow on fault, or back it all); `offset` = GPU VA, which Mesa expects in [0x2000000, 4 GiB) as on Linux; `pad` back: bit 0 set = the BO is write-combined for the CPU and needs no `MADVISE` (optional: 0 = unknown, every `MADVISE` is sent) |
| `0x44` | `PANFROST_GET_PARAM` `{u32 param, pad; u64 value}` | 16 | yes, params below |
| `0x45` | `PANFROST_GET_BO_OFFSET` `{u32 handle, pad; u64 offset}` | 16 | only for imported BOs (IOSurfaces, `0x7e`) |
| `0x7e` | IOSurface -> GEM handle, or describe (ios-topaz's layout) | 40 | `EGL_IOSURFACE_A34` images: `../a34gl/README.md` |
| `0x48` | `PANFROST_MADVISE` `{u32 handle, madv, retained}` | 12 | yes (BO cache: `DONTNEED` 1 when cached, `WILLNEED` 0 on reuse); `retained = 0` makes Mesa free the BO instead |
| `0x43` | `PANFROST_MMAP_BO` | 16 | **not used**: mapping is `IOConnectMapMemory64(type = handle)` |
| `0x49`-`0x4d` | `SET_LABEL_BO`, `JM_CTX_*`, `SYNC_BO`, `QUERY_BO_INFO` | | only at versions 1.4 / 1.5 / 1.6 |

`GET_PARAM` numbers queried: 0 `GPU_PROD_ID` (0x9204), 1 `GPU_REVISION`, 2 `SHADER_PRESENT`,
8 `L2_FEATURES`, 10 `TILER_FEATURES`, 11 `MEM_FEATURES`, 12 `MMU_FEATURES`,
13 `THREAD_FEATURES`, 14 `MAX_THREADS`, 15 `THREAD_MAX_WORKGROUP_SZ`, 18-21
`TEXTURE_FEATURES0-3`, 39 `THREAD_TLS_ALLOC`, 40 `AFBC_FEATURES`; optional: 43
`ALLOWED_JM_CTX_PRIORITIES`; at 1.3+: 42 `SYSTEM_TIMESTAMP_FREQUENCY`; at 1.6+: 44
`SELECTED_COHERENCY`. An unknown parameter should fail (`kIOReturnBadArgument`). Client memory of type *handle* must be the BO's whole (page-rounded) size;
memory is mapped write-combined (no `WB_MMAP` before 1.6). IOReturn codes become errno:
`BadArgument` EINVAL, `NoMemory` ENOMEM, `NotFound` ENOENT, `Timeout` ETIMEDOUT, `Busy` EBUSY,
`Unsupported` ENOTTY, others EIO.

## Running

Copy the `mesa-ios` artifact's files into `/a34` on the phone and run `/a34/mali-tri`
(or `/a34/mali-tri out.ppm`). It prints `GL_RENDERER` (expected: `Mali-G68 (Panfrost)`),
`GL_VERSION`, a few pixels and `PASS`/`FAIL`; exit status 0 = right pixels, 1 = wrong
pixels, 2 = EGL/GL setup failed. `EGL_LOG_LEVEL=debug`, `PAN_MESA_DEBUG=sync,trace` and
`REDWOOD_DRM_DEBUG=1` help. The binaries are ad-hoc signed only; add entitlements (ldid)
if the userland's AMFI needs them.

## Credits

- **ios-topaz** (<https://github.com/MakrSas/ios-topaz>, `userland/mesa/`): the approach of a
  libdrm stand-in that turns DRM requests into IOKit struct-method calls and BO mappings
  into `IOConnectMapMemory64`, the `/dev/null` descriptor, the errno mapping and the
  Mesa-on-iOS build setup are adapted from its `redwood-drm` and `mk-mesa-mac.sh`.
- **HackiOS** (OOSEX GANG, <https://github.com/oosexgang/hackios-xnu-userland>), on which
  topaz's user space is based, and whose `RedwoodMali` user client (HackiOS's Mali-G52 port)
  set the selector convention used here. Their user space is under the MIT License:

```
MIT License

Copyright (c) 2026 OOSEX GANG

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

- **Mesa** (MIT) is fetched, not vendored; the patches here are under Mesa's licence.
  **libdrm**'s `xf86drm.h` (MIT) is fetched at build time.
