/*
 * liba34media.dylib: code of ours inside mediaserverd, the audio server (and the home of
 * MediaToolbox's player, video queue, routing and decompression servers). liba34gl-inject.dylib,
 * which the kernel inserts into the programs boot-arg a34inject= names, opens it there (A34_LOAD,
 * userland/a34gl/inject.c).
 *
 * The crash it is about (README.md): whenever a program starts playing a movie -- the keyboard's
 * "slide to type" introduction, Setup's "hello" -- mediaserverd aborted on an Objective-C exception
 * that nobody caught, thrown in a block on one of its serial dispatch queues (its player server's,
 * "com.apple.coremedia.player.item.P/AV"). The kernel's crash line cannot say which: by the time
 * std::terminate runs, the stack has been unwound to libdispatch. Here:
 *
 *   the display's alignments  VideoToolbox's FigGetAlignmentForIOSurfaceOutputToDisplayService(), asked
 *                             by MediaToolbox how the frames of a movie that goes to the display are to
 *                             be laid out, reads AppleCLCD's DisplayPipePlaneBaseAlignment as a
 *                             dictionary { PlaneBaseAlignmentLinear = N } (QuartzCore reads it so too,
 *                             but checks the type). Our kernel publishes a number (ios-sweet 0046:
 *                             64), and -[__NSCFNumber objectForKey:] is the exception (boot 54). The
 *                             function keeps both answers in statics of its own; they are given here,
 *                             before anybody asks, with the kernel's number (the function's own default
 *                             is the same 64). Left alone when the kernel's is a dictionary. This one
 *                             is done in every program the library is loaded into (A34_LOAD), the rest
 *                             only in mediaserverd.
 *   every exception thrown    is said on the console, with the dispatch queue it was thrown on, its
 *                             name and reason and the stack at the throw: shared-cache addresses
 *                             unslid (research/metal/tools/ipsw-dl/ipsw dyld a2s CACHE ADDRESS names
 *                             them), the first frames by dladdr as well. objc's exception
 *                             preprocessor sees it while the thrower is still on the stack.
 *   the one that is fatal     a line UNCAUGHT before the daemon aborts (objc's uncaught exception
 *                             handler, chained to CoreFoundation's).
 *   the portrait prewarm      +[BWCoreImageFilterRenderer prewarmPortraitFilterVersion:semanticStyleFilters:]
 *                             does nothing (HackiOS: Core Image wants an OpenGL ES context for it, and
 *                             iOS's own has no functions behind it here: a jump to 0 at every start).
 *                             Only reached with CMCapture's recorder server, which our root does not
 *                             start (SubServerConfiguration.plist StartRecorderServer=false, ios-sweet's
 *                             sweet-root-tweaks.py); kept for when it does.
 *   the audio set-up          one line: whether RedwoodAudio.driver and VirtualAudio.plugin are where
 *                             the HAL looks for them (the two go together: README.md).
 *
 * Off: boot-arg a34media=0 (the library says so and does nothing), or A34_LOAD= in /a34/mediaserverd.env.
 *
 * Ported from HackiOS's userland/src/inject_media.c (libredwood-media.dylib, branch mtk of
 * github.com/oosexgang/hakios-xnu-kernel). Left out: the flashlight and the camera (FigFlashlight's
 * device vendor, camera_device.c: hardware we do not have), Camera.app's probes, the thread dump
 * through kern.redwood.say (not in our kernel). The addresses are of our iOS 16.7.16 (20H392) iPhone X
 * cache (UUID 8f970452-c9e3-3e88-b114-feb1864bc9e0); HackiOS's were 16.5's. A method that is not at
 * the address expected is left alone.
 *
 * MIT License. Copyright (c) 2025-2026 the HakiOS authors; changes Copyright (c) 2026 the a34-ios
 * authors. See README.md for the full text.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include <mach/mach.h>

extern const void *_dyld_get_shared_cache_range(size_t *length);

/* IOKit's (no header in the iOS SDK) */
typedef mach_port_t io_object_t;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_object_t IOServiceGetMatchingService(mach_port_t main_port, CFDictionaryRef matching);
extern CFTypeRef IORegistryEntrySearchCFProperty(io_object_t entry, const char *plane, CFStringRef key, CFAllocatorRef allocator,
    uint32_t options);
