// SPDX-License-Identifier: GPL-2.0
#include "cryexts.h"

static int cryexts_zero_new_block(struct super_block *sb, u64 block)
{
	struct buffer_head *bh;

	bh = sb_getblk(sb, block);
	if (!bh)
		return -EIO;

	lock_buffer(bh);
	memset(bh->b_data, 0, CRYEXTS_BLOCK_SIZE);
	set_buffer_uptodate(bh);
	mark_buffer_dirty(bh);
	unlock_buffer(bh);
	brelse(bh);
	return 0;
}

struct cryexts_inode_info *cryexts_inode_blocks(struct inode *inode)
{
	return inode->i_private;
}

static struct cryexts_inode_extra *cryexts_disk_inode_extra(struct cryexts_inode *disk_inode)
{
	return (struct cryexts_inode_extra *)
		(disk_inode->reserved + sizeof(disk_inode->reserved) -
		 sizeof(struct cryexts_inode_extra));
}

static bool cryexts_extent_tree_enabled(struct super_block *sb)
{
	return !!(le32_to_cpu(CRYEXTS_SB(sb)->disk_sb->features_incompat) &
		  CRYEXTS_FEATURE_INCOMPAT_EXTENT_TREE);
}

static bool cryexts_extent_tree_v2_enabled(struct super_block *sb)
{
	return le32_to_cpu(CRYEXTS_SB(sb)->disk_sb->version) >=
	       CRYEXTS_VERSION_V6;
}

static bool cryexts_extent_tree_v2_inode(const struct cryexts_inode_info *blocks)
{
	return blocks &&
	       !!(blocks->inode_flags & CRYEXTS_INODE_FLAG_EXTENT_TREE_V2);
}

static bool cryexts_disk_extent_tree_v2_inode(u32 inode_flags)
{
	return !!(inode_flags & CRYEXTS_INODE_FLAG_EXTENT_TREE_V2);
}

static unsigned int cryexts_extent_total_entries(
	struct cryexts_inode_info *blocks);
static struct cryexts_extent *cryexts_extent_entry(
	struct cryexts_inode_info *blocks, unsigned int index);
static void cryexts_free_extent_tree_v2_caches(
	struct cryexts_inode_info *blocks);

static struct cryexts_extent_root_ref *cryexts_disk_extent_root_refs(
	struct cryexts_inode *disk_inode)
{
	return (struct cryexts_extent_root_ref *)(disk_inode->reserved +
						  CRYEXTS_EXTENT_TREE_V2_ROOT_REFS_OFFSET);
}

static const struct cryexts_extent_root_ref *cryexts_disk_extent_root_refs_const(
	const struct cryexts_inode *disk_inode)
{
	return (const struct cryexts_extent_root_ref *)(disk_inode->reserved +
							CRYEXTS_EXTENT_TREE_V2_ROOT_REFS_OFFSET);
}

static u64 cryexts_block_group_of(struct super_block *sb, u64 block)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);

	if (!cryexts_has_block_groups(sb) || !sbi->blocks_per_group)
		return 0;
	return div_u64(block, sbi->blocks_per_group);
}

static u64 cryexts_inode_group_of(struct inode *inode)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(inode->i_sb);
	u64 index;

	if (!cryexts_has_block_groups(inode->i_sb) || !sbi->inodes_per_group)
		return U64_MAX;
	if (inode->i_ino < CRYEXTS_ROOT_INO)
		return U64_MAX;
	index = inode->i_ino - 1;
	return div_u64(index, sbi->inodes_per_group);
}

static void cryexts_reset_reservation(struct cryexts_inode_info *blocks)
{
	if (!blocks)
		return;
	blocks->reservation_start = 0;
	blocks->reservation_next = 0;
	blocks->reservation_end = 0;
}

static void cryexts_update_alloc_hint(struct inode *inode, u64 block)
{
	struct cryexts_inode_info *blocks = cryexts_inode_blocks(inode);

	if (!blocks || !block)
		return;
	blocks->alloc_hint_block = block + 1;
	blocks->alloc_goal_group = cryexts_block_group_of(inode->i_sb, block);
	if (blocks->reservation_start &&
	    (block < blocks->reservation_start ||
	     block >= blocks->reservation_end))
		cryexts_reset_reservation(blocks);
	if (blocks->reservation_start &&
	    block + 1 > blocks->reservation_next)
		blocks->reservation_next = block + 1;
}

void cryexts_set_inode_alloc_hint(struct inode *inode, u64 block)
{
	cryexts_update_alloc_hint(inode, block);
}

static void cryexts_init_inode_alloc_hint(struct inode *inode)
{
	struct cryexts_inode_info *blocks = cryexts_inode_blocks(inode);
	u64 first_block;
	u64 last_block = 0;
	unsigned int i;

	if (!blocks) {
		return;
	}

	blocks->alloc_goal_group = U64_MAX;
	blocks->alloc_hint_block = 0;
	cryexts_reset_reservation(blocks);

	first_block = cryexts_inode_first_block(inode);
	if (first_block)
		blocks->alloc_goal_group =
			cryexts_block_group_of(inode->i_sb, first_block);

	if (blocks->use_extents) {
		for (i = 0; i < cryexts_extent_total_entries(blocks); i++) {
			struct cryexts_extent *extent =
				cryexts_extent_entry(blocks, i);
			u64 physical;
			u32 len;

			if (!extent)
				continue;
			physical = le64_to_cpu(extent->physical_start);
			len = le32_to_cpu(extent->length);
			if (physical && len)
				last_block = physical + len - 1;
		}
	} else {
		unsigned int count = cryexts_dir_block_count(inode);

		if (count == 0 && S_ISREG(inode->i_mode))
			count = DIV_ROUND_UP_ULL(i_size_read(inode),
						 CRYEXTS_BLOCK_SIZE);
		for (i = 0; i < count; i++) {
			u64 block = cryexts_inode_block_at(inode, i);

			if (block)
				last_block = block;
		}
	}

	if (last_block)
		cryexts_update_alloc_hint(inode, last_block);
}

static int cryexts_alloc_data_block_for_inode(struct inode *inode,
					      struct cryexts_inode_info *blocks,
					      u64 *physical)
{
	struct super_block *sb = inode->i_sb;
	u64 goal = blocks->alloc_hint_block;
	int err;

	if (cryexts_prealloc_feature_enabled(sb) &&
	    cryexts_has_block_groups(sb)) {
		if (blocks->reservation_next &&
		    blocks->reservation_next < blocks->reservation_end)
			goal = blocks->reservation_next;
		else if (blocks->alloc_hint_block)
			goal = blocks->alloc_hint_block;

		err = cryexts_alloc_block_goal(sb, goal,
					       blocks->alloc_goal_group,
					       physical);
		if (!err) {
			u64 group = cryexts_block_group_of(sb, *physical);

			if (!blocks->reservation_start ||
			    *physical < blocks->reservation_start ||
			    *physical >= blocks->reservation_end ||
			    group != blocks->alloc_goal_group) {
				blocks->reservation_start = *physical;
				blocks->reservation_next = *physical + 1;
				blocks->reservation_end = min_t(
					u64, *physical +
						     CRYEXTS_RESERVATION_WINDOW_BLOCKS,
					cryexts_group_first_block(sb, group) +
						cryexts_group_blocks(sb, group));
			} else {
				blocks->reservation_next = *physical + 1;
			}
		}
		return err;
	}

	return cryexts_alloc_block_goal(sb, blocks->alloc_hint_block,
					blocks->alloc_goal_group, physical);
}

static unsigned int cryexts_extent_total_entries(struct cryexts_inode_info *blocks)
{
	unsigned int total = 0;
	unsigned int i;

	if (!blocks)
		return 0;
	if (cryexts_extent_tree_v2_inode(blocks)) {
		for (i = 0; i < blocks->extent_leaf_count; i++)
			total += blocks->extent_leaves[i].entries;
		return total;
	}
	return blocks->extent_entries + blocks->extent_overflow_entries;
}

static u32 cryexts_disk_extent_overflow_checksum(struct cryexts_inode *disk_inode)
{
	return le32_to_cpu(*(__le32 *)(disk_inode->reserved +
				       CRYEXTS_EXTENT_ROOT_OVERFLOW_CSUM_OFFSET));
}

static void cryexts_set_disk_extent_overflow_checksum(
	struct cryexts_inode *disk_inode, u32 checksum)
{
	*(__le32 *)(disk_inode->reserved +
		    CRYEXTS_EXTENT_ROOT_OVERFLOW_CSUM_OFFSET) =
		cpu_to_le32(checksum);
}

static u64 cryexts_extent_file_blocks_max_from_inline_max(u16 inline_max)
{
	if (inline_max == CRYEXTS_EXTENT_ROOT_INLINE_EXTENTS)
		return (u64)(CRYEXTS_EXTENT_ROOT_INLINE_EXTENTS +
			     CRYEXTS_EXTENTS_PER_BLOCK) *
		       CRYEXTS_MAX_EXTENT_BLOCKS;
	return (u64)CRYEXTS_MAX_INLINE_EXTENTS * CRYEXTS_MAX_EXTENT_BLOCKS;
}

static u64 cryexts_extent_file_blocks_max_from_inode_flags(u32 inode_flags,
							   u16 inline_max)
{
	if (cryexts_disk_extent_tree_v2_inode(inode_flags))
		return CRYEXTS_EXTENT_TREE_V2_FILE_BLOCKS_MAX;
	return cryexts_extent_file_blocks_max_from_inline_max(inline_max);
}

static u64 cryexts_extent_file_blocks_max_for_blocks(
	struct cryexts_inode_info *blocks)
{
	if (!blocks)
		return cryexts_extent_file_blocks_max_from_inline_max(
			CRYEXTS_MAX_INLINE_EXTENTS);
	return cryexts_extent_file_blocks_max_from_inode_flags(
		blocks->inode_flags, blocks->extent_inline_max);
}

static struct cryexts_extent *cryexts_extent_entry(struct cryexts_inode_info *blocks,
						   unsigned int index)
{
	unsigned int i;

	if (cryexts_extent_tree_v2_inode(blocks)) {
		for (i = 0; i < blocks->extent_leaf_count; i++) {
			struct cryexts_extent_leaf_cache *leaf =
				&blocks->extent_leaves[i];

			if (index < leaf->entries)
				return leaf->extents ? &leaf->extents[index] : NULL;
			index -= leaf->entries;
		}
		return NULL;
	}
	if (index < blocks->extent_entries)
		return &blocks->extents[index];
	index -= blocks->extent_entries;
	if (index >= blocks->extent_overflow_entries || !blocks->overflow_extents)
		return NULL;
	return &blocks->overflow_extents[index];
}

static void cryexts_free_extent_tree_v2_caches(struct cryexts_inode_info *blocks)
{
	unsigned int i;

	if (!blocks)
		return;
	for (i = 0; i < CRYEXTS_EXTENT_TREE_ROOT_REFS; i++) {
		kfree(blocks->extent_leaves[i].extents);
		blocks->extent_leaves[i].extents = NULL;
		blocks->extent_leaves[i].entries = 0;
		blocks->extent_leaves[i].block = 0;
		blocks->extent_leaves[i].checksum = 0;
		memset(&blocks->extent_root_refs[i], 0,
		       sizeof(blocks->extent_root_refs[i]));
	}
	blocks->extent_leaf_count = 0;
}

