/*
 * A34Metal.bundle: the Metal plug-in for the Galaxy A34's Mali-G68 under iOS 16.7.16.
 * Milestone 1 of docs/metal-plan.md: a device that Metal.framework finds, loads and
 * registers, and that answers what it is. It makes no resource yet.
 *
 * How Metal reaches this file (read off Metal 306.6.6 in our dyld cache, docs/metal-plan.md
 * section 1): MTLCreateSystemDefaultDevice -> MTLRegisterDevices looks up every
 * IOAcceleratorES service, reads its properties MetalPluginName and MetalPluginClassName,
 * loads /System/Library/Extensions/<MetalPluginName>.bundle, takes the class of that name
 * (else the bundle's principal class), wants it to be a subclass of Metal's private
 * _MTLDevice, sends it -initWithAcceleratorPort:, and hands a non-nil result to
 * MTLAddDevice. MTLAddDevice asserts that the object conforms to the protocols MTLDevice
 * and MTLDeviceSPI, then sends -initLimits and -initFeatureQueries, both _MTLDevice's own,
 * which ask the subclass for -featureProfile and -maxBufferLength.
 *
 * _MTLDevice implements about 400 of the 463 selectors of those two protocols itself
 * (libraries and functions among them). What it leaves out is what a driver is: buffers,
 * textures, samplers, depth/stencil states, heaps, command queues that do something. Until
 * those exist here, a message of the two protocols that neither class implements is
 * answered with zero and named once in the log (-forwardInvocation: below), so that a
 * program which asks is told no and the log says what it asked for.
 *
 * The scheme -- subclass _MTLDevice directly, declare MTLDeviceSPI at run time, answer the
 * few methods the registration needs, name every refused message once, show the device to
 * chosen programs only -- is HackiOS's (OOSEX GANG, MIT: hakios-xnu-kernel
 * hakios-metal/plugin/HakiosMetal.m and, on branch mtk, userland/src/redwoodmetal.m);
 * the code here is written for this port. See README.md for the licence text.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "a34metal_iokit.h"

#define A34METAL_CLIENTS "/a34/A34Metal.clients"

/* ---- the log: stderr, with the program it was written in (a plug-in runs in other
 * programs' processes; launchd jobs of ours have stderr on the console, and the kernel's
 * devnull tee shows backboardd's and SpringBoard's) */

static void a34m_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void
a34m_log(const char *format, ...)
{
	char line[512];
	va_list ap;

	va_start(ap, format);
	vsnprintf(line, sizeof(line), format, ap);
	va_end(ap);
	fprintf(stderr, "a34metal[%s %d]: %s\n", getprogname(), (int)getpid(), line);
	fflush(stderr);
}

/* ---- switches: boot-args (as liba34gl reads its own) and the environment */

static char a34m_boot_args[1024];
static pthread_once_t a34m_boot_args_once = PTHREAD_ONCE_INIT;

static void
a34m_read_boot_args(void)
{
	size_t size = sizeof(a34m_boot_args) - 1;

	if (sysctlbyname("kern.bootargs", a34m_boot_args, &size, NULL, 0) != 0) {
		a34m_boot_args[0] = '\0';
	}
	a34m_boot_args[sizeof(a34m_boot_args) - 1] = '\0';
}

/* NAME=N among the boot-args, else the fallback */
static long
a34m_boot_arg(const char *name, long fallback)
{
	size_t length = strlen(name);
	const char *at;

	pthread_once(&a34m_boot_args_once, a34m_read_boot_args);
	for (at = a34m_boot_args; (at = strstr(at, name)) != NULL; at += length) {
		if ((at == a34m_boot_args || at[-1] == ' ') && at[length] == '=') {
			return strtol(at + length + 1, NULL, 0);
		}
	}
	return fallback;
}

/*
 * Who is shown the device. Publishing MetalPluginName makes every process that asks Metal
 * load this bundle, and a device that cannot draw yet ends the ones that then draw with
 * it: the window server gives up when it cannot make a command queue, and
 * CoreAnimation prefers Metal to the OpenGL ES that works (HackiOS measured both, its
 * docs/metal-plan.md). So the answer is per program, and "no" is the default:
 *
 *   A34METAL=0 | 1 in the environment          no | yes, whatever else says
 *   metal-probe, a34metal-*                    yes (the tests)
 *   a line in /a34/A34Metal.clients            yes for the program of that name; "*": all
 *   boot-arg a34metal=2                        yes for all but backboardd
 *   boot-arg a34metal=3                        yes for all
 */
