/*
 * ext4ro: a read-only ext4 reader with no write path at all, for the modem's file server
 * (docs/cellular-plan.md, stage 1c). Plain C without OS dependencies: the same two files are
 * built into the kernel (bsd/dev/a34_ext4ro.c, xnu/patches/0951) and tested on the host here
 * (tests/test_ext4ro.sh builds images with mke2fs and compares every file).
 *
 * Reads superblock, group descriptors, inodes (extents or block maps), directories (linear,
 * htree interior blocks are skipped as the empty entries they look like). No journal replay:
 * the data read is what the last completed checkpoint left on the disk. Refuses features it
 * does not understand (meta_bg, inline data, encryption).
 */
#ifndef A34_EXT4RO_H
#define A34_EXT4RO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EXT4RO_MAX_BLOCK        65536u
#define EXT4RO_ROOT_INO         2u

/* Reads len bytes at byte offset off of the file system into buf. 0 on success. */
typedef int (*ext4ro_read_fn)(void *ctx, uint64_t off, void *buf, uint32_t len);

struct ext4ro {
	ext4ro_read_fn read;
	void *ctx;
	uint32_t bsize, inode_size, inodes_per_group, groups, desc_size;
	uint64_t blocks, gdt_block;
	uint32_t incompat;
	uint8_t *blk;           /* scratch: one block, given by the caller (bsize bytes) */
	uint8_t *blk2;          /* scratch for index/indirect walks (bsize bytes) */
	uint8_t *blk3;
};

struct ext4ro_stat {
	uint16_t mode;          /* S_IFDIR 0x4000, S_IFREG 0x8000 in the top bits */
	uint64_t size;
	uint32_t mtime, atime, ctime;
};

/* Reads and checks the superblock. scratch: 3 * EXT4RO_MAX_BLOCK bytes. 0 or a negative code. */
int ext4ro_mount(struct ext4ro *fs, ext4ro_read_fn read, void *ctx, uint8_t *scratch);

int ext4ro_stat(struct ext4ro *fs, uint32_t ino, struct ext4ro_stat *st);

/* Bytes read (0 at or past the end), or a negative code. */
long ext4ro_pread(struct ext4ro *fs, uint32_t ino, uint64_t off, void *buf, uint32_t len);

/* Calls fn for every entry of directory ino ("." and ".." included); fn returning non-zero stops
 * the walk and that value is returned. */
typedef int (*ext4ro_dir_fn)(void *ctx, const char *name, uint32_t namelen, uint32_t ino, uint8_t type);
int ext4ro_readdir(struct ext4ro *fs, uint32_t ino, ext4ro_dir_fn fn, void *ctx);

/* Resolves an absolute path ("/md/NVRAM/X", '/' separators). nocase: ASCII case-insensitive
 * match of each component. 0 and *ino, or a negative code (-2 not found). */
int ext4ro_lookup(struct ext4ro *fs, const char *path, bool nocase, uint32_t *ino);

#define EXT4RO_EIO      (-5)
#define EXT4RO_ENOENT   (-2)
#define EXT4RO_EINVAL   (-22)
#define EXT4RO_ENOTSUP  (-45)
#define EXT4RO_ENOTDIR  (-20)

#endif
