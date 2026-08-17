// SPDX-License-Identifier: GPL-2.0
#include "cryexts.h"

static void cryexts_copy_gdt_to_blocks(struct cryexts_sb_info *sbi)
{
	u64 index;

	if (!sbi->gdt_bhs || !sbi->gdt_storage)
		return;

	for (index = 0; index < sbi->group_desc_table_blocks; index++) {
		if (!sbi->gdt_bhs[index])
			continue;
		memcpy(sbi->gdt_bhs[index]->b_data,
		       sbi->gdt_storage + index * CRYEXTS_BLOCK_SIZE,
		       CRYEXTS_BLOCK_SIZE);
	}
}

int cryexts_load_group_desc_table(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	u64 bytes;
	u64 index;

	if (le32_to_cpu(sbi->disk_sb->version) < CRYEXTS_VERSION_V4)
		return 0;
	if (!sbi->group_desc_table_blocks)
		return -EINVAL;

	sbi->gdt_bhs = kcalloc(sbi->group_desc_table_blocks,
			       sizeof(*sbi->gdt_bhs), GFP_KERNEL);
	if (!sbi->gdt_bhs)
		return -ENOMEM;

	bytes = sbi->group_desc_table_blocks * CRYEXTS_BLOCK_SIZE;
	sbi->gdt_storage = kzalloc(bytes, GFP_KERNEL);
	if (!sbi->gdt_storage) {
		cryexts_release_group_desc_table(sbi);
		return -ENOMEM;
	}

	for (index = 0; index < sbi->group_desc_table_blocks; index++) {
		sbi->gdt_bhs[index] =
			sb_bread(sb, sbi->group_desc_table_start + index);
		if (!sbi->gdt_bhs[index]) {
			pr_err("cryexts: failed to read GDT at block %llu\n",
			       sbi->group_desc_table_start + index);
			cryexts_release_group_desc_table(sbi);
			return -EIO;
		}
		memcpy(sbi->gdt_storage + index * CRYEXTS_BLOCK_SIZE,
		       sbi->gdt_bhs[index]->b_data, CRYEXTS_BLOCK_SIZE);
	}

	sbi->groups = (struct cryexts_group_desc *)sbi->gdt_storage;
	return 0;
}

void cryexts_release_group_desc_table(struct cryexts_sb_info *sbi)
{
	u64 index;

	if (!sbi)
		return;

	if (sbi->gdt_bhs) {
		for (index = 0; index < sbi->group_desc_table_blocks; index++) {
			if (sbi->gdt_bhs[index])
				brelse(sbi->gdt_bhs[index]);
		}
		kfree(sbi->gdt_bhs);
		sbi->gdt_bhs = NULL;
	}

	kfree(sbi->gdt_storage);
	sbi->gdt_storage = NULL;
	sbi->groups = NULL;
}

void cryexts_gdt_prepare_write(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);

	if (!sbi || !sbi->gdt_storage)
		return;

	if (cryexts_metadata_csum_enabled(sb))
		cryexts_update_group_checksums(sb);
	cryexts_copy_gdt_to_blocks(sbi);
}

