# a34gl: CoreAnimation on the Mali-G68 (Mesa panfrost)

On iOS 16 the window server (backboardd) draws with Metal, else with OpenGL ES through
`EAGLContext`, else with CoreAnimation's own software rasterizer. There is no Metal and no
Apple GPU here, so `EAGLContext` fails and everything is drawn by the CPU. **a34gl** puts an
OpenGL ES of ours behind `EAGLContext` inside backboardd: Mesa 26.3's panfrost on the A34's
Mali-G68 (`userland/mesa`), with CoreAnimation's IOSurfaces as the GPU's own textures and
render targets. It is a port of [ios-topaz](https://github.com/MakrSas/ios-topaz)'s
`userland/gpu` (iOS 16.7.16 on an Adreno 610 with freedreno), which grew out of HackiOS.

Built by `.github/workflows/a34gl.yml` (branch `a34gl`, artifact **`a34gl`**), or on a Mac with
`userland/mesa/build.sh` then `userland/a34gl/build.sh`. Installing: [INSTALL.md](INSTALL.md).

| file | what | from |
| --- | --- | --- |
| `eagl.c` | `liba34gl.dylib`: `EAGLContext` by method swizzling (`initWithAPI:properties:`, `API`, `setParameter:to:`, `getMacroContextPrivate`, `texImageIOSurface:...`, `attachImage:toCoreSurface:invertedRender:`, `dealloc`); the GL function table CoreAnimation calls, laid out as iOS's OpenGLES lays out its own; the GLSL ES 1 -> 3.00 rewrite, the programs for CoreAnimation's draws without one, vibrancy, variable blur, IOSurface copies where memory cannot be shared | topaz `eagl.c` (HackiOS) |
| `gl_egl.c` | the library behind it: EGL surfaceless + OpenGL ES 3 on panfrost; IOSurface -> texture by `EGL_IOSURFACE_A34` | new (replaces topaz `gl_osmesa.c`) |
| `a34gl.h`, `log.c`, `a34gl_iokit.h` | the backend interface (topaz `redwood_gl.h`), the log, IOKit declarations | |
| `inject.c` | `liba34gl-inject.dylib`: in backboardd, waits for `RedwoodMali`, `dlopen`s `liba34gl.dylib`, hooks two `CA::OGL::GLESContext` virtual functions | topaz `topaz_inject.c` |
| `bbd.c` | `a34gl-bbd`: backboardd's stand-in in its launchd job (sets `DYLD_INSERT_LIBRARIES`, execs the real one) | topaz `bbd_wrap.c` |
| `surftest.c` | `a34gl-surftest`: the IOSurface path without CoreAnimation | new |
| `mkeagl.py`, `eagl_slots.txt` | generate `eagl_gen.h` (the function table) from the slot offsets, Mesa's `gl.xml` and libGLESv2's exports | HackiOS, unchanged |
| `check-cache.py` | checks inject.c's cache addresses and every slot of `eagl_slots.txt` against a root's dyld cache | new |
| `../mesa/patches/0003-darwin-iosurface-egl-image.patch`, `../mesa/redwood-drm` | the IOSurface import in Mesa and libdrm (selector `0x7e`) | after topaz's `freedreno-darwin.patch` / `redwood-drm` |

## The backend

`gl_egl.c` is all that knows which library draws (topaz's `gl_osmesa.c` used OSMesa with an ES
profile patch; OSMesa was removed from Mesa in 25.1):

- **display**: one per process, `eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA)`. EGL finds
  the GPU through redwood-drm's `drmGetDevices2` (the `RedwoodMali` service), exactly as
  `mali-tri` does.
- **context**: OpenGL ES 3 for whatever API CoreAnimation asks (it asks for 3; eagl.c's own
  programs are GLSL ES 3.00), current with a 16x16 pbuffer. CoreAnimation draws only into its
  own framebuffers, whose colour textures are IOSurfaces.
- **threads**: EGL lets a context be current on one thread at a time (OSMesa did not care). A
  context is let go after `-initWithAPI:` (the creating thread need not be the drawing one);
  if a thread ever finds it held by another, every frame end lets it go from then on
  (`glrelease=1` forces that from the start).
- **IOSurfaces**: `eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_IOSURFACE_A34,
  (EGLClientBuffer)(uintptr_t)IOSurfaceGetID(s), NULL)`, then `glEGLImageTargetTexture2DOES` on
  the texture CoreAnimation bound, then the image is destroyed (the texture keeps the memory).
  Where that fails (a format, layout or kernel that does not allow it) eagl.c falls back to
  topaz's copies: the texture is filled from the surface when its seed or contents change and
  read back into it at the frame's end.

### `EGL_IOSURFACE_A34` (Mesa patch 0003)

The topaz way -- an image named by the IOSurface's ID that ends in
`pipe_screen::resource_from_handle` with a handle the kernel made of the surface -- on Mesa's
EGL/DRI path:

```
eglCreateImageKHR(EGL_IOSURFACE_A34, id)                         egl_dri2.c  dri2_create_image_iosurface()
  redwood_drm_surface({surface = id})          0x7e, describe:   size, offset, row_bytes, width, height, pixel_format, planes
  pixel_format -> fourcc   'BGRA' ARGB8888, 'RGBA' ABGR8888, 'l10r' ARGB2101010, 'RGhA' ABGR16161616F, 'L008' R8, 'L016' R16
  dri2_create_image_dma_buf(WIDTH, HEIGHT, FOURCC, PLANE0_FD = id, PLANE0_OFFSET = offset, PLANE0_PITCH = row_bytes)
    dri2_from_dma_bufs -> panfrost_resource_from_handle(WINSYS_HANDLE_TYPE_FD, handle = id, linear)
      panfrost_kmod_bo_import(fd = id)
        drmPrimeFDToHandle(id)                 0x7e, import:     GEM handle of the surface's memory
        size = redwood_drm_surface(id).size    0x7e, describe    (Linux: lseek() on the dma-buf)
        PANFROST_GET_BO_OFFSET (0x45)                            the BO's GPU address
      CPU access, if Mesa wants it: IOConnectMapMemory64(type = handle)
```

Also in the patch: `u_init_pipe_screen_caps` asks `DRM_CAP_PRIME` on Darwin too, and redwood-drm
answers `DRM_PRIME_CAP_IMPORT` (export stays unsupported), which turns on
`EGL_EXT_image_dma_buf_import`. The constant is in `EGL/eglmesaext.h` (`0x3A34`). One plane only
(`planes == 0`), linear, and panfrost on Valhall wants `row_bytes` and `offset` to be multiples
of 64 bytes; anything else is `EGL_BAD_MATCH` and eagl.c copies instead.

### RedwoodMali selector `0x7e` (exactly ios-topaz's)

`IOConnectCallStructMethod(connection, 0x7e, &s, 40, &s, &40)` with

```c
struct redwood_drm_surface {          /* topaz: struct redwood_surface (RedwoodGPU.cpp) */
	uint32_t surface;             /*  0 in:  the IOSurface's ID */
	uint32_t handle;              /*  4 in:  0xffffffff = describe only (comes back 0); else make the buffer
	                               *    out: the GEM handle of the surface's memory */
	uint64_t size;                /*  8 out: the buffer's size, the surface's allocation rounded up to a page */
	uint32_t offset;              /* 16 out: where the pixels start in the buffer */
	uint32_t row_bytes;           /* 20 out: bytes from one row to the next */
	uint32_t width, height;       /* 24, 28 out */
	uint32_t pixel_format;        /* 32 out: the IOSurface's FourCC ('BGRA' = 0x42475241) */
	uint32_t planes;              /* 36 out: IOSurfaceGetPlaneCount (0: not planar) */
};                                    /* 40 bytes, little-endian */
```

redwood-drm sends `handle = 0` to import (`drmPrimeFDToHandle`) and `0xffffffff` to describe
(`redwood_drm_surface()`), and the kernel fills in the rest either way. As topaz's kernel does,
a surface that is a buffer of the client's already is the same handle again (Linux PRIME
semantics: panfrost keeps one BO per handle and reference-counts it), and the buffer holds the
surface until `GEM_CLOSE` (`0x09`). Errors: `kIOReturnNotFound` (no such surface),
`kIOReturnUnsupported` (its memory cannot be the GPU's), `kIOReturnNoMemory`.

What else the kernel must do with such a handle, because Mesa treats it as any BO: give its GPU
address with `PANFROST_GET_BO_OFFSET` (`0x45`, not used for Mesa's own BOs); map it as client
memory of type = handle (`IOConnectMapMemory64`, the whole page-rounded size); accept it in a
`SUBMIT`'s `bo_handles`, `WAIT_BO`, `MADVISE`; and keep the CPU's and the GPU's view of the
memory the same (CoreAnimation and the display read the surface with the CPU and the display
engine; topaz's kernel cleans the caches of surface memory around submissions). backboardd runs
as `mobile`: `RedwoodMali` must open for a non-root task.

## Injection

What topaz did, in its kernel and root (`xnu/patches/0902`, `0919`, `0947`; `userland/gpu/bbd_wrap.c`):

1. **dyld's policy question.** dyld asks the MAC policy named `AMFI`
   (`amfi_check_dyld_policy_self` = `__mac_syscall("AMFI", 0x5a, {u64 flags in, u64 *flags out})`)
   what it may honour; without an answer it treats the process as restricted and ignores every
   `DYLD_*` variable and every `@` path. Our kernel's AMFI stand-in (ruby/sweet) is registered as
   `"SWEET AMFI"` with no `mpo_policy_syscall`, so the call fails with `ENOPOLICY` -- that is the
   "security policy does not allow @ path expansion" seen on the phone. topaz 0902 answers `0x5a`
   with `0xff` (everything allowed).
2. **launchd drops `DYLD_*` variables** from job descriptions, so `DYLD_INSERT_LIBRARIES` cannot be
   put into backboardd's `EnvironmentVariables`. topaz first replaced `/usr/libexec/backboardd`
   with `bbd_wrap` (real binary moved to `/usr/libexec/real/`), later added `kern.exec_inject`
   (0919: the kernel adds `DYLD_INSERT_LIBRARIES=LIB` to the environment of programs by name,
   from a sysctl or the boot-arg `topaz_inject`).

Here: the wrapper, without moving Apple's binary. `tools/root-add-job.py --wrap /mnt
com.apple.backboardd /a34/a34gl-bbd` changes backboardd's job in `launchd.plist`
(`ProgramArguments[0]` = the wrapper, `EnvironmentVariables.A34GL_REAL` = `/usr/libexec/backboardd`;
`--unwrap` undoes it); the wrapper reads `/a34/backboardd.env`, sets `DYLD_INSERT_LIBRARIES` and
execs the real program, which keeps the pid, the Mach services and its own signature and
entitlements. Everything that is not `DYLD_*` can also go into the job with `--env`.

**Kernel prerequisite** (not in this branch; for `xnu/patches`): a policy named `"AMFI"` that
answers dyld, in `bsd/kern/sweet_amfi.c` next to the existing two (after topaz 0902, APSL 2.0):

```c
static int
a34_amfi_syscall(__unused struct proc *p, int call, user_addr_t arg)
{
	uint64_t in_out[2] = { 0, 0 }, allow = 0xff;    /* AMFI_DYLD_OUTPUT_ALLOW_*: all */
	int error;

	if (call != 0x5a) {             /* other libamfi questions: succeed, output untouched (topaz) */
		return 0;
	}
	error = copyin(arg, in_out, sizeof(in_out));     /* { u64 flags in, user u64 *flags out } */
	if (error == 0) {
		error = copyout(&allow, (user_addr_t)in_out[1], sizeof(allow));
	}
	return error;
}
static const struct mac_policy_ops a34_amfi_mac_ops = { .mpo_policy_syscall = a34_amfi_syscall };
static struct mac_policy_conf a34_amfi_mac_conf = {
	.mpc_name = "AMFI", .mpc_fullname = "dyld policy answer", .mpc_ops = &a34_amfi_mac_ops,
};
static mac_policy_handle_t a34_amfi_mac_handle;
/* where the other two are registered: */
	mac_policy_register(&a34_amfi_mac_conf, &a34_amfi_mac_handle, NULL);
```

Without it nothing is injected and backboardd runs as before (the wrapper is harmless).

## Switches

As in topaz, from the kernel's boot-args, and here also from the environment variable `A34GL`
(same words; the boot-args win): `a34gl=0` everything off (also `A34GL_OFF=1`), `glcopy=1` copy
every IOSurface instead of sharing its memory, `glblit=0` draw straight into the display's surface
(default: into a texture of ours, blitted into the surface at the frame's end), `glwait=0|1`
(who waits for the GPU before a frame is shown; the default waits here unless the display
driver sets `RedwoodWaitsForGPU`), `glrelease=1`, `glframes=1` (a line a frame), `glcensus=1`,
`glshaders=1`, `gltrace=FIRST-LAST`, `gltrace_at=`, `gltrace_draws=`, `gltrace_function=N[,K]`,
`gltrace_frames=`, `gltrace_place=X0,Y0,X1,Y1`, `gltrace_look=1`, `gltrace_lit=1`, `glslow=US`,
`glshow=TEXTURE`, `glplain=0|2|3`, `glfetch=0`, `glgroup=0`, `glprecision=0`, `vblur=0|2`,
`glenv=NAME=VALUE` (see the comment at the top of `eagl.c`). The log goes to `A34GL_LOG`
(e.g. `/var/mobile/a34gl.log`; `/dev/console` is writable by `mobile` and lands in the kernel's
log), else to stderr, which launchd discards for backboardd.

## Checked against our iOS 16.7.16 cache

`check-cache.py <mounted root>/System/Library/Caches/com.apple.dyld` on our root image:

```
cache UUID 8f970452-c9e3-3e88-b114-feb1864bc9e0
GLESContext::function_supported    slot 0x1d5b2fa70 holds 0x1884b2164, expected 0x1884b2164: ok
GLESContext::function_uniform      slot 0x1d5b2fa80 holds 0x1884b21ac, expected 0x1884b21ac: ok
dispatch table: 409 slots checked against OpenGLES's stubs (table at +0x20 in the context), 0 different
```

topaz identifies the cache by nothing but these values (inject.c checks each slot before
replacing it, so a different cache leaves CoreAnimation alone); the slot table is HackiOS's,
read off 16.5, and every one of its 409 named slots matches 16.7.16's OpenGLES stubs.

## Test

`a34gl-surftest [-v] [WIDTH HEIGHT]` (default 320x200): two `'BGRA'` IOSurfaces from
IOSurface.framework (rows of a multiple of 64 bytes); describes them through `0x7e`; imports
them with `EGL_IOSURFACE_A34`; (1) draws into surface A through a framebuffer (clear, scissored
clear, triangle) and checks the pixels with `glReadPixels` and in the surface's memory as the CPU
sees it; (2) samples surface B, filled by the CPU, into a texture of the GPU's own; (3) blits that
into A and checks A's memory. `PASS` / exit 0; 1 wrong pixels; 2 no EGL/GLES; 3 the surface could
not be imported. `-v` turns on `REDWOOD_DRM_DEBUG` and `EGL_LOG_LEVEL=debug`.

## Open points

- The `0x7e` kernel side, `GET_BO_OFFSET` for imported handles and cache maintenance of surface
  memory are the kernel's (another branch); the AMFI answer above is needed for any injection.
- CoreAnimation's surfaces must have 64-byte-aligned rows and offsets for panfrost to take them
  as they are; others are copied (slow but right).
- topaz's Adreno-specific fixes (freedreno's framebuffer-fetch flush) have no panfrost
  counterpart here; if vibrant layers or label shadows show tile garbage, `glfetch=0`.
- eagl.c's `function_uniform` hooks and the function number at `context+0x10 -> +0x10` are
  CoreAnimation internals of this exact build (checked by value at run time, not by symbol).

## Credits and licences

- **ios-topaz** (MakrSas, <https://github.com/MakrSas/ios-topaz>): `eagl.c`, `topaz_inject.c`,
  `bbd_wrap.c`, `redwood_gl.h`, `gl_osmesa.c`, the `0x7e` interface (`redwood-drm`, kernel
  `RedwoodGPU.cpp`) and the freedreno patch the Mesa import follows. Its `CREDITS.md` puts the GPU
  user space under HackiOS's MIT licence (below); ios-topaz has no licence file of its own for its
  additions (`topaz_inject.c`, `bbd_wrap.c`).
- **HackiOS** (OOSEX GANG, <https://github.com/oosexgang/hakios-xnu-kernel>,
  <https://github.com/oosexgang/hackios-xnu-userland>): the EAGL scheme, `mkeagl.py` and
  `eagl_slots.txt` (copied unchanged from hakios-xnu-kernel `userland/` at `63fc2b4`, where
  `userland/` and the tools are MIT, Copyright (c) 2025-2026 the HakiOS authors), and the inject
  scheme (darwin-redwood). The kernel snippet above follows topaz's APSL 2.0 kernel patch.
- **Mesa** (MIT): fetched at build time; our patches are under Mesa's licence.

```
MIT License

Copyright (c) 2026 OOSEX GANG
Copyright (c) 2025-2026 the HakiOS authors

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
