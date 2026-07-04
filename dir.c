// SPDX-License-Identifier: GPL-2.0
#include "cryexts.h"

static int cryexts_validate_dirent(struct super_block *sb,
				   struct cryexts_dir_entry *de,
				   unsigned int offset,
				   unsigned int block_limit);

static bool cryexts_dir_index_feature_enabled(struct super_block *sb)
{
	return !!(le32_to_cpu(CRYEXTS_SB(sb)->disk_sb->features_incompat) &
		  CRYEXTS_FEATURE_INCOMPAT_DIR_INDEX);
}

static u32 cryexts_dir_hash(struct super_block *sb, const char *name, size_t len)
{
	u32 hash = 2166136261u;
	u64 seed = le64_to_cpu(CRYEXTS_SB(sb)->disk_sb->dir_index_seed);
	size_t i;

	for (i = 0; i < sizeof(seed); i++) {
		hash ^= (u8)(seed >> (i * 8));
		hash *= 16777619u;
	}
	for (i = 0; i < len; i++) {
		hash ^= (u8)name[i];
		hash *= 16777619u;
	}
	return hash;
}

static int cryexts_dir_index_load(struct inode *dir,
				  struct cryexts_dir_index_block *index)
{
	struct cryexts_inode_info *info = cryexts_inode_blocks(dir);
	struct buffer_head *bh;

	if (!info || !info->dir_index_block)
		return -ENOENT;

	bh = sb_bread(dir->i_sb, info->dir_index_block);
	if (!bh)
		return -EIO;
	if (!cryexts_dir_index_checksum_valid(
		    dir->i_sb, info->dir_index_block,
		    (const struct cryexts_dir_index_block *)bh->b_data)) {
		brelse(bh);
		return -EUCLEAN;
	}
	memcpy(index, bh->b_data, sizeof(*index));
	brelse(bh);

	if (le32_to_cpu(index->magic) != CRYEXTS_DIR_INDEX_MAGIC ||
	    le16_to_cpu(index->buckets) != CRYEXTS_DIR_INDEX_BUCKETS)
		return -EUCLEAN;

	return 0;
}

static int cryexts_dir_index_store(struct inode *dir,
				   const struct cryexts_dir_index_block *index)
{
	struct cryexts_inode_info *info = cryexts_inode_blocks(dir);
	struct buffer_head *bh;

	if (!info || !info->dir_index_block)
		return -ENOENT;

	bh = sb_getblk(dir->i_sb, info->dir_index_block);
	if (!bh)
		return -EIO;

	lock_buffer(bh);
	memset(bh->b_data, 0, CRYEXTS_BLOCK_SIZE);
	memcpy(bh->b_data, index, sizeof(*index));
	cryexts_dir_index_set_checksum(
		dir->i_sb, info->dir_index_block,
		(struct cryexts_dir_index_block *)bh->b_data);
	set_buffer_uptodate(bh);
	cryexts_journal_record_bh(dir->i_sb, bh);
	mark_buffer_dirty(bh);
	unlock_buffer(bh);
	brelse(bh);
	return 0;
}

static int cryexts_dir_index_ensure(struct inode *dir)
{
	struct cryexts_inode_info *info = cryexts_inode_blocks(dir);
	u64 block;
	int err;

	if (!info || !cryexts_dir_index_feature_enabled(dir->i_sb))
		return 0;
	if (info->dir_index_block)
		return 0;

	err = cryexts_alloc_block_goal(dir->i_sb,
				       cryexts_inode_blocks(dir) ?
					       cryexts_inode_blocks(dir)->alloc_hint_block :
					       0,
				       cryexts_inode_blocks(dir) ?
					       cryexts_inode_blocks(dir)->alloc_goal_group :
					       U64_MAX,
				       &block);
	if (err) {
		pr_err("cryexts: failed to allocate dir-index block for dir ino=%lu (%d)\n",
		       dir->i_ino, err);
		return err;
	}
	info->dir_index_block = block;
	info->inode_flags |= CRYEXTS_INODE_FLAG_DIR_INDEX;
	cryexts_set_inode_alloc_hint(dir, block);
	return 0;
}

static int cryexts_dir_index_rebuild(struct inode *dir)
{
	struct cryexts_inode_info *info = cryexts_inode_blocks(dir);
	struct cryexts_dir_index_block index;
	struct buffer_head *bh;
	unsigned int dir_blocks;
	unsigned int limit = i_size_read(dir);
	unsigned int i;
	u32 total_entries = 0;
	int err;

	if (!info || !cryexts_dir_index_feature_enabled(dir->i_sb))
		return 0;

	err = cryexts_dir_index_ensure(dir);
	if (err)
		return err;

	memset(&index, 0, sizeof(index));
	index.magic = cpu_to_le32(CRYEXTS_DIR_INDEX_MAGIC);
	index.buckets = cpu_to_le16(CRYEXTS_DIR_INDEX_BUCKETS);
	dir_blocks = cryexts_dir_block_count(dir);
	index.dir_blocks = cpu_to_le16(dir_blocks);

	for (i = 0; i < dir_blocks; i++) {
		unsigned int offset = i * CRYEXTS_BLOCK_SIZE;
		unsigned int block_limit = min_t(unsigned int, CRYEXTS_BLOCK_SIZE,
						 limit - offset);

		bh = sb_bread(dir->i_sb, cryexts_inode_block_at(dir, i));
		if (!bh)
			return -EIO;

		while (offset < i * CRYEXTS_BLOCK_SIZE + block_limit) {
			struct cryexts_dir_entry *de;
			unsigned int rec_len;
			unsigned int local_offset = offset - i * CRYEXTS_BLOCK_SIZE;

			de = (struct cryexts_dir_entry *)(bh->b_data + local_offset);
			rec_len = le16_to_cpu(de->rec_len);
			err = cryexts_validate_dirent(dir->i_sb, de, local_offset,
						      block_limit);
			if (err) {
				brelse(bh);
				return err;
			}

			if (le64_to_cpu(de->inode) && de->name_len) {
				u32 hash = cryexts_dir_hash(dir->i_sb, de->name,
							    de->name_len);
				u32 bucket = hash % CRYEXTS_DIR_INDEX_BUCKETS;

				index.block_masks[bucket] |= cpu_to_le16(1U << i);
				total_entries++;
			}
			offset += rec_len;
		}

		brelse(bh);
	}

	index.entries = cpu_to_le32(total_entries);
	err = cryexts_dir_index_store(dir, &index);
	if (err) {
		pr_err("cryexts: failed to store rebuilt dir-index for dir ino=%lu (%d)\n",
		       dir->i_ino, err);
		return err;
	}
	dir->i_blocks = cryexts_inode_block_sectors(dir);
	err = cryexts_write_inode_to_disk(dir);
	if (err)
		pr_err("cryexts: failed to persist dir-index inode state for dir ino=%lu (%d)\n",
		       dir->i_ino, err);
	return err;
}

