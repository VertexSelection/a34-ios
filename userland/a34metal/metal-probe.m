/*
 * metal-probe: does Metal.framework find a device here, and what does it say it is?
 * Milestone 1's test (docs/metal-plan.md). One-shot, prints to stdout, ends with PASS or FAIL.
 *
 *   metal-probe            what a program gets: MTLCreateSystemDefaultDevice()
 *   metal-probe --direct [BUNDLE]
 *                          when the kernel's IOAcceleratorES does not name the plug-in yet:
 *                          load BUNDLE (default /System/Library/Extensions/A34Metal.bundle),
 *                          make its principal class as Metal would
 *                          (-initWithAcceleratorPort:) and register it with Metal's own
 *                          MTLAddDevice -- the same assertions, limits and feature
 *                          queries -- then ask MTLCreateSystemDefaultDevice() again
 *   metal-probe --any      PASS for any device (default: only for class A34MetalDevice)
 *   metal-probe --next     after the verdict, also ask for a command queue and a buffer
 *                          (milestone 2's first questions; not judged)
 *
 * exit 0 PASS, 1 no device, 2 a device of another class, 3 the device is not as Metal wants it.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <dlfcn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "a34metal_iokit.h"

#define DEFAULT_BUNDLE "/System/Library/Extensions/A34Metal.bundle"

static void
say(const char *format, ...) __attribute__((format(printf, 1, 2)));

static void
say(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vprintf(format, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);         /* a line a step: if a step ends the program, the log says which */
}

/* a string property of a registry entry into a buffer; false if absent or not a string */
static bool
string_property(io_registry_entry_t entry, CFStringRef key, char *buffer, size_t size)
{
	CFTypeRef value = IORegistryEntryCreateCFProperty(entry, key, kCFAllocatorDefault, 0);
	bool ok = false;

	if (value != NULL) {
		ok = CFGetTypeID(value) == CFStringGetTypeID() &&
		    CFStringGetCString((CFStringRef)value, buffer, (CFIndex)size, kCFStringEncodingUTF8);
		CFRelease(value);
	}
	return ok;
}

/* What Metal will look at: every IOAcceleratorES service and its two properties. Returns the first service. */
static io_service_t
registry(void)
{
	io_iterator_t iterator = IO_OBJECT_NULL;
	io_service_t service, first = IO_OBJECT_NULL;
	unsigned count = 0;

	if (IOServiceGetMatchingServices(A34METAL_IOKIT_PORT, IOServiceMatching("IOAcceleratorES"), &iterator) !=
	    KERN_SUCCESS) {
		say("registry: IOServiceGetMatchingServices(IOAcceleratorES) failed");
		return IO_OBJECT_NULL;
	}
	while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
		char name[128] = "", class_name[128] = "", path[256];
		struct stat status;
		uint64_t entry = 0;

		count++;
		IORegistryEntryGetRegistryEntryID(service, &entry);
		if (!string_property(service, CFSTR("MetalPluginName"), name, sizeof(name))) {
			say("registry: IOAcceleratorES %#llx has no MetalPluginName: Metal passes it over",
			    (unsigned long long)entry);
		} else {
			string_property(service, CFSTR("MetalPluginClassName"), class_name, sizeof(class_name));
			snprintf(path, sizeof(path), "/System/Library/Extensions/%s.bundle", name);
			say("registry: IOAcceleratorES %#llx MetalPluginName \"%s\" MetalPluginClassName \"%s\"; %s %s",
			    (unsigned long long)entry, name, class_name, path,
			    stat(path, &status) == 0 ? "is there" : "IS MISSING");
		}
		if (first == IO_OBJECT_NULL) {
			first = service;
		} else {
			IOObjectRelease(service);
		}
	}
	IOObjectRelease(iterator);
	if (count == 0) {
		say("registry: no IOAcceleratorES service (boot-arg sweetaccel=0?)");
	}
	service = IOServiceGetMatchingService(A34METAL_IOKIT_PORT, IOServiceMatching("RedwoodMali"));
	say("registry: RedwoodMali (the GPU's own service) %s", service != IO_OBJECT_NULL ? "is there" : "is not there");
	if (service != IO_OBJECT_NULL) {
		IOObjectRelease(service);
	}
	return first;
}

