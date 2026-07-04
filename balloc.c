// SPDX-License-Identifier: GPL-2.0
#include "cryexts.h"

unsigned int cryexts_inodes_per_block(void)
{
	return CRYEXTS_BLOCK_SIZE / sizeof(struct cryexts_inode);
}

u64 cryexts_max_inodes(struct super_block *sb)
{
	return cryexts_inodes_count(sb);
}

u64 cryexts_blocks_count(struct super_block *sb)
{
	return le64_to_cpu(CRYEXTS_SB(sb)->disk_sb->blocks_count);
}

u64 cryexts_inodes_count(struct super_block *sb)
{
	return le64_to_cpu(CRYEXTS_SB(sb)->disk_sb->inodes_count);
}

u64 cryexts_group_first_block(struct super_block *sb, u64 group)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);

	if (!sbi->groups || group >= sbi->group_count)
		return 0;
	return le64_to_cpu(sbi->groups[group].group_start);
}

u64 cryexts_group_blocks(struct super_block *sb, u64 group)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);

	if (!sbi->groups || group >= sbi->group_count)
		return 0;
	return le64_to_cpu(sbi->groups[group].blocks_count);
}

u64 cryexts_group_inode_table_start(struct super_block *sb, u64 group)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);

	if (!sbi->groups || group >= sbi->group_count)
		return 0;
	return le64_to_cpu(sbi->groups[group].inode_table_start);
}

u32 cryexts_group_inode_table_blocks(struct super_block *sb, u64 group)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);

	if (!sbi->groups || group >= sbi->group_count)
		return 0;
	return le32_to_cpu(sbi->groups[group].inode_table_blocks);
}

u32 cryexts_group_free_blocks(struct super_block *sb, u64 group)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);

	if (!sbi->groups || group >= sbi->group_count)
		return 0;
	return le32_to_cpu(sbi->groups[group].free_blocks_count);
}

u32 cryexts_group_free_inodes(struct super_block *sb, u64 group)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);

	if (!sbi->groups || group >= sbi->group_count)
		return 0;
	return le32_to_cpu(sbi->groups[group].free_inodes_count);
}

bool cryexts_bitmap_test(const unsigned char *bitmap, u64 bit)
{
	return !!(bitmap[bit / 8] & (1U << (bit % 8)));
}

void cryexts_bitmap_set(unsigned char *bitmap, u64 bit)
{
	bitmap[bit / 8] |= (unsigned char)(1U << (bit % 8));
}

void cryexts_bitmap_clear(unsigned char *bitmap, u64 bit)
{
	bitmap[bit / 8] &= (unsigned char)~(1U << (bit % 8));
}

void cryexts_mark_bitmap_dirty(struct cryexts_sb_info *sbi)
{
	u64 group;
	u64 gdt_index;

	if (sbi->group_count <= 1) {
		if (sbi->block_bitmap_bh) {
			cryexts_journal_record_bh(sbi->sb, sbi->block_bitmap_bh);
			mark_buffer_dirty(sbi->block_bitmap_bh);
		}
		if (sbi->inode_bitmap_bh) {
			cryexts_journal_record_bh(sbi->sb, sbi->inode_bitmap_bh);
			mark_buffer_dirty(sbi->inode_bitmap_bh);
		}
		return;
	}

	if (sbi->gdt_bhs) {
		cryexts_gdt_prepare_write(sbi->sb);
		for (gdt_index = 0; gdt_index < sbi->group_desc_table_blocks;
		     gdt_index++) {
			if (!sbi->gdt_bhs[gdt_index])
				continue;
			cryexts_journal_record_bh(sbi->sb, sbi->gdt_bhs[gdt_index]);
			mark_buffer_dirty(sbi->gdt_bhs[gdt_index]);
		}
	}
	for (group = 0; group < sbi->group_count; group++) {
		if (sbi->group_block_bitmap_bhs &&
		    sbi->group_block_bitmap_bhs[group]) {
			cryexts_journal_record_bh(sbi->sb,
				sbi->group_block_bitmap_bhs[group]);
			mark_buffer_dirty(sbi->group_block_bitmap_bhs[group]);
		}
		if (sbi->group_inode_bitmap_bhs &&
		    sbi->group_inode_bitmap_bhs[group]) {
			cryexts_journal_record_bh(sbi->sb,
				sbi->group_inode_bitmap_bhs[group]);
			mark_buffer_dirty(sbi->group_inode_bitmap_bhs[group]);
		}
	}
}

