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
 *    passwords of the services BluetoothGlobal, MobileBluetooth, BluetoothLE and BluetoothLESync in the system keychain (+0x254c30 get, +0x255334 set, +0x25602c and
 *    +0x25656c lists, +0x255e40 delete). securityd cannot store an item here: this kernel's AppleKeyStore does not
 *    wrap keys (selector 10, "SWEET AKS: securityd sel 10 ... -> 0xe00002c7"), so "Failed to set new identity root!",
 *    and the stack's init (+0x599048) restarts bluetoothd (+0x487df8 -> +0x90734 _exit(1)) -- 15 times in that boot.
 *    SecItemCopyMatching/Add/Update/Delete of those services are answered from
 *    /var/mobile/Library/Bluetooth/com.a34.keychain.plist (a binary plist, bluetoothd's own user: NOT encrypted --
 *    the pairing keys of this research port are readable by root); every other keychain call goes to securityd as
 *    before. A34BT_KEYCHAIN=securityd leaves bluetoothd's to securityd too.
 * 6. A crash said: SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGABRT print the faulting pc, lr, address and the frame-pointer
 *    chain as image+offset ("crash: SIGSEGV at 0x... pc bluetoothd+0x... lr ... frames ..."), then the signal is let
 *    through as before (the kernel's CRASH line still comes).
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
#include <signal.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <execinfo.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <xpc/xpc.h>

/*
 * The SDK marks the Mach-service connection unavailable on iOS; libxpc in the system has it (bluetoothd calls it).
 * Reached by its symbol name, for the call and for the interposer table.
 */
extern xpc_connection_t a34_xpc_create_mach_service(const char *name, dispatch_queue_t targetq, uint64_t flags)
    __asm__("_xpc_connection_create_mach_service");

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
	xpc_connection_t c = a34_xpc_create_mach_service(name, targetq, flags);

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

/*
 * bluetoothd's generic passwords: the services its helpers pass (+0x254c30 get, +0x255334 set, +0x25602c and +0x25656c
 * lists, +0x255e40 delete; a NULL service there is "MobileBluetooth"): BluetoothGlobal (its root keys: "Identity
 * Root", "Encryption Root", cloud keys, "Local Device Static Random Address" ...), MobileBluetooth (classic devices:
 * LinkKey, Pincode ...), BluetoothLE and BluetoothLESync (LE devices, listed by label). Boot 0989-bt-a: only
 * MobileBluetooth was caught, so the root keys still went to securityd and failed (-25300, -25291).
 */
static const char *const kc_services[] = { "MobileBluetooth", "BluetoothGlobal", "BluetoothLE", "BluetoothLESync" };

static pthread_mutex_t kc_lock = PTHREAD_MUTEX_INITIALIZER;
static CFMutableDictionaryRef kc_items;          /* "service|account" -> item (its attributes and v_Data) */
static bool kc_local = true;
static unsigned kc_said;

static void
kc_cstr(CFTypeRef v, char *out, size_t room)
{
	out[0] = 0;
	if (v != NULL && CFGetTypeID(v) == CFStringGetTypeID()) {
		(void)CFStringGetCString(v, out, (CFIndex)room, kCFStringEncodingUTF8);
	} else if (v != NULL) {
		snprintf(out, room, "(not a string)");
	} else {
		snprintf(out, room, "-");
	}
}

/* "service|account" of an item or a query (NULL when either is missing or not a string) */
static CFStringRef
kc_key(CFDictionaryRef d)
{
	CFTypeRef service = CFDictionaryGetValue(d, kSecAttrService), account = CFDictionaryGetValue(d, kSecAttrAccount);

	if (service == NULL || account == NULL || CFGetTypeID(service) != CFStringGetTypeID() ||
	    CFGetTypeID(account) != CFStringGetTypeID()) {
		return NULL;
	}
	return CFStringCreateWithFormat(NULL, NULL, CFSTR("%@|%@"), service, account);
}

static void
kc_load(void)
{
	CFDataRef data = NULL;
	CFPropertyListRef plist = NULL;
	struct stat st;
	int fd;

	if (kc_items != NULL) {
		return;
	}
	kc_items = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
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
		plist = CFPropertyListCreateWithData(NULL, data, kCFPropertyListMutableContainersAndLeaves, NULL, NULL);
		CFRelease(data);
	}
	if (plist != NULL && CFGetTypeID(plist) == CFDictionaryGetTypeID()) {
		/* every item under its own service and account (rekeyed: a file of 0988's kept only the account) */
		CFIndex n = CFDictionaryGetCount(plist);
		const void **k = malloc(sizeof(void *) * (size_t)(n ? n : 1)), **v = malloc(sizeof(void *) * (size_t)(n ? n : 1));

		CFDictionaryGetKeysAndValues(plist, k, v);
		for (CFIndex i = 0; i < n; i++) {
			CFStringRef key;

			if (CFGetTypeID(v[i]) != CFDictionaryGetTypeID()) {
				continue;
			}
			if (CFDictionaryGetValue(v[i], kSecAttrService) == NULL) {
				CFDictionarySetValue((CFMutableDictionaryRef)v[i], kSecAttrService, CFSTR("MobileBluetooth"));
			}
			if ((key = kc_key(v[i])) != NULL) {
				CFDictionarySetValue(kc_items, key, v[i]);
				CFRelease(key);
			}
		}
		free(k);
		free(v);
	}
	if (plist != NULL) {
		CFRelease(plist);
	}
	say("keychain: %ld items of bluetoothd's services from " KC_FILE, (long)CFDictionaryGetCount(kc_items));
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

/* a generic password of one of bluetoothd's services (or of its access group, with no service named) */
static bool
kc_is_ours(CFDictionaryRef query)
{
	CFTypeRef cls, service, group;

	if (!kc_local || query == NULL || CFGetTypeID(query) != CFDictionaryGetTypeID()) {
		return false;
	}
	cls = CFDictionaryGetValue(query, kSecClass);
	if (cls == NULL || !CFEqual(cls, kSecClassGenericPassword)) {
		return false;
	}
	service = CFDictionaryGetValue(query, kSecAttrService);
	if (service != NULL && CFGetTypeID(service) == CFStringGetTypeID()) {
		char name[64];

		kc_cstr(service, name, sizeof(name));
		for (unsigned i = 0; i < sizeof(kc_services) / sizeof(kc_services[0]); i++) {
			if (strcmp(name, kc_services[i]) == 0) {
				return true;
			}
		}
		return false;
	}
	group = CFDictionaryGetValue(query, kSecAttrAccessGroup);
	return service == NULL && group != NULL && CFEqual(group, CFSTR("com.apple.bluetooth"));
}

/* the item matches the query's service, account and label, where the query names them */
static bool
kc_matches(CFDictionaryRef item, CFDictionaryRef query)
{
	static const CFStringRef *const keys[] = { &kSecAttrService, &kSecAttrAccount, &kSecAttrLabel };

	for (unsigned i = 0; i < 3; i++) {
		CFTypeRef want = CFDictionaryGetValue(query, *keys[i]), have = CFDictionaryGetValue(item, *keys[i]);

		if (want != NULL && (have == NULL || !CFEqual(want, have))) {
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

/* the attributes worth keeping: not the class, not the query's own keys (match limit, return flags, use-*) */
static void
kc_copy_attrs(CFMutableDictionaryRef into, CFDictionaryRef from)
{
	CFIndex n = CFDictionaryGetCount(from);
	const void **k = malloc(sizeof(void *) * (size_t)(n ? n : 1)), **v = malloc(sizeof(void *) * (size_t)(n ? n : 1));

	CFDictionaryGetKeysAndValues(from, k, v);
	for (CFIndex i = 0; i < n; i++) {
		char name[48];

		if (CFGetTypeID(k[i]) != CFStringGetTypeID()) {
			continue;
		}
		kc_cstr(k[i], name, sizeof(name));
		if (strcmp(name, "class") == 0 || strncmp(name, "m_", 2) == 0 || strncmp(name, "r_", 2) == 0 ||
		    strncmp(name, "u_", 2) == 0 || !kc_plist_value(v[i])) {
			continue;
		}
		CFDictionarySetValue(into, k[i], v[i]);
	}
	free(k);
	free(v);
}

/* "op service account [label]: keys ... -> status", for the log */
static void
kc_say(const char *op, CFDictionaryRef query, CFDictionaryRef more, OSStatus status, long count)
{
	char service[48], account[96], label[64], keys[400];
	size_t used = 0;
	CFIndex n;

	if (kc_said++ >= 200) {
		return;
	}
	kc_cstr(CFDictionaryGetValue(query, kSecAttrService), service, sizeof(service));
	kc_cstr(CFDictionaryGetValue(query, kSecAttrAccount), account, sizeof(account));
	kc_cstr(CFDictionaryGetValue(query, kSecAttrLabel), label, sizeof(label));
	keys[0] = 0;
	for (int pass = 0; pass < 2; pass++) {
		CFDictionaryRef d = pass == 0 ? query : more;
		const void **k;

		if (d == NULL || (n = CFDictionaryGetCount(d)) == 0) {
			continue;
		}
		k = malloc(sizeof(void *) * (size_t)n);
		CFDictionaryGetKeysAndValues(d, k, NULL);
		used += (size_t)snprintf(keys + used, sizeof(keys) - used, "%s", pass == 0 ? "" : " |");
		for (CFIndex i = 0; i < n && used < sizeof(keys) - 20; i++) {
			char name[32];

			kc_cstr(k[i], name, sizeof(name));
			used += (size_t)snprintf(keys + used, sizeof(keys) - used, " %s", name);
		}
		free(k);
	}
	say("keychain: %s %s/%s%s%s: %ld item(s), %d [keys:%s]", op, service, account, label[0] != '-' ? " label " : "",
	    label[0] != '-' ? label : "", count, (int)status, keys);
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

/* the items the query matches, as an array (+1); kc_lock held */
static CFMutableArrayRef
kc_find_locked(CFDictionaryRef query, bool keys_too)
{
	CFMutableArrayRef found = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	CFIndex n = CFDictionaryGetCount(kc_items);

	if (n > 0) {
		const void **k = malloc(sizeof(void *) * (size_t)n), **v = malloc(sizeof(void *) * (size_t)n);

		CFDictionaryGetKeysAndValues(kc_items, k, v);
		for (CFIndex i = 0; i < n; i++) {
			if (kc_matches(v[i], query)) {
				CFArrayAppendValue(found, keys_too ? k[i] : v[i]);
			}
		}
		free(k);
		free(v);
	}
	return found;
}

static OSStatus
my_SecItemCopyMatching(CFDictionaryRef query, CFTypeRef *result)
{
	CFTypeRef limit;
	bool all, data, attrs;
	CFMutableArrayRef found, out;
	OSStatus status;
	CFIndex n;

	if (!kc_is_ours(query)) {
		return SecItemCopyMatching(query, result);
	}
	limit = CFDictionaryGetValue(query, kSecMatchLimit);
	all = limit != NULL && CFEqual(limit, kSecMatchLimitAll);
	data = CFDictionaryGetValue(query, kSecReturnData) == kCFBooleanTrue;
	attrs = CFDictionaryGetValue(query, kSecReturnAttributes) == kCFBooleanTrue;
	pthread_mutex_lock(&kc_lock);
	kc_load();
	found = kc_find_locked(query, false);
	n = CFArrayGetCount(found);
	out = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	for (CFIndex i = 0; i < n && (all || i < 1); i++) {
		CFTypeRef r = kc_result(CFArrayGetValueAtIndex(found, i), data, attrs);

		CFArrayAppendValue(out, r);
		CFRelease(r);
	}
	pthread_mutex_unlock(&kc_lock);
	CFRelease(found);
	status = n != 0 ? errSecSuccess : errSecItemNotFound;
	kc_say(all ? "copy-all" : "copy", query, NULL, status, n);
	if (result != NULL) {
		*result = n == 0 ? NULL : all ? CFRetain(out) : CFRetain(CFArrayGetValueAtIndex(out, 0));
	}
	CFRelease(out);
	return status;
}

static OSStatus
my_SecItemAdd(CFDictionaryRef attributes, CFTypeRef *result)
{
	CFStringRef key;
	CFMutableDictionaryRef item;
	OSStatus status = errSecSuccess;

	if (!kc_is_ours(attributes)) {
		return SecItemAdd(attributes, result);
	}
	if (result != NULL) {
		*result = NULL;
	}
	item = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	kc_copy_attrs(item, attributes);
	if (CFDictionaryGetValue(item, kSecAttrService) == NULL) {
		CFDictionarySetValue(item, kSecAttrService, CFSTR("MobileBluetooth"));
	}
	if ((key = kc_key(item)) == NULL) {
		status = errSecParam;                   /* (no account) */
	} else {
		pthread_mutex_lock(&kc_lock);
		kc_load();
		if (CFDictionaryGetValue(kc_items, key) != NULL) {
			status = errSecDuplicateItem;
		} else {
			CFDictionarySetValue(kc_items, key, item);
			kc_save();
		}
		pthread_mutex_unlock(&kc_lock);
		CFRelease(key);
	}
	CFRelease(item);
	kc_say("add", attributes, NULL, status, status == errSecSuccess ? 1 : 0);
	return status;
}

static OSStatus
my_SecItemUpdate(CFDictionaryRef query, CFDictionaryRef update)
{
	CFMutableArrayRef found;
	CFIndex n;

	if (!kc_is_ours(query)) {
		return SecItemUpdate(query, update);
	}
	pthread_mutex_lock(&kc_lock);
	kc_load();
	found = kc_find_locked(query, false);
	n = CFArrayGetCount(found);
	for (CFIndex i = 0; i < n; i++) {
		kc_copy_attrs((CFMutableDictionaryRef)CFArrayGetValueAtIndex(found, i), update);
	}
	if (n != 0) {
		kc_save();
	}
	pthread_mutex_unlock(&kc_lock);
	CFRelease(found);
	kc_say("update", query, update, n != 0 ? errSecSuccess : errSecItemNotFound, n);
	return n != 0 ? errSecSuccess : errSecItemNotFound;
}

static OSStatus
my_SecItemDelete(CFDictionaryRef query)
{
	CFMutableArrayRef keys;
	CFIndex n;

	if (!kc_is_ours(query)) {
		return SecItemDelete(query);
	}
	pthread_mutex_lock(&kc_lock);
	kc_load();
	keys = kc_find_locked(query, true);
	n = CFArrayGetCount(keys);
	for (CFIndex i = 0; i < n; i++) {
		CFDictionaryRemoveValue(kc_items, CFArrayGetValueAtIndex(keys, i));
	}
	if (n != 0) {
		kc_save();
	}
	pthread_mutex_unlock(&kc_lock);
	CFRelease(keys);
	kc_say("delete", query, NULL, n != 0 ? errSecSuccess : errSecItemNotFound, n);
	return n != 0 ? errSecSuccess : errSecItemNotFound;
}

/* ---- 5. its os_log on the console ---- */

static unsigned log_said;

/*
 * The format with its arguments. buf is __builtin_os_log_format's: a summary byte, a count, then per item a descriptor
 * (high nibble its type: 0 scalar, 1 count (a precision), 2 string, 3 pointer (data), 4 object, 5 wide string,
 * 6 errno, 7 mask; low nibble privacy flags), a size and the data. A mask item -- "%{private, mask.hash}s" puts one
 * holding "hash" ahead of its string -- belongs to no conversion and is skipped (boot 0988-btkc-a: taken for the
 * string, it made "hash" a pointer, and bluetoothd crashed at 0x68736160). Only a string, an object or a pointer
 * item is followed, and only when it points above 4 GB (iOS user space starts after __PAGEZERO).
 */
#define LOG_ITEM_SCALAR 0
#define LOG_ITEM_COUNT  1
#define LOG_ITEM_STRING 2
#define LOG_ITEM_POINTER 3
#define LOG_ITEM_OBJECT 4
#define LOG_ITEM_MASK   7

struct log_item {
	uint8_t type, size;
	const uint8_t *data;
	uint64_t value;
};

/* the next item that is an argument (masks skipped); false when there is none */
static bool
log_next(const uint8_t *buf, uint32_t size, uint32_t *pos, uint32_t *left, struct log_item *it)
{
	while (*left > 0 && *pos + 2 <= size && *pos + 2u + buf[*pos + 1] <= size) {
		it->type = buf[*pos] >> 4;
		it->size = buf[*pos + 1];
		it->data = buf + *pos + 2;
		it->value = 0;
		memcpy(&it->value, it->data, it->size <= 8 ? it->size : 8);
		*pos += 2u + it->size;
		(*left)--;
		if (it->type != LOG_ITEM_MASK) {
			return true;
		}
	}
	return false;
}

static bool
log_pointer_ok(const struct log_item *it)
{
	return it->size == 8 && it->value >= 0x100000000ull && it->value < 0x1000000000000ull;
}

static void
log_format(char *out, size_t room, const char *format, const uint8_t *buf, uint32_t size)
{
	size_t o = 0;
	uint32_t pos = 2, left = size >= 2 ? buf[1] : 0;
	long precision;

#define PUT(...) do { if (o < room) { int k_ = snprintf(out + o, room - o, __VA_ARGS__); if (k_ > 0) { o += (size_t)k_; } } } while (0)
	for (const char *p = format; *p != 0 && o + 1 < room; p++) {
		struct log_item it;
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
			if (!log_next(buf, size, &pos, &left, &it)) {
				break;
			}
			precision = (long)it.value;
		}
		if (!log_next(buf, size, &pos, &left, &it)) {
			PUT("<?>");
			continue;
		}
		if (it.type == LOG_ITEM_STRING) {
			if (log_pointer_ok(&it)) {
				PUT("%.200s", (const char *)(uintptr_t)it.value);
			} else {
				PUT(it.value == 0 ? "(null)" : "<string>");
			}
		} else if (it.type == LOG_ITEM_OBJECT) {
			if (log_pointer_ok(&it)) {
				CFStringRef text = CFCopyDescription((CFTypeRef)(uintptr_t)it.value);
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
				PUT(it.value == 0 ? "(null)" : "<object>");
			}
		} else if (it.type == LOG_ITEM_POINTER) {
			if (log_pointer_ok(&it) && precision > 0) {
				const uint8_t *b = (const uint8_t *)(uintptr_t)it.value;

				for (long i = 0; i < precision && i < 32; i++) {
					PUT("%02x", b[i]);
				}
			} else {
				PUT("<data>");
			}
		} else if (it.type != LOG_ITEM_SCALAR && it.type != LOG_ITEM_COUNT) {
			PUT("<%u>", it.type);                          /* wide string, errno: not followed */
		} else {
			switch (conv) {
			case 'd': case 'i':
				PUT("%lld", it.size == 1 ? (long long)(int8_t)it.value : it.size == 2 ? (long long)(int16_t)it.value :
				    it.size == 4 ? (long long)(int32_t)it.value : (long long)it.value);
				break;
			case 'x': case 'X': case 'p':
				PUT("%#llx", (unsigned long long)it.value);
				break;
			case 'c':
				PUT("%c", (int)(it.value & 0xff));
				break;
			case 'f': case 'g': case 'e': {
				double f = 0;

				if (it.size == 8) {
					memcpy(&f, it.data, 8);
				}
				PUT("%g", f);
				break;
			}
			default:                                       /* u, o, !, and the formatters of {bluetooth:...} */
				PUT("%llu", (unsigned long long)it.value);
				break;
			}
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

/* ---- 6. crashes ---- */

static void
crash_where(uintptr_t a, char *out, size_t room)
{
	Dl_info info;

	if (a != 0 && dladdr((void *)a, &info) != 0 && info.dli_fname != NULL) {
		const char *image = strrchr(info.dli_fname, '/') != NULL ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname;

		snprintf(out, room, "%s+%#lx", image, (unsigned long)(a - (uintptr_t)info.dli_fbase));
	} else {
		snprintf(out, room, "%#lx", (unsigned long)a);
	}
}

static void
crash_handler(int sig, siginfo_t *si, void *context)
{
	ucontext_t *uc = context;
	char pc[96], lr[96], frames[900], one[96];
	size_t used = 0;
	uintptr_t fp = 0;

	frames[0] = 0;
	if (uc != NULL && uc->uc_mcontext != NULL) {
		crash_where((uintptr_t)uc->uc_mcontext->__ss.__pc, pc, sizeof(pc));
		crash_where((uintptr_t)uc->uc_mcontext->__ss.__lr, lr, sizeof(lr));
		fp = (uintptr_t)uc->uc_mcontext->__ss.__fp;
	} else {
		snprintf(pc, sizeof(pc), "?");
		snprintf(lr, sizeof(lr), "?");
	}
	for (int i = 0; i < 14 && fp >= 0x100000000ull && (fp & 7) == 0 && used < sizeof(frames) - 100; i++) {
		uintptr_t next = ((uintptr_t *)fp)[0], ret = ((uintptr_t *)fp)[1];

		crash_where(ret, one, sizeof(one));
		used += (size_t)snprintf(frames + used, sizeof(frames) - used, " %s", one);
		if (next <= fp) {
			break;
		}
		fp = next;
	}
	say("crash: signal %d at %#lx: pc %s lr %s frames%s", sig, si != NULL ? (unsigned long)(uintptr_t)si->si_addr : 0UL,
	    pc, lr, frames);
	signal(sig, SIG_DFL);                   /* returning re-runs the faulting instruction: the system's crash, as before */
	if (sig == SIGABRT || sig == SIGTRAP) {
		raise(sig);
	}
}

static void
crash_handlers(void)
{
	static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGABRT };
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = crash_handler;
	sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
	sigemptyset(&sa.sa_mask);
	for (unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) {
		(void)sigaction(sigs[i], &sa, NULL);
	}
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
	{ (const void *)my_create_mach_service, (const void *)a34_xpc_create_mach_service },
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
	crash_handlers();
	say("liba34bt-btd loaded: BlueTool %s, keychain (bluetoothd's services) %s, os_log on the console, exits and crashes said",
	    bluetool_local ? "answered here" : "left to launchd", kc_local ? "kept in " KC_FILE : "left to securityd");
}
