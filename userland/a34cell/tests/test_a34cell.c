/*
 * a34cell host tests: the policy, the identity check, SMS PDUs, the model's parsers and the AT
 * channel against a scripted fake modem on a socket pair. Built and run on the build machine
 * (macOS in CI, Linux works too): userland/a34cell/build.sh.
 */
#include "../at.h"
#include "../model.h"
#include "../policy.h"
#include "../sms.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int failures, checks;

#define CHECK(c) do { checks++; if (!(c)) { failures++; \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_STR(a, b) do { checks++; if (strcmp((a), (b)) != 0) { failures++; \
	fprintf(stderr, "FAIL %s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); } } while (0)

/* A 15-digit number with a valid check digit, built here (no real identity in the tests). */
static void
make_ident(const char *body14, char out[16])
{
	int sum = 0;

	memcpy(out, body14, 14);
	for (int i = 0; i < 14; i++) {
		int v = body14[13 - i] - '0';
		if ((i & 1) == 0) {
			v *= 2;
			if (v > 9)
				v -= 9;
		}
		sum += v;
	}
	out[14] = (char)('0' + (10 - sum % 10) % 10);
	out[15] = 0;
}

static void
test_policy(void)
{
	CHECK(policy_check("AT") == POLICY_ALLOW);
	CHECK(policy_check("AT+CPIN?") == POLICY_ALLOW);
	CHECK(policy_check("AT+CMGS=23") == POLICY_ALLOW);
	CHECK(policy_check("AT+CMGS=2x") == POLICY_DENY_UNKNOWN);
	CHECK(policy_check("ATD+905551112233;") == POLICY_ALLOW);
	CHECK(policy_check("ATD5551112233") == POLICY_DENY_UNKNOWN);       /* data call form */
	CHECK(policy_check("ATD555;AT+CFUN=1") == POLICY_DENY_FORMAT);
	CHECK(policy_check("AT+CGDCONT=1,\"IPV4V6\",\"internet\"") == POLICY_ALLOW);
	CHECK(policy_check("AT+CGDCONT=1,\"IP\",\"inter net\"") == POLICY_DENY_UNKNOWN);
	CHECK(policy_check("AT+CGACT=1,1") == POLICY_ALLOW);
	CHECK(policy_check("AT+CPIN=\"0000\"") == POLICY_DENY_UNKNOWN);
	CHECK(policy_check("AT+CPIN?\rAT") == POLICY_DENY_FORMAT);
	CHECK(policy_check("") == POLICY_DENY_FORMAT);
	CHECK(policy_check("+CSQ") == POLICY_DENY_FORMAT);
	/* identity programming: never, in any spelling or position */
	CHECK(policy_check("AT+EGMR=0,7") == POLICY_DENY_IDENTITY);
	CHECK(policy_check("at+egmr?") == POLICY_DENY_IDENTITY);
	CHECK(policy_check("ATI+EGMR") == POLICY_DENY_IDENTITY);
	CHECK(policy_check("AT+EFUN=1") == POLICY_DENY_RADIO);
	/* radio: off always, on only through the gate */
	CHECK(policy_check("AT+CFUN=4") == POLICY_ALLOW);
	CHECK(policy_check("AT+CFUN=0") == POLICY_ALLOW);
	policy_radio_set_wanted(false);
	policy_radio_close();
	CHECK(policy_check("AT+CFUN=1") == POLICY_DENY_RADIO);

	char id[16];
	make_ident("12345678901234", id);
	CHECK(ident_well_formed(id));
	char bad[16];
	memcpy(bad, id, 16);
	bad[3] = bad[3] == '9' ? '0' : (char)(bad[3] + 1);
	CHECK(!ident_well_formed(bad));
	CHECK(!ident_well_formed("000000000000000"));
	CHECK(!ident_well_formed("1234"));

	const char *ok_lines[] = { id };
	policy_radio_set_wanted(true);
	CHECK(ident_check_reply(ok_lines, 1));
	CHECK(policy_radio_gate());
	CHECK(policy_check("AT+CFUN=1") == POLICY_ALLOW);
	const char *two[] = { id, id };
	CHECK(!ident_check_reply(two, 2));      /* two identities: not one well-formed answer */
	CHECK(!policy_radio_gate());
	const char *zero[] = { "000000000000000" };
	CHECK(!ident_check_reply(zero, 1));
	char prefixed[32];
	snprintf(prefixed, sizeof(prefixed), "+CGSN: \"%s\"", id);
	const char *pl[] = { prefixed };
	CHECK(ident_check_reply(pl, 1));
	policy_radio_set_wanted(false);
	CHECK(!policy_radio_gate());
	policy_radio_close();
}

static void
test_sms(void)
{
	struct sms_deliver m;

	/* TS 23.040 SMS-DELIVER, GSM 7-bit (the widely published example) */
	CHECK(sms_decode_deliver("07911326040000F0040B911346610089F60000208062917314080CC8F71D14969741F977FD07", &m) == 0);
	CHECK_STR(m.smsc, "+31624000000");
	CHECK_STR(m.sender, "+31641600986");
	CHECK_STR(m.text, "How are you?");
	CHECK(m.year == 2002 && m.month == 8 && m.day == 26 && m.hour == 19 && m.minute == 37 && m.second == 41);

	/* SMS-SUBMIT, GSM 7-bit, to an international number */
	char hex[400];
	int len = 0;
	CHECK(sms_encode_submit("+46708251358", "hellohello", hex, sizeof(hex), &len) == 0);
	CHECK_STR(hex, "0011000B916407281553F80000AA0AE8329BFD4697D9EC37");
	CHECK(len == 23);

	/* extension table characters take two septets */
	CHECK(sms_encode_submit("5551234", "[x]", hex, sizeof(hex), &len) == 0);
	CHECK(strstr(hex, "0000AA05") != NULL);         /* DCS 0, VP, UDL = 5 septets */

	/* UCS-2 when a character has no GSM code (Turkish dotless i), and back */
	const char *tr = "\xC3\x87" "a\xC4\x9F" "r\xC4\xB1 geldi";     /* "Çağrı geldi" */
	CHECK(sms_encode_submit("+905551112233", tr, hex, sizeof(hex), &len) == 0);
	/* turn the SUBMIT into a DELIVER with the same UD to decode it */
	const char *ud = strstr(hex, "0008AA");
	CHECK(ud != NULL);
	if (ud) {
		char deliver[400];
		snprintf(deliver, sizeof(deliver), "00040C910955151122330008%s%s", "52109151000000", ud + 6);
		CHECK(sms_decode_deliver(deliver, &m) == 0);
		CHECK_STR(m.sender, "+905551112233");
		CHECK_STR(m.text, tr);
		CHECK(m.dcs == 8);
	}

	/* alphanumeric sender "Info" (TON 5, 4 septets = 7 semi-octets), concatenated part 2/3
	 * (UDH IEI 00, 6 octets -> 7 septets with one fill bit), GSM 7-bit "Hi" */
	CHECK(sms_decode_deliver("0044" "07D049B7F90D" "0000" "52109151000000" "09" "050003AA0302" "9069", &m) == 0);
	CHECK_STR(m.sender, "Info");
	CHECK_STR(m.text, "Hi");
	CHECK(m.concat_ref == 0xAA && m.concat_total == 3 && m.concat_seq == 2);
	CHECK(sms_encode_submit("abc", "x", hex, sizeof(hex), &len) == -1);
	char longtxt[200];
	memset(longtxt, 'a', 161);
	longtxt[161] = 0;
	CHECK(sms_encode_submit("123", longtxt, hex, sizeof(hex), &len) == -2);
}

static void
test_model(void)
{
	struct cell_state s;

	model_init(&s);
	CHECK(model_line(&s, "+CPIN: READY", true) && s.sim == SIM_READY);
	CHECK(model_line(&s, "+CEREG: 2,1,\"2F1A\",\"01A2B3C4\",7", true));
	CHECK(s.cereg.stat == 1 && s.cereg.area == 0x2F1A && s.cereg.cell == 0x01A2B3C4 && s.cereg.act == 7);
	CHECK(model_line(&s, "+CEREG: 5,\"2F1B\",\"01A2B3C5\",7", false));
	CHECK(s.cereg.stat == 5 && s.cereg.area == 0x2F1B);
	CHECK(model_line(&s, "+CREG: 0,2", true) && s.creg.stat == 2);
	CHECK(model_line(&s, "+CREG: 3", false) && s.creg.stat == 3);
	CHECK(model_registered(&s));
	CHECK(model_line(&s, "+COPS: 0,0,\"Turkcell\",7", true));
	CHECK_STR(s.oper, "Turkcell");
	CHECK(model_line(&s, "+CSQ: 20,99", true));
	CHECK(model_dbm(&s) == -73 && model_bars(&s) == 4);
	CHECK(model_line(&s, "+CSQ: 6,99", true) && model_bars(&s) == 2);
	CHECK(model_line(&s, "+CIREGU: 1,5", false) && s.ims == 1 && s.ims_ext == 5);
	CHECK(model_line(&s, "+CIREG: 2,0", true) && s.ims == 0);
	CHECK(model_line(&s, "+CLCC: 1,1,4,0,0,\"+905551112233\",145", true));
	CHECK(s.ncalls == 1 && s.calls[0].stat == 4);
	CHECK_STR(s.calls[0].number, "+905551112233");
	CHECK(model_line(&s, "RING", false) && s.ringing);
	CHECK(!model_line(&s, "+ECSQ: 1,2,3", false));
	char pl[4096];
	CHECK(model_plist(&s, pl, sizeof(pl)) > 0);
	CHECK(strstr(pl, "<key>Operator</key><string>Turkcell</string>") != NULL);
	CHECK(model_plist(&s, pl, 64) == -1);
}

/* ---- the AT channel against a fake modem ---- */

struct fake {
	int fd;
};

static const char *
reply_for(const char *cmd)
{
	if (strcmp(cmd, "AT") == 0)
		return "\r\nOK\r\n";
	if (strcmp(cmd, "AT+CPIN?") == 0)
		/* a URC in the middle of the answer, then the answer */
		return "\r\n+CEREG: 1,\"2F1A\",\"01A2B3C4\",7\r\n\r\n+CPIN: READY\r\n\r\nOK\r\n";
	if (strcmp(cmd, "AT+CREG?") == 0)
		return "\r\n+CREG: 2,1,\"1A2B\",\"0000C0DE\",7\r\n\r\nOK\r\n";
	if (strcmp(cmd, "AT+CSQ") == 0)
		return "\r\n+CME ERROR: 100\r\n";
	if (strcmp(cmd, "ATI") == 0)
		return "\r\nMTK\r\nMT6877\r\n\r\nOK\r\n";
	if (strncmp(cmd, "AT+CMGS=", 8) == 0)
		return "\r\n> ";
	if (strcmp(cmd, "PDU") == 0)
		return "\r\n+CMGS: 7\r\n\r\nOK\r\n";
	if (strcmp(cmd, "AT+CLCC") == 0)
		return "";      /* never answers: timeout */
	return "\r\nERROR\r\n";
}

static void *
fake_modem(void *arg)
{
	struct fake *f = arg;
	char buf[1024], line[512];
	size_t len = 0;

	for (;;) {
		ssize_t n = read(f->fd, buf + len, sizeof(buf) - 1 - len);
		if (n <= 0)
			return NULL;
		len += (size_t)n;
		for (;;) {
			size_t i = 0;
			while (i < len && buf[i] != '\r' && buf[i] != 0x1a)
				i++;
			if (i == len)
				break;
			memcpy(line, buf, i);
			line[i] = 0;
			char term = buf[i];
			memmove(buf, buf + i + 1, len - i - 1);
			len -= i + 1;
			const char *rep = reply_for(term == 0x1a ? "PDU" : line);
			if (term == '\r' && strcmp(line, "AT") == 0) {
				/* an unsolicited SMS right after the first AT: two-line +CMT */
				if (write(f->fd, rep, strlen(rep)) < 0)
					return NULL;
				rep = "\r\n+CMT: ,24\r\n07911326040000F0040B911346610089F60000208062917314080CC8F71D14969741F977FD07\r\n";
			}
			if (write(f->fd, rep, strlen(rep)) < 0)
				return NULL;
		}
	}
}

static int urc_count, cmt_count;
static char last_cmt_text[64];

static void
test_urc(void *ctx, const char *line, const char *pdu)
{
	struct cell_state *s = ctx;

	urc_count++;
	if (pdu && strncmp(line, "+CMT:", 5) == 0) {
		struct sms_deliver m;
		if (sms_decode_deliver(pdu, &m) == 0) {
			cmt_count++;
			strncpy(last_cmt_text, m.text, sizeof(last_cmt_text) - 1);
		}
		return;
	}
	model_line(s, line, false);
}

static void
test_at(void)
{
	int sv[2];
	pthread_t th;
	struct fake f;
	struct at_chan ch;
	struct at_resp r;
	struct cell_state s;

	CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
	f.fd = sv[1];
	pthread_create(&th, NULL, fake_modem, &f);
	model_init(&s);
	at_init(&ch, sv[0], test_urc, &s);

	CHECK(at_cmd(&ch, "AT", 2000, NULL) == AT_OK);
	CHECK(at_poll(&ch, 300) == 0);
	CHECK(cmt_count == 1);
	CHECK_STR(last_cmt_text, "How are you?");

	CHECK(at_cmd(&ch, "AT+CPIN?", 2000, &r) == AT_OK);
	CHECK(r.nlines == 1);
	if (r.nlines == 1)
		CHECK_STR(r.lines[0], "+CPIN: READY");
	at_resp_free(&r);
	CHECK(s.cereg.stat == 1 && s.cereg.area == 0x2F1A);    /* the URC went to the model */

	CHECK(at_cmd(&ch, "AT+CREG?", 2000, &r) == AT_OK && r.nlines == 1);
	for (size_t i = 0; i < r.nlines; i++)
		model_line(&s, r.lines[i], true);
	at_resp_free(&r);
	CHECK(s.creg.stat == 1 && s.creg.cell == 0xC0DE);

	CHECK(at_cmd(&ch, "AT+CSQ", 2000, &r) == AT_CME_ERROR && r.code == 100);
	at_resp_free(&r);
	CHECK(at_cmd(&ch, "ATI", 2000, &r) == AT_OK && r.nlines == 2);
	at_resp_free(&r);
	CHECK(at_cmd(&ch, "AT+EGMR=0,7", 2000, NULL) == AT_REFUSED);
	CHECK(at_cmd(&ch, "AT+CLCC", 300, NULL) == AT_TIMEOUT);

	char hex[400];
	int len;
	CHECK(sms_encode_submit("+46708251358", "hellohello", hex, sizeof(hex), &len) == 0);
	CHECK(at_send_pdu(&ch, len, hex, 2000, &r) == AT_OK);
	CHECK(r.nlines == 1);
	if (r.nlines == 1)
		CHECK_STR(r.lines[0], "+CMGS: 7");
	at_resp_free(&r);
	CHECK(at_send_pdu(&ch, len, "00ZZ", 2000, NULL) == AT_REFUSED);

	close(sv[0]);
	pthread_join(th, NULL);
	close(sv[1]);
}

int
main(void)
{
	test_policy();
	test_sms();
	test_model();
	test_at();
	printf("a34cell tests: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
