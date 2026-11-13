/*
 * Darwin has no eventfd. Mesa's panfrost_kmod.c includes this header for its performance
 * counter sessions (PAN_PERF), which then fail to start; nothing else uses it.
 */
#ifndef REDWOOD_COMPAT_SYS_EVENTFD_H
#define REDWOOD_COMPAT_SYS_EVENTFD_H

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>

typedef uint64_t eventfd_t;

#define EFD_SEMAPHORE   1
#define EFD_CLOEXEC     O_CLOEXEC
#define EFD_NONBLOCK    O_NONBLOCK

static inline int
eventfd(unsigned int initval, int flags)
{
	(void)initval;
	(void)flags;
	errno = ENOSYS;
	return -1;
}

static inline int
eventfd_read(int fd, eventfd_t *value)
{
	(void)fd;
	(void)value;
	errno = ENOSYS;
	return -1;
}

static inline int
eventfd_write(int fd, eventfd_t value)
{
	(void)fd;
	(void)value;
	errno = ENOSYS;
	return -1;
}

#endif