extern kern_return_t IOObjectRelease(io_object_t object);

/* libobjc's (objc/objc-exception.h; declared here, the header hides them from some SDKs) */
typedef id (*a34_exception_preprocessor)(id exception);
typedef void (*a34_uncaught_handler)(id exception);
extern a34_exception_preprocessor objc_setExceptionPreprocessor(a34_exception_preprocessor fn);
extern a34_uncaught_handler objc_setUncaughtExceptionHandler(a34_uncaught_handler fn);

#define CACHE_BASE      0x180000000UL   /* where the cache was built to be, before the slide */
#define LINES_MAX       600             /* lines said in all, then quiet: a daemon throwing in a loop */
#define THROWS_NAMED    24              /* throws said with their frames by name; then one line, until: */
#define THROWS_SAID     200             /* then every 100th */

static uintptr_t slide, cache_start, cache_end;
static int pid;
static unsigned lines;
static const char *program = "?";

/* One line for the kernel's log, whatever the daemon's stderr is (launchd's /dev/null): the console. */
static void __attribute__((format(printf, 1, 2)))
say(const char *format, ...)
{
	char line[480];
	va_list ap;
	int n, fd;

	if (__atomic_fetch_add(&lines, 1, __ATOMIC_RELAXED) >= LINES_MAX) {
		return;
	}
	n = snprintf(line, sizeof(line), "a34media: %s[%d]: ", program, pid);
	va_start(ap, format);
	n += vsnprintf(line + n, sizeof(line) - (size_t)n, format, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 2) {
		n = sizeof(line) - 2;
	}
	line[n++] = '\n';
	if ((fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC)) < 0 || write(fd, line, (size_t)n) < 0) {
		(void)write(2, line, (size_t)n);
	}
	if (fd >= 0) {
		close(fd);
	}
}

/* An address as the cache's tools know it: unslid if it is in the shared cache. */
static uintptr_t
unslid(uintptr_t address)
{
	return address >= cache_start && address < cache_end ? address - slide : address;
}

/* ---- the display's alignments (VideoToolbox) ---------------------------------------------------- */

/*
 * FigGetAlignmentForIOSurfaceOutputToDisplayService(&plane_base, &stride), unslid (our 16.7.16 cache): for
 * each answer a "checked" byte and a 32-bit "requirement" in VideoToolbox's data. The instructions that
 * read them are checked before anything is written (they do not move with the slide).
 */
#define FN_DISPLAY_ALIGNMENT    0x194f3b410UL
#define PLANE_BASE_CHECKED      0x1dd2bd853UL   /* _planeBaseRequirement.checked (__bss) */
#define PLANE_BASE_REQUIREMENT  0x1dd2bd42cUL   /* _planeBaseRequirement.requirement (__data) */
#define STRIDE_CHECKED          0x1dd2bd854UL   /* _strideRequirement.checked */
#define STRIDE_REQUIREMENT      0x1dd2bd430UL   /* _strideRequirement.requirement */

static const struct {
	uintptr_t address;
	uint32_t word;
} alignment_code[] = {
	{ 0x194f3b438UL, 0xd0241c19 },  /* adrp x25, _planeBaseRequirement.checked@PAGE */
	{ 0x194f3b43cUL, 0x39614f28 },  /* ldrb w8, [x25, #0x853] */
	{ 0x194f3b440UL, 0xd0241c1a },  /* adrp x26, _planeBaseRequirement.requirement@PAGE */
	{ 0x194f3b448UL, 0x94000637 },  /* bl _getH3clcdService */
	{ 0x194f3b4d8UL, 0xb9442f48 },  /* ldr w8, [x26, #0x42c] */
	{ 0x194f3b4e4UL, 0xd0241c18 },  /* adrp x24, _strideRequirement.checked@PAGE */
	{ 0x194f3b4e8UL, 0x39615308 },  /* ldrb w8, [x24, #0x854] */
	{ 0x194f3b4ecUL, 0xd0241c19 },  /* adrp x25, _strideRequirement.requirement@PAGE */
	{ 0x194f3b584UL, 0xb9443328 },  /* ldr w8, [x25, #0x430] */
};

