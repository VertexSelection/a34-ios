/*
 * a34wifi -- the Galaxy A34's Wi-Fi bring-up from user space (milestone 1 of docs/wifi-plan.md).
 *
 * The kernel side is xnu/patches/0940-a34-wifi-m1.patch (bsd/dev/a34_wifi.c). It does nothing
 * until it is asked, either by boot-arg a34wifi=N or by this tool, which writes the same N into
 * sysctl kern.a34wifi.want (root only). The steps, in order, each needing the ones before it:
 *
 *   1  read only: what the boot loader left (SPM, bus protection, PMIC rails, RC mode)
 *   2  CONNSYS (conn_infra) powered: PMIC VCN18/VCN13, pins, MTCMOS, bus protection off,
 *      conn_infra's IP version read (0x02060002)
 *   3  conn_infra configured as Android's conninfra.ko does on first power-on: EMI remap,
 *      d-die, RF SPI master, the MT6635 A-die (its chip id), AFE calibration, PLLs,
 *      low-power setup; VCN33_1/VCN33_2 for Wi-Fi
 *   4  the Wi-Fi subsystem (WFSYS) powered, the MCU's image /a34/wifi/soc5_0_ram_wmmcu_1_1_hdr.bin
 *      in CONNSYS's memory, the MCU's ROM done (0x1d1e)
 *   5  driver own, the host DMA's command and event rings, the ROM asked for its versions,
 *      /a34/wifi/WIFI_RAM_CODE_soc5_0_1_1.bin loaded, FW_FINISH answered, the firmware's ready
 *      bits (0x180600f0 = 3): "THE WI-FI FIRMWARE IS READY"
 *   6  (M2, 0961) the running firmware's command channel (the SW-WFDMA mailbox in CONNSYS's
 *      memory): its capabilities, its MAC address and version
 *   7  (M2) one scan: the station's BSS activated, a scan request, beacons and probe responses
 *      collected until SCAN_DONE: one line per network heard (BSSID, channel, RSSI, SSID)
 *
 *   8  (0964) join the network named in /a34/wifi/network (below): a scan for it, the
 *      loudest access point of that name chosen, authentication, association, for WPA2
 *      the four-way handshake and its keys installed: "JOINED" in the log
 *
 *   9  (0965) the data path: the network interface (kern.a34wifi.ifname, "enN") attached and
 *      its frames carried over the radio's data rings; then "a34wifi dhcp" for an address
 *
 * The network to join (0964): /a34/wifi/network, written by the phone's owner and by no one
 * else -- it is never in the repository, and nothing here prints the passphrase:
 *
 *   ssid=Name Of The Network
 *   psk=the passphrase          (8 to 63 characters, or 64 hex digits; no psk line: open)
 *   bssid=xx:xx:xx:xx:xx:xx     (optional: only this access point)
 *
 * Everything after the '=' up to the end of the line counts (spaces too). Mode 0600 is best.
 *
 * The phone's Wi-Fi address (0963): /a34/wifi/mac, one line "xx:xx:xx:xx:xx:xx". The chip's
 * efuse has none and Samsung's is in /efs, which nothing here reads; so this tool makes one
 * the first time it is needed -- random, locally administered, unicast -- and keeps it, so
 * that every phone has its own and keeps it from boot to boot. Step 6 reads it. Delete the
 * file to get a new one.
 *
 * Usage (as root):
 *   a34wifi                 the state and the kernel's Wi-Fi log
 *   a34wifi up N [SECONDS]  ask for steps up to N, then follow the log until the kernel is
 *                           done (or SECONDS, default 120, have passed)
 *   a34wifi scan            once step 7 is done: scan again (kern.a34wifi.rescan), and follow
 *   a34wifi log             the kernel's Wi-Fi log only
 *   a34wifi files           the firmware files in /a34/wifi: present, sizes, what they say
 *   a34wifi mac             the phone's Wi-Fi address, made and kept first if there is none
 *   a34wifi join [SECONDS]  check /a34/wifi/network, then ask for step 8 (as "up 8"); once step
 *                           8 has been done, join again (kern.a34wifi.rejoin) if the link was lost
 *   a34wifi dhcp [SECONDS]  (0965) once step 9 is done: have configd's IPConfiguration run DHCP
 *                           on the interface, and wait (default 60 s) for its IPv4 address
 *   a34wifi tcp HOST [PORT] connect to HOST (a name or an address; port 80), send an HTTP HEAD
 *                           request, print the first line of the answer
 *   a34wifi connect [HOST [PORT]]  all of it, for a boot job: "up 9", "dhcp", then "tcp" to
 *                           HOST (default 1.1.1.1, port 80: no name server needed)
 *                           ("up" does that too, before it asks the kernel for anything)
 *
 * Output goes to stdout; as a launchd job (tools/root-add-job.py) that is /dev/console, so it
 * lands in the kernel's persistent log too (tools/pull-log.sh).
 */
