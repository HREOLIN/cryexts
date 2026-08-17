// SPDX-License-Identifier: GPL-2.0
#include "cryexts.h"

static u32 cryexts_checksum_skip(const void *buf, size_t len,
				 size_t skip_offset, size_t skip_len)
{
	const u8 *bytes = buf;
	u32 hash = 2166136261u;
	size_t i;

	for (i = 0; i < len; i++) {
		if (i >= skip_offset && i < skip_offset + skip_len)
			continue;
		hash ^= bytes[i];
		hash *= 16777619u;
	}
	return hash;
}

static u32 cryexts_fnv1a_update(u32 hash, const void *buf, size_t len)
{
	const u8 *bytes = buf;
	size_t i;

	for (i = 0; i < len; i++) {
		hash ^= bytes[i];
		hash *= 16777619u;
	}
	return hash;
}

u32 cryexts_journal_checksum(const void *buf, size_t len)
{
	return cryexts_checksum_skip(buf, len,
				     CRYEXTS_JOURNAL_CHECKSUM_OFFSET,
				     sizeof(__le32));
}

bool cryexts_journal_uses_v2(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);

	return sbi && sbi->journal_enabled && sbi->journal_v2;
}

static bool cryexts_journal_uses_v3(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);

	return sbi && sbi->journal_enabled && sbi->journal_v3;
}

static u32 cryexts_journal_v3_features(struct cryexts_sb_info *sbi);
static u64 cryexts_journal_v3_ring_start(struct cryexts_sb_info *sbi);
static bool cryexts_journal_v3_ring_valid(
	struct cryexts_sb_info *sbi,
	const struct cryexts_journal_v3_control *control);

static u32 cryexts_journal_v2_checksum(const void *buf, size_t len,
				       size_t checksum_offset)
{
	return cryexts_checksum_skip(buf, len, checksum_offset, sizeof(__le32));
}

static bool cryexts_journal_block_is_internal(struct cryexts_sb_info *sbi,
					      u64 block)
{
	return block >= sbi->journal_block &&
	       block < sbi->journal_block + sbi->journal_blocks;
}

static u64 cryexts_journal_v2_descriptor_block(struct cryexts_sb_info *sbi)
{
	return sbi->journal_block + 1;
}

static u64 cryexts_journal_v2_payload_start(struct cryexts_sb_info *sbi)
{
	return sbi->journal_block + 2;
}

static u64 cryexts_journal_v2_payload_area_blocks(struct cryexts_sb_info *sbi)
{
	if (sbi->journal_blocks < CRYEXTS_JOURNAL_V2_MIN_BLOCKS)
		return 0;
	return sbi->journal_blocks - 3;
}

static u64 cryexts_journal_v2_commit_block(struct cryexts_sb_info *sbi)
{
	return sbi->journal_block + sbi->journal_blocks - 1;
}

static u32 cryexts_journal_v2_payload_capacity(struct cryexts_sb_info *sbi)
{
	if (sbi->journal_blocks < CRYEXTS_JOURNAL_V2_MIN_BLOCKS)
		return 0;
	return min_t(u64, CRYEXTS_JOURNAL_V2_MAX_ENTRIES,
		     sbi->journal_blocks - 3);
}

static void cryexts_journal_prepare_header(char *buf, u32 flags,
					   u32 entries, u64 sequence)
{
	struct cryexts_journal_header *jh;

	memset(buf, 0, CRYEXTS_BLOCK_SIZE);
	jh = (struct cryexts_journal_header *)buf;
	jh->magic = cpu_to_le32(CRYEXTS_JOURNAL_MAGIC);
	jh->flags = cpu_to_le32(flags);
	jh->entry_count = cpu_to_le32(entries);
	jh->sequence = cpu_to_le64(sequence);
	jh->checksum = cpu_to_le32(cryexts_journal_checksum(buf,
						CRYEXTS_BLOCK_SIZE));
}

static bool cryexts_journal_header_valid(struct super_block *sb,
					 struct cryexts_journal_header *jh,
					 u32 *entries_out)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	u32 entries;
	u32 stored;
	u32 expected;
	unsigned int i;

	if (le32_to_cpu(jh->magic) != CRYEXTS_JOURNAL_MAGIC)
		return false;
	entries = le32_to_cpu(jh->entry_count);
	if (entries > CRYEXTS_JOURNAL_MAX_ENTRIES ||
	    entries + 1 > sbi->journal_blocks)
		return false;
	stored = le32_to_cpu(jh->checksum);
	expected = cryexts_journal_checksum(jh, CRYEXTS_BLOCK_SIZE);
	if (stored != expected)
		return false;
	for (i = entries; i < CRYEXTS_JOURNAL_MAX_ENTRIES; i++) {
		if (le64_to_cpu(jh->home_blocks[i]))
			return false;
	}
	if (entries_out)
		*entries_out = entries;
	return true;
}

bool cryexts_journal_needs_recovery(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	u32 state = le32_to_cpu(sbi->disk_sb->state);
	u32 incompat = le32_to_cpu(sbi->disk_sb->features_incompat);

	return !!(state & CRYEXTS_FS_STATE_NEEDS_RECOVERY) ||
	       !!(incompat & CRYEXTS_FEATURE_INCOMPAT_NEEDS_RECOVERY);
}

void cryexts_super_set_recovery(struct super_block *sb, bool needed)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	u32 state = le32_to_cpu(sbi->disk_sb->state);
	u32 incompat = le32_to_cpu(sbi->disk_sb->features_incompat);

	if (needed) {
		state |= CRYEXTS_FS_STATE_NEEDS_RECOVERY;
		incompat |= CRYEXTS_FEATURE_INCOMPAT_NEEDS_RECOVERY;
	} else {
		state &= ~CRYEXTS_FS_STATE_NEEDS_RECOVERY;
		incompat &= ~CRYEXTS_FEATURE_INCOMPAT_NEEDS_RECOVERY;
	}

	sbi->disk_sb->state = cpu_to_le32(state);
	sbi->disk_sb->features_incompat = cpu_to_le32(incompat);
	cryexts_mark_super_dirty(sb);
}

static int cryexts_journal_read_header(struct super_block *sb, char *buf)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);

	if (!sbi->journal_enabled || !sbi->journal_blocks)
		return -EOPNOTSUPP;
	return cryexts_read_file_block(sb, sbi->journal_block, buf);
}

static int cryexts_journal_write_header(struct super_block *sb, const char *buf)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);

	if (!sbi->journal_enabled || !sbi->journal_blocks)
		return -EOPNOTSUPP;
	return cryexts_write_file_block(sb, sbi->journal_block, buf);
}

static int cryexts_sync_single_block(struct super_block *sb, u64 block)
{
	struct buffer_head *bh;
	int err;

	bh = sb_getblk(sb, block);
	if (!bh)
		return -EIO;

	err = sync_dirty_buffer(bh);
	brelse(bh);
	return err;
}

static void cryexts_journal_v2_set_sequence(struct super_block *sb, u64 sequence)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);

	sbi->journal_sequence = sequence;
	sbi->journal_last_sequence = sequence;
	sbi->disk_sb->journal_sequence = cpu_to_le64(sequence);
	cryexts_mark_super_dirty(sb);
}

static void cryexts_journal_v2_set_state(struct cryexts_sb_info *sbi,
					 u64 last_sequence,
					 u64 active_sequence,
					 u64 tail_sequence,
					 u64 checkpoint_sequence)
{
	sbi->journal_last_sequence = last_sequence;
	sbi->journal_active_sequence = active_sequence;
	sbi->journal_tail_sequence = tail_sequence;
	sbi->journal_checkpoint_sequence = checkpoint_sequence;
}

static void cryexts_journal_v2_prepare_control(struct super_block *sb,
					       char *buf, u32 flags,
					       u64 active_sequence,
					       u64 last_sequence,
					       u64 tail_sequence,
					       u64 checkpoint_sequence)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	struct cryexts_journal_v2_control *jc;

	memset(buf, 0, CRYEXTS_BLOCK_SIZE);
	jc = (struct cryexts_journal_v2_control *)buf;
	jc->magic = cpu_to_le32(CRYEXTS_JOURNAL_V2_MAGIC);
	jc->layout_version = cpu_to_le16(CRYEXTS_JOURNAL_V2_LAYOUT_VERSION);
	jc->block_type = cpu_to_le16(CRYEXTS_JOURNAL_V2_BLOCK_CONTROL);
	jc->flags = cpu_to_le32(flags);
	jc->features = cpu_to_le32(CRYEXTS_JOURNAL_V2_FEATURE_BASELINE);
	jc->last_sequence = cpu_to_le64(last_sequence);
	jc->active_sequence = cpu_to_le64(active_sequence);
	jc->tail_sequence = cpu_to_le64(tail_sequence);
	jc->checkpoint_sequence = cpu_to_le64(checkpoint_sequence);
	jc->descriptor_block =
		cpu_to_le64(cryexts_journal_v2_descriptor_block(sbi));
	jc->payload_start = cpu_to_le64(cryexts_journal_v2_payload_start(sbi));
	jc->payload_blocks =
		cpu_to_le64(cryexts_journal_v2_payload_area_blocks(sbi));
	jc->commit_block = cpu_to_le64(cryexts_journal_v2_commit_block(sbi));
	jc->checksum = cpu_to_le32(cryexts_journal_v2_checksum(
		buf, CRYEXTS_BLOCK_SIZE,
		offsetof(struct cryexts_journal_v2_control, checksum)));
}

static void cryexts_journal_v2_prepare_descriptor(struct super_block *sb,
						  char *buf, u32 entries,
						  u64 sequence)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	struct cryexts_journal_v2_descriptor *jd;
	unsigned int i;

	memset(buf, 0, CRYEXTS_BLOCK_SIZE);
	jd = (struct cryexts_journal_v2_descriptor *)buf;
	jd->magic = cpu_to_le32(CRYEXTS_JOURNAL_V2_MAGIC);
	jd->layout_version = cpu_to_le16(CRYEXTS_JOURNAL_V2_LAYOUT_VERSION);
	jd->block_type = cpu_to_le16(CRYEXTS_JOURNAL_V2_BLOCK_DESCRIPTOR);
	jd->flags = cpu_to_le32(CRYEXTS_JOURNAL_V2_FLAG_ACTIVE);
	jd->entry_count = cpu_to_le32(entries);
	jd->sequence = cpu_to_le64(sequence);
	jd->payload_start = cpu_to_le64(cryexts_journal_v2_payload_start(sbi));
	jd->commit_block = cpu_to_le64(cryexts_journal_v2_commit_block(sbi));
	for (i = 0; i < entries; i++)
		jd->home_blocks[i] = cpu_to_le64(sbi->journal_home_blocks[i]);
	jd->checksum = cpu_to_le32(cryexts_journal_v2_checksum(
		buf, CRYEXTS_BLOCK_SIZE,
		offsetof(struct cryexts_journal_v2_descriptor, checksum)));
}

static void cryexts_journal_v2_prepare_descriptor_empty(struct super_block *sb,
							char *buf,
							u64 sequence)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	struct cryexts_journal_v2_descriptor *jd;

	memset(buf, 0, CRYEXTS_BLOCK_SIZE);
	jd = (struct cryexts_journal_v2_descriptor *)buf;
	jd->magic = cpu_to_le32(CRYEXTS_JOURNAL_V2_MAGIC);
	jd->layout_version = cpu_to_le16(CRYEXTS_JOURNAL_V2_LAYOUT_VERSION);
	jd->block_type = cpu_to_le16(CRYEXTS_JOURNAL_V2_BLOCK_DESCRIPTOR);
	jd->entry_count = cpu_to_le32(0);
	jd->sequence = cpu_to_le64(sequence);
	jd->payload_start = cpu_to_le64(cryexts_journal_v2_payload_start(sbi));
	jd->commit_block = cpu_to_le64(cryexts_journal_v2_commit_block(sbi));
	jd->checksum = cpu_to_le32(cryexts_journal_v2_checksum(
		buf, CRYEXTS_BLOCK_SIZE,
		offsetof(struct cryexts_journal_v2_descriptor, checksum)));
}

static void cryexts_journal_v2_prepare_commit(struct super_block *sb,
					      char *buf, u32 entries,
					      u64 sequence)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	struct cryexts_journal_v2_commit *jc;

	memset(buf, 0, CRYEXTS_BLOCK_SIZE);
	jc = (struct cryexts_journal_v2_commit *)buf;
	jc->magic = cpu_to_le32(CRYEXTS_JOURNAL_V2_MAGIC);
	jc->layout_version = cpu_to_le16(CRYEXTS_JOURNAL_V2_LAYOUT_VERSION);
	jc->block_type = cpu_to_le16(CRYEXTS_JOURNAL_V2_BLOCK_COMMIT);
	jc->flags = cpu_to_le32(CRYEXTS_JOURNAL_V2_FLAG_COMMITTED);
	jc->entry_count = cpu_to_le32(entries);
	jc->sequence = cpu_to_le64(sequence);
	jc->descriptor_block =
		cpu_to_le64(cryexts_journal_v2_descriptor_block(sbi));
	jc->checksum = cpu_to_le32(cryexts_journal_v2_checksum(
		buf, CRYEXTS_BLOCK_SIZE,
		offsetof(struct cryexts_journal_v2_commit, checksum)));
}

