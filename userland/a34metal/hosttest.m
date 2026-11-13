/*
 * a34metal-hosttest: the plug-in's own logic against the build machine's Metal (macOS), since
 * nothing here can run the iOS build. Not for the phone.
 *
 *   a34metal-hosttest load PLUGIN   the plug-in loads, its class is a subclass of _MTLDevice,
 *                                   states the protocols MTLAddDevice asserts and has the
 *                                   methods the registration sends (exit 0, else 1)
 *   a34metal-hosttest make PLUGIN   also makes a device as Metal does and asks it a few
 *                                   things. macOS's Metal is not iOS 16.7's: what this shows
 *                                   is the plug-in's side, and the build does not depend on it.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void
check(int ok, const char *what)
{
	printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
	fflush(stdout);
	if (!ok) {
		failures++;
	}
}

int
main(int argc, char **argv)
{
	@autoreleasepool {
		Class cls, base, walk;
		Protocol *spi;
		int below_base = 0;
		id device;

		if (argc != 3 || (strcmp(argv[1], "load") != 0 && strcmp(argv[1], "make") != 0)) {
			fprintf(stderr, "usage: a34metal-hosttest load|make PLUGIN\n");
			return 64;
		}
		printf("a34metal-hosttest %s: %s\n", argv[1], argv[2]);
		if (dlopen(argv[2], RTLD_NOW) == NULL) {
			printf("  FAIL dlopen: %s\n", dlerror());
			return 1;
		}
		cls = objc_getClass("A34MetalDevice");
		base = objc_getClass("_MTLDevice");
		spi = objc_getProtocol("MTLDeviceSPI");
		check(cls != Nil, "class A34MetalDevice is registered");
		check(base != Nil, "this Metal has _MTLDevice");
		if (cls == Nil || base == Nil) {
			return 1;
		}
		for (walk = class_getSuperclass(cls); walk != Nil; walk = class_getSuperclass(walk)) {
			if (walk == base) {
				below_base = 1;
			}
		}
		check(below_base, "A34MetalDevice is a subclass of _MTLDevice (getMetalPluginClassForService's test)");
		check([cls conformsToProtocol:@protocol(MTLDevice)], "conforms to MTLDevice (MTLAddDevice's first assertion)");
		if (spi != NULL) {
			check([cls conformsToProtocol:spi], "conforms to MTLDeviceSPI (MTLAddDevice's second assertion)");
		} else {
			printf("  note this Metal has no protocol MTLDeviceSPI\n");
		}
		check([cls instancesRespondToSelector:sel_registerName("initWithAcceleratorPort:")], "-initWithAcceleratorPort:");
		check([cls instancesRespondToSelector:sel_registerName("featureProfile")], "-featureProfile (initLimits)");
		check([cls instancesRespondToSelector:sel_registerName("maxBufferLength")], "-maxBufferLength (initLimits)");
		if (strcmp(argv[1], "load") == 0 || failures != 0) {
			printf("%s\n", failures == 0 ? "load: ok" : "load: FAILED");
			return failures == 0 ? 0 : 1;
		}

		printf("  making a device (-initWithAcceleratorPort:0) ...\n");
		fflush(stdout);
		device = ((id (*)(id, SEL, unsigned int))objc_msgSend)([cls alloc], sel_registerName("initWithAcceleratorPort:"),
		    0);
		check(device != nil, "the device is made");
		if (device == nil) {
			return 1;
		}
		printf("  name \"%s\", featureProfile %lu, maxBufferLength %lu MB\n", [[(id<MTLDevice>)device name] UTF8String],
		    (unsigned long)((NSUInteger (*)(id, SEL))objc_msgSend)(device, sel_registerName("featureProfile")),
		    (unsigned long)([(id<MTLDevice>)device maxBufferLength] >> 20));
		fflush(stdout);
		check(![device respondsToSelector:sel_registerName("newBufferWithLength:options:")],
		    "-newBufferWithLength:options: is not claimed (respondsToSelector: stays truthful)");
		check([(id<MTLDevice>)device newBufferWithLength:4096 options:0] == nil,
		    "a method of MTLDevice nobody implements answers nil (and is named in the log once)");
		check([(id<MTLDevice>)device newBufferWithLength:4096 options:0] == nil, "and again, without a second line");
		printf("%s\n", failures == 0 ? "make: ok" : "make: FAILED");
		return failures == 0 ? 0 : 1;
	}
}