static int cryexts_validate_super(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	struct cryexts_super_block *disk_sb = sbi->disk_sb;
	u64 blocks_count = le64_to_cpu(disk_sb->blocks_count);
	u64 inodes_count = le64_to_cpu(disk_sb->inodes_count);
	u64 root_dir_block = le64_to_cpu(disk_sb->root_dir_block);
	u64 first_data_block = le64_to_cpu(disk_sb->first_data_block);
	u64 journal_block = le64_to_cpu(disk_sb->journal_block);
	u64 journal_blocks = le64_to_cpu(disk_sb->journal_blocks);
	u32 flags = le32_to_cpu(disk_sb->flags);
	u32 compat = le32_to_cpu(disk_sb->features_compat);
	u32 incompat = le32_to_cpu(disk_sb->features_incompat);
	u32 version = le32_to_cpu(disk_sb->version);
	u32 state = le32_to_cpu(disk_sb->state);
	u64 inode_table_end;

	if (le32_to_cpu(disk_sb->magic) != CRYEXTS_MAGIC)
		return -EINVAL;
	if (le32_to_cpu(disk_sb->version) != CRYEXTS_VERSION_V3 &&
	    le32_to_cpu(disk_sb->version) != CRYEXTS_VERSION_V4 &&
	    le32_to_cpu(disk_sb->version) != CRYEXTS_VERSION_V5 &&
	    le32_to_cpu(disk_sb->version) != CRYEXTS_VERSION_V6)
		return -EINVAL;
	if (le32_to_cpu(disk_sb->block_size) != CRYEXTS_BLOCK_SIZE)
		return -EINVAL;
	if (le32_to_cpu(disk_sb->inode_size) != sizeof(struct cryexts_inode))
		return -EINVAL;
	if (blocks_count < CRYEXTS_FIRST_FREE_DATA_BLOCK)
		return -EINVAL;
	if (!(le32_to_cpu(disk_sb->features_incompat) &
	      CRYEXTS_FEATURE_INCOMPAT_BLOCK_GROUPS) &&
	    blocks_count > CRYEXTS_BLOCK_SIZE * 8ULL)
		return -EINVAL;
	if (inodes_count != cryexts_max_inodes(sb))
		return -EINVAL;
	if (!(le32_to_cpu(disk_sb->features_incompat) &
	      CRYEXTS_FEATURE_INCOMPAT_BLOCK_GROUPS) &&
	    inodes_count > CRYEXTS_BLOCK_SIZE * 8ULL)
		return -EINVAL;
	if (le64_to_cpu(disk_sb->free_blocks_count) > blocks_count)
		return -EINVAL;
	if (le64_to_cpu(disk_sb->free_inodes_count) > inodes_count)
		return -EINVAL;
	if (!sbi->inode_table_start || !sbi->inode_table_blocks)
		return -EINVAL;
	if (sbi->block_bitmap_block >= blocks_count ||
	    sbi->inode_bitmap_block >= blocks_count ||
	    sbi->block_bitmap_block == sbi->inode_bitmap_block)
		return -EINVAL;
	if (!(le32_to_cpu(disk_sb->features_incompat) &
	      CRYEXTS_FEATURE_INCOMPAT_BLOCK_GROUPS) &&
	    (sbi->block_bitmap_block != CRYEXTS_BLOCK_BITMAP_BLOCK ||
	     sbi->inode_bitmap_block != CRYEXTS_INODE_BITMAP_BLOCK))
		return -EINVAL;

	inode_table_end = sbi->inode_table_start + sbi->inode_table_blocks;
	if (inode_table_end <= sbi->inode_table_start || inode_table_end > blocks_count)
		return -EINVAL;
	if (le64_to_cpu(disk_sb->root_inode_block) < sbi->inode_table_start ||
	    le64_to_cpu(disk_sb->root_inode_block) >= inode_table_end)
		return -EINVAL;
	if (root_dir_block != first_data_block)
		return -EINVAL;
	if (first_data_block != inode_table_end)
		return -EINVAL;
	if (root_dir_block >= blocks_count)
		return -EINVAL;
	if (sbi->next_ino < CRYEXTS_ROOT_INO + 1 ||
	    sbi->next_ino > cryexts_max_inodes(sb) + 1)
		return -EINVAL;
	if (sbi->next_data_block < root_dir_block + 1 ||
	    sbi->next_data_block > blocks_count)
		return -EINVAL;
	if (flags & ~CRYEXTS_SB_FLAG_ENCRYPTED)
		return -EINVAL;
	if (compat & ~(CRYEXTS_FEATURE_COMPAT_HAS_JOURNAL |
		       CRYEXTS_FEATURE_COMPAT_PREALLOC))
		return -EINVAL;
	if (incompat &
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
	      CRYEXTS_FEATURE_INCOMPAT_JOURNAL_V2 |
	      CRYEXTS_FEATURE_INCOMPAT_JOURNAL_V3 |
	      CRYEXTS_FEATURE_INCOMPAT_JOURNAL_RING) ||
	    (le32_to_cpu(disk_sb->features_ro_compat) &
	     ~(CRYEXTS_FEATURE_RO_COMPAT_METADATA_CSUM |
	       CRYEXTS_FEATURE_RO_COMPAT_LARGE_XATTR)))
		return -EINVAL;
	if (state & ~(CRYEXTS_FS_STATE_CLEAN |
		      CRYEXTS_FS_STATE_NEEDS_RECOVERY |
		      CRYEXTS_FS_STATE_ERRORS))
		return -EINVAL;
	if (version >= CRYEXTS_VERSION_V4) {
		if (!le64_to_cpu(disk_sb->group_count))
			return -EINVAL;
		if (!le64_to_cpu(disk_sb->blocks_per_group))
			return -EINVAL;
		if (!le64_to_cpu(disk_sb->inodes_per_group))
			return -EINVAL;
		if (!le64_to_cpu(disk_sb->group_desc_table_start) ||
		    !le64_to_cpu(disk_sb->group_desc_table_blocks))
			return -EINVAL;
		if (le64_to_cpu(disk_sb->group_desc_table_start) +
			    le64_to_cpu(disk_sb->group_desc_table_blocks) >
		    blocks_count)
			return -EINVAL;
		if (le64_to_cpu(disk_sb->group_count) *
			    sizeof(struct cryexts_group_desc) >
		    le64_to_cpu(disk_sb->group_desc_table_blocks) *
			    CRYEXTS_BLOCK_SIZE)
			return -EINVAL;
		if ((compat & CRYEXTS_FEATURE_COMPAT_HAS_JOURNAL) &&
		    (!journal_block || !journal_blocks ||
		     journal_block >= blocks_count ||
		     journal_block + journal_blocks > blocks_count))
			return -EINVAL;
		if (!(compat & CRYEXTS_FEATURE_COMPAT_HAS_JOURNAL) &&
		    (journal_block || journal_blocks))
			return -EINVAL;
		if (!!(incompat & CRYEXTS_FEATURE_INCOMPAT_NEEDS_RECOVERY) !=
		    !!(state & CRYEXTS_FS_STATE_NEEDS_RECOVERY))
			return -EINVAL;
		if (le32_to_cpu(disk_sb->features_incompat) &
		    CRYEXTS_FEATURE_INCOMPAT_BLOCK_GROUPS) {
			if (le64_to_cpu(disk_sb->group_count) < 2)
				return -EINVAL;
		} else {
			if (le64_to_cpu(disk_sb->group_count) != 1)
				return -EINVAL;
			if (le64_to_cpu(disk_sb->blocks_per_group) != blocks_count)
				return -EINVAL;
			if (le64_to_cpu(disk_sb->inodes_per_group) != inodes_count)
				return -EINVAL;
		}
		if (version >= CRYEXTS_VERSION_V5) {
			if (!(incompat & CRYEXTS_FEATURE_INCOMPAT_ORPHAN_LIST) &&
			    le64_to_cpu(disk_sb->orphan_head))
				return -EINVAL;
			if ((incompat & CRYEXTS_FEATURE_INCOMPAT_ORPHAN_LIST) &&
			    le64_to_cpu(disk_sb->orphan_head) > inodes_count)
				return -EINVAL;
		} else if (le64_to_cpu(disk_sb->orphan_head) ||
			   le64_to_cpu(disk_sb->policy_table_block) ||
			   le64_to_cpu(disk_sb->dir_index_seed) ||
			   le64_to_cpu(disk_sb->metadata_csum_type) ||
			   le64_to_cpu(disk_sb->journal_sequence) ||
			   le64_to_cpu(disk_sb->fs_generation)) {
			return -EINVAL;
		}
	} else if (state || le32_to_cpu(disk_sb->mount_count) ||
		   le32_to_cpu(disk_sb->max_mount_count) ||
		   le32_to_cpu(disk_sb->default_encryption_policy) ||
		   le64_to_cpu(disk_sb->last_mount_time) ||
		   le64_to_cpu(disk_sb->last_write_time) ||
		   le64_to_cpu(disk_sb->last_check_time) ||
		   le64_to_cpu(disk_sb->journal_block) ||
		   le64_to_cpu(disk_sb->journal_blocks) ||
		   le64_to_cpu(disk_sb->group_count) ||
		   le64_to_cpu(disk_sb->blocks_per_group) ||
		   le64_to_cpu(disk_sb->inodes_per_group) ||
		   le64_to_cpu(disk_sb->group_desc_table_start) ||
		   le64_to_cpu(disk_sb->group_desc_table_blocks) ||
		   le64_to_cpu(disk_sb->orphan_head) ||
		   le64_to_cpu(disk_sb->policy_table_block) ||
		   le64_to_cpu(disk_sb->dir_index_seed) ||
		   le64_to_cpu(disk_sb->metadata_csum_type) ||
		   le64_to_cpu(disk_sb->journal_sequence) ||
		   le64_to_cpu(disk_sb->fs_generation)) {
		return -EINVAL;
	}
	if (flags & CRYEXTS_SB_FLAG_ENCRYPTED) {
		if (!le32_to_cpu(disk_sb->key_hash))
			return -EINVAL;
		if (le32_to_cpu(disk_sb->encryption_flags) !=
		    CRYEXTS_ENC_FLAG_DATA)
			return -EINVAL;
		if (le32_to_cpu(disk_sb->encryption_kdf) !=
		    CRYEXTS_KDF_SALTED_FNV1A)
			return -EINVAL;
		if (le32_to_cpu(disk_sb->encryption_alg) != CRYEXTS_ALG_AES_CTR)
			return -EINVAL;
		if (cryexts_salt_is_zero(disk_sb->salt))
			return -EINVAL;
	} else if (le32_to_cpu(disk_sb->key_hash) ||
		   le32_to_cpu(disk_sb->encryption_flags) ||
		   le32_to_cpu(disk_sb->encryption_kdf) ||
		   le32_to_cpu(disk_sb->encryption_alg) ||
		   !cryexts_salt_is_zero(disk_sb->salt)) {
		return -EINVAL;
	}
	return 0;
}

