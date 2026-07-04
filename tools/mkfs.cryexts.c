// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "../cryexts_fs.h"

#define CRYEXTS_META_TAG_SUPER 0x53555052U
#define CRYEXTS_META_TAG_GROUP 0x47525044U
#define CRYEXTS_META_TAG_POLICY 0x504f4c59U

static uint32_t fnv1a_bytes(const void *data, size_t len, uint32_t seed)
{
	const unsigned char *p = data;
	uint32_t hash = seed;

	while (len--) {
		hash ^= *p++;
		hash *= 16777619u;
	}
	return hash;
}

static void derive_salted_key(const char *key, const uint8_t salt[CRYEXTS_SALT_LEN],
			      uint8_t derived[CRYEXTS_DERIVED_KEY_LEN])
{
	unsigned int round;
	size_t key_len = strlen(key);

	for (round = 0; round < CRYEXTS_DERIVED_KEY_LEN / sizeof(uint32_t); round++) {
		uint32_t hash;
		uint8_t round_byte = (uint8_t)round;

		hash = fnv1a_bytes(key, key_len,
				   2166136261u ^ (0x9e3779b9u * (round + 1)));
		hash = fnv1a_bytes(salt, CRYEXTS_SALT_LEN, hash ^ 0x85ebca6bu);
		hash = fnv1a_bytes(&round_byte, sizeof(round_byte), hash ^ 0xc2b2ae35u);

		derived[round * 4 + 0] = (uint8_t)(hash & 0xff);
		derived[round * 4 + 1] = (uint8_t)((hash >> 8) & 0xff);
		derived[round * 4 + 2] = (uint8_t)((hash >> 16) & 0xff);
		derived[round * 4 + 3] = (uint8_t)((hash >> 24) & 0xff);
	}
}

static uint32_t key_verifier(const uint8_t *derived, size_t len)
{
	return fnv1a_bytes(derived, len, 2166136261u);
}

static int fill_random(uint8_t *buf, size_t len)
{
	FILE *fp = fopen("/dev/urandom", "rb");

	if (!fp)
		return -1;
	if (fread(buf, 1, len, fp) != len) {
		fclose(fp);
		return -1;
	}
	fclose(fp);
	return 0;
}

static void ensure_nonzero_salt(uint8_t *salt, size_t len)
{
	size_t i;
	int all_zero = 1;

	for (i = 0; i < len; i++) {
		if (salt[i]) {
			all_zero = 0;
			break;
		}
	}
	if (all_zero)
		salt[0] = 1;
}

static void ensure_nonzero_uuid(uint8_t *uuid, size_t len, uint64_t seed)
{
	size_t i;
	int all_zero = 1;

	for (i = 0; i < len; i++) {
		if (uuid[i]) {
			all_zero = 0;
			break;
		}
	}
	if (!all_zero)
		return;
	for (i = 0; i < len; i++)
		uuid[i] = (uint8_t)((seed + i * 29) & 0xff);
}

static void fill_policy_context(uint8_t *context, size_t len, uint64_t seed,
				uint32_t policy_id)
{
	uint64_t state = seed ^ ((uint64_t)policy_id << 32) ^
			 0x9e3779b97f4a7c15ULL;
	size_t i;

	for (i = 0; i < len; i++) {
		state = state * 6364136223846793005ULL + 1;
		context[i] = (uint8_t)(state >> 33);
	}
}

