/*
 * a34cell: the modem/SIM/network model (model.h).
 */
#include "model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void
model_init(struct cell_state *s)
{
	memset(s, 0, sizeof(*s));
	s->cfun = -1;
	s->creg.stat = s->cgreg.stat = s->cereg.stat = s->c5greg.stat = -1;
	s->creg.act = s->cgreg.act = s->cereg.act = s->c5greg.act = -1;
	s->rssi = 99;
	s->ims = -1;
}

/* Splits "a,"b,c",d" into fields (quotes removed, commas inside quotes kept). */
static int
fields(const char *p, char f[][64], int max)
{
	int n = 0;

	while (*p == ' ')
		p++;
	if (!*p)
		return 0;
	for (;;) {
		size_t o = 0;
		bool q = false;
		if (n == max)
			return n;
		while (*p && (q || *p != ',')) {
			if (*p == '"')
				q = !q;
			else if (o + 1 < 64)
				f[n][o++] = *p;
			p++;
		}
		f[n][o] = 0;
		n++;
		if (*p != ',')
			return n;
		p++;
	}
}

/* Copies src into dst (size n), cut at n - 1 bytes. */
static void
cpy(char *dst, size_t n, const char *src)
{
	size_t l = strlen(src);

	if (l >= n)
		l = n - 1;
	memcpy(dst, src, l);
	dst[l] = 0;
}

static int
num(const char *s, int dflt)
{
	char *e;
	long v;

	if (!s || !*s)
		return dflt;
	v = strtol(s, &e, 10);
	return *e ? dflt : (int)v;
}

static unsigned
hexnum(const char *s)
{
	return s && *s ? (unsigned)strtoul(s, NULL, 16) : 0;
}

/* +CREG/+CGREG/+CEREG/+C5GREG: [<n>,]<stat>[,<lac|tac>,<ci>[,<AcT>...]] */
static void
parse_reg(struct reg *r, const char *p, bool query)
{
	char f[8][64];
	int n = fields(p, f, 8);
	int k = 0;

	if (n == 0)
		return;
	/* The query answer starts with <n>. A URC with a location starts with a bare <stat>
	 * followed by a quoted area; a query answer of only "n,stat" has two numbers. */
	if (query)
		k = 1;
	if (k >= n)
		return;
	r->stat = num(f[k], -1);
	if (n > k + 2) {
		r->area = hexnum(f[k + 1]);
		r->cell = hexnum(f[k + 2]);
		r->act = n > k + 3 ? num(f[k + 3], -1) : -1;
	} else {
		r->area = r->cell = 0;
		r->act = -1;
	}
}

bool
model_line(struct cell_state *s, const char *line, bool query)
{
	char f[8][64];
	int n;

	if (strncmp(line, "+CPIN:", 6) == 0) {
		const char *v = line + 6;
		while (*v == ' ')
			v++;
		if (strcmp(v, "READY") == 0)
			s->sim = SIM_READY;
		else if (strcmp(v, "SIM PIN") == 0)
			s->sim = SIM_PIN;
		else if (strcmp(v, "SIM PUK") == 0)
			s->sim = SIM_PUK;
		else if (strcmp(v, "NOT INSERTED") == 0 || strcmp(v, "NOT READY") == 0)
			s->sim = SIM_ABSENT;
		else
			s->sim = SIM_OTHER;
		return true;
	}
	if (strncmp(line, "+CFUN:", 6) == 0) {
		s->cfun = num(line + 6 + (line[6] == ' '), -1);
		return true;
	}
	if (strncmp(line, "+CREG:", 6) == 0) {
		parse_reg(&s->creg, line + 6, query);
		return true;
	}
	if (strncmp(line, "+CGREG:", 7) == 0) {
		parse_reg(&s->cgreg, line + 7, query);
		return true;
	}
	if (strncmp(line, "+CEREG:", 7) == 0) {
		parse_reg(&s->cereg, line + 7, query);
		return true;
	}
	if (strncmp(line, "+C5GREG:", 8) == 0) {
		parse_reg(&s->c5greg, line + 8, query);
		return true;
	}
	if (strncmp(line, "+COPS:", 6) == 0) {
		/* <mode>[,<format>,<oper>[,<AcT>]] */
		n = fields(line + 6, f, 8);
		if (n >= 3) {
			if (num(f[1], -1) == 2)
				cpy(s->oper_num, sizeof(s->oper_num), f[2]);
			else
				cpy(s->oper, sizeof(s->oper), f[2]);
		} else {
			s->oper[0] = 0;
		}
		return true;
	}
	if (strncmp(line, "+CSQ:", 5) == 0) {
		n = fields(line + 5, f, 8);
		if (n >= 1)
			s->rssi = num(f[0], 99);
		return true;
	}
	if (strncmp(line, "+CIREGU:", 8) == 0 || strncmp(line, "+CIREG:", 7) == 0) {
		bool urc = line[6] == 'U';
		n = fields(line + (urc ? 8 : 7), f, 8);
		int k = (!urc && query) ? 1 : 0;
		if (n > k) {
			s->ims = num(f[k], -1);
			s->ims_ext = n > k + 1 ? num(f[k + 1], 0) : 0;
		}
		return true;
	}
	if (strncmp(line, "+CLCC:", 6) == 0) {
		/* <id>,<dir>,<stat>,<mode>,<mpty>[,<number>,<type>...] -- collected per query */
		n = fields(line + 6, f, 8);
		if (n >= 5 && s->ncalls < MAX_CALLS) {
			struct call *c = &s->calls[s->ncalls++];
			c->id = num(f[0], 0);
			c->dir = num(f[1], 0);
			c->stat = num(f[2], 0);
			c->mode = num(f[3], 0);
			cpy(c->number, sizeof(c->number), n >= 6 ? f[5] : "");
		}
		return true;
	}
	if (strcmp(line, "RING") == 0 || strncmp(line, "+CRING:", 7) == 0) {
		s->ringing = true;
		return true;
	}
	if (strncmp(line, "+CLIP:", 6) == 0) {
		n = fields(line + 6, f, 8);
		if (n >= 1)
			cpy(s->clip, sizeof(s->clip), f[0]);
		return true;
	}
	if (strcmp(line, "NO CARRIER") == 0) {
		s->ringing = false;
		return true;
	}
	return false;
}