void cryexts_mark_super_dirty(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);

	sbi->disk_sb->next_ino = cpu_to_le64(sbi->next_ino);
	sbi->disk_sb->next_data_block = cpu_to_le64(sbi->next_data_block);
	cryexts_update_super_checksum(sb);
	cryexts_journal_record_bh(sb, sbi->s_sbh);
	mark_buffer_dirty(sbi->s_sbh);
}

bool cryexts_data_block_valid(struct super_block *sb, u64 block)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	u64 first_data_block = le64_to_cpu(sbi->disk_sb->first_data_block);
	u64 group;
	u64 group_start;
	u64 group_blocks;
	u64 group_data_start;

	if (!block || block >= cryexts_blocks_count(sb))
		return false;
	if (!cryexts_has_block_groups(sb))
		return block >= first_data_block;

	group = div_u64(block, sbi->blocks_per_group);
	if (group >= sbi->group_count || !sbi->groups)
		return false;

	group_start = le64_to_cpu(sbi->groups[group].group_start);
	group_blocks = le64_to_cpu(sbi->groups[group].blocks_count);
	if (block < group_start || block >= group_start + group_blocks)
		return false;

	if (group == 0)
		group_data_start = first_data_block;
	else
		group_data_start =
			le64_to_cpu(sbi->groups[group].inode_table_start) +
			le32_to_cpu(sbi->groups[group].inode_table_blocks);

	return block >= group_data_start;
}

bool cryexts_has_block_groups(struct super_block *sb)
{
	struct cryexts_super_block *disk_sb = CRYEXTS_SB(sb)->disk_sb;

	return !!(le32_to_cpu(disk_sb->features_incompat) &
		  CRYEXTS_FEATURE_INCOMPAT_BLOCK_GROUPS);
}

bool cryexts_inode_bitmap_used(struct super_block *sb, u64 ino)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);

	if (ino < CRYEXTS_ROOT_INO || ino > cryexts_inodes_count(sb))
		return false;
	if (!cryexts_has_block_groups(sb))
		return cryexts_bitmap_test(sbi->inode_bitmap, ino - 1);
	{
		u64 index = ino - 1;
		u64 group = div_u64(index, sbi->inodes_per_group);
		u64 bit = index % sbi->inodes_per_group;

		if (group >= sbi->group_count || !sbi->group_inode_bitmaps ||
		    !sbi->group_inode_bitmaps[group])
			return false;
		return cryexts_bitmap_test(sbi->group_inode_bitmaps[group], bit);
	}
}

bool cryexts_block_bitmap_used(struct super_block *sb, u64 block)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);

	if (!cryexts_data_block_valid(sb, block))
		return false;
	if (!cryexts_has_block_groups(sb))
		return cryexts_bitmap_test(sbi->block_bitmap, block);
	{
		u64 group = div_u64(block, sbi->blocks_per_group);
		u64 bit;

		if (group >= sbi->group_count || !sbi->group_block_bitmaps ||
		    !sbi->group_block_bitmaps[group])
			return false;
		bit = block - cryexts_group_first_block(sb, group);
		return cryexts_bitmap_test(sbi->group_block_bitmaps[group], bit);
	}
}

