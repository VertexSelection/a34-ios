/*
 * libdrm for Darwin: the part of libdrm Mesa's panfrost, its EGL and its DRI loader use,
 * carried to the kernel's GPU driver -- the IOKit service RedwoodMali -- instead of a
 * /dev/dri node. Built as libdrm.2.dylib, against libdrm's own xf86drm.h.
 *
 * The kernel side speaks Linux's panfrost DRM uAPI (include/uapi/drm/panfrost_drm.h):
 *   - a request is a call of the user client's struct method whose selector is the
 *     ioctl's number (request & 0xff: 0x09 GEM_CLOSE, 0x40 + n for DRM_PANFROST_*);
 *     its structure, IOCPARM_LEN(request) bytes, goes in and comes back as it is;
 *   - a buffer's memory is the client memory of the type that is its GEM handle
 *     (IOConnectMapMemory64), in place of mmap() on the DRM file;
 *   - a job chain has run to its end when SUBMIT returns.
 * Because of the last, the synchronisation objects Mesa makes never need the kernel:
 * they live here, and a SUBMIT signals its out_sync when it returns.
 *
 * What is answered here and never reaches the kernel: DRM_IOCTL_VERSION (panfrost 1.1,
 * or 1.$REDWOOD_DRM_MINOR), DRM_IOCTL_GET_CAP, device enumeration (one platform device
 * whose render node is /dev/null: any descriptor in the process means the one GPU) and
 * every syncobj call.
 *
 * PRIME: import only, and what is imported is an IOSurface. The "dma-buf file descriptor"
 * Mesa hands drmPrimeFDToHandle() is an IOSurface ID (EGL_IOSURFACE_A34, patches/0003-*),
 * and the kernel makes a GEM handle of the surface's memory: selector 0x7e, ios-topaz's
 * "surface" request (struct redwood_drm_surface in redwood_drm.h). Export is not supported.
 *
 * Environment: REDWOOD_DRM_SERVICE (default "RedwoodMali"), REDWOOD_DRM_MINOR (default
 * 1), REDWOOD_DRM_DEBUG=1 (every call on stderr), REDWOOD_DRM_NODE (default /dev/null).
 *
 * Adapted from ios-topaz's userland/mesa/redwood-drm (MIT, which is based on HackiOS's
 * user space, MIT, Copyright (c) 2026 OOSEX GANG): see ../README.md.
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>

#include "drm-uapi/panfrost_drm.h"

#include "redwood_drm.h"
#include "redwood_iokit.h"

#define EXPORT __attribute__((visibility("default")))

#define DRIVER_NAME             "panfrost"
#define DRIVER_DATE             "20180908"
#define DRIVER_DESC             "panfrost DRM over IOKit (RedwoodMali)"
#define DEFAULT_SERVICE         "RedwoodMali"
#define DEFAULT_NODE            "/dev/null"
#define DEFAULT_MINOR           1

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* settings and logging */

static int debug = -1;

static bool
debugging(void)
{
	if (debug < 0) {
		const char *value = getenv("REDWOOD_DRM_DEBUG");

		debug = value != NULL && *value != '\0' && strcmp(value, "0") != 0;
	}
	return debug > 0;
}

static void __attribute__((format(printf, 1, 2)))
note(const char *format, ...)
{
	va_list args;

	if (!debugging()) {
		return;
	}
	va_start(args, format);
	fputs("redwood-drm: ", stderr);
	vfprintf(stderr, format, args);
	fputc('\n', stderr);
	va_end(args);
}

static const char *
setting(const char *name, const char *otherwise)
{
	const char *value = getenv(name);

	return value != NULL && *value != '\0' ? value : otherwise;
}

