/*
 * chunksum: print one checksum per fixed-size chunk of a file or block
 * device, fast enough to compare a 16 GiB partition in about a minute.
 *
 *   chunksum PATH CHUNK_BYTES COUNT [FIRST]
 *
 * Prints "index crc32 adler32" in hex for chunks FIRST..FIRST+COUNT-1.
 * Both checksums are zlib's (crc32 with polynomial 0xedb88320, adler32), so
 * the host can compute the same values with Python's zlib module. Together
 * they are 64 bits: enough to tell changed chunks from unchanged ones.
 *
 * Built static for TWRP (tools/chunksum/build.sh).
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static uint32_t crc_table[8][256];

static void
crc_init(void)
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t c = i;
		for (int k = 0; k < 8; k++)
			c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
		crc_table[0][i] = c;
	}
	for (uint32_t i = 0; i < 256; i++)
		for (int t = 1; t < 8; t++)
			crc_table[t][i] = (crc_table[t - 1][i] >> 8) ^ crc_table[0][crc_table[t - 1][i] & 0xff];
}

/* slicing-by-8 */
static uint32_t
crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
	crc = ~crc;
	while (n >= 8) {
		uint32_t a = crc ^ ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
		uint32_t b = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
		crc = crc_table[7][a & 0xff] ^ crc_table[6][(a >> 8) & 0xff] ^
		    crc_table[5][(a >> 16) & 0xff] ^ crc_table[4][a >> 24] ^
		    crc_table[3][b & 0xff] ^ crc_table[2][(b >> 8) & 0xff] ^
		    crc_table[1][(b >> 16) & 0xff] ^ crc_table[0][b >> 24];
		p += 8;
		n -= 8;
	}
	while (n--)
		crc = crc_table[0][(crc ^ *p++) & 0xff] ^ (crc >> 8);
	return ~crc;
}

static uint32_t
adler32_update(uint32_t adler, const uint8_t *p, size_t n)
{
	uint32_t a = adler & 0xffff, b = adler >> 16;
	while (n) {
		size_t k = n < 5552 ? n : 5552;
		n -= k;
		while (k--) {
			a += *p++;
			b += a;
		}
		a %= 65521;
		b %= 65521;
	}
	return (b << 16) | a;
}

int
main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: %s PATH CHUNK_BYTES COUNT [FIRST]\n", argv[0]);
		return 2;
	}
	size_t chunk = strtoull(argv[2], NULL, 0);
	unsigned long long count = strtoull(argv[3], NULL, 0);
	unsigned long long first = argc > 4 ? strtoull(argv[4], NULL, 0) : 0;
	int fd = open(argv[1], O_RDONLY);
	if (fd < 0 || chunk == 0) {
		perror(argv[1]);
		return 1;
	}
	uint8_t *buf = malloc(chunk);
	if (buf == NULL)
		return 1;
	crc_init();
	for (unsigned long long i = first; i < first + count; i++) {
		size_t got = 0;
		while (got < chunk) {
			ssize_t r = pread(fd, buf + got, chunk - got, (off_t)(i * chunk + got));
			if (r <= 0)
				break;
			got += (size_t)r;
		}
		if (got == 0)
			break;
		printf("%llu %08x %08x\n", i, crc32_update(0, buf, got), adler32_update(1, buf, got));
	}
	return 0;
}
