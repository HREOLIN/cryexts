// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "../cryexts_fs.h"

#define CRYEXTS_META_TAG_GROUP 0x47525044U

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

static uint64_t div_round_up_u64(uint64_t a, uint64_t b)
{
	return (a + b - 1) / b;
}

static uint32_t metadata_fnv1a_bytes(const void *buf, size_t len, uint32_t hash)
{
	const uint8_t *bytes = buf;
	size_t i;

	for (i = 0; i < len; i++) {
		hash ^= bytes[i];
		hash *= 16777619u;
	}

	return hash;
}

static uint32_t metadata_seed(uint64_t fs_generation, uint64_t block, uint32_t tag)
{
	uint32_t hash = 2166136261u;

	hash = metadata_fnv1a_bytes(&fs_generation, sizeof(fs_generation), hash);
	hash = metadata_fnv1a_bytes(&block, sizeof(block), hash);
	hash = metadata_fnv1a_bytes(&tag, sizeof(tag), hash);
	return hash;
}

static uint32_t metadata_checksum_skip(const void *buf, size_t len,
				       size_t skip_offset, size_t skip_len,
				       uint32_t seed)
{
	const uint8_t *bytes = buf;
	uint32_t hash = seed;
	size_t i;

	for (i = 0; i < len; i++) {
		if (i >= skip_offset && i < skip_offset + skip_len)
			continue;
		hash ^= bytes[i];
		hash *= 16777619u;
	}

	return hash;
}

static int metadata_csum_enabled(const struct cryexts_super_block *sb)
{
	return le32toh(sb->version) >= CRYEXTS_VERSION_V5 &&
	       !!(le32toh(sb->features_ro_compat) &
		  CRYEXTS_FEATURE_RO_COMPAT_METADATA_CSUM) &&
	       le64toh(sb->metadata_csum_type) == CRYEXTS_METADATA_CSUM_FNV1A32;
}

static uint32_t group_expected_checksum(const struct cryexts_super_block *sb,
					const struct cryexts_group_desc *group)
{
	uint32_t seed;

	seed = metadata_seed(le64toh(sb->fs_generation),
			     le64toh(group->group_start),
			     CRYEXTS_META_TAG_GROUP);
	return metadata_checksum_skip(
		group, sizeof(*group),
		offsetof(struct cryexts_group_desc, reserved), sizeof(__le32),
		seed);
}

int main(int argc, char **argv)
{
	unsigned char super_block[CRYEXTS_BLOCK_SIZE];
	struct cryexts_super_block *sb;
	struct cryexts_group_desc *groups;
	unsigned char *gdt_region;
	uint64_t group_count;
	uint64_t gdt_start;
	uint64_t gdt_blocks;
	uint64_t gdt_bytes;
	uint64_t expected_gdt_blocks;
	uint64_t group;
	int fd;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s <image-or-device>\n", argv[0]);
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
		fprintf(stderr, "cryexts_gdt_inspect: bad magic\n");
		close(fd);
		return 1;
	}
	if (!(le32toh(sb->features_incompat) & CRYEXTS_FEATURE_INCOMPAT_BLOCK_GROUPS)) {
		fprintf(stderr,
			"cryexts_gdt_inspect: filesystem does not use block groups\n");
		close(fd);
		return 1;
	}

	group_count = le64toh(sb->group_count);
	gdt_start = le64toh(sb->group_desc_table_start);
	gdt_blocks = le64toh(sb->group_desc_table_blocks);
	gdt_bytes = gdt_blocks * CRYEXTS_BLOCK_SIZE;
	expected_gdt_blocks = div_round_up_u64(
		group_count * sizeof(struct cryexts_group_desc),
		CRYEXTS_BLOCK_SIZE);

	if (!gdt_start || !gdt_blocks) {
		fprintf(stderr, "cryexts_gdt_inspect: invalid GDT range\n");
		close(fd);
		return 1;
	}

	gdt_region = calloc(gdt_blocks, CRYEXTS_BLOCK_SIZE);
	if (!gdt_region) {
		perror("calloc");
		close(fd);
		return 2;
	}

	if (read_full(fd, gdt_region, gdt_bytes, gdt_start * CRYEXTS_BLOCK_SIZE) < 0) {
		perror("read gdt");
		free(gdt_region);
		close(fd);
		return 2;
	}

	groups = (struct cryexts_group_desc *)gdt_region;

	printf("version=%u\n", le32toh(sb->version));
	printf("blocks=%" PRIu64 "\n", le64toh(sb->blocks_count));
	printf("inodes=%" PRIu64 "\n", le64toh(sb->inodes_count));
	printf("group_count=%" PRIu64 "\n", group_count);
	printf("blocks_per_group=%" PRIu64 "\n", le64toh(sb->blocks_per_group));
	printf("inodes_per_group=%" PRIu64 "\n", le64toh(sb->inodes_per_group));
	printf("gdt_start=%" PRIu64 "\n", gdt_start);
	printf("gdt_blocks=%" PRIu64 "\n", gdt_blocks);
	printf("gdt_bytes=%" PRIu64 "\n", gdt_bytes);
	printf("desc_size=%zu\n", sizeof(struct cryexts_group_desc));
	printf("descs_per_block=%u\n",
	       (unsigned)(CRYEXTS_BLOCK_SIZE / sizeof(struct cryexts_group_desc)));
	printf("expected_gdt_blocks=%" PRIu64 "\n", expected_gdt_blocks);
	printf("root_block_bitmap=%" PRIu64 "\n", le64toh(sb->block_bitmap_block));
	printf("root_inode_bitmap=%" PRIu64 "\n", le64toh(sb->inode_bitmap_block));
	printf("root_inode_table_start=%" PRIu64 "\n", le64toh(sb->inode_table_start));
	printf("root_dir_block=%" PRIu64 "\n", le64toh(sb->root_dir_block));

	for (group = 0; group < group_count; group++) {
		const struct cryexts_group_desc *gd = &groups[group];

		printf("group[%" PRIu64 "].start=%" PRIu64 "\n",
		       group, le64toh(gd->group_start));
		printf("group[%" PRIu64 "].blocks=%" PRIu64 "\n",
		       group, le64toh(gd->blocks_count));
		printf("group[%" PRIu64 "].block_bitmap=%" PRIu64 "\n",
		       group, le64toh(gd->block_bitmap_block));
		printf("group[%" PRIu64 "].inode_bitmap=%" PRIu64 "\n",
		       group, le64toh(gd->inode_bitmap_block));
		printf("group[%" PRIu64 "].inode_table_start=%" PRIu64 "\n",
		       group, le64toh(gd->inode_table_start));
		printf("group[%" PRIu64 "].inode_table_blocks=%u\n",
		       group, le32toh(gd->inode_table_blocks));
		printf("group[%" PRIu64 "].free_blocks=%u\n",
		       group, le32toh(gd->free_blocks_count));
		printf("group[%" PRIu64 "].free_inodes=%u\n",
		       group, le32toh(gd->free_inodes_count));
		printf("group[%" PRIu64 "].used_dirs=%u\n",
		       group, le32toh(gd->used_dirs_count));
		if (metadata_csum_enabled(sb)) {
			uint32_t stored = le32toh(*(__le32 *)gd->reserved);
			uint32_t expected = group_expected_checksum(sb, gd);

			printf("group[%" PRIu64 "].checksum=%u\n", group, stored);
			printf("group[%" PRIu64 "].expected_checksum=%u\n",
			       group, expected);
		}
	}

	free(gdt_region);
	close(fd);
	return 0;
}