static const char *
node_path(void)
{
	return setting("REDWOOD_DRM_NODE", DEFAULT_NODE);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* the connection to the kernel driver: one per process, made on first need */

static pthread_once_t connection_once = PTHREAD_ONCE_INIT;
static io_connect_t connection;
static kern_return_t connection_result = kIOReturnNotFound;

static void
open_connection(void)
{
	const char *name = setting("REDWOOD_DRM_SERVICE", DEFAULT_SERVICE);
	io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching(name));

	if (service == 0) {
		note("no IOKit service \"%s\"", name);
		connection_result = kIOReturnNotFound;
		return;
	}
	connection_result = IOServiceOpen(service, mach_task_self(), 0, &connection);
	IOObjectRelease(service);
	note("IOServiceOpen(\"%s\") = %#x", name, connection_result);
}

static bool
connected(void)
{
	pthread_once(&connection_once, open_connection);
	return connection_result == kIOReturnSuccess;
}

static int
errno_of(kern_return_t result)
{
	switch (result) {
	case kIOReturnSuccess:
		return 0;
	case kIOReturnNoMemory:
	case kIOReturnNoResources:
		return ENOMEM;
	case kIOReturnBadArgument:
		return EINVAL;
	case kIOReturnNotFound:
		return ENOENT;
	case kIOReturnTimeout:
		return ETIMEDOUT;
	case kIOReturnBusy:
	case kIOReturnExclusiveAccess:
		return EBUSY;
	case kIOReturnNotReady:
		return EAGAIN;
	case kIOReturnUnsupported:
		return ENOTTY;
	case kIOReturnNotPermitted:
	case kIOReturnNotPrivileged:
		return EPERM;
	case kIOReturnNoDevice:
		return ENODEV;
	case kIOReturnVMError:
		return EFAULT;
	default:
		return EIO;
	}
}