static int cryexts_load_extent_overflow(struct inode *inode,
					struct cryexts_inode_info *blocks,
					u16 overflow_entries)
{
	struct buffer_head *bh;
	struct cryexts_extent_header *eh;

	if (!blocks->extent_overflow_block) {
		if (overflow_entries)
			return -EUCLEAN;
		return 0;
	}
	if (!overflow_entries || overflow_entries > CRYEXTS_EXTENTS_PER_BLOCK)
		return -EUCLEAN;

	bh = sb_bread(inode->i_sb, blocks->extent_overflow_block);
	if (!bh)
		return -EIO;

	eh = (struct cryexts_extent_header *)bh->b_data;
	if (le16_to_cpu(eh->magic) != CRYEXTS_EXTENT_MAGIC ||
	    le16_to_cpu(eh->max) != CRYEXTS_EXTENTS_PER_BLOCK ||
	    le16_to_cpu(eh->entries) != overflow_entries) {
		brelse(bh);
		return -EUCLEAN;
	}
	if (cryexts_metadata_csum_enabled(inode->i_sb) &&
	    blocks->extent_overflow_checksum !=
		    cryexts_extent_overflow_checksum(
			    inode->i_sb, blocks->extent_overflow_block,
			    bh->b_data)) {
		brelse(bh);
		return -EUCLEAN;
	}

	blocks->overflow_extents = kmemdup(bh->b_data + sizeof(*eh),
					 overflow_entries * sizeof(struct cryexts_extent),
					 GFP_KERNEL);
	brelse(bh);
	if (!blocks->overflow_extents)
		return -ENOMEM;
	blocks->extent_overflow_entries = overflow_entries;
	return 0;
}

static int cryexts_write_extent_overflow(struct inode *inode,
					 struct cryexts_inode_info *blocks)
{
	struct buffer_head *bh;
	struct cryexts_extent_header *eh;

	if (!blocks->extent_overflow_entries) {
		blocks->extent_overflow_checksum = 0;
		if (!blocks->extent_overflow_block)
			return 0;
		return 0;
	}

	if (!blocks->extent_overflow_block || !blocks->overflow_extents)
		return -EUCLEAN;

	bh = sb_getblk(inode->i_sb, blocks->extent_overflow_block);
	if (!bh)
		return -EIO;

	lock_buffer(bh);
	memset(bh->b_data, 0, CRYEXTS_BLOCK_SIZE);
	eh = (struct cryexts_extent_header *)bh->b_data;
	eh->magic = cpu_to_le16(CRYEXTS_EXTENT_MAGIC);
	eh->entries = cpu_to_le16(blocks->extent_overflow_entries);
	eh->max = cpu_to_le16(CRYEXTS_EXTENTS_PER_BLOCK);
	eh->reserved = cpu_to_le16(0);
	memcpy(bh->b_data + sizeof(*eh), blocks->overflow_extents,
	       blocks->extent_overflow_entries * sizeof(struct cryexts_extent));
	blocks->extent_overflow_checksum = cryexts_metadata_csum_enabled(
		inode->i_sb) ?
		cryexts_extent_overflow_checksum(inode->i_sb,
						 blocks->extent_overflow_block,
						 bh->b_data) :
		0;
	set_buffer_uptodate(bh);
	cryexts_journal_record_bh(inode->i_sb, bh);
	mark_buffer_dirty(bh);
	unlock_buffer(bh);
	brelse(bh);
	return 0;
}

static int cryexts_load_extent_leaf(struct inode *inode,
				    struct cryexts_inode_info *blocks,
				    unsigned int leaf_index,
				    u64 block, u16 entries, u32 checksum)
{
	struct buffer_head *bh;
	struct cryexts_extent_header *eh;
	struct cryexts_extent *extents;

	if (leaf_index >= CRYEXTS_EXTENT_TREE_ROOT_REFS)
		return -EUCLEAN;
	if (!block || !entries || entries > CRYEXTS_EXTENTS_PER_BLOCK)
		return -EUCLEAN;

	bh = sb_bread(inode->i_sb, block);
	if (!bh)
		return -EIO;

	eh = (struct cryexts_extent_header *)bh->b_data;
	if (le16_to_cpu(eh->magic) != CRYEXTS_EXTENT_MAGIC ||
	    le16_to_cpu(eh->max) != CRYEXTS_EXTENTS_PER_BLOCK ||
	    le16_to_cpu(eh->entries) != entries) {
		brelse(bh);
		return -EUCLEAN;
	}
	if (cryexts_metadata_csum_enabled(inode->i_sb) &&
	    checksum != cryexts_extent_leaf_checksum(inode->i_sb, block,
						    bh->b_data)) {
		brelse(bh);
		return -EUCLEAN;
	}

	extents = kmemdup(bh->b_data + sizeof(*eh),
			  entries * sizeof(struct cryexts_extent),
			  GFP_KERNEL);
	brelse(bh);
	if (!extents)
		return -ENOMEM;

	blocks->extent_leaves[leaf_index].block = block;
	blocks->extent_leaves[leaf_index].entries = entries;
	blocks->extent_leaves[leaf_index].checksum = checksum;
	blocks->extent_leaves[leaf_index].extents = extents;
	return 0;
}

static int cryexts_write_extent_leaf(struct inode *inode,
				     struct cryexts_inode_info *blocks,
				     unsigned int leaf_index)
{
	struct cryexts_extent_leaf_cache *leaf;
	struct buffer_head *bh;
	struct cryexts_extent_header *eh;
	u32 checksum = 0;

	if (leaf_index >= blocks->extent_leaf_count)
		return -EUCLEAN;

	leaf = &blocks->extent_leaves[leaf_index];
	if (!leaf->block || !leaf->entries || !leaf->extents)
		return -EUCLEAN;

	bh = sb_getblk(inode->i_sb, leaf->block);
	if (!bh)
		return -EIO;

	lock_buffer(bh);
	memset(bh->b_data, 0, CRYEXTS_BLOCK_SIZE);
	eh = (struct cryexts_extent_header *)bh->b_data;
	eh->magic = cpu_to_le16(CRYEXTS_EXTENT_MAGIC);
	eh->entries = cpu_to_le16(leaf->entries);
	eh->max = cpu_to_le16(CRYEXTS_EXTENTS_PER_BLOCK);
	eh->reserved = cpu_to_le16(0);
	memcpy(bh->b_data + sizeof(*eh), leaf->extents,
	       leaf->entries * sizeof(struct cryexts_extent));
	if (cryexts_metadata_csum_enabled(inode->i_sb))
		checksum = cryexts_extent_leaf_checksum(inode->i_sb,
						       leaf->block,
						       bh->b_data);
	leaf->checksum = checksum;
	blocks->extent_root_refs[leaf_index].checksum = cpu_to_le32(checksum);
	set_buffer_uptodate(bh);
	cryexts_journal_record_bh(inode->i_sb, bh);
	mark_buffer_dirty(bh);
	unlock_buffer(bh);
	brelse(bh);
	return 0;
}

static int cryexts_alloc_extent_leaf_block(struct inode *inode,
					   struct cryexts_inode_info *blocks,
					   unsigned int leaf_index)
{
	u64 block;
	int err;

	if (leaf_index >= CRYEXTS_EXTENT_TREE_ROOT_REFS)
		return -ENOSPC;
	if (blocks->extent_leaves[leaf_index].block)
		return 0;

	err = cryexts_alloc_block_goal(inode->i_sb,
				       blocks->alloc_hint_block,
				       blocks->alloc_goal_group,
				       &block);
	if (err)
		return err;
	err = cryexts_zero_new_block(inode->i_sb, block);
	if (err) {
		cryexts_free_block(inode->i_sb, block);
		return err;
	}
	blocks->extent_leaves[leaf_index].block = block;
	blocks->extent_root_refs[leaf_index].leaf_block = cpu_to_le64(block);
	return 0;
}

static void cryexts_refresh_extent_tree_v2_refs(struct cryexts_inode_info *blocks)
{
	unsigned int i;

	for (i = 0; i < blocks->extent_leaf_count; i++) {
		struct cryexts_extent_leaf_cache *leaf = &blocks->extent_leaves[i];
		struct cryexts_extent_root_ref *ref = &blocks->extent_root_refs[i];

		ref->leaf_block = cpu_to_le64(leaf->block);
		ref->entries = cpu_to_le16(leaf->entries);
		ref->checksum = cpu_to_le32(leaf->checksum);
		if (leaf->entries && leaf->extents)
			ref->logical_start = leaf->extents[0].logical_start;
		else
			ref->logical_start = cpu_to_le64(0);
	}
	for (; i < CRYEXTS_EXTENT_TREE_ROOT_REFS; i++)
		memset(&blocks->extent_root_refs[i], 0,
		       sizeof(blocks->extent_root_refs[i]));
}

static void cryexts_drop_extent_leaf(struct cryexts_inode_info *blocks,
				     unsigned int leaf_index)
{
	unsigned int i;

	if (!blocks || leaf_index >= blocks->extent_leaf_count)
		return;

	kfree(blocks->extent_leaves[leaf_index].extents);
	blocks->extent_leaves[leaf_index].extents = NULL;

	for (i = leaf_index; i + 1 < blocks->extent_leaf_count; i++) {
		blocks->extent_leaves[i] = blocks->extent_leaves[i + 1];
		blocks->extent_root_refs[i] = blocks->extent_root_refs[i + 1];
	}

	memset(&blocks->extent_leaves[blocks->extent_leaf_count - 1], 0,
	       sizeof(blocks->extent_leaves[0]));
	memset(&blocks->extent_root_refs[blocks->extent_leaf_count - 1], 0,
	       sizeof(blocks->extent_root_refs[0]));
	blocks->extent_leaf_count--;
	cryexts_refresh_extent_tree_v2_refs(blocks);
}

static int cryexts_append_extent_entry_v2(struct inode *inode,
					  struct cryexts_inode_info *blocks,
					  u64 logical, u64 physical)
{
	struct cryexts_extent_leaf_cache *leaf;
	struct cryexts_extent *new_extents;
	unsigned int leaf_index;
	unsigned int entry_index = 0;
	bool new_leaf = false;
	int err;

	if (blocks->extent_leaf_count) {
		for (leaf_index = 0; leaf_index < blocks->extent_leaf_count;
		     leaf_index++) {
			leaf = &blocks->extent_leaves[leaf_index];

			for (entry_index = 0; entry_index < leaf->entries;
			     entry_index++) {
				u64 entry_logical =
					le64_to_cpu(leaf->extents[entry_index].
						     logical_start);

				if (logical < entry_logical)
					goto found_slot;
			}
		}
	}

	if (!blocks->extent_leaf_count ||
	    blocks->extent_leaves[blocks->extent_leaf_count - 1].entries >=
		    CRYEXTS_EXTENTS_PER_BLOCK) {
		if (blocks->extent_leaf_count >= CRYEXTS_EXTENT_TREE_ROOT_REFS)
			return -ENOSPC;
		leaf_index = blocks->extent_leaf_count;
		err = cryexts_alloc_extent_leaf_block(inode, blocks, leaf_index);
		if (err)
			return err;
		new_leaf = true;
		entry_index = 0;
	} else {
		leaf_index = blocks->extent_leaf_count - 1;
		entry_index = blocks->extent_leaves[leaf_index].entries;
	}

found_slot:
	leaf = &blocks->extent_leaves[leaf_index];
	if (leaf->entries >= CRYEXTS_EXTENTS_PER_BLOCK)
		return -ENOSPC;
	new_extents = krealloc(leaf->extents,
			       (leaf->entries + 1) *
				       sizeof(struct cryexts_extent),
			       GFP_KERNEL);
	if (!new_extents) {
		if (new_leaf && leaf->block) {
			cryexts_free_block(inode->i_sb, leaf->block);
			leaf->block = 0;
			blocks->extent_root_refs[leaf_index].leaf_block =
				cpu_to_le64(0);
		}
		return -ENOMEM;
	}
	leaf->extents = new_extents;
	memmove(&leaf->extents[entry_index + 1],
		&leaf->extents[entry_index],
		(leaf->entries - entry_index) * sizeof(leaf->extents[0]));
	leaf->extents[entry_index].logical_start = cpu_to_le64(logical);
	leaf->extents[entry_index].physical_start = cpu_to_le64(physical);
	leaf->extents[entry_index].length = cpu_to_le32(1);
	leaf->extents[entry_index].flags = cpu_to_le32(0);
	leaf->entries++;
	if (new_leaf)
		blocks->extent_leaf_count++;
	cryexts_refresh_extent_tree_v2_refs(blocks);
	return 0;
}

