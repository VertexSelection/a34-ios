/*
 * ext4ro: read-only ext4 (ext4ro.h). Layouts: the ext4 disk format as documented in the Linux
 * kernel's Documentation/filesystems/ext4 (superblock, group descriptors, inodes, extent tree,
 * directory entries); our own code.
 */
#include "ext4ro.h"

#include <string.h>

#define SB_OFF                  1024u
#define EXT4_MAGIC              0xEF53u
#define INCOMPAT_FILETYPE       0x0002u
#define INCOMPAT_META_BG        0x0010u
#define INCOMPAT_EXTENTS        0x0040u
#define INCOMPAT_64BIT          0x0080u
#define INCOMPAT_INLINE_DATA    0x8000u
#define INCOMPAT_ENCRYPT        0x10000u
#define INODE_EXTENTS_FL        0x00080000u
#define INODE_INLINE_FL         0x10000000u
#define EXT_MAGIC               0xF30Au

static uint16_t
g16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t
g32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int
rd(struct ext4ro *fs, uint64_t off, void *buf, uint32_t len)
{
	return fs->read(fs->ctx, off, buf, len) ? EXT4RO_EIO : 0;
}

static int
rd_block(struct ext4ro *fs, uint64_t blk, uint8_t *buf)
{
	if (blk == 0 || blk >= fs->blocks)
		return EXT4RO_EIO;
	return rd(fs, blk * fs->bsize, buf, fs->bsize);
}

int
ext4ro_mount(struct ext4ro *fs, ext4ro_read_fn read, void *ctx, uint8_t *scratch)
{
	uint8_t sb[1024];

	memset(fs, 0, sizeof(*fs));
	fs->read = read;
	fs->ctx = ctx;
	if (rd(fs, SB_OFF, sb, sizeof(sb)))
		return EXT4RO_EIO;
	if (g16(sb + 0x38) != EXT4_MAGIC)
		return EXT4RO_EINVAL;
	uint32_t log = g32(sb + 0x18);
	if (log > 6)
		return EXT4RO_EINVAL;
	fs->bsize = 1024u << log;
	fs->incompat = g32(sb + 0x60);
	if (fs->incompat & (INCOMPAT_META_BG | INCOMPAT_INLINE_DATA | INCOMPAT_ENCRYPT))
		return EXT4RO_ENOTSUP;
	fs->inodes_per_group = g32(sb + 0x28);
	uint32_t blocks_per_group = g32(sb + 0x20);
	fs->inode_size = g32(sb + 0x4C) == 0 ? 128u : g16(sb + 0x58);
	fs->blocks = g32(sb + 0x04);
	if (fs->incompat & INCOMPAT_64BIT)
		fs->blocks |= (uint64_t)g32(sb + 0x150) << 32;
	fs->desc_size = (fs->incompat & INCOMPAT_64BIT) ? g16(sb + 0xFE) : 32u;
	if (fs->desc_size < 32 || fs->desc_size > 1024 || fs->inode_size < 128 ||
	    fs->inode_size > fs->bsize || fs->inodes_per_group == 0 || blocks_per_group == 0)
		return EXT4RO_EINVAL;
	uint32_t first_data = g32(sb + 0x14);
	fs->groups = (uint32_t)((fs->blocks - first_data + blocks_per_group - 1) / blocks_per_group);
	fs->gdt_block = first_data + 1;
	fs->blk = scratch;
	fs->blk2 = scratch + EXT4RO_MAX_BLOCK;
	fs->blk3 = scratch + 2 * EXT4RO_MAX_BLOCK;
	return 0;
}

/* The inode's raw bytes (inode_size, at most 1024) into out. */
static int
read_inode(struct ext4ro *fs, uint32_t ino, uint8_t *out)
{
	if (ino == 0)
		return EXT4RO_EINVAL;
	uint32_t group = (ino - 1) / fs->inodes_per_group, index = (ino - 1) % fs->inodes_per_group;
	if (group >= fs->groups)
		return EXT4RO_EINVAL;
	uint8_t d[64];
	uint64_t doff = fs->gdt_block * fs->bsize + (uint64_t)group * fs->desc_size;
	if (rd(fs, doff, d, fs->desc_size < 64 ? 32 : 64))
		return EXT4RO_EIO;
	uint64_t table = g32(d + 0x08);
	if (fs->desc_size >= 64)
		table |= (uint64_t)g32(d + 0x28) << 32;
	uint32_t n = fs->inode_size > 1024 ? 1024 : fs->inode_size;
	return rd(fs, table * fs->bsize + (uint64_t)index * fs->inode_size, out, n);
}

