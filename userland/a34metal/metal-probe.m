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
 *   metal-probe --any      PASS for any device (default: only for class RedwoodMetalDevice)
 *   metal-probe --next     after the verdict, also ask for a command queue and a buffer
 *                          (milestone 2's first questions; not judged)
 *   metal-probe --compute [LIB]   then run kernel a34_fill of LIB (default
 *                          /a34/a34metal-tests.metallib, a34metal-tests.metal) over 1024 of a
 *                          buffer's 4096 words and check every word: 3i + 7 where it ran, the
 *                          canary where it did not
 *   metal-probe --draw [LIB]   then into a 64x64 BGRA IOSurface through Metal's own
 *                          -newTextureWithDescriptor:iosurface:plane:: a pass that only clears,
 *                          checked pixel by pixel in the surface's memory; then a pass that
 *                          clears and draws a34_tri_vs/a34_tri_fs's red triangle, checked at
 *                          places inside and outside it
 *
 * exit 0 PASS, 1 no device, 2 a device of another class, 3 the device is not as Metal wants it,
 * 4 a --compute or --draw check failed.
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
#include <IOSurface/IOSurfaceRef.h>

#define DEFAULT_TESTS "/a34/a34metal-tests.metallib"

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

/* ---- end to end: our own shaders through Metal's public API and the driver ---- */

static id<MTLLibrary>
tests_library(id<MTLDevice> device, const char *path)
{
	NSError *error = nil;
	id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:path]]
						    error:&error];

	if (library == nil) {
		say("FAIL: newLibraryWithURL:%s: %s", path, error != nil ? [[error localizedDescription] UTF8String] : "nil");
		return nil;
	}
	say("library %s: %lu functions", path, (unsigned long)[[library functionNames] count]);
	return library;
}

static const char *
status_name(MTLCommandBufferStatus status)
{
	static const char *const names[] = { "NotEnqueued", "Enqueued", "Committed", "Scheduled", "Completed", "Error" };

	return (unsigned)status < 6 ? names[status] : "?";
}

/* the command buffer completed without an error, said either way */
static bool
completed(id<MTLCommandBuffer> cb, const char *what)
{
	if ([cb status] != MTLCommandBufferStatusCompleted || [cb error] != nil) {
		say("FAIL: %s: command buffer %s%s%s", what, status_name([cb status]), [cb error] != nil ? ": " : "",
		    [cb error] != nil ? [[[cb error] localizedDescription] UTF8String] : "");
		return false;
	}
	return true;
}

static bool
compute_test(id<MTLDevice> device, const char *path)
{
	const NSUInteger words = 4096, grid = 1024, group = 64;
	const uint32_t canary = 0xdeadbeef;
	id<MTLLibrary> library = tests_library(device, path);
	id<MTLFunction> function;
	id<MTLComputePipelineState> pipeline;
	id<MTLCommandQueue> queue;
	id<MTLCommandBuffer> cb;
	id<MTLComputeCommandEncoder> encoder;
	id<MTLBuffer> buffer;
	NSError *error = nil;
	uint32_t *w;
	NSUInteger wrong = 0, first_wrong = words;

	if (library == nil) {
		return false;
	}
	function = [library newFunctionWithName:@"a34_fill"];
	if (function == nil) {
		say("FAIL: compute: no function a34_fill");
		return false;
	}
	pipeline = [device newComputePipelineStateWithFunction:function error:&error];
	if (pipeline == nil) {
		say("FAIL: compute: newComputePipelineStateWithFunction: %s",
		    error != nil ? [[error localizedDescription] UTF8String] : "nil");
		return false;
	}
	say("compute: a34_fill compiled; maxTotalThreadsPerThreadgroup %lu, threadExecutionWidth %lu",
	    (unsigned long)[pipeline maxTotalThreadsPerThreadgroup], (unsigned long)[pipeline threadExecutionWidth]);
	buffer = [device newBufferWithLength:words * 4 options:MTLResourceStorageModeShared];
	queue = [device newCommandQueue];
	if (buffer == nil || queue == nil) {
		say("FAIL: compute: buffer %p, queue %p", (void *)buffer, (void *)queue);
		return false;
	}
	w = (uint32_t *)[buffer contents];
	for (NSUInteger i = 0; i < words; i++) {
		w[i] = canary;
	}
	cb = [queue commandBuffer];
	encoder = [cb computeCommandEncoder];
	[encoder setComputePipelineState:pipeline];
	[encoder setBuffer:buffer offset:0 atIndex:0];
	[encoder dispatchThreadgroups:MTLSizeMake(grid / group, 1, 1) threadsPerThreadgroup:MTLSizeMake(group, 1, 1)];
	[encoder endEncoding];
	[cb commit];
	[cb waitUntilCompleted];
	if (!completed(cb, "compute")) {
		return false;
	}
	for (NSUInteger i = 0; i < words; i++) {
		uint32_t want = i < grid ? (uint32_t)i * 3u + 7u : canary;

		if (w[i] != want) {
			if (wrong++ == 0) {
				first_wrong = i;
			}
		}
	}
	say("compute: words 0..3 %#x %#x %#x %#x, 1023 %#x, 1024 %#x", w[0], w[1], w[2], w[3], w[1023], w[1024]);
	if (wrong != 0) {
		say("FAIL: compute: %lu of %lu words wrong, the first at %lu (%#x)", (unsigned long)wrong,
		    (unsigned long)words, (unsigned long)first_wrong, w[first_wrong]);
		return false;
	}
	say("PASS: compute: 1024 words written as 3i + 7 by the GPU, 3072 canaries untouched");
	return true;
}