static BOOL
a34m_client_allowed(const char **why)
{
	const char *environment = getenv("A34METAL");
	const char *program = getprogname();
	long level;
	FILE *file;

	if (environment != NULL && environment[0] != '\0') {
		*why = "A34METAL in the environment";
		return strcmp(environment, "0") != 0;
	}
	if (strcmp(program, "metal-probe") == 0 || strncmp(program, "a34metal-", 9) == 0) {
		*why = "a test program";
		return YES;
	}
	file = fopen(A34METAL_CLIENTS, "r");
	if (file != NULL) {
		char line[128];
		BOOL named = NO;

		while (!named && fgets(line, sizeof(line), file) != NULL) {
			size_t end = strcspn(line, " \t\r\n");

			line[end] = '\0';
			named = line[0] != '#' && (strcmp(line, "*") == 0 || strcmp(line, program) == 0);
		}
		fclose(file);
		if (named) {
			*why = "named in " A34METAL_CLIENTS;
			return YES;
		}
	}
	level = a34m_boot_arg("a34metal", 1);
	if (level >= 3 || (level == 2 && strcmp(program, "backboardd") != 0)) {
		*why = "boot-arg a34metal";
		return YES;
	}
	*why = "not a test program, not in " A34METAL_CLIENTS ", boot-arg a34metal below 2";
	return NO;
}

/* ---- Metal's private base class: only what is used of it, nothing of its layout */

@interface _MTLDevice : NSObject
- (instancetype)init;           /* its own; -initWithAcceleratorPort: there is abstract (answers nil) */
@end

/*
 * -targetDeviceInfo: _MTLDevice's own answers NULL (two instructions in 306.6.6), and
 * HackiOS found Metal's pipeline-library path reading the name at +0x10 without looking.
 * The layout is the method's type encoding in MTLDeviceSPI, {MTLTargetDeviceArch=QI*};
 * named differently here in case an SDK ever declares Apple's.
 */
struct A34MetalTargetDeviceArch {
	uint64_t target;
	uint32_t version;
	const char *name;       /* formatted into "<pipelinelib>/pipelines.%s" */
};

_Static_assert(sizeof(struct A34MetalTargetDeviceArch) == 24 &&
    __builtin_offsetof(struct A34MetalTargetDeviceArch, version) == 8 &&
    __builtin_offsetof(struct A34MetalTargetDeviceArch, name) == 16, "{MTLTargetDeviceArch=QI*} on arm64");

@interface A34MetalDevice : _MTLDevice {
	io_service_t _accelerator;      /* the IOAcceleratorES service Metal found us by */
	io_connect_t _gpu;              /* RedwoodMali's user client, or IO_OBJECT_NULL */
	uint64_t _productID;            /* GPU_PROD_ID as the kernel reports it, 0 without a GPU */
	NSString *_name;
	BOOL _assertions;
	struct A34MetalTargetDeviceArch _target;
	char _targetName[64];
}
@end

/* The gate of MTLAddDevice: [device conformsToProtocol:] of MTLDevice and of MTLDeviceSPI,
 * each a fatal assertion. MTLDeviceSPI is private (352 required methods) and adopts
 * MTLDevice; stated at run time, which is all the two assertions read. */
static void
a34m_declare_protocols(Class cls)
{
	static const char *const names[] = { "MTLDevice", "MTLDeviceSPI" };
	size_t i;

	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		Protocol *protocol = objc_getProtocol(names[i]);

		if (protocol == NULL) {
			a34m_log("protocol %s is not in this process: MTLAddDevice would refuse the device", names[i]);
		} else if (!class_conformsToProtocol(cls, protocol) && !class_addProtocol(cls, protocol)) {
			a34m_log("class_addProtocol(%s) failed", names[i]);
		}
	}
}