static int cryexts_validate_v5_layout(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	struct cryexts_super_block *disk_sb = sbi->disk_sb;
	u32 version = le32_to_cpu(disk_sb->version);
	u32 incompat = le32_to_cpu(disk_sb->features_incompat);
	u32 ro_compat = le32_to_cpu(disk_sb->features_ro_compat);

	if (version < CRYEXTS_VERSION_V5)
		return 0;

	if ((incompat & CRYEXTS_FEATURE_INCOMPAT_POLICY_TABLE) &&
	    !le64_to_cpu(disk_sb->policy_table_block))
		return -EINVAL;

	if ((incompat & CRYEXTS_FEATURE_INCOMPAT_POLICY_TABLE) &&
	    le64_to_cpu(disk_sb->policy_table_block) &&
	    !cryexts_data_block_valid(sb,
				      le64_to_cpu(disk_sb->policy_table_block)))
		return -EINVAL;

	if (!!(ro_compat & CRYEXTS_FEATURE_RO_COMPAT_METADATA_CSUM) !=
	    !!le64_to_cpu(disk_sb->metadata_csum_type))
		return -EINVAL;
	if ((ro_compat & CRYEXTS_FEATURE_RO_COMPAT_METADATA_CSUM) &&
	    le64_to_cpu(disk_sb->metadata_csum_type) !=
		    CRYEXTS_METADATA_CSUM_FNV1A32)
		return -EINVAL;
	if (version < CRYEXTS_VERSION_V6 &&
	    (incompat & (CRYEXTS_FEATURE_INCOMPAT_JOURNAL_V2 |
			   CRYEXTS_FEATURE_INCOMPAT_JOURNAL_V3)))
		return -EINVAL;
	if ((incompat & CRYEXTS_FEATURE_INCOMPAT_JOURNAL_V2) &&
	    (incompat & CRYEXTS_FEATURE_INCOMPAT_JOURNAL_V3))
		return -EINVAL;
	if ((incompat & CRYEXTS_FEATURE_INCOMPAT_JOURNAL_RING) &&
	    (!(incompat & CRYEXTS_FEATURE_INCOMPAT_JOURNAL_V3) ||
	     version < CRYEXTS_VERSION_V6))
		return -EINVAL;
	if (version >= CRYEXTS_VERSION_V6 &&
	    (incompat & (CRYEXTS_FEATURE_INCOMPAT_JOURNAL_V2 |
			   CRYEXTS_FEATURE_INCOMPAT_JOURNAL_V3))) {
		u32 compat = le32_to_cpu(disk_sb->features_compat);
		u64 min_blocks = incompat & CRYEXTS_FEATURE_INCOMPAT_JOURNAL_V3 ?
			CRYEXTS_JOURNAL_V3_MIN_BLOCKS :
			CRYEXTS_JOURNAL_V2_MIN_BLOCKS;

		if (!(compat & CRYEXTS_FEATURE_COMPAT_HAS_JOURNAL))
			return -EINVAL;
		if (le64_to_cpu(disk_sb->journal_blocks) < min_blocks)
			return -EINVAL;
	}

	return 0;
}