static void cryexts_journal_v2_prepare_commit_empty(struct super_block *sb,
						    char *buf,
						    u64 sequence)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	struct cryexts_journal_v2_commit *jc;

	memset(buf, 0, CRYEXTS_BLOCK_SIZE);
	jc = (struct cryexts_journal_v2_commit *)buf;
	jc->magic = cpu_to_le32(CRYEXTS_JOURNAL_V2_MAGIC);
	jc->layout_version = cpu_to_le16(CRYEXTS_JOURNAL_V2_LAYOUT_VERSION);
	jc->block_type = cpu_to_le16(CRYEXTS_JOURNAL_V2_BLOCK_COMMIT);
	jc->entry_count = cpu_to_le32(0);
	jc->sequence = cpu_to_le64(sequence);
	jc->descriptor_block =
		cpu_to_le64(cryexts_journal_v2_descriptor_block(sbi));
	jc->checksum = cpu_to_le32(cryexts_journal_v2_checksum(
		buf, CRYEXTS_BLOCK_SIZE,
		offsetof(struct cryexts_journal_v2_commit, checksum)));
}

static bool cryexts_journal_v2_control_valid(struct super_block *sb,
					     const struct cryexts_journal_v2_control *jc)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	u32 stored;
	u32 expected;

	if (le32_to_cpu(jc->magic) != CRYEXTS_JOURNAL_V2_MAGIC)
		return false;
	if (le16_to_cpu(jc->layout_version) !=
	    CRYEXTS_JOURNAL_V2_LAYOUT_VERSION)
		return false;
	if (le16_to_cpu(jc->block_type) != CRYEXTS_JOURNAL_V2_BLOCK_CONTROL)
		return false;
	if (le32_to_cpu(jc->features) != CRYEXTS_JOURNAL_V2_FEATURE_BASELINE)
		return false;
	if (le64_to_cpu(jc->descriptor_block) !=
	    cryexts_journal_v2_descriptor_block(sbi))
		return false;
	if (le64_to_cpu(jc->payload_start) !=
	    cryexts_journal_v2_payload_start(sbi))
		return false;
	if (le64_to_cpu(jc->payload_blocks) !=
	    cryexts_journal_v2_payload_area_blocks(sbi))
		return false;
	if (le64_to_cpu(jc->commit_block) !=
	    cryexts_journal_v2_commit_block(sbi))
		return false;
	stored = le32_to_cpu(jc->checksum);
	expected = cryexts_journal_v2_checksum(
		jc, CRYEXTS_BLOCK_SIZE,
		offsetof(struct cryexts_journal_v2_control, checksum));
	return stored == expected;
}

static bool cryexts_journal_v2_sequence_state_valid(
	const struct cryexts_journal_v2_control *jc)
{
	u64 last_sequence = le64_to_cpu(jc->last_sequence);
	u64 active_sequence = le64_to_cpu(jc->active_sequence);
	u64 tail_sequence = le64_to_cpu(jc->tail_sequence);
	u64 checkpoint_sequence = le64_to_cpu(jc->checkpoint_sequence);

	if (tail_sequence > checkpoint_sequence)
		return false;
	if (checkpoint_sequence > last_sequence)
		return false;
	if (active_sequence && active_sequence > last_sequence + 1)
		return false;
	if (!active_sequence && last_sequence &&
	    checkpoint_sequence != last_sequence)
		return false;
	if (!active_sequence && last_sequence && tail_sequence != last_sequence)
		return false;
	if (active_sequence && checkpoint_sequence > last_sequence)
		return false;
	return true;
}

static bool cryexts_journal_v2_descriptor_valid(
	struct super_block *sb, const struct cryexts_journal_v2_descriptor *jd,
	u32 *entries_out, u64 *sequence_out)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	u32 entries;
	u32 stored;
	u32 expected;
	unsigned int i;

	if (le32_to_cpu(jd->magic) != CRYEXTS_JOURNAL_V2_MAGIC)
		return false;
	if (le16_to_cpu(jd->layout_version) !=
	    CRYEXTS_JOURNAL_V2_LAYOUT_VERSION)
		return false;
	if (le16_to_cpu(jd->block_type) != CRYEXTS_JOURNAL_V2_BLOCK_DESCRIPTOR)
		return false;
	entries = le32_to_cpu(jd->entry_count);
	if (entries > cryexts_journal_v2_payload_capacity(sbi))
		return false;
	if (le64_to_cpu(jd->payload_start) !=
	    cryexts_journal_v2_payload_start(sbi))
		return false;
	if (le64_to_cpu(jd->commit_block) !=
	    cryexts_journal_v2_commit_block(sbi))
		return false;
	stored = le32_to_cpu(jd->checksum);
	expected = cryexts_journal_v2_checksum(
		jd, CRYEXTS_BLOCK_SIZE,
		offsetof(struct cryexts_journal_v2_descriptor, checksum));
	if (stored != expected)
		return false;
	for (i = entries; i < CRYEXTS_JOURNAL_V2_MAX_ENTRIES; i++) {
		if (le64_to_cpu(jd->home_blocks[i]))
			return false;
	}
	if (entries_out)
		*entries_out = entries;
	if (sequence_out)
		*sequence_out = le64_to_cpu(jd->sequence);
	return true;
}

static bool cryexts_journal_v2_commit_valid(
	struct super_block *sb, const struct cryexts_journal_v2_commit *jc,
	u32 *entries_out, u64 *sequence_out)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	u32 entries;
	u32 stored;
	u32 expected;

	if (le32_to_cpu(jc->magic) != CRYEXTS_JOURNAL_V2_MAGIC)
		return false;
	if (le16_to_cpu(jc->layout_version) !=
	    CRYEXTS_JOURNAL_V2_LAYOUT_VERSION)
		return false;
	if (le16_to_cpu(jc->block_type) != CRYEXTS_JOURNAL_V2_BLOCK_COMMIT)
		return false;
	if (!(le32_to_cpu(jc->flags) & CRYEXTS_JOURNAL_V2_FLAG_COMMITTED))
		return false;
	entries = le32_to_cpu(jc->entry_count);
	if (entries > cryexts_journal_v2_payload_capacity(sbi))
		return false;
	if (le64_to_cpu(jc->descriptor_block) !=
	    cryexts_journal_v2_descriptor_block(sbi))
		return false;
	stored = le32_to_cpu(jc->checksum);
	expected = cryexts_journal_v2_checksum(
		jc, CRYEXTS_BLOCK_SIZE,
		offsetof(struct cryexts_journal_v2_commit, checksum));
	if (stored != expected)
		return false;
	if (entries_out)
		*entries_out = entries;
	if (sequence_out)
		*sequence_out = le64_to_cpu(jc->sequence);
	return true;
}

static int cryexts_journal_v2_reset_state(struct super_block *sb,
					  u64 last_sequence)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	char *buf;
	int err;

	buf = kzalloc(CRYEXTS_BLOCK_SIZE, GFP_NOFS);
	if (!buf)
		return -ENOMEM;

	cryexts_journal_v2_set_state(sbi, last_sequence, 0,
				     last_sequence, last_sequence);
	cryexts_journal_v2_prepare_control(sb, buf, 0, 0, last_sequence,
					   last_sequence, last_sequence);
	err = cryexts_write_file_block(sb, sbi->journal_block, buf);
	if (err)
		goto out;
	err = cryexts_sync_single_block(sb, sbi->journal_block);
	if (err)
		goto out;

	cryexts_journal_v2_prepare_descriptor_empty(sb, buf, last_sequence);
	err = cryexts_write_file_block(sb,
				       cryexts_journal_v2_descriptor_block(sbi),
				       buf);
	if (err)
		goto out;
	err = cryexts_sync_single_block(sb,
					cryexts_journal_v2_descriptor_block(sbi));
	if (err)
		goto out;

	cryexts_journal_v2_prepare_commit_empty(sb, buf, last_sequence);
	err = cryexts_write_file_block(sb,
				       cryexts_journal_v2_commit_block(sbi),
				       buf);
	if (err)
		goto out;
	err = cryexts_sync_single_block(sb,
					cryexts_journal_v2_commit_block(sbi));

out:
	kfree(buf);
	return err;
}

static int cryexts_journal_v2_begin(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	u64 previous_sequence;
	u64 sequence;
	char *buf;
	int err;

	mutex_lock(&sbi->journal_lock);
	sbi->journal_entry_count = 0;
	memset(sbi->journal_home_blocks, 0, sizeof(sbi->journal_home_blocks));

	if (!cryexts_journal_v2_payload_capacity(sbi)) {
		mutex_unlock(&sbi->journal_lock);
		return -EOPNOTSUPP;
	}

	previous_sequence = sbi->journal_sequence;
	sequence = previous_sequence + 1;
	cryexts_journal_v2_set_sequence(sb, sequence);
	cryexts_journal_v2_set_state(sbi, previous_sequence, sequence,
				     previous_sequence, previous_sequence);
	cryexts_super_set_recovery(sb, true);

	buf = kzalloc(CRYEXTS_BLOCK_SIZE, GFP_NOFS);
	if (!buf) {
		cryexts_super_set_recovery(sb, false);
		mutex_unlock(&sbi->journal_lock);
		return -ENOMEM;
	}

	cryexts_journal_v2_prepare_control(sb, buf,
					   CRYEXTS_JOURNAL_V2_FLAG_ACTIVE,
					   sequence, previous_sequence,
					   previous_sequence,
					   previous_sequence);
	err = cryexts_write_file_block(sb, sbi->journal_block, buf);
	if (err)
		goto out;
	err = cryexts_sync_single_block(sb, sbi->journal_block);
	if (err)
		goto out;

	memset(buf, 0, CRYEXTS_BLOCK_SIZE);
	err = cryexts_write_file_block(sb,
				       cryexts_journal_v2_descriptor_block(sbi),
				       buf);
	if (err)
		goto out;
	err = cryexts_write_file_block(sb,
				       cryexts_journal_v2_commit_block(sbi),
				       buf);
	if (err)
		goto out;
	err = cryexts_sync_metadata(sb);
out:
	if (err) {
		pr_err("cryexts: journal v2 begin failed seq=%llu (%d)\n",
		       sequence, err);
		cryexts_journal_v2_set_sequence(sb, previous_sequence);
		cryexts_journal_v2_set_state(sbi, previous_sequence, 0,
					     previous_sequence,
					     previous_sequence);
		cryexts_super_set_recovery(sb, false);
		mutex_unlock(&sbi->journal_lock);
	}
	kfree(buf);
	return err;
}

static int cryexts_journal_v2_record_block(struct super_block *sb, u64 home_block)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	char *buf;
	char *block_buf;
	u32 capacity = cryexts_journal_v2_payload_capacity(sbi);
	u32 entries = sbi->journal_entry_count;
	unsigned int i;
	int err = 0;

	if (home_block >= cryexts_blocks_count(sb))
		return -EINVAL;
	if (!cryexts_data_block_valid(sb, home_block))
		return -EINVAL;
	if (cryexts_journal_block_is_internal(sbi, home_block))
		return -EINVAL;

	for (i = 0; i < entries; i++) {
		if (sbi->journal_home_blocks[i] == home_block)
			return 0;
	}
	if (entries >= capacity || entries >= CRYEXTS_JOURNAL_V2_MAX_ENTRIES)
		return -ENOSPC;

	buf = kzalloc(CRYEXTS_BLOCK_SIZE, GFP_NOFS);
	block_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_NOFS);
	if (!buf || !block_buf) {
		err = -ENOMEM;
		goto out;
	}

	err = cryexts_read_file_block(sb, home_block, block_buf);
	if (err)
		goto out;
	err = cryexts_write_file_block(sb,
				       cryexts_journal_v2_payload_start(sbi) +
					       entries,
				       block_buf);
	if (err)
		goto out;
	err = cryexts_sync_single_block(sb,
					cryexts_journal_v2_payload_start(sbi) +
						entries);
	if (err)
		goto out;

	sbi->journal_home_blocks[entries] = home_block;
	entries++;

	cryexts_journal_v2_prepare_descriptor(sb, buf, entries,
					      sbi->journal_sequence);
	err = cryexts_write_file_block(sb,
				       cryexts_journal_v2_descriptor_block(sbi),
				       buf);
	if (err)
		goto rollback_entry;
	err = cryexts_sync_single_block(sb,
					cryexts_journal_v2_descriptor_block(sbi));
	if (err)
		goto rollback_entry;

	cryexts_journal_v2_prepare_commit(sb, buf, entries,
					  sbi->journal_sequence);
	err = cryexts_write_file_block(sb,
				       cryexts_journal_v2_commit_block(sbi),
				       buf);
	if (err)
		goto rollback_entry;
	err = cryexts_sync_single_block(sb,
					cryexts_journal_v2_commit_block(sbi));
	if (err)
		goto rollback_entry;

	sbi->journal_entry_count = entries;
	goto out;

