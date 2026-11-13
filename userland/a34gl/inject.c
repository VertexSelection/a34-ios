/*
 * liba34gl-inject.dylib: inserted into programs (DYLD_INSERT_LIBRARIES: the kernel's boot-arg
 * a34inject=/a34/liba34gl-inject.dylib,backboardd,SpringBoard,..., or a34gl-bbd, bbd.c). It puts
 * the OpenGL ES library that stands behind EAGLContext (liba34gl.dylib: eagl.c + gl_egl.c, Mesa on
 * the Mali-G68) into the program, when the kernel has published the GPU's service (RedwoodMali):
 *
 *   backboardd   the window server: liba34gl is loaded at once, and CoreAnimation, which finds
 *                an EAGLContext that works, draws with the GPU instead of its software renderer.
 *                Two of CoreAnimation's virtual functions are hooked for it (below).
 *   any other    (SpringBoard, an application: CoreImage and whoever else makes an EAGLContext --
 *                with no GPU driver of iOS's own behind it such a context has a function table
 *                of nothing, and its first call is a jump to 0). Nothing is loaded until the
 *                program makes its first EAGLContext: -initWithAPI:properties: and
 *                -commonInitWithAPI:properties:, where every way of making one ends, are replaced
 *                by a stub that loads liba34gl (which replaces them in turn) and starts over.
 *                A program that never makes one pays a dlopen of OpenGLES and two method swaps.
 *
 * A34GL_MODE, per program (default mesa): mesa as above; none: an EAGLContext cannot be made
 * (-init... answers nil: for a program that does better without OpenGL ES than with ours);
 * off: nothing is touched. A34GL_OFF=1 and the boot-arg a34gl=0 are "off" for every program.
 * The environment comes from /a34/NAME.env (KEY=VALUE lines, # comments; NAME is the program's,
 * e.g. /a34/backboardd.env, /a34/SpringBoard.env) on top of what the program was started with.
 *
 * Every program it is inserted into gets one line on the console (the kernel's log), whatever
 * its stderr is: "a34gl-inject: NAME[PID]: loaded, mode ...".
 *
 * Ported from ios-topaz's userland/gpu/topaz_inject.c (MIT; its scheme is HackiOS's /
 * darwin-redwood's inject.c, MIT, Copyright (c) 2026 OOSEX GANG). Left out: topaz's
 * containermanagerd fix (not the GPU's business). The addresses are of the iOS 16.7.16
 * (20H392) iPhone X dyld shared cache -- ours: check-cache.py checks them against a root
 * image (cache UUID 8f970452-c9e3-3e88-b114-feb1864bc9e0) -- and every entry is checked
 * at run time against the function it must hold before it is replaced.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>
#include <unistd.h>

#include "a34gl.h"
#include "a34gl_iokit.h"

extern const void *_dyld_get_shared_cache_range(size_t *length);

#ifndef A34GL_DIR
#define A34GL_DIR       "/a34"
#endif
#define A34GL_LIBRARY   A34GL_DIR "/liba34gl.dylib"
#define GPU_SERVICE     "RedwoodMali"
#define OPENGLES        "/System/Library/Frameworks/OpenGLES.framework/OpenGLES"

enum { MODE_OFF, MODE_NONE, MODE_MESA };
static int mode = MODE_MESA;
static bool render_server;              /* this program is the window server */
static char program[64] = "?";

/* One line for the kernel's log, whatever the program's stderr is: the console, else the log. */
static void __attribute__((format(printf, 1, 2)))
announce(const char *format, ...)
{
	char line[400];
	va_list ap;
	int n, fd;

	n = snprintf(line, sizeof(line), "a34gl-inject: %s[%d]: ", program, (int)getpid());
	va_start(ap, format);
	n += vsnprintf(line + n, sizeof(line) - (size_t)n, format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 2) {
		n = sizeof(line) - 2;
	}
	line[n++] = '\n';
	if ((fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC)) < 0 || write(fd, line, (size_t)n) < 0) {
		(void)write(a34gl_log_fd(), line, (size_t)n);
	}
	if (fd >= 0) {
		close(fd);
	}
}