static int cryexts_dir_index_add_name(struct inode *dir, const struct qstr *name,
				      unsigned int logical_block)
{
	struct cryexts_inode_info *info = cryexts_inode_blocks(dir);
	struct cryexts_dir_index_block index;
	u32 bucket;
	u16 mask;
	int err;

	if (!info || !cryexts_dir_index_feature_enabled(dir->i_sb))
		return 0;
	if (logical_block >= CRYEXTS_DIRECT_BLOCKS)
		return cryexts_dir_index_rebuild(dir);

	err = cryexts_dir_index_ensure(dir);
	if (err)
		return err;
	err = cryexts_dir_index_load(dir, &index);
	if (err)
		return cryexts_dir_index_rebuild(dir);

	bucket = cryexts_dir_hash(dir->i_sb, name->name, name->len) %
		 CRYEXTS_DIR_INDEX_BUCKETS;
	mask = le16_to_cpu(index.block_masks[bucket]);
	mask |= (1U << logical_block);
	index.block_masks[bucket] = cpu_to_le16(mask);
	index.dir_blocks = cpu_to_le16(cryexts_dir_block_count(dir));
	index.entries = cpu_to_le32(le32_to_cpu(index.entries) + 1);

	err = cryexts_dir_index_store(dir, &index);
	if (!err)
		return 0;
	pr_err("cryexts: failed to update dir-index bucket for dir ino=%lu name=%.*s (%d), rebuilding\n",
	       dir->i_ino, name->len, name->name, err);
	return cryexts_dir_index_rebuild(dir);
}

static int cryexts_dir_index_remove_name(struct inode *dir,
					 const struct qstr *name,
					 struct buffer_head *bh,
					 unsigned int logical_block)
{
	struct cryexts_inode_info *info = cryexts_inode_blocks(dir);
	struct cryexts_dir_index_block index;
	unsigned int offset;
	unsigned int block_limit;
	u32 bucket;
	u32 entries;
	u16 mask;
	bool bucket_still_in_block = false;
	int err;

	if (!info || !cryexts_dir_index_feature_enabled(dir->i_sb))
		return 0;
	if (logical_block >= CRYEXTS_DIRECT_BLOCKS)
		return cryexts_dir_index_rebuild(dir);

	err = cryexts_dir_index_load(dir, &index);
	if (err)
		return cryexts_dir_index_rebuild(dir);

	bucket = cryexts_dir_hash(dir->i_sb, name->name, name->len) %
		 CRYEXTS_DIR_INDEX_BUCKETS;
	block_limit = min_t(unsigned int, CRYEXTS_BLOCK_SIZE,
			    i_size_read(dir) -
				    logical_block * CRYEXTS_BLOCK_SIZE);
	offset = 0;
	while (offset < block_limit) {
		struct cryexts_dir_entry *de;
		unsigned int rec_len;

		de = (struct cryexts_dir_entry *)(bh->b_data + offset);
		err = cryexts_validate_dirent(dir->i_sb, de, offset,
					      block_limit);
		if (err)
			return cryexts_dir_index_rebuild(dir);
		rec_len = le16_to_cpu(de->rec_len);

		if (le64_to_cpu(de->inode) && de->name_len) {
			u32 hash = cryexts_dir_hash(dir->i_sb, de->name,
						    de->name_len);

			if (hash % CRYEXTS_DIR_INDEX_BUCKETS == bucket) {
				bucket_still_in_block = true;
				break;
			}
		}
		offset += rec_len;
	}

	mask = le16_to_cpu(index.block_masks[bucket]);
	if (!bucket_still_in_block)
		mask &= ~(1U << logical_block);
	index.block_masks[bucket] = cpu_to_le16(mask);
	index.dir_blocks = cpu_to_le16(cryexts_dir_block_count(dir));
	entries = le32_to_cpu(index.entries);
	if (entries)
		entries--;
	index.entries = cpu_to_le32(entries);

	err = cryexts_dir_index_store(dir, &index);
	return err ? cryexts_dir_index_rebuild(dir) : 0;
}

static int cryexts_dir_logical_block_for_bh(struct inode *dir,
					    struct buffer_head *bh,
					    unsigned int *logical_block)
{
	unsigned int dir_blocks = cryexts_dir_block_count(dir);
	unsigned int i;

	for (i = 0; i < dir_blocks; i++) {
		if (cryexts_inode_block_at(dir, i) == bh->b_blocknr) {
			*logical_block = i;
			return 0;
		}
	}

	return -EUCLEAN;
}

static int cryexts_validate_dirent(struct super_block *sb,
				   struct cryexts_dir_entry *de,
				   unsigned int offset,
				   unsigned int block_limit)
{
	unsigned int rec_len = le16_to_cpu(de->rec_len);
	unsigned int name_capacity;
	u64 ino = le64_to_cpu(de->inode);

	if (rec_len < CRYEXTS_DIR_ENTRY_HEADER_SIZE || rec_len % 4)
		return -EUCLEAN;
	if (offset + rec_len > block_limit || offset + rec_len > CRYEXTS_BLOCK_SIZE)
		return -EUCLEAN;

	name_capacity = rec_len - CRYEXTS_DIR_ENTRY_HEADER_SIZE;
	if (de->name_len > name_capacity || de->name_len > CRYEXTS_NAME_LEN)
		return -EUCLEAN;
	if (ino && (ino < CRYEXTS_ROOT_INO || ino > cryexts_max_inodes(sb)))
		return -EUCLEAN;
	if (de->file_type != CRYEXTS_FT_UNKNOWN &&
	    de->file_type != CRYEXTS_FT_REG_FILE &&
	    de->file_type != CRYEXTS_FT_DIR &&
	    de->file_type != CRYEXTS_FT_SYMLINK)
		return -EUCLEAN;
	return 0;
}