static uint32_t metadata_fnv1a_bytes(const void *data, size_t len, uint32_t hash)
{
	const unsigned char *p = data;
	size_t i;

	for (i = 0; i < len; i++) {
		hash ^= p[i];
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

static uint32_t metadata_checksum_skip(const void *data, size_t len,
				       size_t skip_offset, size_t skip_len,
				       uint32_t seed)
{
	const unsigned char *p = data;
	uint32_t hash = seed;
	size_t i;

	for (i = 0; i < len; i++) {
		if (i >= skip_offset && i < skip_offset + skip_len)
			continue;
		hash ^= p[i];
		hash *= 16777619u;
	}
	return hash;
}

static uint32_t journal_checksum_skip(const void *data, size_t len,
				      size_t skip_offset, size_t skip_len)
{
	const unsigned char *p = data;
	uint32_t hash = 2166136261u;
	size_t i;

	for (i = 0; i < len; i++) {
		if (i >= skip_offset && i < skip_offset + skip_len)
			continue;
		hash ^= p[i];
		hash *= 16777619u;
	}
	return hash;
}

static void set_journal_v2_control(struct cryexts_journal_v2_control *jc,
				   uint64_t journal_block,
				   uint64_t journal_blocks)
{
	uint32_t checksum;

	memset(jc, 0, CRYEXTS_BLOCK_SIZE);
	jc->magic = htole32(CRYEXTS_JOURNAL_V2_MAGIC);
	jc->layout_version = htole16(CRYEXTS_JOURNAL_V2_LAYOUT_VERSION);
	jc->block_type = htole16(CRYEXTS_JOURNAL_V2_BLOCK_CONTROL);
	jc->features = htole32(CRYEXTS_JOURNAL_V2_FEATURE_BASELINE);
	jc->last_sequence = htole64(0);
	jc->active_sequence = htole64(0);
	jc->tail_sequence = htole64(0);
	jc->checkpoint_sequence = htole64(0);
	jc->descriptor_block = htole64(journal_block + 1);
	jc->payload_start = htole64(journal_block + 2);
	jc->payload_blocks = htole64(journal_blocks - 3);
	jc->commit_block = htole64(journal_block + journal_blocks - 1);
	checksum = journal_checksum_skip(
		jc, CRYEXTS_BLOCK_SIZE,
		offsetof(struct cryexts_journal_v2_control, checksum),
		sizeof(jc->checksum));
	jc->checksum = htole32(checksum);
}

static void set_journal_v2_descriptor_empty(struct cryexts_journal_v2_descriptor *jd,
					    uint64_t payload_start,
					    uint64_t commit_block)
{
	uint32_t checksum;

	memset(jd, 0, CRYEXTS_BLOCK_SIZE);
	jd->magic = htole32(CRYEXTS_JOURNAL_V2_MAGIC);
	jd->layout_version = htole16(CRYEXTS_JOURNAL_V2_LAYOUT_VERSION);
	jd->block_type = htole16(CRYEXTS_JOURNAL_V2_BLOCK_DESCRIPTOR);
	jd->entry_count = htole32(0);
	jd->sequence = htole64(0);
	jd->payload_start = htole64(payload_start);
	jd->commit_block = htole64(commit_block);
	checksum = journal_checksum_skip(
		jd, CRYEXTS_BLOCK_SIZE,
		offsetof(struct cryexts_journal_v2_descriptor, checksum),
		sizeof(jd->checksum));
	jd->checksum = htole32(checksum);
}

static void set_journal_v2_commit_empty(struct cryexts_journal_v2_commit *jc,
					uint64_t descriptor_block)
{
	uint32_t checksum;

	memset(jc, 0, CRYEXTS_BLOCK_SIZE);
	jc->magic = htole32(CRYEXTS_JOURNAL_V2_MAGIC);
	jc->layout_version = htole16(CRYEXTS_JOURNAL_V2_LAYOUT_VERSION);
	jc->block_type = htole16(CRYEXTS_JOURNAL_V2_BLOCK_COMMIT);
	jc->entry_count = htole32(0);
	jc->sequence = htole64(0);
	jc->descriptor_block = htole64(descriptor_block);
	checksum = journal_checksum_skip(
		jc, CRYEXTS_BLOCK_SIZE,
		offsetof(struct cryexts_journal_v2_commit, checksum),
		sizeof(jc->checksum));
	jc->checksum = htole32(checksum);
}

static void set_super_checksum(struct cryexts_super_block *sb)
{
	uint32_t seed;
	uint32_t checksum;
	__le32 stored;

	if (le64toh(sb->metadata_csum_type) != CRYEXTS_METADATA_CSUM_FNV1A32)
		return;

	seed = metadata_seed(le64toh(sb->fs_generation), 0, CRYEXTS_META_TAG_SUPER);
	checksum = metadata_checksum_skip(
		sb, sizeof(*sb),
		offsetof(struct cryexts_super_block, reserved), sizeof(__le32),
		seed);
	stored = htole32(checksum);
	memcpy(sb->reserved, &stored, sizeof(stored));
}

static void set_group_checksum(const struct cryexts_super_block *sb,
			       struct cryexts_group_desc *group)
{
	uint32_t seed;
	uint32_t checksum;
	__le32 stored;

	if (le64toh(sb->metadata_csum_type) != CRYEXTS_METADATA_CSUM_FNV1A32)
		return;

	seed = metadata_seed(le64toh(sb->fs_generation),
			     le64toh(group->group_start), CRYEXTS_META_TAG_GROUP);
	checksum = metadata_checksum_skip(
		group, sizeof(*group),
		offsetof(struct cryexts_group_desc, reserved), sizeof(__le32),
		seed);
	stored = htole32(checksum);
	memcpy(group->reserved, &stored, sizeof(stored));
}

static void set_policy_table_checksum(const struct cryexts_super_block *sb,
				      uint64_t block,
				      struct cryexts_policy_table_block *pt)
{
	uint32_t seed;
	uint32_t checksum;
	__le32 stored;

	if (le64toh(sb->metadata_csum_type) != CRYEXTS_METADATA_CSUM_FNV1A32)
		return;

	seed = metadata_seed(le64toh(sb->fs_generation), block,
			     CRYEXTS_META_TAG_POLICY);
	checksum = metadata_checksum_skip(
		pt, CRYEXTS_BLOCK_SIZE,
		offsetof(struct cryexts_policy_table_block, reserved), sizeof(__le32),
		seed);
	stored = htole32(checksum);
	memcpy(pt->reserved, &stored, sizeof(stored));
}

static void bitmap_set(unsigned char *bitmap, uint64_t bit)
{
	bitmap[bit / 8] |= (unsigned char)(1U << (bit % 8));
}

static uint64_t min_u64(uint64_t a, uint64_t b)
{
	return a < b ? a : b;
}

static uint64_t div_round_up_u64(uint64_t a, uint64_t b)
{
	return (a + b - 1) / b;
}

static int write_full(int fd, const void *buf, size_t len, off_t off)
{
	const char *p = buf;

	while (len > 0) {
		ssize_t written = pwrite(fd, p, len, off);

		if (written < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (written == 0) {
			errno = EIO;
			return -1;
		}
		p += written;
		off += written;
		len -= written;
	}
	return 0;
}

static uint64_t get_device_size(int fd)
{
	struct stat st;
	uint64_t bytes = 0;

	if (ioctl(fd, BLKGETSIZE64, &bytes) == 0)
		return bytes;
	if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
		return st.st_size;
	return 0;
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [-f] [-G] [-X] [-A] [-I] [-O] [-T] [-M] [-J] [-L label] [-P policy_id] [-E key] <image-or-device> [size_MB]\n",
		prog);
}

int main(int argc, char **argv)
{
	const char *target;
	const char *label = "cryexts";
	const char *key = NULL;
	uint64_t requested_size = 0;
	uint64_t device_size;
	uint64_t blocks_count;
	uint64_t inodes_count;
	uint64_t now = (uint64_t)time(NULL);
	unsigned char block[CRYEXTS_BLOCK_SIZE];
	uint8_t salt[CRYEXTS_SALT_LEN];
	uint8_t derived[CRYEXTS_DERIVED_KEY_LEN];
	uint8_t uuid[CRYEXTS_UUID_LEN];
	struct cryexts_super_block *sb;
	struct cryexts_super_block checksum_sb;
	struct cryexts_inode *root_inode;
	struct cryexts_dir_entry *de;
	struct cryexts_group_desc *groups;
	unsigned int dot_len;
	int force = 0;
	int use_block_groups = 0;
	int use_extents = 0;
	int use_xattrs = 0;
	int use_dir_index = 0;
	int use_orphan_list = 0;
	int use_policy_table = 0;
	int use_metadata_csum = 0;
	int use_journal_v2 = 0;
	int fd;
	int opt;
	uint32_t default_policy_id = 0;
	uint32_t fs_version = CRYEXTS_VERSION;
	uint64_t group_count = 1;
	uint64_t blocks_per_group = 0; /* assigned after blocks_count is known */
	uint64_t inodes_per_group = CRYEXTS_DEFAULT_INODES_PER_GROUP;
	uint64_t gdt_blocks = 1;
	uint64_t root_group = 0;
	uint64_t root_inode_table_start;
	uint64_t root_block_bitmap_block;
	uint64_t root_inode_bitmap_block;
	uint64_t root_dir_block;
	uint64_t first_data_block;
	uint64_t total_used_blocks = 0;
	uint64_t journal_block = 0;
	uint64_t journal_blocks = 0;
	uint64_t policy_table_block = 0;
	uint16_t policy_entry_count = 0;

	while ((opt = getopt(argc, argv, "fGXAIOTMJP:L:E:h")) != -1) {
		switch (opt) {
		case 'f':
			force = 1;
			break;
		case 'G':
			use_block_groups = 1;
			break;
		case 'X':
			use_extents = 1;
			break;
		case 'A':
			use_xattrs = 1;
			break;
		case 'I':
			use_dir_index = 1;
			break;
		case 'O':
			use_orphan_list = 1;
			break;
		case 'T':
			use_policy_table = 1;
			break;
		case 'M':
			use_metadata_csum = 1;
			break;
		case 'J':
			use_journal_v2 = 1;
			use_block_groups = 1;
			fs_version = CRYEXTS_VERSION_V6;
			break;
		case 'P':
			default_policy_id = (uint32_t)strtoul(optarg, NULL, 10);
			use_xattrs = 1;
			use_policy_table = 1;
			break;
		case 'L':
			label = optarg;
			break;
		case 'E':
			key = optarg;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (optind >= argc) {
		usage(argv[0]);
		return 1;
	}

	target = argv[optind++];
	if (optind < argc)
		requested_size = strtoull(argv[optind], NULL, 10) * 1024ULL * 1024ULL;

	if (key && (!*key || strlen(key) >= CRYEXTS_KEY_MAX)) {
		fprintf(stderr, "Encryption key length must be 1..%u bytes\n",
			CRYEXTS_KEY_MAX - 1);
		return 1;
	}

	if (!force) {
		fprintf(stderr, "Refusing to format without -f: %s\n", target);
		return 1;
	}

	fd = open(target, O_RDWR | O_CREAT, 0666);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	if (requested_size && ftruncate(fd, requested_size) < 0) {
		perror("ftruncate");
		close(fd);
		return 1;
	}

	device_size = get_device_size(fd);
	if (device_size < CRYEXTS_BLOCK_SIZE * CRYEXTS_FIRST_FREE_DATA_BLOCK) {
		fprintf(stderr, "Device is too small for CRYEXTS v3 layout\n");
		close(fd);
		return 1;
	}

	blocks_count = device_size / CRYEXTS_BLOCK_SIZE;
	if (!use_block_groups)
		inodes_count = CRYEXTS_INODE_TABLE_BLOCKS *
			       (CRYEXTS_BLOCK_SIZE / sizeof(struct cryexts_inode));
	if (!use_block_groups && blocks_count > CRYEXTS_BLOCK_SIZE * 8ULL) {
		fprintf(stderr,
			"Device is too large for CRYEXTS v3 single-block bitmap\n");
		close(fd);
		return 1;
	}
	if (use_block_groups) {
		blocks_per_group = min_u64(blocks_count,
					   CRYEXTS_DEFAULT_BLOCKS_PER_GROUP);
		group_count = div_round_up_u64(blocks_count, blocks_per_group);
		inodes_count = group_count * inodes_per_group;
		if (group_count * sizeof(struct cryexts_group_desc) >
		    CRYEXTS_BLOCK_SIZE) {
			fprintf(stderr,
				"Current v4.1 mkfs supports only one GDT block\n");
			close(fd);
			return 1;
		}
	}

	root_block_bitmap_block = use_block_groups ? 2 : CRYEXTS_BLOCK_BITMAP_BLOCK;
	root_inode_bitmap_block = use_block_groups ? 3 : CRYEXTS_INODE_BITMAP_BLOCK;
	root_inode_table_start = use_block_groups ? 4 : CRYEXTS_INODE_TABLE_START;
	root_dir_block = use_block_groups ?
		root_inode_table_start + CRYEXTS_DEFAULT_INODE_TABLE_BLOCKS_PER_GROUP :
		CRYEXTS_ROOT_DIR_BLOCK;
	first_data_block = root_dir_block;
	if (use_policy_table) {
		policy_table_block = root_dir_block + 1;
		policy_entry_count = CRYEXTS_POLICY_TABLE_MAX_ENTRIES;
		if (default_policy_id >= policy_entry_count) {
			fprintf(stderr,
				"default policy id must be < %u when policy table is enabled\n",
				policy_entry_count);
			close(fd);
			return 1;
		}
		if (policy_table_block >= blocks_count) {
			fprintf(stderr, "Device is too small for policy table block\n");
			close(fd);
			return 1;
		}
	}
	if (use_block_groups) {
		uint64_t tail_group = group_count - 1;
		uint64_t tail_group_start = tail_group * blocks_per_group;
		uint64_t tail_group_blocks =
			min_u64(blocks_per_group, blocks_count - tail_group_start);
		uint64_t journal_room;

		journal_room = tail_group_blocks > (2 +
			CRYEXTS_DEFAULT_INODE_TABLE_BLOCKS_PER_GROUP) ?
			tail_group_blocks - (2 +
			CRYEXTS_DEFAULT_INODE_TABLE_BLOCKS_PER_GROUP) : 0;
		journal_blocks = min_u64(journal_room, CRYEXTS_DEFAULT_JOURNAL_BLOCKS);
		if (journal_blocks > 0)
			journal_block = tail_group_start + tail_group_blocks - journal_blocks;
	}
	if (use_journal_v2) {
		if (!journal_block || journal_blocks < CRYEXTS_JOURNAL_V2_MIN_BLOCKS) {
			fprintf(stderr,
				"journal v2 requires at least %u reserved journal blocks\n",
				CRYEXTS_JOURNAL_V2_MIN_BLOCKS);
			close(fd);
			return 1;
		}
	}

	memset(block, 0, sizeof(block));
	sb = (struct cryexts_super_block *)(block + CRYEXTS_SUPER_OFFSET);
	sb->magic = htole32(CRYEXTS_MAGIC);
	sb->version = htole32(fs_version);
	sb->block_size = htole32(CRYEXTS_BLOCK_SIZE);
	sb->inode_size = htole32(sizeof(struct cryexts_inode));
	sb->blocks_count = htole64(blocks_count);
	sb->inodes_count = htole64(inodes_count);
	sb->free_inodes_count = htole64(inodes_count - 1);
	sb->block_bitmap_block = htole64(root_block_bitmap_block);
	sb->inode_bitmap_block = htole64(root_inode_bitmap_block);
	sb->inode_table_start = htole64(root_inode_table_start);
	sb->inode_table_blocks = htole64(use_block_groups ?
		CRYEXTS_DEFAULT_INODE_TABLE_BLOCKS_PER_GROUP :
		CRYEXTS_INODE_TABLE_BLOCKS);
	sb->root_inode_block = htole64(root_inode_table_start);
	sb->root_dir_block = htole64(root_dir_block);
	sb->first_data_block = htole64(first_data_block);
	sb->next_ino = htole64(CRYEXTS_ROOT_INO + 1);
	sb->next_data_block = htole64(root_dir_block + 1 +
				      (policy_table_block ? 1 : 0));
	strncpy(sb->label, label, CRYEXTS_LABEL_LEN - 1);
	strncpy(sb->volume_name, label, CRYEXTS_VOLUME_NAME_LEN - 1);
	sb->features_compat = htole32(CRYEXTS_FEATURE_COMPAT_PREALLOC);
	sb->features_ro_compat = htole32(
		(use_metadata_csum ? CRYEXTS_FEATURE_RO_COMPAT_METADATA_CSUM : 0) |
		((fs_version >= CRYEXTS_VERSION_V6 && use_xattrs) ?
			 CRYEXTS_FEATURE_RO_COMPAT_LARGE_XATTR : 0));
	if (key) {
		if (fill_random(salt, sizeof(salt)) < 0) {
			for (size_t i = 0; i < sizeof(salt); i++)
				salt[i] = (uint8_t)((now + i * 17) & 0xff);
		}
		ensure_nonzero_salt(salt, sizeof(salt));
		derive_salted_key(key, salt, derived);
		sb->flags = htole32(CRYEXTS_SB_FLAG_ENCRYPTED);
		sb->key_hash = htole32(key_verifier(derived, sizeof(derived)));
		sb->encryption_flags = htole32(CRYEXTS_ENC_FLAG_DATA);
		sb->encryption_kdf = htole32(CRYEXTS_KDF_SALTED_FNV1A);
		sb->encryption_alg = htole32(CRYEXTS_ALG_AES_CTR);
		memcpy(sb->salt, salt, sizeof(salt));
		memset(derived, 0, sizeof(derived));
		memset(salt, 0, sizeof(salt));
	} else {
		sb->flags = htole32(0);
		sb->key_hash = htole32(0);
		sb->encryption_flags = htole32(0);
		sb->encryption_kdf = htole32(0);
		sb->encryption_alg = htole32(0);
		memset(sb->salt, 0, sizeof(sb->salt));
	}
	sb->features_incompat = htole32(
		(use_extents ? CRYEXTS_FEATURE_INCOMPAT_EXTENTS :
			       CRYEXTS_FEATURE_INCOMPAT_SINGLE_INDIRECT) |
		(use_block_groups ? CRYEXTS_FEATURE_INCOMPAT_BLOCK_GROUPS : 0) |
		(use_dir_index ? CRYEXTS_FEATURE_INCOMPAT_DIR_INDEX : 0) |
		(use_orphan_list ? CRYEXTS_FEATURE_INCOMPAT_ORPHAN_LIST : 0) |
		(use_policy_table ? CRYEXTS_FEATURE_INCOMPAT_POLICY_TABLE : 0) |
		(use_extents ? CRYEXTS_FEATURE_INCOMPAT_EXTENT_TREE : 0) |
		(use_journal_v2 ? CRYEXTS_FEATURE_INCOMPAT_JOURNAL_V2 : 0) |
		(use_xattrs ? (CRYEXTS_FEATURE_INCOMPAT_XATTR |
			       CRYEXTS_FEATURE_INCOMPAT_ENCRYPTION_POLICY) : 0));
	if (journal_blocks > 0)
		sb->features_compat = htole32(le32toh(sb->features_compat) |
					      CRYEXTS_FEATURE_COMPAT_HAS_JOURNAL);
	sb->state = htole32(CRYEXTS_FS_STATE_CLEAN);
	sb->mount_count = htole32(0);
	sb->max_mount_count = htole32(50);
	sb->default_encryption_policy = htole32(default_policy_id);
	sb->last_mount_time = htole64(0);
	sb->last_write_time = htole64(now);
	sb->last_check_time = htole64(now);
	sb->journal_block = htole64(journal_block);
	sb->journal_blocks = htole64(journal_blocks);
	sb->group_count = htole64(group_count);
	sb->blocks_per_group = htole64(blocks_per_group);
	sb->inodes_per_group = htole64(use_block_groups ? inodes_per_group : inodes_count);
	sb->group_desc_table_start = htole64(use_block_groups ? CRYEXTS_GDT_START_BLOCK : 0);
	sb->group_desc_table_blocks = htole64(use_block_groups ? gdt_blocks : 0);
	sb->orphan_head = htole64(0);
	sb->policy_table_block = htole64(policy_table_block);
	sb->dir_index_seed = htole64(use_dir_index ? now : 0);
	sb->metadata_csum_type = htole64(
		use_metadata_csum ? CRYEXTS_METADATA_CSUM_FNV1A32 : 0);
	sb->journal_sequence = htole64(0);
	sb->fs_generation = htole64(1);
	if (fill_random(uuid, sizeof(uuid)) < 0)
		memset(uuid, 0, sizeof(uuid));
	ensure_nonzero_uuid(uuid, sizeof(uuid), now);
	memcpy(sb->uuid, uuid, sizeof(uuid));

	if (use_block_groups) {
		total_used_blocks = 1 + (policy_table_block ? 1 : 0);
		for (uint64_t group = 0; group < group_count; group++) {
			uint64_t group_start = group * blocks_per_group;
			uint64_t data_start;

			if (group == root_group)
				data_start = first_data_block;
			else
				data_start = group_start + 2 +
					CRYEXTS_DEFAULT_INODE_TABLE_BLOCKS_PER_GROUP;
			total_used_blocks += data_start - group_start;
		}
		total_used_blocks += journal_blocks;
	} else {
		total_used_blocks = first_data_block + 1 +
				    (policy_table_block ? 1 : 0);
	}
	sb->free_blocks_count = htole64(blocks_count - total_used_blocks);
	set_super_checksum(sb);
	checksum_sb = *sb;

	if (write_full(fd, block, sizeof(block), 0) < 0) {
		perror("write superblock");
		close(fd);
		return 1;
	}

	memset(block, 0, sizeof(block));
	for (uint64_t i = 0; i < (use_block_groups ?
				  (root_dir_block + 1 + (policy_table_block ? 1 : 0)) :
				  (CRYEXTS_FIRST_FREE_DATA_BLOCK +
				   (policy_table_block ? 1 : 0))); i++)
		bitmap_set(block, i);
	if (write_full(fd, block, sizeof(block),
		       root_block_bitmap_block * CRYEXTS_BLOCK_SIZE) < 0) {
		perror("write block bitmap");
		close(fd);
		return 1;
	}

	memset(block, 0, sizeof(block));
	bitmap_set(block, CRYEXTS_ROOT_INO - 1);
	if (write_full(fd, block, sizeof(block),
		       root_inode_bitmap_block * CRYEXTS_BLOCK_SIZE) < 0) {
		perror("write inode bitmap");
		close(fd);
		return 1;
	}

	for (uint64_t i = 0; i < (use_block_groups ?
				   CRYEXTS_DEFAULT_INODE_TABLE_BLOCKS_PER_GROUP :
				   CRYEXTS_INODE_TABLE_BLOCKS); i++) {
		memset(block, 0, sizeof(block));
		if (write_full(fd, block, sizeof(block),
			       (root_inode_table_start + i) *
				       CRYEXTS_BLOCK_SIZE) < 0) {
			perror("clear inode table");
			close(fd);
			return 1;
		}
	}

	if (use_block_groups) {
		memset(block, 0, sizeof(block));
		groups = (struct cryexts_group_desc *)block;
		for (uint64_t group = 0; group < group_count; group++) {
			uint64_t group_start = group * blocks_per_group;
			uint64_t group_blocks = min_u64(blocks_per_group,
							blocks_count - group_start);
			uint64_t block_bitmap_block;
			uint64_t inode_bitmap_block;
			uint64_t inode_table_start;
			uint64_t data_start;
			uint32_t free_blocks =
				0;
			uint32_t free_inodes = (uint32_t)inodes_per_group;

			if (group == root_group) {
				block_bitmap_block = root_block_bitmap_block;
				inode_bitmap_block = root_inode_bitmap_block;
				inode_table_start = root_inode_table_start;
				data_start = root_dir_block + 1;
			} else {
				block_bitmap_block = group_start;
				inode_bitmap_block = group_start + 1;
				inode_table_start = group_start + 2;
				data_start = inode_table_start +
					CRYEXTS_DEFAULT_INODE_TABLE_BLOCKS_PER_GROUP;
			}
			if (group_blocks > data_start - group_start)
				free_blocks = (uint32_t)(group_blocks -
						 (data_start - group_start));
			if (journal_blocks > 0 && group == group_count - 1) {
				uint64_t journal_in_group = journal_blocks;

				if (journal_in_group > free_blocks)
					journal_in_group = free_blocks;
				free_blocks -= (uint32_t)journal_in_group;
			}
			if (group == root_group && policy_table_block &&
			    free_blocks > 0)
				free_blocks--;

			groups[group].group_start = htole64(group_start);
			groups[group].blocks_count = htole64(group_blocks);
			groups[group].block_bitmap_block = htole64(block_bitmap_block);
			groups[group].inode_bitmap_block = htole64(inode_bitmap_block);
			groups[group].inode_table_start = htole64(inode_table_start);
			groups[group].inode_table_blocks =
				htole32(CRYEXTS_DEFAULT_INODE_TABLE_BLOCKS_PER_GROUP);
			groups[group].free_blocks_count = htole32(free_blocks);
			groups[group].free_inodes_count = htole32(free_inodes);
			groups[group].used_dirs_count = htole32(0);
			if (group == root_group) {
				groups[group].free_inodes_count =
					htole32(free_inodes - 1);
				groups[group].used_dirs_count = htole32(1);
			}
			set_group_checksum(&checksum_sb, &groups[group]);
		}
		if (write_full(fd, block, sizeof(block),
			       CRYEXTS_GDT_START_BLOCK * CRYEXTS_BLOCK_SIZE) < 0) {
			perror("write group desc table");
			close(fd);
			return 1;
		}

		for (uint64_t group = 1; group < group_count; group++) {
			uint64_t group_start = group * blocks_per_group;
			uint64_t group_blocks = min_u64(blocks_per_group,
							blocks_count - group_start);
			uint64_t inode_bitmap_block = group_start + 1;
			uint64_t inode_table_start = group_start + 2;
			uint64_t first_data = inode_table_start +
				CRYEXTS_DEFAULT_INODE_TABLE_BLOCKS_PER_GROUP;
			uint64_t last_data_exclusive = group_start + group_blocks;

			memset(block, 0, sizeof(block));
			for (uint64_t i = 0; i < first_data - group_start; i++)
				bitmap_set(block, i);
			if (journal_blocks > 0 && group == group_count - 1) {
				for (uint64_t blk = journal_block; blk < journal_block + journal_blocks &&
				     blk < last_data_exclusive; blk++)
					bitmap_set(block, blk - group_start);
			}
			if (write_full(fd, block, sizeof(block),
				       group_start * CRYEXTS_BLOCK_SIZE) < 0) {
				perror("write group block bitmap");
				close(fd);
				return 1;
			}

			memset(block, 0, sizeof(block));
			if (write_full(fd, block, sizeof(block),
				       inode_bitmap_block * CRYEXTS_BLOCK_SIZE) < 0) {
				perror("write group inode bitmap");
				close(fd);
				return 1;
			}

			for (uint64_t i = 0;
			     i < CRYEXTS_DEFAULT_INODE_TABLE_BLOCKS_PER_GROUP; i++) {
				memset(block, 0, sizeof(block));
				if (write_full(fd, block, sizeof(block),
					       (inode_table_start + i) *
						       CRYEXTS_BLOCK_SIZE) < 0) {
					perror("clear group inode table");
					close(fd);
					return 1;
				}
			}
			(void)group_blocks;
		}
	}

	memset(block, 0, sizeof(block));
	root_inode = (struct cryexts_inode *)block;
	root_inode->mode = htole16(0040755);
	root_inode->links_count = htole16(2);
	root_inode->uid = htole32(0);
	root_inode->gid = htole32(0);
	root_inode->size = htole64(CRYEXTS_BLOCK_SIZE);
	root_inode->blocks = htole64(CRYEXTS_BLOCK_SIZE / 512);
	root_inode->atime = htole64(now);
	root_inode->ctime = htole64(now);
	root_inode->mtime = htole64(now);
	root_inode->block[0] = htole64(root_dir_block);
	((struct cryexts_inode_extra *)
	 (root_inode->reserved + sizeof(root_inode->reserved) -
	  sizeof(struct cryexts_inode_extra)))->encryption_policy_id =
		htole32(default_policy_id);

	if (write_full(fd, block, sizeof(block),
		       root_inode_table_start * CRYEXTS_BLOCK_SIZE) < 0) {
		perror("write root inode");
		close(fd);
		return 1;
	}

	memset(block, 0, sizeof(block));
	de = (struct cryexts_dir_entry *)block;
	dot_len = cryexts_dir_rec_len(1);
	de->inode = htole64(CRYEXTS_ROOT_INO);
	de->rec_len = htole16(dot_len);
	de->name_len = 1;
	de->file_type = CRYEXTS_FT_DIR;
	memcpy(de->name, ".", 1);

	de = (struct cryexts_dir_entry *)(block + dot_len);
	de->inode = htole64(CRYEXTS_ROOT_INO);
	de->rec_len = htole16(CRYEXTS_BLOCK_SIZE - dot_len);
	de->name_len = 2;
	de->file_type = CRYEXTS_FT_DIR;
	memcpy(de->name, "..", 2);

	if (write_full(fd, block, sizeof(block),
		       root_dir_block * CRYEXTS_BLOCK_SIZE) < 0) {
		perror("write root directory");
		close(fd);
		return 1;
	}

	if (policy_table_block) {
		struct cryexts_policy_table_block *pt;
		uint16_t i;

		memset(block, 0, sizeof(block));
		pt = (struct cryexts_policy_table_block *)block;
		pt->magic = htole32(CRYEXTS_POLICY_TABLE_MAGIC);
		pt->entry_count = htole16(policy_entry_count);
		for (i = 0; i < policy_entry_count; i++) {
			struct cryexts_policy_entry *entry = &pt->entries[i];

			entry->policy_id = htole32(i);
			entry->flags = htole32(0);
			fill_policy_context(entry->context, sizeof(entry->context),
					    now, i);
		}
		set_policy_table_checksum(&checksum_sb, policy_table_block, pt);
		if (write_full(fd, block, sizeof(block),
			       policy_table_block * CRYEXTS_BLOCK_SIZE) < 0) {
			perror("write policy table");
			close(fd);
			return 1;
		}
	}

	if (journal_blocks > 0) {
		uint64_t blk;

		memset(block, 0, sizeof(block));
		for (blk = journal_block; blk < journal_block + journal_blocks; blk++) {
			if (write_full(fd, block, sizeof(block),
				       blk * CRYEXTS_BLOCK_SIZE) < 0) {
				perror("clear journal area");
				close(fd);
				return 1;
			}
		}
		if (use_journal_v2) {
			struct cryexts_journal_v2_control *jc;
			struct cryexts_journal_v2_descriptor *jd;
			struct cryexts_journal_v2_commit *jcommit;

			memset(block, 0, sizeof(block));
			jc = (struct cryexts_journal_v2_control *)block;
			set_journal_v2_control(jc, journal_block, journal_blocks);
			if (write_full(fd, block, sizeof(block),
				       journal_block * CRYEXTS_BLOCK_SIZE) < 0) {
				perror("write journal v2 control block");
				close(fd);
				return 1;
			}

			memset(block, 0, sizeof(block));
			jd = (struct cryexts_journal_v2_descriptor *)block;
			set_journal_v2_descriptor_empty(jd, journal_block + 2,
							journal_block + journal_blocks - 1);
			if (write_full(fd, block, sizeof(block),
				       (journal_block + 1) * CRYEXTS_BLOCK_SIZE) < 0) {
				perror("write journal v2 descriptor block");
				close(fd);
				return 1;
			}

			memset(block, 0, sizeof(block));
			jcommit = (struct cryexts_journal_v2_commit *)block;
			set_journal_v2_commit_empty(jcommit, journal_block + 1);
			if (write_full(fd, block, sizeof(block),
				       (journal_block + journal_blocks - 1) *
					       CRYEXTS_BLOCK_SIZE) < 0) {
				perror("write journal v2 commit block");
				close(fd);
				return 1;
			}
		}
	}

	if (fsync(fd) < 0) {
		perror("fsync");
		close(fd);
		return 1;
	}

	printf("Created CRYEXTS filesystem on %s\n", target);
	printf("Version: %u\n", fs_version);
	printf("Block size: %u\n", CRYEXTS_BLOCK_SIZE);
	printf("Blocks: %llu\n", (unsigned long long)blocks_count);
	printf("Inodes: %llu\n", (unsigned long long)inodes_count);
	printf("Block bitmap: %llu\n", (unsigned long long)root_block_bitmap_block);
	printf("Inode bitmap: %llu\n", (unsigned long long)root_inode_bitmap_block);
	printf("First data block: %llu\n", (unsigned long long)first_data_block);
	printf("First free data block: %llu\n",
	       (unsigned long long)(root_dir_block + 1 +
				    (policy_table_block ? 1 : 0)));
	printf("Inode table blocks: %u\n",
	       use_block_groups ? CRYEXTS_DEFAULT_INODE_TABLE_BLOCKS_PER_GROUP :
				  CRYEXTS_INODE_TABLE_BLOCKS);
	printf("Groups: %llu\n", (unsigned long long)group_count);
	if (journal_blocks > 0)
		printf("Journal: start=%llu blocks=%llu\n",
		       (unsigned long long)journal_block,
		       (unsigned long long)journal_blocks);
	if (journal_blocks > 0)
		printf("Journal format: %s\n",
		       use_journal_v2 ? "v2" : "v1");
	printf("Prealloc: enabled\n");
	printf("Dir index: %s\n", use_dir_index ? "enabled" : "disabled");
	printf("Orphan list: %s\n", use_orphan_list ? "enabled" : "disabled");
	printf("Policy table: %s\n", use_policy_table ? "enabled" : "disabled");
	if (policy_table_block)
		printf("Policy table block: %llu entries=%u default=%u\n",
		       (unsigned long long)policy_table_block,
		       policy_entry_count, default_policy_id);
	printf("Metadata checksum: %s\n",
	       use_metadata_csum ? "enabled" : "disabled");
	printf("Filesystem state: clean\n");
	printf("Filesystem type: cryexts\n");
	if (key)
		printf("Encrypted: yes\n");
	else
		printf("Encrypted: no\n");

	close(fd);
	return 0;
}
