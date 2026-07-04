// SPDX-License-Identifier: GPL-2.0
#include "cryexts.h"

#define CRYEXTS_META_TAG_SUPER 0x53555052U
#define CRYEXTS_META_TAG_GROUP 0x47525044U
#define CRYEXTS_META_TAG_POLICY 0x504f4c59U
#define CRYEXTS_META_TAG_DIRIDX 0x44495258U
#define CRYEXTS_META_TAG_EXTOVF 0x45584f46U

static u32 cryexts_metadata_fnv1a_bytes(const void *buf, size_t len, u32 hash)
{
	const u8 *bytes = buf;
	size_t i;

	for (i = 0; i < len; i++) {
		hash ^= bytes[i];
		hash *= 16777619u;
	}
	return hash;
}

static u32 cryexts_metadata_checksum_skip(const void *buf, size_t len,
					  size_t skip_offset, size_t skip_len,
					  u32 seed)
{
	const u8 *bytes = buf;
	u32 hash = seed;
	size_t i;

	for (i = 0; i < len; i++) {
		if (i >= skip_offset && i < skip_offset + skip_len)
			continue;
		hash ^= bytes[i];
		hash *= 16777619u;
	}
	return hash;
}

static u32 cryexts_metadata_seed(u64 fs_generation, u64 block, u32 tag)
{
	u32 hash = 2166136261u;

	hash = cryexts_metadata_fnv1a_bytes(&fs_generation,
					    sizeof(fs_generation), hash);
	hash = cryexts_metadata_fnv1a_bytes(&block, sizeof(block), hash);
	hash = cryexts_metadata_fnv1a_bytes(&tag, sizeof(tag), hash);
	return hash;
}

static bool cryexts_metadata_csum_enabled_disk(struct cryexts_super_block *disk_sb)
{
	if (le32_to_cpu(disk_sb->version) < CRYEXTS_VERSION_V5)
		return false;
	if (!(le32_to_cpu(disk_sb->features_ro_compat) &
	      CRYEXTS_FEATURE_RO_COMPAT_METADATA_CSUM))
		return false;
	return le64_to_cpu(disk_sb->metadata_csum_type) ==
	       CRYEXTS_METADATA_CSUM_FNV1A32;
}

static u32 cryexts_super_expected_checksum(struct cryexts_super_block *disk_sb)
{
	u32 seed;

	seed = cryexts_metadata_seed(le64_to_cpu(disk_sb->fs_generation), 0,
				     CRYEXTS_META_TAG_SUPER);
	return cryexts_metadata_checksum_skip(
		disk_sb, sizeof(*disk_sb),
		offsetof(struct cryexts_super_block, reserved), sizeof(__le32),
		seed);
}

static u32 cryexts_group_expected_checksum(struct cryexts_super_block *disk_sb,
					   struct cryexts_group_desc *group)
{
	u32 seed;

	seed = cryexts_metadata_seed(le64_to_cpu(disk_sb->fs_generation),
				     le64_to_cpu(group->group_start),
				     CRYEXTS_META_TAG_GROUP);
	return cryexts_metadata_checksum_skip(
		group, sizeof(*group),
		offsetof(struct cryexts_group_desc, reserved), sizeof(__le32),
		seed);
}

static u32 cryexts_policy_expected_checksum(struct cryexts_super_block *disk_sb,
					    u64 block,
					    const struct cryexts_policy_table_block *pt)
{
	u32 seed;

	seed = cryexts_metadata_seed(le64_to_cpu(disk_sb->fs_generation), block,
				     CRYEXTS_META_TAG_POLICY);
	return cryexts_metadata_checksum_skip(
		pt, CRYEXTS_BLOCK_SIZE,
		offsetof(struct cryexts_policy_table_block, reserved), sizeof(__le32),
		seed);
}

static u32 cryexts_dir_index_expected_checksum(struct cryexts_super_block *disk_sb,
					       u64 block,
					       const struct cryexts_dir_index_block *index)
{
	u32 seed;

	seed = cryexts_metadata_seed(le64_to_cpu(disk_sb->fs_generation), block,
				     CRYEXTS_META_TAG_DIRIDX);
	return cryexts_metadata_checksum_skip(
		index, CRYEXTS_BLOCK_SIZE,
		offsetof(struct cryexts_dir_index_block, reserved), sizeof(__le32),
		seed);
}