int cryexts_validate_dir_block(struct inode *dir)
{
	struct cryexts_inode_info *info = cryexts_inode_blocks(dir);
	struct cryexts_dir_index_block index;
	struct buffer_head *bh;
	u64 block = cryexts_inode_first_block(dir);
	unsigned int offset = 0;
	unsigned int limit = i_size_read(dir);
	unsigned int expected_blocks = cryexts_dir_block_count(dir);
	unsigned int i;
	bool seen_dot = false;
	bool seen_dotdot = false;
	bool use_index = false;

	if (!expected_blocks || expected_blocks > CRYEXTS_DIRECT_BLOCKS)
		return -EUCLEAN;
	if (info && (info->inode_flags & CRYEXTS_INODE_FLAG_DIR_INDEX)) {
		int err = cryexts_dir_index_load(dir, &index);

		if (err)
			return err;
		if (le16_to_cpu(index.dir_blocks) != expected_blocks)
			return -EUCLEAN;
		use_index = true;
	}
	for (i = 0; i < expected_blocks; i++) {
		block = cryexts_inode_block_at(dir, i);
		if (!cryexts_data_block_valid(dir->i_sb, block))
			return -EUCLEAN;
	}
	if (limit > (u64)expected_blocks * CRYEXTS_BLOCK_SIZE ||
	    limit % CRYEXTS_BLOCK_SIZE)
		return -EUCLEAN;

	for (i = 0; i < expected_blocks; i++) {
		unsigned int block_limit = min_t(unsigned int, CRYEXTS_BLOCK_SIZE,
						 limit - i * CRYEXTS_BLOCK_SIZE);

		bh = sb_bread(dir->i_sb, cryexts_inode_block_at(dir, i));
		if (!bh) {
			pr_err("cryexts: failed to read dir block %u while validating dir ino=%lu\n",
			       i, dir->i_ino);
			return -EIO;
		}

		while (offset < i * CRYEXTS_BLOCK_SIZE + block_limit) {
			struct cryexts_dir_entry *de;
			unsigned int rec_len;
			int err;
			unsigned int local_offset = offset - i * CRYEXTS_BLOCK_SIZE;

			de = (struct cryexts_dir_entry *)(bh->b_data + local_offset);
			err = cryexts_validate_dirent(dir->i_sb, de, local_offset,
						      block_limit);
			if (err) {
				brelse(bh);
				return err;
			}

			if (le64_to_cpu(de->inode)) {
				if (use_index && de->name_len) {
					u32 hash = cryexts_dir_hash(dir->i_sb, de->name,
								    de->name_len);
					u32 bucket = hash % CRYEXTS_DIR_INDEX_BUCKETS;

					if (!(le16_to_cpu(index.block_masks[bucket]) &
					      (1U << i)))
						return -EUCLEAN;
				}
				if (de->name_len == 1 && de->name[0] == '.')
					seen_dot = true;
				if (de->name_len == 2 && de->name[0] == '.' &&
				    de->name[1] == '.')
					seen_dotdot = true;
			}

			rec_len = le16_to_cpu(de->rec_len);
			offset += rec_len;
		}

		brelse(bh);
	}

	return seen_dot && seen_dotdot ? 0 : -EUCLEAN;
}

static unsigned char cryexts_dtype(__u8 file_type)
{
	switch (file_type) {
	case CRYEXTS_FT_DIR:
		return DT_DIR;
	case CRYEXTS_FT_REG_FILE:
		return DT_REG;
	case CRYEXTS_FT_SYMLINK:
		return DT_LNK;
	default:
		return DT_UNKNOWN;
	}
}

static __u8 cryexts_file_type(umode_t mode)
{
	if (S_ISDIR(mode))
		return CRYEXTS_FT_DIR;
	if (S_ISREG(mode))
		return CRYEXTS_FT_REG_FILE;
	if (S_ISLNK(mode))
		return CRYEXTS_FT_SYMLINK;
	return CRYEXTS_FT_UNKNOWN;
}

static int cryexts_write_from_kernel(struct inode *inode, const char *buf,
				     size_t len)
{
	char *block_buf;
	size_t done = 0;
	int err = 0;
	struct timespec64 now;

	block_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_KERNEL);
	if (!block_buf)
		return -ENOMEM;

	inode_lock(inode);
	while (done < len) {
		u64 logical = done / CRYEXTS_BLOCK_SIZE;
		u64 physical;
		size_t block_off = done % CRYEXTS_BLOCK_SIZE;
		size_t chunk = min_t(size_t, len - done,
				     CRYEXTS_BLOCK_SIZE - block_off);

		err = cryexts_resolve_block(inode, logical, true, &physical);
		if (err)
			goto out_unlock;

		if (block_off || chunk < CRYEXTS_BLOCK_SIZE) {
			err = cryexts_read_inode_block(inode, physical,
						       block_buf);
			if (err)
				goto out_unlock;
		} else {
			memset(block_buf, 0, CRYEXTS_BLOCK_SIZE);
		}

		memcpy(block_buf + block_off, buf + done, chunk);
		err = cryexts_write_inode_block(inode, physical, block_buf);
		if (err)
			goto out_unlock;
		done += chunk;
	}

	i_size_write(inode, len);
	inode->i_blocks = cryexts_inode_block_sectors(inode);
	now = current_time(inode);
	inode->i_mtime = now;
	inode->i_ctime = now;
	err = cryexts_write_inode_to_disk(inode);

out_unlock:
	inode_unlock(inode);
	kfree(block_buf);
	return err;
}

static int cryexts_find_entry(struct inode *dir, const struct qstr *name,
			      struct buffer_head **bhp,
			      struct cryexts_dir_entry **dep)
{
	struct cryexts_inode_info *info = cryexts_inode_blocks(dir);
	struct cryexts_dir_index_block index;
	struct buffer_head *bh;
	unsigned int i;
	unsigned int limit = i_size_read(dir);
	unsigned int dir_blocks = cryexts_dir_block_count(dir);
	u16 candidate_mask = 0;

	if (!dir_blocks)
		return -ENOENT;
	if (info && (info->inode_flags & CRYEXTS_INODE_FLAG_DIR_INDEX) &&
	    !cryexts_dir_index_load(dir, &index)) {
		u32 bucket = cryexts_dir_hash(dir->i_sb, name->name, name->len) %
			     CRYEXTS_DIR_INDEX_BUCKETS;

		candidate_mask = le16_to_cpu(index.block_masks[bucket]);
	}