static int cryexts_insert_extent_after_v2(struct cryexts_inode_info *blocks,
					  unsigned int leaf_index,
					  unsigned int entry_index,
					  u64 logical, u64 physical,
					  u32 len)
{
	struct cryexts_extent_leaf_cache *leaf;
	struct cryexts_extent *new_extents;
	unsigned int tail_entries;

	if (!blocks || leaf_index >= blocks->extent_leaf_count)
		return -EUCLEAN;
	leaf = &blocks->extent_leaves[leaf_index];
	if (!leaf->extents || entry_index >= leaf->entries ||
	    leaf->entries >= CRYEXTS_EXTENTS_PER_BLOCK)
		return -ENOSPC;

	new_extents = krealloc(leaf->extents,
			       (leaf->entries + 1) *
				       sizeof(struct cryexts_extent),
			       GFP_KERNEL);
	if (!new_extents)
		return -ENOMEM;
	leaf->extents = new_extents;

	tail_entries = leaf->entries - entry_index - 1;
	memmove(&leaf->extents[entry_index + 2],
		&leaf->extents[entry_index + 1],
		tail_entries * sizeof(leaf->extents[0]));
	leaf->extents[entry_index + 1].logical_start = cpu_to_le64(logical);
	leaf->extents[entry_index + 1].physical_start = cpu_to_le64(physical);
	leaf->extents[entry_index + 1].length = cpu_to_le32(len);
	leaf->extents[entry_index + 1].flags = cpu_to_le32(0);
	leaf->entries++;
	cryexts_refresh_extent_tree_v2_refs(blocks);
	return 0;
}

static int cryexts_alloc_extent_overflow_block(struct inode *inode,
					       struct cryexts_inode_info *blocks)
{
	int err;
	u64 block;

	if (blocks->extent_overflow_block)
		return 0;

	err = cryexts_alloc_block_goal(inode->i_sb,
				       blocks->alloc_hint_block,
				       blocks->alloc_goal_group,
				       &block);
	if (err)
		return err;
	err = cryexts_zero_new_block(inode->i_sb, block);
	if (err) {
		cryexts_free_block(inode->i_sb, block);
		return err;
	}
	blocks->extent_overflow_block = block;
	return 0;
}

static int cryexts_append_extent_entry(struct inode *inode,
				       struct cryexts_inode_info *blocks,
				       u64 logical, u64 physical)
{
	struct cryexts_extent *dst;
	struct cryexts_extent *new_extents;
	int err;

	if (cryexts_extent_tree_v2_inode(blocks))
		return cryexts_append_extent_entry_v2(inode, blocks,
						      logical, physical);

	if (blocks->extent_entries < blocks->extent_inline_max) {
		dst = &blocks->extents[blocks->extent_entries++];
		dst->logical_start = cpu_to_le64(logical);
		dst->physical_start = cpu_to_le64(physical);
		dst->length = cpu_to_le32(1);
		dst->flags = cpu_to_le32(0);
		return 0;
	}

	if (blocks->extent_overflow_entries >= CRYEXTS_EXTENTS_PER_BLOCK)
		return -ENOSPC;

	err = cryexts_alloc_extent_overflow_block(inode, blocks);
	if (err)
		return err;

	new_extents = krealloc(blocks->overflow_extents,
			       (blocks->extent_overflow_entries + 1) *
				       sizeof(struct cryexts_extent),
			       GFP_KERNEL);
	if (!new_extents)
		return -ENOMEM;
	blocks->overflow_extents = new_extents;
	dst = &blocks->overflow_extents[blocks->extent_overflow_entries++];
	dst->logical_start = cpu_to_le64(logical);
	dst->physical_start = cpu_to_le64(physical);
	dst->length = cpu_to_le32(1);
	dst->flags = cpu_to_le32(0);
	return 0;
}

static int cryexts_try_merge_last_extent(struct inode *inode,
					 struct cryexts_inode_info *blocks,
					 u64 logical, u64 *physical_out)
{
	struct cryexts_extent *last;
	u64 last_logical;
	u64 last_physical;
	u32 last_len;
	u64 physical;
	int err;

	if (!cryexts_extent_total_entries(blocks))
		return 0;

	last = cryexts_extent_entry(blocks, cryexts_extent_total_entries(blocks) - 1);
	if (!last)
		return 0;

	last_logical = le64_to_cpu(last->logical_start);
	last_physical = le64_to_cpu(last->physical_start);
	last_len = le32_to_cpu(last->length);

	if (logical != last_logical + last_len ||
	    last_len >= CRYEXTS_MAX_EXTENT_BLOCKS)
		return 0;

	err = cryexts_alloc_block_goal(inode->i_sb, last_physical + last_len,
				       blocks->alloc_goal_group, &physical);
	if (err)
		return err;
	if (physical != last_physical + last_len) {
		err = cryexts_free_block(inode->i_sb, physical);
		return err;
	}

	err = cryexts_zero_new_block(inode->i_sb, physical);
	if (err) {
		cryexts_free_block(inode->i_sb, physical);
		return err;
	}

	last->length = cpu_to_le32(last_len + 1);
	cryexts_update_alloc_hint(inode, physical);
	*physical_out = physical;
	return 1;
}

u64 cryexts_inode_xattr_block(struct inode *inode)
{
	struct cryexts_inode_info *blocks = cryexts_inode_blocks(inode);

	return blocks ? blocks->xattr_block : 0;
}

u32 cryexts_inode_policy_id(struct inode *inode)
{
	struct cryexts_inode_info *blocks = cryexts_inode_blocks(inode);

	return blocks ? blocks->encryption_policy_id : 0;
}

int cryexts_set_inode_policy_id(struct inode *inode, u32 policy_id)
{
	struct cryexts_inode_info *blocks = cryexts_inode_blocks(inode);

	if (!blocks)
		return -EIO;
	blocks->encryption_policy_id = policy_id;
	return 0;
}

bool cryexts_disk_inode_uses_extents(struct cryexts_inode *disk_inode)
{
	return !!(le32_to_cpu(disk_inode->inode_flags) &
		  CRYEXTS_INODE_FLAG_EXTENTS);
}

bool cryexts_inode_uses_extents(struct inode *inode)
{
	struct cryexts_inode_info *blocks = cryexts_inode_blocks(inode);

	return blocks && blocks->use_extents;
}

void cryexts_free_inode_blocks(struct inode *inode)
{
	struct cryexts_inode_info *blocks = cryexts_inode_blocks(inode);

	if (blocks) {
		cryexts_free_extent_tree_v2_caches(blocks);
		kfree(blocks->overflow_extents);
	}
	kfree(cryexts_inode_blocks(inode));
	inode->i_private = NULL;
}

int cryexts_init_inode_blocks(struct inode *inode,
			      struct cryexts_inode *disk_inode)
{
	struct cryexts_inode_info *blocks;
	struct cryexts_extent_header *eh;
	unsigned int i;

	blocks = kzalloc(sizeof(*blocks), GFP_KERNEL);
	if (!blocks)
		return -ENOMEM;

	blocks->inode_flags = le32_to_cpu(disk_inode->inode_flags);
	blocks->use_extents = cryexts_disk_inode_uses_extents(disk_inode);
	blocks->xattr_block =
		le64_to_cpu(cryexts_disk_inode_extra(disk_inode)->xattr_block);
	blocks->encryption_policy_id =
		le32_to_cpu(cryexts_disk_inode_extra(disk_inode)->encryption_policy_id);
	blocks->next_orphan =
		le64_to_cpu(cryexts_disk_inode_extra(disk_inode)->next_orphan);
	if (S_ISDIR(le16_to_cpu(disk_inode->mode)) &&
	    (blocks->inode_flags & CRYEXTS_INODE_FLAG_DIR_INDEX))
		blocks->dir_index_block =
			le64_to_cpu(disk_inode->indirect_block);
	if (blocks->use_extents) {
		u16 inline_max;
		u16 entries;
		u16 overflow_entries = 0;
		int err;

		eh = (struct cryexts_extent_header *)disk_inode->reserved;
		inline_max = le16_to_cpu(eh->max);
		entries = le16_to_cpu(eh->entries);
		if (le16_to_cpu(eh->magic) != CRYEXTS_EXTENT_MAGIC) {
			kfree(blocks);
			return -EUCLEAN;
		}
		if (cryexts_disk_extent_tree_v2_inode(blocks->inode_flags) &&
		    cryexts_extent_tree_v2_enabled(inode->i_sb)) {
			const struct cryexts_extent_root_ref *refs;

			if (inline_max != CRYEXTS_EXTENT_TREE_ROOT_REFS ||
			    entries > CRYEXTS_EXTENT_TREE_ROOT_REFS ||
			    le16_to_cpu(eh->reserved) !=
				    CRYEXTS_EXTENT_TREE_V2_DEPTH) {
				kfree(blocks);
				return -EUCLEAN;
			}
			blocks->extent_inline_max = CRYEXTS_EXTENT_TREE_ROOT_REFS;
			blocks->extent_leaf_count = entries;
			refs = cryexts_disk_extent_root_refs_const(disk_inode);
			for (i = 0; i < entries; i++) {
				u64 leaf_block = le64_to_cpu(refs[i].leaf_block);
				u16 leaf_entries = le16_to_cpu(refs[i].entries);
				u32 checksum = le32_to_cpu(refs[i].checksum);

				blocks->extent_root_refs[i] = refs[i];
				err = cryexts_load_extent_leaf(inode, blocks, i,
							       leaf_block,
							       leaf_entries,
							       checksum);
				if (err) {
					cryexts_free_extent_tree_v2_caches(blocks);
					kfree(blocks);
					return err;
				}
			}
		} else if (inline_max == CRYEXTS_MAX_INLINE_EXTENTS) {
			if (entries > CRYEXTS_MAX_INLINE_EXTENTS) {
				kfree(blocks);
				return -EUCLEAN;
			}
			blocks->extent_inline_max = CRYEXTS_MAX_INLINE_EXTENTS;
			blocks->extent_entries = entries;
			memcpy(blocks->extents,
			       disk_inode->reserved + sizeof(*eh),
			       sizeof(blocks->extents));
		} else if (inline_max == CRYEXTS_EXTENT_ROOT_INLINE_EXTENTS &&
			   cryexts_extent_tree_enabled(inode->i_sb)) {
			if (entries > CRYEXTS_EXTENT_ROOT_INLINE_EXTENTS) {
				kfree(blocks);
				return -EUCLEAN;
			}
			blocks->extent_inline_max = CRYEXTS_EXTENT_ROOT_INLINE_EXTENTS;
			blocks->extent_entries = entries;
			memcpy(blocks->extents,
			       disk_inode->reserved + sizeof(*eh),
			       entries * sizeof(blocks->extents[0]));
			blocks->extent_overflow_block =
				le64_to_cpu(*(__le64 *)(disk_inode->reserved +
						CRYEXTS_EXTENT_ROOT_OVERFLOW_OFFSET));
			overflow_entries =
				le16_to_cpu(*(__le16 *)(disk_inode->reserved +
						CRYEXTS_EXTENT_ROOT_OVERFLOW_OFFSET +
						sizeof(__le64)));
			blocks->extent_overflow_checksum =
				cryexts_disk_extent_overflow_checksum(disk_inode);
			err = cryexts_load_extent_overflow(inode, blocks,
							  overflow_entries);
			if (err) {
				kfree(blocks);
				return err;
			}
		} else {
			kfree(blocks);
			return -EUCLEAN;
		}
		inode->i_private = blocks;
		cryexts_init_inode_alloc_hint(inode);
		return 0;
	}

