/*
 * a34cell: SMS in PDU mode (3GPP TS 23.040 TPDUs, TS 23.038 alphabets), as the modem hands them
 * over in +CMT and takes them in AT+CMGS. Text is UTF-8 on our side.
 */
#ifndef A34CELL_SMS_H
#define A34CELL_SMS_H

#include <stddef.h>
#include <stdint.h>

struct sms_deliver {
	char sender[48];        /* "+905...", digits, or an alphanumeric sender name (UTF-8) */
	char smsc[24];
	int year, month, day, hour, minute, second, tz_quarters;  /* service centre time stamp */
	int dcs;                /* data coding scheme byte */
	/* concatenated SMS (UDH IEI 0x00 / 0x08): reference, parts, this part (1-based); 0 if single */
	int concat_ref, concat_total, concat_seq;
	char text[1024];        /* UTF-8 */
	int binary;             /* 8-bit data: text holds nothing */
};

/* pdu_hex: the second line of +CMT (SMSC address first). 0 on success, -1 if malformed. */
int sms_decode_deliver(const char *pdu_hex, struct sms_deliver *out);

/*
 * One SMS-SUBMIT for dest ("+90..." or national digits) with text (UTF-8): GSM 7-bit when every
 * character has a default-alphabet code (up to 160), else UCS-2 (up to 70). out_hex gets the
 * whole PDU with a zero-length SMSC (the SIM's default), *tpdu_len the AT+CMGS length.
 * 0 on success, -1 bad number, -2 text too long for one part, -3 output too small.
 */
int sms_encode_submit(const char *dest, const char *text, char *out_hex, size_t outsz,
    int *tpdu_len);

#endif