rollback_entry:
	sbi->journal_home_blocks[entries - 1] = 0;
out:
	if (err)
		pr_err("cryexts: journal v2 record failed home_block=%llu seq=%llu (%d)\n",
		       home_block, sbi->journal_sequence, err);
	kfree(block_buf);
	kfree(buf);
	return err;
}

static int cryexts_journal_v2_commit(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	u64 committed_sequence = sbi->journal_sequence;
	int err = 0;

	err = cryexts_sync_metadata(sb);
	if (err)
		goto out;

	err = cryexts_journal_v2_reset_state(sb, committed_sequence);
	if (err)
		goto out;

	cryexts_super_set_recovery(sb, false);
	err = cryexts_sync_metadata(sb);
out:
	if (err)
		pr_err("cryexts: journal v2 commit failed seq=%llu (%d)\n",
		       committed_sequence, err);
	sbi->journal_entry_count = 0;
	memset(sbi->journal_home_blocks, 0, sizeof(sbi->journal_home_blocks));
	mutex_unlock(&sbi->journal_lock);
	return err;
}

static void cryexts_journal_v2_abort(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);

	(void)cryexts_journal_v2_reset_state(sb, sbi->journal_sequence);
	cryexts_super_set_recovery(sb, false);
	sbi->journal_entry_count = 0;
	memset(sbi->journal_home_blocks, 0, sizeof(sbi->journal_home_blocks));
	if (mutex_is_locked(&sbi->journal_lock))
		mutex_unlock(&sbi->journal_lock);
}

static int cryexts_journal_v2_replay(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	char *control_buf = NULL;
	char *descriptor_buf = NULL;
	char *commit_buf = NULL;
	char *block_buf = NULL;
	struct cryexts_journal_v2_control *control;
	struct cryexts_journal_v2_descriptor *descriptor;
	struct cryexts_journal_v2_commit *commit;
	u32 descriptor_entries = 0;
	u32 commit_entries = 0;
	u64 descriptor_sequence = 0;
	u64 commit_sequence = 0;
	u64 active_sequence;
	u64 blocks_count = cryexts_blocks_count(sb);
	unsigned int i;
	int err = 0;

	control_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_KERNEL);
	descriptor_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_KERNEL);
	commit_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_KERNEL);
	block_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_KERNEL);
	if (!control_buf || !descriptor_buf || !commit_buf || !block_buf) {
		err = -ENOMEM;
		goto out;
	}

	err = cryexts_read_file_block(sb, sbi->journal_block, control_buf);
	if (err)
		goto out;
	control = (struct cryexts_journal_v2_control *)control_buf;
	if (!cryexts_journal_v2_control_valid(sb, control)) {
		err = -EUCLEAN;
		goto out;
	}
	if (!cryexts_journal_v2_sequence_state_valid(control)) {
		err = -EUCLEAN;
		goto out;
	}

	active_sequence = le64_to_cpu(control->active_sequence);
	cryexts_journal_v2_set_state(
		sbi,
		le64_to_cpu(control->last_sequence),
		active_sequence,
		le64_to_cpu(control->tail_sequence),
		le64_to_cpu(control->checkpoint_sequence));
	cryexts_journal_v2_set_sequence(sb,
					le64_to_cpu(control->last_sequence));
	if (!active_sequence) {
		cryexts_super_set_recovery(sb, false);
		err = cryexts_sync_metadata(sb);
		goto out;
	}

	err = cryexts_read_file_block(sb,
				      cryexts_journal_v2_descriptor_block(sbi),
				      descriptor_buf);
	if (err)
		goto out;
	err = cryexts_read_file_block(sb,
				      cryexts_journal_v2_commit_block(sbi),
				      commit_buf);
	if (err)
		goto out;

	descriptor = (struct cryexts_journal_v2_descriptor *)descriptor_buf;
	commit = (struct cryexts_journal_v2_commit *)commit_buf;
	if (!cryexts_journal_v2_descriptor_valid(sb, descriptor,
						 &descriptor_entries,
						 &descriptor_sequence)) {
		err = cryexts_journal_v2_reset_state(sb,
						     le64_to_cpu(control->last_sequence));
		if (!err) {
			cryexts_super_set_recovery(sb, false);
			err = cryexts_sync_metadata(sb);
		}
		goto out;
	}
	if (!cryexts_journal_v2_commit_valid(sb, commit, &commit_entries,
					     &commit_sequence) ||
	    commit_sequence != active_sequence ||
	    descriptor_sequence != active_sequence ||
	    commit_entries > descriptor_entries) {
		err = cryexts_journal_v2_reset_state(sb,
						     le64_to_cpu(control->last_sequence));
		if (!err) {
			cryexts_super_set_recovery(sb, false);
			err = cryexts_sync_metadata(sb);
		}
		goto out;
	}

	sbi->journal_replaying = true;
	for (i = 0; i < commit_entries; i++) {
		u64 home_block = le64_to_cpu(descriptor->home_blocks[i]);

		if (!home_block || home_block >= blocks_count ||
		    !cryexts_data_block_valid(sb, home_block) ||
		    cryexts_journal_block_is_internal(sbi, home_block)) {
			err = -EUCLEAN;
			break;
		}
		err = cryexts_read_file_block(sb,
					      cryexts_journal_v2_payload_start(sbi) + i,
					      block_buf);
		if (err)
			break;
		err = cryexts_write_file_block(sb, home_block, block_buf);
		if (err)
			break;
	}
	sbi->journal_replaying = false;
	if (err)
		goto out;

	err = cryexts_sync_metadata(sb);
	if (err)
		goto out;

	cryexts_journal_v2_set_sequence(sb, active_sequence);
	err = cryexts_journal_v2_reset_state(sb, active_sequence);
	if (!err) {
		cryexts_super_set_recovery(sb, false);
		err = cryexts_sync_metadata(sb);
	}

out:
	sbi->journal_replaying = false;
	kfree(block_buf);
	kfree(commit_buf);
	kfree(descriptor_buf);
	kfree(control_buf);
	return err;
}

static int cryexts_journal_v3_validate_clean(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	struct cryexts_journal_v3_control *control;
	struct cryexts_journal_v3_descriptor *descriptor;
	struct cryexts_journal_v3_commit *commit;
	char *control_buf;
	char *descriptor_buf;
	char *commit_buf;
	u64 descriptor_block = sbi->journal_block + 1;
	u64 payload_start = sbi->journal_block + 2;
	u64 payload_blocks = sbi->journal_blocks - 3;
	u64 commit_block = sbi->journal_block + sbi->journal_blocks - 1;
	u64 last_sequence;
	u32 expected;
	u32 state;
	u32 i;
	int err = -EUCLEAN;

	control_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_KERNEL);
	descriptor_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_KERNEL);
	commit_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_KERNEL);
	if (!control_buf || !descriptor_buf || !commit_buf) {
		err = -ENOMEM;
		goto out;
	}

	err = cryexts_read_file_block(sb, sbi->journal_block, control_buf);
	if (err)
		goto out;
	control = (struct cryexts_journal_v3_control *)control_buf;
	if (sbi->journal_ring) {
		if (le32_to_cpu(control->magic) != CRYEXTS_JOURNAL_V3_MAGIC ||
		    le16_to_cpu(control->layout_version) !=
			    CRYEXTS_JOURNAL_V3_LAYOUT_VERSION ||
		    le16_to_cpu(control->block_type) !=
			    CRYEXTS_JOURNAL_V3_BLOCK_CONTROL ||
		    le32_to_cpu(control->features) !=
			    cryexts_journal_v3_features(sbi))
			goto corrupt;
		err = cryexts_journal_v3_ring_valid(sbi, control) ? 0 : -EUCLEAN;
		if (err)
			goto out;
		expected = cryexts_journal_v2_checksum(
			control_buf, CRYEXTS_BLOCK_SIZE,
			offsetof(struct cryexts_journal_v3_control, checksum));
		if (le32_to_cpu(control->checksum) != expected)
			goto corrupt;
		state = le32_to_cpu(control->state);
		if (state != CRYEXTS_JOURNAL_V3_STATE_IDLE ||
		    le64_to_cpu(control->active_sequence) ||
		    le64_to_cpu(control->checkpoint_sequence) !=
			    le64_to_cpu(control->last_sequence))
			goto corrupt;
		sbi->journal_sequence = le64_to_cpu(control->last_sequence);
		sbi->journal_last_sequence = sbi->journal_sequence;
		sbi->journal_active_sequence = 0;
		sbi->journal_tail_sequence = sbi->journal_sequence;
		sbi->journal_checkpoint_sequence = sbi->journal_sequence;
		sbi->journal_ring_head = le64_to_cpu(control->ring_head);
		sbi->journal_ring_tail = le64_to_cpu(control->ring_tail);
		err = 0;
		goto out;
	}
	if (le32_to_cpu(control->magic) != CRYEXTS_JOURNAL_V3_MAGIC ||
	    le16_to_cpu(control->layout_version) !=
		    CRYEXTS_JOURNAL_V3_LAYOUT_VERSION ||
	    le16_to_cpu(control->block_type) !=
		    CRYEXTS_JOURNAL_V3_BLOCK_CONTROL ||
	    le32_to_cpu(control->features) != cryexts_journal_v3_features(sbi) ||
	    (!sbi->journal_ring &&
	     (le64_to_cpu(control->descriptor_block) != descriptor_block ||
	      le64_to_cpu(control->payload_start) != payload_start ||
	      le64_to_cpu(control->payload_blocks) != payload_blocks ||
	      le64_to_cpu(control->commit_block) != commit_block)))
		goto corrupt;
	if (!cryexts_journal_v3_ring_valid(sbi, control))
		goto corrupt;
	if (sbi->journal_ring) {
		sbi->journal_ring_head = le64_to_cpu(control->ring_head);
		sbi->journal_ring_tail = le64_to_cpu(control->ring_tail);
		descriptor_block = le64_to_cpu(control->descriptor_block);
		payload_start = le64_to_cpu(control->payload_start);
		payload_blocks = le64_to_cpu(control->payload_blocks);
		commit_block = le64_to_cpu(control->commit_block);
	}
	err = cryexts_read_file_block(sb, descriptor_block, descriptor_buf);
	if (err)
		goto out;
	err = cryexts_read_file_block(sb, commit_block, commit_buf);
	if (err)
		goto out;
	expected = cryexts_journal_v2_checksum(
		control_buf, CRYEXTS_BLOCK_SIZE,
		offsetof(struct cryexts_journal_v3_control, checksum));
	if (le32_to_cpu(control->checksum) != expected)
		goto corrupt;
	state = le32_to_cpu(control->state);
	if (state > CRYEXTS_JOURNAL_V3_STATE_CHECKPOINTING)
		goto corrupt;
	if (state != CRYEXTS_JOURNAL_V3_STATE_IDLE) {
		err = -EOPNOTSUPP;
		goto out;
	}
	descriptor =
		(struct cryexts_journal_v3_descriptor *)descriptor_buf;
	if (le32_to_cpu(descriptor->magic) != CRYEXTS_JOURNAL_V3_MAGIC ||
	    le16_to_cpu(descriptor->layout_version) !=
		    CRYEXTS_JOURNAL_V3_LAYOUT_VERSION ||
	    le16_to_cpu(descriptor->block_type) !=
		    CRYEXTS_JOURNAL_V3_BLOCK_DESCRIPTOR ||
	    le32_to_cpu(descriptor->flags) ||
	    le32_to_cpu(descriptor->entry_count) ||
	    le64_to_cpu(descriptor->payload_start) != payload_start ||
	    le64_to_cpu(descriptor->commit_block) != commit_block)
		goto corrupt;
	expected = cryexts_journal_v2_checksum(
		descriptor_buf, CRYEXTS_BLOCK_SIZE,
		offsetof(struct cryexts_journal_v3_descriptor, checksum));
	if (le32_to_cpu(descriptor->checksum) != expected)
		goto corrupt;
	for (i = 0; i < CRYEXTS_JOURNAL_V3_MAX_ENTRIES; i++) {
		if (le64_to_cpu(descriptor->entries[i].home_block) ||
		    le32_to_cpu(descriptor->entries[i].payload_checksum) ||
		    le32_to_cpu(descriptor->entries[i].flags))
			goto corrupt;
	}

	commit = (struct cryexts_journal_v3_commit *)commit_buf;
	if (le32_to_cpu(commit->magic) != CRYEXTS_JOURNAL_V3_MAGIC ||
	    le16_to_cpu(commit->layout_version) !=
		    CRYEXTS_JOURNAL_V3_LAYOUT_VERSION ||
	    le16_to_cpu(commit->block_type) != CRYEXTS_JOURNAL_V3_BLOCK_COMMIT ||
	    le32_to_cpu(commit->flags) || le32_to_cpu(commit->entry_count) ||
	    le64_to_cpu(commit->descriptor_block) != descriptor_block ||
	    le32_to_cpu(commit->descriptor_checksum) !=
		    le32_to_cpu(descriptor->checksum) ||
	    le32_to_cpu(commit->payload_checksum))
		goto corrupt;
	expected = cryexts_journal_v2_checksum(
		commit_buf, CRYEXTS_BLOCK_SIZE,
		offsetof(struct cryexts_journal_v3_commit, checksum));
	if (le32_to_cpu(commit->checksum) != expected)
		goto corrupt;

	last_sequence = le64_to_cpu(control->last_sequence);
	if (le64_to_cpu(control->active_sequence) ||
	    le64_to_cpu(control->checkpoint_sequence) != last_sequence ||
	    le64_to_cpu(descriptor->sequence) != last_sequence ||
	    le64_to_cpu(commit->sequence) != last_sequence)
		goto corrupt;
	sbi->journal_sequence = last_sequence;
	sbi->journal_last_sequence = last_sequence;
	sbi->journal_active_sequence = 0;
	sbi->journal_tail_sequence = last_sequence;
	sbi->journal_checkpoint_sequence = last_sequence;
	sbi->journal_ring_head = le64_to_cpu(control->ring_head);
	sbi->journal_ring_tail = le64_to_cpu(control->ring_tail);
	err = 0;
	goto out;