	for (i = 0; i < dir_blocks; i++) {
		unsigned int offset = i * CRYEXTS_BLOCK_SIZE;
		unsigned int block_limit = min_t(unsigned int, CRYEXTS_BLOCK_SIZE,
						 limit - offset);

		if (candidate_mask && !(candidate_mask & (1U << i)))
			continue;

		bh = sb_bread(dir->i_sb, cryexts_inode_block_at(dir, i));
		if (!bh)
			return -EIO;

		while (offset < i * CRYEXTS_BLOCK_SIZE + block_limit) {
			struct cryexts_dir_entry *de;
			unsigned int rec_len;
			unsigned int local_offset = offset - i * CRYEXTS_BLOCK_SIZE;

			de = (struct cryexts_dir_entry *)(bh->b_data + local_offset);
			rec_len = le16_to_cpu(de->rec_len);
			if (cryexts_validate_dirent(dir->i_sb, de, local_offset,
						    block_limit)) {
				brelse(bh);
				return -EUCLEAN;
			}

			if (le64_to_cpu(de->inode) &&
			    de->name_len == name->len &&
			    !memcmp(de->name, name->name, name->len)) {
				*bhp = bh;
				*dep = de;
				return 0;
			}
			offset += rec_len;
		}

		brelse(bh);
	}

	return -ENOENT;
}

static int cryexts_add_entry(struct inode *dir, const struct qstr *name,
			     u64 ino, umode_t mode)
{
	struct buffer_head *bh;
	struct buffer_head *found_bh;
	struct cryexts_dir_entry *found_de;
	struct cryexts_dir_entry *de;
	unsigned int need = cryexts_dir_rec_len(name->len);
	unsigned int limit = i_size_read(dir);
	unsigned int dir_blocks = cryexts_dir_block_count(dir);
	unsigned int i;
	int err;

	if (name->len > CRYEXTS_NAME_LEN)
		return -ENAMETOOLONG;

	err = cryexts_find_entry(dir, name, &found_bh, &found_de);
	if (err != -ENOENT) {
		if (!err)
			brelse(found_bh);
		return err ? err : -EEXIST;
	}

	for (i = 0; i < dir_blocks; i++) {
		unsigned int offset = i * CRYEXTS_BLOCK_SIZE;
		unsigned int block_limit = min_t(unsigned int, CRYEXTS_BLOCK_SIZE,
						 limit - offset);

		bh = sb_bread(dir->i_sb, cryexts_inode_block_at(dir, i));
		if (!bh)
			return -EIO;

		while (offset < i * CRYEXTS_BLOCK_SIZE + block_limit) {
			unsigned int rec_len;
			unsigned int actual;
			unsigned int local_offset = offset - i * CRYEXTS_BLOCK_SIZE;

			de = (struct cryexts_dir_entry *)(bh->b_data + local_offset);
			rec_len = le16_to_cpu(de->rec_len);
			if (cryexts_validate_dirent(dir->i_sb, de, local_offset,
						    block_limit)) {
				brelse(bh);
				return -EUCLEAN;
			}

			if (!le64_to_cpu(de->inode) && rec_len >= need)
				goto write_entry;

			actual = cryexts_dir_rec_len(de->name_len);
			if (rec_len >= actual + need) {
				struct cryexts_dir_entry *new_de;

				de->rec_len = cpu_to_le16(actual);
				new_de = (struct cryexts_dir_entry *)((char *)de + actual);
				new_de->rec_len = cpu_to_le16(rec_len - actual);
				de = new_de;
				goto write_entry;
			}

			offset += rec_len;
		}

		brelse(bh);
	}

	if (dir_blocks >= CRYEXTS_DIRECT_BLOCKS)
		return -ENOSPC;

	{
		u64 new_block;

		err = cryexts_alloc_block_goal(dir->i_sb,
					       cryexts_inode_blocks(dir) ?
						       cryexts_inode_blocks(dir)->alloc_hint_block :
						       0,
					       cryexts_inode_blocks(dir) ?
						       cryexts_inode_blocks(dir)->alloc_goal_group :
						       U64_MAX,
					       &new_block);
		if (err) {
			pr_err("cryexts: failed to allocate new dir data block for parent ino=%lu while adding %.*s (%d)\n",
			       dir->i_ino, name->len, name->name, err);
			return err;
		}

		cryexts_inode_blocks(dir)->direct[dir_blocks] = new_block;
		cryexts_set_inode_alloc_hint(dir, new_block);
		i_size_write(dir, i_size_read(dir) + CRYEXTS_BLOCK_SIZE);
		dir->i_blocks = cryexts_inode_block_sectors(dir);

		bh = sb_getblk(dir->i_sb, new_block);
		if (!bh) {
			cryexts_inode_blocks(dir)->direct[dir_blocks] = 0;
			i_size_write(dir, i_size_read(dir) - CRYEXTS_BLOCK_SIZE);
			dir->i_blocks = cryexts_inode_block_sectors(dir);
			cryexts_free_block(dir->i_sb, new_block);
			return -EIO;
		}
		lock_buffer(bh);
		memset(bh->b_data, 0, CRYEXTS_BLOCK_SIZE);
		de = (struct cryexts_dir_entry *)bh->b_data;
		de->rec_len = cpu_to_le16(CRYEXTS_BLOCK_SIZE);
		de->inode = 0;
		de->name_len = 0;
		de->file_type = CRYEXTS_FT_UNKNOWN;
		set_buffer_uptodate(bh);
		cryexts_journal_record_bh(dir->i_sb, bh);
		mark_buffer_dirty(bh);
		unlock_buffer(bh);
		goto write_entry;
	}

write_entry:
	cryexts_journal_record_bh(dir->i_sb, bh);
	de->inode = cpu_to_le64(ino);
	de->rec_len = de->rec_len ? de->rec_len : cpu_to_le16(CRYEXTS_BLOCK_SIZE);
	de->name_len = name->len;
	de->file_type = cryexts_file_type(mode);
	memcpy(de->name, name->name, name->len);
	mark_buffer_dirty(bh);
	dir->i_mtime = current_time(dir);
	dir->i_ctime = dir->i_mtime;
	err = cryexts_write_inode_to_disk(dir);
	if (err)
		pr_err("cryexts: failed to persist parent dir ino=%lu after adding %.*s (%d)\n",
		       dir->i_ino, name->len, name->name, err);
	if (!err) {
		err = cryexts_dir_index_add_name(dir, name, i);
		if (err)
			pr_err("cryexts: failed to update dir-index for parent ino=%lu after adding %.*s (%d)\n",
			       dir->i_ino, name->len, name->name, err);
	}
	brelse(bh);
	return err;
}

