/*
 * a34cell: one AT command channel to the modem (a CCCI tty, or a socket in the tests).
 *
 * Commands are sent one at a time; while one waits for its final result, every line that is
 * not part of its answer is handed to the unsolicited-result callback, as are all lines read
 * while no command is running. Two-line URCs (+CMT, +CDS: a header line, then the PDU) are
 * joined into one callback. Every command passes policy_check() first (policy.h).
 */
#ifndef A34CELL_AT_H
#define A34CELL_AT_H

#include <stdbool.h>
#include <stddef.h>

#define AT_MAX_LINES    32
#define AT_MAX_LINE     1024

enum at_result {
	AT_OK = 0,
	AT_ERROR,               /* ERROR */
	AT_CME_ERROR,           /* +CME ERROR: n (code in at_resp.code) */
	AT_CMS_ERROR,           /* +CMS ERROR: n */
	AT_NO_CARRIER,
	AT_BUSY,
	AT_NO_ANSWER,
	AT_CONNECT,
	AT_TIMEOUT,
	AT_IO,                  /* read/write failed, channel closed */
	AT_REFUSED,             /* policy_check() said no: nothing was sent */
};

struct at_resp {
	enum at_result result;
	int code;
	size_t nlines;
	char *lines[AT_MAX_LINES];      /* intermediate lines, without CR/LF */
};

/* line: the URC line; pdu: the second line of +CMT/+CDS, else NULL. */
typedef void (*at_urc_fn)(void *ctx, const char *line, const char *pdu);

struct at_chan {
	int fd;
	char buf[8192];
	size_t len;
	at_urc_fn urc;
	void *ctx;
	int trace;              /* 1: log commands and result codes, never payloads */
	char pending_hdr[AT_MAX_LINE];  /* +CMT header waiting for its PDU line */
};

void at_init(struct at_chan *ch, int fd, at_urc_fn urc, void *ctx);
const char *at_result_name(enum at_result r);

/* Sends cmd (policy-checked), waits up to timeout_ms for the final result. resp may be NULL. */
enum at_result at_cmd(struct at_chan *ch, const char *cmd, int timeout_ms, struct at_resp *resp);

/* AT+CMGS=<tpdu_len>, then the PDU (hex, SMSC octet included) after the "> " prompt, Ctrl-Z. */
enum at_result at_send_pdu(struct at_chan *ch, int tpdu_len, const char *pdu_hex, int timeout_ms,
    struct at_resp *resp);

/* Reads for up to timeout_ms with no command running and dispatches URCs. <0 on I/O error. */
int at_poll(struct at_chan *ch, int timeout_ms);

void at_resp_free(struct at_resp *resp);

/* Overwrites the response lines with zeros, then frees them (for identity replies). */
void at_resp_wipe(struct at_resp *resp);

#endif