corrupt:
	err = -EUCLEAN;
out:
	kfree(commit_buf);
	kfree(descriptor_buf);
	kfree(control_buf);
	return err;
}

static void cryexts_orphan_update_cached_inode(struct super_block *sb,
					       u64 ino, u64 next_orphan)
{
	struct inode *inode;
	struct cryexts_inode_info *info;

	inode = ilookup(sb, ino);
	if (!inode)
		return;
	info = cryexts_inode_blocks(inode);
	if (info)
		info->next_orphan = next_orphan;
	iput(inode);
}

bool cryexts_orphan_feature_enabled(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);

	return !!(le32_to_cpu(sbi->disk_sb->features_incompat) &
		  CRYEXTS_FEATURE_INCOMPAT_ORPHAN_LIST);
}

int cryexts_orphan_set(struct super_block *sb, u64 ino)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	struct buffer_head *bh;
	struct cryexts_inode *disk_inode;
	struct cryexts_inode_extra *extra;
	u64 old_head;
	u64 walk = 0;
	unsigned int guard = 0;

	if (!cryexts_orphan_feature_enabled(sb))
		return 0;
	if (ino < CRYEXTS_ROOT_INO || ino > cryexts_inodes_count(sb))
		return -EINVAL;

	old_head = le64_to_cpu(sbi->disk_sb->orphan_head);
	walk = old_head;
	while (walk && guard++ < cryexts_inodes_count(sb)) {
		struct buffer_head *walk_bh;
		struct cryexts_inode *walk_inode;
		struct cryexts_inode_extra *walk_extra;
		u64 next;

		if (walk == ino)
			return 0;
		walk_inode = cryexts_get_disk_inode(sb, walk, &walk_bh);
		if (!walk_inode)
			return -EIO;
		walk_extra = (struct cryexts_inode_extra *)
			(walk_inode->reserved + sizeof(walk_inode->reserved) -
			 sizeof(struct cryexts_inode_extra));
		next = le64_to_cpu(walk_extra->next_orphan);
		brelse(walk_bh);
		walk = next;
	}

	disk_inode = cryexts_get_disk_inode(sb, ino, &bh);
	if (!disk_inode)
		return -EIO;
	extra = (struct cryexts_inode_extra *)
		(disk_inode->reserved + sizeof(disk_inode->reserved) -
		 sizeof(struct cryexts_inode_extra));
	extra->next_orphan = cpu_to_le64(old_head);
	cryexts_journal_record_bh(sb, bh);
	mark_buffer_dirty(bh);
	brelse(bh);
	cryexts_orphan_update_cached_inode(sb, ino, old_head);

	sbi->disk_sb->orphan_head = cpu_to_le64(ino);
	cryexts_mark_super_dirty(sb);
	return 0;
}

int cryexts_orphan_clear(struct super_block *sb, u64 ino)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	u64 head;
	u64 prev = 0;
	u64 cur;
	unsigned int guard = 0;

	if (!cryexts_orphan_feature_enabled(sb))
		return 0;
	if (ino < CRYEXTS_ROOT_INO || ino > cryexts_inodes_count(sb))
		return -EINVAL;

	head = le64_to_cpu(sbi->disk_sb->orphan_head);
	cur = head;
	while (cur && guard++ < cryexts_inodes_count(sb)) {
		struct buffer_head *bh;
		struct cryexts_inode *disk_inode;
		struct cryexts_inode_extra *extra;
		u64 next;

		disk_inode = cryexts_get_disk_inode(sb, cur, &bh);
		if (!disk_inode)
			return -EIO;
		extra = (struct cryexts_inode_extra *)
			(disk_inode->reserved + sizeof(disk_inode->reserved) -
			 sizeof(struct cryexts_inode_extra));
		next = le64_to_cpu(extra->next_orphan);

		if (cur == ino) {
			extra->next_orphan = cpu_to_le64(0);
			cryexts_journal_record_bh(sb, bh);
			mark_buffer_dirty(bh);
			brelse(bh);
			cryexts_orphan_update_cached_inode(sb, cur, 0);

			if (!prev)
				sbi->disk_sb->orphan_head = cpu_to_le64(next);
			else {
				struct buffer_head *prev_bh;
				struct cryexts_inode *prev_inode;
				struct cryexts_inode_extra *prev_extra;

				prev_inode = cryexts_get_disk_inode(sb, prev, &prev_bh);
				if (!prev_inode)
					return -EIO;
				prev_extra = (struct cryexts_inode_extra *)
					(prev_inode->reserved +
					 sizeof(prev_inode->reserved) -
					 sizeof(struct cryexts_inode_extra));
				prev_extra->next_orphan = cpu_to_le64(next);
				cryexts_journal_record_bh(sb, prev_bh);
				mark_buffer_dirty(prev_bh);
				brelse(prev_bh);
				cryexts_orphan_update_cached_inode(sb, prev, next);
			}
			cryexts_mark_super_dirty(sb);
			return 0;
		}

		brelse(bh);
		prev = cur;
		cur = next;
	}

	return 0;
}

int cryexts_orphan_cleanup(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	u64 cur;
	unsigned int guard = 0;
	int err = 0;

	if (!cryexts_orphan_feature_enabled(sb))
		return 0;

	cur = le64_to_cpu(sbi->disk_sb->orphan_head);
	while (cur && guard++ < cryexts_inodes_count(sb)) {
		struct inode *inode;
		struct cryexts_inode_info *info;
		u64 next;

		inode = cryexts_iget(sb, cur);
		if (IS_ERR(inode)) {
			err = PTR_ERR(inode);
			break;
		}
		info = cryexts_inode_blocks(inode);
		next = info ? info->next_orphan : 0;

		err = cryexts_release_inode_storage(inode);
		if (err) {
			iput(inode);
			break;
		}
		err = cryexts_orphan_clear(sb, cur);
		if (err) {
			iput(inode);
			break;
		}
		clear_nlink(inode);
		err = cryexts_free_inode(sb, cur);
		iput(inode);
		if (err)
			break;
		cur = next;
	}

	if (!err && le64_to_cpu(sbi->disk_sb->orphan_head)) {
		sbi->disk_sb->orphan_head = cpu_to_le64(0);
		cryexts_mark_super_dirty(sb);
	}

	return err;
}

static u32 cryexts_journal_v3_payload_capacity(struct cryexts_sb_info *sbi)
{
	if (sbi->journal_blocks < CRYEXTS_JOURNAL_V3_MIN_BLOCKS)
		return 0;
	return min_t(u64, CRYEXTS_JOURNAL_V3_MAX_ENTRIES,
		     sbi->journal_blocks - 3);
}

static u32 cryexts_journal_v3_features(struct cryexts_sb_info *sbi)
{
	return CRYEXTS_JOURNAL_V3_FEATURE_REDO |
		(sbi->journal_ring ? CRYEXTS_JOURNAL_V3_FEATURE_RING : 0);
}

static u64 cryexts_journal_v3_ring_start(struct cryexts_sb_info *sbi)
{
	return sbi->journal_block + 1;
}

static u64 cryexts_journal_v3_ring_end(struct cryexts_sb_info *sbi)
{
	return sbi->journal_block + sbi->journal_blocks;
}

static bool cryexts_journal_v3_ring_valid(
	struct cryexts_sb_info *sbi,
	const struct cryexts_journal_v3_control *control)
{
	if (!sbi->journal_ring)
		return !le64_to_cpu(control->ring_head) &&
		       !le64_to_cpu(control->ring_tail) &&
		       !le64_to_cpu(control->ring_start) &&
		       !le64_to_cpu(control->ring_end);

	return le64_to_cpu(control->ring_start) ==
		       cryexts_journal_v3_ring_start(sbi) &&
	       le64_to_cpu(control->ring_end) ==
		       cryexts_journal_v3_ring_end(sbi) &&
	       le64_to_cpu(control->ring_head) >=
		       cryexts_journal_v3_ring_start(sbi) &&
	       le64_to_cpu(control->ring_head) <
		       cryexts_journal_v3_ring_end(sbi) &&
	       le64_to_cpu(control->ring_tail) >=
		       cryexts_journal_v3_ring_start(sbi) &&
	       le64_to_cpu(control->ring_tail) <
		       cryexts_journal_v3_ring_end(sbi) &&
	       (le32_to_cpu(control->state) != CRYEXTS_JOURNAL_V3_STATE_IDLE ||
		le64_to_cpu(control->ring_head) ==
		le64_to_cpu(control->ring_tail));
}

static void cryexts_journal_v3_prepare_control(struct super_block *sb,
					       char *buf, u32 state,
				       u64 last_sequence,
				       u64 active_sequence,
				       u64 checkpoint_sequence,
				       u64 descriptor_block, u64 payload_start,
				       u64 payload_blocks, u64 commit_block)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	struct cryexts_journal_v3_control *control;

	memset(buf, 0, CRYEXTS_BLOCK_SIZE);
	control = (struct cryexts_journal_v3_control *)buf;
	control->magic = cpu_to_le32(CRYEXTS_JOURNAL_V3_MAGIC);
	control->layout_version = cpu_to_le16(CRYEXTS_JOURNAL_V3_LAYOUT_VERSION);
	control->block_type = cpu_to_le16(CRYEXTS_JOURNAL_V3_BLOCK_CONTROL);
	control->state = cpu_to_le32(state);
	control->features = cpu_to_le32(cryexts_journal_v3_features(sbi));
	control->last_sequence = cpu_to_le64(last_sequence);
	control->active_sequence = cpu_to_le64(active_sequence);
	control->checkpoint_sequence = cpu_to_le64(checkpoint_sequence);
	control->descriptor_block = cpu_to_le64(descriptor_block);
	control->payload_start = cpu_to_le64(payload_start);
	control->payload_blocks = cpu_to_le64(payload_blocks);
	control->commit_block = cpu_to_le64(commit_block);
	if (sbi->journal_ring) {
		control->ring_start = cpu_to_le64(
			cryexts_journal_v3_ring_start(sbi));
		control->ring_end = cpu_to_le64(
			cryexts_journal_v3_ring_end(sbi));
		control->ring_head = cpu_to_le64(sbi->journal_ring_head);
		control->ring_tail = cpu_to_le64(sbi->journal_ring_tail);
	}
	control->checksum = cpu_to_le32(cryexts_journal_v2_checksum(
		buf, CRYEXTS_BLOCK_SIZE,
		offsetof(struct cryexts_journal_v3_control, checksum)));
}

static void cryexts_journal_v3_prepare_descriptor(struct super_block *sb,
						  char *buf, u32 entries,
						  u64 sequence,
						  u64 payload_start,
						  u64 commit_block)
{
	struct cryexts_journal_v3_descriptor *descriptor;

	(void)sb;

	memset(buf, 0, CRYEXTS_BLOCK_SIZE);
	descriptor = (struct cryexts_journal_v3_descriptor *)buf;
	descriptor->magic = cpu_to_le32(CRYEXTS_JOURNAL_V3_MAGIC);
	descriptor->layout_version =
		cpu_to_le16(CRYEXTS_JOURNAL_V3_LAYOUT_VERSION);
	descriptor->block_type =
		cpu_to_le16(CRYEXTS_JOURNAL_V3_BLOCK_DESCRIPTOR);
	descriptor->entry_count = cpu_to_le32(entries);
	descriptor->sequence = cpu_to_le64(sequence);
	descriptor->payload_start = cpu_to_le64(payload_start);
	descriptor->commit_block = cpu_to_le64(commit_block);
}