/*
 * CoreAnimation's OpenGL ES renderer hands a "function"'s numbers (a colour matrix, ...) to
 * GLContext::function_uniform(); for the functions it has no shader for they go nowhere, and our
 * library draws those with programs of its own (eagl.c) once told them. Without that the lock
 * screen's vibrantColorMatrix layer is a red veil. (topaz; scheme from darwin-redwood inject.c;
 * addresses are this iOS 16.7.16 cache's: check-cache.py.)
 */
#define CACHE_BASE              0x180000000UL
#define VT_GLES_CONTEXT         0x1d5b2f6e8UL   /* vtable for CA::OGL::GLESContext */
#define VSLOT(vtable, offset)   ((vtable) + 0x10 + (offset))
#define FN_FUNCTION_SUPPORTED   0x1884b2164UL   /* CA::OGL::GLContext::function_supported() const */
#define FN_FUNCTION_UNIFORM     0x1884b21acUL   /* CA::OGL::GLContext::function_uniform(unsigned, unsigned long, unsigned long, float const*) */

#define say a34gl_say

static uintptr_t slide;

static void *
replace_virtual(const char *what, uintptr_t slot, uintptr_t expected, void *replacement)
{
	uintptr_t *entry = (uintptr_t *)(slot + slide);
	vm_address_t page = (vm_address_t)entry & ~(vm_address_t)(vm_page_size - 1);
	void *original;

	if (*entry != expected + slide) {
		say("a34gl-inject: %s: slot holds %#lx, expected %#lx: left alone\n", what, (unsigned long)(*entry - slide),
		    (unsigned long)expected);
		return NULL;
	}
	if (vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) {
		say("a34gl-inject: %s: table not writable\n", what);
		return NULL;
	}
	original = (void *)*entry;
	*entry = (uintptr_t)replacement;
	vm_protect(mach_task_self(), page, vm_page_size, FALSE, VM_PROT_READ);
	return original;
}

static int (*orig_function_supported)(void *context);
static void (*gl_function_refused)(unsigned function);
static void (*orig_function_uniform)(void *context, unsigned index, size_t components, size_t count, const float *values);
static void (*gl_function_values)(void *context, unsigned function, unsigned index, size_t components, size_t count,
    const float *values);

static unsigned
context_function(void *context)
{
	return *(*(uint8_t **)((char *)context + 0x10) + 0x10);
}

static int
my_function_supported(void *context)
{
	static uint8_t said[256];
	int supported = orig_function_supported(context);
	unsigned function = context_function(context);

	if (!supported) {
		if (gl_function_refused != NULL) {
			gl_function_refused(function);
		}
		if (!said[function & 255]) {
			said[function & 255] = 1;
			say("a34gl-inject: CoreAnimation's GL renderer has no function %u\n", function);
		}
	}
	return supported;
}

static void
my_function_uniform(void *context, unsigned index, size_t components, size_t count, const float *values)
{
	gl_function_values(context, context_function(context), index, components, count, values);
	orig_function_uniform(context, index, components, count, values);
}

static void
gl_hooks(void *lib)
{
	size_t length = 0;
	const void *cache = _dyld_get_shared_cache_range(&length);

	if (cache == NULL || lib == NULL) {
		return;
	}
	slide = (uintptr_t)cache - CACHE_BASE;
	gl_function_refused = (void (*)(unsigned))dlsym(lib, "a34gl_function_refused");
	gl_function_values = (void (*)(void *, unsigned, unsigned, size_t, size_t, const float *))dlsym(lib,
	    "a34gl_function_values");
	orig_function_supported = replace_virtual("function_supported", VSLOT(VT_GLES_CONTEXT, 0x378), FN_FUNCTION_SUPPORTED,
	    my_function_supported);
	if (gl_function_values != NULL) {
		orig_function_uniform = replace_virtual("function_uniform", VSLOT(VT_GLES_CONTEXT, 0x388), FN_FUNCTION_UNIFORM,
		    my_function_uniform);
	}
	say("a34gl-inject: GLESContext hooks: function_supported %s, function_uniform %s\n",
	    orig_function_supported ? "on" : "OFF", orig_function_uniform ? "on" : "OFF");
}

