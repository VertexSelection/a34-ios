/*
 * liba34bt-btd.dylib: inserted into iOS's bluetoothd by a34bt-btd (btd.c).
 *
 * 1. BlueTool answered here. Before bluetoothd opens its transport it has BlueTool boot the chip: it sends
 *    {kMsgId: "RunBuiltinScript", kMsgArgs: {script: "boot", btaddress: ...}} to the Mach service com.apple.BlueTool
 *    with xpc_connection_send_message_with_reply_sync (bluetoothd 20H392: +0x272034, client +0x1d53c0/+0x1d5714)
 *    and reads "result" (0 done; 10 or 13 "Unknown platform/chipset/manufacturer combination"), "firmware" and
 *    "manufacturer" from the reply (+0x27249c). A reply that is not a dictionary -- the service could not be
 *    reached -- is status 1: "Init failed, still in high power result=1", and bluetoothd _exit()s (+0x3a6128) and is
 *    restarted. That is what boot 0980-btmetal-a did 15 times: BlueTool's job was never started. The kernel
 *    (0939-0971) has the MediaTek controller up already, so every message to com.apple.BlueTool is answered here,
 *    without launchd: result 0, manufacturer 15, a firmware name (HackiOS's answers, userland/src/inject.c,
 *    bluetool_serves, MIT). A34BT_BLUETOOL=launchd in the environment leaves BlueTool to its job.
 * 2. Every exit said: exit() and _exit() with the status and the frames (image + offset) they came from, so a
 *    restart names its place in bluetoothd.
 * 3. The Mach services bluetoothd connects to are said (the first 40).
 *
 * Lines go to the console (the kernel's log) as "a34bt-btd: bluetoothd[PID]: ...".
 */
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void *xpc_object_t;
typedef xpc_object_t xpc_connection_t;
typedef void (^xpc_handler_t)(xpc_object_t object);
extern xpc_connection_t xpc_connection_create_mach_service(const char *name, dispatch_queue_t targetq, uint64_t flags);
extern xpc_object_t xpc_connection_send_message_with_reply_sync(xpc_connection_t connection, xpc_object_t message);
extern void xpc_connection_send_message_with_reply(xpc_connection_t connection, xpc_object_t message,
    dispatch_queue_t replyq, xpc_handler_t handler);
extern void xpc_connection_send_message(xpc_connection_t connection, xpc_object_t message);
extern xpc_object_t xpc_dictionary_create(const char *const *keys, const xpc_object_t *values, size_t count);
extern void xpc_dictionary_set_int64(xpc_object_t xdict, const char *key, int64_t value);
extern void xpc_dictionary_set_string(xpc_object_t xdict, const char *key, const char *string);
extern char *xpc_copy_description(xpc_object_t object);
extern void xpc_release(xpc_object_t object);

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void
say(const char *fmt, ...)
{
	char line[1200];
	va_list ap;
	int n, fd;

	n = snprintf(line, sizeof(line), "a34bt-btd: bluetoothd[%d]: ", (int)getpid());
	va_start(ap, fmt);
	n += vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 2) {
		n = (int)sizeof(line) - 2;
	}
	line[n++] = '\n';
	if ((fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_CLOEXEC)) >= 0) {
		(void)write(fd, line, (size_t)n);
		close(fd);
	} else {
		(void)write(2, line, (size_t)n);
	}
}

/* ---- BlueTool ---- */

static xpc_connection_t bluetool[8];
static bool bluetool_local = true;

static bool
is_bluetool(xpc_connection_t c)
{
	for (unsigned i = 0; c != NULL && i < 8; i++) {
		if (bluetool[i] == c) {
			return true;
		}
	}
	return false;
}

static xpc_object_t
bluetool_answer(xpc_object_t message)
{
	char *text = xpc_copy_description(message);
	xpc_object_t reply = xpc_dictionary_create(NULL, NULL, 0);

	say("com.apple.BlueTool asked %s: answered here (result 0: the kernel has the controller up)", text != NULL ? text : "(?)");
	free(text);
	xpc_dictionary_set_int64(reply, "result", 0);
	xpc_dictionary_set_int64(reply, "manufacturer", 15);
	xpc_dictionary_set_string(reply, "firmware", "MT6635 (A34, kernel 0971)");
	xpc_dictionary_set_string(reply, "output", "");
	return reply;
}