	for (i = 0; i < CRYEXTS_DIRECT_BLOCKS; i++)
		blocks->direct[i] = le64_to_cpu(disk_inode->block[i]);
	blocks->indirect_block = le64_to_cpu(disk_inode->indirect_block);

	inode->i_private = blocks;
	cryexts_init_inode_alloc_hint(inode);
	return 0;
}

u64 cryexts_inode_first_block(struct inode *inode)
{
	struct cryexts_inode_info *blocks = cryexts_inode_blocks(inode);

	if (blocks && blocks->use_extents) {
		if (cryexts_extent_tree_v2_inode(blocks) &&
		    blocks->extent_leaf_count &&
		    blocks->extent_leaves[0].entries &&
		    blocks->extent_leaves[0].extents)
			return le64_to_cpu(
				blocks->extent_leaves[0].extents[0].physical_start);
		if (blocks->extent_entries > 0)
			return le64_to_cpu(blocks->extents[0].physical_start);
	}
	return blocks ? blocks->direct[0] : 0;
}

u64 cryexts_disk_inode_indirect_block(struct cryexts_inode *disk_inode)
{
	return le64_to_cpu(disk_inode->indirect_block);
}

u64 cryexts_inode_indirect_block(struct inode *inode)
{
	struct cryexts_inode_info *blocks = cryexts_inode_blocks(inode);

	return blocks ? blocks->indirect_block : 0;
}

unsigned int cryexts_disk_inode_block_count(struct super_block *sb,
					    struct cryexts_inode *disk_inode)
{
	struct cryexts_extent_header *eh;
	struct cryexts_extent *extents;
	unsigned int i;
	unsigned int count = 0;
	struct buffer_head *bh;
	__le64 *entries;

	if (cryexts_disk_inode_uses_extents(disk_inode)) {
		u16 inline_max;
		u16 entries_count;

		eh = (struct cryexts_extent_header *)disk_inode->reserved;
		inline_max = le16_to_cpu(eh->max);
		entries_count = le16_to_cpu(eh->entries);
		if (le16_to_cpu(eh->magic) != CRYEXTS_EXTENT_MAGIC)
			return 0;
		if (cryexts_disk_extent_tree_v2_inode(
			    le32_to_cpu(disk_inode->inode_flags)) &&
		    cryexts_extent_tree_v2_enabled(sb)) {
			const struct cryexts_extent_root_ref *refs;
			unsigned int j;

			if (inline_max != CRYEXTS_EXTENT_TREE_ROOT_REFS ||
			    entries_count > CRYEXTS_EXTENT_TREE_ROOT_REFS ||
			    le16_to_cpu(eh->reserved) !=
				    CRYEXTS_EXTENT_TREE_V2_DEPTH)
				return 0;
			refs = cryexts_disk_extent_root_refs_const(disk_inode);
			for (i = 0; i < entries_count; i++) {
				u64 leaf_block = le64_to_cpu(refs[i].leaf_block);
				u16 leaf_entries = le16_to_cpu(refs[i].entries);
				struct cryexts_extent_header *obh;

				if (!leaf_block || !leaf_entries ||
				    leaf_entries > CRYEXTS_EXTENTS_PER_BLOCK)
					return 0;
				bh = sb_bread(sb, leaf_block);
				if (!bh)
					return 0;
				obh = (struct cryexts_extent_header *)bh->b_data;
				if (le16_to_cpu(obh->magic) != CRYEXTS_EXTENT_MAGIC ||
				    le16_to_cpu(obh->max) != CRYEXTS_EXTENTS_PER_BLOCK ||
				    le16_to_cpu(obh->entries) != leaf_entries) {
					brelse(bh);
					return 0;
				}
				extents = (struct cryexts_extent *)(bh->b_data +
								 sizeof(*obh));

				for (j = 0; j < leaf_entries; j++)
					count += le32_to_cpu(extents[j].length);
				brelse(bh);
				count++;
			}
			return count;
		} else if (inline_max == CRYEXTS_MAX_INLINE_EXTENTS) {
			if (entries_count > CRYEXTS_MAX_INLINE_EXTENTS)
				return 0;
		} else if (inline_max == CRYEXTS_EXTENT_ROOT_INLINE_EXTENTS &&
			   cryexts_extent_tree_enabled(sb)) {
			u64 overflow_block;
			u16 overflow_entries;
			struct cryexts_extent_header *obh;

			if (entries_count > CRYEXTS_EXTENT_ROOT_INLINE_EXTENTS)
				return 0;
			overflow_block = le64_to_cpu(*(__le64 *)(disk_inode->reserved +
						CRYEXTS_EXTENT_ROOT_OVERFLOW_OFFSET));
			overflow_entries =
				le16_to_cpu(*(__le16 *)(disk_inode->reserved +
						CRYEXTS_EXTENT_ROOT_OVERFLOW_OFFSET +
						sizeof(__le64)));
			if (!!overflow_block != !!overflow_entries)
				return 0;
			if (overflow_block) {
				bh = sb_bread(sb, overflow_block);
				if (!bh)
					return 0;
				obh = (struct cryexts_extent_header *)bh->b_data;
				if (le16_to_cpu(obh->magic) != CRYEXTS_EXTENT_MAGIC ||
				    le16_to_cpu(obh->max) != CRYEXTS_EXTENTS_PER_BLOCK ||
				    le16_to_cpu(obh->entries) != overflow_entries) {
					brelse(bh);
					return 0;
				}
				extents = (struct cryexts_extent *)(bh->b_data +
							 sizeof(*obh));
				for (i = 0; i < overflow_entries; i++)
					count += le32_to_cpu(extents[i].length);
				brelse(bh);
				count++;
			}
		} else {
			return 0;
		}
		extents = (struct cryexts_extent *)(disk_inode->reserved +
						    sizeof(*eh));
		for (i = 0; i < entries_count; i++)
			count += le32_to_cpu(extents[i].length);
		return count;
	}

	for (i = 0; i < CRYEXTS_DIRECT_BLOCKS; i++) {
		if (le64_to_cpu(disk_inode->block[i]))
			count++;
	}
	if (S_ISDIR(le16_to_cpu(disk_inode->mode)) &&
	    (le32_to_cpu(disk_inode->inode_flags) & CRYEXTS_INODE_FLAG_DIR_INDEX) &&
	    le64_to_cpu(disk_inode->indirect_block))
		return count + 1;
	if (!le64_to_cpu(disk_inode->indirect_block))
		return count;

	bh = sb_bread(sb, le64_to_cpu(disk_inode->indirect_block));
	if (!bh)
		return count + 1;

	count++;
	entries = (__le64 *)bh->b_data;
	for (i = 0; i < CRYEXTS_INDIRECT_BLOCKS; i++) {
		if (le64_to_cpu(entries[i]))
			count++;
	}
	brelse(bh);
	return count;
}

unsigned int cryexts_inode_block_count(struct inode *inode)
{
	struct cryexts_inode_info *blocks = cryexts_inode_blocks(inode);
	unsigned int i;
	unsigned int count = 0;
	struct buffer_head *bh;
	__le64 *entries;

	if (!blocks)
		return 0;
	if (blocks->use_extents) {
		for (i = 0; i < cryexts_extent_total_entries(blocks); i++) {
			struct cryexts_extent *extent =
				cryexts_extent_entry(blocks, i);

			if (extent)
				count += le32_to_cpu(extent->length);
		}
		if (cryexts_extent_tree_v2_inode(blocks))
			return count + blocks->extent_leaf_count;
		if (blocks->extent_overflow_block)
			count++;
		return count;
	}
	for (i = 0; i < CRYEXTS_DIRECT_BLOCKS; i++) {
		if (blocks->direct[i])
			count++;
	}
	if (S_ISDIR(inode->i_mode) &&
	    (blocks->inode_flags & CRYEXTS_INODE_FLAG_DIR_INDEX) &&
	    blocks->dir_index_block)
		return count + 1;
	if (!blocks->indirect_block)
		return count;

	bh = sb_bread(inode->i_sb, blocks->indirect_block);
	if (!bh)
		return count + 1;

	count++;
	entries = (__le64 *)bh->b_data;
	for (i = 0; i < CRYEXTS_INDIRECT_BLOCKS; i++) {
		if (le64_to_cpu(entries[i]))
			count++;
	}
	brelse(bh);
	return count;
}

unsigned int cryexts_inode_block_sectors(struct inode *inode)
{
	return cryexts_inode_block_count(inode) * (CRYEXTS_BLOCK_SIZE / 512);
}

u64 cryexts_regular_file_max_size(void)
{
	return (u64)CRYEXTS_FILE_BLOCKS_MAX * CRYEXTS_BLOCK_SIZE;
}

u64 cryexts_regular_file_max_size_for_inode(struct inode *inode)
{
	struct cryexts_inode_info *blocks = cryexts_inode_blocks(inode);

	if (cryexts_inode_uses_extents(inode))
		return cryexts_extent_file_blocks_max_for_blocks(blocks) *
		       CRYEXTS_BLOCK_SIZE;
	return cryexts_regular_file_max_size();
}

size_t cryexts_symlink_size_limit(void)
{
	return cryexts_regular_file_max_size() - 1;
}

unsigned int cryexts_dir_block_count(struct inode *inode)
{
	u64 size = i_size_read(inode);

	if (!size)
		return 0;
	return DIV_ROUND_UP_ULL(size, CRYEXTS_BLOCK_SIZE);
}

u64 cryexts_inode_block_at(struct inode *inode, unsigned int index)
{
	struct cryexts_inode_info *blocks = cryexts_inode_blocks(inode);
	unsigned int i;
	struct buffer_head *bh;
	__le64 *entries;
	u64 block = 0;

	if (!blocks)
		return 0;
	if (blocks->use_extents) {
		for (i = 0; i < cryexts_extent_total_entries(blocks); i++) {
			struct cryexts_extent *extent =
				cryexts_extent_entry(blocks, i);
			u64 logical;
			u64 physical;
			u32 len;

			if (!extent)
				continue;
			logical = le64_to_cpu(extent->logical_start);
			physical = le64_to_cpu(extent->physical_start);
			len = le32_to_cpu(extent->length);

			if (index < logical || index >= logical + len)
				continue;
			return physical + (index - logical);
		}
		return 0;
	}
	if (index < CRYEXTS_DIRECT_BLOCKS)
		return blocks->direct[index];
	if (!blocks->indirect_block)
		return 0;

	index -= CRYEXTS_DIRECT_BLOCKS;
	if (index >= CRYEXTS_INDIRECT_BLOCKS)
		return 0;

	bh = sb_bread(inode->i_sb, blocks->indirect_block);
	if (!bh)
		return 0;
	entries = (__le64 *)bh->b_data;
	block = le64_to_cpu(entries[index]);
	brelse(bh);
	return block;
}

