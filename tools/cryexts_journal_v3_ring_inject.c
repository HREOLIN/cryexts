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
#define RING_TXNS 2
#define RING_MARKER_FMT "CRYEXTS_RING_TXN_%u"

enum inject_scenario {
	SCENARIO_COMMITTED,
	SCENARIO_UNCOMMITTED_TAIL,
};

static int read_full(int fd, void *buf, size_t len, off_t off)
{
	char *p = buf;

	while (len) {
		ssize_t n = pread(fd, p, len, off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (!n) {
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

	while (len) {
		ssize_t n = pwrite(fd, p, len, off);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (!n) {
			errno = EIO;
			return -1;
		}
		p += n;
		off += n;
		len -= n;
	}
	return 0;
}

static uint32_t fnv1a_update(uint32_t hash, const void *buf, size_t len)
{
	const uint8_t *bytes = buf;
	size_t i;

	for (i = 0; i < len; i++) {
		hash ^= bytes[i];
		hash *= 16777619u;
	}
	return hash;
}

static uint32_t checksum_skip(const void *buf, size_t len, size_t offset)
{
	const uint8_t *bytes = buf;
	uint32_t hash = 2166136261u;
	size_t i;

	for (i = 0; i < len; i++) {
		if (i >= offset && i < offset + sizeof(__le32))
			continue;
		hash ^= bytes[i];
		hash *= 16777619u;
	}
	return hash;
}

static uint32_t metadata_seed(uint64_t fs_generation, uint64_t block,
			      uint32_t tag)
{
	uint32_t hash = 2166136261u;

	hash = fnv1a_update(hash, &fs_generation, sizeof(fs_generation));
	hash = fnv1a_update(hash, &block, sizeof(block));
	return fnv1a_update(hash, &tag, sizeof(tag));
}

static uint32_t metadata_checksum_skip(const void *buf, size_t len,
				       size_t offset, uint32_t seed)
{
	const uint8_t *bytes = buf;
	size_t i;

	for (i = 0; i < len; i++) {
		if (i >= offset && i < offset + sizeof(__le32))
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
		sb, sizeof(*sb), offsetof(struct cryexts_super_block, reserved),
		seed);
	stored = htole32(checksum);
	memcpy(sb->reserved, &stored, sizeof(stored));
}

static int mark_root_dir_slack(unsigned char *block, const char *marker)
{
	size_t marker_len = strlen(marker);
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
			if (rec_len < used + marker_len)
				return -1;
			memcpy(block + offset + rec_len - marker_len, marker,
			       marker_len);
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
	unsigned char payload_block[CRYEXTS_BLOCK_SIZE];
	struct cryexts_super_block *sb;
	struct cryexts_journal_v3_control *control;
	struct cryexts_journal_v3_descriptor *descriptor;
	struct cryexts_journal_v3_commit *commit;
	uint64_t journal_block;
	uint64_t journal_blocks;
	uint64_t ring_start;
	uint64_t ring_end;
	uint64_t home_blocknr;
	uint64_t last_sequence;
	uint64_t final_sequence;
	uint64_t final_descriptor = 0;
	uint64_t final_payload = 0;
	uint64_t final_commit = 0;
	uint32_t incompat;
	uint32_t fs_state;
	char marker[32];
	enum inject_scenario scenario = SCENARIO_COMMITTED;
	unsigned int txn;
	int fd;

	if (argc < 2 || argc > 3) {
		fprintf(stderr,
			"Usage: %s <image-or-device> [committed|uncommitted-tail]
",
			argv[0]);
		return 2;
	}
	if (argc == 3) {
		if (!strcmp(argv[2], "committed"))
			scenario = SCENARIO_COMMITTED;
		else if (!strcmp(argv[2], "uncommitted-tail"))
			scenario = SCENARIO_UNCOMMITTED_TAIL;
		else {
			fprintf(stderr, "unknown scenario: %s
", argv[2]);
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
	if (le32toh(sb->magic) != CRYEXTS_MAGIC ||
	    !(le32toh(sb->features_incompat) &
	      CRYEXTS_FEATURE_INCOMPAT_JOURNAL_V3) ||
	    !(le32toh(sb->features_incompat) &
	      CRYEXTS_FEATURE_INCOMPAT_JOURNAL_RING)) {
		fprintf(stderr,
			"cryexts_journal_v3_ring_inject: not a journal v3 ring filesystem
");
		close(fd);
		return 1;
	}

	journal_block = le64toh(sb->journal_block);
	journal_blocks = le64toh(sb->journal_blocks);
	if (!journal_block || journal_blocks < CRYEXTS_JOURNAL_V3_MIN_BLOCKS) {
		fprintf(stderr, "invalid journal area
");
		close(fd);
		return 1;
	}
	if (RING_TXNS * 3 > journal_blocks - 1) {
		fprintf(stderr, "journal area too small for %u ring transactions
",
			RING_TXNS);
		close(fd);
		return 1;
	}
	home_blocknr = le64toh(sb->root_dir_block);

	if (read_full(fd, control_block, sizeof(control_block),
		      journal_block * CRYEXTS_BLOCK_SIZE) < 0 ||
	    read_full(fd, home_block, sizeof(home_block),
		      home_blocknr * CRYEXTS_BLOCK_SIZE) < 0) {
		perror("read journal source blocks");
		close(fd);
		return 2;
	}
	control = (struct cryexts_journal_v3_control *)control_block;
	if (le32toh(control->magic) != CRYEXTS_JOURNAL_V3_MAGIC ||
	    le32toh(control->state) != CRYEXTS_JOURNAL_V3_STATE_IDLE) {
		fprintf(stderr, "journal is not clean
");
		close(fd);
		return 1;
	}
	ring_start = le64toh(control->ring_start);
	ring_end = le64toh(control->ring_end);
	if (!ring_start || ring_start >= ring_end ||
	    ring_end != journal_block + journal_blocks) {
		fprintf(stderr, "invalid journal ring range
");
		close(fd);
		return 1;
	}
	last_sequence = le64toh(control->last_sequence);
	final_sequence = last_sequence + RING_TXNS;

	incompat = le32toh(sb->features_incompat) |
		CRYEXTS_FEATURE_INCOMPAT_NEEDS_RECOVERY;
	fs_state = le32toh(sb->state) | CRYEXTS_FS_STATE_NEEDS_RECOVERY;
	fs_state &= ~CRYEXTS_FS_STATE_CLEAN;
	sb->features_incompat = htole32(incompat);
	sb->state = htole32(fs_state);
	sb->journal_sequence = htole64(final_sequence);
	set_super_checksum(sb);
	if (write_full(fd, super_block, sizeof(super_block), 0) < 0 ||
	    fsync(fd) < 0) {
		perror("write recovery superblock");
		close(fd);
		return 2;
	}

	for (txn = 0; txn < RING_TXNS; txn++) {
		uint64_t descriptor_blocknr = ring_start + txn * 3;
		uint64_t payload_blocknr = descriptor_blocknr + 1;
		uint64_t commit_blocknr = descriptor_blocknr + 2;
		uint64_t sequence = last_sequence + txn + 1;
		int committed = (scenario == SCENARIO_COMMITTED) ||
				(txn != RING_TXNS - 1);
		uint32_t payload_checksum;
		uint32_t descriptor_checksum;

		snprintf(marker, sizeof(marker), RING_MARKER_FMT, txn + 1);

		memcpy(payload_block, home_block, sizeof(payload_block));
		if (mark_root_dir_slack(payload_block, marker) < 0) {
			fprintf(stderr, "no root directory slack
");
			close(fd);
			return 1;
		}
		payload_checksum = fnv1a_update(2166136261u, payload_block,
						CRYEXTS_BLOCK_SIZE);

		memset(descriptor_block, 0, sizeof(descriptor_block));
		descriptor = (struct cryexts_journal_v3_descriptor *)descriptor_block;
		descriptor->magic = htole32(CRYEXTS_JOURNAL_V3_MAGIC);
		descriptor->layout_version = htole16(CRYEXTS_JOURNAL_V3_LAYOUT_VERSION);
		descriptor->block_type = htole16(CRYEXTS_JOURNAL_V3_BLOCK_DESCRIPTOR);
		descriptor->entry_count = htole32(1);
		descriptor->sequence = htole64(sequence);
		descriptor->payload_start = htole64(payload_blocknr);
		descriptor->commit_block = htole64(commit_blocknr);
		descriptor->entries[0].home_block = htole64(home_blocknr);
		descriptor->entries[0].payload_checksum = htole32(payload_checksum);
		descriptor_checksum = checksum_skip(
			descriptor_block, sizeof(descriptor_block),
			offsetof(struct cryexts_journal_v3_descriptor, checksum));
		descriptor->checksum = htole32(descriptor_checksum);

		memset(commit_block, 0, sizeof(commit_block));
		commit = (struct cryexts_journal_v3_commit *)commit_block;
		commit->magic = htole32(CRYEXTS_JOURNAL_V3_MAGIC);
		commit->layout_version = htole16(CRYEXTS_JOURNAL_V3_LAYOUT_VERSION);
		commit->block_type = htole16(CRYEXTS_JOURNAL_V3_BLOCK_COMMIT);
		commit->flags = htole32(committed ?
			CRYEXTS_JOURNAL_V3_FLAG_COMMITTED : 0);
		commit->entry_count = htole32(1);
		commit->descriptor_checksum = htole32(descriptor_checksum);
		commit->payload_checksum = htole32(payload_checksum);
		commit->sequence = htole64(sequence);
		commit->descriptor_block = htole64(descriptor_blocknr);
		commit->checksum = htole32(checksum_skip(
			commit_block, sizeof(commit_block),
			offsetof(struct cryexts_journal_v3_commit, checksum)));

		if (write_full(fd, payload_block, sizeof(payload_block),
			       payload_blocknr * CRYEXTS_BLOCK_SIZE) < 0 ||
		    write_full(fd, descriptor_block, sizeof(descriptor_block),
			       descriptor_blocknr * CRYEXTS_BLOCK_SIZE) < 0 ||
		    write_full(fd, commit_block, sizeof(commit_block),
			       commit_blocknr * CRYEXTS_BLOCK_SIZE) < 0) {
			perror("write ring transaction");
			close(fd);
			return 2;
		}

		final_descriptor = descriptor_blocknr;
		final_payload = payload_blocknr;
		final_commit = commit_blocknr;
	}

	memset(control_block, 0, sizeof(control_block));
	control = (struct cryexts_journal_v3_control *)control_block;
	control->magic = htole32(CRYEXTS_JOURNAL_V3_MAGIC);
	control->layout_version = htole16(CRYEXTS_JOURNAL_V3_LAYOUT_VERSION);
	control->block_type = htole16(CRYEXTS_JOURNAL_V3_BLOCK_CONTROL);
	control->state = htole32(scenario == SCENARIO_COMMITTED ?
		CRYEXTS_JOURNAL_V3_STATE_COMMITTED :
		CRYEXTS_JOURNAL_V3_STATE_PREPARED);
	control->features = htole32(CRYEXTS_JOURNAL_V3_FEATURE_REDO |
		CRYEXTS_JOURNAL_V3_FEATURE_RING);
	control->last_sequence = htole64(scenario == SCENARIO_COMMITTED ?
		final_sequence : final_sequence - 1);
	control->active_sequence = htole64(scenario == SCENARIO_COMMITTED ?
		0 : final_sequence);
	control->checkpoint_sequence = htole64(last_sequence);
	control->descriptor_block = htole64(final_descriptor);
	control->payload_start = htole64(final_payload);
	control->payload_blocks = htole64(1);
	control->commit_block = htole64(final_commit);
	control->ring_start = htole64(ring_start);
	control->ring_end = htole64(ring_end);
	control->ring_head = htole64(ring_start + RING_TXNS * 3);
	control->ring_tail = htole64(ring_start);
	control->checksum = htole32(checksum_skip(
		control_block, sizeof(control_block),
		offsetof(struct cryexts_journal_v3_control, checksum)));
	if (write_full(fd, control_block, sizeof(control_block),
		       journal_block * CRYEXTS_BLOCK_SIZE) < 0 ||
	    fsync(fd) < 0) {
		perror("write ring control block");
		close(fd);
		return 2;
	}

	printf("Injected journal v3 ring %s scenario (%u transactions)
",
	       scenario == SCENARIO_COMMITTED ? "committed" : "uncommitted-tail",
	       RING_TXNS);
	printf("Home block: %llu
", (unsigned long long)home_blocknr);
	printf("Ring start: %llu
", (unsigned long long)ring_start);
	printf("Ring head: %llu
",
	       (unsigned long long)(ring_start + RING_TXNS * 3));
	printf("Final sequence: %llu
", (unsigned long long)final_sequence);
	close(fd);
	return 0;
}