/* A request to the kernel driver: 0, or -1 with errno set. */
static int
call(unsigned selector, void *data, size_t size)
{
	size_t returned = size;
	kern_return_t result;

	if (!connected()) {
		errno = ENODEV;
		return -1;
	}
	result = IOConnectCallStructMethod(connection, selector, data, size, data, &returned);
	note("call %#04x (%zu bytes) = %#x", selector, size, result);
	if (result != kIOReturnSuccess) {
		errno = errno_of(result);
		return -1;
	}
	return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* synchronisation objects: here, not in the kernel (jobs have finished when SUBMIT returns) */

struct syncobj {
	bool used;
	bool signaled;
};

static pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sync_changed = PTHREAD_COND_INITIALIZER;
static struct syncobj *syncobjs;        /* handle n is syncobjs[n - 1] */
static uint32_t syncobj_count;

/* With sync_lock held. */
static struct syncobj *
syncobj(uint32_t handle)
{
	if (handle == 0 || handle > syncobj_count || !syncobjs[handle - 1].used) {
		return NULL;
	}
	return &syncobjs[handle - 1];
}

static int
syncobj_create(bool signaled, uint32_t *handle)
{
	uint32_t i;

	pthread_mutex_lock(&sync_lock);
	for (i = 0; i < syncobj_count && syncobjs[i].used; i++) {
	}
	if (i == syncobj_count) {
		uint32_t count = syncobj_count ? syncobj_count * 2 : 64;
		struct syncobj *grown = realloc(syncobjs, count * sizeof(*grown));

		if (grown == NULL) {
			pthread_mutex_unlock(&sync_lock);
			errno = ENOMEM;
			return -1;
		}
		memset(grown + syncobj_count, 0, (count - syncobj_count) * sizeof(*grown));
		syncobjs = grown;
		syncobj_count = count;
	}
	syncobjs[i].used = true;
	syncobjs[i].signaled = signaled;
	*handle = i + 1;
	pthread_mutex_unlock(&sync_lock);
	return 0;
}

/* Signal (or reset) every valid handle of `handles`: 0, or -1 and ENOENT if one is not a syncobj. */
static int
syncobj_set(const uint32_t *handles, uint32_t count, bool signaled)
{
	int ret = 0;

	pthread_mutex_lock(&sync_lock);
	for (uint32_t i = 0; i < count; i++) {
		struct syncobj *s = syncobj(handles[i]);

		if (s == NULL) {
			ret = -1;
		} else {
			s->signaled = signaled;
		}
	}
	if (signaled) {
		pthread_cond_broadcast(&sync_changed);
	}
	pthread_mutex_unlock(&sync_lock);
	if (ret != 0) {
		errno = ENOENT;
	}
	return ret;
}

static int64_t
monotonic_ns(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

/*
 * Linux's DRM_IOCTL_SYNCOBJ_WAIT: `deadline` is absolute, on CLOCK_MONOTONIC. A syncobj
 * nothing was submitted to is an error unless WAIT_FOR_SUBMIT asks to wait for one.
 */
static int
syncobj_wait(const uint32_t *handles, unsigned count, int64_t deadline, unsigned flags, uint32_t *first)
{
	bool all = flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
	bool for_submit = flags & (DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE);
	int error = 0;

	pthread_mutex_lock(&sync_lock);
	for (;;) {
		unsigned done = 0, first_done = count;

		for (unsigned i = 0; i < count; i++) {
			struct syncobj *s = syncobj(handles[i]);

			if (s == NULL) {
				error = ENOENT;
				break;
			}
			if (s->signaled) {
				done++;
				first_done = first_done < i ? first_done : i;
			} else if (!for_submit) {
				error = EINVAL;
				break;
			}
		}
		if (error != 0) {
			break;
		}
		if (count == 0 || (all ? done == count : done > 0)) {
			if (first != NULL) {
				*first = first_done < count ? first_done : 0;
			}
			break;
		}

		int64_t left = deadline - monotonic_ns();

		if (left <= 0) {
			error = ETIME;
			break;
		}
		if (deadline == INT64_MAX || left > 3600ll * 1000000000) {
			left = 3600ll * 1000000000;
		}
		struct timespec relative = { (time_t)(left / 1000000000), (long)(left % 1000000000) };

		pthread_cond_timedwait_relative_np(&sync_changed, &sync_lock, &relative);
	}
	pthread_mutex_unlock(&sync_lock);
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

/* A sync_file that has signalled: a pipe whose read end is readable (poll(POLLIN) returns at once). */
static int
signaled_sync_file(void)
{
	int ends[2];
	char byte = 0;

	if (pipe(ends) != 0) {
		return -1;
	}
	(void)write(ends[1], &byte, 1);
	close(ends[1]);
	fcntl(ends[0], F_SETFD, FD_CLOEXEC);
	return ends[0];
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* requests */

static drmVersionPtr make_version(void);

static unsigned
driver_minor(void)
{
	return (unsigned)atoi(setting("REDWOOD_DRM_MINOR", "1"));
}

static void
copy_string(char *to, __kernel_size_t *length, const char *from)
{
	size_t n = strlen(from);

	if (to != NULL && *length > 0) {
		memcpy(to, from, n < *length ? n : *length);
	}
	*length = n;
}

static int
get_cap(uint64_t capability, uint64_t *value)
{
	switch (capability) {
	case DRM_CAP_SYNCOBJ:
		*value = 1;
		return 0;
	case DRM_CAP_SYNCOBJ_TIMELINE:
		*value = 0;
		return 0;
	case DRM_CAP_PRIME:             /* IOSurfaces in (drmPrimeFDToHandle), nothing out */
		*value = DRM_PRIME_CAP_IMPORT;
		return 0;
	case DRM_CAP_TIMESTAMP_MONOTONIC:
		*value = 1;
		return 0;
	default:
		*value = 0;
		errno = EINVAL;
		return -1;
	}
}

/* DRM_IOCTL_PANFROST_SUBMIT: the kernel is given no syncobjs (they are this file's), and runs the chain to its end. */
static int
submit(unsigned selector, void *data, size_t size)
{
	struct drm_panfrost_submit request;
	uint32_t out_sync;
	int ret;

	if (size < offsetof(struct drm_panfrost_submit, out_sync) + sizeof(request.out_sync) ||
	    size > sizeof(request)) {
		errno = EINVAL;
		return -1;
	}
	memset(&request, 0, sizeof(request));
	memcpy(&request, data, size);
	if (request.in_sync_count != 0) {
		/* every in-sync was signalled when the job that made it returned; check they exist */
		const uint32_t *in = (const uint32_t *)(uintptr_t)request.in_syncs;

		if (syncobj_wait(in, request.in_sync_count, 0, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, NULL) != 0) {
			note("submit: in-sync not signalled (errno %d)", errno);
		}
	}
	out_sync = request.out_sync;
	request.in_syncs = 0;
	request.in_sync_count = 0;
	request.out_sync = 0;
	ret = call(selector, &request, size);
	if (ret == 0 && out_sync != 0) {
		syncobj_set(&out_sync, 1, true);
	}
	return ret;
}

EXPORT int
drmIoctl(int fd, unsigned long request, void *arg)
{
	unsigned nr = (unsigned)(request & 0xff);
	size_t size = IOCPARM_LEN(request);

	(void)fd;
	if (nr >= DRM_COMMAND_BASE && nr < DRM_COMMAND_END) {
		if (nr == DRM_COMMAND_BASE + DRM_PANFROST_SUBMIT) {
			return submit(nr, arg, size);
		}
		return call(nr, arg, size);
	}
	switch (request) {
	case DRM_IOCTL_GEM_CLOSE:
		return call(nr, arg, size);
	case DRM_IOCTL_VERSION: {
		struct drm_version *v = arg;

		v->version_major = 1;
		v->version_minor = (int)driver_minor();
		v->version_patchlevel = 0;
		copy_string(v->name, &v->name_len, DRIVER_NAME);
		copy_string(v->date, &v->date_len, DRIVER_DATE);
		copy_string(v->desc, &v->desc_len, DRIVER_DESC);
		return 0;
	}
	case DRM_IOCTL_GET_CAP: {
		struct drm_get_cap *cap = arg;
		uint64_t value;
		int ret = get_cap(cap->capability, &value);

		cap->value = value;
		return ret;
	}
	default:
		note("ioctl %#lx (nr %#x) is not supported", request, nr);
		errno = EINVAL;
		return -1;
	}
}

static int
command(unsigned long index, void *data, unsigned long size)
{
	unsigned nr = (unsigned)(DRM_COMMAND_BASE + index);

	if (nr >= DRM_COMMAND_END) {
		return -EINVAL;
	}
	if (nr == DRM_COMMAND_BASE + DRM_PANFROST_SUBMIT ? submit(nr, data, size) : call(nr, data, size)) {
		return -errno;
	}
	return 0;
}

EXPORT int
drmCommandNone(int fd, unsigned long index)
{
	(void)fd;
	return command(index, NULL, 0);
}

EXPORT int
drmCommandRead(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	return command(index, data, size);
}

EXPORT int
drmCommandWrite(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	return command(index, data, size);
}

EXPORT int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	return command(index, data, size);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* the driver */

static drmVersionPtr
make_version(void)
{
	drmVersionPtr v = calloc(1, sizeof(*v));

	if (v == NULL) {
		return NULL;
	}
	v->version_major = 1;
	v->version_minor = (int)driver_minor();
	v->version_patchlevel = 0;
	v->name = strdup(DRIVER_NAME);
	v->name_len = (int)strlen(DRIVER_NAME);
	v->date = strdup(DRIVER_DATE);
	v->date_len = (int)strlen(DRIVER_DATE);
	v->desc = strdup(DRIVER_DESC);
	v->desc_len = (int)strlen(DRIVER_DESC);
	return v;
}

EXPORT drmVersionPtr
drmGetVersion(int fd)
{
	(void)fd;
	return make_version();
}

EXPORT drmVersionPtr
drmGetLibVersion(int fd)
{
	drmVersionPtr v = calloc(1, sizeof(*v));

	(void)fd;
	if (v != NULL) {
		v->version_major = 1;
		v->version_minor = 3;
		v->version_patchlevel = 0;
	}
	return v;
}

EXPORT void
drmFreeVersion(drmVersionPtr v)
{
	if (v != NULL) {
		free(v->name);
		free(v->date);
		free(v->desc);
		free(v);
	}
}

EXPORT int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
	return get_cap(capability, value);
}

EXPORT int
drmSetClientCap(int fd, uint64_t capability, uint64_t value)
{
	(void)fd;
	(void)capability;
	(void)value;
	errno = EINVAL;
	return -1;
}

EXPORT int
drmGetMagic(int fd, drm_magic_t *magic)
{
	(void)fd;
	*magic = 0;
	errno = EACCES;                 /* a render node has no magic */
	return -1;
}

EXPORT int
drmAuthMagic(int fd, drm_magic_t magic)
{
	(void)fd;
	(void)magic;
	errno = EACCES;
	return -1;
}

EXPORT int
drmIsMaster(int fd)
{
	(void)fd;
	return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* opening and naming the device */

static int
open_node(void)
{
	return open(node_path(), O_RDWR | O_CLOEXEC);
}

EXPORT int
drmOpenWithType(const char *name, const char *busid, int type)
{
	(void)busid;
	(void)type;
	if (name != NULL && strcmp(name, DRIVER_NAME) != 0) {
		errno = ENODEV;
		return -1;
	}
	if (!connected()) {
		errno = ENODEV;
		return -1;
	}
	return open_node();
}

EXPORT int
drmOpen(const char *name, const char *busid)
{
	return drmOpenWithType(name, busid, DRM_NODE_PRIMARY);
}

EXPORT int
drmOpenRender(int minor)
{
	(void)minor;
	return drmOpenWithType(DRIVER_NAME, NULL, DRM_NODE_RENDER);
}

EXPORT int
drmOpenControl(int minor)
{
	(void)minor;
	errno = EINVAL;
	return -1;
}

EXPORT int
drmClose(int fd)
{
	return close(fd);
}

EXPORT int
drmGetNodeTypeFromFd(int fd)
{
	(void)fd;
	return DRM_NODE_RENDER;
}

EXPORT char *
drmGetDeviceNameFromFd2(int fd)
{
	(void)fd;
	return strdup(node_path());
}

EXPORT char *
drmGetDeviceNameFromFd(int fd)
{
	return drmGetDeviceNameFromFd2(fd);
}

EXPORT char *
drmGetRenderDeviceNameFromFd(int fd)
{
	return drmGetDeviceNameFromFd2(fd);
}

EXPORT char *
drmGetPrimaryDeviceNameFromFd(int fd)
{
	(void)fd;
	return NULL;
}

/*
 * The one device: on the platform bus (as panfrost's GPUs are on Linux), with a render
 * node only. Everything is in one allocation, freed by drmFreeDevice().
 */
struct device_block {
	drmDevice device;
	char *nodes[DRM_NODE_MAX];
	char node[DRM_NODE_MAX][64];
	drmPlatformBusInfo bus;
	drmPlatformDeviceInfo info;
	char *compatible[2];
	char compatible_name[32];
};

static drmDevicePtr
make_device(void)
{
	struct device_block *b = calloc(1, sizeof(*b));

	if (b == NULL) {
		return NULL;
	}
	snprintf(b->node[DRM_NODE_RENDER], sizeof(b->node[0]), "%s", node_path());
	b->nodes[DRM_NODE_RENDER] = b->node[DRM_NODE_RENDER];
	b->device.nodes = b->nodes;
	b->device.available_nodes = 1 << DRM_NODE_RENDER;
	b->device.bustype = DRM_BUS_PLATFORM;
	snprintf(b->bus.fullname, sizeof(b->bus.fullname), "/IOResources/%s",
	    setting("REDWOOD_DRM_SERVICE", DEFAULT_SERVICE));
	b->device.businfo.platform = &b->bus;
	snprintf(b->compatible_name, sizeof(b->compatible_name), "arm,mali-valhall-jm");
	b->compatible[0] = b->compatible_name;
	b->compatible[1] = NULL;
	b->info.compatible = b->compatible;
	b->device.deviceinfo.platform = &b->info;
	return &b->device;
}

EXPORT void
drmFreeDevice(drmDevicePtr *device)
{
	if (device != NULL && *device != NULL) {
		free(*device);          /* the device is the first member of its block */
		*device = NULL;
	}
}

EXPORT void
drmFreeDevices(drmDevicePtr devices[], int count)
{
	for (int i = 0; devices != NULL && i < count; i++) {
		drmFreeDevice(&devices[i]);
	}
}

EXPORT int
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
	(void)flags;
	if (fd < 0 || fcntl(fd, F_GETFD) == -1) {
		return -EBADF;
	}
	*device = make_device();
	return *device != NULL ? 0 : -ENOMEM;
}

EXPORT int
drmGetDevice(int fd, drmDevicePtr *device)
{
	return drmGetDevice2(fd, 0, device);
}

EXPORT int
drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
	(void)flags;
	if (!connected()) {
		return 0;               /* no GPU: no device, and EGL falls back or fails cleanly */
	}
	if (devices == NULL || max_devices < 1) {
		return 1;
	}
	devices[0] = make_device();
	return devices[0] != NULL ? 1 : -ENOMEM;
}

EXPORT int
drmGetDevices(drmDevicePtr devices[], int max_devices)
{
	return drmGetDevices2(0, devices, max_devices);
}

EXPORT int
drmGetDeviceFromDevId(dev_t dev_id, uint32_t flags, drmDevicePtr *device)
{
	(void)dev_id;
	(void)flags;
	*device = make_device();
	return *device != NULL ? 0 : -ENOMEM;
}

EXPORT int
drmGetNodeTypeFromDevId(dev_t devid)
{
	(void)devid;
	return DRM_NODE_RENDER;
}

EXPORT int
drmDevicesEqual(drmDevicePtr a, drmDevicePtr b)
{
	if (a == NULL || b == NULL || a->bustype != b->bustype) {
		return 0;
	}
	if (a->bustype != DRM_BUS_PLATFORM) {
		return 0;
	}
	return strcmp(a->businfo.platform->fullname, b->businfo.platform->fullname) == 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* buffers */

EXPORT int
drmCloseBufferHandle(int fd, uint32_t handle)
{
	struct drm_gem_close request = { .handle = handle };

	return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &request);
}

/* Buffers as file descriptors (dma-buf) would need the kernel to export them: not here. */
EXPORT int
drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd)
{
	(void)fd;
	(void)handle;
	(void)flags;
	*prime_fd = -1;
	note("drmPrimeHandleToFD: not supported");
	errno = EOPNOTSUPP;
	return -1;
}

/*
 * Selector 0x7e (redwood_drm.h). `prime_fd` is an IOSurface ID: the GEM handle of its memory
 * (the same handle again for a surface this client has already), or with
 * REDWOOD_DRM_SURFACE_DESCRIBE in `handle` only what the surface is like.
 */
static int
surface_request(struct redwood_drm_surface *request)
{
	bool describe = request->handle == REDWOOD_DRM_SURFACE_DESCRIBE;
	int ret = call(REDWOOD_DRM_SURFACE, request, sizeof(*request));
	int error = errno;

	note("surface %u (%s): %s; handle %u, %llu bytes, %ux%u '%c%c%c%c' (%#x), %u planes, %u bytes a row from %u",
	    request->surface, describe ? "describe" : "import", ret == 0 ? "ok" : strerror(error),
	    request->handle, (unsigned long long)request->size, request->width, request->height,
	    (int)(request->pixel_format >> 24 & 0xff), (int)(request->pixel_format >> 16 & 0xff),
	    (int)(request->pixel_format >> 8 & 0xff), (int)(request->pixel_format & 0xff), request->pixel_format,
	    request->planes, request->row_bytes, request->offset);
	errno = error;
	return ret;
}

EXPORT int
drmPrimeFDToHandle(int fd, int prime_fd, uint32_t *handle)
{
	struct redwood_drm_surface request = { .surface = (uint32_t)prime_fd, .handle = 0 };

	(void)fd;
	*handle = 0;
	if (prime_fd <= 0) {
		errno = EBADF;
		return -1;
	}
	if (surface_request(&request) != 0) {
		return -1;
	}
	if (request.handle == 0 || request.handle == REDWOOD_DRM_SURFACE_DESCRIBE) {
		note("surface %u: the kernel gave no handle", request.surface);
		errno = EIO;
		return -1;
	}
	*handle = request.handle;
	return 0;
}

EXPORT int
redwood_drm_surface(int fd, struct redwood_drm_surface *surface)
{
	uint32_t id = surface->surface;

	(void)fd;
	memset(surface, 0, sizeof(*surface));
	surface->surface = id;
	surface->handle = REDWOOD_DRM_SURFACE_DESCRIBE;
	if (id == 0) {
		errno = EBADF;
		return -1;
	}
	return surface_request(surface);
}

struct mapping {
	uintptr_t address;
	uint64_t size;
	uint32_t handle;
};

static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;
static struct mapping *mappings;
static size_t mapping_count, mapping_room;

EXPORT void *
redwood_drm_mmap(void *addr, size_t length, int prot, int flags, int fd, uint32_t handle)
{
	mach_vm_address_t address = (mach_vm_address_t)(uintptr_t)addr;
	mach_vm_size_t size = 0;
	IOOptionBits options = addr == NULL ? kIOMapAnywhere : 0;
	kern_return_t result;

	(void)flags;
	(void)fd;
	if (!(prot & PROT_WRITE)) {
		options |= kIOMapReadOnly;
	}
	if (!connected()) {
		errno = ENODEV;
		return MAP_FAILED;
	}
	result = IOConnectMapMemory64(connection, handle, mach_task_self(), &address, &size, options);
	note("map handle %u (%zu bytes) = %#x at %#llx, %llu bytes", handle, length, result,
	    (unsigned long long)address, (unsigned long long)size);
	if (result != kIOReturnSuccess) {
		errno = errno_of(result);
		return MAP_FAILED;
	}
	if (size < length) {
		IOConnectUnmapMemory64(connection, handle, mach_task_self(), address);
		errno = EINVAL;
		return MAP_FAILED;
	}
	pthread_mutex_lock(&map_lock);
	if (mapping_count == mapping_room) {
		size_t room = mapping_room ? mapping_room * 2 : 256;
		struct mapping *grown = realloc(mappings, room * sizeof(*grown));

		if (grown == NULL) {
			pthread_mutex_unlock(&map_lock);
			IOConnectUnmapMemory64(connection, handle, mach_task_self(), address);
			errno = ENOMEM;
			return MAP_FAILED;
		}
		mappings = grown;
		mapping_room = room;
	}
	mappings[mapping_count++] = (struct mapping) { (uintptr_t)address, size, handle };
	pthread_mutex_unlock(&map_lock);
	return (void *)(uintptr_t)address;
}

EXPORT int
redwood_drm_munmap(void *addr, size_t length)
{
	uintptr_t address = (uintptr_t)addr;
	struct mapping found = { 0, 0, 0 };

	pthread_mutex_lock(&map_lock);
	for (size_t i = mapping_count; i-- > 0;) {
		if (mappings[i].address == address) {
			found = mappings[i];
			mappings[i] = mappings[--mapping_count];
			break;
		}
	}
	pthread_mutex_unlock(&map_lock);
	if (found.address == 0) {
		return munmap(addr, length);    /* not one of ours */
	}
	kern_return_t result = IOConnectUnmapMemory64(connection, found.handle, mach_task_self(), found.address);

	note("unmap handle %u at %#lx = %#x", found.handle, (unsigned long)found.address, result);
	if (result != kIOReturnSuccess) {
		errno = errno_of(result);
		return -1;
	}
	return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* syncobjs (see above) */

EXPORT int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	(void)fd;
	return syncobj_create(flags & DRM_SYNCOBJ_CREATE_SIGNALED, handle);
}

EXPORT int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	struct syncobj *s;

	(void)fd;
	pthread_mutex_lock(&sync_lock);
	s = syncobj(handle);
	if (s != NULL) {
		s->used = false;
		s->signaled = false;
	}
	pthread_mutex_unlock(&sync_lock);
	if (s == NULL) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

static bool
signaled(uint32_t handle, bool *exists)
{
	struct syncobj *s;
	bool result;

	pthread_mutex_lock(&sync_lock);
	s = syncobj(handle);
	*exists = s != NULL;
	result = s != NULL && s->signaled;
	pthread_mutex_unlock(&sync_lock);
	return result;
}

/* A sync file of a syncobj: one that has signalled, as every job has by the time anyone asks. */
EXPORT int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	bool exists;

	(void)fd;
	*sync_file_fd = -1;
	if (!signaled(handle, &exists)) {
		errno = exists ? EINVAL : ENOENT;       /* Linux: a syncobj without a fence cannot be exported */
		return -1;
	}
	*sync_file_fd = signaled_sync_file();
	return *sync_file_fd >= 0 ? 0 : -1;
}