int cryexts_resolve_block(struct inode *inode, u64 logical, bool create,
			  u64 *block)
{
	struct cryexts_inode_info *blocks = cryexts_inode_blocks(inode);
	u64 physical = 0;
	unsigned int i;
	struct buffer_head *bh;
	__le64 *entries;
	int err;

	if (!blocks)
		return -EFBIG;

	if (blocks->use_extents) {
		int merge_res;

		if (logical >= cryexts_extent_file_blocks_max_for_blocks(blocks))
			return -EFBIG;

		for (i = 0; i < cryexts_extent_total_entries(blocks); i++) {
			struct cryexts_extent *extent =
				cryexts_extent_entry(blocks, i);
			u64 logical_start;
			u64 physical_start;
			u32 len;

			if (!extent)
				continue;
			logical_start = le64_to_cpu(extent->logical_start);
			physical_start = le64_to_cpu(extent->physical_start);
			len = le32_to_cpu(extent->length);

			if (logical < logical_start || logical >= logical_start + len)
				continue;
			*block = physical_start + (logical - logical_start);
			return 0;
		}

		if (!create) {
			*block = 0;
			return 0;
		}

		merge_res = cryexts_try_merge_last_extent(inode, blocks, logical,
							   &physical);
		if (merge_res < 0)
			return merge_res;
		if (merge_res > 0) {
			*block = physical;
			return 0;
		}

		err = cryexts_alloc_data_block_for_inode(inode, blocks,
							 &physical);
		if (err)
			return err;
		err = cryexts_zero_new_block(inode->i_sb, physical);
		if (err) {
			cryexts_free_block(inode->i_sb, physical);
			return err;
		}

		err = cryexts_append_extent_entry(inode, blocks, logical, physical);
		if (err) {
			cryexts_free_block(inode->i_sb, physical);
			return err;
		}
		cryexts_update_alloc_hint(inode, physical);
		*block = physical;
		return 0;
	}

	if (logical >= CRYEXTS_FILE_BLOCKS_MAX)
		return -EFBIG;

	if (logical < CRYEXTS_DIRECT_BLOCKS) {
		if (!blocks->direct[logical] && create) {
			err = cryexts_alloc_data_block_for_inode(inode, blocks,
								 &physical);
			if (err)
				return err;
			err = cryexts_zero_new_block(inode->i_sb, physical);
			if (err) {
				cryexts_free_block(inode->i_sb, physical);
				return err;
			}
			blocks->direct[logical] = physical;
			cryexts_update_alloc_hint(inode, physical);
		}
		*block = blocks->direct[logical];
		return 0;
	}

	logical -= CRYEXTS_DIRECT_BLOCKS;
	if (logical >= CRYEXTS_INDIRECT_BLOCKS)
		return -EFBIG;

	if (!blocks->indirect_block) {
		if (!create) {
			*block = 0;
			return 0;
		}

		err = cryexts_alloc_block_goal(inode->i_sb,
					       blocks->alloc_hint_block,
					       blocks->alloc_goal_group,
					       &physical);
		if (err)
			return err;
		blocks->indirect_block = physical;

		bh = sb_getblk(inode->i_sb, physical);
		if (!bh) {
			cryexts_free_block(inode->i_sb, physical);
			blocks->indirect_block = 0;
			return -EIO;
		}
		lock_buffer(bh);
		memset(bh->b_data, 0, CRYEXTS_BLOCK_SIZE);
		set_buffer_uptodate(bh);
		mark_buffer_dirty(bh);
		unlock_buffer(bh);
		brelse(bh);
	}

	bh = sb_bread(inode->i_sb, blocks->indirect_block);
	if (!bh)
		return -EIO;

	entries = (__le64 *)bh->b_data;
	physical = le64_to_cpu(entries[logical]);
	if (!physical && create) {
		err = cryexts_alloc_data_block_for_inode(inode, blocks,
							 &physical);
		if (err) {
			brelse(bh);
			return err;
		}
		err = cryexts_zero_new_block(inode->i_sb, physical);
		if (err) {
			cryexts_free_block(inode->i_sb, physical);
			brelse(bh);
			return err;
		}
		entries[logical] = cpu_to_le64(physical);
		mark_buffer_dirty(bh);
		cryexts_update_alloc_hint(inode, physical);
	}
	brelse(bh);
	*block = physical;
	return 0;
}

int cryexts_free_blocks_from(struct inode *inode, u64 keep_blocks)
{
	struct cryexts_inode_info *blocks = cryexts_inode_blocks(inode);
	struct buffer_head *bh = NULL;
	__le64 *entries = NULL;
	unsigned int i;
	unsigned int total_entries;
	int err;
	bool indirect_dirty = false;
	bool indirect_empty = true;

	if (!blocks)
		return -EIO;

	if (blocks->use_extents) {
		if (cryexts_extent_tree_v2_inode(blocks)) {
			unsigned int leaf_index = 0;

			while (leaf_index < blocks->extent_leaf_count) {
				struct cryexts_extent_leaf_cache *leaf =
					&blocks->extent_leaves[leaf_index];
				unsigned int entry_index = 0;

				while (entry_index < leaf->entries) {
					struct cryexts_extent *extent =
						&leaf->extents[entry_index];
					u64 logical =
						le64_to_cpu(extent->logical_start);
					u64 physical =
						le64_to_cpu(extent->physical_start);
					u32 len = le32_to_cpu(extent->length);
					u64 end = logical + len;
					u32 new_len;
					u32 j;

					if (keep_blocks >= end) {
						entry_index++;
						continue;
					}

					if (keep_blocks <= logical) {
						for (j = 0; j < len; j++) {
							err = cryexts_free_block(
								inode->i_sb,
								physical + j);
							if (err)
								return err;
						}
						memmove(&leaf->extents[entry_index],
							&leaf->extents[entry_index + 1],
							(leaf->entries - entry_index - 1) *
								sizeof(leaf->extents[0]));
						leaf->entries--;
						cryexts_refresh_extent_tree_v2_refs(
							blocks);
						continue;
					}

					new_len = keep_blocks - logical;
					for (j = new_len; j < len; j++) {
						err = cryexts_free_block(
							inode->i_sb, physical + j);
						if (err)
							return err;
					}
					extent->length = cpu_to_le32(new_len);
					entry_index++;
				}

				if (!leaf->entries) {
					if (leaf->block) {
						err = cryexts_free_block(inode->i_sb,
									 leaf->block);
						if (err)
							return err;
					}
					cryexts_drop_extent_leaf(blocks, leaf_index);
					continue;
				}
				leaf_index++;
			}
			cryexts_refresh_extent_tree_v2_refs(blocks);
			return 0;
		}

		total_entries = cryexts_extent_total_entries(blocks);

		for (i = 0; i < total_entries; ) {
			struct cryexts_extent *extent =
				cryexts_extent_entry(blocks, i);
			u64 logical;
			u64 physical;
			u32 len;
			u64 end;
			u32 new_len;
			u32 j;

			if (!extent)
				return -EUCLEAN;
			logical = le64_to_cpu(extent->logical_start);
			physical = le64_to_cpu(extent->physical_start);
			len = le32_to_cpu(extent->length);
			end = logical + len;

			if (keep_blocks >= end) {
				i++;
				continue;
			}

			if (keep_blocks <= logical) {
				for (j = 0; j < len; j++) {
					err = cryexts_free_block(inode->i_sb, physical + j);
					if (err)
						return err;
				}
				if (i < blocks->extent_entries) {
					memmove(&blocks->extents[i], &blocks->extents[i + 1],
						(blocks->extent_entries - i - 1) *
							sizeof(blocks->extents[0]));
					blocks->extent_entries--;
					if (blocks->extent_overflow_entries) {
						blocks->extents[blocks->extent_entries] =
							blocks->overflow_extents[0];
						memmove(&blocks->overflow_extents[0],
							&blocks->overflow_extents[1],
							(blocks->extent_overflow_entries - 1) *
							 sizeof(blocks->overflow_extents[0]));
						blocks->extent_overflow_entries--;
						blocks->extent_entries++;
					}
				} else {
					unsigned int overflow_index =
						i - blocks->extent_entries;

					memmove(&blocks->overflow_extents[overflow_index],
						&blocks->overflow_extents[overflow_index + 1],
						(blocks->extent_overflow_entries -
						 overflow_index - 1) *
							sizeof(blocks->overflow_extents[0]));
					blocks->extent_overflow_entries--;
				}
				total_entries--;
				continue;
			}

			new_len = keep_blocks - logical;
			for (j = new_len; j < len; j++) {
				err = cryexts_free_block(inode->i_sb, physical + j);
				if (err)
					return err;
			}
			extent->length = cpu_to_le32(new_len);
			i++;
		}
		if (!blocks->extent_overflow_entries && blocks->extent_overflow_block) {
			err = cryexts_free_block(inode->i_sb, blocks->extent_overflow_block);
			if (err)
				return err;
			blocks->extent_overflow_block = 0;
			kfree(blocks->overflow_extents);
			blocks->overflow_extents = NULL;
		}
		return 0;
	}

	if (S_ISDIR(inode->i_mode) &&
	    (blocks->inode_flags & CRYEXTS_INODE_FLAG_DIR_INDEX) &&
	    blocks->dir_index_block && keep_blocks == 0) {
		err = cryexts_free_block(inode->i_sb, blocks->dir_index_block);
		if (err)
			return err;
		blocks->dir_index_block = 0;
	}

	for (i = keep_blocks; i < min_t(u64, CRYEXTS_DIRECT_BLOCKS,
					CRYEXTS_FILE_BLOCKS_MAX); i++) {
		if (!blocks->direct[i])
			continue;
		err = cryexts_free_block(inode->i_sb, blocks->direct[i]);
		if (err)
			return err;
		blocks->direct[i] = 0;
	}

	if (!blocks->indirect_block)
		return 0;

	bh = sb_bread(inode->i_sb, blocks->indirect_block);
	if (!bh)
		return -EIO;

	entries = (__le64 *)bh->b_data;
	for (i = 0; i < CRYEXTS_INDIRECT_BLOCKS; i++) {
		u64 logical = CRYEXTS_DIRECT_BLOCKS + i;
		u64 data_block = le64_to_cpu(entries[i]);

		if (!data_block)
			continue;
		if (logical >= keep_blocks) {
			err = cryexts_free_block(inode->i_sb, data_block);
			if (err) {
				brelse(bh);
				return err;
			}
			entries[i] = 0;
			indirect_dirty = true;
		} else {
			indirect_empty = false;
		}
	}

	if (indirect_dirty)
		mark_buffer_dirty(bh);
	if (!indirect_empty) {
		for (i = 0; i < CRYEXTS_INDIRECT_BLOCKS; i++) {
			if (le64_to_cpu(entries[i])) {
				indirect_empty = false;
				break;
			}
		}
	}
	brelse(bh);

	if (indirect_empty) {
		err = cryexts_free_block(inode->i_sb, blocks->indirect_block);
		if (err)
			return err;
		blocks->indirect_block = 0;
	}

	return 0;
}

