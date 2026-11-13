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

Output (all `@rpath/...`, with an `@loader_path` rpath, ad-hoc signed):

- `libEGL.1.dylib`, `libGLESv2.2.dylib`: the API libraries a program links;
- `libgallium-26.3.0.dylib`: Mesa itself (GL state tracker, DRI frontend, panfrost);
- `libdrm.2.dylib`: the stand-in below;
- `mali-tri`: the test (rpath `@executable_path`; put the dylibs next to it);
- `include/`: EGL, GLES2, GLES3, KHR headers; `BUILD_INFO.txt`.

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
| `drmGetCap` | answered here: `SYNCOBJ` 1, `SYNCOBJ_TIMELINE` 0, `PRIME` 0, `TIMESTAMP_MONOTONIC` 1 |
| `drmGetDevices2`, `drmGetDevice2`, ... | one platform-bus device, render node `/dev/null` (no service: no device) |
| `drmOpen*`, the EGL loader's `open()` | `/dev/null`: any descriptor in the process means the one GPU |
| `drmSyncobj*` | user space only: create/destroy/reset/signal/wait (`WAIT_FOR_SUBMIT` blocks until a SUBMIT signals), sync files are signalled pipes |
| BO mmap / munmap (patched in Mesa) | `redwood_drm_mmap` = `IOConnectMapMemory64(type = GEM handle, kIOMapAnywhere)`, `redwood_drm_munmap` = `IOConnectUnmapMemory64` |
| `drmPrime*` | `EOPNOTSUPP` (no dma-buf sharing) |

Environment: `REDWOOD_DRM_DEBUG=1` logs every request; `REDWOOD_DRM_SERVICE`,
`REDWOOD_DRM_NODE`, `REDWOOD_DRM_MINOR`.

### What the kernel user client must implement

Selectors and structure sizes (Mesa 26.3's `panfrost_drm.h`, all little-endian, arm64):

| selector | request | bytes | used by Mesa at driver version 1.1 |
| --- | --- | --- | --- |
| `0x09` | `GEM_CLOSE` `{u32 handle, pad}` | 8 | yes |
| `0x40` | `PANFROST_SUBMIT` | **48** (`jm_ctx_handle`, `pad` after `requirements`) | yes; `jc` = first job, `bo_handles` = user pointer to `bo_handle_count` u32s, `requirements` bit 0 `FS`; syncobj fields always 0 |
| `0x41` | `PANFROST_WAIT_BO` `{u32 handle, pad; s64 timeout_ns}` | 16 | yes (answer at once: jobs are synchronous) |
| `0x42` | `PANFROST_CREATE_BO` `{u32 size, flags, handle, pad; u64 offset}` | 24 | yes; flags `NOEXEC` 1, `HEAP` 2 (the tiler heap, one 128 MiB BO per screen: grow on fault, or back it all); `offset` = GPU VA |
| `0x44` | `PANFROST_GET_PARAM` `{u32 param, pad; u64 value}` | 16 | yes, params below |
| `0x45` | `PANFROST_GET_BO_OFFSET` `{u32 handle, pad; u64 offset}` | 16 | only for imported BOs (not today) |
| `0x48` | `PANFROST_MADVISE` `{u32 handle, madv, retained}` | 12 | yes (BO cache); answer `retained = 1` |
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

Copy the `mesa-ios` artifact's files into one directory on the phone and run `./mali-tri`
(or `./mali-tri out.ppm`). It prints `GL_RENDERER` (expected: `Mali-G68 (Panfrost)`),
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
