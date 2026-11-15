/*
 * liba34cell-sb.dylib: the status bar's cellular items from a34cell, inside SpringBoard.
 *
 * CommCenter is not on this system, so SpringBoard's SBStatusBarStateAggregator never sees
 * service: the signal item stays hidden and the service string empty. a34cell writes the modem's
 * state to /var/run/a34cell.plist and posts "com.a34.cell.state"; this library reads it and, while
 * the modem is registered, sets the bars and the operator's name through the aggregator's own item
 * updaters and keeps UIKit's cellular signal view in its "active" mode. Ported from HackiOS's
 * userland/src/inject_sb.c (cellular section; MIT License, Copyright (c) 2025-2026 the HakiOS
 * authors), reading a34cell's state instead of HackiOS's sysctls.
 *
 * Loaded into SpringBoard by the kernel's a34inject (DYLD_INSERT_LIBRARIES; several libraries
 * separated by ':'), e.g. a34inject=/a34/liba34gl-inject.dylib:/a34/liba34cell-sb.dylib,backboardd,
 * SpringBoard,mediaserverd. In any other program it does nothing.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <notify.h>
#include <stdarg.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define STATE_PATH "/var/run/a34cell.plist"

static struct {
	bool registered, modem_up;
	int bars;
	char oper[64];
} cs;

static id aggregator;

static void
say(const char *fmt, ...)
{
	static int fd = -1;
	char line[512];
	va_list ap;

	if (fd < 0 && (fd = open("/dev/console", O_WRONLY | O_NOCTTY)) < 0)
		fd = 2;
	va_start(ap, fmt);
	int n = vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (n > 0)
		(void)write(fd, line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
}

static void
read_state(void)
{
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)STATE_PATH,
	    (CFIndex)strlen(STATE_PATH), false);
	CFReadStreamRef rs = url ? CFReadStreamCreateWithFile(NULL, url) : NULL;
	CFPropertyListRef pl = NULL;

	if (rs && CFReadStreamOpen(rs)) {
		pl = CFPropertyListCreateWithStream(NULL, rs, 0, kCFPropertyListImmutable, NULL, NULL);
		CFReadStreamClose(rs);
	}
	if (rs)
		CFRelease(rs);
	if (url)
		CFRelease(url);
	memset(&cs, 0, sizeof(cs));
	if (!pl || CFGetTypeID(pl) != CFDictionaryGetTypeID()) {
		if (pl)
			CFRelease(pl);
		return;
	}
	CFDictionaryRef d = pl;
	CFTypeRef v;
	if ((v = CFDictionaryGetValue(d, CFSTR("Registered"))) && CFGetTypeID(v) == CFBooleanGetTypeID())
		cs.registered = CFBooleanGetValue(v);
	if ((v = CFDictionaryGetValue(d, CFSTR("ModemUp"))) && CFGetTypeID(v) == CFBooleanGetTypeID())
		cs.modem_up = CFBooleanGetValue(v);
	if ((v = CFDictionaryGetValue(d, CFSTR("SignalBars"))) && CFGetTypeID(v) == CFNumberGetTypeID())
		CFNumberGetValue(v, kCFNumberIntType, &cs.bars);
	if ((v = CFDictionaryGetValue(d, CFSTR("Operator"))) && CFGetTypeID(v) == CFStringGetTypeID())
		CFStringGetCString(v, cs.oper, sizeof(cs.oper), kCFStringEncodingUTF8);
	CFRelease(pl);
}

/* The aggregator's getters and setters are blocks: called through the block's invoke pointer. */
struct block_layout {
	void *isa;
	int flags, reserved;
	void (*invoke)(void *, ...);
};
#define BLOCK_CALL(block, type, ...) ((type)((struct block_layout *)(block))->invoke)((block), __VA_ARGS__)

static void (*orig_signal)(id, SEL, long, void *, id, void *, void *);

static void
update_signal_item(id self, SEL sel, long item, void *data, id sim, void *getter, void *setter)
{
	static int last = -2;

	aggregator = self;
	if (orig_signal)
		orig_signal(self, sel, item, data, sim, getter, setter);
	if (!cs.registered || !setter || !data)
		return;
	int bars = cs.bars < 1 ? 1 : cs.bars > 4 ? 4 : cs.bars;
	BLOCK_CALL(setter, void (*)(void *, void *, int), data, bars);
	((void (*)(id, SEL, long, BOOL))objc_msgSend)(self, sel_registerName("_setItem:enabled:"), item, YES);
	if (bars != last)
		say("a34cell-sb: %d bars shown\n", bars);
	last = bars;
}

static void (*orig_service)(id, SEL, long, void *, id, id, void *, void *, void *, void *, void *, void *,
    void *, void *);