/* Where a crash happened, into the log: image + offset + symbol. */
static void
where(const char *what, void *addr)
{
	Dl_info info;

	if (dladdr(addr, &info) && info.dli_fname != NULL) {
		a34gl_say_always("a34gl-crash: %s %p = %s+0x%lx (%s)\n", what, addr, info.dli_fname,
		    (unsigned long)((char *)addr - (char *)info.dli_fbase), info.dli_sname ? info.dli_sname : "?");
	} else {
		a34gl_say_always("a34gl-crash: %s %p (no image)\n", what, addr);
	}
}

static void
crashed(int sig, siginfo_t *si, void *uc_)
{
	ucontext_t *uc = (ucontext_t *)uc_;
	void *frames[16];
	int n;

	a34gl_say_always("a34gl-crash: %s[%d]: signal %d at address %p\n", program, getpid(), sig, si ? si->si_addr : NULL);
	if (uc != NULL && uc->uc_mcontext != NULL) {
		where("pc", (void *)uc->uc_mcontext->__ss.__pc);
		where("lr", (void *)uc->uc_mcontext->__ss.__lr);
	}
	n = backtrace(frames, 16);
	for (int i = 0; i < n; i++) {
		where("bt", frames[i]);
	}
	signal(sig, SIG_DFL);
	raise(sig);
}

static void
crash_handlers(void)
{
	struct sigaction sa;
	int sigs[] = { SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGTRAP, SIGFPE };

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = crashed;
	sa.sa_flags = SA_SIGINFO;
	for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
		sigaction(sigs[i], &sa, NULL);
	}
}

/*
 * /a34/NAME.env (KEY=VALUE lines, # comments) for the program NAME, as a34gl-bbd reads
 * /a34/backboardd.env: the way to give a program A34GL_MODE=..., A34GL=..., A34GL_LOG=...,
 * PAN_MESA_DEBUG=... when the kernel inserts this library and there is no wrapper. What the
 * environment has already (the job's, the wrapper's) stays.
 */
static void
environment_file(void)
{
	char path[160], line[512];
	FILE *f;

	snprintf(path, sizeof(path), A34GL_DIR "/%s.env", program);
	if ((f = fopen(path, "r")) == NULL) {
		return;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		char *eq = strchr(line, '='), *nl = strchr(line, '\n');

		if (nl != NULL) {
			*nl = 0;
		}
		if (eq != NULL && line[0] != '#' && eq != line) {
			*eq = 0;
			setenv(line, eq + 1, 0);
		}
	}
	fclose(f);
}

/*
 * liba34gl.dylib into this program, once: when the GPU's service is there (the window server,
 * started before the kernel may have brought the GPU up, gives it five seconds).
 */
static pthread_once_t load_once = PTHREAD_ONCE_INIT;
static void *library;

static void
load(void)
{
	io_service_t gpu = 0;

	for (int i = 0; i < (render_server ? 50 : 1); i++) {
		gpu = IOServiceGetMatchingService(0, IOServiceMatching(GPU_SERVICE));
		if (gpu != 0) {
			break;
		}
		if (render_server) {
			usleep(100000);
		}
	}
	if (gpu == 0) {
		announce("no %s service: %s", GPU_SERVICE, render_server ? "CoreAnimation keeps its software renderer" :
		    "EAGLContexts stay iOS's own");
		return;
	}
	IOObjectRelease(gpu);
	if (access(A34GL_LIBRARY, R_OK) != 0) {
		announce("no %s", A34GL_LIBRARY);
		return;
	}
	setenv("A34GL_ROLE", render_server ? "render" : "client", 0);
	crash_handlers();
	library = dlopen(A34GL_LIBRARY, RTLD_NOW);
	if (library != NULL && (render_server || getenv("A34GL_HOOKS") != NULL)) {
		gl_hooks(library);              /* (CoreAnimation's renderer: the window server's) */
	}
	announce("%s %s", A34GL_LIBRARY, library ? "loaded" : dlerror());
}

