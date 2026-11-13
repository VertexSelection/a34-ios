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
 * Also answered here, when that is sure to be the kernel's answer (redwood_drm.h,
 * REDWOOD_DRM_LOCAL): WAIT_BO for a buffer of the process's own while none of its SUBMITs
 * is running, and MADVISE for the buffers the kernel has said it keeps no upkeep of.
 *
 * Environment: REDWOOD_DRM_SERVICE (default "RedwoodMali"), REDWOOD_DRM_MINOR (default
 * 1), REDWOOD_DRM_DEBUG=1 (every call on stderr), REDWOOD_DRM_NODE (default /dev/null),
 * REDWOOD_DRM_LOCAL (default 1; 0: every WAIT_BO and MADVISE goes to the kernel; 2: see
 * redwood_drm.h).
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
#include <mach/mach_time.h>

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
static bool queued;                     /* this client's SUBMITs are queued (kernel 0919: redwood_drm.h) */

/*
 * The queued mode, asked for when the connection is made: when the kernel offers it (GET_PARAM
 * REDWOOD_DRM_PARAM_ASYNC = 1) and REDWOOD_DRM_ASYNC is not 0. One line on stderr says which.
 */
static void
ask_queued(const char *service)
{
	struct drm_panfrost_get_param param = { .param = REDWOOD_DRM_PARAM_ASYNC };
	struct redwood_drm_set_mode mode = { .async = 1 };
	size_t size = sizeof(param);
	const char *why;

	if (strcmp(setting("REDWOOD_DRM_ASYNC", "1"), "0") == 0) {
		why = "REDWOOD_DRM_ASYNC=0";
	} else if (IOConnectCallStructMethod(connection, DRM_COMMAND_BASE + DRM_PANFROST_GET_PARAM, &param, sizeof(param), &param,
	    &size) != kIOReturnSuccess || param.value != 1) {
		why = "the kernel does not queue";
	} else if (size = sizeof(mode), IOConnectCallStructMethod(connection, REDWOOD_DRM_SET_MODE, &mode, sizeof(mode), &mode,
	    &size) != kIOReturnSuccess) {
		why = "SET_MODE refused";
	} else {
		queued = true;
		why = NULL;
	}
	fprintf(stderr, "redwood-drm: %s[%d]: %s: GPU jobs %s%s%s\n", getprogname(), (int)getpid(), service,
	    queued ? "queued (SUBMIT returns at once)" : "synchronous", why ? " -- " : "", why ? why : "");
}

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
	if (connection_result == kIOReturnSuccess) {
		ask_queued(name);
	}
}