static void
update_service_item(id self, SEL sel, long item, void *data, id sim, id other, void *g1, void *s1, void *g2,
    void *s2, void *g3, void *s3, void *g4, void *s4)
{
	static char shown[64];

	aggregator = self;
	if (orig_service)
		orig_service(self, sel, item, data, sim, other, g1, s1, g2, s2, g3, s3, g4, s4);
	if (!cs.registered || !cs.oper[0] || !s1 || !data)
		return;
	CFStringRef str = CFStringCreateWithCString(NULL, cs.oper, kCFStringEncodingUTF8);
	if (!str)
		return;
	BLOCK_CALL(s1, void (*)(void *, void *, CFStringRef), data, str);
	((void (*)(id, SEL, long, BOOL))objc_msgSend)(self, sel_registerName("_setItem:enabled:"), item, YES);
	CFRelease(str);
	if (strcmp(shown, cs.oper) != 0) {
		snprintf(shown, sizeof(shown), "%s", cs.oper);
		say("a34cell-sb: operator \"%s\" shown\n", cs.oper);
	}
}

/* UIKit's cellular signal view: 0 inactive (grey), 1 searching, 2 active. */
static void (*orig_mode)(id, SEL, long);

static void
set_signal_mode(id self, SEL sel, long mode)
{
	const char *name = object_getClassName(self);

	if (cs.registered && name && strstr(name, "Cellular"))
		mode = 2;
	orig_mode(self, sel, mode);
}

static IMP
replace(const char *cls_name, const char *sel_name, IMP with)
{
	Class cls = objc_getClass(cls_name);
	Method m = cls ? class_getInstanceMethod(cls, sel_registerName(sel_name)) : NULL;

	if (!m) {
		say("a34cell-sb: no -[%s %s]\n", cls_name, sel_name);
		return NULL;
	}
	return method_setImplementation(m, with);
}

static void
refresh(void)
{
	read_state();
	if (!aggregator)
		return;
	static const char *const updaters[] = { "_updateSignalStrengthItem", "_updateServiceItem" };
	for (size_t i = 0; i < sizeof(updaters) / sizeof(updaters[0]); i++) {
		SEL s = sel_registerName(updaters[i]);
		if (class_getInstanceMethod(object_getClass(aggregator), s))
			((void (*)(id, SEL))objc_msgSend)(aggregator, s);
	}
}

static void
install(void)
{
	orig_signal = (void (*)(id, SEL, long, void *, id, void *, void *))replace("SBStatusBarStateAggregator",
	    "_updateSignalStrengthItem:withData:SIMInfo:barsGetter:barsSetter:", (IMP)update_signal_item);
	orig_service = (void (*)(id, SEL, long, void *, id, id, void *, void *, void *, void *, void *, void *,
	    void *, void *))replace("SBStatusBarStateAggregator",
	    "_updateServiceItem:withData:SIMInfo:otherSIMInfo:serviceStringGetter:serviceStringSetter:"
	    "serviceCrossfadeStringGetter:serviceCrossfadeStringSetter:serviceContentTypeGetter:"
	    "serviceContentTypeSetter:serviceBadgeStringGetter:serviceBadgeStringSetter:", (IMP)update_service_item);
	/* the cellular subclass's own method if it has one, else the base class's (which serves it) */
	Class cell = objc_getClass("_UIStatusBarCellularSignalView");
	Method own = NULL;
	if (cell) {
		unsigned n = 0;
		Method *list = class_copyMethodList(cell, &n);
		for (unsigned i = 0; list && i < n; i++)
			if (method_getName(list[i]) == sel_registerName("setSignalMode:"))
				own = list[i];
		free(list);
	}
	IMP m = own ? method_setImplementation(own, (IMP)set_signal_mode) :
	    replace("_UIStatusBarSignalView", "setSignalMode:", (IMP)set_signal_mode);
	if (m)
		orig_mode = (void (*)(id, SEL, long))m;
	int token;
	notify_register_dispatch("com.a34.cell.state", &token, dispatch_get_main_queue(), ^(int t) {
		(void)t;
		refresh();
	});
	read_state();
	say("a34cell-sb: in SpringBoard; hooks %s/%s/%s; state now %s, %d bars, \"%s\"\n",
	    orig_signal ? "signal" : "-", orig_service ? "service" : "-", orig_mode ? "mode" : "-",
	    cs.registered ? "registered" : cs.modem_up ? "modem up" : "no modem", cs.bars, cs.oper);
}

__attribute__((constructor)) static void
a34cell_sb_init(void)
{
	const char *name = getprogname();

	if (!name || strcmp(name, "SpringBoard") != 0)
		return;
	/* UIKit's and SpringBoard's classes are there once the main queue runs. */
	dispatch_async(dispatch_get_main_queue(), ^{
		install();
	});
}