static void cryexts_journal_v3_finish_descriptor(char *buf)
{
	struct cryexts_journal_v3_descriptor *descriptor =
		(struct cryexts_journal_v3_descriptor *)buf;

	descriptor->checksum = cpu_to_le32(cryexts_journal_v2_checksum(
		buf, CRYEXTS_BLOCK_SIZE,
		offsetof(struct cryexts_journal_v3_descriptor, checksum)));
}

static void cryexts_journal_v3_prepare_commit(struct super_block *sb,
					      char *buf, u32 entries,
					      u64 sequence,
					      u32 descriptor_checksum,
					      u32 payload_checksum,
					      bool committed, u64 descriptor_block)
{
	struct cryexts_journal_v3_commit *commit;

	(void)sb;

	memset(buf, 0, CRYEXTS_BLOCK_SIZE);
	commit = (struct cryexts_journal_v3_commit *)buf;
	commit->magic = cpu_to_le32(CRYEXTS_JOURNAL_V3_MAGIC);
	commit->layout_version = cpu_to_le16(CRYEXTS_JOURNAL_V3_LAYOUT_VERSION);
	commit->block_type = cpu_to_le16(CRYEXTS_JOURNAL_V3_BLOCK_COMMIT);
	commit->flags = cpu_to_le32(committed ?
		CRYEXTS_JOURNAL_V3_FLAG_COMMITTED : 0);
	commit->entry_count = cpu_to_le32(entries);
	commit->descriptor_checksum = cpu_to_le32(descriptor_checksum);
	commit->payload_checksum = cpu_to_le32(payload_checksum);
	commit->sequence = cpu_to_le64(sequence);
	commit->descriptor_block = cpu_to_le64(descriptor_block);
	commit->checksum = cpu_to_le32(cryexts_journal_v2_checksum(
		buf, CRYEXTS_BLOCK_SIZE,
		offsetof(struct cryexts_journal_v3_commit, checksum)));
}

static int cryexts_journal_v3_write_sync(struct super_block *sb, u64 block,
					 const char *buf)
{
	int err;

	err = cryexts_write_file_block(sb, block, buf);
	if (err)
		return err;
	return cryexts_sync_single_block(sb, block);
}

static void cryexts_journal_v3_clear_runtime(struct cryexts_sb_info *sbi)
{
	sbi->journal_entry_count = 0;
	sbi->journal_error = 0;
	sbi->journal_active_sequence = 0;
	memset(sbi->journal_home_blocks, 0, sizeof(sbi->journal_home_blocks));
}

static int cryexts_journal_v3_checkpoint_one(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	struct cryexts_journal_v3_descriptor *descriptor;
	struct cryexts_journal_v3_commit *commit;
	char *descriptor_buf = NULL;
	char *control_buf = NULL;
	char *commit_buf = NULL;
	char *payload_buf = NULL;
	u64 last_sequence = sbi->journal_last_sequence;
	u64 descriptor_block = sbi->journal_ring_tail;
	u64 payload_start;
	u64 commit_block;
	u64 next_tail;
	u64 sequence;
	u64 checkpoint_sequence = sbi->journal_checkpoint_sequence;
	u32 entries;
	u32 expected;
	unsigned int i;
	int err = -EUCLEAN;

	if (!sbi->journal_ring || descriptor_block == sbi->journal_ring_head)
		return 0;

	descriptor_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_NOFS);
	control_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_NOFS);
	commit_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_NOFS);
	payload_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_NOFS);
	if (!descriptor_buf || !control_buf || !commit_buf || !payload_buf) {
		err = -ENOMEM;
		goto out;
	}

	err = cryexts_read_file_block(sb, descriptor_block, descriptor_buf);
	if (err)
		goto out;
	descriptor = (struct cryexts_journal_v3_descriptor *)descriptor_buf;
	if (le32_to_cpu(descriptor->magic) != CRYEXTS_JOURNAL_V3_MAGIC ||
	    le16_to_cpu(descriptor->layout_version) !=
		    CRYEXTS_JOURNAL_V3_LAYOUT_VERSION ||
	    le16_to_cpu(descriptor->block_type) !=
		    CRYEXTS_JOURNAL_V3_BLOCK_DESCRIPTOR)
		goto corrupt;
	expected = cryexts_journal_v2_checksum(
		descriptor_buf, CRYEXTS_BLOCK_SIZE,
		offsetof(struct cryexts_journal_v3_descriptor, checksum));
	if (le32_to_cpu(descriptor->checksum) != expected)
		goto corrupt;
	entries = le32_to_cpu(descriptor->entry_count);
	sequence = le64_to_cpu(descriptor->sequence);
	payload_start = le64_to_cpu(descriptor->payload_start);
	commit_block = le64_to_cpu(descriptor->commit_block);
	if (entries > cryexts_journal_v3_payload_capacity(sbi) ||
	    payload_start != descriptor_block + 1 ||
	    commit_block != payload_start + entries)
		goto corrupt;

	err = cryexts_read_file_block(sb, commit_block, commit_buf);
	if (err)
		goto out;
	commit = (struct cryexts_journal_v3_commit *)commit_buf;
	if (le32_to_cpu(commit->magic) != CRYEXTS_JOURNAL_V3_MAGIC ||
	    le16_to_cpu(commit->layout_version) !=
		    CRYEXTS_JOURNAL_V3_LAYOUT_VERSION ||
	    le16_to_cpu(commit->block_type) != CRYEXTS_JOURNAL_V3_BLOCK_COMMIT ||
	    le32_to_cpu(commit->flags) != CRYEXTS_JOURNAL_V3_FLAG_COMMITTED ||
	    le32_to_cpu(commit->entry_count) != entries ||
	    le64_to_cpu(commit->sequence) != sequence ||
	    le64_to_cpu(commit->descriptor_block) != descriptor_block ||
	    le32_to_cpu(commit->descriptor_checksum) !=
		    le32_to_cpu(descriptor->checksum))
		goto corrupt;
	expected = cryexts_journal_v2_checksum(
		commit_buf, CRYEXTS_BLOCK_SIZE,
		offsetof(struct cryexts_journal_v3_commit, checksum));
	if (le32_to_cpu(commit->checksum) != expected)
		goto corrupt;

	cryexts_journal_v3_prepare_control(
		sb, control_buf, CRYEXTS_JOURNAL_V3_STATE_CHECKPOINTING,
		last_sequence, 0, checkpoint_sequence, descriptor_block,
		payload_start, entries, commit_block);
	err = cryexts_journal_v3_write_sync(sb, sbi->journal_block, control_buf);
	if (err)
		goto out;

	for (i = 0; i < entries; i++) {
		u64 home_block = le64_to_cpu(descriptor->entries[i].home_block);
		u32 checksum;

		if (home_block >= cryexts_blocks_count(sb) ||
		    cryexts_journal_block_is_internal(sbi, home_block) ||
		    le32_to_cpu(descriptor->entries[i].flags))
			goto corrupt;
		err = cryexts_read_file_block(sb, payload_start + i, payload_buf);
		if (err)
			goto out;
		checksum = cryexts_fnv1a_update(2166136261u, payload_buf,
						 CRYEXTS_BLOCK_SIZE);
		if (checksum != le32_to_cpu(
			    descriptor->entries[i].payload_checksum))
			goto corrupt;
		err = cryexts_journal_v3_write_sync(sb, home_block, payload_buf);
		if (err)
			goto out;
	}

	next_tail = commit_block + 1;
	if (next_tail == cryexts_journal_v3_ring_end(sbi))
		next_tail = cryexts_journal_v3_ring_start(sbi);
	sbi->journal_ring_tail = next_tail;
	sbi->journal_tail_sequence = sequence;
	sbi->journal_checkpoint_sequence = sequence;

	if (next_tail == sbi->journal_ring_head) {
		sbi->journal_replaying = true;
		cryexts_super_set_recovery(sb, false);
		sbi->journal_replaying = false;
		err = cryexts_sync_single_block(sb, sbi->s_sbh->b_blocknr);
		if (err)
			goto out;
		cryexts_journal_v3_prepare_control(
			sb, control_buf, CRYEXTS_JOURNAL_V3_STATE_IDLE,
			last_sequence, 0, sequence, descriptor_block,
			payload_start, entries, commit_block);
	} else {
		cryexts_journal_v3_prepare_control(
			sb, control_buf, CRYEXTS_JOURNAL_V3_STATE_COMMITTED,
			last_sequence, 0, sequence, descriptor_block,
			payload_start, entries, commit_block);
	}
	err = cryexts_journal_v3_write_sync(sb, sbi->journal_block, control_buf);
	if (err)
		goto out;
	err = 0;
	goto out;

corrupt:
	err = -EUCLEAN;
out:
	kfree(payload_buf);
	kfree(commit_buf);
	kfree(control_buf);
	kfree(descriptor_buf);
	return err;
}

int cryexts_journal_checkpoint_sync(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	int err = 0;

	if (!sbi->journal_enabled || !sbi->journal_v3 || !sbi->journal_ring)
		return 0;
	mutex_lock(&sbi->journal_lock);
	while (!err && sbi->journal_ring_tail != sbi->journal_ring_head)
		err = cryexts_journal_v3_checkpoint_one(sb);
	mutex_unlock(&sbi->journal_lock);
	return err;
}

void cryexts_journal_checkpoint_worker(struct work_struct *work)
{
	struct cryexts_sb_info *sbi =
		container_of(work, struct cryexts_sb_info, journal_checkpoint_work);

	cryexts_journal_checkpoint_sync(sbi->sb);
}

static int cryexts_journal_v3_ring_allocate(struct cryexts_sb_info *sbi,
					     u32 entries, u64 *descriptor_block,
					     u64 *payload_start, u64 *commit_block,
					     u64 *next_head)
{
	u64 start = sbi->journal_ring_head;
	u64 tail = sbi->journal_ring_tail;
	u64 ring_start = cryexts_journal_v3_ring_start(sbi);
	u64 end = cryexts_journal_v3_ring_end(sbi);
	u64 needed = (u64)entries + 2;

	if (!sbi->journal_ring) {
		*descriptor_block = cryexts_journal_v2_descriptor_block(sbi);
		*payload_start = cryexts_journal_v2_payload_start(sbi);
		*commit_block = cryexts_journal_v2_commit_block(sbi);
		*next_head = start;
		return 0;
	}
	if (needed > end - ring_start)
		return -ENOSPC;
	if (start + needed > end) {
		if (start != tail)
			return -ENOSPC;
		start = ring_start;
	} else if (tail != start && tail > start && tail < start + needed) {
		return -ENOSPC;
	}
	*descriptor_block = start;
	*payload_start = start + 1;
	*commit_block = *payload_start + entries;
	*next_head = *commit_block + 1;
	if (*next_head == end)
		*next_head = ring_start;
	return 0;
}

static int cryexts_journal_v3_begin(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	int err = 0;

	mutex_lock(&sbi->journal_lock);
	if (sbi->journal_active_sequence) {
		err = sbi->journal_error ? sbi->journal_error : -EUCLEAN;
		mutex_unlock(&sbi->journal_lock);
		return err;
	}
	if (!cryexts_journal_v3_payload_capacity(sbi)) {
		mutex_unlock(&sbi->journal_lock);
		return -EOPNOTSUPP;
	}

	cryexts_journal_v3_clear_runtime(sbi);
	sbi->journal_active_sequence = sbi->journal_sequence + 1;
	return 0;
}

static int cryexts_journal_v3_record_block(struct super_block *sb,
					    u64 home_block)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	u32 capacity = cryexts_journal_v3_payload_capacity(sbi);
	unsigned int i;
	int err = 0;

	if (!sbi->journal_active_sequence)
		return -EINVAL;
	if (sbi->journal_error)
		return sbi->journal_error;
	if (home_block >= cryexts_blocks_count(sb) ||
	    cryexts_journal_block_is_internal(sbi, home_block)) {
		err = -EINVAL;
		goto out;
	}
	for (i = 0; i < sbi->journal_entry_count; i++) {
		if (sbi->journal_home_blocks[i] == home_block)
			return 0;
	}
	if (sbi->journal_entry_count >= capacity) {
		err = -ENOSPC;
		goto out;
	}

	sbi->journal_home_blocks[sbi->journal_entry_count++] = home_block;
	return 0;

out:
	sbi->journal_error = err;
	return err;
}