int
ext4ro_stat(struct ext4ro *fs, uint32_t ino, struct ext4ro_stat *st)
{
	uint8_t in[1024];
	int e = read_inode(fs, ino, in);

	if (e)
		return e;
	st->mode = g16(in + 0x00);
	st->size = g32(in + 0x04) | (uint64_t)g32(in + 0x6C) << 32;
	st->atime = g32(in + 0x08);
	st->ctime = g32(in + 0x0C);
	st->mtime = g32(in + 0x10);
	return 0;
}

/*
 * The physical block of logical block lblk, 0 for a hole (or an uninitialized extent: zeros),
 * or a negative code. Extent trees are walked from the inode's root down to the leaf.
 */
static int64_t
map_block(struct ext4ro *fs, const uint8_t *in, uint64_t lblk)
{
	uint32_t flags = g32(in + 0x20);

	if (flags & INODE_INLINE_FL)
		return EXT4RO_ENOTSUP;
	if (flags & INODE_EXTENTS_FL) {
		const uint8_t *node = in + 0x28;
		uint32_t node_len = 60;
		for (int level = 0; level < 8; level++) {
			if (node_len < 12 || g16(node) != EXT_MAGIC)
				return EXT4RO_EIO;
			uint16_t entries = g16(node + 2), depth = g16(node + 6);
			if (12u + (uint32_t)entries * 12u > node_len)
				return EXT4RO_EIO;
			if (depth == 0) {
				for (uint16_t i = 0; i < entries; i++) {
					const uint8_t *e = node + 12 + 12 * i;
					uint32_t first = g32(e);
					uint32_t len = g16(e + 4);
					bool uninit = len > 32768;
					if (uninit)
						len -= 32768;
					if (lblk >= first && lblk < (uint64_t)first + len) {
						if (uninit)
							return 0;
						uint64_t start = g32(e + 8) | (uint64_t)g16(e + 6) << 32;
						return (int64_t)(start + (lblk - first));
					}
				}
				return 0;       /* a hole */
			}
			/* index: the last entry whose first block <= lblk */
			const uint8_t *pick = NULL;
			for (uint16_t i = 0; i < entries; i++) {
				const uint8_t *e = node + 12 + 12 * i;
				if (g32(e) <= lblk)
					pick = e;
			}
			if (!pick)
				return 0;
			uint64_t child = g32(pick + 4) | (uint64_t)g16(pick + 8) << 32;
			uint8_t *buf = (level & 1) ? fs->blk3 : fs->blk2;
			if (rd_block(fs, child, buf))
				return EXT4RO_EIO;
			node = buf;
			node_len = fs->bsize;
		}
		return EXT4RO_EIO;
	}
	/* classic block map: 12 direct, then single, double, triple indirect */
	uint64_t per = fs->bsize / 4;
	if (lblk < 12)
		return g32(in + 0x28 + 4 * lblk);
	lblk -= 12;
	uint64_t span = per;
	for (int level = 1; level <= 3; level++) {
		if (lblk < span) {
			uint32_t blk = g32(in + 0x28 + 4 * (11 + level));
			for (int l = level; l > 0; l--) {
				if (blk == 0)
					return 0;
				if (rd_block(fs, blk, fs->blk2))
					return EXT4RO_EIO;
				uint64_t div = 1;
				for (int k = 1; k < l; k++)
					div *= per;
				blk = g32(fs->blk2 + 4 * ((lblk / div) % per));
			}
			return blk;
		}
		lblk -= span;
		span *= per;
	}
	return EXT4RO_EINVAL;
}

