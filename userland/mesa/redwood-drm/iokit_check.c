/*
 * Built only for macOS (build.sh, native stage), whose SDK has the real IOKit headers:
 * fails to compile if a value redwood_iokit.h declares for the iPhoneOS SDK is wrong,
 * or if one of its declarations does not match the SDK's.
 */
#include <IOKit/IOKitLib.h>

#include "redwood_iokit.h"

#define CHECK(name, value) _Static_assert((uint32_t)(name) == (uint32_t)(value), #name);
REDWOOD_IOKIT_VALUES(CHECK)

/* again, the iPhoneOS branch: redeclaring a function or typedef incompatibly is an error */
#undef REDWOOD_IOKIT_H
#define REDWOOD_OWN_IOKIT 1
#define REDWOOD_IOKIT_NO_VALUES 1
#include "redwood_iokit.h"

int
main(void)
{
	return 0;
}