static int cryexts_journal_v3_commit(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	struct cryexts_journal_v3_descriptor *descriptor;
	char *descriptor_buf = NULL;
	char *state_buf = NULL;
	char *payload_buf = NULL;
	u64 previous_sequence = sbi->journal_sequence;
	u64 sequence = sbi->journal_active_sequence;
	u64 descriptor_block;
	u64 payload_start;
	u64 commit_block;
	u64 next_head;
	u64 previous_head = sbi->journal_ring_head;
	u64 previous_tail = sbi->journal_ring_tail;
	u32 entries;
	u32 descriptor_checksum;
	u32 payload_checksum = 2166136261u;
	unsigned int i;
	bool commit_attempted = false;
	int err = sbi->journal_error;

	if (err)
		goto out;

	sbi->disk_sb->journal_sequence = cpu_to_le64(sequence);
	cryexts_gdt_prepare_write(sb);
	cryexts_super_set_recovery(sb, true);
	if (sbi->journal_error) {
		err = sbi->journal_error;
		goto out_clear_recovery;
	}
	entries = sbi->journal_entry_count;
	err = cryexts_journal_v3_ring_allocate(sbi, entries, &descriptor_block,
					       &payload_start, &commit_block,
					       &next_head);
	if (err)
		goto out_clear_recovery;
	if (sbi->journal_ring) {
		if (sbi->journal_ring_tail == previous_head)
			sbi->journal_ring_tail = descriptor_block;
		sbi->journal_ring_head = next_head;
	}

	descriptor_buf = kzalloc(CRYEXTS_BLOCK_SIZE, GFP_NOFS);
	state_buf = kzalloc(CRYEXTS_BLOCK_SIZE, GFP_NOFS);
	payload_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_NOFS);
	if (!descriptor_buf || !state_buf || !payload_buf) {
		err = -ENOMEM;
		goto out_clear_recovery;
	}

	cryexts_journal_v3_prepare_descriptor(sb, descriptor_buf, entries,
					       sequence, payload_start, commit_block);
	descriptor = (struct cryexts_journal_v3_descriptor *)descriptor_buf;
	for (i = 0; i < entries; i++) {
		u64 home_block = sbi->journal_home_blocks[i];
		u32 checksum;

		err = cryexts_read_file_block(sb, home_block, payload_buf);
		if (err)
			goto out_clear_recovery;
		checksum = cryexts_fnv1a_update(2166136261u, payload_buf,
						 CRYEXTS_BLOCK_SIZE);
		payload_checksum = cryexts_fnv1a_update(payload_checksum,
							 payload_buf,
							 CRYEXTS_BLOCK_SIZE);
		descriptor->entries[i].home_block = cpu_to_le64(home_block);
		descriptor->entries[i].payload_checksum = cpu_to_le32(checksum);
		err = cryexts_journal_v3_write_sync(
			sb, payload_start + i,
			payload_buf);
		if (err)
			goto out_clear_recovery;
	}
	if (!entries)
		payload_checksum = 0;

	cryexts_journal_v3_finish_descriptor(descriptor_buf);
	descriptor_checksum = le32_to_cpu(descriptor->checksum);
	err = cryexts_journal_v3_write_sync(
		sb, descriptor_block, descriptor_buf);
	if (err)
		goto out_clear_recovery;

	cryexts_journal_v3_prepare_control(
		sb, state_buf, CRYEXTS_JOURNAL_V3_STATE_PREPARED,
		previous_sequence, sequence, previous_sequence, descriptor_block,
		payload_start, entries, commit_block);
	err = cryexts_journal_v3_write_sync(sb, sbi->journal_block, state_buf);
	if (err)
		goto out_clear_recovery;

	cryexts_journal_v3_prepare_commit(sb, state_buf, entries, sequence,
					  descriptor_checksum,
					  payload_checksum, true, descriptor_block);
	commit_attempted = true;
	err = cryexts_journal_v3_write_sync(
		sb, commit_block, state_buf);
	if (err)
		goto out;

	if (sbi->journal_ring) {
		cryexts_journal_v3_prepare_control(
			sb, state_buf, CRYEXTS_JOURNAL_V3_STATE_COMMITTED,
			sequence, 0, previous_sequence, descriptor_block,
			payload_start, entries, commit_block);
		err = cryexts_journal_v3_write_sync(sb, sbi->journal_block,
						    state_buf);
		if (err)
			goto out;

		sbi->journal_sequence = sequence;
		sbi->journal_last_sequence = sequence;
		cryexts_journal_v3_clear_runtime(sbi);
		schedule_work(&sbi->journal_checkpoint_work);
		goto out_unlock;
	}

	cryexts_journal_v3_prepare_control(
		sb, state_buf, CRYEXTS_JOURNAL_V3_STATE_CHECKPOINTING,
		sequence, sequence, previous_sequence, descriptor_block,
		payload_start, entries, commit_block);
	err = cryexts_journal_v3_write_sync(sb, sbi->journal_block, state_buf);
	if (err)
		goto out;

	for (i = 0; i < entries; i++) {
		u32 checksum;

		err = cryexts_read_file_block(
			sb, payload_start + i,
			payload_buf);
		if (err)
			goto out;
		checksum = cryexts_fnv1a_update(2166136261u, payload_buf,
						 CRYEXTS_BLOCK_SIZE);
		if (checksum != le32_to_cpu(
			    descriptor->entries[i].payload_checksum)) {
			err = -EUCLEAN;
			goto out;
		}
		err = cryexts_journal_v3_write_sync(
			sb, sbi->journal_home_blocks[i], payload_buf);
		if (err)
			goto out;
	}

	sbi->journal_replaying = true;
	cryexts_super_set_recovery(sb, false);
	sbi->journal_replaying = false;
	err = cryexts_sync_single_block(sb, sbi->s_sbh->b_blocknr);
	if (err)
		goto out;

	cryexts_journal_v3_prepare_descriptor(sb, descriptor_buf, 0, sequence,
					       payload_start, commit_block);
	cryexts_journal_v3_finish_descriptor(descriptor_buf);
	descriptor_checksum = le32_to_cpu(
		((struct cryexts_journal_v3_descriptor *)descriptor_buf)->checksum);
	err = cryexts_journal_v3_write_sync(sb, descriptor_block, descriptor_buf);
	if (err)
		goto out;
	cryexts_journal_v3_prepare_commit(sb, state_buf, 0, sequence,
					  descriptor_checksum, 0, false,
					  descriptor_block);
	err = cryexts_journal_v3_write_sync(sb, commit_block, state_buf);
	if (err)
		goto out;

	cryexts_journal_v3_prepare_control(
		sb, state_buf, CRYEXTS_JOURNAL_V3_STATE_IDLE,
		sequence, 0, sequence, descriptor_block, payload_start,
		entries, commit_block);
	err = cryexts_journal_v3_write_sync(sb, sbi->journal_block, state_buf);
	if (err)
		goto out;

	sbi->journal_sequence = sequence;
	sbi->journal_last_sequence = sequence;
	sbi->journal_tail_sequence = sequence;
	sbi->journal_checkpoint_sequence = sequence;
	cryexts_journal_v3_clear_runtime(sbi);
	goto out_unlock;

out_clear_recovery:
	if (sbi->journal_ring) {
		sbi->journal_ring_head = previous_head;
		sbi->journal_ring_tail = previous_tail;
	}
	sbi->disk_sb->journal_sequence = cpu_to_le64(previous_sequence);
	sbi->journal_replaying = true;
	cryexts_super_set_recovery(sb, false);
	sbi->journal_replaying = false;
out:
	if (err) {
		pr_err("cryexts: journal v3 commit failed seq=%llu (%d)\n",
		       sequence, err);
		if (commit_attempted)
			sbi->journal_error = err;
		else
			cryexts_journal_v3_clear_runtime(sbi);
	}
out_unlock:
	kfree(payload_buf);
	kfree(state_buf);
	kfree(descriptor_buf);
	mutex_unlock(&sbi->journal_lock);
	return err;
}

static void cryexts_journal_v3_abort(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);

	cryexts_journal_v3_clear_runtime(sbi);
	if (mutex_is_locked(&sbi->journal_lock))
		mutex_unlock(&sbi->journal_lock);
}

static int cryexts_journal_v3_reset_disk(struct super_block *sb, u64 sequence,
					 char *descriptor_buf, char *state_buf,
					 u64 descriptor_block, u64 payload_start,
					 u64 payload_blocks, u64 commit_block)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	u32 descriptor_checksum;
	int err;

	if (sbi->journal_ring) {
		cryexts_journal_v3_prepare_control(
			sb, state_buf, CRYEXTS_JOURNAL_V3_STATE_IDLE,
			sequence, 0, sequence, descriptor_block, payload_start,
			payload_blocks, commit_block);
		return cryexts_journal_v3_write_sync(sb, sbi->journal_block,
						     state_buf);
	}

	cryexts_journal_v3_prepare_descriptor(
		sb, descriptor_buf, 0, sequence,
		cryexts_journal_v2_payload_start(sbi),
		cryexts_journal_v2_commit_block(sbi));
	cryexts_journal_v3_finish_descriptor(descriptor_buf);
	descriptor_checksum = le32_to_cpu(
		((struct cryexts_journal_v3_descriptor *)descriptor_buf)->checksum);
	err = cryexts_journal_v3_write_sync(
		sb, cryexts_journal_v2_descriptor_block(CRYEXTS_SB(sb)),
		descriptor_buf);
	if (err)
		return err;

	cryexts_journal_v3_prepare_commit(sb, state_buf, 0, sequence,
					  descriptor_checksum, 0, false,
					  cryexts_journal_v2_descriptor_block(sbi));
	err = cryexts_journal_v3_write_sync(
		sb, cryexts_journal_v2_commit_block(CRYEXTS_SB(sb)), state_buf);
	if (err)
		return err;

	cryexts_journal_v3_prepare_control(
		sb, state_buf, CRYEXTS_JOURNAL_V3_STATE_IDLE,
		sequence, 0, sequence,
		cryexts_journal_v2_descriptor_block(sbi),
		cryexts_journal_v2_payload_start(sbi),
		cryexts_journal_v2_payload_area_blocks(sbi),
		cryexts_journal_v2_commit_block(sbi));
	return cryexts_journal_v3_write_sync(
		sb, CRYEXTS_SB(sb)->journal_block, state_buf);
}