EXPORT int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	(void)fd;
	(void)sync_file_fd;     /* every sync file there is here has signalled */
	return syncobj_set(&handle, 1, true);
}

/* The syncobj itself as a file: here a sync file, imported again as a new syncobj in its state. */
EXPORT int
drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
	bool exists;

	(void)fd;
	*obj_fd = -1;
	(void)signaled(handle, &exists);
	if (!exists) {
		errno = ENOENT;
		return -1;
	}
	*obj_fd = signaled_sync_file();
	return *obj_fd >= 0 ? 0 : -1;
}

EXPORT int
drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
	(void)fd;
	(void)obj_fd;
	return syncobj_create(true, handle);
}

EXPORT int
drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec, unsigned flags,
    uint32_t *first_signaled)
{
	(void)fd;
	return syncobj_wait(handles, num_handles, timeout_nsec, flags, first_signaled);
}

EXPORT int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t handle_count)
{
	(void)fd;
	return syncobj_set(handles, handle_count, false);
}

EXPORT int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t handle_count)
{
	(void)fd;
	return syncobj_set(handles, handle_count, true);
}

/* Timelines are not offered (DRM_CAP_SYNCOBJ_TIMELINE is 0); point 0 is the binary syncobj. */
static bool
binary_points(const uint64_t *points, uint32_t count)
{
	for (uint32_t i = 0; points != NULL && i < count; i++) {
		if (points[i] != 0) {
			errno = EINVAL;
			return false;
		}
	}
	return true;
}