int cryexts_punch_hole_blocks(struct inode *inode, u64 first_block,
			      u64 end_block)
{
	struct cryexts_inode_info *blocks = cryexts_inode_blocks(inode);
	unsigned int leaf_index;
	int err;

	if (!blocks || !blocks->use_extents)
		return -EOPNOTSUPP;
	if (!cryexts_extent_tree_v2_inode(blocks))
		return -EOPNOTSUPP;
	if (first_block >= end_block)
		return 0;

	leaf_index = 0;
	while (leaf_index < blocks->extent_leaf_count) {
		struct cryexts_extent_leaf_cache *leaf =
			&blocks->extent_leaves[leaf_index];
		unsigned int entry_index = 0;

		while (entry_index < leaf->entries) {
			struct cryexts_extent *extent = &leaf->extents[entry_index];
			u64 logical = le64_to_cpu(extent->logical_start);
			u64 physical = le64_to_cpu(extent->physical_start);
			u32 len = le32_to_cpu(extent->length);
			u64 extent_end = logical + len;
			u64 punch_start;
			u64 punch_end;
			u32 free_start;
			u32 free_len;
			u32 j;
			bool split_extent;

			if (end_block <= logical)
				break;
			if (first_block >= extent_end) {
				entry_index++;
				continue;
			}

			punch_start = max(first_block, logical);
			punch_end = min(end_block, extent_end);
			free_start = punch_start - logical;
			free_len = punch_end - punch_start;
			split_extent = punch_start > logical &&
				       punch_end < extent_end;
			if (split_extent) {
				if (leaf->entries >= CRYEXTS_EXTENTS_PER_BLOCK)
					return -ENOSPC;
				err = cryexts_insert_extent_after_v2(
					blocks, leaf_index, entry_index,
					punch_end,
					physical + free_start + free_len,
					extent_end - punch_end);
				if (err)
					return err;
				leaf = &blocks->extent_leaves[leaf_index];
				extent = &leaf->extents[entry_index];
				extent->length = cpu_to_le32(punch_start - logical);
			}
			for (j = 0; j < free_len; j++) {
				err = cryexts_free_block(inode->i_sb,
							 physical + free_start + j);
				if (err)
					return err;
			}
			if (split_extent) {
				entry_index += 2;
				continue;
			}

			if (punch_start == logical && punch_end == extent_end) {
				memmove(&leaf->extents[entry_index],
					&leaf->extents[entry_index + 1],
					(leaf->entries - entry_index - 1) *
						sizeof(leaf->extents[0]));
				leaf->entries--;
				continue;
			}

			if (punch_start == logical) {
				extent->logical_start = cpu_to_le64(punch_end);
				extent->physical_start =
					cpu_to_le64(physical + free_start + free_len);
				extent->length = cpu_to_le32(extent_end - punch_end);
				entry_index++;
				continue;
			}

			if (punch_end == extent_end) {
				extent->length = cpu_to_le32(punch_start - logical);
				entry_index++;
				continue;
			}

			entry_index++;
		}

		if (!leaf->entries) {
			if (leaf->block) {
				err = cryexts_free_block(inode->i_sb,
							 leaf->block);
				if (err)
					return err;
			}
			cryexts_drop_extent_leaf(blocks, leaf_index);
			continue;
		}
		leaf_index++;
	}
	cryexts_refresh_extent_tree_v2_refs(blocks);
	return 0;
}

bool cryexts_mode_supported(umode_t mode)
{
	return S_ISDIR(mode) || S_ISREG(mode) || S_ISLNK(mode);
}

int cryexts_validate_inode(struct super_block *sb,
			   struct cryexts_inode *disk_inode,
			   u64 ino)
{
	umode_t mode = le16_to_cpu(disk_inode->mode);
	u64 size = le64_to_cpu(disk_inode->size);
	u64 blocks = le64_to_cpu(disk_inode->blocks);
	unsigned int used_direct_blocks = 0;
	unsigned int used_data_blocks = 0;
	u64 indirect_block = cryexts_disk_inode_indirect_block(disk_inode);
	u32 inode_flags = le32_to_cpu(disk_inode->inode_flags);
	bool dir_index_inode = S_ISDIR(mode) &&
		(inode_flags & CRYEXTS_INODE_FLAG_DIR_INDEX);
	unsigned int i;
	struct buffer_head *ibh = NULL;
	__le64 *entries = NULL;
	u64 xattr_block =
		le64_to_cpu(cryexts_disk_inode_extra(disk_inode)->xattr_block);
	u64 next_orphan =
		le64_to_cpu(cryexts_disk_inode_extra(disk_inode)->next_orphan);
	u32 policy_id =
		le32_to_cpu(cryexts_disk_inode_extra(disk_inode)->encryption_policy_id);

	if (inode_flags & ~(CRYEXTS_INODE_FLAG_EXTENTS |
			    CRYEXTS_INODE_FLAG_EXTENT_TREE_V2 |
			    CRYEXTS_INODE_FLAG_DIR_INDEX |
			    CRYEXTS_INODE_FLAG_IMMUTABLE |
			    CRYEXTS_INODE_FLAG_APPEND_ONLY))
		return -EUCLEAN;
	if (xattr_block &&
	    (!cryexts_data_block_valid(sb, xattr_block) ||
	     !cryexts_block_bitmap_used(sb, xattr_block)))
		return -EUCLEAN;
	if (next_orphan) {
		if (!cryexts_orphan_feature_enabled(sb))
			return -EUCLEAN;
		if (next_orphan < CRYEXTS_ROOT_INO || next_orphan > cryexts_inodes_count(sb))
			return -EUCLEAN;
		if (next_orphan == ino)
			return -EUCLEAN;
	}
	if (!cryexts_policy_exists(sb, policy_id))
		return -EUCLEAN;

	if (ino > cryexts_inodes_count(sb))
		return -EUCLEAN;
	if (mode && !cryexts_inode_bitmap_used(sb, ino))
		return -EUCLEAN;
	if (!mode) {
		if (ino == CRYEXTS_ROOT_INO)
			return -EUCLEAN;
		if (cryexts_inode_bitmap_used(sb, ino))
			return -EUCLEAN;
		return -ENOENT;
	}
	if (!cryexts_mode_supported(mode))
		return -EUCLEAN;

	if (cryexts_disk_inode_uses_extents(disk_inode)) {
		struct cryexts_extent_header *eh;
		struct cryexts_extent *extents;
		u16 inline_max;
		u16 entries_count;
		u64 file_block_limit;
		u64 overflow_block = 0;
		u16 overflow_entries = 0;
		u32 overflow_checksum = 0;
		u64 next_logical = 0;
		u32 j;

		if (!S_ISREG(mode))
			return -EUCLEAN;
		eh = (struct cryexts_extent_header *)disk_inode->reserved;
		inline_max = le16_to_cpu(eh->max);
		entries_count = le16_to_cpu(eh->entries);
		if (le16_to_cpu(eh->magic) != CRYEXTS_EXTENT_MAGIC)
			return -EUCLEAN;
		if (cryexts_disk_extent_tree_v2_inode(inode_flags) &&
		    cryexts_extent_tree_v2_enabled(sb)) {
			const struct cryexts_extent_root_ref *refs;
			u32 k;

			if (inline_max != CRYEXTS_EXTENT_TREE_ROOT_REFS ||
			    entries_count > CRYEXTS_EXTENT_TREE_ROOT_REFS ||
			    le16_to_cpu(eh->reserved) !=
				    CRYEXTS_EXTENT_TREE_V2_DEPTH)
				return -EUCLEAN;
			refs = cryexts_disk_extent_root_refs_const(disk_inode);
			for (i = 0; i < entries_count; i++) {
				struct buffer_head *lbh;
				struct cryexts_extent_header *leh;
				struct cryexts_extent *lextents;
				u64 leaf_block = le64_to_cpu(refs[i].leaf_block);
				u16 leaf_entries = le16_to_cpu(refs[i].entries);
				u32 leaf_checksum = le32_to_cpu(refs[i].checksum);

				if (!leaf_block || !leaf_entries ||
				    leaf_entries > CRYEXTS_EXTENTS_PER_BLOCK)
					return -EUCLEAN;
				if (le64_to_cpu(refs[i].logical_start) <
				    next_logical)
					return -EUCLEAN;
				if (!cryexts_data_block_valid(sb, leaf_block) ||
				    !cryexts_block_bitmap_used(sb, leaf_block))
					return -EUCLEAN;
				lbh = sb_bread(sb, leaf_block);
				if (!lbh)
					return -EIO;
				leh = (struct cryexts_extent_header *)lbh->b_data;
				if (le16_to_cpu(leh->magic) != CRYEXTS_EXTENT_MAGIC ||
				    le16_to_cpu(leh->max) != CRYEXTS_EXTENTS_PER_BLOCK ||
				    le16_to_cpu(leh->entries) != leaf_entries) {
					brelse(lbh);
					return -EUCLEAN;
				}
				if (cryexts_metadata_csum_enabled(sb) &&
				    leaf_checksum != cryexts_extent_leaf_checksum(
							    sb, leaf_block,
							    lbh->b_data)) {
					brelse(lbh);
					return -EUCLEAN;
				}
				lextents = (struct cryexts_extent *)(lbh->b_data +
								 sizeof(*leh));
				for (j = 0; j < leaf_entries; j++) {
					u64 logical =
						le64_to_cpu(lextents[j].logical_start);
					u64 physical =
						le64_to_cpu(lextents[j].physical_start);
					u32 len = le32_to_cpu(lextents[j].length);

					if (!len || len > CRYEXTS_MAX_EXTENT_BLOCKS) {
						brelse(lbh);
						return -EUCLEAN;
					}
					if (j == 0 &&
					    logical != le64_to_cpu(
						    refs[i].logical_start)) {
						brelse(lbh);
						return -EUCLEAN;
					}
					if (logical < next_logical ||
					    logical + len < logical) {
						brelse(lbh);
						return -EUCLEAN;
					}
					if (!cryexts_data_block_valid(sb, physical) ||
					    !cryexts_block_bitmap_used(sb, physical)) {
						brelse(lbh);
						return -EUCLEAN;
					}
					for (k = 0; k < len; k++) {
						if (!cryexts_data_block_valid(
							    sb, physical + k) ||
						    !cryexts_block_bitmap_used(
							    sb, physical + k)) {
							brelse(lbh);
							return -EUCLEAN;
						}
					}
					used_data_blocks += len;
					next_logical = logical + len;
				}
				brelse(lbh);
			}
			goto extent_common_checks;
		} else if (inline_max == CRYEXTS_MAX_INLINE_EXTENTS) {
			if (entries_count > CRYEXTS_MAX_INLINE_EXTENTS)
				return -EUCLEAN;
		} else if (inline_max == CRYEXTS_EXTENT_ROOT_INLINE_EXTENTS &&
			   cryexts_extent_tree_enabled(sb)) {
			struct buffer_head *obh = NULL;
			struct cryexts_extent_header *oeh;
			struct cryexts_extent *oextents;

			if (entries_count > CRYEXTS_EXTENT_ROOT_INLINE_EXTENTS)
				return -EUCLEAN;
			overflow_block =
				le64_to_cpu(*(__le64 *)(disk_inode->reserved +
						CRYEXTS_EXTENT_ROOT_OVERFLOW_OFFSET));
			overflow_entries =
				le16_to_cpu(*(__le16 *)(disk_inode->reserved +
						CRYEXTS_EXTENT_ROOT_OVERFLOW_OFFSET +
						sizeof(__le64)));
			overflow_checksum =
				cryexts_disk_extent_overflow_checksum(disk_inode);
			if (!!overflow_block != !!overflow_entries)
				return -EUCLEAN;
			if (overflow_entries > CRYEXTS_EXTENTS_PER_BLOCK)
				return -EUCLEAN;
			if (!overflow_block && overflow_checksum &&
			    cryexts_metadata_csum_enabled(sb))
				return -EUCLEAN;
			if (overflow_block) {
				if (!cryexts_data_block_valid(sb, overflow_block) ||
				    !cryexts_block_bitmap_used(sb, overflow_block))
					return -EUCLEAN;
				obh = sb_bread(sb, overflow_block);
				if (!obh)
					return -EIO;
				oeh = (struct cryexts_extent_header *)obh->b_data;
				if (le16_to_cpu(oeh->magic) != CRYEXTS_EXTENT_MAGIC ||
				    le16_to_cpu(oeh->max) != CRYEXTS_EXTENTS_PER_BLOCK ||
				    le16_to_cpu(oeh->entries) != overflow_entries) {
					brelse(obh);
					return -EUCLEAN;
				}
				if (cryexts_metadata_csum_enabled(sb) &&
				    overflow_checksum !=
					    cryexts_extent_overflow_checksum(
						    sb, overflow_block,
						    obh->b_data)) {
					brelse(obh);
					return -EUCLEAN;
				}
				oextents = (struct cryexts_extent *)(obh->b_data +
							 sizeof(*oeh));
				extents = (struct cryexts_extent *)(disk_inode->reserved +
									 sizeof(*eh));
				for (i = 0; i < entries_count; i++) {
					u64 logical = le64_to_cpu(extents[i].logical_start);
					u64 physical = le64_to_cpu(extents[i].physical_start);
					u32 len = le32_to_cpu(extents[i].length);

					if (!len || len > CRYEXTS_MAX_EXTENT_BLOCKS) {
						brelse(obh);
						return -EUCLEAN;
					}
					if (logical < next_logical ||
					    logical + len < logical) {
						brelse(obh);
						return -EUCLEAN;
					}
					if (!cryexts_data_block_valid(sb, physical) ||
					    !cryexts_block_bitmap_used(sb, physical)) {
						brelse(obh);
						return -EUCLEAN;
					}
					for (j = 0; j < len; j++) {
						if (!cryexts_data_block_valid(sb, physical + j) ||
						    !cryexts_block_bitmap_used(sb, physical + j)) {
							brelse(obh);
							return -EUCLEAN;
						}
					}
					used_data_blocks += len;
					next_logical = logical + len;
				}
				for (i = 0; i < overflow_entries; i++) {
					u64 logical = le64_to_cpu(oextents[i].logical_start);
					u64 physical = le64_to_cpu(oextents[i].physical_start);
					u32 len = le32_to_cpu(oextents[i].length);

					if (!len || len > CRYEXTS_MAX_EXTENT_BLOCKS) {
						brelse(obh);
						return -EUCLEAN;
					}
					if (logical < next_logical ||
					    logical + len < logical) {
						brelse(obh);
						return -EUCLEAN;
					}
					if (!cryexts_data_block_valid(sb, physical) ||
					    !cryexts_block_bitmap_used(sb, physical)) {
						brelse(obh);
						return -EUCLEAN;
					}
					for (j = 0; j < len; j++) {
						if (!cryexts_data_block_valid(sb, physical + j) ||
						    !cryexts_block_bitmap_used(sb, physical + j)) {
							brelse(obh);
							return -EUCLEAN;
						}
					}
					used_data_blocks += len;
					next_logical = logical + len;
				}
				brelse(obh);
				goto extent_common_checks;
			}
		} else {
			return -EUCLEAN;
		}
		extents = (struct cryexts_extent *)(disk_inode->reserved +
						    sizeof(*eh));
		for (i = 0; i < entries_count; i++) {
			u64 logical = le64_to_cpu(extents[i].logical_start);
			u64 physical = le64_to_cpu(extents[i].physical_start);
			u32 len = le32_to_cpu(extents[i].length);

			if (!len || len > CRYEXTS_MAX_EXTENT_BLOCKS)
				return -EUCLEAN;
			if (logical < next_logical || logical + len < logical)
				return -EUCLEAN;
			if (!cryexts_data_block_valid(sb, physical) ||
			    !cryexts_block_bitmap_used(sb, physical))
				return -EUCLEAN;
			for (j = 0; j < len; j++) {
				if (!cryexts_data_block_valid(sb, physical + j) ||
				    !cryexts_block_bitmap_used(sb, physical + j))
					return -EUCLEAN;
			}
			used_data_blocks += len;
			next_logical = logical + len;
		}
extent_common_checks:
		if (indirect_block)
			return -EUCLEAN;
		for (i = 0; i < CRYEXTS_DIRECT_BLOCKS; i++) {
			if (le64_to_cpu(disk_inode->block[i]))
				return -EUCLEAN;
		}
		if (next_logical > DIV_ROUND_UP_ULL(size, CRYEXTS_BLOCK_SIZE))
			return -EUCLEAN;
		if (size == 0 &&
		    (used_data_blocks ||
		     overflow_block ||
		     (cryexts_disk_extent_tree_v2_inode(inode_flags) &&
		      entries_count)))
			return -EUCLEAN;
		file_block_limit = cryexts_extent_file_blocks_max_from_inode_flags(
			inode_flags, inline_max);
		if (size > file_block_limit * CRYEXTS_BLOCK_SIZE)
			return -EUCLEAN;
		if (blocks != (u64)(used_data_blocks + (overflow_block ? 1 : 0) +
				    (cryexts_disk_extent_tree_v2_inode(inode_flags) ?
					     entries_count :
					     0)) *
				      (CRYEXTS_BLOCK_SIZE / 512))
			return -EUCLEAN;
		return 0;
	}

	for (i = 0; i < CRYEXTS_DIRECT_BLOCKS; i++) {
		u64 data_block = le64_to_cpu(disk_inode->block[i]);

		if (!data_block)
			continue;
		used_direct_blocks++;
		used_data_blocks++;
		if (!cryexts_data_block_valid(sb, data_block) ||
		    !cryexts_block_bitmap_used(sb, data_block))
			return -EUCLEAN;
	}

	if (indirect_block && !dir_index_inode) {
		if (!cryexts_data_block_valid(sb, indirect_block) ||
		    !cryexts_block_bitmap_used(sb, indirect_block))
			return -EUCLEAN;
		ibh = sb_bread(sb, indirect_block);
		if (!ibh)
			return -EIO;
		entries = (__le64 *)ibh->b_data;
		for (i = 0; i < CRYEXTS_INDIRECT_BLOCKS; i++) {
			u64 data_block = le64_to_cpu(entries[i]);

			if (!data_block)
				continue;
			used_data_blocks++;
			if (!cryexts_data_block_valid(sb, data_block) ||
			    !cryexts_block_bitmap_used(sb, data_block)) {
				brelse(ibh);
				return -EUCLEAN;
			}
		}
		brelse(ibh);
	}

	if (S_ISDIR(mode)) {
		unsigned int dir_blocks;
		u64 dir_index_block = 0;

		if (!size || size % CRYEXTS_BLOCK_SIZE)
			return -EUCLEAN;
		dir_blocks = size / CRYEXTS_BLOCK_SIZE;
		if (!dir_blocks || dir_blocks > CRYEXTS_DIRECT_BLOCKS)
			return -EUCLEAN;
		if (inode_flags & CRYEXTS_INODE_FLAG_DIR_INDEX) {
			dir_index_block = indirect_block;
			if (!dir_index_block ||
			    !cryexts_data_block_valid(sb, dir_index_block) ||
			    !cryexts_block_bitmap_used(sb, dir_index_block))
				return -EUCLEAN;
		} else if (indirect_block) {
			return -EUCLEAN;
		}
		if (used_direct_blocks != dir_blocks ||
		    blocks != (dir_blocks + (dir_index_block ? 1 : 0)) *
				      (CRYEXTS_BLOCK_SIZE / 512) ||
		    le64_to_cpu(disk_inode->block[0]) == 0)
			return -EUCLEAN;
		for (i = dir_blocks; i < CRYEXTS_DIRECT_BLOCKS; i++) {
			if (le64_to_cpu(disk_inode->block[i]))
				return -EUCLEAN;
		}
	}

	if (S_ISREG(mode)) {
		if (size > cryexts_regular_file_max_size())
			return -EUCLEAN;
		if (size == 0 && (used_data_blocks || indirect_block))
			return -EUCLEAN;
		if (blocks != cryexts_disk_inode_block_count(sb, disk_inode) *
			      (CRYEXTS_BLOCK_SIZE / 512))
			return -EUCLEAN;
	}

	if (S_ISLNK(mode)) {
		if (size == 0 || size > cryexts_symlink_size_limit())
			return -EUCLEAN;
		if (blocks != cryexts_disk_inode_block_count(sb, disk_inode) *
			      (CRYEXTS_BLOCK_SIZE / 512))
			return -EUCLEAN;
	}

	if (blocks > (u64)(max_t(u64, CRYEXTS_FILE_BLOCKS_MAX,
				 CRYEXTS_EXTENT_TREE_V2_FILE_BLOCKS_MAX) + 1) *
			     (CRYEXTS_BLOCK_SIZE / 512))
		return -EUCLEAN;
	return 0;
}

