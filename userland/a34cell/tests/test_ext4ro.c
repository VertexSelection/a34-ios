/*
 * ext4ro host test: every regular file under SRC_DIR must read back identically from IMAGE
 * (made from SRC_DIR with mke2fs -d), whole and at odd offsets; a few lookups must fail.
 *   test_ext4ro IMAGE SRC_DIR [nocase]
 */
#include "../ext4ro.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int img;
static int checked, failed;

static int
img_read(void *ctx, uint64_t off, void *buf, uint32_t len)
{
	(void)ctx;
	return pread(img, buf, len, (off_t)off) == (ssize_t)len ? 0 : -1;
}

static void
check_file(struct ext4ro *fs, const char *src, const char *rel)
{
	FILE *f = fopen(src, "rb");
	struct stat sst;
	if (!f || stat(src, &sst)) {
		failed++;
		return;
	}
	size_t n = (size_t)sst.st_size;
	unsigned char *want = malloc(n + 1), *got = malloc(n + 1);
	if (fread(want, 1, n, f) != n)
		failed++;
	fclose(f);
	uint32_t ino;
	struct ext4ro_stat st;
	if (ext4ro_lookup(fs, rel, false, &ino) || ext4ro_stat(fs, ino, &st) || st.size != n) {
		fprintf(stderr, "FAIL lookup/stat %s\n", rel);
		failed++;
	} else {
		long r = ext4ro_pread(fs, ino, 0, got, (uint32_t)n + 1);
		if (r != (long)n || memcmp(want, got, n)) {
			fprintf(stderr, "FAIL read %s (%ld of %zu)\n", rel, r, n);
			failed++;
		}
		for (size_t off = 1; off < n; off = off * 3 + 7) {
			size_t len = n - off < 5000 ? n - off : 5000;
			r = ext4ro_pread(fs, ino, off, got, (uint32_t)len);
			if (r != (long)len || memcmp(want + off, got, len)) {
				fprintf(stderr, "FAIL read %s at %zu\n", rel, off);
				failed++;
				break;
			}
		}
	}
	checked++;
	free(want);
	free(got);
}

static void
walk(struct ext4ro *fs, const char *dir, const char *rel)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	if (!d)
		return;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char src[4096], r[4096];
		snprintf(src, sizeof(src), "%s/%s", dir, e->d_name);
		snprintf(r, sizeof(r), "%s/%s", rel, e->d_name);
		struct stat st;
		if (lstat(src, &st))
			continue;
		if (S_ISDIR(st.st_mode))
			walk(fs, src, r);
		else if (S_ISREG(st.st_mode))
			check_file(fs, src, r);
	}
	closedir(d);
}

int
main(int argc, char **argv)
{
	static uint8_t scratch[3 * EXT4RO_MAX_BLOCK];
	struct ext4ro fs;
	uint32_t ino;

	if (argc < 3)
		return 2;
	img = open(argv[1], O_RDONLY);
	if (img < 0 || ext4ro_mount(&fs, img_read, NULL, scratch)) {
		fprintf(stderr, "FAIL mount %s\n", argv[1]);
		return 1;
	}
	walk(&fs, argv[2], "");
	if (ext4ro_lookup(&fs, "/no/such/file", false, &ino) != EXT4RO_ENOENT)
		failed++;
	if (ext4ro_lookup(&fs, "/md/NVRAM/nvd_test/ld00_001", true, &ino) != 0)
		failed++;       /* the case-insensitive lookup of a mixed-case test path */
	if (ext4ro_lookup(&fs, "/md/NVRAM/nvd_test/ld00_001", false, &ino) != EXT4RO_ENOENT)
		failed++;
	printf("ext4ro %s: block %u, %d files checked, %d failures\n", argv[1], fs.bsize, checked, failed);
	return failed ? 1 : 0;
}