/* --direct: the bundle's device, made and registered the way MTLRegisterDevices does it */
static bool
direct(const char *path, io_service_t accelerator)
{
	void (*add_device)(id) = (void (*)(id))dlsym(RTLD_DEFAULT, "MTLAddDevice");
	NSBundle *bundle = [NSBundle bundleWithPath:[NSString stringWithUTF8String:path]];
	Class base = objc_getClass("_MTLDevice");
	Class cls;
	id device;

	if (bundle == nil) {
		say("direct: no bundle at %s", path);
		return false;
	}
	cls = [bundle principalClass];
	if (cls == Nil) {
		say("direct: %s did not load or has no principal class", path);
		return false;
	}
	say("direct: %s loaded, principal class %s", path, class_getName(cls));
	if (base == Nil || ![cls isSubclassOfClass:base]) {
		say("direct: %s is not a subclass of _MTLDevice: Metal would pass it over", class_getName(cls));
		return false;
	}
	if (add_device == NULL) {
		say("direct: this Metal exports no MTLAddDevice");
		return false;
	}
	say("direct: -initWithAcceleratorPort:%#x ...", accelerator);
	device = ((id (*)(id, SEL, unsigned int))objc_msgSend)([cls alloc], sel_registerName("initWithAcceleratorPort:"),
	    accelerator);
	if (device == nil) {
		say("direct: the class answered nil (the plug-in's log line says why)");
		return false;
	}
	say("direct: MTLAddDevice(%s %p) ... (asserts MTLDevice and MTLDeviceSPI, then -initLimits, -initFeatureQueries)",
	    class_getName(object_getClass(device)), (void *)device);
	add_device(device);
	say("direct: registered");
	return true;
}