static void cryexts_set_state(struct super_block *sb, u32 set_bits, u32 clear_bits)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	u32 state;

	if (!sbi || !sbi->disk_sb)
		return;

	state = le32_to_cpu(sbi->disk_sb->state);
	state |= set_bits;
	state &= ~clear_bits;
	sbi->disk_sb->state = cpu_to_le32(state);
	cryexts_update_super_checksum(sb);
	mark_buffer_dirty(sbi->s_sbh);
}

static void cryexts_put_super(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);

	if (!sbi)
		return;
	cancel_work_sync(&sbi->journal_checkpoint_work);
	if (!(sb->s_flags & SB_RDONLY) && sbi->journal_v3 && sbi->journal_ring)
		cryexts_journal_checkpoint_sync(sb);
	if (!(sb->s_flags & SB_RDONLY) &&
	    le32_to_cpu(sbi->disk_sb->version) >= CRYEXTS_VERSION_V4) {
		cryexts_set_state(sb, CRYEXTS_FS_STATE_CLEAN, 0);
		sbi->disk_sb->last_write_time =
			cpu_to_le64(ktime_get_real_seconds());
		cryexts_update_super_checksum(sb);
		mark_buffer_dirty(sbi->s_sbh);
		sync_dirty_buffer(sbi->s_sbh);
	}
	if (sbi->skcipher)
		crypto_free_skcipher(sbi->skcipher);
	cryexts_unload_policy_table(sbi);
	memzero_explicit(sbi->derived_key, sizeof(sbi->derived_key));
	sbi->derived_key_len = 0;
	cryexts_unload_bitmaps(sbi);
	cryexts_release_group_desc_table(sbi);
	brelse(sbi->s_sbh);
	kfree(sbi);
	sb->s_fs_info = NULL;
}