static bool
reg_ok(const struct reg *r)
{
	return r->stat == 1 || r->stat == 5;
}

bool
model_registered(const struct cell_state *s)
{
	return reg_ok(&s->c5greg) || reg_ok(&s->cereg) || reg_ok(&s->cgreg) || reg_ok(&s->creg);
}

int
model_dbm(const struct cell_state *s)
{
	if (s->rssi < 0 || s->rssi > 31)
		return 0;
	return -113 + 2 * s->rssi;
}

int
model_bars(const struct cell_state *s)
{
	int r = s->rssi;

	if (r <= 2 || r == 99)
		return 0;
	if (r >= 12)
		return 4;
	if (r >= 8)
		return 3;
	if (r >= 5)
		return 2;
	return 1;
}

static int
put(char *out, size_t outsz, size_t *pos, const char *fmt, const char *key, const char *sval,
    long ival)
{
	int w;

	if (sval) {
		/* XML-escape the string */
		char esc[192];
		size_t o = 0;
		for (const char *p = sval; *p && o + 7 < sizeof(esc); p++) {
			const char *e = NULL;
			if (*p == '&')
				e = "&amp;";
			else if (*p == '<')
				e = "&lt;";
			else if (*p == '>')
				e = "&gt;";
			if (e) {
				memcpy(esc + o, e, strlen(e));
				o += strlen(e);
			} else {
				esc[o++] = *p;
			}
		}
		esc[o] = 0;
		w = snprintf(out + *pos, outsz - *pos, fmt, key, esc);
	} else {
		w = snprintf(out + *pos, outsz - *pos, fmt, key, ival);
	}
	if (w < 0 || (size_t)w >= outsz - *pos)
		return -1;
	*pos += (size_t)w;
	return 0;
}

int
model_plist(const struct cell_state *s, char *out, size_t outsz)
{
	static const char *const sims[] = { "unknown", "absent", "ready", "pin", "puk", "other" };
	const char *S = "\t<key>%s</key><string>%s</string>\n";
	const char *I = "\t<key>%s</key><integer>%ld</integer>\n";
	const char *B = "\t<key>%s</key><%s/>\n";
	size_t pos = 0;
	int w = snprintf(out, outsz, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
	    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\">\n<dict>\n");

	if (w < 0 || (size_t)w >= outsz)
		return -1;
	pos = (size_t)w;
	if (put(out, outsz, &pos, B, "ModemUp", s->modem_up ? "true" : "false", 0) ||
	    put(out, outsz, &pos, S, "SIM", sims[s->sim], 0) ||
	    put(out, outsz, &pos, I, "CFUN", NULL, s->cfun) ||
	    put(out, outsz, &pos, B, "RadioGate", s->radio_gate ? "true" : "false", 0) ||
	    put(out, outsz, &pos, B, "Registered", model_registered(s) ? "true" : "false", 0) ||
	    put(out, outsz, &pos, I, "CREG", NULL, s->creg.stat) ||
	    put(out, outsz, &pos, I, "CEREG", NULL, s->cereg.stat) ||
	    put(out, outsz, &pos, I, "C5GREG", NULL, s->c5greg.stat) ||
	    put(out, outsz, &pos, I, "AcT", NULL, s->c5greg.act >= 0 ? s->c5greg.act :
	        s->cereg.act >= 0 ? s->cereg.act : s->creg.act) ||
	    put(out, outsz, &pos, S, "Operator", s->oper, 0) ||
	    put(out, outsz, &pos, S, "OperatorNumeric", s->oper_num, 0) ||
	    put(out, outsz, &pos, I, "SignalDBm", NULL, model_dbm(s)) ||
	    put(out, outsz, &pos, I, "SignalBars", NULL, model_bars(s)) ||
	    put(out, outsz, &pos, I, "IMSRegistered", NULL, s->ims) ||
	    put(out, outsz, &pos, I, "IMSFeatures", NULL, s->ims_ext) ||
	    put(out, outsz, &pos, I, "Calls", NULL, s->ncalls) ||
	    put(out, outsz, &pos, B, "Ringing", s->ringing ? "true" : "false", 0) ||
	    put(out, outsz, &pos, I, "SMSReceived", NULL, (long)s->sms_received))
		return -1;
	w = snprintf(out + pos, outsz - pos, "</dict>\n</plist>\n");
	if (w < 0 || (size_t)w >= outsz - pos)
		return -1;
	return (int)(pos + (size_t)w);
}

void
model_redact(const char *in, char *out, size_t outsz)
{
	size_t n = strlen(in), o = 0;

	if (outsz == 0)
		return;
	for (size_t i = 0; i < n && o + 1 < outsz;) {
		size_t run = 0;
		while (i + run < n && in[i + run] >= '0' && in[i + run] <= '9')
			run++;
		if (run >= 8) {
			for (size_t k = 0; k < run && o + 1 < outsz; k++)
				out[o++] = '#';
			i += run;
		} else if (run > 0) {
			for (size_t k = 0; k < run && o + 1 < outsz; k++)
				out[o++] = in[i + k];
			i += run;
		} else {
			out[o++] = in[i++];
		}
	}
	out[o] = 0;
}