struct cryexts_inode *cryexts_get_disk_inode(struct super_block *sb,
					     u64 ino,
					     struct buffer_head **bhp)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	unsigned int ipb = cryexts_inodes_per_block();
	u64 index;
	u64 group;
	u64 index_in_group;
	u64 block;
	unsigned int offset;
	struct buffer_head *bh;

	if (ino < CRYEXTS_ROOT_INO || ino > cryexts_max_inodes(sb))
		return NULL;

	index = ino - 1;
	if (le32_to_cpu(sbi->disk_sb->features_incompat) &
	    CRYEXTS_FEATURE_INCOMPAT_BLOCK_GROUPS) {
		group = div_u64(index, sbi->inodes_per_group);
		if (group >= sbi->group_count)
			return NULL;
		index_in_group = index % sbi->inodes_per_group;
		block = cryexts_group_inode_table_start(sb, group) +
			index_in_group / ipb;
		offset = (index_in_group % ipb) * sizeof(struct cryexts_inode);
	} else {
		block = sbi->inode_table_start + index / ipb;
		offset = (index % ipb) * sizeof(struct cryexts_inode);
	}
	bh = sb_bread(sb, block);
	if (!bh)
		return NULL;

	*bhp = bh;
	return (struct cryexts_inode *)(bh->b_data + offset);
}

