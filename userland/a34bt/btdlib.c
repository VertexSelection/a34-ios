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
 * 4. Its keychain kept here (boot 0988-all-a). bluetoothd keeps everything it must remember -- its LE identity root,
 *    encryption root, cloud keys, static random address (readRootKeys, +0x594fac), paired devices -- as generic
 *    passwords of the service "MobileBluetooth" in the system keychain (+0x254c30 get, +0x255334 set, +0x25602c and
 *    +0x25656c lists, +0x255e40 delete). securityd cannot store an item here: this kernel's AppleKeyStore does not
 *    wrap keys (selector 10, "SWEET AKS: securityd sel 10 ... -> 0xe00002c7"), so "Failed to set new identity root!",
 *    and the stack's init (+0x599048) restarts bluetoothd (+0x487df8 -> +0x90734 _exit(1)) -- 15 times in that boot.
 *    SecItemCopyMatching/Add/Update/Delete of service "MobileBluetooth" are answered from
 *    /var/mobile/Library/Bluetooth/com.a34.keychain.plist (a binary plist, bluetoothd's own user: NOT encrypted --
 *    the pairing keys of this research port are readable by root); every other keychain call goes to securityd as
 *    before. A34BT_KEYCHAIN=securityd leaves bluetoothd's to securityd too.
 * 5. Its os_log on the console (OS_ACTIVITY_DT_MODE did not show it): _os_log_impl, _os_log_error_impl and
 *    _os_log_fault_impl formatted here (format string and arguments, privacy ignored: this is the phone's own log),
 *    the first 3000 lines of each run, debug ones left out; "bt: " ahead of each.
 *
 * Lines go to the console (the kernel's log) as "a34bt-btd: bluetoothd[PID]: ...".
 */
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <ctype.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <os/log.h>
#include <pthread.h>
#include <sys/stat.h>
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

/* ---- 4. bluetoothd's keychain, kept here ---- */

#define KC_DIR  "/var/mobile/Library/Bluetooth"
#define KC_FILE KC_DIR "/com.a34.keychain.plist"

static pthread_mutex_t kc_lock = PTHREAD_MUTEX_INITIALIZER;
static CFMutableDictionaryRef kc_items;          /* account -> item (attributes, v_Data) */
static bool kc_local = true;
static unsigned kc_said;

static void
kc_load(void)
{
	CFDataRef data = NULL;
	struct stat st;
	int fd;

	if (kc_items != NULL) {
		return;
	}
	if ((fd = open(KC_FILE, O_RDONLY | O_CLOEXEC)) >= 0) {
		if (fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size < (4 << 20)) {
			UInt8 *bytes = malloc((size_t)st.st_size);

			if (bytes != NULL && read(fd, bytes, (size_t)st.st_size) == st.st_size) {
				data = CFDataCreate(NULL, bytes, (CFIndex)st.st_size);
			}
			free(bytes);
		}
		close(fd);
	}
	if (data != NULL) {
		CFPropertyListRef plist = CFPropertyListCreateWithData(NULL, data, kCFPropertyListMutableContainersAndLeaves, NULL, NULL);

		CFRelease(data);
		if (plist != NULL && CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
			kc_items = (CFMutableDictionaryRef)plist;
		} else if (plist != NULL) {
			CFRelease(plist);
		}
	}
	if (kc_items == NULL) {
		kc_items = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	}
	say("keychain: %ld MobileBluetooth items from " KC_FILE, (long)CFDictionaryGetCount(kc_items));
}

static void
kc_save(void)
{
	CFDataRef data = CFPropertyListCreateData(NULL, kc_items, kCFPropertyListBinaryFormat_v1_0, 0, NULL);
	char tmp[] = KC_FILE ".new";
	int fd;

	if (data == NULL) {
		say("keychain: the items are not a property list: not kept");
		return;
	}
	(void)mkdir(KC_DIR, 0700);
	if ((fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600)) >= 0) {
		bool ok = write(fd, CFDataGetBytePtr(data), (size_t)CFDataGetLength(data)) == CFDataGetLength(data);

		ok = fsync(fd) == 0 && ok;
		close(fd);
		if (!ok || rename(tmp, KC_FILE) != 0) {
			say("keychain: " KC_FILE " not written");
		}
	} else {
		say("keychain: " KC_FILE " cannot be made");
	}
	CFRelease(data);
}

static bool
kc_is_ours(CFDictionaryRef query)
{
	CFTypeRef cls, service;

	if (!kc_local || query == NULL || CFGetTypeID(query) != CFDictionaryGetTypeID()) {
		return false;
	}
	cls = CFDictionaryGetValue(query, kSecClass);
	service = CFDictionaryGetValue(query, kSecAttrService);
	return cls != NULL && CFEqual(cls, kSecClassGenericPassword) && service != NULL &&
	       CFGetTypeID(service) == CFStringGetTypeID() && CFEqual(service, CFSTR("MobileBluetooth"));
}

static void
kc_name(CFTypeRef v, char *out, size_t room)
{
	out[0] = 0;
	if (v != NULL && CFGetTypeID(v) == CFStringGetTypeID()) {
		(void)CFStringGetCString(v, out, (CFIndex)room, kCFStringEncodingUTF8);
	} else {
		snprintf(out, room, "(any)");
	}
}

/* the item matches the query's account and label, where the query names them */
static bool
kc_matches(CFDictionaryRef item, CFDictionaryRef query)
{
	static const CFStringRef *const keys[] = { &kSecAttrAccount, &kSecAttrLabel };

	for (unsigned i = 0; i < 2; i++) {
		CFTypeRef want = CFDictionaryGetValue(query, *keys[i]);

		if (want != NULL && !CFEqual(want, CFDictionaryGetValue(item, *keys[i]) ?: CFSTR(""))) {
			return false;
		}
	}
	return true;
}

static bool
kc_plist_value(CFTypeRef v)
{
	CFTypeID t = CFGetTypeID(v);

	return t == CFStringGetTypeID() || t == CFDataGetTypeID() || t == CFNumberGetTypeID() || t == CFBooleanGetTypeID() ||
	       t == CFDateGetTypeID() || t == CFArrayGetTypeID() || t == CFDictionaryGetTypeID();
}

static void
kc_copy_attrs(CFMutableDictionaryRef into, CFDictionaryRef from)
{
	CFIndex n = CFDictionaryGetCount(from);
	const void **k = malloc(sizeof(void *) * (size_t)(n ? n : 1)), **v = malloc(sizeof(void *) * (size_t)(n ? n : 1));

	CFDictionaryGetKeysAndValues(from, k, v);
	for (CFIndex i = 0; i < n; i++) {
		if (CFGetTypeID(k[i]) == CFStringGetTypeID() && !CFEqual(k[i], kSecClass) && !CFEqual(k[i], kSecMatchLimit) &&
		    !CFEqual(k[i], kSecReturnData) && !CFEqual(k[i], kSecReturnAttributes) && kc_plist_value(v[i])) {
			CFDictionarySetValue(into, k[i], v[i]);
		}
	}
	free(k);
	free(v);
}

static CFTypeRef
kc_result(CFDictionaryRef item, bool data, bool attrs)
{
	if (attrs) {
		CFMutableDictionaryRef r = CFDictionaryCreateMutableCopy(NULL, 0, item);

		if (!data) {
			CFDictionaryRemoveValue(r, kSecValueData);
		}
		if (CFDictionaryGetValue(r, kSecAttrAccessGroup) == NULL) {
			CFDictionarySetValue(r, kSecAttrAccessGroup, CFSTR("com.apple.bluetooth"));
		}
		return r;
	}
	if (data) {
		CFTypeRef d = CFDictionaryGetValue(item, kSecValueData);

		return d != NULL ? CFRetain(d) : CFDataCreate(NULL, NULL, 0);
	}
	return CFRetain(item);
}

static OSStatus
my_SecItemCopyMatching(CFDictionaryRef query, CFTypeRef *result)
{
	CFTypeRef limit;
	bool all, data, attrs;
	CFMutableArrayRef found;
	CFIndex n;
	char name[96];

	if (!kc_is_ours(query)) {
		return SecItemCopyMatching(query, result);
	}
	limit = CFDictionaryGetValue(query, kSecMatchLimit);
	all = limit != NULL && CFEqual(limit, kSecMatchLimitAll);
	data = CFDictionaryGetValue(query, kSecReturnData) == kCFBooleanTrue;
	attrs = CFDictionaryGetValue(query, kSecReturnAttributes) == kCFBooleanTrue;
	found = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	pthread_mutex_lock(&kc_lock);
	kc_load();
	n = CFDictionaryGetCount(kc_items);
	if (n > 0) {
		const void **k = malloc(sizeof(void *) * (size_t)n), **v = malloc(sizeof(void *) * (size_t)n);

		CFDictionaryGetKeysAndValues(kc_items, k, v);
		for (CFIndex i = 0; i < n; i++) {
			if (kc_matches(v[i], query)) {
				CFTypeRef r = kc_result(v[i], data, attrs);

				CFArrayAppendValue(found, r);
				CFRelease(r);
				if (!all) {
					break;
				}
			}
		}
		free(k);
		free(v);
	}
	pthread_mutex_unlock(&kc_lock);
	kc_name(CFDictionaryGetValue(query, kSecAttrAccount), name, sizeof(name));
	if (kc_said++ < 60) {
		say("keychain: copy %s%s: %ld found", name, all ? " (all)" : "", (long)CFArrayGetCount(found));
	}
	if (CFArrayGetCount(found) == 0) {
		CFRelease(found);
		if (result != NULL) {
			*result = NULL;
		}
		return errSecItemNotFound;
	}
	if (result != NULL) {
		*result = all ? CFRetain(found) : CFRetain(CFArrayGetValueAtIndex(found, 0));
	}
	CFRelease(found);
	return errSecSuccess;
}

static OSStatus
my_SecItemAdd(CFDictionaryRef attributes, CFTypeRef *result)
{
	CFTypeRef account;
	CFMutableDictionaryRef item;
	OSStatus status = errSecSuccess;
	char name[96];

	if (!kc_is_ours(attributes)) {
		return SecItemAdd(attributes, result);
	}
	if (result != NULL) {
		*result = NULL;
	}
	account = CFDictionaryGetValue(attributes, kSecAttrAccount);
	kc_name(account, name, sizeof(name));
	if (account == NULL) {
		say("keychain: add without an account: refused");
		return errSecParam;
	}
	pthread_mutex_lock(&kc_lock);
	kc_load();
	if (CFDictionaryGetValue(kc_items, account) != NULL) {
		status = errSecDuplicateItem;
	} else {
		item = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		kc_copy_attrs(item, attributes);
		CFDictionarySetValue(kc_items, account, item);
		CFRelease(item);
		kc_save();
	}
	pthread_mutex_unlock(&kc_lock);
	if (kc_said++ < 60) {
		say("keychain: add %s: %d", name, (int)status);
	}
	return status;
}

static OSStatus
my_SecItemUpdate(CFDictionaryRef query, CFDictionaryRef update)
{
	CFIndex n, changed = 0;
	char name[96];

	if (!kc_is_ours(query)) {
		return SecItemUpdate(query, update);
	}
	pthread_mutex_lock(&kc_lock);
	kc_load();
	n = CFDictionaryGetCount(kc_items);
	if (n > 0) {
		const void **k = malloc(sizeof(void *) * (size_t)n), **v = malloc(sizeof(void *) * (size_t)n);

		CFDictionaryGetKeysAndValues(kc_items, k, v);
		for (CFIndex i = 0; i < n; i++) {
			if (kc_matches(v[i], query)) {
				kc_copy_attrs((CFMutableDictionaryRef)v[i], update);
				changed++;
			}
		}
		free(k);
		free(v);
	}
	if (changed != 0) {
		kc_save();
	}
	pthread_mutex_unlock(&kc_lock);
	kc_name(CFDictionaryGetValue(query, kSecAttrAccount), name, sizeof(name));
	if (kc_said++ < 60) {
		say("keychain: update %s: %ld item(s)", name, (long)changed);
	}
	return changed != 0 ? errSecSuccess : errSecItemNotFound;
}

static OSStatus
my_SecItemDelete(CFDictionaryRef query)
{
	CFIndex n, removed = 0;
	char name[96];

	if (!kc_is_ours(query)) {
		return SecItemDelete(query);
	}
	pthread_mutex_lock(&kc_lock);
	kc_load();
	n = CFDictionaryGetCount(kc_items);
	if (n > 0) {
		const void **k = malloc(sizeof(void *) * (size_t)n), **v = malloc(sizeof(void *) * (size_t)n);

		CFDictionaryGetKeysAndValues(kc_items, k, v);
		for (CFIndex i = 0; i < n; i++) {
			if (kc_matches(v[i], query)) {
				CFDictionaryRemoveValue(kc_items, k[i]);
				removed++;
			}
		}
		free(k);
		free(v);
	}
	if (removed != 0) {
		kc_save();
	}
	pthread_mutex_unlock(&kc_lock);
	kc_name(CFDictionaryGetValue(query, kSecAttrAccount), name, sizeof(name));
	if (kc_said++ < 60) {
		say("keychain: delete %s: %ld item(s)", name, (long)removed);
	}
	return removed != 0 ? errSecSuccess : errSecItemNotFound;
}

/* ---- 5. its os_log on the console ---- */

static unsigned log_said;

/* the format with its arguments: buf is __builtin_os_log_format's (summary, count, then descriptor, size, data each) */
static void
log_format(char *out, size_t room, const char *format, const uint8_t *buf, uint32_t size)
{
	size_t o = 0;
	uint32_t pos = 2, left = size >= 2 ? buf[1] : 0;
	long precision;

#define PUT(...) do { if (o < room) { int k_ = snprintf(out + o, room - o, __VA_ARGS__); if (k_ > 0) { o += (size_t)k_; } } } while (0)
	for (const char *p = format; *p != 0 && o + 1 < room; p++) {
		const uint8_t *d;
		uint8_t sz;
		uint64_t v = 0;
		bool star = false;
		char conv;

		if (*p != '%') {
			out[o++] = *p;
			continue;
		}
		if (*++p == '%') {
			out[o++] = '%';
			continue;
		}
		precision = -1;
		while (*p != 0 && strchr("-+ #0'", *p) != NULL) {
			p++;
		}
		while (isdigit((unsigned char)*p)) {
			p++;
		}
		if (*p == '.') {
			p++;
			if (*p == '*') {
				star = true;
				p++;
			} else {
				precision = strtol(p, (char **)&p, 10);
			}
		}
		if (*p == '{') {
			while (*p != 0 && *p != '}') {
				p++;
			}
			if (*p != 0) {
				p++;
			}
		}
		while (*p != 0 && strchr("hlqjztL", *p) != NULL) {
			p++;
		}
		if ((conv = *p) == 0) {
			break;
		}
		if (star) {                                            /* the precision, an argument of its own */
			if (left == 0 || pos + 2 > size) {
				break;
			}
			sz = buf[pos + 1];
			d = buf + pos + 2;
			v = 0;
			memcpy(&v, d, sz <= 8 ? sz : 8);
			precision = (long)v;
			pos += 2u + sz;
			left--;
		}
		if (left == 0 || pos + 2 > size || pos + 2u + buf[pos + 1] > size) {
			PUT("<?>");
			continue;
		}
		sz = buf[pos + 1];
		d = buf + pos + 2;
		pos += 2u + sz;
		left--;
		memcpy(&v, d, sz <= 8 ? sz : 8);
		switch (conv) {
		case 's':
			if (sz == 8 && v != 0) {
				PUT("%.200s", (const char *)(uintptr_t)v);
			} else {
				PUT("(null)");
			}
			break;
		case '@':
			if (sz == 8 && v != 0) {
				CFStringRef text = CFCopyDescription((CFTypeRef)(uintptr_t)v);
				char c[300];

				if (text != NULL && CFStringGetCString(text, c, sizeof(c), kCFStringEncodingUTF8)) {
					PUT("%s", c);
				} else {
					PUT("<object>");
				}
				if (text != NULL) {
					CFRelease(text);
				}
			} else {
				PUT("(null)");
			}
			break;
		case 'P':
			if (sz == 8 && v != 0 && precision > 0) {
				const uint8_t *b = (const uint8_t *)(uintptr_t)v;

				for (long i = 0; i < precision && i < 32; i++) {
					PUT("%02x", b[i]);
				}
			} else {
				PUT("<data>");
			}
			break;
		case 'd': case 'i':
			PUT("%lld", sz == 1 ? (long long)(int8_t)v : sz == 2 ? (long long)(int16_t)v :
			    sz == 4 ? (long long)(int32_t)v : (long long)v);
			break;
		case 'x': case 'X': case 'p':
			PUT("%#llx", (unsigned long long)v);
			break;
		case 'c':
			PUT("%c", (int)(v & 0xff));
			break;
		case 'f': case 'g': case 'e': {
			double f = 0;

			if (sz == 8) {
				memcpy(&f, d, 8);
			}
			PUT("%g", f);
			break;
		}
		default:                                               /* u, o, !, and the formatters of {bluetooth:...} */
			PUT("%llu", (unsigned long long)v);
			break;
		}
	}
	out[o < room ? o : room - 1] = 0;
#undef PUT
}

static void
log_say(os_log_type_t type, const char *format, uint8_t *buf, uint32_t size)
{
	static int console = -1;
	char line[1100];
	size_t n;

	static __thread int inside;

	if (type == OS_LOG_TYPE_DEBUG || format == NULL || inside || log_said++ >= 3000) {
		return;
	}
	inside = 1;                     /* (a description that logs is not formatted again) */
	if (console < 0) {
		console = open("/dev/console", O_WRONLY | O_NOCTTY | O_CLOEXEC);
	}
	memcpy(line, "bt: ", 4);
	log_format(line + 4, sizeof(line) - 6, format, buf, size);
	n = strlen(line);
	while (n > 4 && line[n - 1] == '\n') {
		n--;
	}
	line[n++] = '\n';
	if (console >= 0) {
		(void)write(console, line, n);
	}
	inside = 0;
}

static void
my_os_log_impl(void *dso, os_log_t log, os_log_type_t type, const char *format, uint8_t *buf, uint32_t size)
{
	log_say(type, format, buf, size);
	_os_log_impl(dso, log, type, format, buf, size);
}

static void
my_os_log_error_impl(void *dso, os_log_t log, os_log_type_t type, const char *format, uint8_t *buf, uint32_t size)
{
	log_say(type, format, buf, size);
	_os_log_error_impl(dso, log, type, format, buf, size);
}

static void
my_os_log_fault_impl(void *dso, os_log_t log, os_log_type_t type, const char *format, uint8_t *buf, uint32_t size)
{
	log_say(type, format, buf, size);
	_os_log_fault_impl(dso, log, type, format, buf, size);
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
	{ (const void *)my_SecItemCopyMatching, (const void *)SecItemCopyMatching },
	{ (const void *)my_SecItemAdd, (const void *)SecItemAdd },
	{ (const void *)my_SecItemUpdate, (const void *)SecItemUpdate },
	{ (const void *)my_SecItemDelete, (const void *)SecItemDelete },
	{ (const void *)my_os_log_impl, (const void *)_os_log_impl },
	{ (const void *)my_os_log_error_impl, (const void *)_os_log_error_impl },
	{ (const void *)my_os_log_fault_impl, (const void *)_os_log_fault_impl },
};

__attribute__((constructor)) static void
btd_init(void)
{
	const char *mode = getenv("A34BT_BLUETOOL");

	const char *kc = getenv("A34BT_KEYCHAIN");

	bluetool_local = !(mode != NULL && strcmp(mode, "launchd") == 0);
	kc_local = !(kc != NULL && strcmp(kc, "securityd") == 0);
	unsetenv("DYLD_INSERT_LIBRARIES");     /* (not into what bluetoothd starts) */
	say("liba34bt-btd loaded: BlueTool %s, keychain (MobileBluetooth) %s, os_log on the console, exits said",
	    bluetool_local ? "answered here" : "left to launchd", kc_local ? "kept in " KC_FILE : "left to securityd");
}
