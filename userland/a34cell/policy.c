/*
 * a34cell: the allowlist, the identity denylist and the radio gate (policy.h).
 */
#include "policy.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

#define MAX_CMD 160

/* Arguments are checked by kind; anything else in an argument is refused. */
enum arg {
	ARG_NONE,       /* the command exactly as written */
	ARG_DIGITS,     /* AT+CMGS=<length>: 1..3 decimal digits */
	ARG_DIAL,       /* ATD<number>;  digits, + * #, then ';' (voice) */
	ARG_APN,        /* AT+CGDCONT=<cid>,"IP"|"IPV6"|"IPV4V6","<apn>" */
	ARG_CID,        /* AT+CGACT=<0|1>,<cid> and AT+CGPADDR=<cid>: cid 1..15 */
};

struct rule {
	const char *text;       /* exact command, or the prefix the argument follows */
	enum arg arg;
	bool radio;             /* radio power: also needs the radio gate */
};

/*
 * Everything a34cell says. Queries (?, plain reads) and the settings that only change how the
 * modem reports to us. Nothing here writes identity, calibration, SIM-lock or NVRAM items.
 */
static const struct rule rules[] = {
	{ "AT", ARG_NONE, false },
	{ "ATE0", ARG_NONE, false },
	{ "ATI", ARG_NONE, false },
	{ "AT+CMEE=1", ARG_NONE, false },
	{ "AT+CGMI", ARG_NONE, false },
	{ "AT+CGMM", ARG_NONE, false },
	{ "AT+CGMR", ARG_NONE, false },
	{ "AT+CGSN", ARG_NONE, false },         /* only into ident_check_reply(), see policy.h */
	{ "AT+CPIN?", ARG_NONE, false },
	{ "AT+CFUN?", ARG_NONE, false },
	{ "AT+CREG=2", ARG_NONE, false },
	{ "AT+CREG?", ARG_NONE, false },
	{ "AT+CGREG=2", ARG_NONE, false },
	{ "AT+CGREG?", ARG_NONE, false },
	{ "AT+CEREG=2", ARG_NONE, false },
	{ "AT+CEREG?", ARG_NONE, false },
	{ "AT+C5GREG=2", ARG_NONE, false },
	{ "AT+C5GREG?", ARG_NONE, false },
	{ "AT+COPS=3,0", ARG_NONE, false },     /* report the operator as its long name */
	{ "AT+COPS=3,2", ARG_NONE, false },     /* ... or as MCC+MNC */
	{ "AT+COPS?", ARG_NONE, false },
	{ "AT+CSQ", ARG_NONE, false },
	{ "AT+CIREG=2", ARG_NONE, false },      /* IMS registration reports (+CIREGU) */
	{ "AT+CIREG?", ARG_NONE, false },
	{ "AT+CMGF=0", ARG_NONE, false },       /* SMS in PDU mode */
	{ "AT+CNMI=2,2,0,1,0", ARG_NONE, false },/* new SMS straight to us as +CMT */
	{ "AT+CNMA", ARG_NONE, false },         /* acknowledge a +CMT */
	{ "AT+CMGS=", ARG_DIGITS, false },
	{ "AT+CLCC", ARG_NONE, false },
	{ "AT+CRC=1", ARG_NONE, false },        /* +CRING: VOICE */
	{ "AT+CLIP=1", ARG_NONE, false },       /* caller's number with RING */
	{ "ATA", ARG_NONE, false },
	{ "ATH", ARG_NONE, false },
	{ "AT+CHUP", ARG_NONE, false },
	{ "ATD", ARG_DIAL, false },
	{ "AT+CGDCONT?", ARG_NONE, false },
	{ "AT+CGDCONT=", ARG_APN, false },
	{ "AT+CGACT?", ARG_NONE, false },
	{ "AT+CGACT=", ARG_CID, false },
	{ "AT+CGPADDR=", ARG_CID, false },
	/* Transmitter off (minimum functionality / flight mode): always allowed, it is the safe way. */
	{ "AT+CFUN=0", ARG_NONE, false },
	{ "AT+CFUN=4", ARG_NONE, false },
	/* Transmitter on: only through the radio gate. */
	{ "AT+CFUN=1", ARG_NONE, true },
};

/*
 * Never, whatever the table says: the vendor's identity programming command family. Matched
 * anywhere in the command, case-insensitively, so it cannot be smuggled behind another command
 * on the same line either (';' chains are refused below as well).
 */
static const char *const identity_deny[] = {
	"+EGMR",
};

static bool radio_wanted;
static bool ident_ok;

void
policy_radio_close(void)
{
	ident_ok = false;
}

void
policy_radio_set_wanted(bool wanted)
{
	radio_wanted = wanted;
}

bool
policy_radio_gate(void)
{
	return radio_wanted && ident_ok;
}

static bool
contains_ci(const char *hay, const char *needle)
{
	size_t n = strlen(needle);

	for (; *hay; hay++)
		if (strncasecmp(hay, needle, n) == 0)
			return true;
	return false;
}