long
ext4ro_pread(struct ext4ro *fs, uint32_t ino, uint64_t off, void *buf, uint32_t len)
{
	uint8_t in[1024];
	struct ext4ro_stat st;
	int e = read_inode(fs, ino, in);

	if (e)
		return e;
	if ((e = ext4ro_stat(fs, ino, &st)))
		return e;
	if (off >= st.size)
		return 0;
	if (len > st.size - off)
		len = (uint32_t)(st.size - off);
	uint32_t done = 0;
	while (done < len) {
		uint64_t pos = off + done;
		uint64_t lblk = pos / fs->bsize;
		uint32_t skip = (uint32_t)(pos % fs->bsize);
		uint32_t n = fs->bsize - skip;
		if (n > len - done)
			n = len - done;
		int64_t pb = map_block(fs, in, lblk);
		if (pb < 0)
			return (long)pb;
		if (pb == 0) {
			memset((uint8_t *)buf + done, 0, n);
		} else {
			if (rd_block(fs, (uint64_t)pb, fs->blk))
				return EXT4RO_EIO;
			memcpy((uint8_t *)buf + done, fs->blk + skip, n);
		}
		done += n;
	}
	return (long)done;
}

int
ext4ro_readdir(struct ext4ro *fs, uint32_t ino, ext4ro_dir_fn fn, void *ctx)
{
	uint8_t in[1024];
	struct ext4ro_stat st;
	int e = read_inode(fs, ino, in);

	if (e)
		return e;
	if ((e = ext4ro_stat(fs, ino, &st)))
		return e;
	if ((st.mode & 0xF000) != 0x4000)
		return EXT4RO_ENOTDIR;
	for (uint64_t lblk = 0; lblk * fs->bsize < st.size; lblk++) {
		int64_t pb = map_block(fs, in, lblk);
		if (pb < 0)
			return (int)pb;
		if (pb == 0)
			continue;
		if (rd_block(fs, (uint64_t)pb, fs->blk))
			return EXT4RO_EIO;
		for (uint32_t o = 0; o + 8 <= fs->bsize;) {
			const uint8_t *d = fs->blk + o;
			uint32_t dino = g32(d);
			uint16_t rec = g16(d + 4);
			uint32_t nlen = (fs->incompat & INCOMPAT_FILETYPE) ? d[6] : g16(d + 6);
			uint8_t type = (fs->incompat & INCOMPAT_FILETYPE) ? d[7] : 0;
			if (rec < 8 || o + rec > fs->bsize || 8u + nlen > rec)
				break;
			if (dino != 0 && nlen > 0) {
				int r = fn(ctx, (const char *)d + 8, nlen, dino, type);
				if (r)
					return r;
			}
			o += rec;
		}
	}
	return 0;
}

struct find {
	const char *name;
	uint32_t len;
	bool nocase;
	uint32_t ino;
};

static int
eq(const char *a, const char *b, uint32_t n, bool nocase)
{
	for (uint32_t i = 0; i < n; i++) {
		char x = a[i], y = b[i];
		if (nocase) {
			if (x >= 'A' && x <= 'Z')
				x = (char)(x - 'A' + 'a');
			if (y >= 'A' && y <= 'Z')
				y = (char)(y - 'A' + 'a');
		}
		if (x != y)
			return 0;
	}
	return 1;
}

static int
find_fn(void *ctx, const char *name, uint32_t namelen, uint32_t ino, uint8_t type)
{
	struct find *f = ctx;

	(void)type;
	if (namelen == f->len && eq(name, f->name, namelen, f->nocase)) {
		f->ino = ino;
		return 1;
	}
	return 0;
}

int
ext4ro_lookup(struct ext4ro *fs, const char *path, bool nocase, uint32_t *ino)
{
	uint32_t cur = EXT4RO_ROOT_INO;

	if (!path || path[0] != '/')
		return EXT4RO_EINVAL;
	const char *p = path;
	while (*p) {
		while (*p == '/')
			p++;
		if (!*p)
			break;
		const char *end = p;
		while (*end && *end != '/')
			end++;
		struct find f = { p, (uint32_t)(end - p), nocase, 0 };
		if (f.len > 255)
			return EXT4RO_EINVAL;
		int r = ext4ro_readdir(fs, cur, find_fn, &f);
		if (r < 0)
			return r;
		if (r == 0 || f.ino == 0)
			return EXT4RO_ENOENT;
		cur = f.ino;
		p = end;
	}
	*ino = cur;
	return 0;
}