#include <errno.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#define A34WIFI_DIR     "/a34/wifi"
#define MAC_FILE        A34WIFI_DIR "/mac"
#define NETWORK_FILE    A34WIFI_DIR "/network"
#define LOG_MAX         (16 * 1024)

static int
get_int(const char *name, int *value)
{
	size_t len = sizeof(*value);

	return sysctlbyname(name, value, &len, NULL, 0);
}

static int
get_log(char *buf, size_t size)
{
	size_t len = size - 1;

	if (sysctlbyname("kern.a34wifi.log", buf, &len, NULL, 0) != 0) {
		return -1;
	}
	buf[len < size ? len : size - 1] = '\0';
	return 0;
}

static int
state(void)
{
	int want = 0, done = 0, busy = 0;

	if (get_int("kern.a34wifi.want", &want) != 0) {
		printf("a34wifi: no kern.a34wifi in this kernel (%s): it needs patch 0940\n", strerror(errno));
		return 1;
	}
	(void)get_int("kern.a34wifi.done", &done);
	(void)get_int("kern.a34wifi.busy", &busy);
	printf("a34wifi: asked for step %d, step %d done%s\n", want, done, busy ? ", working" : "");
	if (done >= 8) {
		int joined = 0;
		char ifname[16] = "";
		size_t len = sizeof(ifname) - 1;

		(void)get_int("kern.a34wifi.joined", &joined);
		(void)sysctlbyname("kern.a34wifi.ifname", ifname, &len, NULL, 0);
		printf("a34wifi: %s%s%s\n", joined ? "joined" : "NOT joined (a34wifi join: again)", ifname[0] ? ", interface " : "",
		    ifname);
	}
	return 0;
}

static int
print_log(void)
{
	static char buf[LOG_MAX];

	if (get_log(buf, sizeof(buf)) != 0) {
		printf("a34wifi: kern.a34wifi.log: %s\n", strerror(errno));
		return 1;
	}
	fputs(buf, stdout);
	if (buf[0] != '\0' && buf[strlen(buf) - 1] != '\n') {
		putchar('\n');
	}
	return 0;
}

/* Prints what the log gained since *printed bytes of it were printed. */
static void
follow(size_t *printed)
{
	static char buf[LOG_MAX];
	size_t len;

	if (get_log(buf, sizeof(buf)) != 0) {
		return;
	}
	len = strlen(buf);
	if (len < *printed) {           /* the ring wrapped: print it all again */
		*printed = 0;
	}
	if (len > *printed) {
		fputs(buf + *printed, stdout);
		fflush(stdout);
		*printed = len;
	}
}

static int ask(const char *name, int value, int step, int seconds);

/* ---- the phone's own Wi-Fi address (0963) ---- */

/* one line "xx:xx:xx:xx:xx:xx" with a locally administered unicast address, as the kernel reads it */
static int
mac_parse(const char *text, uint8_t mac[6])
{
	unsigned v[6];
	char rest[8] = "";
	int n = sscanf(text, " %2x:%2x:%2x:%2x:%2x:%2x %7s", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], rest);

	if (n != 6) {
		return -1;
	}
	for (int i = 0; i < 6; i++) {
		mac[i] = (uint8_t)v[i];
	}
	return (mac[0] & 3) == 2 ? 0 : -1;
}