static int cryexts_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	u64 blocks = le64_to_cpu(sbi->disk_sb->blocks_count);
	u64 free_blocks = le64_to_cpu(sbi->disk_sb->free_blocks_count);

	buf->f_type = CRYEXTS_MAGIC;
	buf->f_bsize = CRYEXTS_BLOCK_SIZE;
	buf->f_blocks = blocks;
	buf->f_bfree = free_blocks;
	buf->f_bavail = free_blocks;
	buf->f_files = cryexts_max_inodes(sb);
	buf->f_ffree = le64_to_cpu(sbi->disk_sb->free_inodes_count);
	buf->f_namelen = CRYEXTS_NAME_LEN;
	return 0;
}

int cryexts_sync_metadata(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	u64 group;
	int err;

	if (!sbi)
		return 0;

	if (sbi->gdt_bhs)
		cryexts_gdt_prepare_write(sb);

	if (cryexts_metadata_csum_enabled(sb)) {
		if (sbi->s_sbh) {
			cryexts_update_super_checksum(sb);
			mark_buffer_dirty(sbi->s_sbh);
		}
	}

	if (sbi->gdt_bhs) {
		for (group = 0; group < sbi->group_desc_table_blocks; group++) {
			if (!sbi->gdt_bhs[group])
				continue;
			mark_buffer_dirty(sbi->gdt_bhs[group]);
			err = sync_dirty_buffer(sbi->gdt_bhs[group]);
			if (err)
				return err;
		}
	}
	if (sbi->block_bitmap_bh) {
		err = sync_dirty_buffer(sbi->block_bitmap_bh);
		if (err)
			return err;
	}
	if (sbi->inode_bitmap_bh) {
		err = sync_dirty_buffer(sbi->inode_bitmap_bh);
		if (err)
			return err;
	}
	if (sbi->group_block_bitmap_bhs) {
		for (group = 0; group < sbi->group_count; group++) {
			if (!sbi->group_block_bitmap_bhs[group])
				continue;
			err = sync_dirty_buffer(sbi->group_block_bitmap_bhs[group]);
			if (err)
				return err;
		}
	}
	if (sbi->group_inode_bitmap_bhs) {
		for (group = 0; group < sbi->group_count; group++) {
			if (!sbi->group_inode_bitmap_bhs[group])
				continue;
			err = sync_dirty_buffer(sbi->group_inode_bitmap_bhs[group]);
			if (err)
				return err;
		}
	}
	if (sbi->s_sbh) {
		err = sync_dirty_buffer(sbi->s_sbh);
		if (err)
			return err;
	}

	err = sync_blockdev(sb->s_bdev);
	if (err)
		return err;
	return 0;
}

