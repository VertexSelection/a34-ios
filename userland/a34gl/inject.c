/*
 * liba34gl-inject.dylib: inserted into backboardd (DYLD_INSERT_LIBRARIES: the kernel's boot-arg
 * a34inject=/a34/liba34gl-inject.dylib,backboardd, or a34gl-bbd, bbd.c). When the kernel has published the GPU's service (RedwoodMali), it loads the OpenGL
 * ES library that stands behind EAGLContext (liba34gl.dylib, eagl.c + gl_egl.c), so that
 * CoreAnimation's window server draws with the Mali-G68 instead of its software renderer.
 * Boot-arg a34gl=0, or A34GL_OFF=1 in the environment: never.
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
#include <mach/mach.h>
#include <signal.h>
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
#define ENVFILE         A34GL_DIR "/backboardd.env"

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

	a34gl_say_always("a34gl-crash: backboardd[%d]: signal %d at address %p\n", getpid(), sig, si ? si->si_addr : NULL);
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
 * /a34/backboardd.env (KEY=VALUE lines, # comments), as a34gl-bbd reads it: when the kernel
 * inserts this library (boot-arg a34inject=...) there is no wrapper, and this is the way to
 * give backboardd A34GL=..., A34GL_LOG=..., PAN_MESA_DEBUG=... . What the environment has
 * already (the job's, the wrapper's) stays.
 */
static void
environment_file(void)
{
	FILE *f = fopen(ENVFILE, "r");
	char line[512];

	if (f == NULL) {
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

__attribute__((constructor))
static void
a34gl_inject(void)
{
	const char *name = getprogname(), *off = getenv("A34GL_OFF");
	char args[1024] = "";
	size_t size = sizeof(args) - 1;
	io_service_t gpu = 0;
	void *lib;

	/* (what launchd or a34gl-bbd started us with is not for backboardd's children) */
	unsetenv("DYLD_INSERT_LIBRARIES");
	if (name == NULL || strcmp(name, "backboardd") != 0) {
		return;
	}
	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0 || strstr(args, "a34gl=0") == NULL) {
		environment_file();
		off = getenv("A34GL_OFF");
	}
	if ((off != NULL && strcmp(off, "1") == 0) || strstr(args, "a34gl=0") != NULL) {
		say("a34gl-inject: backboardd[%d]: switched off: CoreAnimation keeps its software renderer\n", getpid());
		return;
	}
	if (access(A34GL_LIBRARY, R_OK) != 0) {
		say("a34gl-inject: backboardd[%d]: no %s\n", getpid(), A34GL_LIBRARY);
		return;
	}
	/* the kernel may start the GPU after the root filesystem is there: give it a moment */
	for (int i = 0; i < 50; i++) {
		gpu = IOServiceGetMatchingService(0, IOServiceMatching(GPU_SERVICE));
		if (gpu != 0) {
			break;
		}
		usleep(100000);
	}
	if (gpu == 0) {
		say("a34gl-inject: backboardd[%d]: no %s service: software renderer\n", getpid(), GPU_SERVICE);
		return;
	}
	IOObjectRelease(gpu);
	crash_handlers();
	lib = dlopen(A34GL_LIBRARY, RTLD_NOW);
	gl_hooks(lib);
	say("a34gl-inject: backboardd[%d]: %s %s\n", getpid(), A34GL_LIBRARY, lib ? "loaded" : dlerror());
}
