/*
 * a34cell: SMS PDUs (sms.h). 3GPP TS 23.040 (TPDU layout) and TS 23.038 (the GSM 7-bit default
 * alphabet and its extension table; the Turkish national language tables of Annex A, used when
 * an SMS says so in its user data header).
 */
#include "sms.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* ---- alphabets (Unicode code points; 0x1B is the escape to the shift table) ---- */

static const uint16_t gsm_default[128] = {
	'@', 0xA3, '$', 0xA5, 0xE8, 0xE9, 0xF9, 0xEC, 0xF2, 0xC7, '\n', 0xD8, 0xF8, '\r', 0xC5, 0xE5,
	0x394, '_', 0x3A6, 0x393, 0x39B, 0x3A9, 0x3A0, 0x3A8, 0x3A3, 0x398, 0x39E, 0x1B, 0xC6, 0xE6, 0xDF, 0xC9,
	' ', '!', '"', '#', 0xA4, '%', '&', '\'', '(', ')', '*', '+', ',', '-', '.', '/',
	'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', ';', '<', '=', '>', '?',
	0xA1, 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',
	'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 0xC4, 0xD6, 0xD1, 0xDC, 0xA7,
	0xBF, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o',
	'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', 0xE4, 0xF6, 0xF1, 0xFC, 0xE0,
};

/* Turkish locking shift table (TS 23.038 A.3.1): the default table with these differences. */
static const struct { uint8_t code; uint16_t ch; } turkish_locking[] = {
	{ 0x04, 0x20AC }, { 0x07, 0x131 }, { 0x0B, 0x11E }, { 0x0C, 0x11F }, { 0x1C, 0x15E },
	{ 0x1D, 0x15F }, { 0x40, 0x130 }, { 0x60, 0xE7 },
};

struct ext { uint8_t code; uint16_t ch; };

/* Default extension table (escape 0x1B, then the code). */
static const struct ext ext_default[] = {
	{ 0x0A, 0x0C }, { 0x14, '^' }, { 0x28, '{' }, { 0x29, '}' }, { 0x2F, '\\' }, { 0x3C, '[' },
	{ 0x3D, '~' }, { 0x3E, ']' }, { 0x40, '|' }, { 0x65, 0x20AC },
};

/* Turkish single shift table (TS 23.038 A.2.1). */
static const struct ext ext_turkish[] = {
	{ 0x0A, 0x0C }, { 0x14, '^' }, { 0x28, '{' }, { 0x29, '}' }, { 0x2F, '\\' }, { 0x3C, '[' },
	{ 0x3D, '~' }, { 0x3E, ']' }, { 0x40, '|' }, { 0x47, 0x11E }, { 0x49, 0x130 }, { 0x53, 0x15E },
	{ 0x63, 0xE7 }, { 0x65, 0x20AC }, { 0x67, 0x11F }, { 0x69, 0x131 }, { 0x73, 0x15F },
};

struct tables {
	uint16_t base[128];
	const struct ext *ext;
	size_t next;
};

static void
tables_init(struct tables *t, int locking_lang, int single_lang)
{
	memcpy(t->base, gsm_default, sizeof(t->base));
	if (locking_lang == 1)
		for (size_t i = 0; i < sizeof(turkish_locking) / sizeof(turkish_locking[0]); i++)
			t->base[turkish_locking[i].code] = turkish_locking[i].ch;
	if (single_lang == 1) {
		t->ext = ext_turkish;
		t->next = sizeof(ext_turkish) / sizeof(ext_turkish[0]);
	} else {
		t->ext = ext_default;
		t->next = sizeof(ext_default) / sizeof(ext_default[0]);
	}
}

/* ---- small helpers ---- */

static int
hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

static int
unhex(const char *hex, uint8_t *out, size_t max)
{
	size_t n = strlen(hex);

	if (n % 2 || n / 2 > max)
		return -1;
	for (size_t i = 0; i < n / 2; i++) {
		int a = hexval(hex[2 * i]), b = hexval(hex[2 * i + 1]);
		if (a < 0 || b < 0)
			return -1;
		out[i] = (uint8_t)(a << 4 | b);
	}
	return (int)(n / 2);
}