static IOSurfaceRef
test_surface(unsigned size)
{
	unsigned bpr = size * 4;            /* 256 for 64: a multiple of 64, as panfrost wants */
	NSDictionary *properties = @{
		(id)kIOSurfaceWidth: @(size), (id)kIOSurfaceHeight: @(size), (id)kIOSurfaceBytesPerElement: @4,
		(id)kIOSurfaceBytesPerRow: @(bpr), (id)kIOSurfaceAllocSize: @(bpr * size),
		(id)kIOSurfacePixelFormat: @(0x42475241),   /* 'BGRA' */
	};

	return IOSurfaceCreate((CFDictionaryRef)properties);
}

/* the pixel at (x, y) of the surface, as the CPU reads its memory: 0xAARRGGBB */
static uint32_t
surface_pixel(IOSurfaceRef surface, unsigned x, unsigned y)
{
	uint32_t value;

	IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
	value = *(const uint32_t *)((const uint8_t *)IOSurfaceGetBaseAddress(surface) + y * IOSurfaceGetBytesPerRow(surface) + x * 4);
	IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
	return value;
}

static bool
draw_test(id<MTLDevice> device, const char *path)
{
	const unsigned size = 64;
	const uint32_t green = 0xff00ff00, blue = 0xff0000ff, red = 0xffff0000;
	IOSurfaceRef surface = test_surface(size);
	MTLTextureDescriptor *td;
	id<MTLTexture> texture;
	id<MTLCommandQueue> queue = [device newCommandQueue];
	id<MTLLibrary> library;
	id<MTLRenderPipelineState> pipeline;
	MTLRenderPipelineDescriptor *pd;
	MTLRenderPassDescriptor *pass;
	id<MTLCommandBuffer> cb;
	id<MTLRenderCommandEncoder> encoder;
	NSError *error = nil;
	unsigned wrong = 0;
	bool ok = true;

	if (surface == NULL || queue == nil) {
		say("FAIL: draw: IOSurface %p, queue %p", (void *)surface, (void *)queue);
		return false;
	}
	say("draw: IOSurface %u, %ux%u 'BGRA', %zu bytes a row", IOSurfaceGetID(surface), size, size,
	    IOSurfaceGetBytesPerRow(surface));
	td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:size height:size
							    mipmapped:NO];
	[td setUsage:MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead];
	[td setStorageMode:MTLStorageModeShared];
	texture = [device newTextureWithDescriptor:td iosurface:surface plane:0];
	if (texture == nil) {
		say("FAIL: draw: newTextureWithDescriptor:iosurface:plane: gave nil");
		return false;
	}

	/* 1. a pass that only clears (green) */
	pass = [MTLRenderPassDescriptor renderPassDescriptor];
	pass.colorAttachments[0].texture = texture;
	pass.colorAttachments[0].loadAction = MTLLoadActionClear;
	pass.colorAttachments[0].storeAction = MTLStoreActionStore;
	pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 1.0, 0.0, 1.0);
	cb = [queue commandBuffer];
	encoder = [cb renderCommandEncoderWithDescriptor:pass];
	[encoder endEncoding];
	[cb commit];
	[cb waitUntilCompleted];
	if (!completed(cb, "clear")) {
		return false;
	}
	for (unsigned y = 0; y < size; y++) {
		for (unsigned x = 0; x < size; x++) {
			wrong += surface_pixel(surface, x, y) != green;
		}
	}
	say("draw: clear: (0,0) %#010x, (63,63) %#010x, %u of %u pixels not green", surface_pixel(surface, 0, 0),
	    surface_pixel(surface, size - 1, size - 1), wrong, size * size);
	if (wrong != 0) {
		say("FAIL: draw: the clear");
		ok = false;
	} else {
		say("PASS: draw: a clear through Metal into an IOSurface, every pixel");
	}

	/* 2. clear (blue) and the triangle (red) */
	library = tests_library(device, path);
	if (library == nil) {
		return false;
	}
	pd = [[MTLRenderPipelineDescriptor alloc] init];
	pd.vertexFunction = [library newFunctionWithName:@"a34_tri_vs"];
	pd.fragmentFunction = [library newFunctionWithName:@"a34_tri_fs"];
	pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	if (pd.vertexFunction == nil || pd.fragmentFunction == nil) {
		say("FAIL: draw: a34_tri_vs %p, a34_tri_fs %p", (void *)pd.vertexFunction, (void *)pd.fragmentFunction);
		return false;
	}
	pipeline = [device newRenderPipelineStateWithDescriptor:pd error:&error];
	if (pipeline == nil) {
		say("FAIL: draw: newRenderPipelineStateWithDescriptor: %s",
		    error != nil ? [[error localizedDescription] UTF8String] : "nil");
		return false;
	}
	pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 1.0, 1.0);
	cb = [queue commandBuffer];
	encoder = [cb renderCommandEncoderWithDescriptor:pass];
	[encoder setRenderPipelineState:pipeline];
	[encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
	[encoder endEncoding];
	[cb commit];
	[cb waitUntilCompleted];
	if (!completed(cb, "triangle")) {
		return false;
	}
	{
		/* pixel centres; the triangle spans x 16..48 at its base (y 48) and its apex is at
		 * (32, 16): Metal's y goes up in NDC and down in the texture */
		static const struct { unsigned x, y; bool inside; const char *where; } at[] = {
			{ 32, 32, true, "centre" }, { 32, 44, true, "near the base" }, { 32, 20, true, "near the apex" },
			{ 2, 2, false, "corner" }, { 61, 61, false, "corner" }, { 10, 40, false, "left of it" },
			{ 54, 40, false, "right of it" }, { 32, 8, false, "above the apex" }, { 32, 56, false, "below the base" },
		};
		unsigned bad = 0;

		for (unsigned i = 0; i < sizeof(at) / sizeof(at[0]); i++) {
			uint32_t got = surface_pixel(surface, at[i].x, at[i].y), want = at[i].inside ? red : blue;

			say("draw: triangle: (%2u,%2u) %-14s %#010x, want %#010x%s", at[i].x, at[i].y, at[i].where, got, want,
			    got == want ? "" : "  WRONG");
			bad += got != want;
		}
		if (bad != 0) {
			say("FAIL: draw: the triangle, %u of %zu places wrong", bad, sizeof(at) / sizeof(at[0]));
			ok = false;
		} else {
			say("PASS: draw: a translated vertex and fragment shader drew the triangle into the IOSurface");
		}
	}
	CFRelease(surface);
	return ok;
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
		bool want_direct = false, any = false, next = false, compute = false, draw = false;
		const char *tests_lib = DEFAULT_TESTS;
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
			} else if (strcmp(argv[i], "--compute") == 0 || strcmp(argv[i], "--draw") == 0) {
				*(argv[i][2] == 'c' ? &compute : &draw) = true;
				if (i + 1 < argc && argv[i + 1][0] != '-') {
					tests_lib = argv[++i];
				}
			} else if (strcmp(argv[i], "--next") == 0) {
				next = true;
			} else {
				fprintf(stderr, "usage: metal-probe [--direct [BUNDLE]] [--any] [--next] [--compute [LIB]] "
				    "[--draw [LIB]]\n");
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
		if (!any && strcmp(class_name, "RedwoodMetalDevice") != 0) {
			say("FAIL: a device, but of class %s, not RedwoodMetalDevice (--any accepts it)", class_name);
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
		if (compute || draw) {
			int failed = 0;

			@try {
				if (compute && !compute_test(device, tests_lib)) {
					failed++;
				}
				if (draw && !draw_test(device, tests_lib)) {
					failed++;
				}
			} @catch (NSException *exception) {
				say("FAIL: exception %s: %s", [[exception name] UTF8String], [[exception reason] UTF8String]);
				failed++;
			}
			if (failed) {
				return 4;
			}
		}
		[device release];
		return 0;
	}
}