static int cryexts_delete_entry(struct inode *dir, const struct qstr *name)
{
	struct buffer_head *bh;
	struct cryexts_dir_entry *de;
	unsigned int logical_block;
	int err;

	err = cryexts_find_entry(dir, name, &bh, &de);
	if (err)
		return err;
	err = cryexts_dir_logical_block_for_bh(dir, bh, &logical_block);
	if (err) {
		brelse(bh);
		return err;
	}

	cryexts_journal_record_bh(dir->i_sb, bh);
	de->inode = 0;
	de->name_len = 0;
	de->file_type = CRYEXTS_FT_UNKNOWN;
	mark_buffer_dirty(bh);
	dir->i_mtime = current_time(dir);
	dir->i_ctime = dir->i_mtime;
	err = cryexts_write_inode_to_disk(dir);
	if (!err)
		err = cryexts_dir_index_remove_name(dir, name, bh,
						    logical_block);
	brelse(bh);
	return err;
}

static int cryexts_dir_empty(struct inode *dir)
{
	struct buffer_head *bh;
	unsigned int i;
	unsigned int limit = i_size_read(dir);
	unsigned int dir_blocks = cryexts_dir_block_count(dir);

	for (i = 0; i < dir_blocks; i++) {
		unsigned int offset = i * CRYEXTS_BLOCK_SIZE;
		unsigned int block_limit = min_t(unsigned int, CRYEXTS_BLOCK_SIZE,
						 limit - offset);

		bh = sb_bread(dir->i_sb, cryexts_inode_block_at(dir, i));
		if (!bh)
			return 0;

		while (offset < i * CRYEXTS_BLOCK_SIZE + block_limit) {
			struct cryexts_dir_entry *de;
			unsigned int rec_len;
			unsigned int local_offset = offset - i * CRYEXTS_BLOCK_SIZE;

			de = (struct cryexts_dir_entry *)(bh->b_data + local_offset);
			rec_len = le16_to_cpu(de->rec_len);
			if (cryexts_validate_dirent(dir->i_sb, de, local_offset,
						    block_limit)) {
				brelse(bh);
				return 0;
			}
			if (le64_to_cpu(de->inode) &&
			    !(de->name_len == 1 && de->name[0] == '.') &&
			    !(de->name_len == 2 && de->name[0] == '.' &&
			      de->name[1] == '.')) {
				brelse(bh);
				return 0;
			}
			offset += rec_len;
		}

		brelse(bh);
	}

	return 1;
}

static int cryexts_init_dir_block(struct inode *inode, struct inode *parent)
{
	struct buffer_head *bh;
	u64 block = cryexts_inode_first_block(inode);
	struct cryexts_dir_entry *de;
	unsigned int dot_len;

	bh = sb_getblk(inode->i_sb, block);
	if (!bh)
		return -EIO;

	lock_buffer(bh);
	memset(bh->b_data, 0, CRYEXTS_BLOCK_SIZE);
	de = (struct cryexts_dir_entry *)bh->b_data;
	dot_len = cryexts_dir_rec_len(1);
	de->inode = cpu_to_le64(inode->i_ino);
	de->rec_len = cpu_to_le16(dot_len);
	de->name_len = 1;
	de->file_type = CRYEXTS_FT_DIR;
	memcpy(de->name, ".", 1);

	de = (struct cryexts_dir_entry *)(bh->b_data + dot_len);
	de->inode = cpu_to_le64(parent->i_ino);
	de->rec_len = cpu_to_le16(CRYEXTS_BLOCK_SIZE - dot_len);
	de->name_len = 2;
	de->file_type = CRYEXTS_FT_DIR;
	memcpy(de->name, "..", 2);

	set_buffer_uptodate(bh);
	cryexts_journal_record_bh(inode->i_sb, bh);
	mark_buffer_dirty(bh);
	unlock_buffer(bh);
	brelse(bh);
	return cryexts_dir_index_rebuild(inode);
}

static int cryexts_update_dir_parent(struct inode *dir, u64 parent_ino)
{
	struct buffer_head *bh;
	struct cryexts_dir_entry *de;
	static const char dotdot_name[] = "..";
	struct qstr dotdot = {
		.name = dotdot_name,
		.len = 2,
		.hash = 0,
	};
	int err;

	err = cryexts_find_entry(dir, &dotdot, &bh, &de);
	if (err)
		return err;

	cryexts_journal_record_bh(dir->i_sb, bh);
	de->inode = cpu_to_le64(parent_ino);
	mark_buffer_dirty(bh);
	err = cryexts_write_inode_to_disk(dir);
	brelse(bh);
	return err;
}

static int cryexts_dir_parent_ino(struct inode *dir, u64 *parent_ino)
{
	static const char dotdot_name[] = "..";
	struct qstr dotdot = {
		.name = dotdot_name,
		.len = 2,
		.hash = 0,
	};
	struct buffer_head *bh;
	struct cryexts_dir_entry *de;
	int err;

	err = cryexts_find_entry(dir, &dotdot, &bh, &de);
	if (err)
		return err;

	*parent_ino = le64_to_cpu(de->inode);
	brelse(bh);
	return 0;
}

static int cryexts_dir_has_ancestor(struct inode *dir, u64 ancestor_ino)
{
	struct inode *cursor = dir;
	u64 limit = cryexts_max_inodes(dir->i_sb) + 1;
	u64 parent_ino;
	u64 depth;
	int err = 0;
	int ret = 0;

	for (depth = 0; depth < limit; depth++) {
		if (cursor->i_ino == ancestor_ino) {
			ret = 1;
			goto out;
		}
		if (cursor->i_ino == CRYEXTS_ROOT_INO)
			goto out;

		err = cryexts_dir_parent_ino(cursor, &parent_ino);
		if (err) {
			ret = err;
			goto out;
		}
		if (parent_ino == cursor->i_ino) {
			ret = parent_ino == ancestor_ino ? -EINVAL : 0;
			goto out;
		}
		{
			struct inode *parent;

			parent = cryexts_iget(dir->i_sb, parent_ino);
			if (IS_ERR(parent)) {
				ret = PTR_ERR(parent);
				goto out;
			}
			if (cursor != dir)
				iput(cursor);
			cursor = parent;
		}
	}

	ret = -EUCLEAN;

out:
	if (cursor != dir)
		iput(cursor);
	return ret;
}