/*
 * In a program that is not the window server: -[EAGLContext initWithAPI:properties:] and
 * -commonInitWithAPI:properties: until the first EAGLContext is made. Then liba34gl is loaded,
 * its own methods take these two's place, and the message is sent again; if it could not be
 * loaded (no GPU), iOS's own method is called, as if nothing had been inserted. A34GL_MODE=none:
 * no EAGLContext is made at all.
 */
static IMP ios_init[2];
static SEL init_selector[2];

static id
first_context(id self, SEL selector, unsigned long api, id properties)
{
	IMP ios = selector == init_selector[1] ? ios_init[1] : ios_init[0], now;

	if (mode == MODE_NONE) {
		a34gl_say("a34gl-inject: an EAGLContext of API %lu is asked for: none is made (A34GL_MODE=none)\n", api);
		((void (*)(id, SEL))objc_msgSend)(self, sel_registerName("release"));
		return nil;
	}
	pthread_once(&load_once, load);
	now = class_getMethodImplementation(object_getClass(self), selector);
	if (library != NULL && now != (IMP)first_context) {
		return ((id (*)(id, SEL, unsigned long, id))now)(self, selector, api, properties);
	}
	return ((id (*)(id, SEL, unsigned long, id))ios)(self, selector, api, properties);
}

static bool
wait_for_first_context(void)
{
	static const char *const names[2] = { "initWithAPI:properties:", "commonInitWithAPI:properties:" };
	Class class = objc_getClass("EAGLContext");
	unsigned replaced = 0;

	if (class == Nil && dlopen(OPENGLES, RTLD_NOW | RTLD_GLOBAL) != NULL) {
		class = objc_getClass("EAGLContext");
	}
	for (unsigned i = 0; class != Nil && i < 2; i++) {
		Method method;

		init_selector[i] = sel_registerName(names[i]);
		if ((method = class_getInstanceMethod(class, init_selector[i])) != NULL) {
			ios_init[i] = method_setImplementation(method, (IMP)first_context);
			replaced++;
		}
	}
	return replaced == 2;
}

__attribute__((constructor))
static void
a34gl_inject(void)
{
	const char *name = getprogname(), *asked;
	char args[1024] = "";
	size_t size = sizeof(args) - 1;
	size_t n = 0;

	/* (what the kernel, launchd or a34gl-bbd started us with is not for the program's children) */
	unsetenv("DYLD_INSERT_LIBRARIES");
	for (const char *c = name != NULL ? name : "?"; *c != 0 && n < sizeof(program) - 1; c++) {
		program[n++] = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '.' ||
		    *c == '-' || *c == '_' ? *c : '_';
	}
	program[n] = 0;
	render_server = strcmp(program, "backboardd") == 0;
	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "a34gl=0") != NULL) {
		announce("loaded, mode off (boot-arg a34gl=0)");
		return;
	}
	environment_file();
	asked = getenv("A34GL_MODE");
	if ((getenv("A34GL_OFF") != NULL && strcmp(getenv("A34GL_OFF"), "1") == 0) || (asked != NULL && strcmp(asked, "off") == 0)) {
		announce("loaded, mode off (%s)", asked != NULL && strcmp(asked, "off") == 0 ? "A34GL_MODE=off" : "A34GL_OFF=1");
		return;
	}
	mode = asked != NULL && strcmp(asked, "none") == 0 ? MODE_NONE : MODE_MESA;
	if (asked != NULL && mode == MODE_MESA && strcmp(asked, "mesa") != 0) {
		announce("A34GL_MODE=%s is not known (mesa, none, off): mesa", asked);
	}
	if (render_server && mode == MODE_MESA) {
		announce("loaded, mode mesa: the window server, " A34GL_LIBRARY " now");
		pthread_once(&load_once, load);
		return;
	}
	if (wait_for_first_context()) {
		announce("loaded, mode %s: %s", mode == MODE_NONE ? "none" : "mesa", mode == MODE_NONE ?
		    "an EAGLContext cannot be made here" : A34GL_LIBRARY " at the program's first EAGLContext");
	} else {
		announce("loaded, but EAGLContext's methods are not found: nothing done");
	}
}