/* the type encoding of an instance method of the two protocols, NULL if it is in neither */
static const char *
a34m_protocol_types(SEL selector)
{
	static const char *const names[] = { "MTLDeviceSPI", "MTLDevice" };
	size_t i;
	int required;

	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		Protocol *protocol = objc_getProtocol(names[i]);

		if (protocol == NULL) {
			continue;
		}
		for (required = 1; required >= 0; required--) {
			struct objc_method_description description =
			    protocol_getMethodDescription(protocol, selector, required ? YES : NO, YES);

			if (description.name != NULL && description.types != NULL) {
				return description.types;
			}
		}
	}
	return NULL;
}

static void
a34m_note_unimplemented(SEL selector)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	static NSMutableSet *said;
	NSString *name = NSStringFromSelector(selector);
	BOOL first;

	pthread_mutex_lock(&lock);
	if (said == nil) {
		said = [[NSMutableSet alloc] init];
	}
	first = ![said containsObject:name];
	if (first) {
		[said addObject:name];
	}
	pthread_mutex_unlock(&lock);
	if (first) {
		a34m_log("-[A34MetalDevice %s] is not implemented yet: answered with zero", sel_getName(selector));
	}
}

@implementation A34MetalDevice

+ (void)load
{
	a34m_declare_protocols(self);
}

- (instancetype)initWithAcceleratorPort:(io_service_t)port
{
	const char *why = "";
	io_service_t service;

	if (!a34m_client_allowed(&why)) {
		a34m_log("no device for this program (%s)", why);
		/* Not released: -[_MTLDevice dealloc] on an object whose -init never ran gives
		 * libdispatch a null queue (HackiOS, redwoodmetal.m). Metal asks once a process. */
		return nil;
	}
	a34m_declare_protocols([self class]);

	self = [super init];
	if (self == nil) {
		a34m_log("-[_MTLDevice init] answered nil");
		return nil;
	}
	_accelerator = port;
	if (port != IO_OBJECT_NULL) {
		IOObjectRetain(port);
	}

	/* The GPU itself is the kernel's RedwoodMali service (userland/mesa/README.md), not the
	 * accelerator service Metal found the plug-in by. A device without it still answers. */
	_gpu = IO_OBJECT_NULL;
	service = IOServiceGetMatchingService(A34METAL_IOKIT_PORT, IOServiceMatching("RedwoodMali"));
	if (service == IO_OBJECT_NULL) {
		a34m_log("no RedwoodMali service (boot-arg a34mali=1 starts the GPU)");
	} else {
		kern_return_t result = IOServiceOpen(service, mach_task_self(), 0, &_gpu);

		if (result != KERN_SUCCESS) {
			a34m_log("RedwoodMali did not open: %#x", result);
			_gpu = IO_OBJECT_NULL;
		} else {
			struct a34metal_get_param request = { A34METAL_PARAM_PROD_ID, 0, 0 };
			size_t size = sizeof(request);

			result = IOConnectCallStructMethod(_gpu, A34METAL_SEL_GET_PARAM, &request, sizeof(request),
			    &request, &size);
			if (result == KERN_SUCCESS) {
				_productID = request.value;
			} else {
				a34m_log("RedwoodMali GET_PARAM(GPU_PROD_ID): %#x", result);
			}
		}
		IOObjectRelease(service);
	}
	if (_productID == 0x9204) {
		_name = @"Mali-G68 (A34Metal)";
	} else if (_productID != 0) {
		_name = [[NSString alloc] initWithFormat:@"Mali %#llx (A34Metal)", (unsigned long long)_productID];
	} else {
		_name = @"A34Metal (no GPU)";
	}

	snprintf(_targetName, sizeof(_targetName), "A34Metal");
	_target.target = 0;
	_target.version = 0;
	_target.name = _targetName;

	a34m_log("device \"%s\" for this program (%s); accelerator port %#x, feature profile %lu",
	    [_name UTF8String], why, port, (unsigned long)[self featureProfile]);
	return self;
}

- (void)dealloc
{
	if (_gpu != IO_OBJECT_NULL) {
		IOServiceClose(_gpu);
	}
	if (_accelerator != IO_OBJECT_NULL) {
		IOObjectRelease(_accelerator);
	}
	[_name release];
	[super dealloc];
}