static int cryexts_sync_fs(struct super_block *sb, int wait)
{
	if (!wait)
		return 0;
	return cryexts_sync_metadata(sb);
}

static const struct super_operations cryexts_super_ops = {
	.put_super = cryexts_put_super,
	.statfs = cryexts_statfs,
	.sync_fs = cryexts_sync_fs,
	.drop_inode = generic_delete_inode,
	.evict_inode = cryexts_evict_inode,
};

static int cryexts_fill_super(struct super_block *sb, void *data, int silent)
{
	struct cryexts_sb_info *sbi;
	struct cryexts_super_block *disk_sb;
	struct inode *root_inode;
	int ret = -EINVAL;

	if (!sb_set_blocksize(sb, CRYEXTS_BLOCK_SIZE))
		return -EINVAL;

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	sb->s_fs_info = sbi;
	sbi->sb = sb;
	mutex_init(&sbi->alloc_lock);
	mutex_init(&sbi->journal_lock);
	INIT_WORK(&sbi->journal_checkpoint_work,
		  cryexts_journal_checkpoint_worker);

	sbi->s_sbh = sb_bread(sb, 0);
	if (!sbi->s_sbh) {
		ret = -EIO;
		goto failed;
	}

	disk_sb = (struct cryexts_super_block *)
		(sbi->s_sbh->b_data + CRYEXTS_SUPER_OFFSET);
	sbi->disk_sb = disk_sb;

	sbi->inode_table_start = le64_to_cpu(disk_sb->inode_table_start);
	sbi->inode_table_blocks = le64_to_cpu(disk_sb->inode_table_blocks);
	sbi->block_bitmap_block = le64_to_cpu(disk_sb->block_bitmap_block);
	sbi->inode_bitmap_block = le64_to_cpu(disk_sb->inode_bitmap_block);
	sbi->group_desc_table_start =
		le64_to_cpu(disk_sb->group_desc_table_start);
	sbi->group_desc_table_blocks =
		le64_to_cpu(disk_sb->group_desc_table_blocks);
	sbi->group_count = le64_to_cpu(disk_sb->group_count);
	sbi->blocks_per_group = le64_to_cpu(disk_sb->blocks_per_group);
	sbi->inodes_per_group = le64_to_cpu(disk_sb->inodes_per_group);
	sbi->journal_block = le64_to_cpu(disk_sb->journal_block);
	sbi->journal_blocks = le64_to_cpu(disk_sb->journal_blocks);
	sbi->next_ino = le64_to_cpu(disk_sb->next_ino);
	sbi->next_data_block = le64_to_cpu(disk_sb->next_data_block);
	sbi->journal_sequence = le64_to_cpu(disk_sb->journal_sequence);
	sbi->journal_last_sequence = sbi->journal_sequence;
	sbi->journal_active_sequence = 0;
	sbi->journal_tail_sequence = sbi->journal_sequence;
	sbi->journal_checkpoint_sequence = sbi->journal_sequence;
	sbi->journal_ring_head = sbi->journal_block + 1;
	sbi->journal_ring_tail = sbi->journal_block + 1;
	sbi->journal_enabled =
		!!(le32_to_cpu(disk_sb->features_compat) &
		   CRYEXTS_FEATURE_COMPAT_HAS_JOURNAL);
	sbi->journal_v2 =
		!!(le32_to_cpu(disk_sb->features_incompat) &
		   CRYEXTS_FEATURE_INCOMPAT_JOURNAL_V2);
	sbi->journal_v3 =
		!!(le32_to_cpu(disk_sb->features_incompat) &
		   CRYEXTS_FEATURE_INCOMPAT_JOURNAL_V3);
	sbi->journal_ring =
		!!(le32_to_cpu(disk_sb->features_incompat) &
		   CRYEXTS_FEATURE_INCOMPAT_JOURNAL_RING);
	sbi->encrypted =
		!!(le32_to_cpu(disk_sb->flags) & CRYEXTS_SB_FLAG_ENCRYPTED);
	sbi->key_verifier = le32_to_cpu(disk_sb->key_hash);
	sbi->encryption_flags = le32_to_cpu(disk_sb->encryption_flags);
	sbi->encryption_kdf = le32_to_cpu(disk_sb->encryption_kdf);
	sbi->encryption_alg = le32_to_cpu(disk_sb->encryption_alg);
	memcpy(sbi->salt, disk_sb->salt, sizeof(sbi->salt));

	ret = cryexts_validate_super(sb);
	if (ret) {
		pr_err("cryexts: superblock validation failed (%d)\n", ret);
		goto failed;
	}
	ret = cryexts_verify_super_checksum(sb);
	if (ret) {
		pr_err("cryexts: superblock checksum validation failed (%d)\n",
		       ret);
		goto failed;
	}
	if (le32_to_cpu(disk_sb->version) >= CRYEXTS_VERSION_V4) {
		ret = cryexts_load_group_desc_table(sb);
		if (ret)
			goto failed;
		ret = cryexts_verify_group_checksums(sb);
		if (ret) {
			pr_err("cryexts: group checksum validation failed (%d)\n",
			       ret);
			goto failed;
		}
	}
	ret = cryexts_validate_v5_layout(sb);
	if (ret) {
		pr_err("cryexts: v5 layout validation failed (%d)\n", ret);
		goto failed;
	}
	ret = cryexts_load_bitmaps(sb);
	if (ret) {
		pr_err("cryexts: failed to load bitmaps (%d)\n", ret);
		goto failed;
	}
	ret = cryexts_set_encryption_key(sbi, data);
	if (ret) {
		pr_err("cryexts: failed to set encryption key (%d)\n", ret);
		goto failed;
	}
	ret = cryexts_load_policy_table(sb);
	if (ret) {
		pr_err("cryexts: failed to load policy table (%d)\n", ret);
		goto failed;
	}
	ret = cryexts_journal_replay(sb);
	if (ret) {
		pr_err("cryexts: journal replay failed (%d)\n", ret);
		goto failed;
	}
	if (!(sb->s_flags & SB_RDONLY) &&
	    cryexts_orphan_feature_enabled(sb) &&
	    le64_to_cpu(disk_sb->orphan_head)) {
		if (sbi->journal_enabled) {
			ret = cryexts_journal_begin(sb);
			if (ret) {
				pr_err("cryexts: orphan cleanup journal begin failed (%d)\n",
				       ret);
				goto failed;
			}
		}
		ret = cryexts_orphan_cleanup(sb);
		if (ret) {
			if (sbi->journal_enabled)
				cryexts_journal_abort(sb);
			pr_err("cryexts: orphan cleanup failed (%d)\n", ret);
			goto failed;
		}
		if (sbi->journal_enabled) {
			ret = cryexts_journal_commit(sb);
			if (ret) {
				pr_err("cryexts: orphan cleanup journal commit failed (%d)\n",
				       ret);
				goto failed;
			}
		}
	}

	if (!(sb->s_flags & SB_RDONLY) &&
	    le32_to_cpu(disk_sb->version) >= CRYEXTS_VERSION_V4) {
		u32 mount_count = le32_to_cpu(disk_sb->mount_count);

		disk_sb->mount_count = cpu_to_le32(mount_count + 1);
		disk_sb->last_mount_time = cpu_to_le64(ktime_get_real_seconds());
		cryexts_set_state(sb, 0, CRYEXTS_FS_STATE_CLEAN);
		if (le32_to_cpu(disk_sb->version) >= CRYEXTS_VERSION_V5 &&
		    !le64_to_cpu(disk_sb->fs_generation))
			disk_sb->fs_generation = cpu_to_le64(1);
		cryexts_update_super_checksum(sb);
		mark_buffer_dirty(sbi->s_sbh);
	}

	sb->s_magic = CRYEXTS_MAGIC;
	sb->s_op = &cryexts_super_ops;
	sb->s_xattr = cryexts_xattr_handlers;
	sb->s_maxbytes = (loff_t)CRYEXTS_EXTENT_TREE_V2_FILE_BLOCKS_MAX *
			 CRYEXTS_BLOCK_SIZE;
	sb->s_time_gran = 1;

	root_inode = cryexts_iget(sb, CRYEXTS_ROOT_INO);
	if (IS_ERR(root_inode)) {
		ret = PTR_ERR(root_inode);
		pr_err("cryexts: failed to load root inode (%d)\n", ret);
		goto failed;
	}
	if (!S_ISDIR(root_inode->i_mode)) {
		iput(root_inode);
		pr_err("cryexts: root inode is not a directory\n");
		ret = -EINVAL;
		goto failed;
	}
	ret = cryexts_validate_dir_block(root_inode);
	if (ret) {
		iput(root_inode);
		pr_err("cryexts: root directory validation failed (%d)\n", ret);
		goto failed;
	}

	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root) {
		ret = -ENOMEM;
		goto failed;
	}

	return 0;

failed:
	cryexts_put_super(sb);
	return ret;
}

static struct dentry *cryexts_mount(struct file_system_type *fs_type,
				    int flags, const char *dev_name,
				    void *data)
{
	return mount_bdev(fs_type, flags, dev_name, data, cryexts_fill_super);
}

static struct file_system_type cryexts_fs_type = {
	.owner = THIS_MODULE,
	.name = "cryexts",
	.mount = cryexts_mount,
	.kill_sb = kill_block_super,
	.fs_flags = FS_REQUIRES_DEV,
};

static int __init cryexts_init(void)
{
	return register_filesystem(&cryexts_fs_type);
}

static void __exit cryexts_exit(void)
{
	unregister_filesystem(&cryexts_fs_type);
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Carl and Codex");
MODULE_DESCRIPTION("CRYEXTS experimental filesystem v5 baseline");
MODULE_ALIAS_FS("cryexts");

module_init(cryexts_init);
module_exit(cryexts_exit);
