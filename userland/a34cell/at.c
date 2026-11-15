/*
 * a34cell: the AT command channel (at.h).
 */
#include "at.h"
#include "policy.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static long long
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void
at_init(struct at_chan *ch, int fd, at_urc_fn urc, void *ctx)
{
	memset(ch, 0, sizeof(*ch));
	ch->fd = fd;
	ch->urc = urc;
	ch->ctx = ctx;
}

const char *
at_result_name(enum at_result r)
{
	static const char *const names[] = {
		"OK", "ERROR", "+CME ERROR", "+CMS ERROR", "NO CARRIER", "BUSY", "NO ANSWER",
		"CONNECT", "timeout", "I/O error", "refused by policy",
	};
	return (unsigned)r < sizeof(names) / sizeof(names[0]) ? names[r] : "?";
}

static int
write_all(int fd, const char *p, size_t n)
{
	while (n) {
		ssize_t w = write(fd, p, n);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

/* Reads what is there within timeout_ms into the buffer. 1: got bytes, 0: timeout, -1: error. */
static int
fill(struct at_chan *ch, int timeout_ms)
{
	struct pollfd pfd = { .fd = ch->fd, .events = POLLIN };

	if (ch->len >= sizeof(ch->buf) - 1)
		ch->len = 0;    /* a line longer than the buffer: drop it */
	int r = poll(&pfd, 1, timeout_ms < 0 ? 0 : timeout_ms);
	if (r < 0)
		return errno == EINTR ? 0 : -1;
	if (r == 0)
		return 0;
	ssize_t n = read(ch->fd, ch->buf + ch->len, sizeof(ch->buf) - 1 - ch->len);
	if (n <= 0)
		return n == 0 ? -1 : (errno == EINTR || errno == EAGAIN ? 0 : -1);
	ch->len += (size_t)n;
	return 1;
}

/* Takes one complete line out of the buffer (empty lines are skipped). */
static bool
take_line(struct at_chan *ch, char *out, size_t outsz)
{
	for (;;) {
		size_t i = 0;
		while (i < ch->len && ch->buf[i] != '\r' && ch->buf[i] != '\n')
			i++;
		if (i == ch->len)
			return false;
		size_t n = i < outsz - 1 ? i : outsz - 1;
		memcpy(out, ch->buf, n);
		out[n] = 0;
		size_t skip = i + 1;
		if (skip < ch->len && ch->buf[i] == '\r' && ch->buf[skip] == '\n')
			skip++;
		memmove(ch->buf, ch->buf + skip, ch->len - skip);
		ch->len -= skip;
		if (n > 0)
			return true;
	}
}

static bool
has_prompt(struct at_chan *ch)
{
	/* "> " without a line end, possibly after a CR LF. */
	size_t i = 0;
	while (i < ch->len && (ch->buf[i] == '\r' || ch->buf[i] == '\n'))
		i++;
	if (i < ch->len && ch->buf[i] == '>') {
		size_t k = i + 1;
		if (k < ch->len && ch->buf[k] == ' ')
			k++;
		memmove(ch->buf, ch->buf + k, ch->len - k);
		ch->len -= k;
		return true;
	}
	return false;
}

static bool
is_two_line_urc(const char *line)
{
	return strncmp(line, "+CMT:", 5) == 0 || strncmp(line, "+CDS:", 5) == 0;
}

static void
dispatch_urc(struct at_chan *ch, const char *line)
{
	if (ch->pending_hdr[0]) {
		char hdr[AT_MAX_LINE];
		memcpy(hdr, ch->pending_hdr, sizeof(hdr));
		ch->pending_hdr[0] = 0;
		if (ch->urc)
			ch->urc(ch->ctx, hdr, line);
		return;
	}
	if (is_two_line_urc(line)) {
		snprintf(ch->pending_hdr, sizeof(ch->pending_hdr), "%s", line);
		return;
	}
	if (ch->urc)
		ch->urc(ch->ctx, line, NULL);
}

/* The final result codes of 27.007 / V.250. */
static bool
final_result(const char *line, struct at_resp *r)
{
	static const struct { const char *s; enum at_result res; } fin[] = {
		{ "OK", AT_OK }, { "ERROR", AT_ERROR }, { "NO CARRIER", AT_NO_CARRIER },
		{ "BUSY", AT_BUSY }, { "NO ANSWER", AT_NO_ANSWER }, { "NO DIALTONE", AT_NO_CARRIER },
	};
	for (size_t i = 0; i < sizeof(fin) / sizeof(fin[0]); i++) {
		if (strcmp(line, fin[i].s) == 0) {
			r->result = fin[i].res;
			return true;
		}
	}
	if (strncmp(line, "CONNECT", 7) == 0) {
		r->result = AT_CONNECT;
		return true;
	}
	if (strncmp(line, "+CME ERROR:", 11) == 0) {
		r->result = AT_CME_ERROR;
		r->code = atoi(line + 11);
		return true;
	}
	if (strncmp(line, "+CMS ERROR:", 11) == 0) {
		r->result = AT_CMS_ERROR;
		r->code = atoi(line + 11);
		return true;
	}
	return false;
}

/*
 * The prefix of the answer lines of a command: "AT+CREG?" -> "+CREG:", "AT+CGSN" -> "" (bare
 * lines). Lines with this prefix (or bare lines when the prefix is empty) are the command's;
 * other "+XXX:" lines and RING are URCs.
 */
static void
answer_prefix(const char *cmd, char *out, size_t outsz)
{
	out[0] = 0;
	if (strncmp(cmd, "AT+", 3) != 0 && strncmp(cmd, "AT^", 3) != 0)
		return;
	size_t i = 2, o = 0;
	while (cmd[i] && cmd[i] != '=' && cmd[i] != '?' && o + 2 < outsz)
		out[o++] = cmd[i++];
	out[o++] = ':';
	out[o] = 0;
}

static bool
is_urc_line(const char *line, const char *prefix)
{
	if (strcmp(line, "RING") == 0 || strncmp(line, "+CRING:", 7) == 0)
		return true;
	if (prefix[0] && strncmp(line, prefix, strlen(prefix)) == 0)
		return false;
	return line[0] == '+' || line[0] == '^';
}

static enum at_result
wait_final(struct at_chan *ch, const char *prefix, long long deadline, struct at_resp *r,
    bool want_prompt)
{
	char line[AT_MAX_LINE];

	for (;;) {
		for (;;) {
			if (want_prompt && has_prompt(ch))
				return AT_CONNECT;      /* the "> " of AT+CMGS */
			if (!take_line(ch, line, sizeof(line)))
				break;
			if (ch->pending_hdr[0]) {
				dispatch_urc(ch, line);
				continue;
			}
			if (final_result(line, r))
				return r->result;
			if (is_urc_line(line, prefix)) {
				dispatch_urc(ch, line);
				continue;
			}
			if (r->nlines < AT_MAX_LINES)
				r->lines[r->nlines++] = strdup(line);
		}
		long long left = deadline - now_ms();
		if (left <= 0)
			return r->result = AT_TIMEOUT;
		int f = fill(ch, (int)left);
		if (f < 0)
			return r->result = AT_IO;
	}
}

enum at_result
at_cmd(struct at_chan *ch, const char *cmd, int timeout_ms, struct at_resp *resp)
{
	struct at_resp local;
	struct at_resp *r = resp ? resp : &local;
	char prefix[32];

	memset(r, 0, sizeof(*r));
	enum policy_verdict v = policy_check(cmd);
	if (v != POLICY_ALLOW) {
		fprintf(stderr, "a34cell: %.12s... %s\n", cmd, policy_verdict_name(v));
		return r->result = AT_REFUSED;
	}
	if (ch->trace)
		fprintf(stderr, "a34cell: > %s\n", strncmp(cmd, "ATD", 3) == 0 ? "ATD<number>" : cmd);
	if (write_all(ch->fd, cmd, strlen(cmd)) < 0 || write_all(ch->fd, "\r", 1) < 0)
		return r->result = AT_IO;
	answer_prefix(cmd, prefix, sizeof(prefix));
	enum at_result res = wait_final(ch, prefix, now_ms() + timeout_ms, r, false);
	if (ch->trace) {
		if (res == AT_CME_ERROR || res == AT_CMS_ERROR)
			fprintf(stderr, "a34cell: < %s %d\n", at_result_name(res), r->code);
		else
			fprintf(stderr, "a34cell: < %s (%zu lines)\n", at_result_name(res), r->nlines);
	}
	if (!resp)
		at_resp_free(&local);
	return res;
}

enum at_result
at_send_pdu(struct at_chan *ch, int tpdu_len, const char *pdu_hex, int timeout_ms,
    struct at_resp *resp)
{
	char cmd[32];
	struct at_resp local;
	struct at_resp *r = resp ? resp : &local;

	memset(r, 0, sizeof(*r));
	snprintf(cmd, sizeof(cmd), "AT+CMGS=%d", tpdu_len);
	for (const char *p = pdu_hex; *p; p++) {
		if (!((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F') || (*p >= 'a' && *p <= 'f')))
			return r->result = AT_REFUSED;
	}
	if (policy_check(cmd) != POLICY_ALLOW)
		return r->result = AT_REFUSED;
	if (write_all(ch->fd, cmd, strlen(cmd)) < 0 || write_all(ch->fd, "\r", 1) < 0)
		return r->result = AT_IO;
	long long deadline = now_ms() + timeout_ms;
	enum at_result res = wait_final(ch, "+CMGS:", deadline, r, true);
	if (res != AT_CONNECT) {
		if (!resp)
			at_resp_free(&local);
		return res;
	}
	if (write_all(ch->fd, pdu_hex, strlen(pdu_hex)) < 0 || write_all(ch->fd, "\x1a", 1) < 0)
		return r->result = AT_IO;
	r->result = AT_OK;
	res = wait_final(ch, "+CMGS:", deadline, r, false);
	if (ch->trace)
		fprintf(stderr, "a34cell: SMS submit: %s\n", at_result_name(res));
	if (!resp)
		at_resp_free(&local);
	return res;
}

int
at_poll(struct at_chan *ch, int timeout_ms)
{
	char line[AT_MAX_LINE];
	int f = fill(ch, timeout_ms);

	if (f < 0)
		return -1;
	while (take_line(ch, line, sizeof(line)))
		dispatch_urc(ch, line);
	return 0;
}

void
at_resp_free(struct at_resp *r)
{
	for (size_t i = 0; i < r->nlines; i++)
		free(r->lines[i]);
	r->nlines = 0;
}

void
at_resp_wipe(struct at_resp *r)
{
	for (size_t i = 0; i < r->nlines; i++) {
		volatile char *p = r->lines[i];
		for (size_t k = 0; p[k]; k++)
			p[k] = 0;
		free(r->lines[i]);
	}
	r->nlines = 0;
}
