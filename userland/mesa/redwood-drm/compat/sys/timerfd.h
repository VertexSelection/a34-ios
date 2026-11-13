/*
 * Darwin has no timerfd (nor struct itimerspec in every SDK). Mesa's panfrost_kmod.c
 * includes this header for its performance counter sessions (PAN_PERF), which then fail
 * to start; nothing else uses it.
 */
#ifndef REDWOOD_COMPAT_SYS_TIMERFD_H
#define REDWOOD_COMPAT_SYS_TIMERFD_H

#include <errno.h>
#include <fcntl.h>
#include <time.h>

#define itimerspec redwood_itimerspec
struct redwood_itimerspec {
	struct timespec it_interval;
	struct timespec it_value;
};

#define TFD_TIMER_ABSTIME       1
#define TFD_CLOEXEC             O_CLOEXEC
#define TFD_NONBLOCK            O_NONBLOCK

static inline int
timerfd_create(int clockid, int flags)
{
	(void)clockid;
	(void)flags;
	errno = ENOSYS;
	return -1;
}

static inline int
timerfd_settime(int fd, int flags, const struct itimerspec *new_value, struct itimerspec *old_value)
{
	(void)fd;
	(void)flags;
	(void)new_value;
	(void)old_value;
	errno = ENOSYS;
	return -1;
}

#endif