static int cryexts_journal_v3_replay_ring(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	struct cryexts_journal_v3_control *control;
	struct cryexts_journal_v3_descriptor *descriptor;
	struct cryexts_journal_v3_commit *commit;
	char *control_buf = NULL;
	char *descriptor_buf = NULL;
	char *commit_buf = NULL;
	char *payload_buf = NULL;
	u64 ring_start = cryexts_journal_v3_ring_start(sbi);
	u64 ring_end = cryexts_journal_v3_ring_end(sbi);
	u64 position = 0;
	u64 head = 0;
	u64 payload_start = 0;
	u64 commit_block = 0;
	u64 segment_end = 0;
	u64 last_sequence;
	u64 sequence = 0;
	u32 state;
	u32 entries = 0;
	u32 expected;
	u32 commit_flags;
	unsigned int i;
	bool commit_valid;
	int err = -EUCLEAN;

	control_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_KERNEL);
	descriptor_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_KERNEL);
	commit_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_KERNEL);
	payload_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_KERNEL);
	if (!control_buf || !descriptor_buf || !commit_buf || !payload_buf) {
		err = -ENOMEM;
		goto out;
	}

	err = cryexts_read_file_block(sb, sbi->journal_block, control_buf);
	if (err)
		goto out;
	control = (struct cryexts_journal_v3_control *)control_buf;
	if (le32_to_cpu(control->magic) != CRYEXTS_JOURNAL_V3_MAGIC ||
	    le16_to_cpu(control->layout_version) !=
		    CRYEXTS_JOURNAL_V3_LAYOUT_VERSION ||
	    le16_to_cpu(control->block_type) !=
		    CRYEXTS_JOURNAL_V3_BLOCK_CONTROL ||
	    le32_to_cpu(control->features) != cryexts_journal_v3_features(sbi))
		goto corrupt;
	if (!cryexts_journal_v3_ring_valid(sbi, control))
		goto corrupt;
	expected = cryexts_journal_v2_checksum(
		control_buf, CRYEXTS_BLOCK_SIZE,
		offsetof(struct cryexts_journal_v3_control, checksum));
	if (le32_to_cpu(control->checksum) != expected)
		goto corrupt;

	state = le32_to_cpu(control->state);
	last_sequence = le64_to_cpu(control->last_sequence);
	head = le64_to_cpu(control->ring_head);
	position = le64_to_cpu(control->ring_tail);

	if (state == CRYEXTS_JOURNAL_V3_STATE_IDLE) {
		if (!cryexts_journal_needs_recovery(sb)) {
			err = cryexts_journal_v3_validate_clean(sb);
			goto out;
		}
		if (le64_to_cpu(control->active_sequence) ||
		    le64_to_cpu(control->checkpoint_sequence) != last_sequence)
			goto corrupt;
		goto discard;
	}
	if (state > CRYEXTS_JOURNAL_V3_STATE_CHECKPOINTING)
		goto corrupt;

	if (position == head)
		goto discard;

	while (position != head) {
		err = cryexts_read_file_block(sb, position, descriptor_buf);
		if (err)
			goto out;
		descriptor = (struct cryexts_journal_v3_descriptor *)descriptor_buf;
		if (le32_to_cpu(descriptor->magic) != CRYEXTS_JOURNAL_V3_MAGIC ||
		    le16_to_cpu(descriptor->layout_version) !=
			    CRYEXTS_JOURNAL_V3_LAYOUT_VERSION ||
		    le16_to_cpu(descriptor->block_type) !=
			    CRYEXTS_JOURNAL_V3_BLOCK_DESCRIPTOR)
			goto corrupt;
		expected = cryexts_journal_v2_checksum(
			descriptor_buf, CRYEXTS_BLOCK_SIZE,
			offsetof(struct cryexts_journal_v3_descriptor, checksum));
		if (le32_to_cpu(descriptor->checksum) != expected)
			goto corrupt;
		entries = le32_to_cpu(descriptor->entry_count);
		sequence = le64_to_cpu(descriptor->sequence);
		payload_start = le64_to_cpu(descriptor->payload_start);
		commit_block = le64_to_cpu(descriptor->commit_block);
		if (entries > cryexts_journal_v3_payload_capacity(sbi) ||
		    payload_start != position + 1 ||
		    commit_block != payload_start + entries)
			goto corrupt;

		segment_end = commit_block + 1;
		if (segment_end == ring_end)
			segment_end = ring_start;

		err = cryexts_read_file_block(sb, commit_block, commit_buf);
		if (err)
			goto out;
		commit = (struct cryexts_journal_v3_commit *)commit_buf;
		commit_valid =
			le32_to_cpu(commit->magic) == CRYEXTS_JOURNAL_V3_MAGIC &&
			le16_to_cpu(commit->layout_version) ==
				CRYEXTS_JOURNAL_V3_LAYOUT_VERSION &&
			le16_to_cpu(commit->block_type) ==
				CRYEXTS_JOURNAL_V3_BLOCK_COMMIT &&
			le64_to_cpu(commit->descriptor_block) == position &&
			le32_to_cpu(commit->entry_count) == entries &&
			le64_to_cpu(commit->sequence) == sequence;
		if (commit_valid) {
			expected = cryexts_journal_v2_checksum(
				commit_buf, CRYEXTS_BLOCK_SIZE,
				offsetof(struct cryexts_journal_v3_commit, checksum));
			commit_valid = le32_to_cpu(commit->checksum) == expected;
		}
		if (!commit_valid) {
			if (segment_end == head)
				break;
			goto corrupt;
		}
		commit_flags = le32_to_cpu(commit->flags);
		if (commit_flags != CRYEXTS_JOURNAL_V3_FLAG_COMMITTED ||
		    le32_to_cpu(commit->descriptor_checksum) !=
			    le32_to_cpu(descriptor->checksum))
			goto corrupt;

		for (i = 0; i < entries; i++) {
			u64 home_block =
				le64_to_cpu(descriptor->entries[i].home_block);
			unsigned int j;

			if (home_block >= cryexts_blocks_count(sb) ||
			    cryexts_journal_block_is_internal(sbi, home_block) ||
			    le32_to_cpu(descriptor->entries[i].flags))
				goto corrupt;
			for (j = 0; j < i; j++) {
				if (le64_to_cpu(descriptor->entries[j].home_block) ==
				    home_block)
					goto corrupt;
			}
			err = cryexts_read_file_block(sb, payload_start + i, payload_buf);
			if (err)
				goto out;
			expected = cryexts_fnv1a_update(2166136261u, payload_buf,
						       CRYEXTS_BLOCK_SIZE);
			if (le32_to_cpu(descriptor->entries[i].payload_checksum) !=
			    expected)
				goto corrupt;
		}

		sbi->journal_replaying = true;
		for (i = 0; i < entries; i++) {
			err = cryexts_read_file_block(sb, payload_start + i, payload_buf);
			if (err)
				break;
			err = cryexts_journal_v3_write_sync(
				sb, le64_to_cpu(descriptor->entries[i].home_block),
				payload_buf);
			if (err)
				break;
		}
		sbi->journal_replaying = false;
		if (err)
			goto out;

		last_sequence = sequence;
		position = segment_end;
	}

discard:
	sbi->journal_sequence = last_sequence;
	sbi->journal_last_sequence = last_sequence;
	sbi->journal_tail_sequence = last_sequence;
	sbi->journal_checkpoint_sequence = last_sequence;
	sbi->journal_ring_tail = head;
	sbi->journal_ring_head = head;
	sbi->disk_sb->journal_sequence = cpu_to_le64(last_sequence);
	sbi->journal_replaying = true;
	cryexts_super_set_recovery(sb, false);
	sbi->journal_replaying = false;
	err = cryexts_sync_single_block(sb, sbi->s_sbh->b_blocknr);
	if (err)
		goto out;
	err = cryexts_journal_v3_reset_disk(
		sb, last_sequence, descriptor_buf, control_buf,
		sbi->journal_block + 1, sbi->journal_block + 2,
		sbi->journal_blocks - 3,
		sbi->journal_block + sbi->journal_blocks - 1);
	if (!err)
		cryexts_journal_v3_clear_runtime(sbi);
	goto out;

corrupt:
	err = -EUCLEAN;
out:
	sbi->journal_replaying = false;
	kfree(payload_buf);
	kfree(commit_buf);
	kfree(descriptor_buf);
	kfree(control_buf);
	return err;
}

static int cryexts_journal_v3_replay(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	struct cryexts_journal_v3_control *control;

	if (sbi->journal_ring)
		return cryexts_journal_v3_replay_ring(sb);
	struct cryexts_journal_v3_descriptor *descriptor;
	struct cryexts_journal_v3_commit *commit;
	char *control_buf = NULL;
	char *descriptor_buf = NULL;
	char *commit_buf = NULL;
	char *payload_buf = NULL;
	u64 descriptor_block = cryexts_journal_v2_descriptor_block(sbi);
	u64 payload_start = cryexts_journal_v2_payload_start(sbi);
	u64 payload_blocks = cryexts_journal_v2_payload_area_blocks(sbi);
	u64 commit_block = cryexts_journal_v2_commit_block(sbi);
	u64 last_sequence;
	u64 active_sequence;
	u64 checkpoint_sequence;
	u32 state;
	u32 entries;
	u32 aggregate_checksum = 2166136261u;
	u32 expected;
	u32 commit_flags;
	unsigned int i;
	int err = -EUCLEAN;
	bool commit_valid;

	control_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_KERNEL);
	descriptor_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_KERNEL);
	commit_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_KERNEL);
	payload_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_KERNEL);
	if (!control_buf || !descriptor_buf || !commit_buf || !payload_buf) {
		err = -ENOMEM;
		goto out;
	}

	err = cryexts_read_file_block(sb, sbi->journal_block, control_buf);
	if (err)
		goto out;
	control = (struct cryexts_journal_v3_control *)control_buf;
	if (le32_to_cpu(control->magic) != CRYEXTS_JOURNAL_V3_MAGIC ||
	    le16_to_cpu(control->layout_version) !=
		    CRYEXTS_JOURNAL_V3_LAYOUT_VERSION ||
	    le16_to_cpu(control->block_type) !=
		    CRYEXTS_JOURNAL_V3_BLOCK_CONTROL ||
	    le32_to_cpu(control->features) != cryexts_journal_v3_features(sbi) ||
	    (!sbi->journal_ring &&
	     (le64_to_cpu(control->descriptor_block) != descriptor_block ||
	      le64_to_cpu(control->payload_start) != payload_start ||
	      le64_to_cpu(control->payload_blocks) != payload_blocks ||
	      le64_to_cpu(control->commit_block) != commit_block)))
		goto corrupt;
	if (!cryexts_journal_v3_ring_valid(sbi, control))
		goto corrupt;
	if (sbi->journal_ring) {
		sbi->journal_ring_head = le64_to_cpu(control->ring_head);
		sbi->journal_ring_tail = le64_to_cpu(control->ring_tail);
		descriptor_block = le64_to_cpu(control->descriptor_block);
		payload_start = le64_to_cpu(control->payload_start);
		payload_blocks = le64_to_cpu(control->payload_blocks);
		commit_block = le64_to_cpu(control->commit_block);
	}
	expected = cryexts_journal_v2_checksum(
		control_buf, CRYEXTS_BLOCK_SIZE,
		offsetof(struct cryexts_journal_v3_control, checksum));
	if (le32_to_cpu(control->checksum) != expected)
		goto corrupt;

	state = le32_to_cpu(control->state);
	if (state == CRYEXTS_JOURNAL_V3_STATE_IDLE) {
		if (!cryexts_journal_needs_recovery(sb)) {
			err = cryexts_journal_v3_validate_clean(sb);
			goto out;
		}
		last_sequence = le64_to_cpu(control->last_sequence);
		if (le64_to_cpu(control->active_sequence) ||
		    le64_to_cpu(control->checkpoint_sequence) != last_sequence)
			goto corrupt;
		goto discard;
	}
	if (state > CRYEXTS_JOURNAL_V3_STATE_CHECKPOINTING)
		goto corrupt;

	last_sequence = le64_to_cpu(control->last_sequence);
	active_sequence = le64_to_cpu(control->active_sequence);
	checkpoint_sequence = le64_to_cpu(control->checkpoint_sequence);
	if (!active_sequence || checkpoint_sequence > last_sequence)
		goto corrupt;
	if (state == CRYEXTS_JOURNAL_V3_STATE_CHECKPOINTING) {
		if (active_sequence != last_sequence)
			goto corrupt;
	} else if (active_sequence != last_sequence + 1) {
		goto corrupt;
	}
	if (state == CRYEXTS_JOURNAL_V3_STATE_CHECKPOINTING &&
	    !cryexts_journal_needs_recovery(sb))
		goto discard;

	if (state == CRYEXTS_JOURNAL_V3_STATE_ACTIVE)
		goto discard;

	err = cryexts_read_file_block(sb, descriptor_block, descriptor_buf);
	if (err)
		goto out;
	err = cryexts_read_file_block(sb, commit_block, commit_buf);
	if (err)
		goto out;

	descriptor = (struct cryexts_journal_v3_descriptor *)descriptor_buf;
	if (le32_to_cpu(descriptor->magic) != CRYEXTS_JOURNAL_V3_MAGIC ||
	    le16_to_cpu(descriptor->layout_version) !=
		    CRYEXTS_JOURNAL_V3_LAYOUT_VERSION ||
	    le16_to_cpu(descriptor->block_type) !=
		    CRYEXTS_JOURNAL_V3_BLOCK_DESCRIPTOR ||
	    le32_to_cpu(descriptor->flags) ||
	    le64_to_cpu(descriptor->sequence) != active_sequence ||
	    le64_to_cpu(descriptor->payload_start) != payload_start ||
	    le64_to_cpu(descriptor->commit_block) != commit_block)
		goto corrupt;
	entries = le32_to_cpu(descriptor->entry_count);
	if (entries > cryexts_journal_v3_payload_capacity(sbi))
		goto corrupt;
	expected = cryexts_journal_v2_checksum(
		descriptor_buf, CRYEXTS_BLOCK_SIZE,
		offsetof(struct cryexts_journal_v3_descriptor, checksum));
	if (le32_to_cpu(descriptor->checksum) != expected)
		goto corrupt;
	for (i = entries; i < CRYEXTS_JOURNAL_V3_MAX_ENTRIES; i++) {
		if (le64_to_cpu(descriptor->entries[i].home_block) ||
		    le32_to_cpu(descriptor->entries[i].payload_checksum) ||
		    le32_to_cpu(descriptor->entries[i].flags))
			goto corrupt;
	}

	commit = (struct cryexts_journal_v3_commit *)commit_buf;
	commit_valid =
		le32_to_cpu(commit->magic) == CRYEXTS_JOURNAL_V3_MAGIC &&
		le16_to_cpu(commit->layout_version) ==
			CRYEXTS_JOURNAL_V3_LAYOUT_VERSION &&
		le16_to_cpu(commit->block_type) ==
			CRYEXTS_JOURNAL_V3_BLOCK_COMMIT &&
		le64_to_cpu(commit->descriptor_block) == descriptor_block;
	if (commit_valid) {
		expected = cryexts_journal_v2_checksum(
			commit_buf, CRYEXTS_BLOCK_SIZE,
			offsetof(struct cryexts_journal_v3_commit, checksum));
		commit_valid = le32_to_cpu(commit->checksum) == expected;
	}
	if (!commit_valid) {
		if (state == CRYEXTS_JOURNAL_V3_STATE_PREPARED)
			goto discard;
		goto corrupt;
	}

	commit_flags = le32_to_cpu(commit->flags);
	if (!commit_flags) {
		if (state == CRYEXTS_JOURNAL_V3_STATE_PREPARED)
			goto discard;
		goto corrupt;
	}
	if (commit_flags != CRYEXTS_JOURNAL_V3_FLAG_COMMITTED ||
	    le32_to_cpu(commit->entry_count) != entries ||
	    le64_to_cpu(commit->sequence) != active_sequence ||
	    le32_to_cpu(commit->descriptor_checksum) !=
		    le32_to_cpu(descriptor->checksum))
		goto corrupt;

	/* Validate every payload before modifying any home block. */
	for (i = 0; i < entries; i++) {
		u64 home_block =
			le64_to_cpu(descriptor->entries[i].home_block);
		unsigned int j;

		if (home_block >= cryexts_blocks_count(sb) ||
		    cryexts_journal_block_is_internal(sbi, home_block) ||
		    le32_to_cpu(descriptor->entries[i].flags))
			goto corrupt;
		for (j = 0; j < i; j++) {
			if (le64_to_cpu(descriptor->entries[j].home_block) ==
			    home_block)
				goto corrupt;
		}
		err = cryexts_read_file_block(sb, payload_start + i, payload_buf);
		if (err)
			goto out;
		expected = cryexts_fnv1a_update(2166136261u, payload_buf,
					       CRYEXTS_BLOCK_SIZE);
		if (le32_to_cpu(descriptor->entries[i].payload_checksum) !=
		    expected)
			goto corrupt;
		aggregate_checksum = cryexts_fnv1a_update(
			aggregate_checksum, payload_buf, CRYEXTS_BLOCK_SIZE);
	}
	if (!entries)
		aggregate_checksum = 0;
	if (le32_to_cpu(commit->payload_checksum) != aggregate_checksum)
		goto corrupt;

	cryexts_journal_v3_prepare_control(
		sb, control_buf, CRYEXTS_JOURNAL_V3_STATE_CHECKPOINTING,
		active_sequence, active_sequence, checkpoint_sequence,
		descriptor_block, payload_start, entries, commit_block);
	err = cryexts_journal_v3_write_sync(sb, sbi->journal_block,
					    control_buf);
	if (err)
		goto out;

	sbi->journal_replaying = true;
	for (i = 0; i < entries; i++) {
		err = cryexts_read_file_block(sb, payload_start + i, payload_buf);
		if (err)
			break;
		err = cryexts_journal_v3_write_sync(
			sb, le64_to_cpu(descriptor->entries[i].home_block),
			payload_buf);
		if (err)
			break;
	}
	sbi->journal_replaying = false;
	if (err)
		goto out;
	last_sequence = active_sequence;

