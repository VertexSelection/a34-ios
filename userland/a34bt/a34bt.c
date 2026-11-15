/*
 * a34bt: the Galaxy A34's Bluetooth controller from user space, without bluetoothd.
 *
 * Milestone 1 of docs/bluetooth-plan.md: the kernel (xnu/patches/0939-a34-bt-m1.patch, boot-arg
 * a34bt=1) powers CONNSYS and the Bluetooth subsystem of the MT6877, puts the firmware into its
 * memory, switches Bluetooth's function on and asks the controller the first six HCI commands. When
 * that worked it opens a raw HCI channel: the kernel control "com.a34.bt.hci" (root only, datagram:
 * one H4 packet per message, both ways). This program uses it to see the controller do real work:
 *
 *   a34bt status          the kernel's state (sysctl kern.a34bt)
 *   a34bt wait [SECS]     wait until the kernel says "up" or "failed" (default 120 s)
 *   a34bt cmd OPCODE [BYTE...]
 *                         one HCI command (hex: "a34bt cmd 1001", "a34bt cmd 0c13 41 33 34 00"),
 *                         prints what comes back until its Command Complete/Status
 *   a34bt probe [SECS]    wait, then: reset, version, address, name, features, buffer sizes, an LE
 *                         scan and a classic inquiry of SECS seconds each (default 8); one line per
 *                         result and "a34bt: PASS" or "a34bt: FAIL ..." at the end
 *   a34bt dump [SECS]     print every packet the controller sends for SECS seconds (default 10)
 *   a34bt scan [SECS]     LE for SECS seconds (default 20): how many devices, how many Apple ones, and whether
 *                         AirPods/Beats "proximity pairing" adverts are among them (their model bytes). With
 *                         bluetoothd holding the controller (kernel 0971, a34bt=2) the channel cannot send:
 *                         it then counts what bluetoothd's own scans bring in
 *
 * Every line goes to the console (/dev/console, so into the kernel's log, as a one-shot launchd job's stdout did
 * not in boot 0961-radio-a) and to stdout when that is something else.
 * Addresses of other devices are never printed, only how many were heard and how strongly.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>

/*
 * The iPhoneOS SDK has neither sys/kern_control.h nor sys/sys_domain.h; the ABI they describe is the
 * kernel's (xnu bsd/sys/kern_control.h, sys_domain.h) and has not changed in years.
 */
#if __has_include(<sys/kern_control.h>)
#include <sys/kern_control.h>
#include <sys/sys_domain.h>
#else
#ifndef AF_SYSTEM
#define AF_SYSTEM       32
#endif
#ifndef PF_SYSTEM
#define PF_SYSTEM       AF_SYSTEM
#endif
#define SYSPROTO_CONTROL 2
#define AF_SYS_CONTROL  2
#define MAX_KCTL_NAME   96
struct ctl_info {
	u_int32_t ctl_id;
	char ctl_name[MAX_KCTL_NAME];
};
struct sockaddr_ctl {
	u_char sc_len;
	u_char sc_family;
	u_int16_t ss_sysaddr;
	u_int32_t sc_id;
	u_int32_t sc_unit;
	u_int32_t sc_reserved[5];
};
#define CTLIOCGINFO     _IOWR('N', 3, struct ctl_info)
#endif

