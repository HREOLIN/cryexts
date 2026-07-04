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

int main(int argc, char **argv)
{
	unsigned char super_block[CRYEXTS_BLOCK_SIZE];
	unsigned char policy_block[CRYEXTS_BLOCK_SIZE];
	struct cryexts_super_block *sb;
	struct cryexts_policy_table_block *pt;
	int fd;
	uint16_t i;

	if (argc != 2) {
		fprintf(stderr, "Usage: %s <image>\n", argv[0]);
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
		fprintf(stderr, "cryexts_policy_inspect: bad magic\n");
		close(fd);
		return 1;
	}
	if (!(le32toh(sb->features_incompat) & CRYEXTS_FEATURE_INCOMPAT_POLICY_TABLE)) {
		fprintf(stderr, "cryexts_policy_inspect: policy table feature is disabled\n");
		close(fd);
		return 1;
	}
	if (!le64toh(sb->policy_table_block)) {
		fprintf(stderr, "cryexts_policy_inspect: policy table block is not set\n");
		close(fd);
		return 1;
	}

	if (read_full(fd, policy_block, sizeof(policy_block),
		      le64toh(sb->policy_table_block) * CRYEXTS_BLOCK_SIZE) < 0) {
		perror("read policy table");
		close(fd);
		return 2;
	}

	pt = (struct cryexts_policy_table_block *)policy_block;
	printf("policy_table_block=%llu\n",
	       (unsigned long long)le64toh(sb->policy_table_block));
	printf("default_policy=%u\n", le32toh(sb->default_encryption_policy));
	printf("magic=%u\n", le32toh(pt->magic));
	printf("entries=%u\n", le16toh(pt->entry_count));
	for (i = 0; i < le16toh(pt->entry_count); i++) {
		unsigned int j;

		printf("policy[%u].id=%u context=",
		       i, le32toh(pt->entries[i].policy_id));
		for (j = 0; j < CRYEXTS_POLICY_CONTEXT_LEN; j++)
			printf("%02x", pt->entries[i].context[j]);
		printf("\n");
	}

	close(fd);
	return 0;
}
