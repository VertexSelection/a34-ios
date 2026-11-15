/*
 * a34cell: the cellular daemon of the A34 port (docs/cellular-plan.md).
 *
 *   a34cell [-port /dev/a34at0] [-state /var/run/a34cell.plist] [-radio] [-trace] [-once]
 *
 * Talks AT to the modem over the kernel's CCCI AT port and keeps a model of the SIM, the
 * registration, the signal, IMS and calls (model.h), which it writes as a property list and
 * announces with the Darwin notification "com.a34.cell.state" whenever it changes.
 *
 * Safe by construction (policy.h): an allowlist of commands, never the identity programming
 * commands, and the transmitter stays off unless -radio is given AND the modem reports a
 * well-formed identity of its own (which it does after reading the phone's NVRAM, read-only);
 * the identity itself is never logged, stored or passed on. Without a modem port (the kernel's
 * CCCI stage not active) it says so and exits 0.
 */
#include "at.h"
#include "model.h"
#include "policy.h"
#include "sms.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <notify.h>
#endif

static struct cell_state st;
static const char *state_path = "/var/run/a34cell.plist";
static const char *inbox_dir = "/var/mobile/Library/a34cell";
static volatile sig_atomic_t stop;

static void
on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static void
publish(void)
{
	static char last[4096];
	char buf[4096], tmp[512];
	int n = model_plist(&st, buf, sizeof(buf));

	if (n < 0 || strcmp(buf, last) == 0)
		return;
	memcpy(last, buf, (size_t)n + 1);
	snprintf(tmp, sizeof(tmp), "%s.tmp", state_path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	fwrite(buf, 1, (size_t)n, f);
	fclose(f);
	rename(tmp, state_path);
#if defined(__APPLE__)
	notify_post("com.a34.cell.state");
#endif
	fprintf(stderr, "a34cell: sim %d cfun %d reg cs%d eps%d 5gs%d act %d \"%s\" %s %d dBm (%d bars) ims %d/%d calls %d\n",
	    st.sim, st.cfun, st.creg.stat, st.cereg.stat, st.c5greg.stat,
	    st.c5greg.act >= 0 ? st.c5greg.act : st.cereg.act, st.oper, st.oper_num, model_dbm(&st),
	    model_bars(&st), st.ims, st.ims_ext, st.ncalls);
}

/* An SMS for the phone: kept in the user's own inbox directory on the phone (0600), never
 * logged beyond its size. Bridging it to Messages is a later stage. */
static void
store_sms(const char *pdu)
{
	struct sms_deliver m;
	char path[256];

	if (sms_decode_deliver(pdu, &m) != 0) {
		fprintf(stderr, "a34cell: SMS received, PDU not understood (%zu hex chars)\n", strlen(pdu));
		return;
	}
	st.sms_received++;
	fprintf(stderr, "a34cell: SMS received: %zu bytes of text%s, part %d/%d\n", strlen(m.text),
	    m.binary ? " (8-bit data)" : "", m.concat_seq ? m.concat_seq : 1,
	    m.concat_total ? m.concat_total : 1);
	mkdir(inbox_dir, 0700);
	snprintf(path, sizeof(path), "%s/%04d%02d%02d-%02d%02d%02d-%u.txt", inbox_dir, m.year, m.month,
	    m.day, m.hour, m.minute, m.second, st.sms_received);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return;
	dprintf(fd, "From: %s\nDate: %04d-%02d-%02d %02d:%02d:%02d (%+d min)\nPart: %d/%d ref %d\n\n%s\n",
	    m.sender, m.year, m.month, m.day, m.hour, m.minute, m.second, m.tz_quarters * 15,
	    m.concat_seq ? m.concat_seq : 1, m.concat_total ? m.concat_total : 1, m.concat_ref, m.text);
	close(fd);
}

static void
urc(void *ctx, const char *line, const char *pdu)
{
	(void)ctx;
	if (pdu && strncmp(line, "+CMT:", 5) == 0) {
		store_sms(pdu);
		return;
	}
	if (!model_line(&st, line, false)) {
		/* Unknown URCs: their name only (e.g. "+ECSQ"), never their arguments. */
		char name[24];
		size_t i = 0;
		while (line[i] && line[i] != ':' && i + 1 < sizeof(name)) {
			name[i] = line[i];
			i++;
		}
		name[i] = 0;
		fprintf(stderr, "a34cell: URC %s\n", name);
	}
}

/* A query whose answer lines go into the model (as query answers). */
static enum at_result
query(struct at_chan *ch, const char *cmd)
{
	struct at_resp r;
	enum at_result res = at_cmd(ch, cmd, 5000, &r);

	for (size_t i = 0; i < r.nlines; i++)
		model_line(&st, r.lines[i], true);
	at_resp_free(&r);
	return res;
}

static void
kernel_state(void)
{
	char buf[512];
	size_t len = sizeof(buf);

	if (sysctlbyname("kern.a34cell", buf, &len, NULL, 0) == 0 && len > 0) {
		buf[len < sizeof(buf) ? len : sizeof(buf) - 1] = 0;
		fprintf(stderr, "a34cell: kernel: %s\n", buf);
	} else {
		fprintf(stderr, "a34cell: kernel: no kern.a34cell (kernel without patch 0939, or a34cell=0)\n");
	}
}

/* The transmitter: off unless the operator wants it and the identity check passes. */
static void
radio_policy(struct at_chan *ch, bool want)
{
	struct at_resp r;

	policy_radio_set_wanted(want);
	policy_radio_close();
	if (want) {
		enum at_result res = at_cmd(ch, "AT+CGSN", 5000, &r);
		bool ok = res == AT_OK &&
		    ident_check_reply((const char *const *)r.lines, r.nlines);
		at_resp_wipe(&r);
		fprintf(stderr, "a34cell: identity check: %s\n", ok ? "passed (the modem reports a well-formed identity of its own)" :
		    "FAILED: the transmitter stays off");
	}
	st.radio_gate = policy_radio_gate();
	query(ch, "AT+CFUN?");
	if (st.radio_gate) {
		if (st.cfun != 1 && at_cmd(ch, "AT+CFUN=1", 30000, NULL) == AT_OK)
			fprintf(stderr, "a34cell: transmitter on (AT+CFUN=1)\n");
	} else if (st.cfun != 4 && st.cfun != 0) {
		/* Whatever the modem came up with: off (flight mode), the safe state. */
		enum at_result res = at_cmd(ch, "AT+CFUN=4", 30000, NULL);
		fprintf(stderr, "a34cell: transmitter off (AT+CFUN=4): %s\n", at_result_name(res));
	} else {
		fprintf(stderr, "a34cell: transmitter stays off (CFUN=%d)%s\n", st.cfun,
		    want ? "" : "; -radio not given");
	}
	query(ch, "AT+CFUN?");
}

static void
log_lines(const char *what, struct at_resp *r)
{
	for (size_t i = 0; i < r->nlines; i++)
		fprintf(stderr, "a34cell: %s: %s\n", what, r->lines[i]);
}

int
main(int argc, char **argv)
{
	const char *port = "/dev/a34at0";
	bool radio = false, once = false;
	int trace = 0;
	struct at_chan ch;
	struct at_resp r;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-port") == 0 && i + 1 < argc)
			port = argv[++i];
		else if (strcmp(argv[i], "-state") == 0 && i + 1 < argc)
			state_path = argv[++i];
		else if (strcmp(argv[i], "-inbox") == 0 && i + 1 < argc)
			inbox_dir = argv[++i];
		else if (strcmp(argv[i], "-radio") == 0)
			radio = true;
		else if (strcmp(argv[i], "-trace") == 0)
			trace = 1;
		else if (strcmp(argv[i], "-once") == 0)
			once = true;
		else {
			fprintf(stderr, "usage: a34cell [-port DEV] [-state PLIST] [-inbox DIR] [-radio] [-trace] [-once]\n");
			return 2;
		}
	}
	setvbuf(stderr, NULL, _IOLBF, 0);
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);
	model_init(&st);
	fprintf(stderr, "a34cell: start (port %s, radio %s)\n", port, radio ? "wanted" : "off");
	kernel_state();

	int fd = open(port, O_RDWR | O_NOCTTY);
	if (fd < 0) {
		fprintf(stderr, "a34cell: no modem port %s (%s): the kernel's CCCI stage is not running "
		    "(needs a34cell=3 on a boot from `boot`, and the modem's HS2); nothing to do\n", port,
		    strerror(errno));
		publish();
		return 0;
	}
	at_init(&ch, fd, urc, NULL);
	ch.trace = trace;

	enum at_result res = AT_TIMEOUT;
	for (int i = 0; i < 10 && !stop; i++) {
		res = at_cmd(&ch, "AT", 1000, NULL);
		if (res == AT_OK)
			break;
	}
	if (res != AT_OK) {
		fprintf(stderr, "a34cell: the modem does not answer AT on %s (%s)\n", port,
		    at_result_name(res));
		publish();
		return 1;
	}
	st.modem_up = true;
	at_cmd(&ch, "ATE0", 2000, NULL);
	at_cmd(&ch, "AT+CMEE=1", 2000, NULL);
	if (at_cmd(&ch, "ATI", 2000, &r) == AT_OK)
		log_lines("ATI", &r);
	at_resp_free(&r);
	if (at_cmd(&ch, "AT+CGMR", 2000, &r) == AT_OK)
		log_lines("firmware", &r);
	at_resp_free(&r);
	res = query(&ch, "AT+CPIN?");
	fprintf(stderr, "a34cell: SIM: %s\n", res == AT_OK ? (st.sim == SIM_READY ? "ready" :
	    st.sim == SIM_PIN ? "PIN required" : st.sim == SIM_PUK ? "PUK required" : "not ready") :
	    "no answer (no SIM?)");

	/* Reports we want as URCs. A modem may not know one (C5GREG on an LTE-only firmware). */
	static const char *const setup[] = {
		"AT+CREG=2", "AT+CGREG=2", "AT+CEREG=2", "AT+C5GREG=2", "AT+COPS=3,0", "AT+CIREG=2",
		"AT+CMGF=0", "AT+CNMI=2,2,0,1,0", "AT+CLIP=1", "AT+CRC=1",
	};
	for (size_t i = 0; i < sizeof(setup) / sizeof(setup[0]); i++) {
		res = at_cmd(&ch, setup[i], 3000, NULL);
		if (res != AT_OK)
			fprintf(stderr, "a34cell: %s: %s\n", setup[i], at_result_name(res));
	}
	radio_policy(&ch, radio);
	publish();

	time_t next = 0;
	while (!stop) {
		if (time(NULL) >= next) {
			query(&ch, "AT+CSQ");
			query(&ch, "AT+CREG?");
			query(&ch, "AT+CEREG?");
			query(&ch, "AT+C5GREG?");
			query(&ch, "AT+COPS?");
			query(&ch, "AT+CIREG?");
			st.ncalls = 0;
			query(&ch, "AT+CLCC");
			publish();
			if (once)
				break;
			next = time(NULL) + 10;
		}
		if (at_poll(&ch, 1000) < 0) {
			fprintf(stderr, "a34cell: modem port closed\n");
			st.modem_up = false;
			publish();
			return 1;
		}
	}
	close(fd);
	return 0;
}