/* AppleCLCD's property `key`: 1 a dictionary, 0 a number (in *number), -1 neither or no display. */
static int
display_property(const char *key, uint32_t *number)
{
	io_object_t display = IOServiceGetMatchingService(0, IOServiceMatching("AppleCLCD"));
	CFStringRef name = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
	CFTypeRef value = display != 0 && name != NULL ? IORegistryEntrySearchCFProperty(display, "IOService", name, NULL, 0) : NULL;
	int kind = -1;

	if (value != NULL && CFGetTypeID(value) == CFDictionaryGetTypeID()) {
		kind = 1;
	} else if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID() &&
	    CFNumberGetValue((CFNumberRef)value, kCFNumberSInt32Type, number)) {
		kind = 0;
	}
	if (value != NULL) {
		CFRelease(value);
	}
	if (name != NULL) {
		CFRelease(name);
	}
	if (display != 0) {
		IOObjectRelease(display);
	}
	return kind;
}

/* One answer: the requirement, then the byte that says it is there. */
static bool
give_answer(const char *key, uintptr_t checked, uintptr_t requirement)
{
	volatile uint8_t *done = (volatile uint8_t *)(checked + slide);
	volatile uint32_t *value = (volatile uint32_t *)(requirement + slide);
	uint32_t number = 64;           /* (the function's own default, and the kernel's) */
	int kind = display_property(key, &number);

	if (kind == 1) {
		say("display alignment: AppleCLCD's %s is a dictionary: VideoToolbox reads it itself", key);
		return false;
	}
	if (*done != 0) {
		say("display alignment: VideoToolbox has its %s already (%u)", key, (unsigned)*value);
		return false;
	}
	if (number == 0 || (number & (number - 1)) != 0 || number > 4096) {
		number = 64;
	}
	*value = number;
	__atomic_store_n(done, 1, __ATOMIC_RELEASE);
	say("display alignment: VideoToolbox's %s set to %u (AppleCLCD's is %s)", key, (unsigned)number,
	    kind == 0 ? "a number, where it reads a dictionary" : "not there");
	return true;
}

static void
display_alignment(void)
{
	void *function = dlsym(RTLD_DEFAULT, "FigGetAlignmentForIOSurfaceOutputToDisplayService");

	if (slide == 0 && cache_start == 0) {
		return;
	}
	if (function != NULL && (uintptr_t)function != FN_DISPLAY_ALIGNMENT + slide) {
		say("display alignment: FigGetAlignmentForIOSurfaceOutputToDisplayService at %#lx, expected %#lx: left alone",
		    (unsigned long)unslid((uintptr_t)function), (unsigned long)FN_DISPLAY_ALIGNMENT);
		return;
	}
	for (size_t i = 0; i < sizeof(alignment_code) / sizeof(alignment_code[0]); i++) {
		uint32_t word = *(const uint32_t *)(alignment_code[i].address + slide);

		if (word != alignment_code[i].word) {
			say("display alignment: VideoToolbox's code at %#lx is %#x, expected %#x: left alone",
			    (unsigned long)alignment_code[i].address, word, alignment_code[i].word);
			return;
		}
	}
	give_answer("DisplayPipePlaneBaseAlignment", PLANE_BASE_CHECKED, PLANE_BASE_REQUIREMENT);
	give_answer("DisplayPipeStrideRequirements", STRIDE_CHECKED, STRIDE_REQUIREMENT);
}

/* ---- the portrait prewarm (HackiOS) --------------------------------------------------------------- */

