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

static void print_extent_array(const struct cryexts_extent *extents,
			       uint16_t entries,
			       const char *prefix)
{
	uint16_t i;

	for (i = 0; i < entries; i++) {
		printf("%s[%u]: logical=%llu physical=%llu len=%u flags=%u\n",
		       prefix, i,
		       (unsigned long long)le64toh(extents[i].logical_start),
		       (unsigned long long)le64toh(extents[i].physical_start),
		       le32toh(extents[i].length),
		       le32toh(extents[i].flags));
	}
}

static int print_v2_extent_leaf(int fd, uint64_t leaf_block, uint16_t entries,
				const char *prefix)
{
	unsigned char block[CRYEXTS_BLOCK_SIZE];
	const struct cryexts_extent_header *eh;
	const struct cryexts_extent *extents;

	if (read_full(fd, block, sizeof(block),
		      leaf_block * CRYEXTS_BLOCK_SIZE) < 0)
		return -1;

	eh = (const struct cryexts_extent_header *)block;
	extents = (const struct cryexts_extent *)(block + sizeof(*eh));
	printf("%s.block=%llu\n", prefix, (unsigned long long)leaf_block);
	printf("%s.header.magic=%u\n", prefix, le16toh(eh->magic));
	printf("%s.header.entries=%u\n", prefix, le16toh(eh->entries));
	printf("%s.header.max=%u\n", prefix, le16toh(eh->max));
	print_extent_array(extents, entries, prefix);
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
	const struct cryexts_extent_header *eh;
	const struct cryexts_extent *extents;
	uint64_t ino;
	uint16_t inline_max;
	uint16_t inline_entries;
	uint64_t overflow_block = 0;
	uint16_t overflow_entries = 0;
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
		fprintf(stderr, "cryexts_extent_inspect: bad magic\n");
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

	if (!(le32toh(inode.inode_flags) & CRYEXTS_INODE_FLAG_EXTENTS)) {
		fprintf(stderr, "cryexts_extent_inspect: inode %llu is not extent-backed\n",
			(unsigned long long)ino);
		close(fd);
		return 1;
	}

	eh = (const struct cryexts_extent_header *)inode.reserved;
	inline_max = le16toh(eh->max);
	inline_entries = le16toh(eh->entries);
	extents = (const struct cryexts_extent *)(inode.reserved + sizeof(*eh));

	printf("inode=%llu\n", (unsigned long long)ino);
	printf("inline_max=%u\n", inline_max);
	printf("inline_entries=%u\n", inline_entries);
	printf("inode_size=%llu\n", (unsigned long long)le64toh(inode.size));
	printf("inode_blocks=%llu\n", (unsigned long long)le64toh(inode.blocks));
	if (le32toh(inode.inode_flags) & CRYEXTS_INODE_FLAG_EXTENT_TREE_V2) {
		const struct cryexts_extent_root_ref *refs =
			(const struct cryexts_extent_root_ref *)(inode.reserved +
							 CRYEXTS_EXTENT_TREE_V2_ROOT_REFS_OFFSET);
		uint16_t i;

		printf("tree_v2=1\n");
		printf("leaf_count=%u\n", inline_entries);
		for (i = 0; i < inline_entries; i++) {
			char prefix[32];
			uint64_t logical_start = le64toh(refs[i].logical_start);
			uint64_t leaf_block = le64toh(refs[i].leaf_block);
			uint16_t leaf_entries = le16toh(refs[i].entries);
			uint32_t checksum = le32toh(refs[i].checksum);

			printf("root_ref[%u].logical_start=%llu\n", i,
			       (unsigned long long)logical_start);
			printf("root_ref[%u].leaf_block=%llu\n", i,
			       (unsigned long long)leaf_block);
			printf("root_ref[%u].entries=%u\n", i, leaf_entries);
			printf("root_ref[%u].checksum=%u\n", i, checksum);
			snprintf(prefix, sizeof(prefix), "leaf[%u]", i);
			if (print_v2_extent_leaf(fd, leaf_block, leaf_entries, prefix) < 0) {
				perror("read extent leaf");
				close(fd);
				return 2;
			}
		}
	} else {
		print_extent_array(extents, inline_entries, "inline");

		if (inline_max == CRYEXTS_EXTENT_ROOT_INLINE_EXTENTS) {
			const unsigned char *ptr =
				inode.reserved + CRYEXTS_EXTENT_ROOT_OVERFLOW_OFFSET;
			unsigned char block[CRYEXTS_BLOCK_SIZE];
			const struct cryexts_extent_header *oeh;
			const struct cryexts_extent *oextents;

			overflow_block = le64toh(*((const __le64 *)ptr));
			overflow_entries = le16toh(*((const __le16 *)(ptr + sizeof(__le64))));
			printf("overflow_block=%llu\n",
			       (unsigned long long)overflow_block);
			printf("overflow_entries=%u\n", overflow_entries);

			if (overflow_block) {
				if (read_full(fd, block, sizeof(block),
					      overflow_block * CRYEXTS_BLOCK_SIZE) < 0) {
					perror("read overflow block");
					close(fd);
					return 2;
				}
				oeh = (const struct cryexts_extent_header *)block;
				oextents = (const struct cryexts_extent *)(block + sizeof(*oeh));
				printf("overflow_header.magic=%u\n", le16toh(oeh->magic));
				printf("overflow_header.entries=%u\n", le16toh(oeh->entries));
				printf("overflow_header.max=%u\n", le16toh(oeh->max));
				print_extent_array(oextents, overflow_entries, "overflow");
			}
		}
	}

	close(fd);
	return 0;
}