static int cryexts_iterate(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	struct buffer_head *bh;
	unsigned int limit = i_size_read(inode);
	unsigned int dir_blocks = cryexts_dir_block_count(inode);
	unsigned int i;

	if (ctx->pos >= inode->i_size)
		return 0;

	for (i = ctx->pos / CRYEXTS_BLOCK_SIZE; i < dir_blocks; i++) {
		unsigned int offset = max_t(unsigned int, ctx->pos,
					    i * CRYEXTS_BLOCK_SIZE);
		unsigned int block_limit = min_t(unsigned int, CRYEXTS_BLOCK_SIZE,
						 limit - i * CRYEXTS_BLOCK_SIZE);

		bh = sb_bread(inode->i_sb, cryexts_inode_block_at(inode, i));
		if (!bh)
			return -EIO;

		while (offset < i * CRYEXTS_BLOCK_SIZE + block_limit) {
			struct cryexts_dir_entry *de;
			unsigned int rec_len;
			unsigned int local_offset = offset - i * CRYEXTS_BLOCK_SIZE;

			de = (struct cryexts_dir_entry *)(bh->b_data + local_offset);
			rec_len = le16_to_cpu(de->rec_len);
			if (cryexts_validate_dirent(inode->i_sb, de, local_offset,
						    block_limit)) {
				brelse(bh);
				return -EUCLEAN;
			}

			if (le64_to_cpu(de->inode) && de->name_len) {
				if (!dir_emit(ctx, de->name, de->name_len,
					      le64_to_cpu(de->inode),
					      cryexts_dtype(de->file_type))) {
					brelse(bh);
					return 0;
				}
			}

			offset += rec_len;
			ctx->pos = offset;
		}

		brelse(bh);
	}

	return 0;
}

static struct dentry *cryexts_lookup(struct inode *dir, struct dentry *dentry,
				     unsigned int flags)
{
	struct buffer_head *bh;
	struct cryexts_dir_entry *de;
	struct inode *inode = NULL;
	int err;

	if (dentry->d_name.len > CRYEXTS_NAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	err = cryexts_find_entry(dir, &dentry->d_name, &bh, &de);
	if (!err) {
		inode = cryexts_iget(dir->i_sb, le64_to_cpu(de->inode));
		brelse(bh);
		if (IS_ERR(inode))
			return ERR_CAST(inode);
	} else if (err != -ENOENT) {
		return ERR_PTR(err);
	}

	return d_splice_alias(inode, dentry);
}

static int cryexts_create(struct user_namespace *mnt_userns,
			  struct inode *dir, struct dentry *dentry,
			  umode_t mode, bool excl)
{
	struct inode *inode;
	int err;

	err = cryexts_journal_begin(dir->i_sb);
	if (err) {
		pr_err("cryexts: mkdir journal begin failed in parent ino=%lu name=%.*s (%d)\n",
		       dir->i_ino, dentry->d_name.len, dentry->d_name.name, err);
		return err;
	}

	inode = cryexts_new_inode(dir, S_IFREG | mode, 0);
	if (IS_ERR(inode)) {
		cryexts_journal_abort(dir->i_sb);
		return PTR_ERR(inode);
	}

	err = cryexts_add_entry(dir, &dentry->d_name, inode->i_ino,
				inode->i_mode);
	if (err) {
		clear_nlink(inode);
		discard_new_inode(inode);
		cryexts_journal_abort(dir->i_sb);
		return err;
	}

	err = cryexts_journal_commit(dir->i_sb);
	if (err)
		return err;
	d_instantiate_new(dentry, inode);
	return 0;
}

static int cryexts_mkdir(struct user_namespace *mnt_userns,
			 struct inode *dir, struct dentry *dentry,
			 umode_t mode)
{
	struct inode *inode;
	u64 block;
	int err;

	err = cryexts_journal_begin(dir->i_sb);
	if (err)
		return err;

	err = cryexts_alloc_block_goal(dir->i_sb,
				       cryexts_inode_blocks(dir) ?
					       cryexts_inode_blocks(dir)->alloc_hint_block :
					       0,
				       cryexts_inode_blocks(dir) ?
					       cryexts_inode_blocks(dir)->alloc_goal_group :
					       U64_MAX,
				       &block);
	if (err) {
		pr_err("cryexts: mkdir failed to allocate first data block in parent ino=%lu name=%.*s (%d)\n",
		       dir->i_ino, dentry->d_name.len, dentry->d_name.name, err);
		cryexts_journal_abort(dir->i_sb);
		return err;
	}

	inode = cryexts_new_inode(dir, S_IFDIR | mode, block);
	if (IS_ERR(inode)) {
		pr_err("cryexts: mkdir failed to allocate inode in parent ino=%lu name=%.*s (%ld)\n",
		       dir->i_ino, dentry->d_name.len, dentry->d_name.name,
		       PTR_ERR(inode));
		goto fail_free_block;
	}

	err = cryexts_init_dir_block(inode, dir);
	if (err) {
		pr_err("cryexts: mkdir failed to initialize new dir inode=%lu parent ino=%lu name=%.*s (%d)\n",
		       inode->i_ino, dir->i_ino, dentry->d_name.len,
		       dentry->d_name.name, err);
		goto fail;
	}

	err = cryexts_add_entry(dir, &dentry->d_name, inode->i_ino,
				inode->i_mode);
	if (err) {
		pr_err("cryexts: mkdir failed to link new dir inode=%lu into parent ino=%lu name=%.*s (%d)\n",
		       inode->i_ino, dir->i_ino, dentry->d_name.len,
		       dentry->d_name.name, err);
		goto fail;
	}

	inc_nlink(dir);
	err = cryexts_write_inode_to_disk(dir);
	if (err) {
		pr_err("cryexts: mkdir failed to persist parent link count ino=%lu name=%.*s (%d)\n",
		       dir->i_ino, dentry->d_name.len, dentry->d_name.name, err);
		goto fail;
	}
	err = cryexts_journal_commit(dir->i_sb);
	if (err) {
		pr_err("cryexts: mkdir journal commit failed parent ino=%lu name=%.*s (%d)\n",
		       dir->i_ino, dentry->d_name.len, dentry->d_name.name, err);
		return err;
	}
	d_instantiate_new(dentry, inode);
	return 0;

fail:
	clear_nlink(inode);
	discard_new_inode(inode);
fail_free_block:
	if (block)
		cryexts_free_block(dir->i_sb, block);
	cryexts_journal_abort(dir->i_sb);
	return err;
}

static int cryexts_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	int err;

	err = cryexts_journal_begin(dir->i_sb);
	if (err)
		return err;

	err = cryexts_delete_entry(dir, &dentry->d_name);
	if (err) {
		cryexts_journal_abort(dir->i_sb);
		return err;
	}

	drop_nlink(inode);
	inode->i_ctime = current_time(inode);
	err = cryexts_write_inode_to_disk(inode);
	if (err) {
		cryexts_journal_abort(dir->i_sb);
		return err;
	}
	if (!inode->i_nlink) {
		err = cryexts_orphan_set(dir->i_sb, inode->i_ino);
		if (err) {
			cryexts_journal_abort(dir->i_sb);
			return err;
		}
		err = cryexts_release_inode_storage(inode);
		if (err) {
			cryexts_journal_abort(dir->i_sb);
			return err;
		}
		err = cryexts_orphan_clear(dir->i_sb, inode->i_ino);
		if (err) {
			cryexts_journal_abort(dir->i_sb);
			return err;
		}
		cryexts_free_inode(dir->i_sb, inode->i_ino);
	}
	err = cryexts_journal_commit(dir->i_sb);
	if (err)
		return err;
	return 0;
}