int cryexts_write_inode_to_disk(struct inode *inode)
{
	struct buffer_head *bh;
	struct cryexts_inode *disk_inode;
	struct timespec64 ts;
	struct cryexts_inode_info *blocks = cryexts_inode_blocks(inode);
	struct cryexts_extent_header *eh;
	u16 inline_entries;
	int err;
	unsigned int i;

	disk_inode = cryexts_get_disk_inode(inode->i_sb, inode->i_ino, &bh);
	if (!disk_inode)
		return -EIO;

	if (blocks)
		inode->i_blocks = cryexts_inode_block_sectors(inode);

	disk_inode->mode = cpu_to_le16(inode->i_mode);
	disk_inode->links_count = cpu_to_le16(inode->i_nlink);
	disk_inode->uid = cpu_to_le32(i_uid_read(inode));
	disk_inode->gid = cpu_to_le32(i_gid_read(inode));
	disk_inode->size = cpu_to_le64(i_size_read(inode));
	disk_inode->blocks = cpu_to_le64(inode->i_blocks);
	ts = inode->i_atime;
	disk_inode->atime = cpu_to_le64(ts.tv_sec);
	ts = inode->i_ctime;
	disk_inode->ctime = cpu_to_le64(ts.tv_sec);
	ts = inode->i_mtime;
	disk_inode->mtime = cpu_to_le64(ts.tv_sec);
	memset(disk_inode->reserved, 0, sizeof(disk_inode->reserved));
	if (blocks && blocks->use_extents) {
		for (i = 0; i < CRYEXTS_DIRECT_BLOCKS; i++)
			disk_inode->block[i] = cpu_to_le64(0);
		disk_inode->indirect_block = cpu_to_le64(0);
		disk_inode->inode_flags = cpu_to_le32(blocks->inode_flags);
		eh = (struct cryexts_extent_header *)disk_inode->reserved;
		eh->magic = cpu_to_le16(CRYEXTS_EXTENT_MAGIC);
		if (cryexts_extent_tree_v2_inode(blocks)) {
			struct cryexts_extent_root_ref *refs;

			cryexts_refresh_extent_tree_v2_refs(blocks);
			for (i = 0; i < blocks->extent_leaf_count; i++) {
				err = cryexts_write_extent_leaf(inode, blocks, i);
				if (err) {
					brelse(bh);
					return err;
				}
			}
			inline_entries = blocks->extent_leaf_count;
			eh->entries = cpu_to_le16(inline_entries);
			eh->max = cpu_to_le16(CRYEXTS_EXTENT_TREE_ROOT_REFS);
			eh->reserved = cpu_to_le16(CRYEXTS_EXTENT_TREE_V2_DEPTH);
			refs = cryexts_disk_extent_root_refs(disk_inode);
			memcpy(refs, blocks->extent_root_refs,
			       sizeof(blocks->extent_root_refs));
		} else {
			err = cryexts_write_extent_overflow(inode, blocks);
			if (err) {
				brelse(bh);
				return err;
			}
			inline_entries = blocks->extent_entries;
			eh->entries = cpu_to_le16(inline_entries);
			eh->max = cpu_to_le16(blocks->extent_inline_max);
			eh->reserved = cpu_to_le16(0);
			memcpy(disk_inode->reserved + sizeof(*eh), blocks->extents,
			       inline_entries * sizeof(blocks->extents[0]));
		}
		if (!cryexts_extent_tree_v2_inode(blocks) &&
		    blocks->extent_inline_max == CRYEXTS_EXTENT_ROOT_INLINE_EXTENTS) {
			*(__le64 *)(disk_inode->reserved +
				    CRYEXTS_EXTENT_ROOT_OVERFLOW_OFFSET) =
				cpu_to_le64(blocks->extent_overflow_block);
			*(__le16 *)(disk_inode->reserved +
				    CRYEXTS_EXTENT_ROOT_OVERFLOW_OFFSET +
				    sizeof(__le64)) =
				cpu_to_le16(blocks->extent_overflow_entries);
			cryexts_set_disk_extent_overflow_checksum(
				disk_inode, blocks->extent_overflow_checksum);
		}
	} else {
		for (i = 0; i < CRYEXTS_DIRECT_BLOCKS; i++)
			disk_inode->block[i] = cpu_to_le64(blocks ? blocks->direct[i] : 0);
		disk_inode->indirect_block =
			cpu_to_le64((blocks && S_ISDIR(inode->i_mode) &&
				     (blocks->inode_flags & CRYEXTS_INODE_FLAG_DIR_INDEX)) ?
					    blocks->dir_index_block :
					    (blocks ? blocks->indirect_block : 0));
		disk_inode->inode_flags = cpu_to_le32(blocks ? blocks->inode_flags : 0);
	}
	cryexts_disk_inode_extra(disk_inode)->xattr_block =
		cpu_to_le64(blocks ? blocks->xattr_block : 0);
	cryexts_disk_inode_extra(disk_inode)->encryption_policy_id =
		cpu_to_le32(blocks ? blocks->encryption_policy_id : 0);
	cryexts_disk_inode_extra(disk_inode)->next_orphan =
		cpu_to_le64(blocks ? blocks->next_orphan : 0);

	cryexts_journal_record_bh(inode->i_sb, bh);
	mark_buffer_dirty(bh);
	brelse(bh);
	return 0;
}

static int cryexts_init_vfs_inode(struct inode *inode,
				  struct cryexts_inode *disk_inode)
{
	int err;

	inode->i_mode = le16_to_cpu(disk_inode->mode);
	i_uid_write(inode, le32_to_cpu(disk_inode->uid));
	i_gid_write(inode, le32_to_cpu(disk_inode->gid));
	set_nlink(inode, le16_to_cpu(disk_inode->links_count));
	i_size_write(inode, le64_to_cpu(disk_inode->size));
	inode->i_blocks = le64_to_cpu(disk_inode->blocks);
	inode->i_atime.tv_sec = le64_to_cpu(disk_inode->atime);
	inode->i_atime.tv_nsec = 0;
	inode->i_ctime.tv_sec = le64_to_cpu(disk_inode->ctime);
	inode->i_ctime.tv_nsec = 0;
	inode->i_mtime.tv_sec = le64_to_cpu(disk_inode->mtime);
	inode->i_mtime.tv_nsec = 0;
	err = cryexts_init_inode_blocks(inode, disk_inode);
	if (err)
		return err;

	if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &cryexts_dir_inode_operations;
		inode->i_fop = &cryexts_dir_operations;
	} else if (S_ISREG(inode->i_mode)) {
		inode->i_op = &cryexts_file_inode_operations;
		inode->i_fop = &cryexts_file_operations;
		inode->i_mapping->a_ops = &cryexts_file_aops;
	} else if (S_ISLNK(inode->i_mode)) {
		inode->i_op = &cryexts_symlink_inode_operations;
		inode_nohighmem(inode);
	}
	return 0;
}

struct inode *cryexts_iget(struct super_block *sb, u64 ino)
{
	struct inode *inode;
	struct buffer_head *bh;
	struct cryexts_inode *disk_inode;
	int err;

	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW))
		return inode;

	disk_inode = cryexts_get_disk_inode(sb, ino, &bh);
	if (!disk_inode) {
		iget_failed(inode);
		return ERR_PTR(-ENOENT);
	}
	err = cryexts_validate_inode(sb, disk_inode, ino);
	if (err) {
		iget_failed(inode);
		brelse(bh);
		return ERR_PTR(err);
	}

	err = cryexts_init_vfs_inode(inode, disk_inode);
	if (err) {
		iget_failed(inode);
		brelse(bh);
		return ERR_PTR(err);
	}
	brelse(bh);
	unlock_new_inode(inode);
	return inode;
}

struct inode *cryexts_new_inode(struct inode *dir, umode_t mode,
				u64 data_block)
{
	struct super_block *sb = dir->i_sb;
	struct inode *inode;
	struct timespec64 now;
	struct cryexts_inode_info *info;
	u64 ino;
	u64 goal_group = cryexts_inode_group_of(dir);
	int err;

	err = cryexts_alloc_inode_goal(sb, goal_group, &ino);
	if (err)
		return ERR_PTR(err);

	inode = new_inode(sb);
	if (!inode) {
		err = -ENOMEM;
		goto fail_free_ino;
	}

	inode->i_ino = ino;
	inode->i_mode = mode;
	inode->i_uid = current_fsuid();
	inode->i_gid = current_fsgid();
	set_nlink(inode, S_ISDIR(mode) ? 2 : 1);
	i_size_write(inode, S_ISDIR(mode) ? CRYEXTS_BLOCK_SIZE : 0);
	inode->i_blocks = data_block ? CRYEXTS_BLOCK_SIZE / 512 : 0;
	now = current_time(inode);
	inode->i_atime = now;
	inode->i_ctime = now;
	inode->i_mtime = now;
	inode->i_private = kzalloc(sizeof(struct cryexts_inode_info),
				   GFP_KERNEL);
	if (!inode->i_private) {
		err = -ENOMEM;
		iput(inode);
		goto fail_free_ino;
	}
	info = cryexts_inode_blocks(inode);
	info->alloc_goal_group = U64_MAX;
	if (S_ISREG(mode) &&
	    le32_to_cpu(CRYEXTS_SB(sb)->disk_sb->version) >= CRYEXTS_VERSION_V4 &&
	    (le32_to_cpu(CRYEXTS_SB(sb)->disk_sb->features_incompat) &
	     CRYEXTS_FEATURE_INCOMPAT_EXTENTS)) {
		info->use_extents = true;
		info->inode_flags = CRYEXTS_INODE_FLAG_EXTENTS;
		if (cryexts_extent_tree_v2_enabled(sb) &&
		    cryexts_extent_tree_enabled(sb)) {
			info->inode_flags |= CRYEXTS_INODE_FLAG_EXTENT_TREE_V2;
			info->extent_inline_max = CRYEXTS_EXTENT_TREE_ROOT_REFS;
		} else {
			info->extent_inline_max =
				(le32_to_cpu(CRYEXTS_SB(sb)->disk_sb->version) >=
				 CRYEXTS_VERSION_V5 &&
				 cryexts_extent_tree_enabled(sb)) ?
					CRYEXTS_EXTENT_ROOT_INLINE_EXTENTS :
					CRYEXTS_MAX_INLINE_EXTENTS;
		}
	}
	if (cryexts_inode_blocks(dir) && cryexts_inode_policy_id(dir))
		cryexts_inode_blocks(inode)->encryption_policy_id =
			cryexts_inode_policy_id(dir);
	else
		cryexts_inode_blocks(inode)->encryption_policy_id =
			le32_to_cpu(CRYEXTS_SB(sb)->disk_sb->default_encryption_policy);
	if (cryexts_inode_blocks(dir)) {
		if (goal_group != U64_MAX)
			info->alloc_goal_group = goal_group;
		else if (cryexts_inode_blocks(dir)->alloc_goal_group != U64_MAX)
			info->alloc_goal_group =
				cryexts_inode_blocks(dir)->alloc_goal_group;
		else if (cryexts_inode_first_block(dir))
			info->alloc_goal_group =
				cryexts_block_group_of(sb,
						       cryexts_inode_first_block(dir));
		info->alloc_hint_block = cryexts_inode_blocks(dir)->alloc_hint_block;
	}
	if (data_block) {
		if (cryexts_inode_uses_extents(inode)) {
			if (cryexts_extent_tree_v2_inode(info)) {
				int append_err;

				append_err = cryexts_append_extent_entry(
					inode, info, 0, data_block);
				if (append_err) {
					iput(inode);
					cryexts_free_inode(sb, ino);
					return ERR_PTR(append_err);
				}
			} else {
				info->extents[0].logical_start = cpu_to_le64(0);
				info->extents[0].physical_start = cpu_to_le64(data_block);
				info->extents[0].length = cpu_to_le32(1);
				info->extents[0].flags = cpu_to_le32(0);
				info->extent_entries = 1;
			}
		} else {
			cryexts_inode_blocks(inode)->direct[0] = data_block;
		}
		cryexts_update_alloc_hint(inode, data_block);
	}

	if (S_ISDIR(mode)) {
		inode->i_op = &cryexts_dir_inode_operations;
		inode->i_fop = &cryexts_dir_operations;
	} else if (S_ISREG(mode)) {
		inode->i_op = &cryexts_file_inode_operations;
		inode->i_fop = &cryexts_file_operations;
		inode->i_mapping->a_ops = &cryexts_file_aops;
	} else if (S_ISLNK(mode)) {
		inode->i_op = &cryexts_symlink_inode_operations;
		inode_nohighmem(inode);
	}

	err = insert_inode_locked(inode);
	if (err) {
		clear_nlink(inode);
		discard_new_inode(inode);
		goto fail_free_ino;
	}

	err = cryexts_write_inode_to_disk(inode);
	if (err) {
		clear_nlink(inode);
		discard_new_inode(inode);
		goto fail_free_ino;
	}

	return inode;

fail_free_ino:
	cryexts_free_inode(sb, ino);
	return ERR_PTR(err ? err : -ENOMEM);
}

int cryexts_release_inode_storage(struct inode *inode)
{
	int err;

	if (!cryexts_inode_blocks(inode))
		return 0;

	truncate_inode_pages(inode->i_mapping, 0);
	err = cryexts_free_blocks_from(inode, 0);
	if (err)
		return err;
	err = cryexts_free_xattr_storage(inode);
	if (err)
		return err;
	i_size_write(inode, 0);
	inode->i_blocks = 0;
	return cryexts_write_inode_to_disk(inode);
}

void cryexts_evict_inode(struct inode *inode)
{
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
	cryexts_free_inode_blocks(inode);
}
