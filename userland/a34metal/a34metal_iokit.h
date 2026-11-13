/*
 * The IOKit calls of the Metal plug-in and its probe: the SDK's <IOKit/IOKitLib.h> where it
 * has one (macOS; iPhoneOS from the 18.5 SDK on), else these declarations, as
 * ../mesa/redwood-drm/redwood_iokit.h does for Mesa's libdrm stand-in.
 */
#ifndef A34METAL_IOKIT_H
#define A34METAL_IOKIT_H

#if __has_include(<IOKit/IOKitLib.h>)
#include <IOKit/IOKitLib.h>
#else

#include <stdint.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>

typedef kern_return_t IOReturn;
typedef uint32_t IOOptionBits;
typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
typedef io_object_t io_iterator_t;
typedef io_object_t io_registry_entry_t;

#define IO_OBJECT_NULL ((io_object_t)0)

#ifdef __cplusplus
extern "C" {
#endif
CFMutableDictionaryRef IOServiceMatching(const char *name);
io_service_t IOServiceGetMatchingService(mach_port_t mainPort, CFDictionaryRef matching);
kern_return_t IOServiceGetMatchingServices(mach_port_t mainPort, CFDictionaryRef matching, io_iterator_t *existing);
io_object_t IOIteratorNext(io_iterator_t iterator);
kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t *connect);
kern_return_t IOServiceClose(io_connect_t connect);
kern_return_t IOObjectRetain(io_object_t object);
kern_return_t IOObjectRelease(io_object_t object);
kern_return_t IOConnectCallStructMethod(mach_port_t connection, uint32_t selector,
    const void *inputStruct, size_t inputStructCnt, void *outputStruct, size_t *outputStructCnt);
CFTypeRef IORegistryEntryCreateCFProperty(io_registry_entry_t entry, CFStringRef key, CFAllocatorRef allocator,
    IOOptionBits options);
kern_return_t IORegistryEntryGetRegistryEntryID(io_registry_entry_t entry, uint64_t *entryID);
#ifdef __cplusplus
}
#endif

#endif

/* the main port: 0 means the default one to every call here (kIOMainPortDefault is iOS 15's name) */
#define A34METAL_IOKIT_PORT ((mach_port_t)0)

/* RedwoodMali (xnu/patches/0909): Linux's DRM_PANFROST_GET_PARAM, selector 0x44, 16 bytes */
struct a34metal_get_param {
	uint32_t param;
	uint32_t pad;
	uint64_t value;
};
#define A34METAL_SEL_GET_PARAM  0x44u
#define A34METAL_PARAM_PROD_ID  0u      /* DRM_PANFROST_PARAM_GPU_PROD_ID: 0x9204 on the Mali-G68 */

#endif
