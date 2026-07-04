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
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../cryexts_fs.h"

#define CRYEXTS_META_TAG_SUPER 0x53555052U
#define CRYEXTS_META_TAG_GROUP 0x47525044U
#define CRYEXTS_META_TAG_POLICY 0x504f4c59U
#define CRYEXTS_META_TAG_DIRIDX 0x44495258U
#define CRYEXTS_META_TAG_EXTOVF 0x45584f46U

static int errors;
static int repair_mode;
static int warnings_found;

static uint64_t min_u64(uint64_t a, uint64_t b)
{
	return a < b ? a : b;
}

static uint64_t div_round_up_u64(uint64_t a, uint64_t b)
{
	return (a + b - 1) / b;
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

static void report(const char *msg)
{
	fprintf(stderr, "cryextsck: %s\n", msg);
	errors++;
}

static void warn_msg(const char *msg)
{
	fprintf(stderr, "cryextsck: %s\n", msg);
	warnings_found++;
}

static uint64_t inodes_per_block(void)
{
	return CRYEXTS_BLOCK_SIZE / sizeof(struct cryexts_inode);
}

static int has_block_groups(const struct cryexts_super_block *sb)
{
	return !!(le32toh(sb->features_incompat) &
		  CRYEXTS_FEATURE_INCOMPAT_BLOCK_GROUPS);
}

static int has_journal(const struct cryexts_super_block *sb)
{
	return !!(le32toh(sb->features_compat) &
		  CRYEXTS_FEATURE_COMPAT_HAS_JOURNAL);
}

static int has_journal_v2(const struct cryexts_super_block *sb)
{
	return has_journal(sb) &&
	       !!(le32toh(sb->features_incompat) &
		  CRYEXTS_FEATURE_INCOMPAT_JOURNAL_V2);
}

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

static uint32_t journal_checksum_skip(const void *buf, size_t len,
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

static uint32_t journal_v2_checksum(const void *buf, size_t len,
				    size_t checksum_offset)
{
	return journal_checksum_skip(buf, len, checksum_offset, sizeof(__le32));
}

static uint64_t journal_v2_descriptor_block(const struct cryexts_super_block *sb)
{
	return le64toh(sb->journal_block) + 1;
}

static uint64_t journal_v2_payload_start(const struct cryexts_super_block *sb)
{
	return le64toh(sb->journal_block) + 2;
}

static uint64_t journal_v2_payload_area_blocks(const struct cryexts_super_block *sb)
{
	uint64_t journal_blocks = le64toh(sb->journal_blocks);

	if (journal_blocks < CRYEXTS_JOURNAL_V2_MIN_BLOCKS)
		return 0;
	return journal_blocks - 3;
}

static uint64_t journal_v2_commit_block(const struct cryexts_super_block *sb)
{
	return le64toh(sb->journal_block) + le64toh(sb->journal_blocks) - 1;
}

static uint32_t journal_v2_payload_capacity(const struct cryexts_super_block *sb)
{
	uint64_t journal_blocks = le64toh(sb->journal_blocks);

	if (journal_blocks < CRYEXTS_JOURNAL_V2_MIN_BLOCKS)
		return 0;
	return (uint32_t)min_u64(CRYEXTS_JOURNAL_V2_MAX_ENTRIES,
				 journal_blocks - 3);
}

static int metadata_csum_enabled(const struct cryexts_super_block *sb)
{
	return le32toh(sb->version) >= CRYEXTS_VERSION_V5 &&
	       !!(le32toh(sb->features_ro_compat) &
		  CRYEXTS_FEATURE_RO_COMPAT_METADATA_CSUM) &&
	       le64toh(sb->metadata_csum_type) ==
		       CRYEXTS_METADATA_CSUM_FNV1A32;
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

static uint32_t super_expected_checksum(const struct cryexts_super_block *sb)
{
	uint32_t seed;

	seed = metadata_seed(le64toh(sb->fs_generation), 0, CRYEXTS_META_TAG_SUPER);
	return metadata_checksum_skip(
		sb, sizeof(*sb),
		offsetof(struct cryexts_super_block, reserved), sizeof(__le32),
		seed);
}

static uint32_t group_expected_checksum(const struct cryexts_super_block *sb,
					const struct cryexts_group_desc *group)
{
	uint32_t seed;

	seed = metadata_seed(le64toh(sb->fs_generation),
			     le64toh(group->group_start), CRYEXTS_META_TAG_GROUP);
	return metadata_checksum_skip(
		group, sizeof(*group),
		offsetof(struct cryexts_group_desc, reserved), sizeof(__le32),
		seed);
}

static uint32_t policy_expected_checksum(const struct cryexts_super_block *sb,
					 uint64_t block,
					 const struct cryexts_policy_table_block *pt)
{
	uint32_t seed;

	seed = metadata_seed(le64toh(sb->fs_generation), block,
			     CRYEXTS_META_TAG_POLICY);
	return metadata_checksum_skip(
		pt, CRYEXTS_BLOCK_SIZE,
		offsetof(struct cryexts_policy_table_block, reserved), sizeof(__le32),
		seed);
}

static uint32_t dir_index_expected_checksum(const struct cryexts_super_block *sb,
					    uint64_t block,
					    const struct cryexts_dir_index_block *index)
{
	uint32_t seed;

	seed = metadata_seed(le64toh(sb->fs_generation), block,
			     CRYEXTS_META_TAG_DIRIDX);
	return metadata_checksum_skip(
		index, CRYEXTS_BLOCK_SIZE,
		offsetof(struct cryexts_dir_index_block, reserved), sizeof(__le32),
		seed);
}

static uint32_t extent_overflow_expected_checksum(
	const struct cryexts_super_block *sb, uint64_t block, const void *buf)
{
	uint32_t seed;

	seed = metadata_seed(le64toh(sb->fs_generation), block,
			     CRYEXTS_META_TAG_EXTOVF);
	return metadata_fnv1a_bytes(buf, CRYEXTS_BLOCK_SIZE, seed);
}

static int journal_block_is_internal(const struct cryexts_super_block *sb,
				     uint64_t block)
{
	uint64_t journal_start = le64toh(sb->journal_block);
	uint64_t journal_end = journal_start + le64toh(sb->journal_blocks);

	return block >= journal_start && block < journal_end;
}

static uint64_t max_inodes(const struct cryexts_super_block *sb)
{
	if (has_block_groups(sb))
		return le64toh(sb->inodes_count);
	return le64toh(sb->inode_table_blocks) * inodes_per_block();
}

static uint64_t group_inode_limit(const struct cryexts_super_block *sb,
				  uint64_t group)
{
	uint64_t inodes_count = le64toh(sb->inodes_count);
	uint64_t inodes_per_group = le64toh(sb->inodes_per_group);
	uint64_t base = group * inodes_per_group;

	if (!has_block_groups(sb))
		return inodes_count;
	if (base >= inodes_count)
		return 0;
	return min_u64(inodes_per_group, inodes_count - base);
}

static uint64_t group_data_start(const struct cryexts_super_block *sb,
				 const struct cryexts_group_desc *groups,
				 uint64_t group)
{
	if (!has_block_groups(sb))
		return le64toh(sb->first_data_block);
	if (group == 0)
		return le64toh(sb->root_dir_block) + 1;
	return le64toh(groups[group].inode_table_start) +
	       le32toh(groups[group].inode_table_blocks);
}

static uint64_t journal_blocks_in_group(const struct cryexts_super_block *sb,
					const struct cryexts_group_desc *groups,
					uint64_t group)
{
	uint64_t journal_start = le64toh(sb->journal_block);
	uint64_t journal_blocks = le64toh(sb->journal_blocks);
	uint64_t group_start;
	uint64_t group_end;
	uint64_t journal_end;
	uint64_t overlap_start;
	uint64_t overlap_end;

	if (!has_journal(sb) || !journal_blocks)
		return 0;
	if (!has_block_groups(sb))
		return journal_blocks;

	group_start = le64toh(groups[group].group_start);
	group_end = group_start + le64toh(groups[group].blocks_count);
	journal_end = journal_start + journal_blocks;
	overlap_start = journal_start > group_start ? journal_start : group_start;
	overlap_end = journal_end < group_end ? journal_end : group_end;
	if (overlap_end <= overlap_start)
		return 0;
	return overlap_end - overlap_start;
}

static uint64_t reserved_metadata_blocks(const struct cryexts_super_block *sb,
					 const struct cryexts_group_desc *groups)
{
	uint64_t total = 0;

	if (!has_block_groups(sb))
		return le64toh(sb->first_data_block) +
		       journal_blocks_in_group(sb, groups, 0);

	for (uint64_t group = 0; group < le64toh(sb->group_count); group++) {
		uint64_t group_start = le64toh(groups[group].group_start);
		uint64_t reserved_end;

		/*
		 * Group 0's root directory block is a real data block: it must stay
		 * marked used in the bitmap, but superblock free-count recomputation
		 * should count it via inode references rather than as metadata.
		 */
		if (group == 0)
			reserved_end = le64toh(sb->first_data_block);
		else
			reserved_end = group_data_start(sb, groups, group);

		total += reserved_end - group_start;
		total += journal_blocks_in_group(sb, groups, group);
	}
	return total;
}

static int block_to_group(const struct cryexts_super_block *sb,
			  const struct cryexts_group_desc *groups,
			  uint64_t block, uint64_t *group_out)
{
	uint64_t blocks_per_group = le64toh(sb->blocks_per_group);
	uint64_t group_count = le64toh(sb->group_count);
	uint64_t group;
	uint64_t group_start;
	uint64_t group_blocks;

	if (!has_block_groups(sb)) {
		*group_out = 0;
		return 0;
	}

	if (!blocks_per_group)
		return -1;
	group = block / blocks_per_group;
	if (group >= group_count)
		return -1;

	group_start = le64toh(groups[group].group_start);
	group_blocks = le64toh(groups[group].blocks_count);
	if (block < group_start || block >= group_start + group_blocks)
		return -1;

	*group_out = group;
	return 0;
}

static void bitmap_set(unsigned char *bitmap, uint64_t bit)
{
	bitmap[bit / 8] |= (unsigned char)(1U << (bit % 8));
}

static int bitmap_test(const unsigned char *bitmap, uint64_t bit)
{
	return !!(bitmap[bit / 8] & (1U << (bit % 8)));
}

static int salt_is_zero(const uint8_t *salt)
{
	for (unsigned int i = 0; i < CRYEXTS_SALT_LEN; i++) {
		if (salt[i])
			return 0;
	}
	return 1;
}

static int data_block_valid(const struct cryexts_super_block *sb,
			    const struct cryexts_group_desc *groups,
			    uint64_t block)
{
	uint64_t blocks_count = le64toh(sb->blocks_count);
	uint64_t first_data_block = le64toh(sb->first_data_block);
	uint64_t group;
	uint64_t data_start;

	if (block == 0 || block >= blocks_count)
		return 0;
	if (!has_block_groups(sb))
		return block >= first_data_block;
	if (block_to_group(sb, groups, block, &group) < 0)
		return 0;
	data_start = group == 0 ? first_data_block : group_data_start(sb, groups, group);
	return block >= data_start;
}

static int validate_super(const struct cryexts_super_block *sb)
{
	uint64_t blocks_count = le64toh(sb->blocks_count);
	uint64_t inodes_count = le64toh(sb->inodes_count);
	uint64_t inode_table_start = le64toh(sb->inode_table_start);
	uint64_t inode_table_blocks = le64toh(sb->inode_table_blocks);
	uint64_t inode_table_end = inode_table_start + inode_table_blocks;
	uint64_t block_bitmap_block = le64toh(sb->block_bitmap_block);
	uint64_t inode_bitmap_block = le64toh(sb->inode_bitmap_block);
	uint64_t root_dir_block = le64toh(sb->root_dir_block);
	uint64_t first_data_block = le64toh(sb->first_data_block);
	uint64_t group_count = le64toh(sb->group_count);
	uint64_t blocks_per_group = le64toh(sb->blocks_per_group);
	uint64_t inodes_per_group = le64toh(sb->inodes_per_group);
	uint64_t journal_block = le64toh(sb->journal_block);
	uint64_t journal_blocks = le64toh(sb->journal_blocks);
	uint32_t flags = le32toh(sb->flags);
	uint32_t version = le32toh(sb->version);
	uint32_t state = le32toh(sb->state);
	int ok = 1;

	if (le32toh(sb->magic) != CRYEXTS_MAGIC) {
		report("bad magic");
		ok = 0;
	}
	if (version != CRYEXTS_VERSION_V3 &&
	    version != CRYEXTS_VERSION_V4 &&
	    version != CRYEXTS_VERSION_V5 &&
	    version != CRYEXTS_VERSION_V6) {
		report("bad version");
		ok = 0;
	}
	if (le32toh(sb->block_size) != CRYEXTS_BLOCK_SIZE) {
		report("bad block size");
		ok = 0;
	}
	if (le32toh(sb->inode_size) != sizeof(struct cryexts_inode)) {
		report("bad inode size");
		ok = 0;
	}
	if (blocks_count < CRYEXTS_FIRST_FREE_DATA_BLOCK) {
		report("block count too small");
		ok = 0;
	}
	if (!has_block_groups(sb) && blocks_count > CRYEXTS_BLOCK_SIZE * 8ULL) {
		report("block count exceeds v2 bitmap capacity");
		ok = 0;
	}
	if (inodes_count != max_inodes(sb)) {
		report("bad inode count");
		ok = 0;
	}
	if (!has_block_groups(sb) && inodes_count > CRYEXTS_BLOCK_SIZE * 8ULL) {
		report("inode count exceeds v2 bitmap capacity");
		ok = 0;
	}
	if (le64toh(sb->free_blocks_count) > blocks_count) {
		report("bad free block count");
		ok = 0;
	}
	if (le64toh(sb->free_inodes_count) > inodes_count) {
		report("bad free inode count");
		ok = 0;
	}
	if (block_bitmap_block == inode_bitmap_block ||
	    block_bitmap_block >= blocks_count ||
	    inode_bitmap_block >= blocks_count) {
		report("bad bitmap block location");
		ok = 0;
	}
	if (!has_block_groups(sb) &&
	    (block_bitmap_block != CRYEXTS_BLOCK_BITMAP_BLOCK ||
	     inode_bitmap_block != CRYEXTS_INODE_BITMAP_BLOCK)) {
		report("bad bitmap block location");
		ok = 0;
	}
	if (!inode_table_start || !inode_table_blocks ||
	    inode_table_end <= inode_table_start ||
	    inode_table_end > blocks_count) {
		report("bad inode table range");
		ok = 0;
	}
	if (le64toh(sb->root_inode_block) < inode_table_start ||
	    le64toh(sb->root_inode_block) >= inode_table_end) {
		report("bad root inode block");
		ok = 0;
	}
	if (root_dir_block != first_data_block) {
		report("bad root dir block");
		ok = 0;
	}
	if (first_data_block != inode_table_end) {
		report("bad first data block");
		ok = 0;
	}
	if (root_dir_block >= blocks_count) {
		report("root dir block is outside device");
		ok = 0;
	}
	if (le64toh(sb->next_ino) < CRYEXTS_ROOT_INO + 1 ||
	    le64toh(sb->next_ino) > max_inodes(sb) + 1) {
		report("bad next inode");
		ok = 0;
	}
	if (le64toh(sb->next_data_block) < root_dir_block + 1 ||
	    le64toh(sb->next_data_block) > blocks_count) {
		report("bad next data block");
		ok = 0;
	}
	if (flags & ~CRYEXTS_SB_FLAG_ENCRYPTED) {
		report("unsupported superblock flags");
		ok = 0;
	}
	if ((flags & CRYEXTS_SB_FLAG_ENCRYPTED) && !le32toh(sb->key_hash)) {
		report("encrypted filesystem has empty key hash");
		ok = 0;
	}
	if (flags & CRYEXTS_SB_FLAG_ENCRYPTED) {
		if (le32toh(sb->encryption_flags) != CRYEXTS_ENC_FLAG_DATA) {
			report("encrypted filesystem has bad encryption flags");
			ok = 0;
		}
		if (le32toh(sb->encryption_kdf) != CRYEXTS_KDF_SALTED_FNV1A) {
			report("encrypted filesystem has bad encryption kdf");
			ok = 0;
		}
		if (le32toh(sb->encryption_alg) != CRYEXTS_ALG_AES_CTR) {
			report("encrypted filesystem has bad encryption algorithm");
			ok = 0;
		}
		if (salt_is_zero(sb->salt)) {
			report("encrypted filesystem has empty salt");
			ok = 0;
		}
	} else if (le32toh(sb->key_hash) || le32toh(sb->encryption_flags) ||
		   le32toh(sb->encryption_kdf) || le32toh(sb->encryption_alg) ||
		   !salt_is_zero(sb->salt)) {
		report("unencrypted filesystem has stray encryption metadata");
		ok = 0;
	}
	if ((le32toh(sb->features_compat) &
	     ~(CRYEXTS_FEATURE_COMPAT_HAS_JOURNAL |
	       CRYEXTS_FEATURE_COMPAT_PREALLOC)) ||
	    (le32toh(sb->features_incompat) &
	     ~(CRYEXTS_FEATURE_INCOMPAT_SINGLE_INDIRECT |
	       CRYEXTS_FEATURE_INCOMPAT_BLOCK_GROUPS |
	       CRYEXTS_FEATURE_INCOMPAT_NEEDS_RECOVERY |
	       CRYEXTS_FEATURE_INCOMPAT_EXTENTS |
	       CRYEXTS_FEATURE_INCOMPAT_XATTR |
	       CRYEXTS_FEATURE_INCOMPAT_ENCRYPTION_POLICY |
	       CRYEXTS_FEATURE_INCOMPAT_DIR_INDEX |
	       CRYEXTS_FEATURE_INCOMPAT_ORPHAN_LIST |
	       CRYEXTS_FEATURE_INCOMPAT_POLICY_TABLE |
	       CRYEXTS_FEATURE_INCOMPAT_EXTENT_TREE |
	       CRYEXTS_FEATURE_INCOMPAT_JOURNAL_V2)) ||
	    (le32toh(sb->features_ro_compat) &
	     ~(CRYEXTS_FEATURE_RO_COMPAT_METADATA_CSUM |
	       CRYEXTS_FEATURE_RO_COMPAT_LARGE_XATTR))) {
		report("unsupported feature flags");
		ok = 0;
	}
	if (version >= CRYEXTS_VERSION_V4) {
		if (state & ~(CRYEXTS_FS_STATE_CLEAN |
			      CRYEXTS_FS_STATE_NEEDS_RECOVERY |
			      CRYEXTS_FS_STATE_ERRORS)) {
			report("bad filesystem state");
			ok = 0;
		}
		if (!group_count) {
			report("bad group count");
			ok = 0;
		}
		if (!blocks_per_group) {
			report("bad blocks per group");
			ok = 0;
		}
		if (!inodes_per_group) {
			report("bad inodes per group");
			ok = 0;
		}
		if (has_block_groups(sb)) {
			if (blocks_per_group > CRYEXTS_BLOCK_SIZE * 8ULL) {
				report("bad blocks per group");
				ok = 0;
			}
			if (inodes_per_group > inode_table_blocks * inodes_per_block()) {
				report("bad inodes per group");
				ok = 0;
			}
			if (group_count != div_round_up_u64(blocks_count, blocks_per_group)) {
				report("bad group count");
				ok = 0;
			}
			if (inodes_count != group_count * inodes_per_group) {
				report("bad inode count");
				ok = 0;
			}
			if (!le64toh(sb->group_desc_table_start) ||
			    !le64toh(sb->group_desc_table_blocks)) {
				report("bad group descriptor table");
				ok = 0;
			}
			if (le64toh(sb->group_desc_table_start) +
				    le64toh(sb->group_desc_table_blocks) >
			    blocks_count) {
				report("bad group descriptor table");
				ok = 0;
			}
			if (group_count * sizeof(struct cryexts_group_desc) >
			    le64toh(sb->group_desc_table_blocks) *
				    CRYEXTS_BLOCK_SIZE) {
				report("group descriptor table is too small");
				ok = 0;
			}
		} else {
			if (group_count != 1) {
				report("bad group count");
				ok = 0;
			}
			if (blocks_per_group != blocks_count) {
				report("bad blocks per group");
				ok = 0;
			}
			if (inodes_per_group != inodes_count) {
				report("bad inodes per group");
				ok = 0;
			}
		}
		if (has_journal(sb)) {
			if (!journal_block || !journal_blocks ||
			    journal_block >= blocks_count ||
			    journal_block + journal_blocks > blocks_count) {
				report("bad journal range");
				ok = 0;
			}
			if (!has_block_groups(sb) &&
			    !data_block_valid(sb, NULL, journal_block)) {
				report("bad journal location");
				ok = 0;
			}
		} else if (journal_block || journal_blocks) {
			report("journal metadata present without journal feature");
			ok = 0;
		}
		if (!!(le32toh(sb->features_incompat) &
		       CRYEXTS_FEATURE_INCOMPAT_NEEDS_RECOVERY) !=
		    !!(state & CRYEXTS_FS_STATE_NEEDS_RECOVERY)) {
			if (repair_mode)
				warn_msg("needs_recovery flag/state mismatch");
			else
				report("needs_recovery flag/state mismatch");
			ok = 0;
		}
		if (version >= CRYEXTS_VERSION_V5) {
			if ((le32toh(sb->features_incompat) &
			     CRYEXTS_FEATURE_INCOMPAT_POLICY_TABLE) &&
			    !le64toh(sb->policy_table_block)) {
				report("policy table feature enabled without policy table block");
				ok = 0;
			}
			if ((le32toh(sb->features_incompat) &
			     CRYEXTS_FEATURE_INCOMPAT_ORPHAN_LIST) &&
			    le64toh(sb->orphan_head) > inodes_count) {
				report("bad orphan head");
				ok = 0;
			}
			if ((le32toh(sb->features_incompat) &
			     CRYEXTS_FEATURE_INCOMPAT_POLICY_TABLE) &&
			    le64toh(sb->policy_table_block) &&
			    le64toh(sb->policy_table_block) >= blocks_count) {
				report("bad policy table block");
				ok = 0;
			}
			if (!!(le32toh(sb->features_ro_compat) &
			       CRYEXTS_FEATURE_RO_COMPAT_METADATA_CSUM) !=
			    !!le64toh(sb->metadata_csum_type)) {
				report("metadata checksum flag/type mismatch");
				ok = 0;
			}
			if ((le32toh(sb->features_ro_compat) &
			     CRYEXTS_FEATURE_RO_COMPAT_METADATA_CSUM) &&
			    le64toh(sb->metadata_csum_type) !=
				    CRYEXTS_METADATA_CSUM_FNV1A32) {
				report("unsupported metadata checksum type");
				ok = 0;
			}
			if (!le64toh(sb->fs_generation)) {
				report("bad filesystem generation");
				ok = 0;
			}
			if (version < CRYEXTS_VERSION_V6 &&
			    (le32toh(sb->features_incompat) &
			     CRYEXTS_FEATURE_INCOMPAT_JOURNAL_V2)) {
				report("journal v2 feature requires version 6");
				ok = 0;
			}
			if (version >= CRYEXTS_VERSION_V6 &&
			    (le32toh(sb->features_incompat) &
			     CRYEXTS_FEATURE_INCOMPAT_JOURNAL_V2)) {
				if (!has_journal(sb)) {
					report("journal v2 feature set without journal");
					ok = 0;
				}
				if (journal_blocks < CRYEXTS_JOURNAL_V2_MIN_BLOCKS) {
					report("journal v2 area is too small");
					ok = 0;
				}
			}
			if (metadata_csum_enabled(sb) &&
			    le32toh(*(__le32 *)sb->reserved) !=
				    super_expected_checksum(sb)) {
				report("superblock checksum mismatch");
				ok = 0;
			}
		} else if (le64toh(sb->orphan_head) ||
			   le64toh(sb->policy_table_block) ||
			   le64toh(sb->dir_index_seed) ||
			   le64toh(sb->metadata_csum_type) ||
			   le64toh(sb->journal_sequence) ||
			   le64toh(sb->fs_generation)) {
			report("unexpected v5 superblock metadata");
			ok = 0;
		}
	}
	return ok ? 0 : -1;
}

static int validate_journal_header(int fd,
				   const struct cryexts_super_block *sb,
				   const struct cryexts_group_desc *groups,
				   const unsigned char *header_block)
{
	const struct cryexts_journal_v2_control *jc;
	const struct cryexts_journal_v2_descriptor *jd;
	const struct cryexts_journal_v2_commit *commit;
	const struct cryexts_journal_header *jh;
	uint64_t blocks_count = le64toh(sb->blocks_count);
	uint64_t active_sequence = 0;
	uint64_t descriptor_sequence = 0;
	uint64_t commit_sequence = 0;
	uint32_t flags;
	uint32_t entries;
	uint32_t commit_entries = 0;
	uint32_t stored_checksum;
	uint32_t expected_checksum;
	int needs_recovery;
	unsigned int i;
	int ok = 1;

	if (!has_journal(sb))
		return 0;

	if (has_journal_v2(sb)) {
		unsigned char descriptor_block[CRYEXTS_BLOCK_SIZE];
		unsigned char commit_block[CRYEXTS_BLOCK_SIZE];

		jc = (const struct cryexts_journal_v2_control *)header_block;
		if (le32toh(jc->magic) != CRYEXTS_JOURNAL_V2_MAGIC) {
			report("journal v2 control magic is invalid");
			return -1;
		}
		if (le16toh(jc->layout_version) != CRYEXTS_JOURNAL_V2_LAYOUT_VERSION ||
		    le16toh(jc->block_type) != CRYEXTS_JOURNAL_V2_BLOCK_CONTROL) {
			report("journal v2 control header is invalid");
			return -1;
		}
		if (le32toh(jc->features) != CRYEXTS_JOURNAL_V2_FEATURE_BASELINE) {
			report("journal v2 control features are invalid");
			return -1;
		}
		if (le64toh(jc->descriptor_block) != journal_v2_descriptor_block(sb) ||
		    le64toh(jc->payload_start) != journal_v2_payload_start(sb) ||
		    le64toh(jc->payload_blocks) != journal_v2_payload_area_blocks(sb) ||
		    le64toh(jc->commit_block) != journal_v2_commit_block(sb)) {
			report("journal v2 control layout does not match journal area");
			return -1;
		}
		stored_checksum = le32toh(jc->checksum);
		expected_checksum = journal_v2_checksum(
			header_block, CRYEXTS_BLOCK_SIZE,
			offsetof(struct cryexts_journal_v2_control, checksum));
		if (stored_checksum != expected_checksum) {
			report("journal v2 control checksum mismatch");
			return -1;
		}
		if (le64toh(jc->tail_sequence) > le64toh(jc->checkpoint_sequence) ||
		    le64toh(jc->checkpoint_sequence) > le64toh(jc->last_sequence)) {
			report("journal v2 control sequence window is invalid");
			return -1;
		}
		if (!le64toh(jc->active_sequence) &&
		    (le64toh(jc->tail_sequence) != le64toh(jc->last_sequence) ||
		     le64toh(jc->checkpoint_sequence) != le64toh(jc->last_sequence))) {
			report("journal v2 idle control state is not checkpointed");
			return -1;
		}

		active_sequence = le64toh(jc->active_sequence);
		if (!active_sequence) {
			if (le32toh(sb->state) & CRYEXTS_FS_STATE_NEEDS_RECOVERY) {
				report("journal v2 control is idle but superblock needs recovery");
				return -1;
			}
			return 0;
		}

		if (read_full(fd, descriptor_block, sizeof(descriptor_block),
			      journal_v2_descriptor_block(sb) * CRYEXTS_BLOCK_SIZE) < 0) {
			perror("read journal v2 descriptor");
			return -1;
		}
		if (read_full(fd, commit_block, sizeof(commit_block),
			      journal_v2_commit_block(sb) * CRYEXTS_BLOCK_SIZE) < 0) {
			perror("read journal v2 commit");
			return -1;
		}

		jd = (const struct cryexts_journal_v2_descriptor *)descriptor_block;
		if (le32toh(jd->magic) != CRYEXTS_JOURNAL_V2_MAGIC ||
		    le16toh(jd->layout_version) != CRYEXTS_JOURNAL_V2_LAYOUT_VERSION ||
		    le16toh(jd->block_type) != CRYEXTS_JOURNAL_V2_BLOCK_DESCRIPTOR) {
			report("journal v2 descriptor header is invalid");
			return -1;
		}
		entries = le32toh(jd->entry_count);
		if (entries > journal_v2_payload_capacity(sb)) {
			report("journal v2 descriptor entry count is invalid");
			return -1;
		}
		if (le64toh(jd->payload_start) != journal_v2_payload_start(sb) ||
		    le64toh(jd->commit_block) != journal_v2_commit_block(sb)) {
			report("journal v2 descriptor layout is invalid");
			return -1;
		}
		stored_checksum = le32toh(jd->checksum);
		expected_checksum = journal_v2_checksum(
			descriptor_block, CRYEXTS_BLOCK_SIZE,
			offsetof(struct cryexts_journal_v2_descriptor, checksum));
		if (stored_checksum != expected_checksum) {
			report("journal v2 descriptor checksum mismatch");
			return -1;
		}

		commit = (const struct cryexts_journal_v2_commit *)commit_block;
		if (le32toh(commit->magic) != CRYEXTS_JOURNAL_V2_MAGIC ||
		    le16toh(commit->layout_version) != CRYEXTS_JOURNAL_V2_LAYOUT_VERSION ||
		    le16toh(commit->block_type) != CRYEXTS_JOURNAL_V2_BLOCK_COMMIT) {
			report("journal v2 commit header is invalid");
			return -1;
		}
		if (!(le32toh(commit->flags) & CRYEXTS_JOURNAL_V2_FLAG_COMMITTED)) {
			report("journal v2 commit flag is missing");
			return -1;
		}
		commit_entries = le32toh(commit->entry_count);
		if (commit_entries > entries) {
			report("journal v2 commit entry count exceeds descriptor");
			return -1;
		}
		if (le64toh(commit->descriptor_block) != journal_v2_descriptor_block(sb)) {
			report("journal v2 commit descriptor pointer is invalid");
			return -1;
		}
		stored_checksum = le32toh(commit->checksum);
		expected_checksum = journal_v2_checksum(
			commit_block, CRYEXTS_BLOCK_SIZE,
			offsetof(struct cryexts_journal_v2_commit, checksum));
		if (stored_checksum != expected_checksum) {
			report("journal v2 commit checksum mismatch");
			return -1;
		}

		descriptor_sequence = le64toh(jd->sequence);
		commit_sequence = le64toh(commit->sequence);
		if (descriptor_sequence != active_sequence ||
		    commit_sequence != active_sequence) {
			report("journal v2 sequence mismatch");
			return -1;
		}
		if (!(le32toh(sb->state) & CRYEXTS_FS_STATE_NEEDS_RECOVERY)) {
			report("journal v2 transaction exists but superblock is not in recovery state");
			return -1;
		}

		for (i = 0; i < commit_entries; i++) {
			uint64_t home_block = le64toh(jd->home_blocks[i]);

			if (!home_block || home_block >= blocks_count ||
			    !data_block_valid(sb, groups, home_block)) {
				report("journal v2 payload targets an invalid home block");
				ok = 0;
				continue;
			}
			if (journal_block_is_internal(sb, home_block)) {
				report("journal v2 payload points back into the journal area");
				ok = 0;
			}
		}
		for (; i < CRYEXTS_JOURNAL_V2_MAX_ENTRIES; i++) {
			if (le64toh(jd->home_blocks[i])) {
				report("journal v2 descriptor has non-zero trailing home block entries");
				ok = 0;
				break;
			}
		}
		if (!ok)
			return -1;
		report("journal v2 replay pending");
		return -1;
	}

	jh = (const struct cryexts_journal_header *)header_block;
	if (le32toh(jh->magic) != CRYEXTS_JOURNAL_MAGIC) {
		if (le32toh(sb->state) & CRYEXTS_FS_STATE_NEEDS_RECOVERY) {
			if (repair_mode)
				warn_msg("needs_recovery is set but journal header magic is invalid");
			else
				report("needs_recovery is set but journal header magic is invalid");
		}
		return 0;
	}

	entries = le32toh(jh->entry_count);
	flags = le32toh(jh->flags);
	stored_checksum = le32toh(jh->checksum);
	expected_checksum = journal_checksum(header_block, CRYEXTS_BLOCK_SIZE);
	needs_recovery = !!(le32toh(sb->state) & CRYEXTS_FS_STATE_NEEDS_RECOVERY);

	if (entries > CRYEXTS_JOURNAL_MAX_ENTRIES ||
	    entries + 1 > le64toh(sb->journal_blocks)) {
		if (repair_mode)
			warn_msg("journal header entry count is invalid");
		else
			report("journal header entry count is invalid");
		ok = 0;
	}
	if (stored_checksum != expected_checksum) {
		if (repair_mode)
			warn_msg("journal header checksum mismatch");
		else
			report("journal header checksum mismatch");
		ok = 0;
	}
	if ((flags & CRYEXTS_JOURNAL_FLAG_VALID) && !entries) {
		if (repair_mode)
			warn_msg("journal header is marked valid but has no payload entries");
		else
			report("journal header is marked valid but has no payload entries");
		ok = 0;
	}
	if (!(flags & CRYEXTS_JOURNAL_FLAG_VALID) && entries) {
		if (repair_mode)
			warn_msg("journal header has payload entries but is not marked valid");
		else
			report("journal header has payload entries but is not marked valid");
		ok = 0;
	}
	if ((flags & CRYEXTS_JOURNAL_FLAG_VALID) && !needs_recovery) {
		if (repair_mode)
			warn_msg("journal header is valid but superblock is not in recovery state");
		else
			report("journal header is valid but superblock is not in recovery state");
		ok = 0;
	}
	for (i = 0; i < entries && i < CRYEXTS_JOURNAL_MAX_ENTRIES; i++) {
		uint64_t home_block = le64toh(jh->home_blocks[i]);

		if (!home_block || home_block >= blocks_count ||
		    !data_block_valid(sb, groups, home_block)) {
			if (repair_mode)
				warn_msg("journal payload targets an invalid home block");
			else
				report("journal payload targets an invalid home block");
			ok = 0;
			continue;
		}
		if (journal_block_is_internal(sb, home_block)) {
			if (repair_mode)
				warn_msg("journal payload points back into the journal area");
			else
				report("journal payload points back into the journal area");
			ok = 0;
		}
	}
	for (; i < CRYEXTS_JOURNAL_MAX_ENTRIES; i++) {
		if (le64toh(jh->home_blocks[i])) {
			if (repair_mode)
				warn_msg("journal header has non-zero trailing home block entries");
			else
				report("journal header has non-zero trailing home block entries");
			ok = 0;
			break;
		}
	}
	return ok ? 0 : -1;
}

static int validate_groups(const struct cryexts_super_block *sb,
			   const struct cryexts_group_desc *groups)
{
	uint64_t blocks_count = le64toh(sb->blocks_count);
	uint64_t group_count = le64toh(sb->group_count);
	uint64_t blocks_per_group = le64toh(sb->blocks_per_group);
	int ok = 1;

	if (!has_block_groups(sb))
		return 0;

	for (uint64_t group = 0; group < group_count; group++) {
		uint64_t expected_start = group * blocks_per_group;
		uint64_t expected_blocks =
			min_u64(blocks_per_group, blocks_count - expected_start);
		uint64_t group_start = le64toh(groups[group].group_start);
		uint64_t group_blocks = le64toh(groups[group].blocks_count);
		uint64_t block_bitmap_block = le64toh(groups[group].block_bitmap_block);
		uint64_t inode_bitmap_block = le64toh(groups[group].inode_bitmap_block);
		uint64_t inode_table_start = le64toh(groups[group].inode_table_start);
		uint64_t inode_table_blocks = le32toh(groups[group].inode_table_blocks);
		uint64_t inode_table_end = inode_table_start + inode_table_blocks;
		uint64_t data_start = group_data_start(sb, groups, group);
		uint64_t free_blocks_expected =
			group_blocks > data_start - group_start ?
			group_blocks - (data_start - group_start) : 0;
		uint64_t free_inodes_expected = group_inode_limit(sb, group);
		uint64_t journal_reserved =
			journal_blocks_in_group(sb, groups, group);

		if (journal_reserved > free_blocks_expected)
			journal_reserved = free_blocks_expected;
		free_blocks_expected -= journal_reserved;

		if (group_start != expected_start || group_blocks != expected_blocks) {
			report("bad group descriptor range");
			ok = 0;
		}
		if (group == 0) {
			if (block_bitmap_block != le64toh(sb->block_bitmap_block) ||
			    inode_bitmap_block != le64toh(sb->inode_bitmap_block) ||
			    inode_table_start != le64toh(sb->inode_table_start)) {
				report("bad group0 descriptor layout");
				ok = 0;
			}
		} else if (block_bitmap_block != group_start ||
			   inode_bitmap_block != group_start + 1 ||
			   inode_table_start != group_start + 2) {
			report("bad group descriptor layout");
			ok = 0;
		}
		if (!inode_table_blocks || inode_table_end > group_start + group_blocks) {
			report("bad group inode table");
			ok = 0;
		}
		if (block_bitmap_block >= group_start + group_blocks ||
		    inode_bitmap_block >= group_start + group_blocks ||
		    data_start > group_start + group_blocks) {
			report("bad group metadata location");
			ok = 0;
		}
		if (le32toh(groups[group].free_blocks_count) > free_blocks_expected) {
			report("bad group free blocks count");
			ok = 0;
		}
		if (group == 0 && free_inodes_expected > 0)
			free_inodes_expected--;
		if (le32toh(groups[group].free_inodes_count) > free_inodes_expected) {
			report("bad group free inodes count");
			ok = 0;
		}
		if (metadata_csum_enabled(sb) &&
		    le32toh(*(__le32 *)groups[group].reserved) !=
			    group_expected_checksum(sb, &groups[group])) {
			report("group descriptor checksum mismatch");
			ok = 0;
		}
	}
	return ok ? 0 : -1;
}

static int validate_v5_layout_after_groups(const struct cryexts_super_block *sb,
					   const struct cryexts_group_desc *groups)
{
	uint32_t version = le32toh(sb->version);
	uint32_t incompat = le32toh(sb->features_incompat);
	uint64_t policy_table_block = le64toh(sb->policy_table_block);

	if (version < CRYEXTS_VERSION_V5)
		return 0;

	if ((incompat & CRYEXTS_FEATURE_INCOMPAT_POLICY_TABLE) &&
	    policy_table_block &&
	    !data_block_valid(sb, groups, policy_table_block)) {
		report("bad policy table block");
		return -1;
	}

	return 0;
}

static int policy_exists_in_table(const struct cryexts_policy_table_block *pt,
				  uint32_t policy_id)
{
	uint16_t count = le16toh(pt->entry_count);
	uint16_t i;

	for (i = 0; i < count; i++) {
		if (le32toh(pt->entries[i].policy_id) == policy_id)
			return 1;
	}
	return 0;
}

static int validate_policy_table(int fd, const struct cryexts_super_block *sb,
				 const struct cryexts_group_desc *groups,
				 unsigned char *block_seen,
				 uint64_t block_count,
				 unsigned char *policy_block)
{
	uint32_t incompat = le32toh(sb->features_incompat);
	uint64_t block = le64toh(sb->policy_table_block);
	struct cryexts_policy_table_block *pt;
	uint16_t count;
	uint16_t i;

	if (!(incompat & CRYEXTS_FEATURE_INCOMPAT_POLICY_TABLE)) {
		if (block) {
			report("policy table block present without feature flag");
			return -1;
		}
		return 0;
	}

	if (!block) {
		report("policy table feature enabled without policy table block");
		return -1;
	}
	if (!data_block_valid(sb, groups, block)) {
		report("bad policy table block");
		return -1;
	}
	if (read_full(fd, policy_block, CRYEXTS_BLOCK_SIZE,
		      block * CRYEXTS_BLOCK_SIZE) < 0) {
		perror("read policy table");
		return -1;
	}

	pt = (struct cryexts_policy_table_block *)policy_block;
	count = le16toh(pt->entry_count);
	if (le32toh(pt->magic) != CRYEXTS_POLICY_TABLE_MAGIC ||
	    !count || count > CRYEXTS_POLICY_TABLE_MAX_ENTRIES) {
		report("bad policy table block");
		return -1;
	}
	if (metadata_csum_enabled(sb) &&
	    le32toh(*(__le32 *)pt->reserved) !=
		    policy_expected_checksum(sb, block, pt)) {
		report("policy table checksum mismatch");
		return -1;
	}

	for (i = 0; i < count; i++) {
		uint16_t j;
		uint32_t id = le32toh(pt->entries[i].policy_id);

		for (j = 0; j < i; j++) {
			if (le32toh(pt->entries[j].policy_id) == id) {
				report("policy table contains duplicate policy id");
				return -1;
			}
		}
	}

	if (!policy_exists_in_table(pt, le32toh(sb->default_encryption_policy))) {
		report("default policy id is missing from policy table");
		return -1;
	}

	if (block_seen && block < block_count) {
		if (block_seen[block]) {
			report("policy table block is referenced by multiple metadata owners");
			return -1;
		}
		block_seen[block] = 1;
	}

	return 0;
}

static int validate_reserved_bitmaps(int fd, const struct cryexts_super_block *sb,
				     const struct cryexts_group_desc *groups)
{
	unsigned char block_bitmap[CRYEXTS_BLOCK_SIZE];
	unsigned char inode_bitmap[CRYEXTS_BLOCK_SIZE];
	int ok = 1;

	if (!has_block_groups(sb)) {
		uint64_t block_bitmap_block = le64toh(sb->block_bitmap_block);
		uint64_t inode_bitmap_block = le64toh(sb->inode_bitmap_block);

		if (read_full(fd, block_bitmap, sizeof(block_bitmap),
			      block_bitmap_block * CRYEXTS_BLOCK_SIZE) < 0) {
			perror("read block bitmap");
			return -1;
		}
		if (read_full(fd, inode_bitmap, sizeof(inode_bitmap),
			      inode_bitmap_block * CRYEXTS_BLOCK_SIZE) < 0) {
			perror("read inode bitmap");
			return -1;
		}

		for (uint64_t block = 0; block < CRYEXTS_FIRST_FREE_DATA_BLOCK; block++) {
			if (!bitmap_test(block_bitmap, block)) {
				if (repair_mode)
					warn_msg("reserved block is not marked used in block bitmap");
				else
					report("reserved block is not marked used in block bitmap");
				ok = 0;
				break;
			}
		}
		if (!bitmap_test(inode_bitmap, CRYEXTS_ROOT_INO - 1)) {
			if (repair_mode)
				warn_msg("root inode is not marked used in inode bitmap");
			else
				report("root inode is not marked used in inode bitmap");
			ok = 0;
		}
		return ok ? 0 : -1;
	}

	for (uint64_t group = 0; group < le64toh(sb->group_count); group++) {
		uint64_t group_start = le64toh(groups[group].group_start);
		uint64_t group_end = group_start + le64toh(groups[group].blocks_count);
		uint64_t reserved_end = group_data_start(sb, groups, group);
		uint64_t block_bitmap_block = le64toh(groups[group].block_bitmap_block);
		uint64_t inode_bitmap_block = le64toh(groups[group].inode_bitmap_block);

		if (read_full(fd, block_bitmap, sizeof(block_bitmap),
			      block_bitmap_block * CRYEXTS_BLOCK_SIZE) < 0) {
			perror("read group block bitmap");
			return -1;
		}
		if (read_full(fd, inode_bitmap, sizeof(inode_bitmap),
			      inode_bitmap_block * CRYEXTS_BLOCK_SIZE) < 0) {
			perror("read group inode bitmap");
			return -1;
		}

		for (uint64_t block = group_start; block < reserved_end; block++) {
			if (!bitmap_test(block_bitmap, block - group_start)) {
				if (repair_mode)
					warn_msg("reserved block is not marked used in block bitmap");
				else
					report("reserved block is not marked used in block bitmap");
				ok = 0;
				break;
			}
		}
		if (has_journal(sb)) {
			uint64_t journal_start = le64toh(sb->journal_block);
			uint64_t journal_end = journal_start + le64toh(sb->journal_blocks);
			uint64_t overlap_start =
				journal_start > group_start ? journal_start : group_start;
			uint64_t overlap_end =
				journal_end < group_end ? journal_end : group_end;

			for (uint64_t block = overlap_start; block < overlap_end; block++) {
				if (!bitmap_test(block_bitmap, block - group_start)) {
					if (repair_mode)
						warn_msg("journal block is not marked used in block bitmap");
					else
						report("journal block is not marked used in block bitmap");
					ok = 0;
					break;
				}
			}
		}
		if (group == 0 && !bitmap_test(inode_bitmap, CRYEXTS_ROOT_INO - 1)) {
			if (repair_mode)
				warn_msg("root inode is not marked used in inode bitmap");
			else
				report("root inode is not marked used in inode bitmap");
			ok = 0;
		}
	}
	return ok ? 0 : -1;
}

static int mode_supported(uint16_t mode)
{
	return (mode & 0170000) == 0040000 ||
	       (mode & 0170000) == 0100000 ||
	       (mode & 0170000) == 0120000;
}

static int inode_uses_extents(const struct cryexts_inode *inode)
{
	return !!(le32toh(inode->inode_flags) & CRYEXTS_INODE_FLAG_EXTENTS);
}

static uint32_t dir_hash(const struct cryexts_super_block *sb,
			 const char *name, size_t len)
{
	uint32_t hash = 2166136261u;
	uint64_t seed = le64toh(sb->dir_index_seed);
	size_t i;

	for (i = 0; i < sizeof(seed); i++) {
		hash ^= (uint8_t)(seed >> (i * 8));
		hash *= 16777619u;
	}
	for (i = 0; i < len; i++) {
		hash ^= (uint8_t)name[i];
		hash *= 16777619u;
	}
	return hash;
}

static uint64_t extent_file_blocks_max_from_inline_max(uint16_t inline_max)
{
	if (inline_max == CRYEXTS_EXTENT_ROOT_INLINE_EXTENTS)
		return (uint64_t)(CRYEXTS_EXTENT_ROOT_INLINE_EXTENTS +
				  CRYEXTS_EXTENTS_PER_BLOCK) *
		       CRYEXTS_MAX_EXTENT_BLOCKS;
	return (uint64_t)CRYEXTS_MAX_INLINE_EXTENTS * CRYEXTS_MAX_EXTENT_BLOCKS;
}

static uint64_t extent_file_blocks_max_from_inode_flags(uint32_t inode_flags,
							uint16_t inline_max)
{
	if (inode_flags & CRYEXTS_INODE_FLAG_EXTENT_TREE_V2)
		return CRYEXTS_EXTENT_TREE_V2_FILE_BLOCKS_MAX;
	return extent_file_blocks_max_from_inline_max(inline_max);
}

static int validate_extent_array(const struct cryexts_super_block *sb,
				 const struct cryexts_group_desc *groups,
				 const struct cryexts_extent *extents,
				 uint16_t entries,
				 uint64_t *next_logical,
				 unsigned char *block_seen,
				 uint64_t block_count,
				 unsigned int *used_blocks)
{
	unsigned int i;

	for (i = 0; i < entries; i++) {
		uint64_t logical = le64toh(extents[i].logical_start);
		uint64_t physical = le64toh(extents[i].physical_start);
		uint32_t len = le32toh(extents[i].length);
		uint32_t j;

		if (!len || len > CRYEXTS_MAX_EXTENT_BLOCKS) {
			report("bad extent length");
			return 0;
		}
		if (logical < *next_logical || logical + len < logical) {
			report("extent logical ranges overlap or are not sorted");
			return 0;
		}
		for (j = 0; j < len; j++) {
			uint64_t data_block = physical + j;

			if (!data_block_valid(sb, groups, data_block)) {
				report("extent data block points outside data area");
				return 0;
			}
			if (block_seen && data_block < block_count &&
			    block_seen[data_block]) {
				report("data block is referenced by multiple inodes");
				return 0;
			}
			if (block_seen && data_block < block_count)
				block_seen[data_block] = 1;
			(*used_blocks)++;
		}
		*next_logical = logical + len;
	}
	return 1;
}

static const struct cryexts_inode_extra *inode_extra(const struct cryexts_inode *inode)
{
	return (const struct cryexts_inode_extra *)
		(inode->reserved + sizeof(inode->reserved) -
		 sizeof(struct cryexts_inode_extra));
}

static int validate_xattr_block(int fd,
				const struct cryexts_super_block *sb,
				const struct cryexts_group_desc *groups,
				uint64_t block,
				unsigned int *entries_out,
				uint64_t *overflow_block_out)
{
	unsigned char buf[CRYEXTS_BLOCK_SIZE];
	const struct cryexts_xattr_block_header *xh;
	unsigned int entries;
	unsigned int used;
	unsigned int offset;
	unsigned int i;

	if (read_full(fd, buf, sizeof(buf), block * CRYEXTS_BLOCK_SIZE) < 0) {
		perror("read xattr block");
		return 0;
	}

	xh = (const struct cryexts_xattr_block_header *)buf;
	if (le32toh(xh->magic) != CRYEXTS_XATTR_MAGIC) {
		report("bad xattr block magic");
		return 0;
	}

	entries = le16toh(xh->entries);
	used = le16toh(xh->used_bytes);
	if (entries > CRYEXTS_XATTR_MAX_ITEMS) {
		report("xattr block has too many entries");
		return 0;
	}
	if (used > CRYEXTS_BLOCK_SIZE - sizeof(*xh)) {
		report("xattr block used bytes overflow");
		return 0;
	}

	offset = sizeof(*xh);
	for (i = 0; i < entries; i++) {
		const struct cryexts_xattr_entry *xe;
		unsigned int name_len;
		unsigned int value_len;
		unsigned int total_len;

		if (offset + sizeof(*xe) > CRYEXTS_BLOCK_SIZE) {
			report("xattr entry header overruns block");
			return 0;
		}
		xe = (const struct cryexts_xattr_entry *)(buf + offset);
		name_len = xe->name_len;
		value_len = le16toh(xe->value_len);
		total_len = sizeof(*xe) + name_len + value_len;
		if (!name_len || name_len > CRYEXTS_XATTR_MAX_NAME_LEN) {
			report("xattr entry has invalid name length");
			return 0;
		}
		if (xe->namespace_id != CRYEXTS_XATTR_NAMESPACE_USER) {
			report("xattr entry has invalid namespace");
			return 0;
		}
		if (offset + total_len > CRYEXTS_BLOCK_SIZE) {
			report("xattr entry overruns block");
			return 0;
		}
		offset += total_len;
	}

	if (offset != sizeof(*xh) + used) {
		report("xattr block used bytes mismatch");
		return 0;
	}

	if (entries_out)
		*entries_out = entries;
	if (overflow_block_out)
		*overflow_block_out = le64toh(xh->overflow_block);
	if (le64toh(xh->overflow_block) &&
	    !(le32toh(sb->features_ro_compat) & CRYEXTS_FEATURE_RO_COMPAT_LARGE_XATTR)) {
		report("xattr overflow used without large-xattr feature");
		return 0;
	}
	if (le64toh(xh->overflow_block) &&
	    !data_block_valid(sb, groups, le64toh(xh->overflow_block))) {
		report("xattr overflow block points outside data area");
		return 0;
	}

	return 1;
}

static unsigned int count_indirect_entries(int fd,
					   const struct cryexts_super_block *sb,
					   const struct cryexts_group_desc *groups,
					   uint64_t indirect_block,
					   unsigned char *block_seen,
					   uint64_t block_count, int *ok)
{
	unsigned char block[CRYEXTS_BLOCK_SIZE];
	__le64 *entries = (__le64 *)block;
	unsigned int count = 0;

	if (read_full(fd, block, sizeof(block),
		      indirect_block * CRYEXTS_BLOCK_SIZE) < 0) {
		perror("read indirect block");
		*ok = 0;
		return 0;
	}

	for (unsigned int i = 0; i < CRYEXTS_INDIRECT_BLOCKS; i++) {
		uint64_t data_block = le64toh(entries[i]);

		if (!data_block)
			continue;
		count++;
		if (!data_block_valid(sb, groups, data_block)) {
			report("indirect entry points outside data area");
			*ok = 0;
			continue;
		}
		if (block_seen && data_block < block_count && block_seen[data_block]) {
			report("data block is referenced by multiple inodes");
			*ok = 0;
		}
		if (block_seen && data_block < block_count)
			block_seen[data_block] = 1;
	}
	return count;
}

static int validate_inode(int fd,
			  const struct cryexts_super_block *sb,
			  const struct cryexts_group_desc *groups,
			  const struct cryexts_inode *inode,
			  uint64_t ino,
			  unsigned char *inode_seen,
			  unsigned char *block_seen,
			  uint64_t block_count,
			  const struct cryexts_policy_table_block *policy_table)
{
	uint16_t mode = le16toh(inode->mode);
	uint64_t size = le64toh(inode->size);
	uint64_t blocks = le64toh(inode->blocks);
	int is_dir = (mode & 0170000) == 0040000;
	int is_reg = (mode & 0170000) == 0100000;
	int is_lnk = (mode & 0170000) == 0120000;
	uint64_t indirect_block = le64toh(inode->indirect_block);
	uint32_t inode_flags = le32toh(inode->inode_flags);
	int dir_index_inode = is_dir &&
		!!(inode_flags & CRYEXTS_INODE_FLAG_DIR_INDEX);
	uint64_t xattr_block = le64toh(inode_extra(inode)->xattr_block);
	uint64_t xattr_overflow_block = 0;
	uint32_t policy_id = le32toh(inode_extra(inode)->encryption_policy_id);
	unsigned int used_direct_blocks = 0;
	unsigned int used_blocks = 0;
	int ok = 1;

	if (!mode) {
		if (ino == CRYEXTS_ROOT_INO) {
			report("root inode is empty");
			return -1;
		}
		return 0;
	}
	if (!mode_supported(mode)) {
		report("unsupported inode mode");
		ok = 0;
	}
	if (inode_flags & ~(CRYEXTS_INODE_FLAG_EXTENTS |
			    CRYEXTS_INODE_FLAG_EXTENT_TREE_V2 |
			    CRYEXTS_INODE_FLAG_DIR_INDEX |
			    CRYEXTS_INODE_FLAG_IMMUTABLE |
			    CRYEXTS_INODE_FLAG_APPEND_ONLY)) {
		report("unsupported inode flags");
		ok = 0;
	}
	if (policy_table && !policy_exists_in_table(policy_table, policy_id)) {
		report("inode references unknown encryption policy");
		ok = 0;
	}
	if (xattr_block) {
		if (!data_block_valid(sb, groups, xattr_block)) {
			report("inode xattr block points outside data area");
			ok = 0;
		} else {
			if (block_seen && xattr_block < block_count &&
			    block_seen[xattr_block]) {
				report("xattr block is referenced by multiple inodes");
				ok = 0;
			}
			if (block_seen && xattr_block < block_count)
				block_seen[xattr_block] = 1;
			if (!validate_xattr_block(fd, sb, groups, xattr_block, NULL,
						       &xattr_overflow_block))
				ok = 0;
			if (xattr_overflow_block) {
				uint64_t nested_xattr_overflow = 0;

				if (block_seen && xattr_overflow_block < block_count &&
				    block_seen[xattr_overflow_block]) {
					report("xattr overflow block is referenced by multiple inodes");
					ok = 0;
				}
				if (block_seen && xattr_overflow_block < block_count)
					block_seen[xattr_overflow_block] = 1;
				if (!validate_xattr_block(fd, sb, groups,
						       xattr_overflow_block,
						       NULL, &nested_xattr_overflow))
					ok = 0;
				if (nested_xattr_overflow) {
					report("xattr overflow block chains to another overflow block");
					ok = 0;
				}
			}
		}
	}
	if (inode_uses_extents(inode)) {
		const struct cryexts_extent_header *eh;
		const struct cryexts_extent *extents;
		uint16_t inline_max;
		uint16_t inline_entries;
		uint64_t next_logical = 0;
		uint64_t overflow_block = 0;
		uint16_t overflow_entries = 0;
		uint32_t overflow_checksum = 0;

		if (!is_reg) {
			report("only regular files may use extents");
			ok = 0;
		}
		eh = (const struct cryexts_extent_header *)inode->reserved;
		inline_max = le16toh(eh->max);
		inline_entries = le16toh(eh->entries);
		if (le16toh(eh->magic) != CRYEXTS_EXTENT_MAGIC) {
			report("bad extent header");
			ok = 0;
			goto out_inode_seen;
		}
		if (inode_flags & CRYEXTS_INODE_FLAG_EXTENT_TREE_V2) {
			const struct cryexts_extent_root_ref *refs;
			unsigned int leaf_index;

			if (inline_max != CRYEXTS_EXTENT_TREE_ROOT_REFS ||
			    inline_entries > CRYEXTS_EXTENT_TREE_ROOT_REFS ||
			    le16toh(eh->reserved) != CRYEXTS_EXTENT_TREE_V2_DEPTH) {
				report("bad extent tree v2 root header");
				ok = 0;
				goto out_inode_seen;
			}
			refs = (const struct cryexts_extent_root_ref *)(inode->reserved +
							 CRYEXTS_EXTENT_TREE_V2_ROOT_REFS_OFFSET);
			for (leaf_index = 0; leaf_index < inline_entries; leaf_index++) {
				unsigned char block[CRYEXTS_BLOCK_SIZE];
				const struct cryexts_extent_header *leh;
				const struct cryexts_extent *lextents;
				uint64_t leaf_block = le64toh(refs[leaf_index].leaf_block);
				uint16_t leaf_entries = le16toh(refs[leaf_index].entries);
				uint32_t leaf_checksum = le32toh(refs[leaf_index].checksum);

				if (!leaf_block || !leaf_entries ||
				    leaf_entries > CRYEXTS_EXTENTS_PER_BLOCK) {
					report("bad extent tree v2 leaf ref");
					ok = 0;
					goto out_inode_seen;
				}
				if (!data_block_valid(sb, groups, leaf_block)) {
					report("extent tree v2 leaf block points outside data area");
					ok = 0;
				} else {
					if (block_seen && leaf_block < block_count &&
					    block_seen[leaf_block]) {
						report("extent tree v2 leaf block is referenced by multiple inodes");
						ok = 0;
					}
					if (block_seen && leaf_block < block_count)
						block_seen[leaf_block] = 1;
				}
				if (read_full(fd, block, sizeof(block),
					      leaf_block * CRYEXTS_BLOCK_SIZE) < 0) {
					perror("read extent tree v2 leaf block");
					ok = 0;
					continue;
				}
				leh = (const struct cryexts_extent_header *)block;
				lextents = (const struct cryexts_extent *)(block +
								   sizeof(*leh));
				if (le16toh(leh->magic) != CRYEXTS_EXTENT_MAGIC ||
				    le16toh(leh->max) != CRYEXTS_EXTENTS_PER_BLOCK ||
				    le16toh(leh->entries) != leaf_entries) {
					report("bad extent tree v2 leaf header");
					ok = 0;
					continue;
				}
				if (metadata_csum_enabled(sb) &&
				    leaf_checksum != extent_overflow_expected_checksum(
							    sb, leaf_block, block)) {
					report("extent tree v2 leaf checksum mismatch");
					ok = 0;
					continue;
				}
				if (!validate_extent_array(sb, groups, lextents,
							   leaf_entries,
							   &next_logical,
							   block_seen,
							   block_count,
							   &used_blocks))
					ok = 0;
				used_blocks++;
			}
		} else if (inline_max == CRYEXTS_MAX_INLINE_EXTENTS) {
			if (inline_entries > CRYEXTS_MAX_INLINE_EXTENTS) {
				report("bad extent header");
				ok = 0;
				goto out_inode_seen;
			}
		} else if (inline_max == CRYEXTS_EXTENT_ROOT_INLINE_EXTENTS &&
			   (le32toh(sb->features_incompat) &
			    CRYEXTS_FEATURE_INCOMPAT_EXTENT_TREE)) {
			const unsigned char *overflow_ptr =
				inode->reserved + CRYEXTS_EXTENT_ROOT_OVERFLOW_OFFSET;

			if (inline_entries > CRYEXTS_EXTENT_ROOT_INLINE_EXTENTS) {
				report("bad extent header");
				ok = 0;
				goto out_inode_seen;
			}
			overflow_block = le64toh(*((const __le64 *)overflow_ptr));
			overflow_entries = le16toh(*((const __le16 *)(overflow_ptr +
							 sizeof(__le64))));
			overflow_checksum =
				le32toh(*((const __le32 *)(overflow_ptr +
							  sizeof(__le64) +
							  sizeof(__le16))));
			if (!!overflow_block != !!overflow_entries) {
				report("bad extent overflow pointer");
				ok = 0;
				goto out_inode_seen;
			}
			if (overflow_entries > CRYEXTS_EXTENTS_PER_BLOCK) {
				report("bad extent overflow entry count");
				ok = 0;
				goto out_inode_seen;
			}
			if (!overflow_block && overflow_checksum &&
			    metadata_csum_enabled(sb)) {
				report("bad extent overflow checksum pointer");
				ok = 0;
				goto out_inode_seen;
			}
		} else {
			report("bad extent header");
			ok = 0;
			goto out_inode_seen;
		}
		if (indirect_block) {
			report("extent inode unexpectedly uses indirect block");
			ok = 0;
		}
		for (unsigned int i = 0; i < CRYEXTS_DIRECT_BLOCKS; i++) {
			if (le64toh(inode->block[i])) {
				report("extent inode unexpectedly uses direct blocks");
				ok = 0;
				break;
			}
		}
		if (!(inode_flags & CRYEXTS_INODE_FLAG_EXTENT_TREE_V2)) {
			extents = (const struct cryexts_extent *)(inode->reserved +
								  sizeof(*eh));
			if (!validate_extent_array(sb, groups, extents, inline_entries,
						   &next_logical, block_seen,
						   block_count, &used_blocks))
				ok = 0;
		}
		if (overflow_block) {
			unsigned char block[CRYEXTS_BLOCK_SIZE];
			const struct cryexts_extent_header *oeh;
			const struct cryexts_extent *oextents;

			if (!data_block_valid(sb, groups, overflow_block)) {
				report("extent overflow block points outside data area");
				ok = 0;
			} else {
				if (block_seen && overflow_block < block_count &&
				    block_seen[overflow_block]) {
					report("extent overflow block is referenced by multiple inodes");
					ok = 0;
				}
				if (block_seen && overflow_block < block_count)
					block_seen[overflow_block] = 1;
			}
			if (read_full(fd, block, sizeof(block),
				      overflow_block * CRYEXTS_BLOCK_SIZE) < 0) {
				perror("read extent overflow block");
				ok = 0;
			} else {
				oeh = (const struct cryexts_extent_header *)block;
				oextents = (const struct cryexts_extent *)(block +
								 sizeof(*oeh));
				if (le16toh(oeh->magic) != CRYEXTS_EXTENT_MAGIC ||
				    le16toh(oeh->max) != CRYEXTS_EXTENTS_PER_BLOCK ||
				    le16toh(oeh->entries) != overflow_entries) {
					report("bad extent overflow header");
					ok = 0;
				} else if (metadata_csum_enabled(sb) &&
					   overflow_checksum !=
						   extent_overflow_expected_checksum(
							   sb, overflow_block,
							   block)) {
					report("extent overflow checksum mismatch");
					ok = 0;
				} else if (!validate_extent_array(sb, groups, oextents,
								  overflow_entries,
								  &next_logical,
								  block_seen,
								  block_count,
								  &used_blocks)) {
					ok = 0;
				}
			}
			used_blocks++;
		}
		if (size > extent_file_blocks_max_from_inode_flags(inode_flags,
								  inline_max) *
				 CRYEXTS_BLOCK_SIZE) {
			report("regular inode size exceeds extent-inline limit");
			ok = 0;
		}
		if (next_logical > (size + CRYEXTS_BLOCK_SIZE - 1) /
				   CRYEXTS_BLOCK_SIZE) {
			report("extent mapping extends beyond regular inode size");
			ok = 0;
		}
		if (size == 0 && used_blocks) {
			report("empty regular file still owns extent blocks");
			ok = 0;
		}
		if (blocks != (uint64_t)used_blocks * (CRYEXTS_BLOCK_SIZE / 512)) {
			report("extent inode block count mismatch");
			ok = 0;
		}
		goto out_inode_seen;
	}

	for (unsigned int i = 0; i < CRYEXTS_DIRECT_BLOCKS; i++) {
		uint64_t data_block = le64toh(inode->block[i]);

		if (!data_block)
			continue;
		used_direct_blocks++;
		used_blocks++;
		if (!data_block_valid(sb, groups, data_block)) {
			report("inode data block points outside data area");
			ok = 0;
		} else {
			if (block_seen && data_block < block_count && block_seen[data_block]) {
				report("data block is referenced by multiple inodes");
				ok = 0;
			}
			if (block_seen && data_block < block_count)
				block_seen[data_block] = 1;
		}
	}

	if (indirect_block && !dir_index_inode) {
		if (!data_block_valid(sb, groups, indirect_block)) {
			report("inode indirect block points outside data area");
			ok = 0;
		} else {
			if (block_seen && indirect_block < block_count &&
			    block_seen[indirect_block]) {
				report("indirect block is referenced by multiple inodes");
				ok = 0;
			}
			if (block_seen && indirect_block < block_count)
				block_seen[indirect_block] = 1;
		}
		used_blocks++;
		used_blocks += count_indirect_entries(fd, sb, groups, indirect_block,
						      block_seen, block_count, &ok);
	}

	if (is_dir) {
		uint64_t dir_blocks;
		uint64_t dir_index_block = 0;

		if (!size || size % CRYEXTS_BLOCK_SIZE) {
			report("directory inode size is not block-aligned");
			ok = 0;
		}
		dir_blocks = size / CRYEXTS_BLOCK_SIZE;
		if (!dir_blocks || dir_blocks > CRYEXTS_DIRECT_BLOCKS) {
			report("directory inode block count exceeds direct-block limit");
			ok = 0;
		}
		if (inode_flags & CRYEXTS_INODE_FLAG_DIR_INDEX) {
			dir_index_block = indirect_block;
			if (!dir_index_block) {
				report("directory index flag is set without index block");
				ok = 0;
			} else if (!data_block_valid(sb, groups, dir_index_block)) {
				report("directory index block points outside data area");
				ok = 0;
			} else {
				if (block_seen && dir_index_block < block_count &&
				    block_seen[dir_index_block]) {
					report("directory index block is referenced by multiple inodes");
					ok = 0;
				}
				if (block_seen && dir_index_block < block_count)
					block_seen[dir_index_block] = 1;
				used_blocks++;
			}
		} else if (indirect_block) {
			report("directory inode unexpectedly uses indirect block");
			ok = 0;
		}
		if (used_direct_blocks != dir_blocks ||
		    blocks != (dir_blocks + (dir_index_block ? 1 : 0)) *
				      (CRYEXTS_BLOCK_SIZE / 512) ||
		    !le64toh(inode->block[0])) {
			report("bad directory inode layout");
			ok = 0;
		}
		for (unsigned int i = dir_blocks; i < CRYEXTS_DIRECT_BLOCKS; i++) {
			if (le64toh(inode->block[i])) {
				report("directory inode has unexpected trailing block");
				ok = 0;
				break;
			}
		}
	}

	if (is_reg) {
		if (size > (uint64_t)CRYEXTS_FILE_BLOCKS_MAX * CRYEXTS_BLOCK_SIZE) {
			report("regular inode size exceeds single-indirect limit");
			ok = 0;
		}
		if (size == 0 && (used_blocks || indirect_block)) {
			report("empty regular file still owns data blocks");
			ok = 0;
		}
		if (blocks != (uint64_t)used_blocks * (CRYEXTS_BLOCK_SIZE / 512)) {
			report("regular inode block count mismatch");
			ok = 0;
		}
	}

	if (is_lnk) {
		if (!size ||
		    size > (uint64_t)CRYEXTS_FILE_BLOCKS_MAX * CRYEXTS_BLOCK_SIZE - 1) {
			report("symlink inode size exceeds limit");
			ok = 0;
		}
		if (blocks != (uint64_t)used_blocks * (CRYEXTS_BLOCK_SIZE / 512)) {
			report("symlink inode block count mismatch");
			ok = 0;
		}
	}

out_inode_seen:
	if (inode_seen && ino > 0)
		inode_seen[ino - 1] = 1;
	return ok ? 0 : -1;
}

static int read_inode(int fd, const struct cryexts_super_block *sb,
		      const struct cryexts_group_desc *groups,
		      uint64_t ino, struct cryexts_inode *inode)
{
	uint64_t index = ino - 1;
	uint64_t block;
	uint64_t offset;

	if (has_block_groups(sb)) {
		uint64_t group = index / le64toh(sb->inodes_per_group);
		uint64_t index_in_group = index % le64toh(sb->inodes_per_group);

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

static int validate_orphan_list(int fd, const struct cryexts_super_block *sb,
				const struct cryexts_group_desc *groups)
{
	uint32_t version = le32toh(sb->version);
	uint32_t incompat = le32toh(sb->features_incompat);
	uint64_t inodes_count = le64toh(sb->inodes_count);
	uint64_t head = le64toh(sb->orphan_head);
	unsigned char *seen;
	unsigned int guard = 0;
	int ok = 1;

	if (version < CRYEXTS_VERSION_V5)
		return 0;
	if (!(incompat & CRYEXTS_FEATURE_INCOMPAT_ORPHAN_LIST)) {
		if (head) {
			report("orphan head present without orphan feature");
			return -1;
		}
		return 0;
	}
	if (!head)
		return 0;

	seen = calloc(inodes_count ? inodes_count : 1, 1);
	if (!seen) {
		perror("calloc orphan seen");
		return -1;
	}

	while (head && guard++ < inodes_count) {
		struct cryexts_inode inode;
		uint64_t next_orphan;
		uint16_t mode;
		uint16_t links_count;

		if (head < CRYEXTS_ROOT_INO || head > inodes_count) {
			report("orphan list points to invalid inode");
			ok = 0;
			break;
		}
		if (seen[head - 1]) {
			report("orphan list contains a cycle");
			ok = 0;
			break;
		}
		seen[head - 1] = 1;

		if (read_inode(fd, sb, groups, head, &inode) < 0) {
			perror("read orphan inode");
			ok = 0;
			break;
		}
		mode = le16toh(inode.mode);
		links_count = le16toh(inode.links_count);
		if (!mode) {
			report("orphan list points to a free inode");
			ok = 0;
			break;
		}
		if (links_count != 0) {
			report("orphan inode still has non-zero link count");
			ok = 0;
			break;
		}
		next_orphan = le64toh(((const struct cryexts_inode_extra *)
			(inode.reserved + sizeof(inode.reserved) -
			 sizeof(struct cryexts_inode_extra)))->next_orphan);
		if (next_orphan == head) {
			report("orphan inode points to itself");
			ok = 0;
			break;
		}
		head = next_orphan;
	}

	if (head && guard >= inodes_count) {
		report("orphan list is too long");
		ok = 0;
	}
	if (ok) {
		report("orphan cleanup pending");
		ok = 0;
	}

	free(seen);
	return ok ? 0 : -1;
}

static int validate_dir_block(int fd, const struct cryexts_super_block *sb,
			      const struct cryexts_group_desc *groups,
			      const struct cryexts_inode *inode)
{
	unsigned char block[CRYEXTS_BLOCK_SIZE];
	unsigned char index_buf[CRYEXTS_BLOCK_SIZE];
	const struct cryexts_dir_index_block *index = NULL;
	uint64_t size = le64toh(inode->size);
	uint64_t dir_blocks = size / CRYEXTS_BLOCK_SIZE;
	uint64_t dir_index_block = le64toh(inode->indirect_block);
	uint32_t inode_flags = le32toh(inode->inode_flags);
	uint32_t live_entries = 0;
	int seen_dot = 0;
	int seen_dotdot = 0;

	if (inode_flags & CRYEXTS_INODE_FLAG_DIR_INDEX) {
		if (read_full(fd, index_buf, sizeof(index_buf),
			      dir_index_block * CRYEXTS_BLOCK_SIZE) < 0) {
			perror("read dir index block");
			return -1;
		}
		index = (const struct cryexts_dir_index_block *)index_buf;
		if (le32toh(index->magic) != CRYEXTS_DIR_INDEX_MAGIC ||
		    le16toh(index->buckets) != CRYEXTS_DIR_INDEX_BUCKETS ||
		    le16toh(index->dir_blocks) != dir_blocks) {
			report("bad directory index block");
			return -1;
		}
		if (metadata_csum_enabled(sb) &&
		    le32toh(*(__le32 *)index->reserved) !=
			    dir_index_expected_checksum(sb, dir_index_block, index)) {
			report("directory index checksum mismatch");
			return -1;
		}
	}

	for (uint64_t i = 0; i < dir_blocks; i++) {
		unsigned int offset = 0;
		uint64_t data_block = le64toh(inode->block[i]);

		if (read_full(fd, block, sizeof(block),
			      data_block * CRYEXTS_BLOCK_SIZE) < 0) {
			perror("read dir block");
			return -1;
		}

		while (offset < CRYEXTS_BLOCK_SIZE) {
			struct cryexts_dir_entry *de;
			unsigned int rec_len;
			uint64_t ino;

			de = (struct cryexts_dir_entry *)(block + offset);
			rec_len = le16toh(de->rec_len);
			ino = le64toh(de->inode);
			if (rec_len < CRYEXTS_DIR_ENTRY_HEADER_SIZE || rec_len % 4 ||
			    offset + rec_len > CRYEXTS_BLOCK_SIZE) {
				report("bad directory rec_len");
				return -1;
			}
			if (de->name_len > rec_len - CRYEXTS_DIR_ENTRY_HEADER_SIZE) {
				report("bad directory name_len");
				return -1;
			}
			if (ino && (ino < CRYEXTS_ROOT_INO || ino > max_inodes(sb))) {
				report("directory entry points to invalid inode");
				return -1;
			}
			if (ino) {
				struct cryexts_inode target;

				if (read_inode(fd, sb, groups, ino, &target) < 0) {
					perror("read referenced inode");
					return -1;
				}
				if (!le16toh(target.mode)) {
					report("directory entry points to a free inode");
					return -1;
				}
			}
			if (de->file_type != CRYEXTS_FT_UNKNOWN &&
			    de->file_type != CRYEXTS_FT_REG_FILE &&
			    de->file_type != CRYEXTS_FT_DIR &&
			    de->file_type != CRYEXTS_FT_SYMLINK) {
				report("bad directory file_type");
				return -1;
			}
			if (ino && de->name_len == 1 && de->name[0] == '.')
				seen_dot = 1;
			if (ino && de->name_len == 2 && de->name[0] == '.' &&
			    de->name[1] == '.')
				seen_dotdot = 1;
			if (ino && de->name_len)
				live_entries++;
			if (ino && de->name_len && index) {
				uint32_t bucket = dir_hash(sb, de->name, de->name_len) %
						  CRYEXTS_DIR_INDEX_BUCKETS;
				uint16_t mask = le16toh(index->block_masks[bucket]);

				if (!(mask & (1U << i))) {
					report("directory index misses a live dirent block");
					return -1;
				}
			}
			offset += rec_len;
		}
	}

	if (index && le32toh(index->entries) != live_entries) {
		report("directory index entry count mismatch");
		return -1;
	}
	if (!seen_dot || !seen_dotdot) {
		report("directory missing dot entries");
		return -1;
	}
	return 0;
}

static int repair_super_counts(int fd, const struct cryexts_super_block *sb,
			       const struct cryexts_group_desc *groups,
			       const unsigned char *inode_seen,
			       const unsigned char *block_seen,
			       unsigned char *super_block)
{
	uint64_t used_inodes = 0;
	uint64_t used_blocks = reserved_metadata_blocks(sb, groups);
	uint64_t blocks_count = le64toh(sb->blocks_count);
	uint64_t inodes_count = le64toh(sb->inodes_count);
	struct cryexts_super_block *wsb = (struct cryexts_super_block *)super_block;

	for (uint64_t i = 0; i < inodes_count; i++) {
		if (inode_seen[i])
			used_inodes++;
	}
	for (uint64_t i = 0; i < blocks_count; i++) {
		if (block_seen[i])
			used_blocks++;
	}

	if (le64toh(sb->free_inodes_count) != inodes_count - used_inodes ||
	    le64toh(sb->free_blocks_count) != blocks_count - used_blocks) {
		wsb->free_inodes_count = htole64(inodes_count - used_inodes);
		wsb->free_blocks_count = htole64(blocks_count - used_blocks);
		if (repair_mode) {
			if (write_full(fd, super_block, CRYEXTS_BLOCK_SIZE, 0) < 0) {
				perror("write superblock");
				return -1;
			}
			printf("cryextsck: repaired superblock free counts\n");
		} else {
			report("superblock free counts mismatch");
			return -1;
		}
	}
	return 0;
}

static int repair_bitmaps(int fd, const struct cryexts_super_block *sb,
			  const struct cryexts_group_desc *groups,
			  const unsigned char *inode_seen,
			  const unsigned char *block_seen,
			  unsigned char *inode_bitmap,
			  unsigned char *block_bitmap)
{
	uint64_t blocks_count = le64toh(sb->blocks_count);
	uint64_t inodes_count = le64toh(sb->inodes_count);
	int dirty = 0;

	if (has_block_groups(sb)) {
		for (uint64_t group = 0; group < le64toh(sb->group_count); group++) {
			unsigned char group_block_bitmap[CRYEXTS_BLOCK_SIZE];
			unsigned char group_inode_bitmap[CRYEXTS_BLOCK_SIZE];
			uint64_t group_start = le64toh(groups[group].group_start);
			uint64_t group_blocks = le64toh(groups[group].blocks_count);
			uint64_t group_end = group_start + group_blocks;
			uint64_t block_bitmap_off =
				le64toh(groups[group].block_bitmap_block) * CRYEXTS_BLOCK_SIZE;
			uint64_t inode_bitmap_off =
				le64toh(groups[group].inode_bitmap_block) * CRYEXTS_BLOCK_SIZE;
			uint64_t reserved_end = group_data_start(sb, groups, group);
			int group_dirty = 0;

			if (read_full(fd, group_block_bitmap, sizeof(group_block_bitmap),
				      block_bitmap_off) < 0) {
				perror("read group block bitmap");
				return -1;
			}
			if (read_full(fd, group_inode_bitmap, sizeof(group_inode_bitmap),
				      inode_bitmap_off) < 0) {
				perror("read group inode bitmap");
				return -1;
			}

			for (uint64_t block = group_start; block < reserved_end; block++) {
				uint64_t bit = block - group_start;

				if (!bitmap_test(group_block_bitmap, bit)) {
					bitmap_set(group_block_bitmap, bit);
					group_dirty = 1;
				}
			}
			if (has_journal(sb)) {
				uint64_t journal_start = le64toh(sb->journal_block);
				uint64_t journal_end =
					journal_start + le64toh(sb->journal_blocks);
				uint64_t overlap_start =
					journal_start > group_start ? journal_start : group_start;
				uint64_t overlap_end =
					journal_end < group_end ? journal_end : group_end;

				for (uint64_t block = overlap_start;
				     block < overlap_end; block++) {
					uint64_t bit = block - group_start;

					if (!bitmap_test(group_block_bitmap, bit)) {
						bitmap_set(group_block_bitmap, bit);
						group_dirty = 1;
					}
				}
			}
			for (uint64_t ino = 0; ino < group_inode_limit(sb, group); ino++) {
				uint64_t global_ino = group * le64toh(sb->inodes_per_group) + ino;

				if (global_ino >= inodes_count)
					break;
				if (inode_seen[global_ino] &&
				    !bitmap_test(group_inode_bitmap, ino)) {
					bitmap_set(group_inode_bitmap, ino);
					group_dirty = 1;
				}
			}
			for (uint64_t block = group_start;
			     block < group_start + group_blocks && block < blocks_count;
			     block++) {
				uint64_t bit = block - group_start;

				if (block_seen[block] &&
				    !bitmap_test(group_block_bitmap, bit)) {
					bitmap_set(group_block_bitmap, bit);
					group_dirty = 1;
				}
			}

			if (!group_dirty)
				continue;
			dirty = 1;
			if (!repair_mode)
				continue;
			if (write_full(fd, group_block_bitmap, CRYEXTS_BLOCK_SIZE,
				       block_bitmap_off) < 0) {
				perror("write group block bitmap");
				return -1;
			}
			if (write_full(fd, group_inode_bitmap, CRYEXTS_BLOCK_SIZE,
				       inode_bitmap_off) < 0) {
				perror("write group inode bitmap");
				return -1;
			}
		}

		if (!dirty)
			return 0;
		if (!repair_mode) {
			report("bitmap mismatch");
			return -1;
		}
		printf("cryextsck: repaired bitmap references\n");
		return 0;
	}

	for (uint64_t i = 0; i < le64toh(sb->first_data_block); i++) {
		if (!bitmap_test(block_bitmap, i)) {
			bitmap_set(block_bitmap, i);
			dirty = 1;
		}
	}
	for (uint64_t i = 0; i < inodes_count; i++) {
		if (inode_seen[i] && !bitmap_test(inode_bitmap, i)) {
			bitmap_set(inode_bitmap, i);
			dirty = 1;
		}
	}
	for (uint64_t i = 0; i < blocks_count; i++) {
		if (block_seen[i] && !bitmap_test(block_bitmap, i)) {
			bitmap_set(block_bitmap, i);
			dirty = 1;
		}
	}

	if (!dirty)
		return 0;
	if (!repair_mode) {
		report("bitmap mismatch");
		return -1;
	}
	if (write_full(fd, block_bitmap, CRYEXTS_BLOCK_SIZE,
		       le64toh(sb->block_bitmap_block) * CRYEXTS_BLOCK_SIZE) < 0) {
		perror("write block bitmap");
		return -1;
	}
	if (write_full(fd, inode_bitmap, CRYEXTS_BLOCK_SIZE,
		       le64toh(sb->inode_bitmap_block) * CRYEXTS_BLOCK_SIZE) < 0) {
		perror("write inode bitmap");
		return -1;
	}
	printf("cryextsck: repaired bitmap references\n");
	return 0;
}

static int repair_recovery_state(int fd, unsigned char *super_block,
				 unsigned char *journal_header_block)
{
	struct cryexts_super_block *wsb =
		(struct cryexts_super_block *)(super_block + CRYEXTS_SUPER_OFFSET);
	struct cryexts_journal_header *jh =
		(struct cryexts_journal_header *)journal_header_block;
	uint32_t state = le32toh(wsb->state);
	uint32_t incompat = le32toh(wsb->features_incompat);
	uint32_t flags;
	uint32_t entries;
	int super_dirty = 0;
	int journal_dirty = 0;

	if (!has_journal(wsb))
		return 0;
	if (has_journal_v2(wsb))
		return 0;

	flags = le32toh(jh->flags);
	entries = le32toh(jh->entry_count);

	if ((state & CRYEXTS_FS_STATE_NEEDS_RECOVERY) &&
	    le32toh(jh->magic) != CRYEXTS_JOURNAL_MAGIC) {
		memset(journal_header_block, 0, CRYEXTS_BLOCK_SIZE);
		jh->magic = htole32(CRYEXTS_JOURNAL_MAGIC);
		jh->checksum = htole32(journal_checksum(journal_header_block,
							CRYEXTS_BLOCK_SIZE));
		state &= ~CRYEXTS_FS_STATE_NEEDS_RECOVERY;
		incompat &= ~CRYEXTS_FEATURE_INCOMPAT_NEEDS_RECOVERY;
		super_dirty = 1;
		journal_dirty = 1;
	}

	if (le32toh(jh->magic) == CRYEXTS_JOURNAL_MAGIC &&
	    !(flags & CRYEXTS_JOURNAL_FLAG_VALID) && !entries &&
	    (state & CRYEXTS_FS_STATE_NEEDS_RECOVERY)) {
		state &= ~CRYEXTS_FS_STATE_NEEDS_RECOVERY;
		incompat &= ~CRYEXTS_FEATURE_INCOMPAT_NEEDS_RECOVERY;
		super_dirty = 1;
	}

	if (le32toh(jh->magic) == CRYEXTS_JOURNAL_MAGIC &&
	    journal_checksum(journal_header_block, CRYEXTS_BLOCK_SIZE) !=
		    le32toh(jh->checksum) &&
	    !(flags & CRYEXTS_JOURNAL_FLAG_VALID)) {
		memset(journal_header_block, 0, CRYEXTS_BLOCK_SIZE);
		jh->magic = htole32(CRYEXTS_JOURNAL_MAGIC);
		jh->checksum = htole32(journal_checksum(journal_header_block,
							CRYEXTS_BLOCK_SIZE));
		journal_dirty = 1;
		if (state & CRYEXTS_FS_STATE_NEEDS_RECOVERY) {
			state &= ~CRYEXTS_FS_STATE_NEEDS_RECOVERY;
			incompat &= ~CRYEXTS_FEATURE_INCOMPAT_NEEDS_RECOVERY;
			super_dirty = 1;
		}
	}

	if (!super_dirty && !journal_dirty)
		return 0;

	wsb->state = htole32(state);
	wsb->features_incompat = htole32(incompat);
	if (super_dirty) {
		if (write_full(fd, super_block, CRYEXTS_BLOCK_SIZE, 0) < 0) {
			perror("write superblock");
			return -1;
		}
	}
	if (journal_dirty) {
		if (write_full(fd, journal_header_block, CRYEXTS_BLOCK_SIZE,
			       le64toh(wsb->journal_block) * CRYEXTS_BLOCK_SIZE) < 0) {
			perror("write journal header");
			return -1;
		}
	}
	printf("cryextsck: repaired recovery state / journal header\n");
	return 0;
}

int main(int argc, char **argv)
{
	unsigned char block[CRYEXTS_BLOCK_SIZE];
	unsigned char gdt_block[CRYEXTS_BLOCK_SIZE];
	unsigned char journal_header_block[CRYEXTS_BLOCK_SIZE];
	unsigned char policy_table_block[CRYEXTS_BLOCK_SIZE];
	struct cryexts_super_block *sb;
	struct cryexts_group_desc *groups = NULL;
	struct cryexts_policy_table_block *policy_table = NULL;
	unsigned char inode_bitmap[CRYEXTS_BLOCK_SIZE];
	unsigned char block_bitmap[CRYEXTS_BLOCK_SIZE];
	unsigned char *inode_seen = NULL;
	unsigned char *block_seen = NULL;
	uint64_t blocks_count;
	uint64_t inodes_count;
	int fd;
	int argi = 1;

	if (argc < 2) {
		fprintf(stderr, "Usage: %s <image-or-device>\n", argv[0]);
		return 2;
	}

	if (argc >= 3 && !strcmp(argv[1], "--repair")) {
		repair_mode = 1;
		argi++;
	}

	if (argc - argi != 1) {
		fprintf(stderr, "Usage: %s [--repair] <image-or-device>\n", argv[0]);
		return 2;
	}

	fd = open(argv[argi], repair_mode ? O_RDWR : O_RDONLY);
	if (fd < 0) {
		perror("open");
		return 2;
	}

	if (read_full(fd, block, sizeof(block), 0) < 0) {
		perror("read superblock");
		close(fd);
		return 2;
	}

	sb = (struct cryexts_super_block *)(block + CRYEXTS_SUPER_OFFSET);
	validate_super(sb);
	if (errors)
		goto out;
	if (has_block_groups(sb)) {
		if (le64toh(sb->group_desc_table_blocks) > 1) {
			report("multi-block GDT is not yet supported by cryextsck");
			goto out;
		}
		if (read_full(fd, gdt_block, sizeof(gdt_block),
			      le64toh(sb->group_desc_table_start) *
				      CRYEXTS_BLOCK_SIZE) < 0) {
			perror("read group descriptor table");
			errors++;
			goto out;
		}
		groups = (struct cryexts_group_desc *)gdt_block;
		validate_groups(sb, groups);
		if (errors)
			goto out;
	}
	validate_v5_layout_after_groups(sb, groups);
	if (errors)
		goto out;
	validate_orphan_list(fd, sb, groups);
	if (errors)
		goto out;
	if (has_journal(sb)) {
		if (read_full(fd, journal_header_block, sizeof(journal_header_block),
			      le64toh(sb->journal_block) * CRYEXTS_BLOCK_SIZE) < 0) {
			perror("read journal header");
			errors++;
			goto out;
		}
		validate_journal_header(fd, sb, groups, journal_header_block);
		if (errors && !repair_mode)
			goto out;
	}

	blocks_count = le64toh(sb->blocks_count);
	inodes_count = le64toh(sb->inodes_count);
	inode_seen = calloc(inodes_count ? inodes_count : 1, 1);
	block_seen = calloc(blocks_count ? blocks_count : 1, 1);
	if (!inode_seen || !block_seen) {
		perror("calloc");
		errors++;
		goto out;
	}
	memset(inode_bitmap, 0, sizeof(inode_bitmap));
	memset(block_bitmap, 0, sizeof(block_bitmap));
	validate_reserved_bitmaps(fd, sb, groups);
	if (errors)
		goto out;
	if (warnings_found && !repair_mode)
		goto out;
	if (validate_policy_table(fd, sb, groups, block_seen, blocks_count,
				  policy_table_block) < 0)
		goto out;
	if (le32toh(sb->features_incompat) &
	    CRYEXTS_FEATURE_INCOMPAT_POLICY_TABLE)
		policy_table =
			(struct cryexts_policy_table_block *)policy_table_block;

	if (!has_block_groups(sb)) {
		if (read_full(fd, block_bitmap, sizeof(block_bitmap),
			      le64toh(sb->block_bitmap_block) *
				      CRYEXTS_BLOCK_SIZE) < 0) {
			perror("read block bitmap");
			goto out;
		}
		if (read_full(fd, inode_bitmap, sizeof(inode_bitmap),
			      le64toh(sb->inode_bitmap_block) *
				      CRYEXTS_BLOCK_SIZE) < 0) {
			perror("read inode bitmap");
			goto out;
		}
	}

	for (uint64_t ino = CRYEXTS_ROOT_INO; ino <= inodes_count; ino++) {
		struct cryexts_inode inode;

		if (read_inode(fd, sb, groups, ino, &inode) < 0) {
			perror("read inode");
			errors++;
			break;
		}
		if (validate_inode(fd, sb, groups, &inode, ino, inode_seen,
				   block_seen, blocks_count, policy_table) < 0)
			continue;
		if ((le16toh(inode.mode) & 0170000) == 0040000)
			validate_dir_block(fd, sb, groups, &inode);
	}

	if (repair_super_counts(fd, sb, groups, inode_seen, block_seen, block) < 0)
		goto out;
	sb = (struct cryexts_super_block *)(block + CRYEXTS_SUPER_OFFSET);
	if (repair_bitmaps(fd, sb, groups, inode_seen, block_seen, inode_bitmap,
			   block_bitmap) < 0)
		goto out;
	if (repair_mode && has_journal(sb)) {
		if (repair_recovery_state(fd, block, journal_header_block) < 0)
			goto out;
		sb = (struct cryexts_super_block *)(block + CRYEXTS_SUPER_OFFSET);
	}

out:
	free(inode_seen);
	free(block_seen);
	close(fd);
	if (errors) {
		fprintf(stderr, "cryextsck: found %d error(s)\n", errors);
		return 1;
	}
	printf("cryextsck: %s clean%s\n", argv[argi],
	       le32toh(sb->flags) & CRYEXTS_SB_FLAG_ENCRYPTED ?
		       " (encrypted data blocks)" :
		       "");
	return 0;
}