/* +[BWCoreImageFilterRenderer prewarmPortraitFilterVersion:semanticStyleFilters:] */
static void
prewarm_portrait_filter(__unused id self, __unused SEL selector, __unused int version, __unused BOOL styles)
{
	static bool said;

	if (!said) {
		said = true;
		say("the portrait filters' prewarm skipped (it wants an OpenGL ES context)");
	}
}

/*
 * The class methods replaced, each as soon as its class exists. `expected` is the unslid address
 * of the method in our 16.7.16 cache (HackiOS's 16.5: 0x1a2115c0c): one that is not there is left alone.
 */
static struct replacement {
	const char *class_name, *selector;
	uintptr_t expected;
	IMP replacement;
	bool done;
} replacements[] = {
	{ "BWCoreImageFilterRenderer", "prewarmPortraitFilterVersion:semanticStyleFilters:", 0x1a2165b8cUL,
	  (IMP)prewarm_portrait_filter, false },
};
static pthread_mutex_t replace_lock = PTHREAD_MUTEX_INITIALIZER;

static void
image_added(__unused const struct mach_header *header, __unused intptr_t image_slide)
{
	pthread_mutex_lock(&replace_lock);
	for (size_t i = 0; i < sizeof(replacements) / sizeof(replacements[0]); i++) {
		struct replacement *r = &replacements[i];
		Class cls;
		Method method;
		IMP original;

		if (r->done || (cls = objc_getClass(r->class_name)) == Nil) {
			continue;
		}
		r->done = true;
		method = class_getClassMethod(cls, sel_registerName(r->selector));
		original = method ? method_getImplementation(method) : NULL;
		if (original == NULL || (uintptr_t)original != r->expected + slide) {
			say("+[%s %s]: found %#lx, expected %#lx: left alone", r->class_name, r->selector,
			    original ? (unsigned long)unslid((uintptr_t)original) : 0UL, (unsigned long)r->expected);
			continue;
		}
		method_setImplementation(method, r->replacement);
		say("+[%s %s] replaced", r->class_name, r->selector);
	}
	pthread_mutex_unlock(&replace_lock);
}

/* ---- exceptions ------------------------------------------------------------------------------------- */

static a34_exception_preprocessor previous_preprocessor;
static a34_uncaught_handler previous_uncaught;
static unsigned throws;
static __thread int busy;               /* (an exception thrown while one is being said is passed on unsaid) */

/* The last throws, to name the fatal one by its number. */
#define REMEMBERED      16
static struct {
	id exception;
	unsigned number;
} remembered[REMEMBERED];

/* At most `size` bytes of an NSString (toll-free a CFString), without allocating. */
static void
text_of(id object, char *out, size_t size)
{
	CFStringRef s = (CFStringRef)object;
	CFIndex used = 0;

	out[0] = 0;
	if (s == NULL || CFGetTypeID(s) != CFStringGetTypeID()) {
		return;
	}
	CFStringGetBytes(s, CFRangeMake(0, CFStringGetLength(s)), kCFStringEncodingUTF8, '?', false, (UInt8 *)out,
	    (CFIndex)size - 1, &used);
	out[used] = 0;
	for (char *c = out; *c; c++) {
		*c = *c == '\n' || *c == '\r' ? ' ' : *c;
	}
}

static id
message(id object, const char *selector)
{
	SEL sel = sel_registerName(selector);

	return object != nil && class_respondsToSelector(object_getClass(object), sel) ?
	       ((id (*)(id, SEL))objc_msgSend)(object, sel) : nil;
}

/* "NAME: REASON" of an exception (an NSException, or whatever object was thrown). */
static void
describe(id exception, char *out, size_t size)
{
	char name[96], reason[320];

	text_of(message(exception, "name"), name, sizeof(name));
	text_of(message(exception, "reason"), reason, sizeof(reason));
	snprintf(out, size, "%s %s: %s", exception ? class_getName(object_getClass(exception)) : "nil", name[0] ? name : "-",
	    reason[0] ? reason : "-");
}

