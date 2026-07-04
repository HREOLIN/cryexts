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

static int read_inode_at(int fd, const struct cryexts_super_block *sb,
			 const struct cryexts_group_desc *groups,
			 uint64_t ino, struct cryexts_inode *inode)
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

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s <image> <inode-number>\n", prog);
}

int main(int argc, char **argv)
{
	unsigned char super_block[CRYEXTS_BLOCK_SIZE];
	unsigned char gdt_block[CRYEXTS_BLOCK_SIZE];
	unsigned char index_block[CRYEXTS_BLOCK_SIZE];
	struct cryexts_super_block *sb;
	struct cryexts_group_desc *groups = NULL;
	struct cryexts_inode inode;
	uint64_t ino;
	uint64_t index_blk;
	int fd;
	unsigned int i;
	unsigned int active_buckets = 0;
	unsigned int mask_refs = 0;

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
		fprintf(stderr, "cryexts_dir_index_inspect: bad magic\n");
		close(fd);
		return 1;
	}

	if (le32toh(sb->features_incompat) & CRYEXTS_FEATURE_INCOMPAT_BLOCK_GROUPS) {
		if (read_full(fd, gdt_block, sizeof(gdt_block),
			      le64toh(sb->group_desc_table_start) *
				      CRYEXTS_BLOCK_SIZE) < 0) {
			perror("read gdt");
			close(fd);
			return 2;
		}
		groups = (struct cryexts_group_desc *)gdt_block;
	}

	if (read_inode_at(fd, sb, groups, ino, &inode) < 0) {
		perror("read inode");
		close(fd);
		return 2;
	}

	if (!((le16toh(inode.mode) & 0170000) == 0040000)) {
		fprintf(stderr, "cryexts_dir_index_inspect: inode %llu is not a directory\n",
			(unsigned long long)ino);
		close(fd);
		return 1;
	}
	if (!(le32toh(inode.inode_flags) & CRYEXTS_INODE_FLAG_DIR_INDEX)) {
		fprintf(stderr, "cryexts_dir_index_inspect: inode %llu has no dir index\n",
			(unsigned long long)ino);
		close(fd);
		return 1;
	}

	index_blk = le64toh(inode.indirect_block);
	printf("inode=%llu\n", (unsigned long long)ino);
	printf("index_block=%llu\n", (unsigned long long)index_blk);

	if (read_full(fd, index_block, sizeof(index_block),
		      index_blk * CRYEXTS_BLOCK_SIZE) < 0) {
		perror("read index block");
		close(fd);
		return 2;
	}

	{
		const struct cryexts_dir_index_block *idx =
			(const struct cryexts_dir_index_block *)index_block;

		printf("magic=%u\n", le32toh(idx->magic));
		printf("buckets=%u\n", le16toh(idx->buckets));
		printf("dir_blocks=%u\n", le16toh(idx->dir_blocks));
		printf("entries=%u\n", le32toh(idx->entries));
		for (i = 0; i < CRYEXTS_DIR_INDEX_BUCKETS; i++) {
			unsigned int bit;
			uint16_t mask = le16toh(idx->block_masks[i]);

			if (!mask)
				continue;
			active_buckets++;
			for (bit = 0; bit < 16; bit++) {
				if (mask & (1U << bit))
					mask_refs++;
			}
			printf("bucket[%u]=0x%04x\n", i, mask);
		}
		printf("active_buckets=%u\n", active_buckets);
		printf("mask_refs=%u\n", mask_refs);
	}

	close(fd);
	return 0;
}
