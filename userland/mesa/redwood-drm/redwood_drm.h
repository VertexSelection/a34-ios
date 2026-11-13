/*
 * What redwood_drm.c adds to libdrm's API (xf86drm.h).
 *
 * A buffer's memory in this process: on Linux Mesa mmap()s the DRM file at the offset
 * DRM_IOCTL_PANFROST_MMAP_BO gives; on Darwin there is no such file, and the kernel driver
 * hands the buffer over as the IOKit client memory whose type is the buffer's GEM handle
 * (IOConnectMapMemory64). Mesa's panfrost is patched (patches/0002-*) to call
 * redwood_drm_mmap()/redwood_drm_munmap() instead of mmap()/munmap().
 *
 * An IOSurface as a buffer (selector 0x7e, ios-topaz's "surface" request, layout and
 * semantics unchanged): Linux imports a dma-buf by file descriptor; here the "file
 * descriptor" is an IOSurface ID. drmPrimeFDToHandle(fd, IOSurfaceID, &handle) asks the
 * kernel for a GEM handle of the surface's memory; redwood_drm_surface() asks it only to
 * describe the surface (patches/0003-*: EGL_IOSURFACE_A34 images, the BO size of an import).
 */
#ifndef REDWOOD_DRM_H
#define REDWOOD_DRM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* As mmap(addr, length, prot, flags, fd, <offset of handle>): MAP_FAILED and errno on failure. */
void *redwood_drm_mmap(void *addr, size_t length, int prot, int flags, int fd, uint32_t handle);

/* As munmap(): a mapping redwood_drm_mmap() made goes back to the kernel driver. */
int redwood_drm_munmap(void *addr, size_t length);

/*
 * RedwoodMali selector 0x7e: the memory of an IOSurface as a buffer. 40 bytes, in and out
 * (IOConnectCallStructMethod, the structure goes in and comes back):
 *
 *   in   surface      the IOSurface's ID (IOSurfaceGetID)
 *   in   handle       REDWOOD_DRM_SURFACE_DESCRIBE: describe only, no buffer is made
 *                     (comes back 0); anything else (0): make the buffer
 *   out  handle       the GEM handle of the surface's memory. A surface that is a buffer of
 *                     this client's already is that same handle again, as Linux's PRIME
 *                     imports are; the buffer holds the surface until GEM_CLOSE (0x09)
 *   out  size         the buffer's size in bytes: the surface's allocation, page-rounded
 *   out  offset       where the pixels start in the buffer (bytes)
 *   out  row_bytes    the distance from one row of pixels to the next (bytes)
 *   out  width, height, pixel_format (the IOSurface's FourCC, e.g. 'BGRA' 0x42475241),
 *        planes (IOSurfaceGetPlaneCount: 0 for an unplanar surface)
 *
 * The handle is an ordinary BO: PANFROST_GET_BO_OFFSET (0x45) gives its GPU address, its
 * client memory of type = handle maps it, it may be named in a SUBMIT's bo_handles, and
 * WAIT_BO / MADVISE / GEM_CLOSE take it. Errors: kIOReturnNotFound (ENOENT) no such surface,
 * kIOReturnUnsupported (ENOTTY) the surface's memory cannot be the GPU's, kIOReturnNoMemory.
 */
#define REDWOOD_DRM_SURFACE             0x7e
#define REDWOOD_DRM_SURFACE_DESCRIBE    0xffffffffu

struct redwood_drm_surface {
	uint32_t surface;                       /* the IOSurface's ID */
	uint32_t handle;                        /* (the kernel's: which of the two is asked) */
	uint64_t size;                          /* back: how big the buffer is, */
	uint32_t offset, row_bytes;             /* where the pixels start in it and how long a row of them is, */
	uint32_t width, height, pixel_format, planes;   /* and what they are */
};

#ifdef __cplusplus
#define REDWOOD_DRM_ASSERT static_assert
#else
#define REDWOOD_DRM_ASSERT _Static_assert
#endif
REDWOOD_DRM_ASSERT(sizeof(struct redwood_drm_surface) == 40, "selector 0x7e takes 40 bytes");
REDWOOD_DRM_ASSERT(offsetof(struct redwood_drm_surface, size) == 8, "0x7e layout");
REDWOOD_DRM_ASSERT(offsetof(struct redwood_drm_surface, offset) == 16, "0x7e layout");
REDWOOD_DRM_ASSERT(offsetof(struct redwood_drm_surface, width) == 24, "0x7e layout");
REDWOOD_DRM_ASSERT(offsetof(struct redwood_drm_surface, planes) == 36, "0x7e layout");
#undef REDWOOD_DRM_ASSERT

/*
 * What the memory of an IOSurface would be like as a buffer (`surface->surface` in, the
 * rest out; no buffer is made): 0, or -1 with errno.
 */
int redwood_drm_surface(int fd, struct redwood_drm_surface *surface);

/*
 * Requests answered here instead of by the kernel (REDWOOD_DRM_LOCAL, default 1; 0: none):
 *
 *   WAIT_BO   The kernel runs a job chain to its end inside SUBMIT, so a buffer of this
 *             process's own is idle whenever none of the process's threads is inside a
 *             SUBMIT: then the answer is "ready" at once. With a SUBMIT in flight, and for
 *             an IOSurface's buffer (0x7e: other programs' jobs may use it), the kernel is
 *             asked as before.
 *   MADVISE   The kernel never purges; DONTNEED only tells it to leave a cacheable buffer
 *             out of its per-job CPU cache upkeep while Mesa has no use for it. A buffer the
 *             CPU maps write-combined gets no upkeep at all, and for it MADVISE means
 *             nothing: answered here ("retained") -- for the buffers the kernel has said are
 *             such, with REDWOOD_DRM_CREATE_BO_WC in the `pad` that comes back from
 *             CREATE_BO (0x42). A kernel that does not say so (pad 0, as Linux leaves it)
 *             gets every MADVISE, as before. REDWOOD_DRM_LOCAL=2, for a measurement: every
 *             buffer made here is taken to be write-combined (a cacheable one then stays in
 *             the kernel's upkeep while it sits in Mesa's cache: slower, never wrong).
 */
#define REDWOOD_DRM_CREATE_BO_WC        0x1u

/*
 * The requests made of the kernel so far, for profiling (liba34gl's glprof): calls and time
 * (mach_absolute_time units) by selector -- 0x40 SUBMIT, whose time is the GPU's, the job chain
 * running to its end before it returns; 0x42 CREATE_BO; 0x09 GEM_CLOSE; ... -- and the client
 * memory mappings made and taken away. Counted always (two clock reads a request).
 */
struct redwood_drm_stats {
	uint64_t calls[256];
	uint64_t ticks[256];
	uint64_t maps, map_ticks;
	uint64_t unmaps, unmap_ticks;
	uint64_t local_waits, local_madvises;   /* WAIT_BO and MADVISE answered here, without the kernel */
};

void redwood_drm_stats(struct redwood_drm_stats *stats);

#ifdef __cplusplus
}
#endif

#endif
