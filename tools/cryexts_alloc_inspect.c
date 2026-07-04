// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
	if (!(le32toh(sb->features_incompat) &
	      CRYEXTS_FEATURE_INCOMPAT_BLOCK_GROUPS)) {
		*groups_out = NULL;
		return 0;
	}
	if (read_full(fd, gdt_block, CRYEXTS_BLOCK_SIZE,
		      le64toh(sb->group_desc_table_start) *
			      CRYEXTS_BLOCK_SIZE) < 0)
		return -1;
	*groups_out = (struct cryexts_group_desc *)gdt_block;
	return 0;
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

	if (le32toh(sb->features_incompat) &
	    CRYEXTS_FEATURE_INCOMPAT_BLOCK_GROUPS) {
		uint64_t inodes_per_group = le64toh(sb->inodes_per_group);
		uint64_t group = index / inodes_per_group;
		uint64_t index_in_group = index % inodes_per_group;

		block = le64toh(groups[group].inode_table_start) +
			index_in_group / inodes_per_block();
		offset = (index_in_group % inodes_per_block()) *
			 sizeof(struct cryexts_inode);
	} else {
		block = le64toh(sb->inode_table_start) +
			index / inodes_per_block();
		offset = (index % inodes_per_block()) *
			 sizeof(struct cryexts_inode);
	}

	return read_full(fd, inode, sizeof(*inode),
			 block * CRYEXTS_BLOCK_SIZE + offset);
}

static void account_extent(uint64_t logical, uint64_t physical, uint32_t len,
			   uint64_t blocks_per_group,
			   uint64_t *first_data_block,
			   uint64_t *first_data_group,
			   uint64_t *segments,
			   uint64_t *largest_extent_len)
{
	(void)logical;
	if (!len)
		return;
	if (!*first_data_block) {
		*first_data_block = physical;
		*first_data_group = blocks_per_group ?
			physical / blocks_per_group : 0;
	}
	(*segments)++;
	if (len > *largest_extent_len)
		*largest_extent_len = len;
}

static int account_v2_leaf(int fd, uint64_t leaf_block, uint16_t entries,
			   uint64_t blocks_per_group,
			   uint64_t *first_data_block,
			   uint64_t *first_data_group,
			   uint64_t *segments,
			   uint64_t *largest_extent_len)
{
	unsigned char block[CRYEXTS_BLOCK_SIZE];
	const struct cryexts_extent_header *eh;
	const struct cryexts_extent *extents;
	uint16_t i;

	if (read_full(fd, block, sizeof(block),
		      leaf_block * CRYEXTS_BLOCK_SIZE) < 0)
		return -1;
	eh = (const struct cryexts_extent_header *)block;
	if (le16toh(eh->magic) != CRYEXTS_EXTENT_MAGIC ||
	    le16toh(eh->entries) != entries)
		return -1;
	extents = (const struct cryexts_extent *)(block + sizeof(*eh));
	for (i = 0; i < entries; i++) {
		account_extent(le64toh(extents[i].logical_start),
			       le64toh(extents[i].physical_start),
			       le32toh(extents[i].length),
			       blocks_per_group, first_data_block,
			       first_data_group, segments, largest_extent_len);
	}
	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s <image> <inode-number>...\n", prog);
}