#define A34BT_CONTROL   "com.a34.bt.hci"
#define A34BT_SYSCTL    "kern.a34bt"
#define PKT_MAX         1100u           /* an H4 packet: ACL up to 1021 bytes of data on this controller */

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void
say(const char *fmt, ...)
{
	static int console = -2;
	static bool also_stdout = true;
	char line[512];
	va_list ap;
	int n;

	if (console == -2) {
		struct stat c, o;

		console = open("/dev/console", O_WRONLY | O_NOCTTY | O_CLOEXEC);
		/* stdout already the console (a launchd job's StandardOutPath): once is enough */
		also_stdout = !(console >= 0 && fstat(console, &c) == 0 && fstat(1, &o) == 0 && S_ISCHR(o.st_mode) &&
		    o.st_rdev == c.st_rdev);
	}
	va_start(ap, fmt);
	n = snprintf(line, sizeof(line), "a34bt: ");
	n += vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 2) {
		n = (int)sizeof(line) - 2;
	}
	line[n++] = '\n';
	if (console >= 0) {
		(void)write(console, line, (size_t)n);
	}
	if (also_stdout || console < 0) {
		(void)fwrite(line, 1, (size_t)n, stdout);
		fflush(stdout);
	}
}

static uint64_t
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* ---- the kernel's state ---- */

static int
kernel_state(char *out, size_t room)
{
	size_t len = room - 1;

	if (sysctlbyname(A34BT_SYSCTL, out, &len, NULL, 0) != 0) {
		snprintf(out, room, "(no %s: a kernel without patch 0939, errno %d)", A34BT_SYSCTL, errno);
		return -1;
	}
	out[len < room ? len : room - 1] = 0;
	return 0;
}

/* until the state begins with "up" (0) or "failed"/"off" (-1), or secs pass (-2); each new state is said */
static int
wait_up(unsigned secs)
{
	char state[512], last[512] = "";
	uint64_t until = now_ms() + (uint64_t)secs * 1000u;

	for (;;) {
		if (kernel_state(state, sizeof(state)) != 0) {
			say("%s", state);
			return -1;
		}
		if (strcmp(state, last) != 0) {
			say("kernel: %s", state);
			strlcpy(last, state, sizeof(last));
		}
		if (strncmp(state, "up", 2) == 0) {
			return 0;
		}
		if (strncmp(state, "failed", 6) == 0 || strncmp(state, "off", 3) == 0) {
			return -1;
		}
		if (now_ms() > until) {
			say("still not up after %u s", secs);
			return -2;
		}
		usleep(250000);
	}
}

/* ---- the raw HCI channel ---- */