static int cryexts_alloc_inode_in_group(struct super_block *sb, u64 group,
					u64 *ino)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	unsigned char *bitmap;
	u64 base_ino;
	u64 limit;
	u64 bit;

	if (group >= sbi->group_count || !sbi->group_inode_bitmaps)
		return -EINVAL;

	bitmap = sbi->group_inode_bitmaps[group];
	if (!bitmap)
		return -EIO;

	base_ino = group * sbi->inodes_per_group + 1;
	limit = min_t(u64, sbi->inodes_per_group,
		      cryexts_inodes_count(sb) - group * sbi->inodes_per_group);
	for (bit = 0; bit < limit; bit++) {
		u64 candidate = base_ino + bit;

		if (candidate < CRYEXTS_ROOT_INO + 1)
			continue;
		if (!cryexts_bitmap_test(bitmap, bit)) {
			cryexts_bitmap_set(bitmap, bit);
			*ino = candidate;
			sbi->groups[group].free_inodes_count =
				cpu_to_le32(le32_to_cpu(
					sbi->groups[group].free_inodes_count) - 1);
			return 0;
		}
	}
	return -ENOSPC;
}

static int cryexts_alloc_block_in_group(struct super_block *sb, u64 group,
					u64 start_block_hint, u64 *block)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	unsigned char *bitmap;
	u64 first_block;
	u64 blocks;
	u64 start_bit;
	u64 bit;

	if (group >= sbi->group_count || !sbi->group_block_bitmaps)
		return -EINVAL;

	bitmap = sbi->group_block_bitmaps[group];
	if (!bitmap)
		return -EIO;

	first_block = cryexts_group_first_block(sb, group);
	blocks = cryexts_group_blocks(sb, group);
	if (!blocks)
		return -ENOSPC;

	start_bit = 0;
	if (start_block_hint >= first_block &&
	    start_block_hint < first_block + blocks)
		start_bit = start_block_hint - first_block;

	for (bit = start_bit; bit < blocks; bit++) {
		u64 candidate = first_block + bit;

		if (candidate < le64_to_cpu(sbi->disk_sb->first_data_block))
			continue;
		if (!cryexts_bitmap_test(bitmap, bit)) {
			cryexts_bitmap_set(bitmap, bit);
			*block = candidate;
			sbi->groups[group].free_blocks_count =
				cpu_to_le32(le32_to_cpu(
					sbi->groups[group].free_blocks_count) - 1);
			return 0;
		}
	}
	for (bit = 0; bit < start_bit; bit++) {
		u64 candidate = first_block + bit;

		if (candidate < le64_to_cpu(sbi->disk_sb->first_data_block))
			continue;
		if (!cryexts_bitmap_test(bitmap, bit)) {
			cryexts_bitmap_set(bitmap, bit);
			*block = candidate;
			sbi->groups[group].free_blocks_count =
				cpu_to_le32(le32_to_cpu(
					sbi->groups[group].free_blocks_count) - 1);
			return 0;
		}
	}
	return -ENOSPC;
}

static int cryexts_alloc_inode_legacy(struct super_block *sb, u64 *ino)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	u64 max = cryexts_inodes_count(sb);
	u64 i;

	for (i = CRYEXTS_ROOT_INO; i < max; i++) {
		if (!cryexts_bitmap_test(sbi->inode_bitmap, i)) {
			cryexts_bitmap_set(sbi->inode_bitmap, i);
			*ino = i + 1;
			return 0;
		}
	}
	return -ENOSPC;
}

static int cryexts_alloc_block_legacy(struct super_block *sb, u64 *block)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	u64 count = cryexts_blocks_count(sb);
	u64 start = le64_to_cpu(sbi->disk_sb->first_data_block);
	u64 i;

	for (i = start; i < count; i++) {
		if (!cryexts_bitmap_test(sbi->block_bitmap, i)) {
			cryexts_bitmap_set(sbi->block_bitmap, i);
			*block = i;
			return 0;
		}
	}
	return -ENOSPC;
}

static u64 cryexts_first_data_group(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);

	if (!cryexts_has_block_groups(sb) || !sbi->blocks_per_group)
		return 0;
	return div_u64(le64_to_cpu(sbi->disk_sb->first_data_block),
		       sbi->blocks_per_group);
}

bool cryexts_prealloc_feature_enabled(struct super_block *sb)
{
	return !!(le32_to_cpu(CRYEXTS_SB(sb)->disk_sb->features_compat) &
		  CRYEXTS_FEATURE_COMPAT_PREALLOC);
}