static size_t
put_utf8(char *out, size_t pos, size_t max, uint32_t cp)
{
	char b[4];
	size_t n;

	if (cp < 0x80) {
		b[0] = (char)cp;
		n = 1;
	} else if (cp < 0x800) {
		b[0] = (char)(0xC0 | cp >> 6);
		b[1] = (char)(0x80 | (cp & 0x3F));
		n = 2;
	} else if (cp < 0x10000) {
		b[0] = (char)(0xE0 | cp >> 12);
		b[1] = (char)(0x80 | (cp >> 6 & 0x3F));
		b[2] = (char)(0x80 | (cp & 0x3F));
		n = 3;
	} else {
		b[0] = (char)(0xF0 | cp >> 18);
		b[1] = (char)(0x80 | (cp >> 12 & 0x3F));
		b[2] = (char)(0x80 | (cp >> 6 & 0x3F));
		b[3] = (char)(0x80 | (cp & 0x3F));
		n = 4;
	}
	if (pos + n + 1 > max)
		return pos;
	memcpy(out + pos, b, n);
	out[pos + n] = 0;
	return pos + n;
}

/* Next code point of a UTF-8 string; 0xFFFD for a bad sequence. Advances *s. */
static uint32_t
get_utf8(const char **s)
{
	const unsigned char *p = (const unsigned char *)*s;
	uint32_t cp;
	int n;

	if (p[0] < 0x80) {
		*s += 1;
		return p[0];
	}
	if ((p[0] & 0xE0) == 0xC0) {
		cp = p[0] & 0x1F;
		n = 1;
	} else if ((p[0] & 0xF0) == 0xE0) {
		cp = p[0] & 0x0F;
		n = 2;
	} else if ((p[0] & 0xF8) == 0xF0) {
		cp = p[0] & 0x07;
		n = 3;
	} else {
		*s += 1;
		return 0xFFFD;
	}
	for (int i = 1; i <= n; i++) {
		if ((p[i] & 0xC0) != 0x80) {
			*s += i;
			return 0xFFFD;
		}
		cp = cp << 6 | (p[i] & 0x3F);
	}
	*s += n + 1;
	return cp;
}

/* Septet i of a packed 7-bit stream that starts at bit offset 0 of data. */
static uint8_t
septet_at(const uint8_t *data, size_t len, size_t i)
{
	size_t bit = i * 7, byte = bit / 8, shift = bit % 8;
	unsigned v = byte < len ? data[byte] >> shift : 0;

	if (shift > 1 && byte + 1 < len)
		v |= (unsigned)data[byte + 1] << (8 - shift);
	return (uint8_t)(v & 0x7F);
}

/* Unpacks septets [skip, skip+count) as text through the tables, into UTF-8. */
static void
gsm7_to_utf8(const uint8_t *data, size_t len, size_t skip, size_t count, const struct tables *t,
    char *out, size_t outsz)
{
	size_t pos = 0;

	out[0] = 0;
	for (size_t i = skip; i < skip + count; i++) {
		uint8_t c = septet_at(data, len, i);
		if (c == 0x1B && i + 1 < skip + count) {
			uint8_t e = septet_at(data, len, ++i);
			uint16_t ch = ' ';
			for (size_t k = 0; k < t->next; k++)
				if (t->ext[k].code == e)
					ch = t->ext[k].ch;
			pos = put_utf8(out, pos, outsz, ch);
			continue;
		}
		pos = put_utf8(out, pos, outsz, t->base[c]);
	}
}

/* Semi-octet digits of an address; 'F' filler dropped. */
static void
bcd_digits(const uint8_t *p, int ndigits, char *out, size_t outsz)
{
	static const char sym[] = "0123456789*#abc";
	size_t o = 0;

	for (int i = 0; i < ndigits && o + 1 < outsz; i++) {
		int d = (i & 1) ? p[i / 2] >> 4 : p[i / 2] & 0x0F;
		if (d == 0x0F)
			break;
		out[o++] = sym[d];
	}
	out[o] = 0;
}

static int
swapped_bcd(uint8_t b)
{
	int lo = b & 0x0F, hi = b >> 4;
	return (lo > 9 ? 0 : lo) * 10 + (hi > 9 ? 0 : hi);
}

/* ---- SMS-DELIVER ---- */