static int
hci_open(void)
{
	struct ctl_info info;
	struct sockaddr_ctl addr;
	int fd = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);

	if (fd < 0) {
		say("socket(PF_SYSTEM): %s", strerror(errno));
		return -1;
	}
	memset(&info, 0, sizeof(info));
	strlcpy(info.ctl_name, A34BT_CONTROL, sizeof(info.ctl_name));
	if (ioctl(fd, CTLIOCGINFO, &info) != 0) {
		say("no kernel control %s (%s): the kernel is not up, or not patch 0939", A34BT_CONTROL, strerror(errno));
		close(fd);
		return -1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sc_len = sizeof(addr);
	addr.sc_family = AF_SYSTEM;
	addr.ss_sysaddr = AF_SYS_CONTROL;
	addr.sc_id = info.ctl_id;
	addr.sc_unit = 0;
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		say("connect %s: %s (root only)", A34BT_CONTROL, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

static void
hex_line(const char *what, const uint8_t *b, size_t n)
{
	char text[3 * 48 + 8];
	size_t used = 0, shown = n < 48 ? n : 48;

	text[0] = 0;
	for (size_t i = 0; i < shown; i++) {
		used += (size_t)snprintf(text + used, sizeof(text) - used, " %02x", b[i]);
	}
	say("%s %zu:%s%s", what, n, text, n > shown ? " ..." : "");
}

/* one packet from the controller within ms: its length, 0 if none, -1 on error */
static int
hci_read(int fd, uint8_t *buf, unsigned ms)
{
	struct pollfd p = { .fd = fd, .events = POLLIN };
	int r = poll(&p, 1, (int)ms);
	ssize_t n;

	if (r <= 0) {
		return r < 0 && errno != EINTR ? -1 : 0;
	}
	n = recv(fd, buf, PKT_MAX, 0);
	if (n < 0) {
		return errno == EAGAIN || errno == EINTR ? 0 : -1;
	}
	return (int)n;
}

static int send_errno;                  /* of the last send that failed (EBUSY: bluetoothd has the controller) */

static int
hci_send(int fd, uint16_t opcode, const uint8_t *params, uint8_t len)
{
	uint8_t pkt[4 + 255];

	pkt[0] = 0x01;
	pkt[1] = (uint8_t)opcode;
	pkt[2] = (uint8_t)(opcode >> 8);
	pkt[3] = len;
	if (len != 0) {
		memcpy(pkt + 4, params, len);
	}
	if (send(fd, pkt, 4u + len, 0) != (ssize_t)(4u + len)) {
		send_errno = errno;
		if (send_errno != EBUSY) {
			say("send %04x: %s", opcode, strerror(send_errno));
		}
		return -1;
	}
	return 0;
}

typedef void (*event_fn)(const uint8_t *pkt, int len, void *ctx);

/*
 * A command and its answer: the Command Complete (copied to out, *out_len) or Command Status of its opcode,
 * within ms. Everything else that comes meanwhile goes to other() (or is said when verbose). Returns the
 * status byte, or -1 when nothing answered.
 */
static int
hci_command(int fd, uint16_t opcode, const uint8_t *params, uint8_t len, uint8_t *out, int *out_len,
    unsigned ms, event_fn other, void *ctx, int verbose)
{
	uint8_t pkt[PKT_MAX];
	uint64_t until = now_ms() + ms;

	if (hci_send(fd, opcode, params, len) != 0) {
		return -1;
	}
	while (now_ms() < until) {
		int n = hci_read(fd, pkt, 50);

		if (n < 0) {
			say("recv: %s", strerror(errno));
			return -1;
		}
		if (n == 0) {
			continue;
		}
		if (verbose) {
			hex_line("in ", pkt, (size_t)n);
		}
		if (n >= 7 && pkt[0] == 0x04 && pkt[1] == 0x0e && (pkt[4] | (pkt[5] << 8)) == opcode) {
			if (out != NULL) {
				memcpy(out, pkt, (size_t)n);
				*out_len = n;
			}
			return pkt[6];
		}
		if (n >= 7 && pkt[0] == 0x04 && pkt[1] == 0x0f && (pkt[5] | (pkt[6] << 8)) == opcode) {
			if (out != NULL) {
				memcpy(out, pkt, (size_t)n);
				*out_len = n;
			}
			return pkt[3];
		}
		if (other != NULL) {
			other(pkt, n, ctx);
		}
	}
	say("command %04x: no answer in %u ms", opcode, ms);
	return -1;
}

/* ---- probe ---- */

struct heard {
	uint8_t addr[64][7];            /* address type + address, only to count each device once */
	unsigned unique, reports, inquiry, inquiry_done;
	int best_rssi;
	/* Apple's adverts (manufacturer data, company 0x004c), and its "proximity pairing" ones (type 0x07: AirPods, Beats) */
	uint8_t apple_addr[32][6];
	unsigned apple_unique, apple_reports, proximity;
	int proximity_rssi;
	uint8_t model[2];
};

/* one LE advert: its AD structures looked through for Apple's manufacturer data */
static void
heard_le_data(struct heard *h, const uint8_t *addr, const uint8_t *d, unsigned len, int rssi)
{
	bool apple = false;

	for (unsigned at = 0; at + 1 < len; ) {
		unsigned l = d[at];

		if (l == 0 || at + 1 + l > len) {
			break;
		}
		if (d[at + 1] == 0xff && l >= 3 && d[at + 2] == 0x4c && d[at + 3] == 0x00) {
			apple = true;
			if (l >= 8 && d[at + 4] == 0x07) {      /* 07 <length> <prefix> <model, 2 bytes> <status> ... */
				h->proximity++;
				if (rssi != 127 && (h->proximity_rssi == 0 || rssi > h->proximity_rssi)) {
					h->proximity_rssi = rssi;
					h->model[0] = d[at + 7];
					h->model[1] = d[at + 8];
				}
			}
		}
		at += 1 + l;
	}
	if (apple) {
		unsigned i;

		h->apple_reports++;
		for (i = 0; i < h->apple_unique; i++) {
			if (memcmp(h->apple_addr[i], addr, 6) == 0) {
				break;
			}
		}
		if (i == h->apple_unique && i < 32) {
			memcpy(h->apple_addr[i], addr, 6);
			h->apple_unique++;
		}
	}
}

static void
heard_add(struct heard *h, uint8_t type, const uint8_t *addr, int rssi)
{
	unsigned i;

	for (i = 0; i < h->unique && i < 64; i++) {
		if (h->addr[i][0] == type && memcmp(h->addr[i] + 1, addr, 6) == 0) {
			break;
		}
	}
	if (i == h->unique && i < 64) {
		h->addr[i][0] = type;
		memcpy(h->addr[i] + 1, addr, 6);
		h->unique++;
	}
	if (rssi != 127 && rssi > h->best_rssi) {
		h->best_rssi = rssi;
	}
}

/* LE Advertising Reports (04 3e .. 02) and classic inquiry results (02, 22, 2f), inquiry complete (01) */
static void
on_event(const uint8_t *p, int n, void *ctx)
{
	struct heard *h = ctx;

	if (n >= 5 && p[0] == 0x04 && p[1] == 0x3e && p[3] == 0x02) {          /* LE Advertising Report */
		unsigned count = p[4], at = 5;

		for (unsigned i = 0; i < count && (int)(at + 9) <= n; i++) {
			unsigned dlen = p[at + 8];

			if ((int)(at + 9 + dlen + 1) > n) {
				break;
			}
			heard_add(h, (uint8_t)(0x10 | p[at + 1]), p + at + 2, (int8_t)p[at + 9 + dlen]);
			heard_le_data(h, p + at + 2, p + at + 9, dlen, (int8_t)p[at + 9 + dlen]);
			h->reports++;
			at += 10 + dlen;
		}
	} else if (n >= 5 && p[0] == 0x04 && p[1] == 0x3e && p[3] == 0x0d) {   /* LE Extended Advertising Report */
		unsigned count = p[4], at = 5;

		for (unsigned i = 0; i < count && (int)(at + 24) <= n; i++) {
			unsigned dlen = p[at + 23];

			if ((int)(at + 24 + dlen) > n) {
				break;
			}
			heard_add(h, (uint8_t)(0x10 | p[at + 2]), p + at + 3, (int8_t)p[at + 13]);
			heard_le_data(h, p + at + 3, p + at + 24, dlen, (int8_t)p[at + 13]);
			h->reports++;
			at += 24 + dlen;
		}
	} else if (n >= 4 && p[0] == 0x04 && (p[1] == 0x02 || p[1] == 0x22 || p[1] == 0x2f)) {
		unsigned count = p[1] == 0x2f ? 1u : p[3];
		int rssi = 127;

		if (p[1] == 0x22 && n >= 4 + 14) {
			rssi = (int8_t)p[4 + 13];
		} else if (p[1] == 0x2f && n >= 4 + 14) {
			rssi = (int8_t)p[4 + 13];
		}
		if (n >= 10) {
			heard_add(h, 0x00, p + 4, rssi);
		}
		h->inquiry += count;
	} else if (n >= 4 && p[0] == 0x04 && p[1] == 0x01) {
		h->inquiry_done = 1;
	}
}

static void
say_apple(const struct heard *h)
{
	say("Apple: %u adverts from %u devices; AirPods/Beats proximity pairing: %u adverts%s", h->apple_reports, h->apple_unique,
	    h->proximity, h->proximity ? "" : " (none: AirPods in their open case, near the phone, send them)");
	if (h->proximity) {
		say("the strongest proximity-pairing advert: %d dBm, model bytes %02x %02x (AirPods Pro 2: 14 20 or 24 20)",
		    h->proximity_rssi, h->model[0], h->model[1]);
	}
}

/*
 * LE for secs seconds, passive. When the kernel refuses our packets (EBUSY: bluetoothd has the controller, 0971),
 * only listens: the reports bluetoothd's own scans bring in come through the channel too.
 */
static int
scan(unsigned secs)
{
	static const uint8_t event_mask[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0x1f, 0x00, 0x20 };
	static const uint8_t scan_params[7] = { 0x00, 0x60, 0x00, 0x30, 0x00, 0x00, 0x00 };
	static const uint8_t scan_on[2] = { 0x01, 0x00 }, scan_off[2] = { 0x00, 0x00 };
	uint8_t a[PKT_MAX];
	int n = 0, st, fd;
	bool ours = true;
	struct heard h;
	uint64_t until;

	if (wait_up(120) != 0 || (fd = hci_open()) < 0) {
		say("FAIL: no controller");
		return 2;
	}
	memset(&h, 0, sizeof(h));
	h.best_rssi = -128;
	send_errno = 0;
	st = hci_command(fd, 0x0c01, event_mask, sizeof(event_mask), a, &n, 3000, on_event, &h, 0);
	if (st < 0 && send_errno == EBUSY) {
		ours = false;
		say("bluetoothd has the controller: listening %u s to what its scans hear", secs);
	} else {
		st = hci_command(fd, 0x200b, scan_params, sizeof(scan_params), a, &n, 3000, on_event, &h, 0);
		if (st == 0) {
			st = hci_command(fd, 0x200c, scan_on, sizeof(scan_on), a, &n, 3000, on_event, &h, 0);
		}
		if (st != 0) {
			say("LE scan could not start (%d): listening anyway", st);
		}
	}
	until = now_ms() + (uint64_t)secs * 1000u;
	while (now_ms() < until) {
		int got = hci_read(fd, a, 100);

		if (got > 0) {
			on_event(a, got, &h);
		} else if (got < 0) {
			break;
		}
	}
	if (ours) {
		(void)hci_command(fd, 0x200c, scan_off, sizeof(scan_off), a, &n, 3000, on_event, &h, 0);
	}
	close(fd);
	say("LE %s %u s: %u advertising reports from %u devices, strongest %d dBm", ours ? "scan" : "listening", secs, h.reports,
	    h.unique, h.unique ? h.best_rssi : 0);
	say_apple(&h);
	return h.reports != 0 ? 0 : 4;
}

static int
probe(unsigned secs)
{
	static const uint8_t event_mask[8] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0x1f, 0x00, 0x20 };    /* + LE Meta */
	static const uint8_t scan_params[7] = { 0x00, 0x60, 0x00, 0x30, 0x00, 0x00, 0x00 };         /* passive, 60/30 ms */
	static const uint8_t scan_on[2] = { 0x01, 0x00 }, scan_off[2] = { 0x00, 0x00 };
	static const uint8_t inquiry[5] = { 0x33, 0x8b, 0x9e, 0x00, 0x00 };                         /* GIAC, set below */
	uint8_t a[PKT_MAX], inq[5];
	int n = 0, st, fd, failures = 0;
	struct heard h;
	uint64_t until;

	if (wait_up(120) != 0) {
		say("FAIL: the kernel did not bring the controller up");
		return 2;
	}
	if ((fd = hci_open()) < 0) {
		say("FAIL: no raw HCI channel");
		return 2;
	}
	memset(&h, 0, sizeof(h));
	h.best_rssi = -128;

#define CMD(what, op, par, len) do { \
		st = hci_command(fd, (op), (par), (len), a, &n, 3000, on_event, &h, 0); \
		if (st != 0) { say("%s (%04x): %s %d", (what), (op), st < 0 ? "no answer" : "status", st); failures++; } \
	} while (0)

	CMD("HCI_Reset", 0x0c03, NULL, 0);
	CMD("Read_Local_Version_Information", 0x1001, NULL, 0);
	if (st == 0 && n >= 15) {
		say("controller: HCI version %u revision %#x, LMP version %u subversion %#x, manufacturer %#06x",
		    a[7], a[8] | (a[9] << 8), a[10], a[13] | (a[14] << 8), a[11] | (a[12] << 8));
	}
	CMD("Read_BD_ADDR", 0x1009, NULL, 0);
	if (st == 0 && n >= 13) {
		say("controller address %02x:%02x:%02x:%02x:%02x:%02x", a[12], a[11], a[10], a[9], a[8], a[7]);
	}
	CMD("Read_Local_Name", 0x0c14, NULL, 0);
	if (st == 0 && n >= 8) {
		char name[249];

		memcpy(name, a + 7, (size_t)(n - 7) < sizeof(name) ? (size_t)(n - 7) : sizeof(name) - 1);
		name[(size_t)(n - 7) < sizeof(name) ? (size_t)(n - 7) : sizeof(name) - 1] = 0;
		say("controller name \"%s\"", name);
	}
	CMD("Read_Local_Supported_Features", 0x1003, NULL, 0);
	if (st == 0 && n >= 15) {
		say("features %02x %02x %02x %02x %02x %02x %02x %02x (LE %s, BR/EDR %s)", a[7], a[8], a[9], a[10], a[11],
		    a[12], a[13], a[14], (a[11] & 0x40) ? "yes" : "no", (a[11] & 0x20) ? "not supported" : "yes");
	}
	CMD("Read_Buffer_Size", 0x1005, NULL, 0);
	if (st == 0 && n >= 14) {
		say("ACL %u bytes x %u, SCO %u x %u", a[7] | (a[8] << 8), a[10] | (a[11] << 8), a[9], a[12] | (a[13] << 8));
	}
	CMD("LE_Read_Buffer_Size", 0x2002, NULL, 0);
	if (st == 0 && n >= 10) {
		say("LE ACL %u bytes x %u", a[7] | (a[8] << 8), a[9]);
	}

	/* LE: a passive scan */
	CMD("Set_Event_Mask", 0x0c01, event_mask, sizeof(event_mask));
	CMD("LE_Set_Scan_Parameters", 0x200b, scan_params, sizeof(scan_params));
	CMD("LE_Set_Scan_Enable on", 0x200c, scan_on, sizeof(scan_on));
	if (st == 0) {
		until = now_ms() + (uint64_t)secs * 1000u;
		while (now_ms() < until) {
			int got = hci_read(fd, a, 100);

			if (got > 0) {
				on_event(a, got, &h);
			} else if (got < 0) {
				break;
			}
		}
		CMD("LE_Set_Scan_Enable off", 0x200c, scan_off, sizeof(scan_off));
		say("LE scan %u s: %u advertising reports from %u devices, strongest %d dBm", secs, h.reports, h.unique,
		    h.unique ? h.best_rssi : 0);
		say_apple(&h);
	}

	/* BR/EDR: a general inquiry of about secs seconds (units of 1.28 s) */
	{
		unsigned le_devices = h.unique;

		memcpy(inq, inquiry, sizeof(inq));
		inq[3] = (uint8_t)((secs * 100u + 127u) / 128u);
		if (inq[3] < 1) {
			inq[3] = 1;
		}
		h.best_rssi = -128;
		st = hci_command(fd, 0x0401, inq, sizeof(inq), a, &n, 3000, on_event, &h, 0);
		if (st != 0) {
			say("Inquiry (0401): %s %d", st < 0 ? "no answer" : "status", st);
			failures++;
		} else {
			until = now_ms() + (uint64_t)inq[3] * 1280u + 3000u;
			while (!h.inquiry_done && now_ms() < until) {
				int got = hci_read(fd, a, 100);

				if (got > 0) {
					on_event(a, got, &h);
				} else if (got < 0) {
					break;
				}
			}
			say("inquiry %u x 1.28 s: %u results from %u devices%s", inq[3], h.inquiry, h.unique - le_devices,
			    h.inquiry_done ? "" : " (no Inquiry_Complete)");
		}
	}
#undef CMD
	close(fd);
	if (failures != 0) {
		say("FAIL: %d command(s) not answered with status 0", failures);
		return 3;
	}
	say("PASS: the controller answers; %u LE reports and %u inquiry results heard", h.reports, h.inquiry);
	return 0;
}

