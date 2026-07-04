// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "../cryexts_fs.h"

static int read_full(int fd, void *buf, size_t len, off_t off)
{
	char *p = buf;

	while (len > 0) {
		ssize_t n = pread(fd, p, len, off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0) {
			errno = EIO;
			return -1;
		}
		p += n;
		off += n;
		len -= n;
	}
	return 0;
}

static int write_full(int fd, const void *buf, size_t len, off_t off)
{
	const char *p = buf;

	while (len > 0) {
		ssize_t n = pwrite(fd, p, len, off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (n == 0) {
			errno = EIO;
			return -1;
		}
		p += n;
		off += n;
		len -= n;
	}
	return 0;
}

static uint64_t inodes_per_block(void)
{
	return CRYEXTS_BLOCK_SIZE / sizeof(struct cryexts_inode);
}

static int read_gdt(int fd, const struct cryexts_super_block *sb,
		    unsigned char *gdt_block,
		    struct cryexts_group_desc **groups_out)
{
	if (!(le32toh(sb->features_incompat) & CRYEXTS_FEATURE_INCOMPAT_BLOCK_GROUPS)) {
		*groups_out = NULL;
		return 0;
	}
	if (read_full(fd, gdt_block, CRYEXTS_BLOCK_SIZE,
		      le64toh(sb->group_desc_table_start) * CRYEXTS_BLOCK_SIZE) < 0)
		return -1;
	*groups_out = (struct cryexts_group_desc *)gdt_block;
	return 0;
}

static int read_inode_at(int fd, const struct cryexts_super_block *sb,
			 const struct cryexts_group_desc *groups,
			 uint64_t ino,
			 struct cryexts_inode *inode,
			 uint64_t *disk_off_out)
{
	uint64_t index = ino - 1;
	uint64_t block;
	uint64_t offset;

	if (ino < CRYEXTS_ROOT_INO || ino > le64toh(sb->inodes_count))
		return -1;

	if (le32toh(sb->features_incompat) & CRYEXTS_FEATURE_INCOMPAT_BLOCK_GROUPS) {
		uint64_t inodes_per_group = le64toh(sb->inodes_per_group);
		uint64_t group = index / inodes_per_group;
		uint64_t index_in_group = index % inodes_per_group;

		block = le64toh(groups[group].inode_table_start) +
			index_in_group / inodes_per_block();
		offset = (index_in_group % inodes_per_block()) * sizeof(struct cryexts_inode);
	} else {
		block = le64toh(sb->inode_table_start) + index / inodes_per_block();
		offset = (index % inodes_per_block()) * sizeof(struct cryexts_inode);
	}

	if (disk_off_out)
		*disk_off_out = block * CRYEXTS_BLOCK_SIZE + offset;
	return read_full(fd, inode, sizeof(*inode), block * CRYEXTS_BLOCK_SIZE + offset);
}

static int write_inode_at(int fd, uint64_t disk_off,
			  const struct cryexts_inode *inode)
{
	return write_full(fd, inode, sizeof(*inode), disk_off);
}

static int find_dir_entry_in_root(int fd, const struct cryexts_super_block *sb,
				  const char *name,
				  unsigned char *dir_block,
				  uint64_t *dir_block_no_out,
				  unsigned int *entry_off_out,
				  uint64_t *target_ino_out)
{
	struct cryexts_inode root_inode;
	struct cryexts_group_desc *groups = NULL;
	unsigned char gdt_block[CRYEXTS_BLOCK_SIZE];
	uint64_t root_inode_off;
	uint64_t root_dir_block;
	unsigned int off = 0;

	if (read_gdt(fd, sb, gdt_block, &groups) < 0)
		return -1;
	if (read_inode_at(fd, sb, groups, CRYEXTS_ROOT_INO, &root_inode, &root_inode_off) < 0)
		return -1;

	root_dir_block = le64toh(root_inode.block[0]);
	if (read_full(fd, dir_block, CRYEXTS_BLOCK_SIZE,
		      root_dir_block * CRYEXTS_BLOCK_SIZE) < 0)
		return -1;

	while (off < CRYEXTS_BLOCK_SIZE) {
		struct cryexts_dir_entry *de =
			(struct cryexts_dir_entry *)(dir_block + off);
		unsigned int rec_len = le16toh(de->rec_len);
		uint64_t ino = le64toh(de->inode);

		if (rec_len < CRYEXTS_DIR_ENTRY_HEADER_SIZE || off + rec_len > CRYEXTS_BLOCK_SIZE)
			return -1;
		if (ino && de->name_len == strlen(name) &&
		    !memcmp(de->name, name, de->name_len)) {
			*dir_block_no_out = root_dir_block;
			*entry_off_out = off;
			*target_ino_out = ino;
			return 0;
		}
		off += rec_len;
	}

	errno = ENOENT;
	return -1;
}

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s <image-or-device> <root-file-name>\n", prog);
}