static const char *
yes_no(BOOL value)
{
	return value ? "yes" : "no";
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		bool want_direct = false, any = false, next = false;
		const char *bundle = DEFAULT_BUNDLE;
		io_service_t accelerator;
		id<MTLDevice> device;
		NSArray *(*copy_all)(void);
		const char *class_name;
		NSString *name;
		int i;

		static const struct {
			const char *name;
			NSInteger family;
		} families[] = {
			{ "Apple1", 1001 }, { "Apple2", 1002 }, { "Apple3", 1003 }, { "Apple4", 1004 }, { "Apple5", 1005 },
			{ "Common1", 3001 }, { "Common2", 3002 }, { "Common3", 3003 },
		};
		static const struct {
			const char *name;
			NSUInteger set;
		} feature_sets[] = {    /* MTLFeatureSet_iOS_*: what CoreAnimation asks with */
			{ "GPUFamily1_v1", 0 }, { "GPUFamily2_v1", 1 }, { "GPUFamily3_v1", 4 }, { "GPUFamily4_v1", 11 },
			{ "GPUFamily5_v1", 16 },
		};

		for (i = 1; i < argc; i++) {
			if (strcmp(argv[i], "--direct") == 0) {
				want_direct = true;
				if (i + 1 < argc && argv[i + 1][0] != '-') {
					bundle = argv[++i];
				}
			} else if (strcmp(argv[i], "--any") == 0) {
				any = true;
			} else if (strcmp(argv[i], "--next") == 0) {
				next = true;
			} else {
				fprintf(stderr, "usage: metal-probe [--direct [BUNDLE]] [--any] [--next]\n");
				return 64;
			}
		}
		say("metal-probe: Metal's device discovery (docs/metal-plan.md, milestone 1)");
		accelerator = registry();

		say("MTLCreateSystemDefaultDevice() ...");
		device = MTLCreateSystemDefaultDevice();
		say("MTLCreateSystemDefaultDevice() = %p", (void *)device);
		if (device == nil && want_direct) {
			if (direct(bundle, accelerator)) {
				device = MTLCreateSystemDefaultDevice();
				say("MTLCreateSystemDefaultDevice() after the registration = %p", (void *)device);
			}
		}

		copy_all = (NSArray *(*)(void))dlsym(RTLD_DEFAULT, "MTLCopyAllDevices");
		if (copy_all == NULL) {
			say("MTLCopyAllDevices: not in this Metal (iOS has it from 18.0)");
		} else {
			NSArray *all = copy_all();

			say("MTLCopyAllDevices() = %lu device(s)", (unsigned long)[all count]);
			[all release];
		}

		if (device == nil) {
			say("FAIL: Metal has no device%s", want_direct ? "" :
			    " (no MetalPluginName on IOAcceleratorES, the bundle is missing, or the plug-in "
			    "refused this program: see the lines above and a34metal's own)");
			return 1;
		}

		class_name = class_getName(object_getClass(device));
		name = [device name];
		say("device: class %s, name \"%s\", registryID %#llx", class_name, name != nil ? [name UTF8String] : "(nil)",
		    (unsigned long long)[device registryID]);
		say("device: conforms to MTLDevice %s, to MTLDeviceSPI %s",
		    yes_no([device conformsToProtocol:@protocol(MTLDevice)]),
		    yes_no(objc_getProtocol("MTLDeviceSPI") != NULL &&
		    [device conformsToProtocol:objc_getProtocol("MTLDeviceSPI")]));
		say("device: hasUnifiedMemory %s, maxBufferLength %lu MB, recommendedMaxWorkingSetSize %llu MB",
		    yes_no([device hasUnifiedMemory]), (unsigned long)([device maxBufferLength] >> 20),
		    (unsigned long long)([device recommendedMaxWorkingSetSize] >> 20));
		if ([device respondsToSelector:sel_registerName("featureProfile")]) {
			say("device: featureProfile %lu", (unsigned long)((NSUInteger (*)(id, SEL))objc_msgSend)(device,
			    sel_registerName("featureProfile")));
		}
		for (i = 0; i < (int)(sizeof(families) / sizeof(families[0])); i++) {
			say("device: supportsFamily %-8s %s", families[i].name,
			    yes_no([device supportsFamily:(MTLGPUFamily)families[i].family]));
		}
		for (i = 0; i < (int)(sizeof(feature_sets) / sizeof(feature_sets[0])); i++) {
			say("device: supportsFeatureSet iOS_%-14s %s", feature_sets[i].name,
			    yes_no(((BOOL (*)(id, SEL, NSUInteger))objc_msgSend)(device, sel_registerName("supportsFeatureSet:"),
			    feature_sets[i].set)));
		}
		say("device: argumentBuffersSupport tier %lu, readWriteTextureSupport tier %lu",
		    (unsigned long)[device argumentBuffersSupport], (unsigned long)[device readWriteTextureSupport]);
		say("device: supportsTextureSampleCount 1 %s, 4 %s", yes_no([device supportsTextureSampleCount:1]),
		    yes_no([device supportsTextureSampleCount:4]));

		if (![device conformsToProtocol:@protocol(MTLDevice)] || name == nil || [name length] == 0) {
			say("FAIL: a device, but without the MTLDevice protocol or a name");
			return 3;
		}
		if (!any && strcmp(class_name, "A34MetalDevice") != 0) {
			say("FAIL: a device, but of class %s, not A34MetalDevice (--any accepts it)", class_name);
			return 2;
		}
		say("PASS: Metal's default device is %s \"%s\"", class_name, [name UTF8String]);
		if (next) {
			/* Not part of the verdict: milestone 2's first questions. */
			@try {
				id<MTLCommandQueue> queue;
				id<MTLBuffer> buffer;

				say("next: newCommandQueue ...");
				queue = [device newCommandQueue];
				say("next: newCommandQueue = %p", (void *)queue);
				[queue release];
				say("next: newBufferWithLength:4096 ...");
				buffer = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
				say("next: newBufferWithLength:4096 = %p", (void *)buffer);
				[buffer release];
			} @catch (NSException *exception) {
				say("next: exception %s: %s", [[exception name] UTF8String], [[exception reason] UTF8String]);
			}
		}
		[device release];
		return 0;
	}
}