discard:
	sbi->journal_sequence = last_sequence;
	sbi->journal_last_sequence = last_sequence;
	sbi->journal_tail_sequence = last_sequence;
	sbi->journal_checkpoint_sequence = last_sequence;
	if (sbi->journal_ring) {
		sbi->journal_ring_tail = le64_to_cpu(control->ring_head);
		sbi->journal_ring_head = le64_to_cpu(control->ring_head);
	}
	sbi->disk_sb->journal_sequence = cpu_to_le64(last_sequence);
	sbi->journal_replaying = true;
	cryexts_super_set_recovery(sb, false);
	sbi->journal_replaying = false;
	err = cryexts_sync_single_block(sb, sbi->s_sbh->b_blocknr);
	if (err)
		goto out;
	err = cryexts_journal_v3_reset_disk(sb, last_sequence,
					    descriptor_buf, control_buf,
					    descriptor_block, payload_start,
					    payload_blocks, commit_block);
	if (!err)
		cryexts_journal_v3_clear_runtime(sbi);
	goto out;

corrupt:
	err = -EUCLEAN;
out:
	sbi->journal_replaying = false;
	kfree(payload_buf);
	kfree(commit_buf);
	kfree(descriptor_buf);
	kfree(control_buf);
	return err;
}

int cryexts_journal_begin(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	int err;

	if (!sbi->journal_enabled || sbi->journal_replaying)
		return 0;
	if (cryexts_journal_uses_v3(sb))
		return cryexts_journal_v3_begin(sb);
	if (cryexts_journal_uses_v2(sb))
		return cryexts_journal_v2_begin(sb);

	mutex_lock(&sbi->journal_lock);
	sbi->journal_entry_count = 0;
	memset(sbi->journal_home_blocks, 0, sizeof(sbi->journal_home_blocks));
	cryexts_super_set_recovery(sb, true);
	err = cryexts_sync_metadata(sb);
	if (err) {
		cryexts_super_set_recovery(sb, false);
		mutex_unlock(&sbi->journal_lock);
		return err;
	}
	return 0;
}

int cryexts_journal_record_block(struct super_block *sb, u64 home_block)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	struct cryexts_journal_header *jh;
	char *journal_buf;
	char *block_buf;
	unsigned int i;
	int err = 0;

	if (!sbi->journal_enabled || sbi->journal_replaying)
		return 0;
	if (cryexts_journal_uses_v3(sb))
		return cryexts_journal_v3_record_block(sb, home_block);
	if (cryexts_journal_uses_v2(sb))
		return cryexts_journal_v2_record_block(sb, home_block);
	if (!sbi->journal_blocks)
		return -EOPNOTSUPP;
	if (home_block >= cryexts_blocks_count(sb))
		return -EINVAL;
	if (!cryexts_data_block_valid(sb, home_block))
		return -EINVAL;
	if (cryexts_journal_block_is_internal(sbi, home_block))
		return -EINVAL;

	for (i = 0; i < sbi->journal_entry_count; i++) {
		if (sbi->journal_home_blocks[i] == home_block)
			return 0;
	}
	if (sbi->journal_entry_count >= CRYEXTS_JOURNAL_MAX_ENTRIES ||
	    sbi->journal_entry_count + 1 >= sbi->journal_blocks)
		return -ENOSPC;

	journal_buf = kzalloc(CRYEXTS_BLOCK_SIZE, GFP_NOFS);
	block_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_NOFS);
	if (!journal_buf || !block_buf) {
		err = -ENOMEM;
		goto out;
	}

	err = cryexts_read_file_block(sb, home_block, block_buf);
	if (err)
		goto out;
	err = cryexts_write_file_block(sb,
				       sbi->journal_block + 1 +
					       sbi->journal_entry_count,
				       block_buf);
	if (err)
		goto out;
	err = cryexts_sync_single_block(sb,
					sbi->journal_block + 1 +
						sbi->journal_entry_count);
	if (err)
		goto out;

	err = cryexts_journal_read_header(sb, journal_buf);
	if (err && err != -EIO)
		goto out;

	jh = (struct cryexts_journal_header *)journal_buf;
	if (!cryexts_journal_header_valid(sb, jh, NULL))
		memset(journal_buf, 0, CRYEXTS_BLOCK_SIZE);

	jh = (struct cryexts_journal_header *)journal_buf;
	jh->magic = cpu_to_le32(CRYEXTS_JOURNAL_MAGIC);
	jh->flags = cpu_to_le32(CRYEXTS_JOURNAL_FLAG_VALID);
	jh->entry_count = cpu_to_le32(sbi->journal_entry_count + 1);
	jh->sequence = cpu_to_le64(++sbi->journal_sequence);
	sbi->disk_sb->journal_sequence = cpu_to_le64(sbi->journal_sequence);
	jh->home_blocks[sbi->journal_entry_count] = cpu_to_le64(home_block);
	sbi->journal_home_blocks[sbi->journal_entry_count++] = home_block;
	jh->checksum = cpu_to_le32(cryexts_journal_checksum(journal_buf,
						CRYEXTS_BLOCK_SIZE));
	cryexts_mark_super_dirty(sb);

	err = cryexts_journal_write_header(sb, journal_buf);
	if (!err)
		err = cryexts_sync_single_block(sb, sbi->journal_block);

out:
	kfree(block_buf);
	kfree(journal_buf);
	return err;
}

int cryexts_journal_record_bh(struct super_block *sb, struct buffer_head *bh)
{
	if (!bh)
		return -EINVAL;
	return cryexts_journal_record_block(sb, bh->b_blocknr);
}

int cryexts_journal_commit(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	char *journal_buf;
	int err = 0;

	if (!sbi->journal_enabled || sbi->journal_replaying) {
		if (mutex_is_locked(&sbi->journal_lock))
			mutex_unlock(&sbi->journal_lock);
		return 0;
	}
	if (cryexts_journal_uses_v3(sb))
		return cryexts_journal_v3_commit(sb);
	if (cryexts_journal_uses_v2(sb))
		return cryexts_journal_v2_commit(sb);

	journal_buf = kzalloc(CRYEXTS_BLOCK_SIZE, GFP_NOFS);
	if (!journal_buf) {
		err = -ENOMEM;
		goto out_unlock;
	}

	cryexts_journal_prepare_header(journal_buf, 0, 0,
				       sbi->journal_sequence);
	err = cryexts_journal_write_header(sb, journal_buf);
	if (!err) {
		cryexts_super_set_recovery(sb, false);
		sbi->disk_sb->journal_sequence = cpu_to_le64(sbi->journal_sequence);
		cryexts_mark_super_dirty(sb);
		err = cryexts_sync_metadata(sb);
	}
	sbi->journal_entry_count = 0;
	memset(sbi->journal_home_blocks, 0, sizeof(sbi->journal_home_blocks));
	kfree(journal_buf);

out_unlock:
	mutex_unlock(&sbi->journal_lock);
	return err;
}

void cryexts_journal_abort(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);

	if (!sbi->journal_enabled || sbi->journal_replaying) {
		if (mutex_is_locked(&sbi->journal_lock))
			mutex_unlock(&sbi->journal_lock);
		return;
	}
	if (cryexts_journal_uses_v3(sb)) {
		cryexts_journal_v3_abort(sb);
		return;
	}
	if (cryexts_journal_uses_v2(sb)) {
		cryexts_journal_v2_abort(sb);
		return;
	}

	if (!sbi->journal_entry_count)
		cryexts_super_set_recovery(sb, false);
	sbi->journal_entry_count = 0;
	memset(sbi->journal_home_blocks, 0, sizeof(sbi->journal_home_blocks));
	if (mutex_is_locked(&sbi->journal_lock))
		mutex_unlock(&sbi->journal_lock);
}

int cryexts_journal_replay(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	char *journal_buf = NULL;
	char *block_buf = NULL;
	struct cryexts_journal_header *jh;
	u32 flags;
	u32 entries;
	unsigned int i;
	u64 blocks_count = cryexts_blocks_count(sb);
	u64 sequence;
	int err = 0;

	if (!sbi->journal_enabled)
		return 0;
	if (cryexts_journal_uses_v3(sb))
		return cryexts_journal_v3_replay(sb);
	if (!cryexts_journal_needs_recovery(sb))
		return 0;
	if (cryexts_journal_uses_v2(sb))
		return cryexts_journal_v2_replay(sb);

	journal_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_KERNEL);
	block_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_KERNEL);
	if (!journal_buf || !block_buf) {
		err = -ENOMEM;
		goto out;
	}

	err = cryexts_journal_read_header(sb, journal_buf);
	if (err)
		goto out;

	jh = (struct cryexts_journal_header *)journal_buf;
	if (le32_to_cpu(jh->magic) != CRYEXTS_JOURNAL_MAGIC) {
		cryexts_super_set_recovery(sb, false);
		err = 0;
		goto out;
	}
	if (!cryexts_journal_header_valid(sb, jh, &entries)) {
		err = -EUCLEAN;
		goto out;
	}

	flags = le32_to_cpu(jh->flags);
	if (!(flags & CRYEXTS_JOURNAL_FLAG_VALID) || !entries) {
		cryexts_super_set_recovery(sb, false);
		err = 0;
		goto out;
	}
	if (entries > CRYEXTS_JOURNAL_MAX_ENTRIES ||
	    entries + 1 > sbi->journal_blocks) {
		err = -EUCLEAN;
		goto out;
	}

	sbi->journal_replaying = true;
	for (i = 0; i < entries; i++) {
		u64 home_block = le64_to_cpu(jh->home_blocks[i]);

		if (!home_block || home_block >= blocks_count ||
		    !cryexts_data_block_valid(sb, home_block) ||
		    cryexts_journal_block_is_internal(sbi, home_block)) {
			err = -EUCLEAN;
			break;
		}
		err = cryexts_read_file_block(sb, sbi->journal_block + 1 + i, block_buf);
		if (err)
			break;
		err = cryexts_write_file_block(sb, home_block, block_buf);
		if (err)
			break;
	}
	sbi->journal_replaying = false;
	if (err)
		goto out;

	sequence = le64_to_cpu(jh->sequence);
	cryexts_journal_prepare_header(journal_buf, 0, 0, sequence);
	err = cryexts_journal_write_header(sb, journal_buf);
	if (!err) {
		sbi->journal_sequence = sequence;
		sbi->disk_sb->journal_sequence = cpu_to_le64(sequence);
		cryexts_super_set_recovery(sb, false);
		cryexts_mark_super_dirty(sb);
		err = cryexts_sync_metadata(sb);
	}

out:
	sbi->journal_replaying = false;
	kfree(block_buf);
	kfree(journal_buf);
	return err;
}