static int cryexts_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	int err;

	err = cryexts_journal_begin(dir->i_sb);
	if (err)
		return err;

	if (!cryexts_dir_empty(inode)) {
		cryexts_journal_abort(dir->i_sb);
		return -ENOTEMPTY;
	}

	err = cryexts_delete_entry(dir, &dentry->d_name);
	if (err) {
		cryexts_journal_abort(dir->i_sb);
		return err;
	}

	clear_nlink(inode);
	drop_nlink(dir);
	inode->i_ctime = current_time(inode);
	cryexts_write_inode_to_disk(inode);
	cryexts_write_inode_to_disk(dir);
	err = cryexts_orphan_set(dir->i_sb, inode->i_ino);
	if (err) {
		cryexts_journal_abort(dir->i_sb);
		return err;
	}
	err = cryexts_release_inode_storage(inode);
	if (err) {
		cryexts_journal_abort(dir->i_sb);
		return err;
	}
	err = cryexts_orphan_clear(dir->i_sb, inode->i_ino);
	if (err) {
		cryexts_journal_abort(dir->i_sb);
		return err;
	}
	cryexts_free_inode(dir->i_sb, inode->i_ino);
	err = cryexts_journal_commit(dir->i_sb);
	if (err)
		return err;
	return 0;
}

static int cryexts_rename(struct user_namespace *mnt_userns,
			  struct inode *old_dir, struct dentry *old_dentry,
			  struct inode *new_dir, struct dentry *new_dentry,
			  unsigned int flags)
{
	struct inode *old_inode = d_inode(old_dentry);
	struct inode *new_inode = d_inode(new_dentry);
	struct buffer_head *old_bh;
	struct buffer_head *new_bh = NULL;
	struct cryexts_dir_entry *old_de;
	struct cryexts_dir_entry *new_de = NULL;
	struct inode *victim = NULL;
	u64 victim_ino = 0;
	umode_t victim_mode = 0;
	u64 old_parent_ino = old_dir->i_ino;
	u64 new_parent_ino = new_dir->i_ino;
	bool victim_is_dir = false;
	bool victim_dir_count_dropped = false;
	int err;

	err = cryexts_journal_begin(old_dir->i_sb);
	if (err)
		return err;

	if (flags) {
		cryexts_journal_abort(old_dir->i_sb);
		return -EINVAL;
	}
	if (!old_inode) {
		cryexts_journal_abort(old_dir->i_sb);
		return -ENOENT;
	}
	if (old_dir == new_dir &&
	    old_dentry->d_name.len == new_dentry->d_name.len &&
	    !memcmp(old_dentry->d_name.name, new_dentry->d_name.name,
		    old_dentry->d_name.len)) {
		cryexts_journal_abort(old_dir->i_sb);
		return 0;
	}
	if (old_dentry->d_name.len > CRYEXTS_NAME_LEN ||
	    new_dentry->d_name.len > CRYEXTS_NAME_LEN) {
		cryexts_journal_abort(old_dir->i_sb);
		return -ENAMETOOLONG;
	}
	if (S_ISDIR(old_inode->i_mode)) {
		err = cryexts_dir_has_ancestor(new_dir, old_inode->i_ino);
		if (err) {
			cryexts_journal_abort(old_dir->i_sb);
			return err > 0 ? -EINVAL : err;
		}
	}

	err = cryexts_find_entry(old_dir, &old_dentry->d_name, &old_bh, &old_de);
	if (err) {
		cryexts_journal_abort(old_dir->i_sb);
		return err;
	}

	if (new_inode) {
		victim = new_inode;
		victim_is_dir = S_ISDIR(victim->i_mode);
		if (S_ISDIR(old_inode->i_mode) && !S_ISDIR(victim->i_mode)) {
			err = -ENOTDIR;
			goto out_old;
		}
		if (!S_ISDIR(old_inode->i_mode) && S_ISDIR(victim->i_mode)) {
			err = -EISDIR;
			goto out_old;
		}
		if (S_ISDIR(victim->i_mode) && !cryexts_dir_empty(victim)) {
			err = -ENOTEMPTY;
			goto out_old;
		}

		err = cryexts_find_entry(new_dir, &new_dentry->d_name, &new_bh,
					 &new_de);
		if (err && err != -ENOENT)
			goto out_old;
	}

	if (new_inode) {
		victim_ino = victim->i_ino;
		victim_mode = victim->i_mode;
		err = cryexts_delete_entry(new_dir, &new_dentry->d_name);
		if (err)
			goto out_new;
		if (victim_is_dir) {
			drop_nlink(new_dir);
			victim_dir_count_dropped = true;
		}
	}

	err = cryexts_add_entry(new_dir, &new_dentry->d_name, old_inode->i_ino,
				old_inode->i_mode);
	if (err)
		goto restore_target;

	if (S_ISDIR(old_inode->i_mode) && old_dir != new_dir) {
		err = cryexts_update_dir_parent(old_inode, new_parent_ino);
		if (err) {
			cryexts_delete_entry(new_dir, &new_dentry->d_name);
			goto restore_target;
		}
	}

	err = cryexts_delete_entry(old_dir, &old_dentry->d_name);
	if (err) {
		if (S_ISDIR(old_inode->i_mode) && old_dir != new_dir)
			cryexts_update_dir_parent(old_inode, old_parent_ino);
		cryexts_delete_entry(new_dir, &new_dentry->d_name);
		goto restore_target;
	}

	old_inode->i_ctime = current_time(old_inode);
	cryexts_write_inode_to_disk(old_inode);
	old_dir->i_mtime = current_time(old_dir);
	old_dir->i_ctime = old_dir->i_mtime;
	cryexts_write_inode_to_disk(old_dir);
	if (old_dir != new_dir) {
		new_dir->i_mtime = current_time(new_dir);
		new_dir->i_ctime = new_dir->i_mtime;
		cryexts_write_inode_to_disk(new_dir);
	}

	if (victim_ino) {
		drop_nlink(victim);
		victim->i_ctime = current_time(victim);
		cryexts_write_inode_to_disk(victim);
		if (!victim->i_nlink) {
			err = cryexts_orphan_set(new_dir->i_sb, victim_ino);
			if (err)
				pr_warn("cryexts: failed to orphan victim during rename\n");
			else {
				err = cryexts_release_inode_storage(victim);
				if (err)
					pr_warn("cryexts: failed to release victim blocks during rename\n");
				else if (cryexts_orphan_clear(new_dir->i_sb, victim_ino))
					pr_warn("cryexts: failed to clear victim orphan during rename\n");
			}
			cryexts_free_inode(new_dir->i_sb, victim_ino);
		}
	}

	if (S_ISDIR(old_inode->i_mode) && old_dir != new_dir) {
		drop_nlink(old_dir);
		inc_nlink(new_dir);
		cryexts_write_inode_to_disk(old_dir);
		cryexts_write_inode_to_disk(new_dir);
	}

	brelse(old_bh);
	if (new_bh)
		brelse(new_bh);
	err = cryexts_journal_commit(old_dir->i_sb);
	if (err)
		return err;
	return 0;

restore_target:
	if (victim_dir_count_dropped && victim_ino &&
	    victim_is_dir && new_inode) {
		inc_nlink(new_dir);
		cryexts_write_inode_to_disk(new_dir);
	}
	if (victim_ino &&
	    cryexts_add_entry(new_dir, &new_dentry->d_name, victim_ino,
			      victim_mode))
		pr_warn("cryexts: rename rollback failed\n");
out_new:
	if (new_bh)
		brelse(new_bh);
out_old:
	brelse(old_bh);
	cryexts_journal_abort(old_dir->i_sb);
	return err;
}