EXPORT int
redwood_drm_queued(void)
{
	pthread_once(&connection_once, open_connection);
	return queued;
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
	case kIOReturnAborted:
		return EINTR;
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

/* the requests so far (redwood_drm_stats()) */
static struct redwood_drm_stats stats;

static void
count(uint64_t *calls, uint64_t *ticks, uint64_t began)
{
	__atomic_fetch_add(calls, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(ticks, mach_absolute_time() - began, __ATOMIC_RELAXED);
}

EXPORT void
redwood_drm_stats(struct redwood_drm_stats *out)
{
	for (unsigned i = 0; i < 256; i++) {
		out->calls[i] = __atomic_load_n(&stats.calls[i], __ATOMIC_RELAXED);
		out->ticks[i] = __atomic_load_n(&stats.ticks[i], __ATOMIC_RELAXED);
	}
	out->maps = __atomic_load_n(&stats.maps, __ATOMIC_RELAXED);
	out->map_ticks = __atomic_load_n(&stats.map_ticks, __ATOMIC_RELAXED);
	out->unmaps = __atomic_load_n(&stats.unmaps, __ATOMIC_RELAXED);
	out->unmap_ticks = __atomic_load_n(&stats.unmap_ticks, __ATOMIC_RELAXED);
	out->local_waits = __atomic_load_n(&stats.local_waits, __ATOMIC_RELAXED);
	out->local_madvises = __atomic_load_n(&stats.local_madvises, __ATOMIC_RELAXED);
	out->queued_jobs = __atomic_load_n(&stats.queued_jobs, __ATOMIC_RELAXED);
	out->failed_jobs = __atomic_load_n(&stats.failed_jobs, __ATOMIC_RELAXED);
}

/* A request to the kernel driver: 0, or -1 with errno set. */
static int
call(unsigned selector, void *data, size_t size)
{
	size_t returned = size;
	kern_return_t result;
	uint64_t began;

	if (!connected()) {
		errno = ENODEV;
		return -1;
	}
	began = mach_absolute_time();
	result = IOConnectCallStructMethod(connection, selector, data, size, data, &returned);
	count(&stats.calls[selector & 0xff], &stats.ticks[selector & 0xff], began);
	note("call %#04x (%zu bytes) = %#x", selector, size, result);
	if (result != kIOReturnSuccess) {
		errno = errno_of(result);
		return -1;
	}
	return 0;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*
 * Synchronisation objects: here, not in the kernel. A syncobj holds a fence or not; the fence
 * is the number of the job whose end signals it (seq), 0 for one that is signalled already.
 * Synchronous (every job has ended when its SUBMIT returns), every fence is 0. Queued (kernel
 * 0919), a SUBMIT's out_sync gets the job's number, and a fence is signalled once the kernel
 * has said that every job of this client up to its number is done: WAIT_SEQ, which also
 * waits. `known_done` is the highest such number. The kernel runs a client's jobs one at a
 * time in the order they were queued and says "done up to" only for an unbroken run of them,
 * so a job done before one ahead of it (should that ever be) does not count before that one.
 */

struct syncobj {
	bool used;
	bool has_fence;
	uint64_t seq;
};

static pthread_mutex_t sync_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sync_changed = PTHREAD_COND_INITIALIZER;
static struct syncobj *syncobjs;        /* handle n is syncobjs[n - 1] */
static uint32_t syncobj_count;
static uint64_t known_done;             /* (sync_lock) every job of ours up to this number is done */

/* With sync_lock held. */
static struct syncobj *
syncobj(uint32_t handle)
{
	if (handle == 0 || handle > syncobj_count || !syncobjs[handle - 1].used) {
		return NULL;
	}
	return &syncobjs[handle - 1];
}

/* With sync_lock held. */
static bool
fence_signaled(const struct syncobj *s)
{
	return s->has_fence && s->seq <= known_done;
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
	syncobjs[i] = (struct syncobj){ .used = true, .has_fence = signaled, .seq = 0 };
	*handle = i + 1;
	pthread_mutex_unlock(&sync_lock);
	return 0;
}

/*
 * Give every valid handle of `handles` the fence `seq` (has_fence) or none: 0, or -1 and ENOENT
 * if one is not a syncobj.
 */
static int
syncobj_set_fence(const uint32_t *handles, uint32_t count, bool has_fence, uint64_t seq)
{
	int ret = 0;

	pthread_mutex_lock(&sync_lock);
	for (uint32_t i = 0; i < count; i++) {
		struct syncobj *s = syncobj(handles[i]);

		if (s == NULL) {
			ret = -1;
		} else {
			s->has_fence = has_fence;
			s->seq = has_fence ? seq : 0;
		}
	}
	if (has_fence) {
		pthread_cond_broadcast(&sync_changed);
	}
	pthread_mutex_unlock(&sync_lock);
	if (ret != 0) {
		errno = ENOENT;
	}
	return ret;
}

static int
syncobj_set(const uint32_t *handles, uint32_t count, bool signaled)
{
	return syncobj_set_fence(handles, count, signaled, 0);
}

static int64_t
monotonic_ns(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

/*
 * WAIT_SEQ (0x7b): until every job of this client up to `seq` is done, at most until the absolute
 * CLOCK_MONOTONIC `deadline` (<= now: only asked; INT64_MAX: no limit). 0 and known_done raised;
 * -1 with errno ETIME (not done in time) or another. A failed job is said once on stderr.
 * Called without sync_lock (it may sleep in the kernel); takes it to note what it learnt.
 */
static int
wait_seq(uint64_t seq, int64_t deadline)
{
	struct redwood_drm_wait_seq request = { .seq = seq };
	int64_t left;
	int ret;

	for (;;) {
		left = deadline == INT64_MAX ? -1 : deadline - monotonic_ns();
		request.seq = seq;
		request.timeout_ns = deadline == INT64_MAX ? UINT64_MAX : left > 0 ? (uint64_t)left : 0;
		request.status = 0;
		ret = call(REDWOOD_DRM_WAIT_SEQ, &request, sizeof(request));
		if (ret == 0 || errno != EINTR) {
			break;
		}
		/* (interrupted: asked again) */
	}
	if (ret != 0) {
		if (errno == ETIMEDOUT) {
			errno = ETIME;
		}
		return -1;
	}
	if (request.status != 0) {
		__atomic_fetch_add(&stats.failed_jobs, 1, __ATOMIC_RELAXED);
		fprintf(stderr, "redwood-drm: %s[%d]: a GPU job up to number %llu failed (the kernel recovered; its picture may be "
		    "wrong)\n", getprogname(), (int)getpid(), (unsigned long long)seq);
	}
	pthread_mutex_lock(&sync_lock);
	if (seq > known_done) {
		known_done = seq;
		pthread_cond_broadcast(&sync_changed);
	}
	pthread_mutex_unlock(&sync_lock);
	return 0;
}

/*
 * Linux's DRM_IOCTL_SYNCOBJ_WAIT: `deadline` is absolute, on CLOCK_MONOTONIC. A syncobj
 * nothing was submitted to is an error unless WAIT_FOR_SUBMIT asks to wait for one. A fence of
 * a queued job is waited for in the kernel (WAIT_SEQ): for all of them, the last; for any one,
 * the first -- jobs end in the order they were queued.
 */
static int
syncobj_wait(const uint32_t *handles, unsigned count, int64_t deadline, unsigned flags, uint32_t *first)
{
	bool all = flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL;
	bool for_submit = flags & (DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE);
	bool available = flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE;
	int error = 0;

	pthread_mutex_lock(&sync_lock);
	for (;;) {
		unsigned done = 0, first_done = count, unsubmitted = 0;
		uint64_t lowest = UINT64_MAX, highest = 0;

		for (unsigned i = 0; i < count; i++) {
			struct syncobj *s = syncobj(handles[i]);

			if (s == NULL) {
				error = ENOENT;
				break;
			}
			if (fence_signaled(s) || (available && s->has_fence)) {
				done++;
				first_done = first_done < i ? first_done : i;
			} else if (s->has_fence) {
				lowest = s->seq < lowest ? s->seq : lowest;
				highest = s->seq > highest ? s->seq : highest;
			} else if (!for_submit) {
				error = EINVAL;
				break;
			} else {
				unsubmitted++;
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
		if (highest != 0 && (!all || unsubmitted == 0)) {
			/* queued jobs to wait for, in the kernel */
			uint64_t target = all ? highest : lowest;

			pthread_mutex_unlock(&sync_lock);
			if (wait_seq(target, deadline) != 0) {
				error = errno;
				pthread_mutex_lock(&sync_lock);
				break;
			}
			pthread_mutex_lock(&sync_lock);
			continue;
		}

		/* a syncobj nothing has been submitted to yet (WAIT_FOR_SUBMIT): until one is */
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

/*
 * Sync files: pipes. One of a fence that has signalled is readable at once (poll(POLLIN)); one
 * of a queued job's fence is not written to (its write end is closed: a poll() sees the end of
 * the file, not the job's -- nothing here polls them). Which fence a pipe stands for is kept by
 * the pipe's inode, so that importing it again (Mesa's fences go through a sync file: a
 * syncobj exported and imported into another) gives the same fence.
 */
#define SYNC_FILES      64

static struct {
	dev_t dev;
	ino_t ino;
	uint64_t seq;
} sync_files[SYNC_FILES];               /* (sync_lock) */
static unsigned sync_file_next;

static int
fence_sync_file(uint64_t seq)
{
	struct stat st;
	int ends[2];
	char byte = 0;
	bool signaled;

	if (pipe(ends) != 0) {
		return -1;
	}
	pthread_mutex_lock(&sync_lock);
	signaled = seq <= known_done;
	if (!signaled && fstat(ends[0], &st) == 0) {
		unsigned i = sync_file_next++ % SYNC_FILES;

		sync_files[i].dev = st.st_dev;
		sync_files[i].ino = st.st_ino;
		sync_files[i].seq = seq;
	}
	pthread_mutex_unlock(&sync_lock);
	if (signaled) {
		(void)write(ends[1], &byte, 1);
	}
	close(ends[1]);
	fcntl(ends[0], F_SETFD, FD_CLOEXEC);
	return ends[0];
}

/* The fence a sync file stands for (0: signalled, or not one of ours). */
static uint64_t
sync_file_seq(int fd)
{
	struct stat st;
	uint64_t seq = 0;

	if (fd < 0 || fstat(fd, &st) != 0) {
		return 0;
	}
	pthread_mutex_lock(&sync_lock);
	for (unsigned i = 0; i < SYNC_FILES; i++) {
		if (sync_files[i].seq != 0 && sync_files[i].dev == st.st_dev && sync_files[i].ino == st.st_ino) {
			seq = sync_files[i].seq;
			break;
		}
	}
	pthread_mutex_unlock(&sync_lock);
	return seq;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*
 * What is known of a GEM handle, for the requests answered here: HANDLE_SURFACE, an IOSurface's
 * memory (0x7e); HANDLE_WC, a buffer the kernel keeps no CPU cache upkeep of; and (queued) the
 * number of the last job of ours that named it. Forgotten at GEM_CLOSE. Handles past the table
 * are unknown (0): everything about them goes to the kernel.
 */
#define HANDLE_SURFACE  1u
#define HANDLE_WC       2u
#define HANDLES         65536u
#define HANDLE_SEQS     8192u           /* (the kernel gives a client handles 1..4096) */

static uint8_t handle_kinds[HANDLES];
static uint64_t handle_seqs[HANDLE_SEQS];
static int submits_running;             /* threads of this process inside a SUBMIT */

static unsigned
handle_kind(uint32_t handle)
{
	return handle < HANDLES ? __atomic_load_n(&handle_kinds[handle], __ATOMIC_RELAXED) : 0;
}

static void
set_handle_kind(uint32_t handle, unsigned kind)
{
	if (handle < HANDLES) {
		__atomic_store_n(&handle_kinds[handle], (uint8_t)kind, __ATOMIC_RELAXED);
	}
	if (handle < HANDLE_SEQS) {
		__atomic_store_n(&handle_seqs[handle], 0, __ATOMIC_RELAXED);
	}
}

/* (queued) the last job of ours that named the handle; UINT64_MAX: not known */
static uint64_t
handle_seq(uint32_t handle)
{
	return handle < HANDLE_SEQS ? __atomic_load_n(&handle_seqs[handle], __ATOMIC_RELAXED) : UINT64_MAX;
}

static void
note_handle_seq(uint32_t handle, uint64_t seq)
{
	if (handle < HANDLE_SEQS) {
		__atomic_store_n(&handle_seqs[handle], seq, __ATOMIC_RELAXED);
	}
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

/* REDWOOD_DRM_LOCAL: 0 nothing is answered here; 1 (default); 2 every buffer made here taken to be write-combined */
static int
local_answers(void)
{
	static int mode = -1;

	if (mode < 0) {
		mode = atoi(setting("REDWOOD_DRM_LOCAL", "1"));
	}
	return mode;
}

/*
 * DRM_IOCTL_PANFROST_SUBMIT. The kernel is given no syncobjs: they are this file's.
 *   synchronous  the chain has run to its end when the call returns: out_sync is signalled.
 *   queued       the call returns at once with the job's number in in_syncs; out_sync gets it
 *                as its fence, and every buffer the job names notes it (WAIT_BO). in_syncs need
 *                no wait: the kernel runs this client's jobs one at a time in the order they
 *                were queued, so a job queued before this one ends before it starts.
 */
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
	if (request.in_sync_count != 0 && !queued) {
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
	__atomic_fetch_add(&submits_running, 1, __ATOMIC_SEQ_CST);
	ret = call(selector, &request, size);
	__atomic_fetch_sub(&submits_running, 1, __ATOMIC_SEQ_CST);
	if (ret != 0) {
		return ret;
	}
	if (queued) {
		uint64_t seq = request.in_syncs;
		const uint32_t *bos = (const uint32_t *)(uintptr_t)request.bo_handles;

		__atomic_fetch_add(&stats.queued_jobs, 1, __ATOMIC_RELAXED);
		for (uint32_t i = 0; bos != NULL && i < request.bo_handle_count; i++) {
			note_handle_seq(bos[i], seq);
		}
		if (out_sync != 0) {
			syncobj_set_fence(&out_sync, 1, true, seq);
		}
		note("submit: job %llu queued", (unsigned long long)seq);
	} else if (out_sync != 0) {
		syncobj_set(&out_sync, 1, true);
	}
	return 0;
}

/*
 * WAIT_BO when this client's jobs are queued: a buffer the jobs name is idle once the last of
 * them is done. Known here (it is not an IOSurface's, which other programs' jobs may name, and
 * its handle is in the table): answered without the kernel when that job is known to be done,
 * else by WAIT_SEQ for it (which teaches known_done as well). Otherwise the kernel's WAIT_BO.
 * timeout_ns is Linux's: absolute on CLOCK_MONOTONIC, <= 0 only asking (EBUSY while busy).
 */
static int
wait_bo_queued(unsigned nr, struct drm_panfrost_wait_bo *request, size_t size)
{
	uint64_t seq = handle_kind(request->handle) & HANDLE_SURFACE ? UINT64_MAX : handle_seq(request->handle);
	uint64_t done;
	int ret;

	if (seq == UINT64_MAX) {
		return call(nr, request, size);
	}
	pthread_mutex_lock(&sync_lock);
	done = known_done;
	pthread_mutex_unlock(&sync_lock);
	if (seq <= done) {
		__atomic_fetch_add(&stats.local_waits, 1, __ATOMIC_RELAXED);
		return 0;
	}
	ret = wait_seq(seq, request->timeout_ns <= 0 ? 0 : request->timeout_ns);
	if (ret != 0 && errno == ETIME) {
		errno = request->timeout_ns <= 0 ? EBUSY : ETIMEDOUT;
	}
	return ret;
}

/*
 * A request of the driver's own (0x40 + n): SUBMIT as above; WAIT_BO and MADVISE answered here
 * where that is the kernel's answer (redwood_drm.h); CREATE_BO noted. 0, or -1 with errno.
 */
static int
driver_request(unsigned nr, void *data, size_t size)
{
	int ret;

	switch (nr - DRM_COMMAND_BASE) {
	case DRM_PANFROST_SUBMIT:
		return submit(nr, data, size);
	case DRM_PANFROST_WAIT_BO: {
		struct drm_panfrost_wait_bo *request = data;

		if (size < sizeof(*request) || local_answers() < 1) {
			break;
		}
		if (queued) {
			return wait_bo_queued(nr, request, size);
		}
		/* synchronous: no job of this process is running, and no other program's job uses this buffer: idle */
		if (!(handle_kind(request->handle) & HANDLE_SURFACE) && __atomic_load_n(&submits_running, __ATOMIC_SEQ_CST) == 0) {
			__atomic_fetch_add(&stats.local_waits, 1, __ATOMIC_RELAXED);
			return 0;
		}
		break;
	}
	case DRM_PANFROST_MADVISE: {
		struct drm_panfrost_madvise *madvise = data;
		unsigned kind = size >= sizeof(*madvise) ? handle_kind(madvise->handle) : 0;

		/* a buffer without cache upkeep: nothing for the kernel to note, nothing is ever purged */
		if ((kind & HANDLE_WC) && madvise->madv <= PANFROST_MADV_DONTNEED) {
			madvise->retained = 1;
			__atomic_fetch_add(&stats.local_madvises, 1, __ATOMIC_RELAXED);
			return 0;
		}
		break;
	}
	case DRM_PANFROST_CREATE_BO: {
		struct drm_panfrost_create_bo *create = data;

		ret = call(nr, data, size);
		if (ret == 0 && size >= sizeof(*create)) {
			bool wc = (create->pad & REDWOOD_DRM_CREATE_BO_WC) != 0 ||
			    (local_answers() >= 2 && !(create->flags & PANFROST_BO_HEAP));

			set_handle_kind(create->handle, local_answers() >= 1 && wc ? HANDLE_WC : 0);
			create->pad = 0;        /* (as Linux leaves it) */
		}
		return ret;
	}
	default:
		break;
	}
	return call(nr, data, size);
}

EXPORT int
drmIoctl(int fd, unsigned long request, void *arg)
{
	unsigned nr = (unsigned)(request & 0xff);
	size_t size = IOCPARM_LEN(request);

	(void)fd;
	if (nr >= DRM_COMMAND_BASE && nr < DRM_COMMAND_END) {
		return driver_request(nr, arg, size);
	}
	switch (request) {
	case DRM_IOCTL_GEM_CLOSE:
		set_handle_kind(((const struct drm_gem_close *)arg)->handle, 0);
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
	if (driver_request(nr, data, size)) {
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
	set_handle_kind(request.handle, HANDLE_SURFACE);
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
	uint64_t began = mach_absolute_time();

	result = IOConnectMapMemory64(connection, handle, mach_task_self(), &address, &size, options);
	count(&stats.maps, &stats.map_ticks, began);
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
	uint64_t began = mach_absolute_time();
	kern_return_t result = IOConnectUnmapMemory64(connection, found.handle, mach_task_self(), found.address);

	count(&stats.unmaps, &stats.unmap_ticks, began);

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
		s->has_fence = false;
		s->seq = 0;
	}
	pthread_mutex_unlock(&sync_lock);
	if (s == NULL) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

/* A syncobj's state: whether it exists, has a fence, and the fence. */
static bool
syncobj_state(uint32_t handle, bool *has_fence, uint64_t *seq)
{
	struct syncobj *s;

	pthread_mutex_lock(&sync_lock);
	s = syncobj(handle);
	*has_fence = s != NULL && s->has_fence;
	*seq = s != NULL ? s->seq : 0;
	pthread_mutex_unlock(&sync_lock);
	return s != NULL;
}

static bool
signaled(uint32_t handle, bool *exists)
{
	struct syncobj *s;
	bool result;

	pthread_mutex_lock(&sync_lock);
	s = syncobj(handle);
	*exists = s != NULL;
	result = s != NULL && fence_signaled(s);
	pthread_mutex_unlock(&sync_lock);
	return result;
}

/*
 * A sync file of a syncobj's fence (Mesa makes every pipe_fence so: the context's syncobj
 * exported, imported into a new one). Linux: a syncobj without a fence cannot be exported.
 */
EXPORT int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	bool has_fence;
	uint64_t seq;

	(void)fd;
	*sync_file_fd = -1;
	if (!syncobj_state(handle, &has_fence, &seq)) {
		errno = ENOENT;
		return -1;
	}
	if (!has_fence) {
		errno = EINVAL;
		return -1;
	}
	*sync_file_fd = fence_sync_file(seq);
	return *sync_file_fd >= 0 ? 0 : -1;
}

EXPORT int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	(void)fd;
	return syncobj_set_fence(&handle, 1, true, sync_file_seq(sync_file_fd));
}

/* The syncobj itself as a file: here a sync file of its fence, imported again as a new syncobj in its state. */
EXPORT int
drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd)
{
	bool has_fence;
	uint64_t seq;

	(void)fd;
	*obj_fd = -1;
	if (!syncobj_state(handle, &has_fence, &seq)) {
		errno = ENOENT;
		return -1;
	}
	*obj_fd = fence_sync_file(has_fence ? seq : 0);
	return *obj_fd >= 0 ? 0 : -1;
}

EXPORT int
drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle)
{
	uint64_t seq = sync_file_seq(obj_fd);

	(void)fd;
	if (syncobj_create(true, handle) != 0) {
		return -1;
	}
	return syncobj_set_fence(handle, 1, true, seq);
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
	bool has_fence;
	uint64_t seq;

	(void)fd;
	(void)flags;
	if (dst_point != 0 || src_point != 0) {
		errno = EINVAL;
		return -1;
	}
	if (!syncobj_state(src_handle, &has_fence, &seq)) {
		errno = ENOENT;
		return -1;
	}
	return syncobj_set_fence(&dst_handle, 1, has_fence, seq);
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