int
sms_decode_deliver(const char *pdu_hex, struct sms_deliver *m)
{
	uint8_t b[200];
	int n = unhex(pdu_hex, b, sizeof(b));
	size_t i = 0;

	memset(m, 0, sizeof(*m));
	if (n < 1)
		return -1;
	/* SMSC: length in octets (type + digits) */
	int sl = b[i++];
	if (sl > 0) {
		if (i + (size_t)sl > (size_t)n || sl > 11)
			return -1;
		size_t o = 0;
		if (b[i] == 0x91)
			m->smsc[o++] = '+';
		bcd_digits(b + i + 1, (sl - 1) * 2, m->smsc + o, sizeof(m->smsc) - o);
		i += (size_t)sl;
	}
	if (i >= (size_t)n)
		return -1;
	uint8_t fo = b[i++];
	if ((fo & 0x03) != 0x00)
		return -1;      /* not an SMS-DELIVER */
	bool udhi = fo & 0x40;
	/* originating address: number of digits (semi-octets), type, digits */
	if (i + 2 > (size_t)n)
		return -1;
	int al = b[i++];
	uint8_t at = b[i++];
	size_t aoct = (size_t)(al + 1) / 2;
	if (i + aoct > (size_t)n || aoct > 12)
		return -1;
	if ((at & 0x70) == 0x50) {
		/* alphanumeric (TON 5): GSM 7-bit packed, al semi-octets */
		struct tables t;
		tables_init(&t, 0, 0);
		gsm7_to_utf8(b + i, aoct, 0, (size_t)al * 4 / 7, &t, m->sender, sizeof(m->sender));
	} else {
		size_t o = 0;
		if ((at & 0x70) == 0x10)
			m->sender[o++] = '+';
		bcd_digits(b + i, al, m->sender + o, sizeof(m->sender) - o);
	}
	i += aoct;
	if (i + 2 + 7 + 1 > (size_t)n)
		return -1;
	i++;                    /* PID */
	m->dcs = b[i++];
	m->year = 2000 + swapped_bcd(b[i]);
	m->month = swapped_bcd(b[i + 1]);
	m->day = swapped_bcd(b[i + 2]);
	m->hour = swapped_bcd(b[i + 3]);
	m->minute = swapped_bcd(b[i + 4]);
	m->second = swapped_bcd(b[i + 5]);
	{
		uint8_t tz = b[i + 6];
		int q = swapped_bcd(tz & 0xF7);
		m->tz_quarters = (tz & 0x08) ? -q : q;
	}
	i += 7;
	int udl = b[i++];
	const uint8_t *ud = b + i;
	size_t udlen = (size_t)n - i;

	/* Alphabet: general data coding (bits 7..6 = 00) or the 0xF0 group. */
	int alphabet;   /* 0 GSM 7-bit, 1 8-bit, 2 UCS-2 */
	if ((m->dcs & 0xC0) == 0x00)
		alphabet = (m->dcs >> 2) & 0x03;
	else if ((m->dcs & 0xF0) == 0xF0)
		alphabet = (m->dcs & 0x04) ? 1 : 0;
	else if ((m->dcs & 0xF0) == 0xE0)
		alphabet = 2;
	else
		alphabet = 0;
	if (alphabet == 3)
		alphabet = 0;

	size_t udh_len = 0;     /* octets, including the UDHL octet */
	int lock_lang = 0, single_lang = 0;
	if (udhi) {
		if (udlen < 1 || ud[0] + 1u > udlen)
			return -1;
		udh_len = (size_t)ud[0] + 1;
		for (size_t k = 1; k + 1 < udh_len;) {
			uint8_t iei = ud[k], iel = ud[k + 1];
			const uint8_t *v = ud + k + 2;
			if (k + 2 + iel > udh_len)
				break;
			if (iei == 0x00 && iel == 3) {
				m->concat_ref = v[0];
				m->concat_total = v[1];
				m->concat_seq = v[2];
			} else if (iei == 0x08 && iel == 4) {
				m->concat_ref = v[0] << 8 | v[1];
				m->concat_total = v[2];
				m->concat_seq = v[3];
			} else if (iei == 0x24 && iel == 1) {
				single_lang = v[0];
			} else if (iei == 0x25 && iel == 1) {
				lock_lang = v[0];
			}
			k += 2 + iel;
		}
	}

	if (alphabet == 0) {
		struct tables t;
		size_t skip = udh_len ? (udh_len * 8 + 6) / 7 : 0;
		if ((size_t)udl < skip || ((size_t)udl * 7 + 7) / 8 > udlen)
			return -1;
		tables_init(&t, lock_lang, single_lang);
		gsm7_to_utf8(ud, udlen, skip, (size_t)udl - skip, &t, m->text, sizeof(m->text));
	} else if (alphabet == 2) {
		size_t pos = 0;
		if ((size_t)udl > udlen || (size_t)udl < udh_len)
			return -1;
		for (size_t k = udh_len; k + 1 < (size_t)udl; k += 2) {
			uint32_t cp = (uint32_t)ud[k] << 8 | ud[k + 1];
			if (cp >= 0xD800 && cp < 0xDC00 && k + 3 < (size_t)udl) {
				uint32_t lo = (uint32_t)ud[k + 2] << 8 | ud[k + 3];
				if (lo >= 0xDC00 && lo < 0xE000) {
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
					k += 2;
				}
			}
			pos = put_utf8(m->text, pos, sizeof(m->text), cp);
		}
	} else {
		m->binary = 1;
	}
	return 0;
}

/* ---- SMS-SUBMIT ---- */