bool cryexts_metadata_csum_enabled(struct super_block *sb)
{
	return cryexts_metadata_csum_enabled_disk(CRYEXTS_SB(sb)->disk_sb);
}

void cryexts_update_super_checksum(struct super_block *sb)
{
	struct cryexts_super_block *disk_sb = CRYEXTS_SB(sb)->disk_sb;
	__le32 *checksum;

	if (!cryexts_metadata_csum_enabled_disk(disk_sb))
		return;

	checksum = (__le32 *)disk_sb->reserved;
	*checksum = cpu_to_le32(cryexts_super_expected_checksum(disk_sb));
}

int cryexts_verify_super_checksum(struct super_block *sb)
{
	struct cryexts_super_block *disk_sb = CRYEXTS_SB(sb)->disk_sb;
	u32 stored;
	u32 expected;

	if (!cryexts_metadata_csum_enabled_disk(disk_sb))
		return 0;

	stored = le32_to_cpu(*(__le32 *)disk_sb->reserved);
	expected = cryexts_super_expected_checksum(disk_sb);
	return stored == expected ? 0 : -EUCLEAN;
}

void cryexts_update_group_checksums(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	u64 group;

	if (!sbi->groups || !cryexts_metadata_csum_enabled(sb))
		return;

	for (group = 0; group < sbi->group_count; group++) {
		__le32 *checksum = (__le32 *)sbi->groups[group].reserved;

		*checksum = cpu_to_le32(cryexts_group_expected_checksum(
			sbi->disk_sb, &sbi->groups[group]));
	}
}

int cryexts_verify_group_checksums(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	u64 group;

	if (!sbi->groups || !cryexts_metadata_csum_enabled(sb))
		return 0;

	for (group = 0; group < sbi->group_count; group++) {
		u32 stored;
		u32 expected;

		stored = le32_to_cpu(*(__le32 *)sbi->groups[group].reserved);
		expected = cryexts_group_expected_checksum(
			sbi->disk_sb, &sbi->groups[group]);
		if (stored != expected)
			return -EUCLEAN;
	}

	return 0;
}

void cryexts_dir_index_set_checksum(struct super_block *sb, u64 block,
				    struct cryexts_dir_index_block *index)
{
	__le32 *checksum;

	if (!cryexts_metadata_csum_enabled(sb))
		return;

	checksum = (__le32 *)index->reserved;
	*checksum = cpu_to_le32(cryexts_dir_index_expected_checksum(
		CRYEXTS_SB(sb)->disk_sb, block, index));
}

bool cryexts_dir_index_checksum_valid(struct super_block *sb, u64 block,
				      const struct cryexts_dir_index_block *index)
{
	u32 stored;
	u32 expected;

	if (!cryexts_metadata_csum_enabled(sb))
		return true;

	stored = le32_to_cpu(*(__le32 *)index->reserved);
	expected = cryexts_dir_index_expected_checksum(
		CRYEXTS_SB(sb)->disk_sb, block, index);
	return stored == expected;
}

bool cryexts_policy_table_checksum_valid(
	struct super_block *sb, u64 block,
	const struct cryexts_policy_table_block *pt)
{
	u32 stored;
	u32 expected;

	if (!cryexts_metadata_csum_enabled(sb))
		return true;

	stored = le32_to_cpu(*(__le32 *)pt->reserved);
	expected = cryexts_policy_expected_checksum(
		CRYEXTS_SB(sb)->disk_sb, block, pt);
	return stored == expected;
}

u32 cryexts_extent_overflow_checksum(struct super_block *sb, u64 block,
				     const void *buf)
{
	u32 seed;

	seed = cryexts_metadata_seed(
		le64_to_cpu(CRYEXTS_SB(sb)->disk_sb->fs_generation), block,
		CRYEXTS_META_TAG_EXTOVF);
	return cryexts_metadata_fnv1a_bytes(buf, CRYEXTS_BLOCK_SIZE, seed);
}

u32 cryexts_extent_leaf_checksum(struct super_block *sb, u64 block,
				 const void *buf)
{
	return cryexts_extent_overflow_checksum(sb, block, buf);
}
