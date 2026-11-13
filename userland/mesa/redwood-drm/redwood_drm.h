/*
 * What redwood_drm.c adds to libdrm's API (xf86drm.h): a buffer's memory in this process.
 * On Linux Mesa mmap()s the DRM file at the offset DRM_IOCTL_PANFROST_MMAP_BO gives; on
 * Darwin there is no such file, and the kernel driver hands the buffer over as the IOKit
 * client memory whose type is the buffer's GEM handle (IOConnectMapMemory64). Mesa's
 * panfrost is patched (patches/0002-*) to call these two instead of mmap()/munmap().
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

#ifdef __cplusplus
}
#endif

#endif
