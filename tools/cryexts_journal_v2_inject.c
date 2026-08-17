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

#define CRYEXTS_META_TAG_SUPER 0x53555052U
#define CRYEXTS_V11_ROLLBACK_MARKER "CRYEXTS_V11_V2_COMMITTED_HOME"

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

static uint32_t journal_v2_checksum(const void *buf, size_t checksum_offset)
{
	return checksum_skip(buf, CRYEXTS_BLOCK_SIZE, checksum_offset,
			     sizeof(__le32));
}

static uint32_t metadata_fnv1a_bytes(const void *data, size_t len, uint32_t seed)
{
	const uint8_t *bytes = data;
	size_t i;

	for (i = 0; i < len; i++) {
		seed ^= bytes[i];
		seed *= 16777619u;
	}
	return seed;
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
	size_t i;

	for (i = 0; i < len; i++) {
		if (i >= skip_offset && i < skip_offset + skip_len)
			continue;
		seed ^= bytes[i];
		seed *= 16777619u;
	}
	return seed;
}

static void set_super_checksum(struct cryexts_super_block *sb)
{
	uint32_t checksum;
	uint32_t seed;
	__le32 stored;

	if (le64toh(sb->metadata_csum_type) != CRYEXTS_METADATA_CSUM_FNV1A32)
		return;

	seed = metadata_seed(le64toh(sb->fs_generation), 0,
			     CRYEXTS_META_TAG_SUPER);
	checksum = metadata_checksum_skip(
		sb, sizeof(*sb),
		offsetof(struct cryexts_super_block, reserved), sizeof(__le32),
		seed);
	stored = htole32(checksum);
	memcpy(sb->reserved, &stored, sizeof(stored));
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s <image-or-device> [recovery|rollback-window]\n",
		prog);
}

static int mark_root_dir_slack(unsigned char *block)
{
	const char marker[] = CRYEXTS_V11_ROLLBACK_MARKER;
	size_t offset = 0;

	while (offset < CRYEXTS_BLOCK_SIZE) {
		struct cryexts_dir_entry *de =
			(struct cryexts_dir_entry *)(block + offset);
		unsigned int rec_len = le16toh(de->rec_len);
		unsigned int used;

		if (rec_len < CRYEXTS_DIR_ENTRY_HEADER_SIZE || rec_len % 4 ||
		    offset + rec_len > CRYEXTS_BLOCK_SIZE)
			return -1;
		used = cryexts_dir_rec_len(de->name_len);
		if (offset + rec_len == CRYEXTS_BLOCK_SIZE) {
			if (rec_len < used + sizeof(marker))
				return -1;
			memcpy(block + offset + rec_len - sizeof(marker), marker,
			       sizeof(marker));
			return 0;
		}
		offset += rec_len;
	}
	return -1;
}

