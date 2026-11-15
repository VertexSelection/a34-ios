/*
 * a34_watch: a test program's own witness in the kernel log (linked into metal_drv_test and
 * metal-probe; nothing else of theirs changes).
 *
 * Boot 54 (kernel 0929): both tests printed their first lines and then nothing at all -- not
 * their own lines, not the plug-in's, not redwood-drm's stderr line, not this file's heartbeat on
 * a /dev/console descriptor of its own -- while the kernel saw them open the GPU, make buffers and
 * close it 30-40 s later, with no crash record. What was written after a point never reached the
 * log. So nothing here relies on a descriptor that was open before:
 *
 *   - stdout and stderr are replaced (funopen) by a writer that puts every line, prefixed with
 *     the seconds since the program started, through /dev/console opened anew for that line
 *     (O_NONBLOCK: a console that does not take it costs the line, not the program), and also
 *     to /dev/null, which the kernel copies into its log as "|name| line" when the program is
 *     named in boot-arg sweetnull (ios-sweet 0048) -- a road no tty state can close. The
 *     environment variable A34WATCH_NULL=0 leaves /dev/null out.
 *   - a heartbeat every 5 s: "alive" first, with whether stdout's own descriptor 1 is still a
 *     working file, then the main thread's return addresses. No symbol lookup (dladdr waits for
 *     dyld's lock, which the main thread may hold): addresses in the shared cache are printed
 *     unslid ("c:0x18..." , symbolize with the cache), others as image+offset from a table kept
 *     by dyld's add-image callback.
 *   - a line from exit() (atexit) and from each signal that ends the program.
 */
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define A34_WATCH_PERIOD 5      /* seconds */
#define A34_WATCH_FRAMES 16
#define A34_WATCH_IMAGES 64

extern const void *_dyld_get_shared_cache_range(size_t *length);

static uint64_t watch_start;
static mach_timebase_info_data_t watch_timebase;
static thread_t watch_main;
static uintptr_t watch_stack_top, watch_stack_bottom;   /* the main thread's stack (the constructor runs on it) */
static int watch_null = -1;                              /* /dev/null: the kernel's tee, if boot-arg sweetnull names us */
static uintptr_t cache_start, cache_end, cache_slide;   /* the dyld shared cache, mapped */
static struct {
	uintptr_t base;
	const char *name;
} images[A34_WATCH_IMAGES];                             /* images outside the cache, as dyld adds them */
static volatile int nimages;
static volatile uint64_t lines_out, lines_lost;

static double
watch_seconds(void)
{
	return (double)(mach_absolute_time() - watch_start) * watch_timebase.numer / watch_timebase.denom / 1e9;
}

/* one buffer of text into the log: a new /dev/console descriptor and /dev/null (async-signal-safe) */
static void
watch_out(const char *text, size_t n)
{
	int fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);

	if (fd >= 0) {
		if (write(fd, text, n) == (ssize_t)n) {
			lines_out++;
		} else {
			lines_lost++;
		}
		close(fd);
	} else {
		lines_lost++;
	}
	if (watch_null >= 0) {
		(void)write(watch_null, text, n);
	}
}

static void __attribute__((format(printf, 1, 2)))
watch_say(const char *format, ...)
{
	char line[1200];
	int n = snprintf(line, sizeof(line), "a34watch: %s[%d] %.1f s: ", getprogname(), (int)getpid(), watch_seconds());
	va_list ap;

	va_start(ap, format);
	n += vsnprintf(line + n, sizeof(line) - (size_t)n - 1, format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 2) {
		n = (int)sizeof(line) - 2;
	}
	line[n++] = '\n';
	watch_out(line, (size_t)n);
}

/* stdout's and stderr's writer: each line with the seconds since the start in front */
static int
watch_stdio_write(void *cookie, const char *buf, int len)
{
	static __thread int at_line_start = 1;
	char out[1400];
	size_t used = 0;

	(void)cookie;
	for (int i = 0; i < len; i++) {
		if (at_line_start) {
			int k = snprintf(out + used, sizeof(out) - used, "+%.1f ", watch_seconds());

			used += k > 0 ? (size_t)k : 0;
			at_line_start = 0;
		}
		out[used++] = buf[i];
		if (buf[i] == '\n') {
			at_line_start = 1;
		}
		if (used > sizeof(out) - 32 || buf[i] == '\n') {
			watch_out(out, used);
			used = 0;
		}
	}
	if (used != 0) {
		watch_out(out, used);
	}
	return len;
}