/* says the kept address; makes and keeps a new one if there is no file. 0: there is one now */
static int
keep_mac(void)
{
	char text[65], line[32];
	uint8_t mac[6];
	ssize_t got;
	int fd = open(MAC_FILE, O_RDONLY);

	if (fd >= 0) {
		got = read(fd, text, sizeof(text) - 1);
		close(fd);
		text[got > 0 ? got : 0] = '\0';
		if (got > 0 && got < (ssize_t)sizeof(text) - 1 && mac_parse(text, mac) == 0) {
			printf("a34wifi: Wi-Fi address %02x:%02x:%02x:%02x:%02x:%02x (kept in %s)\n", mac[0], mac[1], mac[2],
			    mac[3], mac[4], mac[5], MAC_FILE);
			return 0;
		}
		printf("a34wifi: %s is not one line \"xx:xx:xx:xx:xx:xx\" with a locally administered unicast address; "
		    "the kernel will not use it (delete it and run 'a34wifi mac' for a new one)\n", MAC_FILE);
		return 1;
	}
	if (errno != ENOENT) {
		printf("a34wifi: %s: %s\n", MAC_FILE, strerror(errno));
		return 1;
	}
	arc4random_buf(mac, sizeof(mac));
	mac[0] = (uint8_t)((mac[0] & 0xfc) | 0x02);     /* locally administered, unicast */
	snprintf(line, sizeof(line), "%02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	fd = open(MAC_FILE, O_WRONLY | O_CREAT | O_EXCL, 0644);
	if (fd < 0 || write(fd, line, strlen(line)) != (ssize_t)strlen(line) || fsync(fd) != 0) {
		printf("a34wifi: %s not written: %s (the kernel will use a random address for this boot)\n", MAC_FILE,
		    strerror(errno));
		if (fd >= 0) {
			close(fd);
			unlink(MAC_FILE);
		}
		return 1;
	}
	close(fd);
	printf("a34wifi: new Wi-Fi address %.17s, kept in %s from now on\n", line, MAC_FILE);
	return 0;
}

/* ---- the network to join (0964) ---- */

/* what /a34/wifi/network asks for, said without the passphrase; 0 if the kernel will take it */
static int
check_network(void)
{
	char text[1024], *line, *next;
	int fd = open(NETWORK_FILE, O_RDONLY), ssid = 0, bad = 0;
	struct stat st;
	ssize_t got;
	const char *psk = "none: an open network";

	if (fd < 0) {
		printf("a34wifi: %s: %s. Write it on the phone's root (it is never in the repository):\n"
		    "    ssid=Name Of The Network\n    psk=the passphrase   (no psk line: an open network)\n", NETWORK_FILE,
		    strerror(errno));
		return 1;
	}
	if (fstat(fd, &st) == 0 && (st.st_mode & 077) != 0) {
		printf("a34wifi: %s can be read by others (mode %03o): chmod 600 is better\n", NETWORK_FILE,
		    (unsigned)(st.st_mode & 0777));
	}
	got = read(fd, text, sizeof(text) - 1);
	close(fd);
	if (got <= 0 || got >= (ssize_t)sizeof(text) - 1) {
		printf("a34wifi: %s: empty, unreadable or longer than %zu bytes\n", NETWORK_FILE, sizeof(text) - 2);
		return 1;
	}
	text[got] = '\0';
	for (line = text; line != NULL && *line != '\0'; line = next) {
		size_t n;

		next = strchr(line, '\n');
		if (next != NULL) {
			*next++ = '\0';
		}
		n = strlen(line);
		if (n > 0 && line[n - 1] == '\r') {
			line[--n] = '\0';
		}
		if (n == 0 || line[0] == '#') {
			continue;
		}
		if (strncmp(line, "ssid=", 5) == 0) {
			ssid = 1;
			if (n - 5 < 1 || n - 5 > 32) {
				printf("a34wifi: ssid= is not 1 to 32 bytes\n");
				bad = 1;
			} else {
				printf("a34wifi: network \"%s\"\n", line + 5);
			}
		} else if (strncmp(line, "psk=", 4) == 0) {
			size_t k = n - 4, hex = 0;

			for (size_t i = 4; i < n; i++) {
				hex += (line[i] >= '0' && line[i] <= '9') || (line[i] >= 'a' && line[i] <= 'f') ||
				    (line[i] >= 'A' && line[i] <= 'F');
			}
			psk = k == 64 && hex == 64 ? "WPA2-PSK, a 64-digit key" : k >= 8 && k <= 63 ? "WPA2-PSK, a passphrase" : NULL;
			if (psk == NULL) {
				printf("a34wifi: psk= is neither 8 to 63 characters nor 64 hex digits (%zu characters)\n", k);
				psk = "unusable";
				bad = 1;
			}
		} else if (strncmp(line, "bssid=", 6) == 0) {
			printf("a34wifi: only the access point %s\n", line + 6);
		} else {
			printf("a34wifi: a line that is not ssid=, psk= or bssid= (line %zu bytes)\n", n);
			bad = 1;
		}
	}
	if (!ssid) {
		printf("a34wifi: %s has no ssid= line\n", NETWORK_FILE);
		bad = 1;
	}
	printf("a34wifi: security: %s\n", psk);
	return bad;
}

static int
up(int step, int seconds)
{
	if (step < 1 || step > 9) {
		fprintf(stderr, "a34wifi: step %d: 1 to 9\n", step);
		return 2;
	}
	if (step >= 6) {
		(void)keep_mac();       /* step 6 reads it; without it the kernel uses a random one for this boot */
	}
	if (step >= 8 && check_network() != 0) {
		printf("a34wifi: step 8 needs %s: asking for step 7 only\n", NETWORK_FILE);
		step = 7;
	}
	return ask("kern.a34wifi.want", step, step, seconds);
}

/* kern.a34wifi.rescan: the scan (step 7) once more */
static int
rescan(int seconds)
{
	return ask("kern.a34wifi.rescan", 1, 7, seconds);
}

/* writes `value` into sysctl `name`, then follows the log until the kernel is no longer busy */
static int
ask(const char *name, int value, int step, int seconds)
{
	int want = value, done = 0, busy = 0;
	size_t printed = 0;
	time_t until = time(NULL) + seconds;

	follow(&printed);
	if (sysctlbyname(name, NULL, NULL, &want, sizeof(want)) != 0) {
		printf("a34wifi: %s=%d: %s\n", name, want, strerror(errno));
		return 1;
	}
	printf("a34wifi: %s=%d\n", name, want);
	fflush(stdout);
	for (;;) {
		sleep(1);
		follow(&printed);
		if (get_int("kern.a34wifi.done", &done) != 0 || get_int("kern.a34wifi.busy", &busy) != 0) {
			printf("a34wifi: state: %s\n", strerror(errno));
			return 1;
		}
		if (!busy) {
			break;
		}
		if (time(NULL) >= until) {
			printf("a34wifi: still working after %d s (step %d done); the kernel goes on\n", seconds, done);
			return 1;
		}
	}
	printf("a34wifi: step %d done (asked for %d)\n", done, step);
	return done >= step ? 0 : 1;
}

/* ---- the firmware files ---- */

static uint8_t *
slurp(const char *path, size_t *size)
{
	struct stat st;
	uint8_t *data;
	ssize_t got;
	int fd = open(path, O_RDONLY);

	if (fd < 0) {
		return NULL;
	}
	if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > (64 << 20)) {
		close(fd);
		return NULL;
	}
	data = malloc((size_t)st.st_size);
	got = data != NULL ? read(fd, data, (size_t)st.st_size) : -1;
	close(fd);
	if (got != st.st_size) {
		free(data);
		return NULL;
	}
	*size = (size_t)st.st_size;
	return data;
}

static uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void
printable(char *out, const uint8_t *in, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		out[i] = (in[i] >= 0x20 && in[i] < 0x7f) ? (char)in[i] : '.';
	}
	out[n] = '\0';
}

static int
files(void)
{
	static const char *names[] = {
		"soc5_0_ram_wmmcu_1_1_hdr.bin",
		"WIFI_RAM_CODE_soc5_0_1_1.bin",
	};
	int bad = 0;

	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		char path[256], text[40];
		size_t size = 0;
		uint8_t *data;

		snprintf(path, sizeof(path), "%s/%s", A34WIFI_DIR, names[i]);
		data = slurp(path, &size);
		if (data == NULL) {
			printf("a34wifi: %s: missing or unreadable\n", path);
			bad = 1;
			continue;
		}
		if (i == 0 && size >= 48) {
			/* the *_hdr.bin header, 48 bytes: build time (16), platform (4), hardware and software
			 * versions (big-endian 16 bits each), load address (in CONNSYS's view: 0xf0000000 is
			 * the start of its memory) */
			char platform[8];

			printable(text, data, 16);
			printable(platform, data + 16, 4);
			printf("a34wifi: %s: %zu bytes, built %s, %s, hw 0x%04x sw 0x%04x, to 0x%08x\n", path, size, text,
			    platform, (unsigned)(data[20] << 8 | data[21]), (unsigned)(data[22] << 8 | data[23]),
			    le32(data + 24));
		} else if (i == 1 && size >= 36) {
			/* WIFI_RAM_CODE ends in its tailer; the last 36 bytes hold the build date */
			printable(text, data + size - 36, 36);
			printf("a34wifi: %s: %zu bytes, tail \"%s\"\n", path, size, text);
		} else {
			printf("a34wifi: %s: %zu bytes (too short?)\n", path, size);
			bad = 1;
		}
		free(data);
	}
	return bad;
}

