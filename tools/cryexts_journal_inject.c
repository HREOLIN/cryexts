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

static uint32_t journal_checksum(const void *buf, size_t len)
{
	const uint8_t *bytes = buf;
	uint32_t hash = 2166136261u;
	size_t i;

	for (i = 0; i < len; i++) {
		if (i >= CRYEXTS_JOURNAL_CHECKSUM_OFFSET &&
		    i < CRYEXTS_JOURNAL_CHECKSUM_OFFSET + sizeof(__le32))
			continue;
		hash ^= bytes[i];
		hash *= 16777619u;
	}
	return hash;
}

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

static void usage(const char *prog)
{
	fprintf(stderr, "Usage: %s <image-or-device>\n", prog);
}

int main(int argc, char **argv)
{
	unsigned char super_block[CRYEXTS_BLOCK_SIZE];
	unsigned char home_block[CRYEXTS_BLOCK_SIZE];
	unsigned char journal_header_block[CRYEXTS_BLOCK_SIZE];
	unsigned char zero_block[CRYEXTS_BLOCK_SIZE];
	struct cryexts_super_block *sb;
	struct cryexts_journal_header *jh;
	uint64_t journal_block;
	uint64_t journal_blocks;
	uint64_t root_dir_block;
	uint64_t blocks_count;
	uint64_t sequence;
	uint32_t incompat;
	uint32_t state;
	int fd;

	if (argc != 2) {
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
		fprintf(stderr, "cryexts_journal_inject: bad magic\n");
		close(fd);
		return 1;
	}
	if (!(le32toh(sb->features_compat) & CRYEXTS_FEATURE_COMPAT_HAS_JOURNAL)) {
		fprintf(stderr, "cryexts_journal_inject: filesystem has no journal\n");
		close(fd);
		return 1;
	}

	journal_block = le64toh(sb->journal_block);
	journal_blocks = le64toh(sb->journal_blocks);
	root_dir_block = le64toh(sb->root_dir_block);
	blocks_count = le64toh(sb->blocks_count);
	if (!journal_block || journal_blocks < 2 || root_dir_block >= blocks_count) {
		fprintf(stderr, "cryexts_journal_inject: bad journal or root dir layout\n");
		close(fd);
		return 1;
	}

	if (read_full(fd, home_block, sizeof(home_block),
		      root_dir_block * CRYEXTS_BLOCK_SIZE) < 0) {
		perror("read root dir block");
		close(fd);
		return 2;
	}

	memset(journal_header_block, 0, sizeof(journal_header_block));
	memset(zero_block, 0, sizeof(zero_block));

	if (write_full(fd, home_block, sizeof(home_block),
		       (journal_block + 1) * CRYEXTS_BLOCK_SIZE) < 0) {
		perror("write journal payload");
		close(fd);
		return 2;
	}

	jh = (struct cryexts_journal_header *)journal_header_block;
	sequence = 1;
	if (read_full(fd, journal_header_block, sizeof(journal_header_block),
		      journal_block * CRYEXTS_BLOCK_SIZE) == 0) {
		if (le32toh(jh->magic) == CRYEXTS_JOURNAL_MAGIC)
			sequence = le64toh(jh->sequence) + 1;
	}

	memset(journal_header_block, 0, sizeof(journal_header_block));
	jh = (struct cryexts_journal_header *)journal_header_block;
	jh->magic = htole32(CRYEXTS_JOURNAL_MAGIC);
	jh->flags = htole32(CRYEXTS_JOURNAL_FLAG_VALID);
	jh->entry_count = htole32(1);
	jh->sequence = htole64(sequence);
	jh->home_blocks[0] = htole64(root_dir_block);
	jh->checksum = htole32(journal_checksum(journal_header_block,
					       sizeof(journal_header_block)));

	if (write_full(fd, journal_header_block, sizeof(journal_header_block),
		       journal_block * CRYEXTS_BLOCK_SIZE) < 0) {
		perror("write journal header");
		close(fd);
		return 2;
	}

	incompat = le32toh(sb->features_incompat);
	state = le32toh(sb->state);
	incompat |= CRYEXTS_FEATURE_INCOMPAT_NEEDS_RECOVERY;
	state |= CRYEXTS_FS_STATE_NEEDS_RECOVERY;
	state &= ~CRYEXTS_FS_STATE_CLEAN;
	sb->features_incompat = htole32(incompat);
	sb->state = htole32(state);

	if (write_full(fd, super_block, sizeof(super_block), 0) < 0) {
		perror("write superblock");
		close(fd);
		return 2;
	}

	if (write_full(fd, zero_block, sizeof(zero_block),
		       root_dir_block * CRYEXTS_BLOCK_SIZE) < 0) {
		perror("corrupt root dir block");
		close(fd);
		return 2;
	}

	close(fd);
	printf("Injected recovery scenario into %s\n", argv[1]);
	printf("Journal header block: %llu\n", (unsigned long long)journal_block);
	printf("Journal payload block: %llu\n",
	       (unsigned long long)(journal_block + 1));
	printf("Corrupted home block: %llu\n", (unsigned long long)root_dir_block);
	return 0;
}