/* Addresses as lines of eight, unslid. */
static void
say_addresses(const char *what, unsigned number, uintptr_t *frames, int count)
{
	for (int i = 0; i < count; i += 8) {
		char line[200];
		size_t used = (size_t)snprintf(line, sizeof(line), "%s %u stack %d-%d:", what, number, i,
		    i + 7 < count - 1 ? i + 7 : count - 1);

		for (int k = i; k < count && k < i + 8 && used < sizeof(line); k++) {
			used += (size_t)snprintf(line + used, sizeof(line) - used, " %#lx", (unsigned long)unslid(frames[k]));
		}
		say("%s", line);
	}
}

/* The first frames by name (exported symbols only: the cache's own are stripped; a2s names the rest). */
static void
say_names(const char *what, unsigned number, uintptr_t *frames, int count, int first, int last)
{
	for (int i = first; i < count && i <= last; i++) {
		Dl_info info;
		const char *image = "?";

		if (dladdr((void *)frames[i], &info) && info.dli_fname != NULL) {
			image = strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname;
			say("%s %u #%d %#lx %s %s+%#lx", what, number, i, (unsigned long)unslid(frames[i]), image,
			    info.dli_sname ? info.dli_sname : "?",
			    (unsigned long)(frames[i] - (uintptr_t)(info.dli_saddr ? info.dli_saddr : info.dli_fbase)));
		} else {
			say("%s %u #%d %#lx %s", what, number, i, (unsigned long)unslid(frames[i]), image);
		}
	}
}

static void install_uncaught(void);

/*
 * objc_exception_throw calls this for every Objective-C exception, before it looks for a handler: the
 * thrower's frames are still on the stack. Caught ones are said too (numbered; mediaserverd seldom
 * throws), so the fatal one may be told apart by its number in the UNCAUGHT line.
 */
static id
preprocess(id exception)
{
	unsigned number;

	if (busy) {
		return previous_preprocessor ? previous_preprocessor(exception) : exception;
	}
	busy = 1;
	install_uncaught();             /* (CoreFoundation's may have come since: ours goes first, then it) */
	number = __atomic_add_fetch(&throws, 1, __ATOMIC_RELAXED);
	remembered[number % REMEMBERED].exception = exception;
	remembered[number % REMEMBERED].number = number;
	if (number <= THROWS_SAID || number % 100 == 0) {
		char text[480];
		const char *queue = dispatch_queue_get_label(DISPATCH_CURRENT_QUEUE_LABEL);
		uintptr_t frames[40];
		int count = backtrace((void **)frames, 40);

		describe(exception, text, sizeof(text));
		say("throw %u on queue \"%s\" (thread %#llx): %s", number, queue ? queue : "?",
		    (unsigned long long)pthread_mach_thread_np(pthread_self()), text);
		if (number <= THROWS_NAMED) {
			/* #0 is this function, #1 objc_exception_throw */
			say_addresses("throw", number, frames + 1, count - 1);
			say_names("throw", number, frames, count, 2, 12);
		}
	}
	busy = 0;
	return previous_preprocessor ? previous_preprocessor(exception) : exception;
}

/* The exception nobody caught: then CoreFoundation's handler, then abort. */
static void
uncaught(id exception)
{
	char text[480];
	unsigned number = 0;
	CFArrayRef addresses = (CFArrayRef)message(exception, "callStackReturnAddresses");
	uintptr_t frames[64];
	int count = 0;

	busy = 1;
	for (unsigned i = 0; i < REMEMBERED; i++) {
		if (remembered[i].exception == exception) {
			number = remembered[i].number;
		}
	}
	describe(exception, text, sizeof(text));
	say("UNCAUGHT exception (throw %u), mediaserverd aborts: %s", number, text);
	if (addresses != NULL && CFGetTypeID(addresses) == CFArrayGetTypeID()) {
		for (CFIndex i = 0; i < CFArrayGetCount(addresses) && count < 64; i++) {
			CFNumberRef value = CFArrayGetValueAtIndex(addresses, i);
			long long address = 0;

			if (value != NULL && CFGetTypeID(value) == CFNumberGetTypeID() &&
			    CFNumberGetValue(value, kCFNumberLongLongType, &address)) {
				frames[count++] = (uintptr_t)address;
			}
		}
		say_addresses("UNCAUGHT", number, frames, count);
		if (number == 0 || number > THROWS_NAMED) {
			say_names("UNCAUGHT", number, frames, count, 0, 12);
		}
	}
	if (previous_uncaught != NULL) {
		previous_uncaught(exception);
	}
}