/* step 8, or (once step 8 has been done) step 8 again: kern.a34wifi.rejoin */
static int
join(int seconds)
{
	int done = 0, joined = 0, rc;

	if (get_int("kern.a34wifi.done", &done) != 0 || done < 8) {
		return up(8, seconds);
	}
	(void)get_int("kern.a34wifi.joined", &joined);
	if (joined) {
		printf("a34wifi: joined already\n");
		return 0;
	}
	if (check_network() != 0) {
		return 1;
	}
	rc = ask("kern.a34wifi.rejoin", 1, done, seconds);
	(void)get_int("kern.a34wifi.joined", &joined);
	printf("a34wifi: %s\n", joined ? "joined again" : "NOT joined");
	return rc != 0 || !joined;
}

/* ---- an address by DHCP (0965) ---- */

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>

/* in the libraries, not in the iOS SDK's headers */
extern kern_return_t bootstrap_look_up(mach_port_t bootstrap, const char *name, mach_port_t *service);

/*
 * configd's IPConfiguration plugin, asked by hand as macOS's "ipconfig setservice IF DHCP" asks
 * it (bootp's ipconfig.defs, subsystem 20000, routine ipconfig_set_service, message 20011): it
 * makes a service for the interface, runs DHCP and publishes the address, the router and the
 * name servers, so the rest of iOS uses them. The message's layout is HakiOS's
 * userland/src/net_test.m (github.com/oosexgang/hakios-xnu-kernel, MIT). Root only.
 */
#pragma pack(push, 4)
struct set_request {
	mach_msg_header_t head;
	mach_msg_body_t body;
	mach_msg_ool_descriptor_t method;
	NDR_record_t ndr;
	char name[16];
	mach_msg_type_number_t method_length;
};
struct set_reply {
	mach_msg_header_t head;
	NDR_record_t ndr;
	kern_return_t result;
	char service[128];
	int status;
	mach_msg_max_trailer_t trailer;
};
#pragma pack(pop)