int main(int argc, char **argv)
{
	unsigned char super_block[CRYEXTS_BLOCK_SIZE];
	unsigned char home_block[CRYEXTS_BLOCK_SIZE];
	unsigned char control_block[CRYEXTS_BLOCK_SIZE];
	unsigned char descriptor_block[CRYEXTS_BLOCK_SIZE];
	unsigned char commit_block[CRYEXTS_BLOCK_SIZE];
	unsigned char zero_block[CRYEXTS_BLOCK_SIZE];
	struct cryexts_super_block *sb;
	struct cryexts_journal_v2_control *jc;
	struct cryexts_journal_v2_descriptor *jd;
	struct cryexts_journal_v2_commit *jcommit;
	uint64_t journal_block;
	uint64_t journal_blocks;
	uint64_t payload_block;
	uint64_t commit_block_no;
	uint64_t root_dir_block;
	uint64_t sequence;
	uint32_t incompat;
	uint32_t state;
	int rollback_window = 0;
	int fd;

	if (argc < 2 || argc > 3) {
		usage(argv[0]);
		return 2;
	}
	if (argc == 3) {
		if (!strcmp(argv[2], "rollback-window"))
			rollback_window = 1;
		else if (strcmp(argv[2], "recovery")) {
			usage(argv[0]);
			return 2;
		}
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
		fprintf(stderr, "cryexts_journal_v2_inject: bad magic\n");
		close(fd);
		return 1;
	}
	if (!(le32toh(sb->features_compat) & CRYEXTS_FEATURE_COMPAT_HAS_JOURNAL) ||
	    !(le32toh(sb->features_incompat) & CRYEXTS_FEATURE_INCOMPAT_JOURNAL_V2)) {
		fprintf(stderr, "cryexts_journal_v2_inject: filesystem is not journal v2\n");
		close(fd);
		return 1;
	}

	journal_block = le64toh(sb->journal_block);
	journal_blocks = le64toh(sb->journal_blocks);
	root_dir_block = le64toh(sb->root_dir_block);
	payload_block = journal_block + 2;
	commit_block_no = journal_block + journal_blocks - 1;
	sequence = le64toh(sb->journal_sequence) + 1;
	if (!journal_block || journal_blocks < CRYEXTS_JOURNAL_V2_MIN_BLOCKS) {
		fprintf(stderr, "cryexts_journal_v2_inject: bad journal v2 layout\n");
		close(fd);
		return 1;
	}

	if (read_full(fd, home_block, sizeof(home_block),
		      root_dir_block * CRYEXTS_BLOCK_SIZE) < 0) {
		perror("read root dir block");
		close(fd);
		return 2;
	}

	memset(control_block, 0, sizeof(control_block));
	memset(descriptor_block, 0, sizeof(descriptor_block));
	memset(commit_block, 0, sizeof(commit_block));
	memset(zero_block, 0, sizeof(zero_block));

	if (write_full(fd, home_block, sizeof(home_block),
		       payload_block * CRYEXTS_BLOCK_SIZE) < 0) {
		perror("write payload block");
		close(fd);
		return 2;
	}

	jc = (struct cryexts_journal_v2_control *)control_block;
	jc->magic = htole32(CRYEXTS_JOURNAL_V2_MAGIC);
	jc->layout_version = htole16(CRYEXTS_JOURNAL_V2_LAYOUT_VERSION);
	jc->block_type = htole16(CRYEXTS_JOURNAL_V2_BLOCK_CONTROL);
	jc->flags = htole32(CRYEXTS_JOURNAL_V2_FLAG_ACTIVE);
	jc->features = htole32(CRYEXTS_JOURNAL_V2_FEATURE_BASELINE);
	jc->last_sequence = htole64(sequence - 1);
	jc->active_sequence = htole64(sequence);
	jc->tail_sequence = htole64(sequence - 1);
	jc->checkpoint_sequence = htole64(sequence - 1);
	jc->descriptor_block = htole64(journal_block + 1);
	jc->payload_start = htole64(payload_block);
	jc->payload_blocks = htole64(journal_blocks - 3);
	jc->commit_block = htole64(commit_block_no);
	jc->checksum = htole32(journal_v2_checksum(
		control_block,
		offsetof(struct cryexts_journal_v2_control, checksum)));

	jd = (struct cryexts_journal_v2_descriptor *)descriptor_block;
	jd->magic = htole32(CRYEXTS_JOURNAL_V2_MAGIC);
	jd->layout_version = htole16(CRYEXTS_JOURNAL_V2_LAYOUT_VERSION);
	jd->block_type = htole16(CRYEXTS_JOURNAL_V2_BLOCK_DESCRIPTOR);
	jd->flags = htole32(CRYEXTS_JOURNAL_V2_FLAG_ACTIVE);
	jd->entry_count = htole32(1);
	jd->sequence = htole64(sequence);
	jd->payload_start = htole64(payload_block);
	jd->commit_block = htole64(commit_block_no);
	jd->home_blocks[0] = htole64(root_dir_block);
	jd->checksum = htole32(journal_v2_checksum(
		descriptor_block,
		offsetof(struct cryexts_journal_v2_descriptor, checksum)));

	jcommit = (struct cryexts_journal_v2_commit *)commit_block;
	jcommit->magic = htole32(CRYEXTS_JOURNAL_V2_MAGIC);
	jcommit->layout_version = htole16(CRYEXTS_JOURNAL_V2_LAYOUT_VERSION);
	jcommit->block_type = htole16(CRYEXTS_JOURNAL_V2_BLOCK_COMMIT);
	jcommit->flags = htole32(CRYEXTS_JOURNAL_V2_FLAG_COMMITTED);
	jcommit->entry_count = htole32(1);
	jcommit->sequence = htole64(sequence);
	jcommit->descriptor_block = htole64(journal_block + 1);
	jcommit->checksum = htole32(journal_v2_checksum(
		commit_block,
		offsetof(struct cryexts_journal_v2_commit, checksum)));

	if (write_full(fd, control_block, sizeof(control_block),
		       journal_block * CRYEXTS_BLOCK_SIZE) < 0) {
		perror("write control block");
		close(fd);
		return 2;
	}
	if (write_full(fd, descriptor_block, sizeof(descriptor_block),
		       (journal_block + 1) * CRYEXTS_BLOCK_SIZE) < 0) {
		perror("write descriptor block");
		close(fd);
		return 2;
	}
	if (write_full(fd, commit_block, sizeof(commit_block),
		       commit_block_no * CRYEXTS_BLOCK_SIZE) < 0) {
		perror("write commit block");
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
	sb->journal_sequence = htole64(sequence - 1);
	sb->last_write_time = htole64(0);
	set_super_checksum(sb);

	if (write_full(fd, super_block, sizeof(super_block), 0) < 0) {
		perror("write superblock");
		close(fd);
		return 2;
	}

	if (rollback_window && mark_root_dir_slack(home_block) < 0) {
		fprintf(stderr,
			"cryexts_journal_v2_inject: root directory has no safe marker slack\n");
		close(fd);
		return 1;
	}
	if (write_full(fd, rollback_window ? home_block : zero_block,
		       sizeof(home_block),
		       root_dir_block * CRYEXTS_BLOCK_SIZE) < 0) {
		perror(rollback_window ? "write committed home block" :
		       "corrupt home block");
		close(fd);
		return 2;
	}
	if (fsync(fd) < 0) {
		perror("fsync image");
		close(fd);
		return 2;
	}

	close(fd);
	printf("Injected v2 %s scenario into %s\n",
	       rollback_window ? "rollback-window" : "recovery", argv[1]);
	printf("Control block: %llu\n", (unsigned long long)journal_block);
	printf("Descriptor block: %llu\n", (unsigned long long)(journal_block + 1));
	printf("Payload block: %llu\n", (unsigned long long)payload_block);
	printf("Commit block: %llu\n", (unsigned long long)commit_block_no);
	printf("Home block: %llu\n", (unsigned long long)root_dir_block);
	printf("Sequence: %llu\n", (unsigned long long)sequence);
	if (rollback_window)
		printf("Home marker: %s\n", CRYEXTS_V11_ROLLBACK_MARKER);
	return 0;
}
