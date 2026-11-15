/*
 * a34cell: what we know about the modem, the SIM and the network, from AT answers and URCs
 * (3GPP TS 27.007 / 27.005 forms), and its export to the rest of iOS.
 */
#ifndef A34CELL_MODEL_H
#define A34CELL_MODEL_H

#include <stdbool.h>
#include <stddef.h>

enum sim_state { SIM_UNKNOWN = 0, SIM_ABSENT, SIM_READY, SIM_PIN, SIM_PUK, SIM_OTHER };

/* 27.007 <stat>: 0 not registered, 1 home, 2 searching, 3 denied, 4 unknown, 5 roaming. */
struct reg {
	int stat;               /* -1: never reported */
	unsigned area;          /* LAC or TAC */
	unsigned cell;
	int act;                /* access technology, -1 if absent (7 = E-UTRAN, 11/13 = NR) */
};

#define MAX_CALLS 4

struct call {
	int id, dir, stat, mode;        /* +CLCC: stat 0 active 1 held 2 dialing 3 alerting 4 incoming 5 waiting */
	char number[32];
};

struct cell_state {
	bool modem_up;
	enum sim_state sim;
	int cfun;               /* -1 unknown */
	struct reg creg, cgreg, cereg, c5greg;
	char oper[64];          /* long alphanumeric name */
	char oper_num[8];       /* MCC MNC */
	int rssi;               /* +CSQ 0..31, 99 unknown */
	int ims;                /* +CIREG(U) reg_info: -1 unknown, 0 not registered, 1 registered */
	int ims_ext;            /* +CIREGU ext_info bits: 1 RTP voice, 2 RTP text, 4 SMS over IP, 8 video */
	int ncalls;
	struct call calls[MAX_CALLS];
	bool ringing;
	char clip[32];          /* last +CLIP number */
	unsigned sms_received;
	bool radio_gate;
};

void model_init(struct cell_state *s);

/* One answer line or URC. query: the line answers our own "?" query (27.007 puts <n> first in
 * +CREG?/+CEREG?/+C5GREG?/+CIREG? answers but not in the URCs). Returns true if it was known. */
bool model_line(struct cell_state *s, const char *line, bool query);

/* Registered on the packet (EPS/5GS) or circuit domain, home or roaming. */
bool model_registered(const struct cell_state *s);

/* Signal: dBm from +CSQ (-113 + 2 rssi), bars 0..4 (Android's GSM thresholds). */
int model_dbm(const struct cell_state *s);
int model_bars(const struct cell_state *s);

/* A modem line for the log: every run of 8 or more digits (an IMEI, IMSI, ICCID or phone
 * number would be one) replaced by '#'s of the same length. */
void model_redact(const char *in, char *out, size_t outsz);

/* The state as an XML property list (for SpringBoard's status-bar hook later). Returns the
 * length written, or -1 if it did not fit. */
int model_plist(const struct cell_state *s, char *out, size_t outsz);

#endif
