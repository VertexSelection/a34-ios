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
- **threads**: CoreAnimation uses one context from several threads in turn, as EAGL allows; EGL
  does not (see "Threads" below: Mesa patch 0004). eagl.c remembers which thread made a
  context current last and makes it current again on any other thread's first call.
- **IOSurfaces**: `eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_IOSURFACE_A34,
  (EGLClientBuffer)(uintptr_t)IOSurfaceGetID(s), NULL)`, then `glEGLImageTargetTexture2DOES` on
  the texture CoreAnimation bound, then the image is destroyed (the texture keeps the memory).
  Where that fails (a format, layout or kernel that does not allow it) eagl.c falls back to
  topaz's copies: the texture is filled from the surface when its seed or contents change and
  read back into it at the frame's end. A surface CoreAnimation draws into is drawn as a
  texture of ours that the GPU copies into the surface at the frame's end (`glblit=0`: straight
  into the surface); that texture starts with the surface's pixels copied in by the GPU at
  `-attachImage:` (topaz: by the CPU, 11 MB for the display's surface).

### Threads (Mesa patch 0004)

EAGL lets a context move between threads: whichever thread last called `+setCurrentContext:`
has it (using it on two threads at the same time is the application's error), and
CoreAnimation's render server does move its context. OSMesa, behind topaz's library, kept no
note of threads at all. Mesa's EGL does: `eglMakeCurrent` of a context current to another
thread fails with `EGL_BAD_ACCESS` (`_eglCheckMakeCurrent`), and on the phone that error came at
nearly every GL call (the screen stayed black). Patch 0004 gives EGL contexts EAGL's semantics
on Darwin -- the context is **taken over** -- and has to do it from the taking thread only:

- **EGL** (`eglcontext.c` `_eglBindContext`): the context's `Binding` moves to this thread and
  the surfaces the other thread bound it to let go of it. Nothing of the other thread is
  touched: its `_EGLThreadInfo` is thread-local storage that may already be gone with the
  thread. Its binding goes *stale*: `CurrentContext` still names the context, and the
  references that binding took (context, surfaces, `dri2_dpy->ref_count`) are still held, so
  the context cannot be freed under it. Each thread now keeps the surfaces it bound
  (`CurrentDraw/CurrentRead`), because the context no longer says. `_eglGetCurrentContext()`
  answers NULL on a thread whose context was taken.
- **DRI/gallium, taking thread** (`dri2_make_current`): `driUnbindContext()` of the taken
  context lets go of the drawables the other thread bound it to (`dri_context->draw/read`
  references). `dri_unbind_context()` touches the GL state only when the context is the
  *calling* thread's current one (`st_api_get_current()` is thread-local), which it is not
  here -- so the pipe context, its unflushed work and all GL state go with the context. One
  context used by one thread at a time needs no flush to change threads. Then
  `driBindContext()` binds it here as usual (`st_api_make_current` -> `_mesa_make_current`,
  whose current context on this thread is none at that point, so nothing is flushed or let go).
- **DRI/gallium, the thread it was taken from**, at its next `eglMakeCurrent`/`eglReleaseThread`:
  its previous context is found stale. Then nothing of that context is undone (no
  `driUnbindContext`, no flush: it is another thread's now). Only this thread's GL
  thread-locals, which still name it, are cleared (`_mesa_glapi_set_dispatch(NULL)`,
  `_mesa_glapi_set_context(NULL)`). Left as they were, the next `_mesa_make_current` on this
  thread would treat the context as its own current one: flush it and drop its window-system
  framebuffers while the other thread draws with it -- the dangling state that would crash.
  The stale references are then given back as for any previous binding.
- **Serialisation**: `eglMakeCurrent` calls the driver without the display lock (`egl_relax`).
  Upstream that is safe because only the owning thread changes a binding; with takeover two
  threads do, so `dri2_make_current` takes one process-wide mutex on Darwin.
- **Left as is**: a thread that ends while it has a context keeps its references, so the
  context leaks instead of being freed under a dangling pointer. A context whose `Binding` is
  the calling thread although that thread never bound it (thread-local storage reused by a new
  thread) is taken over like any other. GL calls made on a thread after its context was taken,
  without making it current again, still reach that context through the stale thread-locals.
  That is OSMesa's behaviour, and the application's error if two threads draw at once.
  eagl.c makes the context current again whenever a call comes from another thread than the
  last one.

Without 0004 (an older Mesa), eagl.c falls back to letting the context go at every frame end
once it has seen `EGL_BAD_ACCESS` (`glrelease=1` forces it).

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

Here, two ways (both need the dyld answer below; kernel patch 0914 has it, boot-arg
`a34dyld=0` turns it off):

- **the kernel** (0914, like topaz 0919): boot-arg
  `a34inject=/a34/liba34gl-inject.dylib,backboardd` adds `DYLD_INSERT_LIBRARIES` to
  backboardd's environment at exec. Nothing in the root changes. The loader then reads
  `/a34/backboardd.env` itself, so `A34GL=...`, `PAN_MESA_DEBUG=...` and the like go there.
- **the wrapper**, without moving Apple's binary. `tools/root-add-job.py --wrap /mnt
com.apple.backboardd /a34/a34gl-bbd` changes backboardd's job in `launchd.plist`
(`ProgramArguments[0]` = the wrapper, `EnvironmentVariables.A34GL_REAL` = `/usr/libexec/backboardd`;
`--unwrap` undoes it); the wrapper reads `/a34/backboardd.env`, sets `DYLD_INSERT_LIBRARIES` and
execs the real program, which keeps the pid, the Mach services and its own signature and
entitlements. Everything that is not `DYLD_*` can also go into the job with `--env`.

**Kernel prerequisite**, now in `xnu/patches/0914` (boot-arg `a34dyld`, default on): a policy
named `"AMFI"` that answers dyld, as below (after topaz 0902, APSL 2.0):

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

## Programs other than backboardd

Any program that makes an `EAGLContext` gets one of iOS's OpenGLES with no driver behind it
(the kernel publishes an empty IOAcceleratorES): a function table of nothing, and the first GL
call jumps to 0. That is the SpringBoard crash with Bold Text on (CoreUI -> `+[CIContext
contextWithOptions:]` -> `CI::GLContext::init`). The loader therefore works in every program it
is inserted into (the kernel's `a34inject` list), and each gets **one line on the console**,
whatever its stderr is: `a34gl-inject: NAME[PID]: loaded, mode ...`.

- **backboardd**: as before -- liba34gl at once, CoreAnimation's two hooks.
- **any other program**: lazily. The loader swaps `-initWithAPI:properties:` and
  `-commonInitWithAPI:properties:` (every initializer ends in one of the two) for a stub. At the
  program's first EAGLContext the stub loads liba34gl (announced on the console too), which
  takes the methods over; the library's own context (Mesa, the GPU) is made only at that
  EAGLContext's first GL call. A program that never makes one pays a `dlopen` of OpenGLES and
  two method swaps; no Mesa, no GPU client. No CoreAnimation hooks (`A34GL_HOOKS=1` forces).
- `A34GL_MODE` per program, in `/a34/NAME.env` (`/a34/SpringBoard.env`, ...; read by the loader
  like `/a34/backboardd.env`): `mesa` (default), `none` (an EAGLContext cannot be made: `-init...`
  answers nil, for a program that does better without GL), `off` (nothing touched).

What makes a context of ours work for a program that is not CoreAnimation (read off the
16.7.16 cache's OpenGLES): an EAGLContext's ivar `_private` points to a structure with the
object at +0, the sharegroup at +8, the API at +0x10, flags at +0x18 and the function table at
+0x20. `+setCurrentContext:` stores `_private` in the thread's OpenGL slot (pthread key 30) and
retains the object; `+currentContext` reads it back; `EAGLGetCurrentMacroContextPrivate()` and
`-getMacroContextPrivate` answer `_private + 0x20`; and every `gl*` function OpenGLES exports is
a stub that loads the slot and jumps through the table. `struct eagl` now begins exactly so and
`_private` is set to it, so all of that reaches our entries with iOS's own code unreplaced --
whether the program takes the table (CoreAnimation, CoreImage) or calls `glClear()`.

Also for such programs: sharegroups (`-initWithAPI:sharegroup:`; a token of ours, the library's
contexts share objects); `-setBlockFence:onQueue:` (the block goes to its queue after a flush);
`glMapBufferOES` (Mesa advertises it and does not export it); shaders handed on as they are
unless they use `sampler2DRect`; no `glGetError` after every call; what is drawn into a
`-texImageIOSurface:` texture reaches the surface (its memory when shared, else read back at
`glFlush`/`glFinish`, BGRA only). Not done: `-renderbufferStorage:fromDrawable:` and
`-presentRenderbuffer:` (a CAEAGLLayer shows nothing).

**Every process**: the userland side is ready for a wildcard in the kernel's `a34inject` (the
cost per program is above); keep launchd out, and `A34GL_MODE=off` in `/a34/NAME.env` excepts
one. Each program that really draws is one more client of the GPU with Mesa's own buffers.

## Queued GPU jobs (kernel 0919)

With a kernel that queues (redwood-drm asks at open; `REDWOOD_DRM_ASYNC=0` declines), Mesa's
SUBMIT returns at once and the job runs in the kernel's worker; redwood-drm turns the job
numbers into Mesa's syncobjs, fences and WAIT_BO (see `../mesa/README.md`). Where the CPU reads
what the GPU wrote, it waits for the right job: `glFinish` and `glClientWaitSync` through the
fence (WAIT_SEQ); `glReadPixels` (and so eagl.c's copies out, `glshow`, `gltrace`), texture and
buffer maps through Mesa's WAIT_BO before it maps a busy BO; IOSurfaceLock in the kernel, for
the jobs that name the surface (eagl.c's CPU uploads lock the surface). The frame: backboardd
no longer waits for it (`glFinish` at the frame's end) but hands it on once its jobs are queued
(`glFlush`): the **panel's copy of the surface waits in the kernel** for the jobs that name it,
at most 500 ms -- that is what is relied on. `glwait=1` waits here as before. Other programs
still wait at the end of a frame drawn into a surface. glprof's kernel line gives the jobs
queued a frame, the time in `wait_seq` and `wait_bo`, and failed jobs. `glasync=0` (boot-arg
or `A34GL`) makes the jobs synchronous in every program a34gl is in, as `REDWOOD_DRM_ASYNC=0`
does. `glwait=1` is not the same: a frame's jobs still run after their SUBMIT, only the frame's
end waits. (`glenv=` is now applied before the connection to the kernel is made, so
`glenv=REDWOOD_DRM_ASYNC=0` works too.)

The seed of a surface the GPU has drawn into moves by an IOSurfaceLock and unlock. That lock
waits in the kernel for the jobs naming the surface: at the frame's end, the frame's own copy
into the display's surface, 1-1.5 ms of the drawing thread. With queued jobs it is done on a
dispatch queue after the frame's jobs have gone to the kernel, so the seed moves once the picture
is whole. `glseedwait=1`: on the drawing thread, as before.

### Copied surfaces: when they are copied

A surface that cannot be the texture's memory ('A008', 'LA08', 'RGhA', no format, 'BGRA' that
the kernel or Mesa does not take) is copied: when -texImageIOSurface:/-attachImage: makes the
texture, and again when its seed has moved or its memory adds up to something else when it is
bound. HackiOS (hakios-mtk `eagl.c`) does the same as topaz and as here (the same functions,
the same read-only lock, seed taken after the copy). The one difference is that it gives the
texture new storage on every copy (`glTexImage2D`), where 'BGRA' copies here go into the storage
the texture has (`glTexSubImage2D`: an 11 MB surface took 120-145 ms with new storage). Mesa
keeps that safe with queued jobs: panfrost replaces or waits for a BO that a job still reads.

Changed: the seed is taken **before** the memory is read. An owner that changes the surface
while it is being copied (a read-only lock keeps nobody out) then moves the seed past it, and the
next look copies again. Before, the half-made picture stayed until the next change. A status
bar's backing store cleared for its next text is all zero, and under a layer drawn opaque that
is a black box. Also, what is copied is added up first. The next look compares with what was
copied, where before every copy for a seed was made twice (the sum was the old one). A copy made
of memory that is all zero is logged ("copied ALL ZERO", with the surface's ID, name, format,
size and seed; the kernel's `SWEET SURF ... create id N` line names its maker), and so is the
next copy that is not. Sums: every look until 30 alike, then every 2nd frame, after 240 every
4th; a surface whose owner has told every change by its seed (twice or more), every 8th.
`glsum=2`: as before, `glsum=0`: none.

A surface that is drawn into here (-attachImage:) into a texture that is not its memory (rows
the kernel or Mesa do not take, e.g. 'BGRA' 1125 x 2436 at 4512 bytes a row: Mesa wants 64-byte
rows for linear RGBA on Valhall, `pan_linear_or_tiled_row_align_req()`) has what was drawn only
after the frame's end reads it back. Shown here as well (-texImageIOSurface:), it is now filled
by the GPU from the texture drawn into: when it is made, and whenever that has been drawn into
since. Its memory is used only for others' changes. `gltarget=0`: from the memory, as before.

Black pictures: at the lock that moves the seed of a full-screen surface drawn here (frame end;
with queued jobs on the seed queue, after the jobs), 64 pixels of a grid are read from its
memory. A picture that becomes (nearly) all black is logged ("is BLACK", with frame and surface),
and so is the next one that is not. A black flash on the screen that never shows up here is
made after a34gl, in the display's copy. `glblack=0`: not looked at.

## The log

Without `A34GL_LOG` everything goes to stderr. For backboardd, launchd makes that `/dev/null`,
and the kernel's "devnull tee" (ios-sweet patch 0048, boot-arg `sweetnull`, default
`backboardd,SpringBoard`) copies what backboardd writes there into the console and the
persistent log as `|backboardd| ...`. That is where the lines are with kernel injection, and
the recommended place. With `A34GL_LOG=/var/mobile/a34gl.log` (in `/a34/backboardd.env` or the
job's environment) they go to that file instead, and not to the kernel's log.

Every message is rate-limited (`log.c`): the first 32 of each are written, then at most one
every 10 s, with the count so far. A message repeating at every GL call costs a few lines.
Not limited: what a switch asks for (`gltrace`, `glframes`, `glslow`, `glcensus`,
`glshaders`, `gltrace_place`) and the periodic lines. Those are now a frame line for the first
40 frames and every 600th (topaz: every 20th), and the summary at frame 30 and every 3600th
(topaz: every 200th).

## Switches

As in topaz, from the kernel's boot-args, and here also from the environment variable `A34GL`
(same words; the boot-args win): `a34gl=0` everything off (also `A34GL_OFF=1`), `glcopy=1` copy
every IOSurface instead of sharing its memory, `glblit=0` draw straight into the display's surface
(default: into a texture of ours, blitted into the surface at the frame's end), `glwait=0|1`
(who waits for the GPU before a frame is shown; the default waits here unless the display
driver sets `RedwoodWaitsForGPU`), `glrelease=1` (let the context go at every frame end), `glframes=1` (a line a frame), `glcensus=1`,
`glshaders=1`, `gltrace=FIRST-LAST`, `gltrace_at=`, `gltrace_draws=`, `gltrace_function=N[,K]`,
`gltrace_frames=`, `gltrace_place=X0,Y0,X1,Y1`, `gltrace_look=1`, `gltrace_lit=1`, `glslow=US`,
`glshow=TEXTURE`, `glplain=0|2|3`, `glfetch=0`, `glgroup=0`, `glprecision=0`, `vblur=0|2`,
`glenv=NAME=VALUE` (see the comment at the top of `eagl.c`); `vblur=3` (the variable blur's
edges normalized by the picture's opacity, topaz's way: see below), `gltable=1` (every draw
with no program and a one-row table goes through the home indicator's arithmetic, topaz's way;
default: only function 53; `gltable=0`: none), `glskip=N[,N...]` (CoreAnimation's draws of
those functions with no program are not drawn: to find the one that paints wrongly);
a34gl's own: `glprof=N` (below),
`glsum=0` (an IOSurface that is copied into a texture is refilled when its seed moves, never
because its memory adds up differently -- that check reads the whole surface; `glsum=2`: as often
as before, see "Copied surfaces"), `glasync=0` (GPU jobs synchronous), `glseedwait=1` (a drawn
surface's seed moved on the drawing thread), `glvbafter=N[,K[,H]]` (the draws after the variable blur; H:
the least height of the vrb's rectangle that starts a frame, default 100, which leaves out the status
bar's), `gltarget=0`, `glblack=0` (see "Copied surfaces"),
`glerrors=0|1` (`glGetError` after every call never | always; default the first 120 frames).
The log: see "The log".

### Draws with no program, and the variable blur

CoreAnimation's GL renderer draws the functions it has no shader for with no program at all;
eagl.c draws them with programs of its own. The first draw of each function in each way now
gets a line without `glcensus=1`: `a34gl: no program: function N ("xxx"), <how it is drawn>:
... framebuffer, textures (size, format, a surface's memory or a copy), blending, colour, its
numbers`. A surface that is copied instead of shared says why (`... is copied, not the
texture's memory: <reason>`).

The blur whose radius varies ("vrb") with its edges "normalized": topaz divided by the
picture's opacity. The picture is what is on the screen behind the strip, and its opacity is
whatever the draws before left there (nobody needs it for the display): in the App Library the
pods' material leaves less than 1 over undarkened colours, and inside the strip under the
search field the pods came out white (icons and gaps, opaque, were right). Now the first level
of the blur counts which of its sixteen pixels are picture at all (opacity not 0, inside the
texture), the further levels carry that, and the reading divides by it: edges still do not
darken, and the picture's colours are taken as they are. `vblur=3` is topaz's division,
`vblur=0` no variable blur, `vblur=2` the mask painted, `glvbdump=N` writes down for the first N
"vrb" draws sixteen pixels across the middle row of the picture read and of what was left.
HackiOS's variable blur (hakios-mtk `eagl.c`: `vblur_level()`, `vblur_draw_begin()`, the four
shaders, 8 levels, 6 chains) is byte for byte topaz's: `vblur=3`. The App Library's white pods
stayed with the coverage division (boot 32), so the white is not made by the division alone.
`glvbdump=3` (boot 34) showed the blur innocent: dark in, dark out, opaque. The white comes after
it. `glvbafter=N[,K]` follows what becomes of the strip: for N frames that have a "vrb" draw (from
the Kth such frame on), each draw after the vrb that lands on the rectangle the vrb drew, or reads
a texture holding it (where that draw lands is followed in turn), gets lines `a34gl-vb:`. They give
CoreAnimation's function, its program (with that program's fragment shader the first time) or which
of ours drew it, the framebuffer and the box in pixels, the textures read (`*`: one followed), the
blending, the first corner's colour, and sixteen pixels across the middle row of the box, before and
after the draw (hex RGBA as the framebuffer holds them). Clears over a followed rectangle are listed
too, and at the frame's end every followed rectangle is read once more. At most 64 draws a frame are
written down, and the pixel reads wait for the GPU, so those frames are slow. A draw that makes
dark samples (no colour above 0x40) bright (none below 0xc0) gets `^ THE FIRST draw that made
dark samples bright here` (then `another`). `glvbafter=N,K,H`: only a vrb whose rectangle is at
least H pixels tall starts a frame followed (default 100: the status bar's is 80, the App
Library's 238).

### Profiling: `glprof=N`

Every N frames (`glprof=1`: 60) four lines `a34gl-prof:` give, a frame's worth on average:
wall time from the frame's first draw to its end, the drawing thread's CPU time in that span,
the time inside our GL entries, and backboardd's CPU time over all its threads (the
difference to the entries is CoreAnimation's own work between calls). Then the entry time by
kind (draw, uniform, texture, framebuffer, program, flush, query, state) with call counts; and
what eagl.c does inside those entries, timed apart: Mesa's draw calls themselves, IOSurface
sums, CPU uploads, GPU copies in, copies out, EGLImages made, IOSurfaceLock/Unlock,
`eglMakeCurrent`, `glGetError` checks, shader compiles and program links. Then the requests to
the kernel, counted by redwood-drm by selector (`submit` -- synchronous, so its time is the
GPU's --, `create_bo`, `gem_close`, `madvise`, `bo_offset`, `iosurface` (0x7e), client memory
maps), and the threads that called GL with their calls a frame. Off, it costs a test per call.

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
into A and checks A's memory; (4) moves the context to a second thread and back (patch 0004:
the second thread takes it over and draws, the first takes it back and checks, the second,
stale now, lets go, and the first draws again). `PASS` / exit 0; 1 wrong pixels; 2 no EGL/GLES; 3 the surface could
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