EXPORT int
drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t handle_count)
{
	if (!binary_points(points, handle_count)) {
		return -1;
	}
	return drmSyncobjSignal(fd, handles, handle_count);
}

EXPORT int
drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles, int64_t timeout_nsec,
    unsigned flags, uint32_t *first_signaled)
{
	if (!binary_points(points, num_handles)) {
		return -1;
	}
	return drmSyncobjWait(fd, handles, num_handles, timeout_nsec, flags, first_signaled);
}

EXPORT int
drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count, uint32_t flags)
{
	(void)fd;
	(void)flags;
	for (uint32_t i = 0; i < handle_count; i++) {
		bool exists;

		(void)signaled(handles[i], &exists);
		if (!exists) {
			errno = ENOENT;
			return -1;
		}
		points[i] = 0;
	}
	return 0;
}

EXPORT int
drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t handle_count)
{
	return drmSyncobjQuery2(fd, handles, points, handle_count, 0);
}

EXPORT int
drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point,
    uint32_t flags)
{
	bool exists;
	bool state;

	(void)fd;
	(void)flags;
	if (dst_point != 0 || src_point != 0) {
		errno = EINVAL;
		return -1;
	}
	state = signaled(src_handle, &exists);
	if (!exists) {
		errno = ENOENT;
		return -1;
	}
	return syncobj_set(&dst_handle, 1, state);
}

EXPORT int
drmSyncobjEventfd(int fd, uint32_t handle, uint64_t point, int ev_fd, uint32_t flags)
{
	(void)fd;
	(void)handle;
	(void)point;
	(void)ev_fd;
	(void)flags;
	errno = EOPNOTSUPP;
	return -1;
}
