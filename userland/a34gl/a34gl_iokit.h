/*
 * The IOKit calls of liba34gl and its loader: the iPhoneOS SDK's <IOKit/IOKitLib.h> where it has
 * one, else redwood-drm's declarations (../mesa/redwood-drm/redwood_iokit.h) and the one more
 * call used here.
 */
#ifndef A34GL_IOKIT_H
#define A34GL_IOKIT_H

#include "redwood_iokit.h"

#ifndef REDWOOD_IOKIT_FROM_SDK
typedef io_object_t io_registry_entry_t;
#ifdef __cplusplus
extern "C" {
#endif
CFTypeRef IORegistryEntryCreateCFProperty(io_registry_entry_t entry, CFStringRef key, CFAllocatorRef allocator,
    IOOptionBits options);
#ifdef __cplusplus
}
#endif
#endif

#endif
