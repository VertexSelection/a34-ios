/*
 * a34_watch: a test program's own witness on the console (linked into metal_drv_test and
 * metal-probe; nothing else of theirs changes).
 *
 * The first run of metal-probe on the phone (boot 31, kernel 0918) printed four lines, went
 * silent for 30 s inside MTLCreateSystemDefaultDevice(), opened the GPU, and was never heard
 * of again: no further line, no crash record. So from the start of main() on:
 *
 *   - its own descriptor on /dev/console, opened here, independent of whatever happens to
 *     the program's stdout and stderr;
 *   - every 5 s a line "a34watch: PROGRAM[PID] T s: ..." with the main thread's backtrace
 *     (frame-pointer walk while the thread is suspended; symbol names looked up after it runs
 *     again, nearest exported name, so a private function shows as its nearest export);
 *   - a last line from exit() (atexit), and one for each signal that would end the program.
 *
 * Lines are written with write(2), in one call each.
 */
#include <dlfcn.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define A34_WATCH_PERIOD 5      /* seconds */
#define A34_WATCH_FRAMES 14

static int watch_fd = -1;
static uint64_t watch_start;
static mach_timebase_info_data_t watch_timebase;
static thread_t watch_main;

static double
watch_seconds(void)
{
	uint64_t ticks = mach_absolute_time() - watch_start;

	return (double)ticks * watch_timebase.numer / watch_timebase.denom / 1e9;
}

static void __attribute__((format(printf, 1, 2)))
watch_say(const char *format, ...)
{
	char line[1024];
	int n = snprintf(line, sizeof(line), "a34watch: %s[%d] %.1f s: ", getprogname(), (int)getpid(), watch_seconds());
	va_list ap;

	va_start(ap, format);
	n += vsnprintf(line + n, sizeof(line) - (size_t)n - 1, format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 2) {
		n = (int)sizeof(line) - 2;
	}
	line[n++] = '\n';
	(void)write(watch_fd >= 0 ? watch_fd : 2, line, (size_t)n);
}

/* the main thread's return addresses, read while it is stopped */
static unsigned
watch_backtrace(uintptr_t *pcs, unsigned max)
{
	pthread_t main_pthread = pthread_main_thread_np();
	uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(main_pthread);
	uintptr_t bottom = top - pthread_get_stacksize_np(main_pthread);
	arm_thread_state64_t state;
	mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
	unsigned n = 0;

	if (thread_suspend(watch_main) != KERN_SUCCESS) {
		return 0;
	}
	if (thread_get_state(watch_main, ARM_THREAD_STATE64, (thread_state_t)&state, &count) == KERN_SUCCESS) {
		uintptr_t fp = (uintptr_t)arm_thread_state64_get_fp(state);

		pcs[n++] = (uintptr_t)arm_thread_state64_get_pc(state);
		if (n < max) {
			pcs[n++] = (uintptr_t)arm_thread_state64_get_lr(state);
		}
		while (n < max && fp >= bottom && fp + 16 <= top && (fp & 7) == 0) {
			uintptr_t next = ((uintptr_t *)fp)[0];
			uintptr_t ret = ((uintptr_t *)fp)[1];

			if (ret == 0) {
				break;
			}
			pcs[n++] = ret;
			if (next <= fp) {
				break;
			}
			fp = next;
		}
	}
	thread_resume(watch_main);
	return n;
}

static void *
watch_thread(void *unused)
{
	(void)unused;
	pthread_setname_np("a34watch");
	for (;;) {
		uintptr_t pcs[A34_WATCH_FRAMES];
		char line[900];
		size_t used = 0;
		unsigned n;

		sleep(A34_WATCH_PERIOD);
		n = watch_backtrace(pcs, A34_WATCH_FRAMES);
		line[0] = '\0';
		for (unsigned i = 0; i < n && used + 80 < sizeof(line); i++) {
			Dl_info info;
			const char *name = "?";
			uintptr_t offset = 0;

			if (dladdr((void *)pcs[i], &info) != 0) {
				if (info.dli_sname != NULL) {
					name = info.dli_sname;
					offset = pcs[i] - (uintptr_t)info.dli_saddr;
				} else if (info.dli_fname != NULL) {
					name = strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname;
					offset = pcs[i] - (uintptr_t)info.dli_fbase;
				}
			}
			used += (size_t)snprintf(line + used, sizeof(line) - used, "%s%s+%#lx", i ? " < " : "", name,
			    (unsigned long)offset);
		}
		watch_say("alive; main thread: %s", n ? line : "(no backtrace)");
	}
	return NULL;
}

static void
watch_exit(void)
{
	watch_say("exit()");
}

static void
watch_signal(int sig)
{
	char line[96];
	int n = snprintf(line, sizeof(line), "a34watch: %s[%d]: signal %d\n", getprogname(), (int)getpid(), sig);

	(void)write(watch_fd >= 0 ? watch_fd : 2, line, (size_t)n);
	signal(sig, SIG_DFL);
	raise(sig);
}

__attribute__((constructor)) static void
watch_start_up(void)
{
	static const int signals[] = { SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE, SIGTERM, SIGPIPE, SIGSYS };
	pthread_t thread;

	mach_timebase_info(&watch_timebase);
	watch_start = mach_absolute_time();
	watch_main = mach_thread_self();
	watch_fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_APPEND | O_CLOEXEC);
	setvbuf(stdout, NULL, _IOLBF, 0);
	atexit(watch_exit);
	for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
		signal(signals[i], watch_signal);
	}
	if (pthread_create(&thread, NULL, watch_thread, NULL) == 0) {
		pthread_detach(thread);
	}
	watch_say("started (console fd %d; a line every %d s with the main thread's backtrace)", watch_fd,
	    A34_WATCH_PERIOD);
}