int main(int argc, char **argv)
{
	unsigned char super_block[CRYEXTS_BLOCK_SIZE];
	unsigned char gdt_block[CRYEXTS_BLOCK_SIZE];
	unsigned char dir_block[CRYEXTS_BLOCK_SIZE];
	struct cryexts_super_block *sb;
	struct cryexts_group_desc *groups = NULL;
	struct cryexts_inode inode;
	struct cryexts_inode_extra *extra;
	struct cryexts_dir_entry *de;
	uint64_t dir_block_no;
	uint64_t ino;
	uint64_t inode_off;
	unsigned int entry_off;
	uint32_t incompat;
	uint32_t state;
	int fd;

	if (argc != 3) {
		usage(argv[0]);
		return 2;
	}

	fd = open(argv[1], O_RDWR);
	if (fd < 0) {
		perror("open");
		return 2;
	}

	if (read_full(fd, super_block, sizeof(super_block), 0) < 0) {
		perror("read superblock");
		close(fd);
		return 2;
	}

	sb = (struct cryexts_super_block *)(super_block + CRYEXTS_SUPER_OFFSET);
	if (le32toh(sb->magic) != CRYEXTS_MAGIC) {
		fprintf(stderr, "cryexts_orphan_inject: bad magic\n");
		close(fd);
		return 1;
	}
	if (!(le32toh(sb->features_incompat) & CRYEXTS_FEATURE_INCOMPAT_ORPHAN_LIST)) {
		fprintf(stderr, "cryexts_orphan_inject: orphan feature is not enabled\n");
		close(fd);
		return 1;
	}
	if (read_gdt(fd, sb, gdt_block, &groups) < 0) {
		perror("read gdt");
		close(fd);
		return 2;
	}

	if (find_dir_entry_in_root(fd, sb, argv[2], dir_block, &dir_block_no,
				   &entry_off, &ino) < 0) {
		perror("find root dir entry");
		close(fd);
		return 2;
	}

	if (read_inode_at(fd, sb, groups, ino, &inode, &inode_off) < 0) {
		perror("read target inode");
		close(fd);
		return 2;
	}

	de = (struct cryexts_dir_entry *)(dir_block + entry_off);
	de->inode = htole64(0);
	de->name_len = 0;
	de->file_type = CRYEXTS_FT_UNKNOWN;
	if (write_full(fd, dir_block, sizeof(dir_block),
		       dir_block_no * CRYEXTS_BLOCK_SIZE) < 0) {
		perror("write root dir block");
		close(fd);
		return 2;
	}

	extra = (struct cryexts_inode_extra *)
		(inode.reserved + sizeof(inode.reserved) -
		 sizeof(struct cryexts_inode_extra));
	inode.links_count = htole16(0);
	extra->next_orphan = htole64(0);
	if (write_inode_at(fd, inode_off, &inode) < 0) {
		perror("write target inode");
		close(fd);
		return 2;
	}

	sb->orphan_head = htole64(ino);
	incompat = le32toh(sb->features_incompat) | CRYEXTS_FEATURE_INCOMPAT_NEEDS_RECOVERY;
	state = le32toh(sb->state) | CRYEXTS_FS_STATE_NEEDS_RECOVERY;
	state &= ~CRYEXTS_FS_STATE_CLEAN;
	sb->features_incompat = htole32(incompat);
	sb->state = htole32(state);
	if (write_full(fd, super_block, sizeof(super_block), 0) < 0) {
		perror("write superblock");
		close(fd);
		return 2;
	}

	close(fd);
	printf("Injected orphan recovery scenario into %s\n", argv[1]);
	printf("Removed root dir entry: %s\n", argv[2]);
	printf("Orphan head inode: %llu\n", (unsigned long long)ino);
	return 0;
}
