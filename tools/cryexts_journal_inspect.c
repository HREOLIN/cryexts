// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
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

static uint32_t checksum_skip(const void *buf, size_t len,
			      size_t skip_offset, size_t skip_len)
{
	const uint8_t *bytes = buf;
	uint32_t hash = 2166136261u;
	size_t i;

	for (i = 0; i < len; i++) {
		if (i >= skip_offset && i < skip_offset + skip_len)
			continue;
		hash ^= bytes[i];
		hash *= 16777619u;
	}
	return hash;
}

static uint32_t journal_v1_checksum(const void *buf)
{
	return checksum_skip(buf, CRYEXTS_BLOCK_SIZE,
			     CRYEXTS_JOURNAL_CHECKSUM_OFFSET,
			     sizeof(__le32));
}

static uint32_t journal_v2_checksum(const void *buf, size_t checksum_offset)
{
	return checksum_skip(buf, CRYEXTS_BLOCK_SIZE, checksum_offset, sizeof(__le32));
}

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s <image>\n", prog);
}

int main(int argc, char **argv)
{
	unsigned char super_block[CRYEXTS_BLOCK_SIZE];
	unsigned char block[CRYEXTS_BLOCK_SIZE];
	struct cryexts_super_block *sb;
	int fd;
	uint64_t journal_block;
	uint64_t journal_blocks;
	int journal_v2;

	if (argc != 2) {
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
		fprintf(stderr, "cryexts_journal_inspect: bad magic\n");
		close(fd);
		return 1;
	}

	journal_block = le64toh(sb->journal_block);
	journal_blocks = le64toh(sb->journal_blocks);
	journal_v2 = !!(le32toh(sb->features_incompat) &
			CRYEXTS_FEATURE_INCOMPAT_JOURNAL_V2);

	printf("version=%u\n", le32toh(sb->version));
	printf("journal_enabled=%u\n",
	       !!(le32toh(sb->features_compat) & CRYEXTS_FEATURE_COMPAT_HAS_JOURNAL));
	printf("journal_format=%s\n", journal_v2 ? "v2" : "v1");
	printf("journal_block=%llu\n", (unsigned long long)journal_block);
	printf("journal_blocks=%llu\n", (unsigned long long)journal_blocks);

	if (!journal_block || !journal_blocks) {
		close(fd);
		return 0;
	}

	if (read_full(fd, block, sizeof(block), journal_block * CRYEXTS_BLOCK_SIZE) < 0) {
		perror("read journal block");
		close(fd);
		return 2;
	}

	if (!journal_v2) {
		const struct cryexts_journal_header *jh =
			(const struct cryexts_journal_header *)block;

		printf("v1.magic=%u\n", le32toh(jh->magic));
		printf("v1.flags=%u\n", le32toh(jh->flags));
		printf("v1.entry_count=%u\n", le32toh(jh->entry_count));
		printf("v1.sequence=%llu\n",
		       (unsigned long long)le64toh(jh->sequence));
		printf("v1.checksum=%u\n", le32toh(jh->checksum));
		printf("v1.expected_checksum=%u\n", journal_v1_checksum(block));
		close(fd);
		return 0;
	}

	{
		unsigned char descriptor_block[CRYEXTS_BLOCK_SIZE];
		unsigned char commit_block[CRYEXTS_BLOCK_SIZE];
		const struct cryexts_journal_v2_control *jc =
			(const struct cryexts_journal_v2_control *)block;
		const struct cryexts_journal_v2_descriptor *jd;
		const struct cryexts_journal_v2_commit *commit;
		uint32_t entries;
		uint32_t i;

		printf("control.magic=%u\n", le32toh(jc->magic));
		printf("control.layout_version=%u\n", le16toh(jc->layout_version));
		printf("control.block_type=%u\n", le16toh(jc->block_type));
		printf("control.flags=%u\n", le32toh(jc->flags));
		printf("control.features=%u\n", le32toh(jc->features));
		printf("control.last_sequence=%llu\n",
		       (unsigned long long)le64toh(jc->last_sequence));
		printf("control.active_sequence=%llu\n",
		       (unsigned long long)le64toh(jc->active_sequence));
		printf("control.tail_sequence=%llu\n",
		       (unsigned long long)le64toh(jc->tail_sequence));
		printf("control.checkpoint_sequence=%llu\n",
		       (unsigned long long)le64toh(jc->checkpoint_sequence));
		printf("control.descriptor_block=%llu\n",
		       (unsigned long long)le64toh(jc->descriptor_block));
		printf("control.payload_start=%llu\n",
		       (unsigned long long)le64toh(jc->payload_start));
		printf("control.payload_blocks=%llu\n",
		       (unsigned long long)le64toh(jc->payload_blocks));
		printf("control.commit_block=%llu\n",
		       (unsigned long long)le64toh(jc->commit_block));
		printf("control.idle=%u\n", le64toh(jc->active_sequence) == 0);
		printf("control.checkpoint_complete=%u\n",
		       le64toh(jc->active_sequence) == 0 &&
		       le64toh(jc->tail_sequence) ==
			       le64toh(jc->checkpoint_sequence) &&
		       le64toh(jc->checkpoint_sequence) ==
			       le64toh(jc->last_sequence));
		printf("control.checksum=%u\n", le32toh(jc->checksum));
		printf("control.expected_checksum=%u\n",
		       journal_v2_checksum(block,
				 offsetof(struct cryexts_journal_v2_control,
					  checksum)));

		if (read_full(fd, descriptor_block, sizeof(descriptor_block),
			      le64toh(jc->descriptor_block) * CRYEXTS_BLOCK_SIZE) < 0) {
			perror("read descriptor");
			close(fd);
			return 2;
		}
		if (read_full(fd, commit_block, sizeof(commit_block),
			      le64toh(jc->commit_block) * CRYEXTS_BLOCK_SIZE) < 0) {
			perror("read commit");
			close(fd);
			return 2;
		}

		jd = (const struct cryexts_journal_v2_descriptor *)descriptor_block;
		commit = (const struct cryexts_journal_v2_commit *)commit_block;
		printf("descriptor.magic=%u\n", le32toh(jd->magic));
		printf("descriptor.layout_version=%u\n", le16toh(jd->layout_version));
		printf("descriptor.block_type=%u\n", le16toh(jd->block_type));
		printf("descriptor.flags=%u\n", le32toh(jd->flags));
		printf("descriptor.entry_count=%u\n", le32toh(jd->entry_count));
		printf("descriptor.sequence=%llu\n",
		       (unsigned long long)le64toh(jd->sequence));
		printf("descriptor.payload_start=%llu\n",
		       (unsigned long long)le64toh(jd->payload_start));
		printf("descriptor.commit_block=%llu\n",
		       (unsigned long long)le64toh(jd->commit_block));
		printf("descriptor.checksum=%u\n", le32toh(jd->checksum));
		printf("descriptor.expected_checksum=%u\n",
		       journal_v2_checksum(descriptor_block,
				 offsetof(struct cryexts_journal_v2_descriptor,
					  checksum)));

		entries = le32toh(jd->entry_count);
		for (i = 0; i < entries && i < CRYEXTS_JOURNAL_V2_MAX_ENTRIES; i++) {
			printf("descriptor.home[%u]=%llu\n", i,
			       (unsigned long long)le64toh(jd->home_blocks[i]));
		}

		printf("commit.magic=%u\n", le32toh(commit->magic));
		printf("commit.layout_version=%u\n", le16toh(commit->layout_version));
		printf("commit.block_type=%u\n", le16toh(commit->block_type));
		printf("commit.flags=%u\n", le32toh(commit->flags));
		printf("commit.entry_count=%u\n", le32toh(commit->entry_count));
		printf("commit.sequence=%llu\n",
		       (unsigned long long)le64toh(commit->sequence));
		printf("commit.descriptor_block=%llu\n",
		       (unsigned long long)le64toh(commit->descriptor_block));
		printf("commit.checksum=%u\n", le32toh(commit->checksum));
		printf("commit.expected_checksum=%u\n",
		       journal_v2_checksum(commit_block,
				 offsetof(struct cryexts_journal_v2_commit,
					  checksum)));
	}

	close(fd);
	return 0;
}