int main(int argc, char **argv)
{
	unsigned char super_block[CRYEXTS_BLOCK_SIZE];
	unsigned char gdt_block[CRYEXTS_BLOCK_SIZE];
	struct cryexts_super_block *sb;
	struct cryexts_group_desc *groups = NULL;
	uint64_t blocks_per_group;
	uint64_t inodes_per_group;
	int fd;
	int argi;

	if (argc < 3) {
		usage(argv[0]);
		return 2;
	}

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
		fprintf(stderr, "cryexts_alloc_inspect: bad magic\n");
		close(fd);
		return 1;
	}
	if (read_gdt(fd, sb, gdt_block, &groups) < 0) {
		perror("read gdt");
		close(fd);
		return 2;
	}

	blocks_per_group = le64toh(sb->blocks_per_group);
	inodes_per_group = le64toh(sb->inodes_per_group);
	printf("blocks_per_group=%llu\n",
	       (unsigned long long)blocks_per_group);
	printf("inodes_per_group=%llu\n",
	       (unsigned long long)inodes_per_group);

	for (argi = 2; argi < argc; argi++) {
		struct cryexts_inode inode;
		const struct cryexts_extent_header *eh;
		uint64_t ino = strtoull(argv[argi], NULL, 10);
		uint64_t inode_group = inodes_per_group ?
			(ino - 1) / inodes_per_group : 0;
		uint64_t first_data_block = 0;
		uint64_t first_data_group = 0;
		uint64_t segments = 0;
		uint64_t largest_extent_len = 0;
		uint32_t inode_flags;
		uint16_t inline_entries;
		uint16_t inline_max;

		if (read_inode_at(fd, sb, groups, ino, &inode) < 0) {
			perror("read inode");
			close(fd);
			return 2;
		}
		inode_flags = le32toh(inode.inode_flags);
		printf("inode[%llu].group=%llu\n",
		       (unsigned long long)ino,
		       (unsigned long long)inode_group);
		printf("inode[%llu].size=%llu\n",
		       (unsigned long long)ino,
		       (unsigned long long)le64toh(inode.size));
		printf("inode[%llu].blocks=%llu\n",
		       (unsigned long long)ino,
		       (unsigned long long)le64toh(inode.blocks));

		if (inode_flags & CRYEXTS_INODE_FLAG_EXTENTS) {
			eh = (const struct cryexts_extent_header *)inode.reserved;
			inline_entries = le16toh(eh->entries);
			inline_max = le16toh(eh->max);
			if (inode_flags & CRYEXTS_INODE_FLAG_EXTENT_TREE_V2) {
				const struct cryexts_extent_root_ref *refs =
					(const struct cryexts_extent_root_ref *)
						(inode.reserved +
						 CRYEXTS_EXTENT_TREE_V2_ROOT_REFS_OFFSET);
				uint16_t i;

				for (i = 0; i < inline_entries; i++) {
					if (account_v2_leaf(
						    fd,
						    le64toh(refs[i].leaf_block),
						    le16toh(refs[i].entries),
						    blocks_per_group,
						    &first_data_block,
						    &first_data_group,
						    &segments,
						    &largest_extent_len) < 0) {
						perror("read extent leaf");
						close(fd);
						return 2;
					}
				}
			} else {
				const struct cryexts_extent *extents =
					(const struct cryexts_extent *)
						(inode.reserved + sizeof(*eh));
				uint16_t i;

				for (i = 0; i < inline_entries; i++)
					account_extent(
						le64toh(extents[i].logical_start),
						le64toh(extents[i].physical_start),
						le32toh(extents[i].length),
						blocks_per_group,
						&first_data_block,
						&first_data_group,
						&segments,
						&largest_extent_len);
				if (inline_max == CRYEXTS_EXTENT_ROOT_INLINE_EXTENTS) {
					const unsigned char *ptr =
						inode.reserved +
						CRYEXTS_EXTENT_ROOT_OVERFLOW_OFFSET;
					uint64_t overflow_block =
						le64toh(*((const __le64 *)ptr));
					uint16_t overflow_entries =
						le16toh(*((const __le16 *)
							(ptr + sizeof(__le64))));

					if (overflow_block &&
					    account_v2_leaf(fd, overflow_block,
							    overflow_entries,
							    blocks_per_group,
							    &first_data_block,
							    &first_data_group,
							    &segments,
							    &largest_extent_len) < 0) {
						perror("read extent overflow");
						close(fd);
						return 2;
					}
				}
			}
		} else if (le64toh(inode.block[0])) {
			first_data_block = le64toh(inode.block[0]);
			first_data_group = blocks_per_group ?
				first_data_block / blocks_per_group : 0;
			segments = 1;
			largest_extent_len = 1;
		}
		printf("inode[%llu].first_data_block=%llu\n",
		       (unsigned long long)ino,
		       (unsigned long long)first_data_block);
		printf("inode[%llu].first_data_group=%llu\n",
		       (unsigned long long)ino,
		       (unsigned long long)first_data_group);
		printf("inode[%llu].extent_segments=%llu\n",
		       (unsigned long long)ino,
		       (unsigned long long)segments);
		printf("inode[%llu].largest_extent_len=%llu\n",
		       (unsigned long long)ino,
		       (unsigned long long)largest_extent_len);
	}
	close(fd);
	return 0;
}