int cryexts_alloc_inode_goal(struct super_block *sb, u64 goal_group, u64 *ino)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	int err = -ENOSPC;
	u64 group;
	u64 start_group;
	u64 scanned;

	mutex_lock(&sbi->alloc_lock);
	if (!cryexts_has_block_groups(sb)) {
		err = cryexts_alloc_inode_legacy(sb, ino);
		if (!err) {
			if (sbi->next_ino < *ino + 1)
				sbi->next_ino = *ino + 1;
			sbi->disk_sb->free_inodes_count =
				cpu_to_le64(le64_to_cpu(
					sbi->disk_sb->free_inodes_count) - 1);
			cryexts_mark_bitmap_dirty(sbi);
			cryexts_mark_super_dirty(sb);
		}
		mutex_unlock(&sbi->alloc_lock);
		return err;
	}

	start_group = 0;
	if (cryexts_prealloc_feature_enabled(sb) && goal_group < sbi->group_count)
		start_group = goal_group;
	for (scanned = 0; scanned < sbi->group_count; scanned++) {
		group = (start_group + scanned) % sbi->group_count;
		if (!cryexts_group_free_inodes(sb, group))
			continue;
		err = cryexts_alloc_inode_in_group(sb, group, ino);
		if (!err)
			break;
	}
	if (!err) {
		if (sbi->next_ino < *ino + 1)
			sbi->next_ino = *ino + 1;
		sbi->disk_sb->free_inodes_count =
			cpu_to_le64(le64_to_cpu(sbi->disk_sb->free_inodes_count) - 1);
		cryexts_mark_bitmap_dirty(sbi);
		cryexts_mark_super_dirty(sb);
	}
	mutex_unlock(&sbi->alloc_lock);
	return err;
}

int cryexts_alloc_inode(struct super_block *sb, u64 *ino)
{
	return cryexts_alloc_inode_goal(sb, U64_MAX, ino);
}

int cryexts_free_inode(struct super_block *sb, u64 ino)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	struct buffer_head *bh;
	struct cryexts_inode *disk_inode;

	if (ino < CRYEXTS_ROOT_INO || ino > cryexts_inodes_count(sb))
		return -EINVAL;

	disk_inode = cryexts_get_disk_inode(sb, ino, &bh);
	if (!disk_inode)
		return -EIO;
	memset(disk_inode, 0, sizeof(*disk_inode));
	mark_buffer_dirty(bh);
	brelse(bh);

	mutex_lock(&sbi->alloc_lock);
	if (!cryexts_has_block_groups(sb)) {
		if (!cryexts_bitmap_test(sbi->inode_bitmap, ino - 1)) {
			mutex_unlock(&sbi->alloc_lock);
			return 0;
		}
		cryexts_bitmap_clear(sbi->inode_bitmap, ino - 1);
	} else {
		u64 group = (ino - 1) / sbi->inodes_per_group;
		u64 bit = (ino - 1) % sbi->inodes_per_group;

		if (group >= sbi->group_count ||
		    !cryexts_bitmap_test(sbi->group_inode_bitmaps[group], bit)) {
			mutex_unlock(&sbi->alloc_lock);
			return 0;
		}
		cryexts_bitmap_clear(sbi->group_inode_bitmaps[group], bit);
		sbi->groups[group].free_inodes_count =
			cpu_to_le32(le32_to_cpu(
				sbi->groups[group].free_inodes_count) + 1);
	}
	sbi->disk_sb->free_inodes_count =
		cpu_to_le64(le64_to_cpu(sbi->disk_sb->free_inodes_count) + 1);
	cryexts_mark_bitmap_dirty(sbi);
	cryexts_mark_super_dirty(sb);
	mutex_unlock(&sbi->alloc_lock);
	return 0;
}

