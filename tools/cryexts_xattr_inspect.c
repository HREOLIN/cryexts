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
			 struct cryexts_inode *inode)
{
	uint64_t index = ino - 1;
	uint64_t block;
	uint64_t offset;

	if (ino < CRYEXTS_ROOT_INO || ino > le64toh(sb->inodes_count)) {
		errno = EINVAL;
		return -1;
	}

	if (le32toh(sb->features_incompat) & CRYEXTS_FEATURE_INCOMPAT_BLOCK_GROUPS) {
		uint64_t inodes_per_group = le64toh(sb->inodes_per_group);
		uint64_t group = index / inodes_per_group;
		uint64_t index_in_group = index % inodes_per_group;

		block = le64toh(groups[group].inode_table_start) +
			index_in_group / inodes_per_block();
		offset = (index_in_group % inodes_per_block()) *
			 sizeof(struct cryexts_inode);
	} else {
		block = le64toh(sb->inode_table_start) + index / inodes_per_block();
		offset = (index % inodes_per_block()) * sizeof(struct cryexts_inode);
	}

	return read_full(fd, inode, sizeof(*inode),
			 block * CRYEXTS_BLOCK_SIZE + offset);
}

static const struct cryexts_inode_extra *inode_extra(const struct cryexts_inode *inode)
{
	return (const struct cryexts_inode_extra *)
		(inode->reserved + sizeof(inode->reserved) -
		 sizeof(struct cryexts_inode_extra));
}

static int print_xattr_block(int fd, uint64_t block, const char *prefix)
{
	unsigned char buf[CRYEXTS_BLOCK_SIZE];
	const struct cryexts_xattr_block_header *xh;
	unsigned int entries;
	unsigned int used;
	unsigned int offset;
	unsigned int i;

	if (read_full(fd, buf, sizeof(buf), block * CRYEXTS_BLOCK_SIZE) < 0)
		return -1;

	xh = (const struct cryexts_xattr_block_header *)buf;
	entries = le16toh(xh->entries);
	used = le16toh(xh->used_bytes);

	printf("%s.block=%llu\n", prefix, (unsigned long long)block);
	printf("%s.magic=%u\n", prefix, le32toh(xh->magic));
	printf("%s.entries=%u\n", prefix, entries);
	printf("%s.used_bytes=%u\n", prefix, used);
	printf("%s.overflow_block=%llu\n", prefix,
	       (unsigned long long)le64toh(xh->overflow_block));

	offset = sizeof(*xh);
	for (i = 0; i < entries; i++) {
		const struct cryexts_xattr_entry *xe;
		unsigned int name_len;
		unsigned int value_len;
		unsigned int total_len;
		char name[CRYEXTS_XATTR_MAX_NAME_LEN + 1];

		xe = (const struct cryexts_xattr_entry *)(buf + offset);
		name_len = xe->name_len;
		value_len = le16toh(xe->value_len);
		total_len = sizeof(*xe) + name_len + value_len;
		memcpy(name, xe->data, name_len);
		name[name_len] = '\0';
		printf("%s.item[%u].name=%s\n", prefix, i, name);
		printf("%s.item[%u].name_len=%u\n", prefix, i, name_len);
		printf("%s.item[%u].value_len=%u\n", prefix, i, value_len);
		printf("%s.item[%u].namespace=%u\n", prefix, i, xe->namespace_id);
		offset += total_len;
	}

	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s <image> <inode-number>\n", prog);
}

int main(int argc, char **argv)
{
	unsigned char super_block[CRYEXTS_BLOCK_SIZE];
	unsigned char gdt_block[CRYEXTS_BLOCK_SIZE];
	struct cryexts_super_block *sb;
	struct cryexts_group_desc *groups = NULL;
	struct cryexts_inode inode;
	uint64_t ino;
	uint64_t root_block;
	uint64_t overflow_block = 0;
	int fd;

	if (argc != 3) {
		usage(argv[0]);
		return 2;
	}

	ino = strtoull(argv[2], NULL, 10);
	fd = open(argv[1], O_RDONLY);
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
		fprintf(stderr, "cryexts_xattr_inspect: bad magic\n");
		close(fd);
		return 1;
	}
	if (read_gdt(fd, sb, gdt_block, &groups) < 0) {
		perror("read gdt");
		close(fd);
		return 2;
	}
	if (read_inode_at(fd, sb, groups, ino, &inode) < 0) {
		perror("read inode");
		close(fd);
		return 2;
	}

	root_block = le64toh(inode_extra(&inode)->xattr_block);
	printf("inode=%llu\n", (unsigned long long)ino);
	printf("xattr_root_block=%llu\n", (unsigned long long)root_block);
	if (!root_block) {
		printf("xattr_present=0\n");
		close(fd);
		return 0;
	}

	printf("xattr_present=1\n");
	if (print_xattr_block(fd, root_block, "root") < 0) {
		perror("read root xattr block");
		close(fd);
		return 2;
	}

	{
		unsigned char root_buf[CRYEXTS_BLOCK_SIZE];
		const struct cryexts_xattr_block_header *xh;

		if (read_full(fd, root_buf, sizeof(root_buf),
			      root_block * CRYEXTS_BLOCK_SIZE) < 0) {
			perror("read root xattr block");
			close(fd);
			return 2;
		}
		xh = (const struct cryexts_xattr_block_header *)root_buf;
		overflow_block = le64toh(xh->overflow_block);
	}
	if (overflow_block) {
		if (print_xattr_block(fd, overflow_block, "overflow") < 0) {
			perror("read overflow xattr block");
			close(fd);
			return 2;
		}
	}

	close(fd);
	return 0;
}