/* ---- what the registration asks */

/*
 * -[_MTLDevice initLimits] and -initGPUFamilySupport read everything out of this number.
 * In 306.6.6 initLimits has a column for 0, 1 and 4...9 and asserts
 * "_limits.maxColorAttachments > 0" for anything else. HackiOS measured on 16.5: 0 claims
 * the families Apple1 and Common1 only, 1 adds Apple2, which is the lowest that Metal
 * Performance Shaders accepts; Apple's own A11 driver answers 5. Every family is a set of
 * promises, so the lowest that keeps the system's own clients alive is the default, and
 * metal-probe prints what each value claims (A34METAL_PROFILE=N, boot-arg a34metal_profile=N).
 */
- (NSUInteger)featureProfile
{
	static long profile = -1;

	if (profile < 0) {
		const char *environment = getenv("A34METAL_PROFILE");
		long asked = environment != NULL ? strtol(environment, NULL, 0) : a34m_boot_arg("a34metal_profile", 1);

		if (asked != 0 && asked != 1 && (asked < 4 || asked > 9)) {
			a34m_log("feature profile %ld is not one Metal has limits for (0, 1, 4...9): 1", asked);
			asked = 1;
		}
		profile = asked;
	}
	return (NSUInteger)profile;
}

/* The kernel's buffer sizes are 32-bit and every program's buffers together are held to
 * boot-arg a34malimem MB (default 1024): a quarter of that, until a buffer is measured. */
- (NSUInteger)maxBufferLength
{
	return (NSUInteger)256 << 20;
}

- (NSString *)name
{
	return _name;
}

- (uint64_t)registryID
{
	uint64_t entry = 0;

	if (_accelerator != IO_OBJECT_NULL) {
		IORegistryEntryGetRegistryEntryID(_accelerator, &entry);
	}
	return entry;
}

- (io_service_t)acceleratorPort
{
	return _accelerator;
}

- (const struct A34MetalTargetDeviceArch *)targetDeviceInfo
{
	return &_target;
}

/* ---- what this GPU is */

- (BOOL)hasUnifiedMemory
{
	return YES;
}

- (BOOL)isHeadless
{
	return NO;
}

- (BOOL)isLowPower
{
	return NO;
}

- (BOOL)isRemovable
{
	return NO;
}

/* CoreImage reads the two sizes for its limits and throws without them (UniXNU's
 * AdrenoMetal found that; HackiOS's docs/metal-plan.md). Half of the kernel's default
 * allowance for all programs, until the kernel says what it has. */
- (uint64_t)sharedMemorySize
{
	return (uint64_t)512 << 20;
}

- (uint64_t)dedicatedMemorySize
{
	return 0;
}

- (uint64_t)recommendedMaxWorkingSetSize
{
	return (uint64_t)512 << 20;
}

- (NSUInteger)currentAllocatedSize
{
	return 0;       /* nothing is allocated through this device yet */
}

- (BOOL)supportsVertexAmplificationCount:(NSUInteger)count
{
	return count == 1;
}

- (BOOL)supportsSampleCount:(NSUInteger)count
{
	return count == 1;      /* no multisampling yet */
}

- (BOOL)metalAssertionsEnabled
{
	return _assertions;
}

- (void)setMetalAssertionsEnabled:(BOOL)enabled
{
	_assertions = enabled;
}

/* ---- everything else of MTLDevice and MTLDeviceSPI that nobody implements: zero, named once */

- (NSMethodSignature *)methodSignatureForSelector:(SEL)selector
{
	NSMethodSignature *signature = [super methodSignatureForSelector:selector];
	const char *types;

	if (signature != nil) {
		return signature;
	}
	types = a34m_protocol_types(selector);
	return types != NULL ? [NSMethodSignature signatureWithObjCTypes:types] : nil;
}

- (void)forwardInvocation:(NSInvocation *)invocation
{
	NSUInteger length = [[invocation methodSignature] methodReturnLength];

	a34m_note_unimplemented([invocation selector]);
	if (length != 0) {
		void *zero = calloc(1, length);

		if (zero != NULL) {
			[invocation setReturnValue:zero];
			free(zero);
		}
	}
}

@end