static int
ask_dhcp(const char *interface)
{
	static const char *const statuses[] = { "done", "permission denied", "no such interface", "invalid parameter",
		                                "invalid operation", "allocation failed", "internal error", "not supported" };
	const void *keys[1] = { CFSTR("ConfigMethod") }, *values[1] = { CFSTR("DHCP") };
	CFDictionaryRef plist = CFDictionaryCreate(NULL, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	CFDataRef method = plist != NULL ? CFPropertyListCreateData(NULL, plist, kCFPropertyListBinaryFormat_v1_0, 0, NULL) : NULL;
	mach_port_t server = MACH_PORT_NULL, reply_port = mig_get_reply_port();
	union {
		struct set_request request;
		struct set_reply reply;
	} message;
	kern_return_t result;
	int status = -1;

	if (method == NULL) {
		printf("a34wifi: dhcp: no property list\n");
		return 1;
	}
	result = bootstrap_look_up(bootstrap_port, "com.apple.network.IPConfiguration", &server);
	if (result != KERN_SUCCESS) {
		printf("a34wifi: dhcp: configd's IPConfiguration is not there (0x%x)\n", result);
		CFRelease(method);
		CFRelease(plist);
		return 1;
	}
	memset(&message, 0, sizeof(message));
	message.request.head.msgh_bits = MACH_MSGH_BITS_COMPLEX | MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE);
	message.request.head.msgh_size = sizeof(message.request);
	message.request.head.msgh_remote_port = server;
	message.request.head.msgh_local_port = reply_port;
	message.request.head.msgh_id = 20011;
	message.request.body.msgh_descriptor_count = 1;
	message.request.method.address = (void *)CFDataGetBytePtr(method);
	message.request.method.size = (mach_msg_size_t)CFDataGetLength(method);
	message.request.method.deallocate = FALSE;
	message.request.method.copy = MACH_MSG_VIRTUAL_COPY;
	message.request.method.type = MACH_MSG_OOL_DESCRIPTOR;
	message.request.ndr = NDR_record;
	strlcpy(message.request.name, interface, sizeof(message.request.name));
	message.request.method_length = (mach_msg_type_number_t)CFDataGetLength(method);
	result = mach_msg(&message.request.head, MACH_SEND_MSG | MACH_RCV_MSG, sizeof(message.request), sizeof(message),
	    reply_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	if (result != KERN_SUCCESS) {
		printf("a34wifi: dhcp: the message did not go through (0x%x)\n", result);
	} else if (message.reply.result != KERN_SUCCESS) {
		printf("a34wifi: dhcp: IPConfiguration does not take the message (%d)\n", message.reply.result);
	} else {
		status = message.reply.status;
		message.reply.service[sizeof(message.reply.service) - 1] = '\0';
		printf("a34wifi: dhcp: IPConfiguration, \"setservice %s DHCP\": %d (%s), service %s\n", interface, status,
		    status >= 0 && status < 8 ? statuses[status] : "?", message.reply.service);
	}
	mach_port_deallocate(mach_task_self(), server);
	CFRelease(method);
	CFRelease(plist);
	return status == 0 ? 0 : 1;
}
#else
static int
ask_dhcp(const char *interface)
{
	printf("a34wifi: dhcp %s: only on iOS (configd's IPConfiguration)\n", interface);
	return 1;
}
#endif

/* the interface's IPv4 address, if it has one */
static int
ipv4_of(const char *interface, char *text, size_t size)
{
	struct ifaddrs *list = NULL, *a;
	int found = 0;

	if (getifaddrs(&list) != 0) {
		return 0;
	}
	for (a = list; a != NULL && !found; a = a->ifa_next) {
		if (a->ifa_addr != NULL && a->ifa_addr->sa_family == AF_INET && strcmp(a->ifa_name, interface) == 0) {
			found = inet_ntop(AF_INET, &((struct sockaddr_in *)(void *)a->ifa_addr)->sin_addr, text,
			    (socklen_t)size) != NULL;
		}
	}
	freeifaddrs(list);
	return found;
}

static int
dhcp(int seconds)
{
	char interface[16] = "", address[INET_ADDRSTRLEN];
	size_t len = sizeof(interface) - 1;
	time_t until = time(NULL) + seconds;

	if (sysctlbyname("kern.a34wifi.ifname", interface, &len, NULL, 0) != 0 || interface[0] == '\0') {
		printf("a34wifi: dhcp: no Wi-Fi interface yet (kern.a34wifi.ifname: %s): 'a34wifi up 9' first\n",
		    interface[0] == '\0' && errno == 0 ? "empty" : strerror(errno));
		return 1;
	}
	interface[sizeof(interface) - 1] = '\0';
	if (ipv4_of(interface, address, sizeof(address))) {
		printf("a34wifi: %s has %s already\n", interface, address);
		return 0;
	}
	if (ask_dhcp(interface) != 0) {
		return 1;
	}
	while (time(NULL) < until) {
		sleep(1);
		if (ipv4_of(interface, address, sizeof(address))) {
			printf("a34wifi: %s: IPv4 address %s (DHCP)\n", interface, address);
			return 0;
		}
	}
	printf("a34wifi: %s: no IPv4 address after %d s\n", interface, seconds);
	return 1;
}

/* connect, an HTTP HEAD request, the first line of the answer: does the data path carry anything */
static int
tcp(const char *host, const char *port)
{
	struct addrinfo hints, *list = NULL;
	char request[256], answer[512], address[INET6_ADDRSTRLEN] = "?", *end;
	ssize_t n;
	int s, error;
	time_t t0 = time(NULL);

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	error = getaddrinfo(host, port, &hints, &list);
	if (error != 0) {
		printf("a34wifi: tcp %s: %s\n", host, gai_strerror(error));
		return 1;
	}
	if (list->ai_family == AF_INET) {
		inet_ntop(AF_INET, &((struct sockaddr_in *)(void *)list->ai_addr)->sin_addr, address, sizeof(address));
	} else if (list->ai_family == AF_INET6) {
		inet_ntop(AF_INET6, &((struct sockaddr_in6 *)(void *)list->ai_addr)->sin6_addr, address, sizeof(address));
	}
	s = socket(list->ai_family, SOCK_STREAM, 0);
	if (s < 0 || connect(s, list->ai_addr, list->ai_addrlen) != 0) {
		printf("a34wifi: tcp %s (%s) port %s: %s\n", host, address, port, strerror(errno));
		if (s >= 0) {
			close(s);
		}
		freeaddrinfo(list);
		return 1;
	}
	snprintf(request, sizeof(request), "HEAD / HTTP/1.0\r\nHost: %s\r\nUser-Agent: a34wifi\r\n\r\n", host);
	if (write(s, request, strlen(request)) < 0) {
		printf("a34wifi: tcp %s: write: %s\n", host, strerror(errno));
	}
	n = read(s, answer, sizeof(answer) - 1);
	answer[n > 0 ? n : 0] = '\0';
	if ((end = strpbrk(answer, "\r\n")) != NULL) {
		*end = '\0';
	}
	printf("a34wifi: tcp %s (%s) port %s: connected; the answer begins \"%s\" (%ld s)\n", host, address, port, answer,
	    (long)(time(NULL) - t0));
	close(s);
	freeaddrinfo(list);
	return n > 0 ? 0 : 1;
}

int
main(int argc, char **argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc == 1) {
		int r = state();

		return r != 0 ? r : print_log();
	}
	if (strcmp(argv[1], "up") == 0 && argc >= 3) {
		return up(atoi(argv[2]), argc >= 4 ? atoi(argv[3]) : 120);
	}
	if (strcmp(argv[1], "scan") == 0) {
		return rescan(argc >= 3 ? atoi(argv[2]) : 60);
	}
	if (strcmp(argv[1], "log") == 0) {
		return print_log();
	}
	if (strcmp(argv[1], "files") == 0) {
		return files();
	}
	if (strcmp(argv[1], "mac") == 0) {
		return keep_mac();
	}
	if (strcmp(argv[1], "join") == 0) {
		return join(argc >= 3 ? atoi(argv[2]) : 180);
	}
	if (strcmp(argv[1], "tcp") == 0 && argc >= 3) {
		return tcp(argv[2], argc >= 4 ? argv[3] : "80");
	}
	if (strcmp(argv[1], "connect") == 0) {
		int rc = up(9, 240);

		if (rc == 0) {
			rc = dhcp(60);
		}
		if (rc == 0) {
			rc = tcp(argc >= 3 ? argv[2] : "1.1.1.1", argc >= 4 ? argv[3] : "80");
		}
		(void)state();
		return rc;
	}
	if (strcmp(argv[1], "dhcp") == 0) {
		return dhcp(argc >= 3 ? atoi(argv[2]) : 60);
	}
	fprintf(stderr, "usage: a34wifi [up N [SECONDS] | scan [SECONDS] | log | files | mac | join [SECONDS] | "
	    "dhcp [SECONDS] | tcp HOST [PORT] | connect [HOST [PORT]]]\n");
	return 2;
}
