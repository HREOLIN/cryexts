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

int cryexts_journal_begin(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	int err;

	if (!sbi->journal_enabled || sbi->journal_replaying)
		return 0;
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

	if (!sbi->journal_enabled || !cryexts_journal_needs_recovery(sb))
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