int cryexts_alloc_block_goal(struct super_block *sb, u64 goal_block,
			     u64 goal_group, u64 *block)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	int err = -ENOSPC;
	u64 group;
	u64 start_group;
	u64 scanned;
	bool have_goal_block = goal_block &&
		cryexts_data_block_valid(sb, goal_block);
	bool use_prealloc = cryexts_prealloc_feature_enabled(sb);

	mutex_lock(&sbi->alloc_lock);
	if (!cryexts_has_block_groups(sb)) {
		err = cryexts_alloc_block_legacy(sb, block);
		if (!err) {
			sbi->disk_sb->free_blocks_count =
				cpu_to_le64(le64_to_cpu(
					sbi->disk_sb->free_blocks_count) - 1);
			sbi->next_data_block = *block + 1;
			cryexts_mark_bitmap_dirty(sbi);
			cryexts_mark_super_dirty(sb);
		}
		mutex_unlock(&sbi->alloc_lock);
		return err;
	}

	start_group = cryexts_first_data_group(sb);
	if (use_prealloc && goal_group < sbi->group_count)
		start_group = goal_group;
	else if (use_prealloc && have_goal_block)
		start_group = div_u64(goal_block, sbi->blocks_per_group);

	for (scanned = 0; scanned < sbi->group_count; scanned++) {
		u64 start_hint = 0;

		group = (start_group + scanned) % sbi->group_count;
		if (group < cryexts_first_data_group(sb))
			continue;
		if (!cryexts_group_free_blocks(sb, group))
			continue;
		if (use_prealloc) {
			if (have_goal_block &&
			    div_u64(goal_block, sbi->blocks_per_group) == group)
				start_hint = goal_block;
			else if (group == goal_group &&
				 sbi->next_data_block >=
					 cryexts_group_first_block(sb, group) &&
				 sbi->next_data_block <
					 cryexts_group_first_block(sb, group) +
					 cryexts_group_blocks(sb, group))
				start_hint = sbi->next_data_block;
		} else if (scanned == 0) {
			start_hint = sbi->next_data_block;
		}
		err = cryexts_alloc_block_in_group(sb, group, start_hint, block);
		if (!err)
			break;
	}
	if (err) {
		for (group = cryexts_first_data_group(sb);
		     group < sbi->group_count; group++) {
			if (!cryexts_group_free_blocks(sb, group))
				continue;
			err = cryexts_alloc_block_in_group(sb, group, 0, block);
			if (!err)
				break;
		}
	}
	if (!err) {
		sbi->disk_sb->free_blocks_count =
			cpu_to_le64(le64_to_cpu(sbi->disk_sb->free_blocks_count) - 1);
		sbi->next_data_block = *block + 1;
		cryexts_mark_bitmap_dirty(sbi);
		cryexts_mark_super_dirty(sb);
	}
	mutex_unlock(&sbi->alloc_lock);
	return err;
}

int cryexts_alloc_block(struct super_block *sb, u64 *block)
{
	return cryexts_alloc_block_goal(sb, 0, U64_MAX, block);
}

int cryexts_free_block(struct super_block *sb, u64 block)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);

	if (!cryexts_data_block_valid(sb, block))
		return -EINVAL;

	mutex_lock(&sbi->alloc_lock);
	if (!cryexts_has_block_groups(sb)) {
		if (!cryexts_bitmap_test(sbi->block_bitmap, block)) {
			mutex_unlock(&sbi->alloc_lock);
			return 0;
		}
		cryexts_bitmap_clear(sbi->block_bitmap, block);
	} else {
		u64 group = div_u64(block, sbi->blocks_per_group);
		u64 bit;

		if (group >= sbi->group_count) {
			mutex_unlock(&sbi->alloc_lock);
			return -EINVAL;
		}
		bit = block - cryexts_group_first_block(sb, group);
		if (!cryexts_bitmap_test(sbi->group_block_bitmaps[group], bit)) {
			mutex_unlock(&sbi->alloc_lock);
			return 0;
		}
		cryexts_bitmap_clear(sbi->group_block_bitmaps[group], bit);
		sbi->groups[group].free_blocks_count =
			cpu_to_le32(le32_to_cpu(
				sbi->groups[group].free_blocks_count) + 1);
	}
	sbi->disk_sb->free_blocks_count =
		cpu_to_le64(le64_to_cpu(sbi->disk_sb->free_blocks_count) + 1);
	cryexts_mark_bitmap_dirty(sbi);
	cryexts_mark_super_dirty(sb);
	mutex_unlock(&sbi->alloc_lock);
	return 0;
}

