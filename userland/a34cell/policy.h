/*
 * a34cell: what may be said to the modem.
 *
 * Every AT command goes through policy_check() before a byte reaches the modem. The rule is an
 * allowlist: a command not in the table below is refused. On top of it a short denylist is
 * checked first and can never be overridden, not by the table, not by a debug switch: the
 * vendor's identity programming commands. Turning the transmitter on (AT+CFUN=1) is a third
 * class: allowed only once the radio gate is open (see policy_radio_gate()); turning it off
 * (AT+CFUN=4, AT+CFUN=0) is always allowed, and MediaTek's own radio command (AT+EFUN) never.
 *
 * Why: the phone's IMEI is registered in its country; transmitting with any other identity is
 * illegal there, and the NVRAM that holds it must never change. This daemon never writes it,
 * never asks the modem to, and keeps the transmitter off unless the identity the modem will use
 * is the device's own (the modem read its NVRAM, read-only, from the phone's own partitions).
 */
#ifndef A34CELL_POLICY_H
#define A34CELL_POLICY_H

#include <stdbool.h>
#include <stddef.h>

enum policy_verdict {
	POLICY_ALLOW = 0,
	POLICY_DENY_UNKNOWN,    /* not on the allowlist */
	POLICY_DENY_IDENTITY,   /* identity programming: never */
	POLICY_DENY_RADIO,      /* radio power while the gate is closed */
	POLICY_DENY_FORMAT,     /* bad characters, too long, an argument out of its pattern */
};

/* The radio gate. Closed at start. It opens only when both hold:
 *   - the operator asked for it (a34cell -radio, i.e. the user's own switch), and
 *   - ident_check_reply() accepted the modem's answer to AT+CGSN: the modem reports a
 *     well-formed identity (15 digits, valid check digit, not a blank/default pattern), which is
 *     what it does when it read the device's own NVRAM. The value itself is never kept.
 * Once closed again (policy_radio_close()) it stays closed until both hold again. */
void policy_radio_close(void);
void policy_radio_set_wanted(bool wanted);
bool policy_radio_gate(void);

enum policy_verdict policy_check(const char *cmd);
const char *policy_verdict_name(enum policy_verdict v);

/* Looks at the lines the modem sent for AT+CGSN and says only whether they hold one well-formed
 * identity. The lines are wiped by the caller right after; nothing of them is logged, stored or
 * passed on. Opens the identity half of the radio gate when true, closes it when false. */
bool ident_check_reply(const char *const *lines, size_t nlines);

/* The check digit rule (ISO/IEC 7812 Luhn) over exactly 15 decimal digits, plus the patterns a
 * modem reports when it has no identity (all zeros, all the same digit). Exposed for tests. */
bool ident_well_formed(const char *digits);

#endif