static bool
arg_ok(enum arg kind, const char *a)
{
	size_t n = strlen(a);

	switch (kind) {
	case ARG_NONE:
		return n == 0;
	case ARG_DIGITS:
		if (n < 1 || n > 3)
			return false;
		for (size_t i = 0; i < n; i++)
			if (!isdigit((unsigned char)a[i]))
				return false;
		return true;
	case ARG_DIAL:
		/* 1..40 of [0-9+*#], then exactly one ';' at the end (a voice call). */
		if (n < 2 || n > 41 || a[n - 1] != ';')
			return false;
		for (size_t i = 0; i + 1 < n; i++)
			if (!isdigit((unsigned char)a[i]) && a[i] != '+' && a[i] != '*' && a[i] != '#')
				return false;
		return true;
	case ARG_CID: {
		/* "<0|1>,<cid>" for CGACT, "<cid>" for CGPADDR: digits and at most one comma. */
		int commas = 0;
		if (n < 1 || n > 5)
			return false;
		for (size_t i = 0; i < n; i++) {
			if (a[i] == ',')
				commas++;
			else if (!isdigit((unsigned char)a[i]))
				return false;
		}
		return commas <= 1 && a[0] != ',' && a[n - 1] != ',';
	}
	case ARG_APN: {
		/* <cid>,"<type>","<apn>" with an APN of letters, digits, '.', '-', '_'. */
		const char *p = a;
		if (!isdigit((unsigned char)*p))
			return false;
		while (isdigit((unsigned char)*p))
			p++;
		static const char *const types[] = { ",\"IP\",\"", ",\"IPV6\",\"", ",\"IPV4V6\",\"" };
		size_t t;
		for (t = 0; t < sizeof(types) / sizeof(types[0]); t++)
			if (strncmp(p, types[t], strlen(types[t])) == 0)
				break;
		if (t == sizeof(types) / sizeof(types[0]))
			return false;
		p += strlen(types[t]);
		size_t len = 0;
		while (*p && *p != '"') {
			if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-' && *p != '_')
				return false;
			p++;
			len++;
		}
		return len >= 1 && len <= 63 && p[0] == '"' && p[1] == 0;
	}
	}
	return false;
}

enum policy_verdict
policy_check(const char *cmd)
{
	size_t n = cmd ? strlen(cmd) : 0;

	if (n == 0 || n > MAX_CMD)
		return POLICY_DENY_FORMAT;
	for (size_t i = 0; i < n; i++)
		if ((unsigned char)cmd[i] < 0x20 || (unsigned char)cmd[i] > 0x7e)
			return POLICY_DENY_FORMAT;      /* no CR/LF/Ctrl-Z smuggling */
	for (size_t i = 0; i < sizeof(identity_deny) / sizeof(identity_deny[0]); i++)
		if (contains_ci(cmd, identity_deny[i]))
			return POLICY_DENY_IDENTITY;
	if (strncasecmp(cmd, "AT", 2) != 0)
		return POLICY_DENY_FORMAT;
	/* One command per line: ';' only as the voice-call terminator of ATD. */
	const char *semi = strchr(cmd, ';');
	if (semi && !(strncmp(cmd, "ATD", 3) == 0 && semi[1] == 0))
		return POLICY_DENY_FORMAT;

	for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
		const struct rule *r = &rules[i];
		size_t rl = strlen(r->text);
		if (r->arg == ARG_NONE) {
			if (strcmp(cmd, r->text) != 0)
				continue;
		} else if (strncmp(cmd, r->text, rl) != 0 || !arg_ok(r->arg, cmd + rl)) {
			continue;
		}
		if (r->radio && !policy_radio_gate())
			return POLICY_DENY_RADIO;
		return POLICY_ALLOW;
	}
	/* MediaTek's radio command: never from here (AT+CFUN is the one we use). */
	if (strncasecmp(cmd, "AT+EFUN", 7) == 0)
		return POLICY_DENY_RADIO;
	return POLICY_DENY_UNKNOWN;
}

const char *
policy_verdict_name(enum policy_verdict v)
{
	switch (v) {
	case POLICY_ALLOW: return "allowed";
	case POLICY_DENY_UNKNOWN: return "refused (not on the allowlist)";
	case POLICY_DENY_IDENTITY: return "refused (identity programming, never)";
	case POLICY_DENY_RADIO: return "refused (radio gate closed)";
	case POLICY_DENY_FORMAT: return "refused (format)";
	}
	return "refused";
}

bool
ident_well_formed(const char *d)
{
	int sum = 0;
	bool all_same = true;

	if (!d || strlen(d) != 15)
		return false;
	for (int i = 0; i < 15; i++) {
		if (!isdigit((unsigned char)d[i]))
			return false;
		if (d[i] != d[0])
			all_same = false;
	}
	if (all_same)
		return false;
	/* Luhn over the 15 digits, the last one being the check digit. */
	for (int i = 0; i < 15; i++) {
		int v = d[14 - i] - '0';
		if (i & 1) {
			v *= 2;
			if (v > 9)
				v -= 9;
		}
		sum += v;
	}
	return sum % 10 == 0;
}

bool
ident_check_reply(const char *const *lines, size_t nlines)
{
	char digits[16];
	int found = 0;

	for (size_t i = 0; i < nlines; i++) {
		const char *l = lines[i];
		if (!l)
			continue;
		if (strncmp(l, "+CGSN:", 6) == 0)
			l += 6;
		while (*l == ' ' || *l == '"')
			l++;
		size_t k = 0;
		while (isdigit((unsigned char)l[k]) && k < 16)
			k++;
		if (k == 0)
			continue;
		found++;
		if (k != 15 || (l[k] != 0 && l[k] != '"' && l[k] != ' ')) {
			found = 99;     /* a line that is not one 15-digit identity */
			continue;
		}
		memcpy(digits, l, 15);
		digits[15] = 0;
	}
	ident_ok = (found == 1) && ident_well_formed(digits);
	/* The digits are not kept. */
	volatile char *v = digits;
	for (size_t i = 0; i < sizeof(digits); i++)
		v[i] = 0;
	return ident_ok;
}