static int cryexts_link(struct dentry *old_dentry, struct inode *dir,
			struct dentry *dentry)
{
	struct inode *inode = d_inode(old_dentry);
	int err;

	err = cryexts_journal_begin(dir->i_sb);
	if (err)
		return err;

	if (S_ISDIR(inode->i_mode)) {
		cryexts_journal_abort(dir->i_sb);
		return -EPERM;
	}

	err = cryexts_add_entry(dir, &dentry->d_name, inode->i_ino,
				inode->i_mode);
	if (err) {
		cryexts_journal_abort(dir->i_sb);
		return err;
	}

	ihold(inode);
	inc_nlink(inode);
	inode->i_ctime = current_time(inode);
	err = cryexts_write_inode_to_disk(inode);
	if (err) {
		cryexts_delete_entry(dir, &dentry->d_name);
		drop_nlink(inode);
		iput(inode);
		cryexts_journal_abort(dir->i_sb);
		return err;
	}

	err = cryexts_journal_commit(dir->i_sb);
	if (err)
		return err;
	d_instantiate(dentry, inode);
	return 0;
}

static int cryexts_symlink(struct user_namespace *mnt_userns,
			   struct inode *dir, struct dentry *dentry,
			   const char *symname)
{
	struct inode *inode;
	size_t len;
	int err;

	err = cryexts_journal_begin(dir->i_sb);
	if (err)
		return err;

	len = strlen(symname);
	if (!len) {
		cryexts_journal_abort(dir->i_sb);
		return -EINVAL;
	}
	if (len > cryexts_symlink_size_limit()) {
		cryexts_journal_abort(dir->i_sb);
		return -ENAMETOOLONG;
	}

	inode = cryexts_new_inode(dir, S_IFLNK | 0777, 0);
	if (IS_ERR(inode)) {
		cryexts_journal_abort(dir->i_sb);
		return PTR_ERR(inode);
	}

	err = cryexts_write_from_kernel(inode, symname, len);
	if (err)
		goto fail;

	err = cryexts_add_entry(dir, &dentry->d_name, inode->i_ino,
				inode->i_mode);
	if (err)
		goto fail;

	err = cryexts_journal_commit(dir->i_sb);
	if (err)
		return err;
	d_instantiate_new(dentry, inode);
	return 0;

fail:
	cryexts_release_inode_storage(inode);
	cryexts_free_inode(dir->i_sb, inode->i_ino);
	clear_nlink(inode);
	discard_new_inode(inode);
	cryexts_journal_abort(dir->i_sb);
	return err;
}

const struct file_operations cryexts_dir_operations = {
	.owner = THIS_MODULE,
	.iterate_shared = cryexts_iterate,
	.llseek = generic_file_llseek,
	.read = generic_read_dir,
};

const struct inode_operations cryexts_dir_inode_operations = {
	.lookup = cryexts_lookup,
	.create = cryexts_create,
	.mkdir = cryexts_mkdir,
	.link = cryexts_link,
	.symlink = cryexts_symlink,
	.rename = cryexts_rename,
	.unlink = cryexts_unlink,
	.rmdir = cryexts_rmdir,
	.getattr = cryexts_getattr,
	.setattr = cryexts_setattr,
	.listxattr = cryexts_listxattr,
};