/* the main thread's return addresses, read while it is stopped (nothing that takes a lock) */
static unsigned
watch_backtrace(uintptr_t *pcs, unsigned max)
{
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
		while (n < max && fp >= watch_stack_bottom && fp + 16 <= watch_stack_top && (fp & 7) == 0) {
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

static void
watch_where(uintptr_t pc, char *out, size_t size)
{
	int best = -1;

	if (pc >= cache_start && pc < cache_end) {
		snprintf(out, size, "c:%#lx", (unsigned long)(pc - cache_slide));
		return;
	}
	for (int i = 0; i < nimages; i++) {
		if (images[i].base <= pc && (best < 0 || images[i].base > images[best].base)) {
			best = i;
		}
	}
	if (best >= 0) {
		snprintf(out, size, "%s+%#lx", images[best].name, (unsigned long)(pc - images[best].base));
	} else {
		snprintf(out, size, "%#lx", (unsigned long)pc);
	}
}

static void *
watch_thread(void *unused)
{
	(void)unused;
	pthread_setname_np("a34watch");
	for (;;) {
		uintptr_t pcs[A34_WATCH_FRAMES];
		char line[1000], where[96];
		size_t used = 0;
		struct stat st;
		unsigned n;

		sleep(A34_WATCH_PERIOD);
		watch_say("alive; fd 1 %s, %llu lines out, %llu lost", fstat(1, &st) == 0 ? (S_ISCHR(st.st_mode) ?
		    "a character device" : "not a character device") : strerror(errno), (unsigned long long)lines_out,
		    (unsigned long long)lines_lost);
		n = watch_backtrace(pcs, A34_WATCH_FRAMES);
		line[0] = '\0';
		for (unsigned i = 0; i < n && used + 100 < sizeof(line); i++) {
			watch_where(pcs[i], where, sizeof(where));
			used += (size_t)snprintf(line + used, sizeof(line) - used, "%s%s", i ? " < " : "", where);
		}
		watch_say("main thread: %s", n ? line : "(no backtrace)");
	}
	return NULL;
}

/* an image's name without asking dyld (its lock may be held here): LC_ID_DYLIB, else its kind */
static const char *
watch_image_name(const struct mach_header *header)
{
	const struct mach_header_64 *h = (const struct mach_header_64 *)header;
	const uint8_t *cmd = (const uint8_t *)(h + 1);

	if (h->magic != MH_MAGIC_64) {
		return "?";
	}
	for (uint32_t i = 0; i < h->ncmds; i++) {
		const struct load_command *lc = (const struct load_command *)cmd;

		if (lc->cmd == LC_ID_DYLIB) {
			const struct dylib_command *d = (const struct dylib_command *)lc;
			const char *path = (const char *)lc + d->dylib.name.offset;

			return strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
		}
		cmd += lc->cmdsize;
	}
	return h->filetype == MH_EXECUTE ? "main" : h->filetype == MH_BUNDLE ? "bundle" : "image";
}

static void
watch_added(const struct mach_header *header, intptr_t slide)
{
	uintptr_t base = (uintptr_t)header;
	int i;

	(void)slide;
	if ((base >= cache_start && base < cache_end) || nimages >= A34_WATCH_IMAGES) {
		return;
	}
	i = nimages;
	images[i].base = base;
	images[i].name = watch_image_name(header);
	nimages = i + 1;
}

static void
watch_exit(void)
{
	fflush(stdout);
	fflush(stderr);
	watch_say("exit() (%llu lines out, %llu lost)", (unsigned long long)lines_out, (unsigned long long)lines_lost);
}

static void
watch_signal(int sig)
{
	char line[96];
	int n = snprintf(line, sizeof(line), "a34watch: %s[%d]: signal %d\n", getprogname(), (int)getpid(), sig);

	watch_out(line, (size_t)n);
	signal(sig, SIG_DFL);
	raise(sig);
}

__attribute__((constructor)) static void
watch_start_up(void)
{
	static const int signals[] = { SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE, SIGTERM, SIGHUP, SIGPIPE, SIGSYS };
	const char *null_wanted = getenv("A34WATCH_NULL");
	size_t length = 0;
	FILE *out, *err;
	pthread_t thread;

	mach_timebase_info(&watch_timebase);
	watch_start = mach_absolute_time();
	watch_main = mach_thread_self();
	watch_stack_top = (uintptr_t)pthread_get_stackaddr_np(pthread_self());
	watch_stack_bottom = watch_stack_top - pthread_get_stacksize_np(pthread_self());
	cache_start = (uintptr_t)_dyld_get_shared_cache_range(&length);
	cache_end = cache_start + length;
	cache_slide = cache_start != 0 ? cache_start - 0x180000000ul : 0;
	if (null_wanted == NULL || strcmp(null_wanted, "0") != 0) {
		watch_null = open("/dev/null", O_WRONLY | O_CLOEXEC);
	}
	_dyld_register_func_for_add_image(watch_added);

	/* stdout and stderr: every line through watch_out, line-buffered */
	out = funopen(NULL, NULL, watch_stdio_write, NULL, NULL);
	err = funopen(NULL, NULL, watch_stdio_write, NULL, NULL);
	if (out != NULL) {
		setvbuf(out, NULL, _IOLBF, 0);
		stdout = out;
	}
	if (err != NULL) {
		setvbuf(err, NULL, _IOLBF, 0);
		stderr = err;
	}
	atexit(watch_exit);
	signal(SIGPIPE, SIG_IGN);
	for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
		if (signals[i] != SIGPIPE) {
			signal(signals[i], watch_signal);
		}
	}
	if (pthread_create(&thread, NULL, watch_thread, NULL) == 0) {
		pthread_detach(thread);
	}
	watch_say("started; stdout and stderr go through /dev/console opened per line%s; shared cache at %#lx "
	    "(slide %#lx); a heartbeat every %d s", watch_null >= 0 ? " and /dev/null (boot-arg sweetnull)" : "",
	    (unsigned long)cache_start, (unsigned long)cache_slide, A34_WATCH_PERIOD);
}