int cryexts_load_bitmaps(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	u64 group;

	if (!cryexts_has_block_groups(sb) || sbi->group_count <= 1) {
		sbi->block_bitmap_bh = sb_bread(sb, sbi->block_bitmap_block);
		if (!sbi->block_bitmap_bh)
			return -EIO;
		sbi->inode_bitmap_bh = sb_bread(sb, sbi->inode_bitmap_block);
		if (!sbi->inode_bitmap_bh)
			return -EIO;
		sbi->block_bitmap = (unsigned char *)sbi->block_bitmap_bh->b_data;
		sbi->inode_bitmap = (unsigned char *)sbi->inode_bitmap_bh->b_data;
		return 0;
	}

	sbi->group_block_bitmap_bhs =
		kcalloc(sbi->group_count, sizeof(*sbi->group_block_bitmap_bhs),
			GFP_KERNEL);
	sbi->group_inode_bitmap_bhs =
		kcalloc(sbi->group_count, sizeof(*sbi->group_inode_bitmap_bhs),
			GFP_KERNEL);
	sbi->group_block_bitmaps =
		kcalloc(sbi->group_count, sizeof(*sbi->group_block_bitmaps),
			GFP_KERNEL);
	sbi->group_inode_bitmaps =
		kcalloc(sbi->group_count, sizeof(*sbi->group_inode_bitmaps),
			GFP_KERNEL);
	if (!sbi->group_block_bitmap_bhs || !sbi->group_inode_bitmap_bhs ||
	    !sbi->group_block_bitmaps || !sbi->group_inode_bitmaps)
		return -ENOMEM;

	for (group = 0; group < sbi->group_count; group++) {
		u64 block_bitmap_block =
			le64_to_cpu(sbi->groups[group].block_bitmap_block);
		u64 inode_bitmap_block =
			le64_to_cpu(sbi->groups[group].inode_bitmap_block);

		sbi->group_block_bitmap_bhs[group] =
			sb_bread(sb, block_bitmap_block);
		if (!sbi->group_block_bitmap_bhs[group])
			return -EIO;
		sbi->group_inode_bitmap_bhs[group] =
			sb_bread(sb, inode_bitmap_block);
		if (!sbi->group_inode_bitmap_bhs[group])
			return -EIO;
		sbi->group_block_bitmaps[group] =
			(unsigned char *)sbi->group_block_bitmap_bhs[group]->b_data;
		sbi->group_inode_bitmaps[group] =
			(unsigned char *)sbi->group_inode_bitmap_bhs[group]->b_data;
	}
	return 0;
}

void cryexts_unload_bitmaps(struct cryexts_sb_info *sbi)
{
	u64 group;

	if (sbi->block_bitmap_bh)
		brelse(sbi->block_bitmap_bh);
	if (sbi->inode_bitmap_bh)
		brelse(sbi->inode_bitmap_bh);
	sbi->block_bitmap_bh = NULL;
	sbi->inode_bitmap_bh = NULL;
	sbi->block_bitmap = NULL;
	sbi->inode_bitmap = NULL;

	if (sbi->group_block_bitmap_bhs) {
		for (group = 0; group < sbi->group_count; group++) {
			if (sbi->group_block_bitmap_bhs[group])
				brelse(sbi->group_block_bitmap_bhs[group]);
		}
		kfree(sbi->group_block_bitmap_bhs);
		sbi->group_block_bitmap_bhs = NULL;
	}
	if (sbi->group_inode_bitmap_bhs) {
		for (group = 0; group < sbi->group_count; group++) {
			if (sbi->group_inode_bitmap_bhs[group])
				brelse(sbi->group_inode_bitmap_bhs[group]);
		}
		kfree(sbi->group_inode_bitmap_bhs);
		sbi->group_inode_bitmap_bhs = NULL;
	}
	kfree(sbi->group_block_bitmaps);
	kfree(sbi->group_inode_bitmaps);
	sbi->group_block_bitmaps = NULL;
	sbi->group_inode_bitmaps = NULL;
}