static xpc_connection_t
my_create_mach_service(const char *name, dispatch_queue_t targetq, uint64_t flags)
{
	static unsigned said;
	xpc_connection_t c = xpc_connection_create_mach_service(name, targetq, flags);

	if (said++ < 40) {
		say("connects to %s (flags %#llx)", name != NULL ? name : "(null)", (unsigned long long)flags);
	}
	if (bluetool_local && name != NULL && strcmp(name, "com.apple.BlueTool") == 0) {
		for (unsigned i = 0; i < 8; i++) {
			if (bluetool[i] == NULL) {
				bluetool[i] = c;
				break;
			}
		}
	}
	return c;
}

static xpc_object_t
my_send_sync(xpc_connection_t c, xpc_object_t message)
{
	if (is_bluetool(c)) {
		return bluetool_answer(message);
	}
	return xpc_connection_send_message_with_reply_sync(c, message);
}

static void
my_send_async(xpc_connection_t c, xpc_object_t message, dispatch_queue_t q, xpc_handler_t handler)
{
	if (is_bluetool(c)) {
		xpc_object_t reply = bluetool_answer(message);

		dispatch_async(q != NULL ? q : dispatch_get_global_queue(0, 0), ^{
			handler(reply);
			xpc_release(reply);
		});
		return;
	}
	xpc_connection_send_message_with_reply(c, message, q, handler);
}

static void
my_send(xpc_connection_t c, xpc_object_t message)
{
	if (is_bluetool(c)) {
		char *text = xpc_copy_description(message);

		say("com.apple.BlueTool sent %s: dropped", text != NULL ? text : "(?)");
		free(text);
		return;
	}
	xpc_connection_send_message(c, message);
}

/* ---- exits ---- */

static void
say_frames(const char *what, int status)
{
	void *frames[14];
	char text[900];
	size_t used = 0;
	int n = backtrace(frames, 14);

	text[0] = 0;
	for (int i = 1; i < n && used < sizeof(text) - 64; i++) {
		Dl_info info;
		const char *image = "?";
		uintptr_t off = (uintptr_t)frames[i];

		if (dladdr(frames[i], &info) != 0 && info.dli_fname != NULL) {
			image = strrchr(info.dli_fname, '/') != NULL ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname;
			off = (uintptr_t)frames[i] - (uintptr_t)info.dli_fbase;
		}
		used += (size_t)snprintf(text + used, sizeof(text) - used, " %s+%#lx", image, (unsigned long)off);
	}
	say("%s(%d) from%s", what, status, text);
}

static void my_exit(int status) __attribute__((noreturn));
static void my__exit(int status) __attribute__((noreturn));

static void
my_exit(int status)
{
	say_frames("exit", status);
	exit(status);
}

static void
my__exit(int status)
{
	say_frames("_exit", status);
	_exit(status);
}

__attribute__((used)) static const struct {
	const void *replacement;
	const void *replacee;
} interposers[] __attribute__((section("__DATA,__interpose"))) = {
	{ (const void *)my_create_mach_service, (const void *)xpc_connection_create_mach_service },
	{ (const void *)my_send_sync, (const void *)xpc_connection_send_message_with_reply_sync },
	{ (const void *)my_send_async, (const void *)xpc_connection_send_message_with_reply },
	{ (const void *)my_send, (const void *)xpc_connection_send_message },
	{ (const void *)my_exit, (const void *)exit },
	{ (const void *)my__exit, (const void *)_exit },
};

__attribute__((constructor)) static void
btd_init(void)
{
	const char *mode = getenv("A34BT_BLUETOOL");

	bluetool_local = !(mode != NULL && strcmp(mode, "launchd") == 0);
	unsetenv("DYLD_INSERT_LIBRARIES");     /* (not into what bluetoothd starts) */
	say("liba34bt-btd loaded: BlueTool %s, exits said", bluetool_local ? "answered here" : "left to launchd");
}
