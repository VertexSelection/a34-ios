/*
 * The few IOKit calls redwood_drm.c makes. The iPhoneOS SDK ships IOKit.framework's
 * library (IOKit.tbd) but not its headers, so where <IOKit/IOKitLib.h> is missing the
 * declarations below take its place. They are the public IOKitLib API, unchanged since
 * macOS 10.5 / iOS 2. build.sh compiles iokit_check.c against the macOS SDK's real
 * header, which checks every value in REDWOOD_IOKIT_VALUES against it.
 */
#ifndef REDWOOD_IOKIT_H
#define REDWOOD_IOKIT_H

#define REDWOOD_IOKIT_VALUES(X) \
	X(kIOReturnError,               0xe00002bc) \
	X(kIOReturnNoMemory,            0xe00002bd) \
	X(kIOReturnNoResources,         0xe00002be) \
	X(kIOReturnNoDevice,            0xe00002c0) \
	X(kIOReturnNotPrivileged,       0xe00002c1) \
	X(kIOReturnBadArgument,         0xe00002c2) \
	X(kIOReturnExclusiveAccess,     0xe00002c5) \
	X(kIOReturnUnsupported,         0xe00002c7) \
	X(kIOReturnVMError,             0xe00002c8) \
	X(kIOReturnBusy,                0xe00002d5) \
	X(kIOReturnTimeout,             0xe00002d6) \
	X(kIOReturnNotReady,            0xe00002d8) \
	X(kIOReturnNotPermitted,        0xe00002e2) \
	X(kIOReturnNotFound,            0xe00002f0) \
	X(kIOMapAnywhere,               0x00000001) \
	X(kIOMapReadOnly,               0x00001000)

#if __has_include(<IOKit/IOKitLib.h>) && !defined(REDWOOD_OWN_IOKIT)
#include <IOKit/IOKitLib.h>
#define REDWOOD_IOKIT_FROM_SDK 1
#else

#include <stdint.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>

typedef kern_return_t IOReturn;
typedef uint32_t IOOptionBits;
typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;

#define kIOReturnSuccess        KERN_SUCCESS
#ifndef REDWOOD_IOKIT_NO_VALUES
#define REDWOOD_IOKIT_ENUM(name, value) name = (int)(value),
enum { REDWOOD_IOKIT_VALUES(REDWOOD_IOKIT_ENUM) };
#undef REDWOOD_IOKIT_ENUM
#endif

#ifdef __cplusplus
extern "C" {
#endif
CFMutableDictionaryRef IOServiceMatching(const char *name);
io_service_t IOServiceGetMatchingService(mach_port_t mainPort, CFDictionaryRef matching);
kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t *connect);
kern_return_t IOServiceClose(io_connect_t connect);
kern_return_t IOObjectRelease(io_object_t object);
kern_return_t IOConnectCallStructMethod(mach_port_t connection, uint32_t selector,
    const void *inputStruct, size_t inputStructCnt, void *outputStruct, size_t *outputStructCnt);
kern_return_t IOConnectMapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t intoTask,
    mach_vm_address_t *atAddress, mach_vm_size_t *ofSize, IOOptionBits options);
kern_return_t IOConnectUnmapMemory64(io_connect_t connect, uint32_t memoryType, task_port_t fromTask,
    mach_vm_address_t atAddress);
#ifdef __cplusplus
}
#endif

#endif

#endif
