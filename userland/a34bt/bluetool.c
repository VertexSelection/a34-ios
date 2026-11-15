/*
 * a34bt-bluetool: stands in for iOS's BlueTool (job com.apple.BlueTool, /usr/sbin/BlueTool -R, user mobile).
 *
 * bluetoothd asks BlueTool, through the Mach service com.apple.BlueTool, to run its "boot", "init" or "deepsleep"
 * script before it opens the controller: on an iPhone X that brings the Broadcom chip up over its UART, with one of
 * 221 firmware images built into BlueTool, chosen by ProductHash and WifiVendor. None of that fits the A34, whose
 * MediaTek controller the kernel has already brought up (xnu/patches 0939/0970/0971, a34bt=2). Without an answer
 * bluetoothd says "BlueTool timed out running boot script" and gives up (HackiOS, run 609).
 *
 * So the job runs this instead (tools/root-add-job.py --wrap /mnt com.apple.BlueTool /a34/a34bt-bluetool): it
 * checks in the same Mach service and answers every request with success -- "result" 0, "manufacturer" 15 and a
 * "firmware" name, as HackiOS's bluetool_serves() did inside the real BlueTool for iOS 16.5 (userland/src/inject.c,
 * github.com/oosexgang/hakios-xnu-kernel, MIT); a "command" (RunCommand) gets an empty "output". Each request is
 * said on the console (the kernel's log): "a34bt-bluetool: asked { ... }".
 *
 * The XPC calls are declared here: the iPhoneOS SDK marks the Mach-service ones unavailable on iOS, but libxpc in
 * the system has them (as HackiOS declares them).
 */
#include <dispatch/dispatch.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void *xpc_object_t;
typedef xpc_object_t xpc_connection_t;
typedef void (^xpc_handler_t)(xpc_object_t object);
extern const struct _xpc_type_s _xpc_type_connection, _xpc_type_dictionary, _xpc_type_error;
extern xpc_connection_t xpc_connection_create_mach_service(const char *name, dispatch_queue_t targetq, uint64_t flags);
extern void xpc_connection_set_event_handler(xpc_connection_t connection, xpc_handler_t handler);
extern void xpc_connection_resume(xpc_connection_t connection);
extern void xpc_connection_send_message(xpc_connection_t connection, xpc_object_t message);
extern const struct _xpc_type_s *xpc_get_type(xpc_object_t object);
extern xpc_object_t xpc_dictionary_create_reply(xpc_object_t original);
extern void xpc_dictionary_set_int64(xpc_object_t xdict, const char *key, int64_t value);
extern void xpc_dictionary_set_string(xpc_object_t xdict, const char *key, const char *string);
extern const char *xpc_dictionary_get_string(xpc_object_t xdict, const char *key);
extern char *xpc_copy_description(xpc_object_t object);
extern void xpc_release(xpc_object_t object);

#define XPC_CONNECTION_MACH_SERVICE_LISTENER (1 << 0)

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* one line on the console (the kernel's log), else stderr */
static void
say(const char *fmt, ...)
{
	char line[1024];
	va_list ap;
	int n, fd;

	n = snprintf(line, sizeof(line), "a34bt-bluetool[%d]: ", (int)getpid());
	va_start(ap, fmt);
	n += vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 2) {
		n = (int)sizeof(line) - 2;
	}
	line[n++] = '\n';
	if ((fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_CLOEXEC)) < 0 || write(fd, line, (size_t)n) < 0) {
		(void)write(2, line, (size_t)n);
	}
	if (fd >= 0) {
		close(fd);
	}
}

int
main(void)
{
	xpc_connection_t listener = xpc_connection_create_mach_service("com.apple.BlueTool", NULL,
	    XPC_CONNECTION_MACH_SERVICE_LISTENER);

	if (listener == NULL) {
		say("com.apple.BlueTool could not be checked in (not started as its launchd job?)");
		return 1;
	}
	say("serving com.apple.BlueTool: the kernel has the MediaTek controller up (a34bt=2); every script answered as done");
	xpc_connection_set_event_handler(listener, ^(xpc_object_t peer) {
		if (xpc_get_type(peer) != &_xpc_type_connection) {
			return;
		}
		xpc_connection_set_event_handler(peer, ^(xpc_object_t message) {
			char *text;
			xpc_object_t reply;

			if (xpc_get_type(message) != &_xpc_type_dictionary) {
				return;
			}
			text = xpc_copy_description(message);
			say("asked %s", text != NULL ? text : "(?)");
			free(text);
			if ((reply = xpc_dictionary_create_reply(message)) != NULL) {
				xpc_dictionary_set_int64(reply, "result", 0);
				xpc_dictionary_set_int64(reply, "manufacturer", 15);
				xpc_dictionary_set_string(reply, "firmware", "MT6635 (A34, kernel 0971)");
				xpc_dictionary_set_string(reply, "output", "");
				xpc_connection_send_message(peer, reply);
				xpc_release(reply);
			}
		});
		xpc_connection_resume(peer);
	});
	xpc_connection_resume(listener);
	dispatch_main();
}
