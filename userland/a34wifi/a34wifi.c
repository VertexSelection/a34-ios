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
 * Usage (as root):
 *   a34wifi                 the state and the kernel's Wi-Fi log
 *   a34wifi up N [SECONDS]  ask for steps up to N, then follow the log until the kernel is
 *                           done (or SECONDS, default 120, have passed)
 *   a34wifi scan            once step 7 is done: scan again (kern.a34wifi.rescan), and follow
 *   a34wifi log             the kernel's Wi-Fi log only
 *   a34wifi files           the firmware files in /a34/wifi: present, sizes, what they say
 *
 * Output goes to stdout; as a launchd job (tools/root-add-job.py) that is /dev/console, so it
 * lands in the kernel's persistent log too (tools/pull-log.sh).
 */
#include <errno.h>
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

static int
up(int step, int seconds)
{
	if (step < 1 || step > 9) {
		fprintf(stderr, "a34wifi: step %d: 1 to 9\n", step);
		return 2;
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
	fprintf(stderr, "usage: a34wifi [up N [SECONDS] | scan [SECONDS] | log | files]\n");
	return 2;
}