static pthread_mutex_t install_lock = PTHREAD_MUTEX_INITIALIZER;

static void
install_uncaught(void)
{
	a34_uncaught_handler old;

	pthread_mutex_lock(&install_lock);
	old = objc_setUncaughtExceptionHandler(uncaught);
	if (old != uncaught) {
		previous_uncaught = old;
	}
	pthread_mutex_unlock(&install_lock);
}

static void
install_preprocessor(void)
{
	a34_exception_preprocessor old;

	pthread_mutex_lock(&install_lock);
	old = objc_setExceptionPreprocessor(preprocess);
	if (old != preprocess) {
		previous_preprocessor = old;
	}
	pthread_mutex_unlock(&install_lock);
}

/* ---- the audio set-up ------------------------------------------------------------------------------- */

#define REDWOOD_AUDIO   "/System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver/RedwoodAudio"
#define VIRTUAL_AUDIO   "/Library/Audio/Plug-Ins/HAL/VirtualAudio.plugin"
#define VIRTUAL_AUDIO_OFF "/Library/Audio/Plug-Ins/HAL.disabled/VirtualAudio.plugin"

static void
audio_setup(void)
{
	bool redwood = access(REDWOOD_AUDIO, R_OK) == 0, virtual = access(VIRTUAL_AUDIO, F_OK) == 0;

	say("RedwoodAudio.driver %s, VirtualAudio.plugin %s%s", redwood ? "there" : "NOT there",
	    virtual ? "there" : access(VIRTUAL_AUDIO_OFF, F_OK) == 0 ? "in HAL.disabled" : "NOT there",
	    redwood && virtual ? ": the speaker route can be built" :
	    virtual ? ": VirtualAudio with no device of ours (topaz: it faults at start without one)" :
	    redwood ? ": no route without VirtualAudio (move it back from HAL.disabled)" : ": no audio device at all");
}

static void
install_again(__unused void *context)
{
	install_preprocessor();
	install_uncaught();
}

__attribute__((constructor))
static void
a34media_init(void)
{
	const char *name = getprogname();
	char args[1024] = "";
	size_t size = sizeof(args) - 1, length = 0;
	const void *cache;

	program = name != NULL ? name : "?";
	pid = getpid();
	if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0 && strstr(args, "a34media=0") != NULL) {
		say("loaded, off (boot-arg a34media=0)");
		return;
	}
	if ((cache = _dyld_get_shared_cache_range(&length)) != NULL) {
		cache_start = (uintptr_t)cache;
		cache_end = cache_start + length;
		slide = cache_start - CACHE_BASE;
	}
	if (name == NULL || strcmp(name, "mediaserverd") != 0) {
		say("loaded (cache slide %#lx): the display's alignments only", (unsigned long)slide);
		display_alignment();
		return;
	}
	say("loaded (cache slide %#lx, mediaserverd at %p): exceptions said, the display's alignments, the portrait "
	    "prewarm skipped", (unsigned long)slide, (const void *)_dyld_get_image_header(0));
	display_alignment();
	install_preprocessor();
	install_uncaught();
	_dyld_register_func_for_add_image(image_added);         /* (also called for every image already there) */
	audio_setup();
	/* once the daemon is up, again: whoever set theirs after us (CoreFoundation) is then behind ours */
	dispatch_after_f(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), NULL,
	    install_again);
}