/* The default-alphabet septets for a code point: 1 (base), 2 (escape + extension), 0 (none). */
static int
gsm7_code(uint32_t cp, uint8_t out[2])
{
	for (int c = 0; c < 128; c++) {
		if (c != 0x1B && gsm_default[c] == cp) {
			out[0] = (uint8_t)c;
			return 1;
		}
	}
	for (size_t k = 0; k < sizeof(ext_default) / sizeof(ext_default[0]); k++) {
		if (ext_default[k].ch == cp) {
			out[0] = 0x1B;
			out[1] = ext_default[k].code;
			return 2;
		}
	}
	return 0;
}

static size_t
put_hex(char *out, size_t pos, size_t max, uint8_t v)
{
	static const char hx[] = "0123456789ABCDEF";

	if (pos + 3 > max)
		return max + 1;
	out[pos] = hx[v >> 4];
	out[pos + 1] = hx[v & 0x0F];
	out[pos + 2] = 0;
	return pos + 2;
}

int
sms_encode_submit(const char *dest, const char *text, char *out, size_t outsz, int *tpdu_len)
{
	uint8_t tp[180];
	size_t n = 0;
	char digits[24];
	size_t nd = 0;
	bool intl = false;

	const char *d = dest;
	if (*d == '+') {
		intl = true;
		d++;
	}
	for (; *d; d++) {
		if (!isdigit((unsigned char)*d) || nd >= 20)
			return -1;
		digits[nd++] = *d;
	}
	if (nd == 0)
		return -1;

	tp[n++] = 0x11;         /* SMS-SUBMIT, relative validity period */
	tp[n++] = 0x00;         /* message reference: the modem sets it */
	tp[n++] = (uint8_t)nd;
	tp[n++] = intl ? 0x91 : 0x81;
	for (size_t k = 0; k < nd; k += 2) {
		uint8_t lo = (uint8_t)(digits[k] - '0');
		uint8_t hi = k + 1 < nd ? (uint8_t)(digits[k + 1] - '0') : 0x0F;
		tp[n++] = (uint8_t)(hi << 4 | lo);
	}
	tp[n++] = 0x00;         /* PID */

	/* GSM 7-bit if every character has a code, else UCS-2. */
	uint8_t sept[200];
	size_t ns = 0;
	bool gsm = true;
	for (const char *s = text; *s;) {
		uint8_t c[2];
		int k = gsm7_code(get_utf8(&s), c);
		if (k == 0) {
			gsm = false;
			break;
		}
		if (ns + (size_t)k > 160)
			return -2;      /* (and longer still as UCS-2) */
		memcpy(sept + ns, c, (size_t)k);
		ns += (size_t)k;
	}
	if (gsm) {
		tp[n++] = 0x00;         /* DCS: GSM 7-bit */
		tp[n++] = 0xAA;         /* VP: 4 days */
		tp[n++] = (uint8_t)ns;
		size_t bytes = (ns * 7 + 7) / 8;
		memset(tp + n, 0, bytes);
		for (size_t k = 0; k < ns; k++) {
			size_t bit = k * 7, byte = bit / 8, shift = bit % 8;
			tp[n + byte] |= (uint8_t)(sept[k] << shift);
			if (shift > 1)
				tp[n + byte + 1] |= (uint8_t)(sept[k] >> (8 - shift));
		}
		n += bytes;
	} else {
		uint8_t u[140];
		size_t nu = 0;
		for (const char *s = text; *s;) {
			uint32_t cp = get_utf8(&s);
			if (cp >= 0x10000) {
				uint32_t v = cp - 0x10000;
				uint16_t hi = (uint16_t)(0xD800 + (v >> 10)), lo = (uint16_t)(0xDC00 + (v & 0x3FF));
				if (nu + 4 > sizeof(u))
					return -2;
				u[nu++] = (uint8_t)(hi >> 8);
				u[nu++] = (uint8_t)hi;
				u[nu++] = (uint8_t)(lo >> 8);
				u[nu++] = (uint8_t)lo;
			} else {
				if (nu + 2 > sizeof(u))
					return -2;
				u[nu++] = (uint8_t)(cp >> 8);
				u[nu++] = (uint8_t)cp;
			}
		}
		tp[n++] = 0x08;         /* DCS: UCS-2 */
		tp[n++] = 0xAA;
		tp[n++] = (uint8_t)nu;
		memcpy(tp + n, u, nu);
		n += nu;
	}

	size_t pos = 0;
	pos = put_hex(out, pos, outsz, 0x00);   /* SMSC: the SIM's */
	for (size_t k = 0; k < n && pos <= outsz; k++)
		pos = put_hex(out, pos, outsz, tp[k]);
	if (pos > outsz)
		return -3;
	*tpdu_len = (int)n;
	return 0;
}