static int
dump(unsigned secs)
{
	uint8_t pkt[PKT_MAX];
	uint64_t until = now_ms() + (uint64_t)secs * 1000u;
	int fd = hci_open(), n;
	unsigned count = 0;

	if (fd < 0) {
		return 2;
	}
	while (now_ms() < until) {
		if ((n = hci_read(fd, pkt, 100)) < 0) {
			break;
		}
		if (n > 0) {
			hex_line("in ", pkt, (size_t)n);
			count++;
		}
	}
	say("%u packets in %u s", count, secs);
	close(fd);
	return 0;
}

static int
parse_hex(const char *s, unsigned long max, unsigned long *out)
{
	char *end;
	unsigned long v;

	errno = 0;
	v = strtoul(s, &end, 16);
	if (errno != 0 || *s == 0 || *end != 0 || v > max) {
		return -1;
	}
	*out = v;
	return 0;
}

static int
command(int argc, char **argv)
{
	uint8_t params[255];
	unsigned long opcode, v;
	int fd, st, n = 0;
	uint8_t a[PKT_MAX];

	if (argc < 1 || parse_hex(argv[0], 0xffff, &opcode) != 0 || argc - 1 > 255) {
		fprintf(stderr, "usage: a34bt cmd OPCODE [BYTE...]   (hex, e.g. a34bt cmd 1001)\n");
		return 1;
	}
	for (int i = 1; i < argc; i++) {
		if (parse_hex(argv[i], 0xff, &v) != 0) {
			fprintf(stderr, "a34bt: \"%s\" is not a hex byte\n", argv[i]);
			return 1;
		}
		params[i - 1] = (uint8_t)v;
	}
	if ((fd = hci_open()) < 0) {
		return 2;
	}
	hex_line("out", params, (size_t)(argc - 1));
	st = hci_command(fd, (uint16_t)opcode, params, (uint8_t)(argc - 1), a, &n, 5000, NULL, NULL, 1);
	close(fd);
	return st == 0 ? 0 : 3;
}

int
main(int argc, char **argv)
{
	char state[512];
	unsigned secs;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc < 2 || strcmp(argv[1], "status") == 0) {
		int r = kernel_state(state, sizeof(state));

		say("%s", state);
		return r == 0 ? 0 : 2;
	}
	secs = argc >= 3 ? (unsigned)strtoul(argv[2], NULL, 10) : 0;
	if (strcmp(argv[1], "wait") == 0) {
		return wait_up(secs ? secs : 120) == 0 ? 0 : 2;
	}
	if (strcmp(argv[1], "probe") == 0) {
		return probe(secs ? (secs > 60 ? 60 : secs) : 8);
	}
	if (strcmp(argv[1], "scan") == 0) {
		return scan(secs ? (secs > 300 ? 300 : secs) : 20);
	}
	if (strcmp(argv[1], "dump") == 0) {
		return dump(secs ? secs : 10);
	}
	if (strcmp(argv[1], "cmd") == 0) {
		return command(argc - 2, argv + 2);
	}
	fprintf(stderr, "usage: a34bt status | wait [SECS] | probe [SECS] | scan [SECS] | dump [SECS] | cmd OPCODE [BYTE...]\n");
	return 1;
}
