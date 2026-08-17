// SPDX-License-Identifier: GPL-2.0
#include "cryexts.h"
#include <linux/falloc.h>
#include <linux/highmem.h>
#include <linux/limits.h>
#include <linux/pagemap.h>
#include <linux/writeback.h>

static int cryexts_zero_file_range(struct inode *inode, u64 block,
				   size_t offset, size_t len)
{
	char *block_buf;
	int err;

	if (!len)
		return 0;

	block_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_KERNEL);
	if (!block_buf)
		return -ENOMEM;

	err = cryexts_read_inode_block(inode, block, block_buf);
	if (err)
		goto out;

	memset(block_buf + offset, 0, len);
	err = cryexts_write_inode_block(inode, block, block_buf);

out:
	kfree(block_buf);
	return err;
}

static int cryexts_read_inode_data(struct inode *inode, char *buf, size_t len)
{
	size_t done = 0;
	char *block_buf;
	int err = 0;

	block_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_KERNEL);
	if (!block_buf)
		return -ENOMEM;

	while (done < len) {
		u64 logical = done / CRYEXTS_BLOCK_SIZE;
		u64 physical;
		size_t block_off = done % CRYEXTS_BLOCK_SIZE;
		size_t chunk = min_t(size_t, len - done,
				     CRYEXTS_BLOCK_SIZE - block_off);

		err = cryexts_resolve_block(inode, logical, false, &physical);
		if (err)
			goto out;
		if (!physical) {
			err = -EIO;
			goto out;
		}
		err = cryexts_read_inode_block(inode, physical, block_buf);
		if (err)
			goto out;
		memcpy(buf + done, block_buf + block_off, chunk);
		done += chunk;
	}

out:
	kfree(block_buf);
	return err;
}

static void cryexts_invalidate_cache_range(struct inode *inode, loff_t start,
					   loff_t end)
{
	pgoff_t start_index;
	pgoff_t end_index;

	if (!inode || end <= start)
		return;

	start_index = start >> PAGE_SHIFT;
	end_index = (end - 1) >> PAGE_SHIFT;
	invalidate_inode_pages2_range(inode->i_mapping, start_index, end_index);
}

static int cryexts_fill_page(struct page *page)
{
	struct inode *inode = page->mapping->host;
	char *page_buf;
	char *block_buf;
	loff_t pos = page_offset(page);
	size_t remaining = 0;
	size_t done = 0;
	int err = 0;

	if (!cryexts_inode_blocks(inode))
		return -EIO;

	if (pos < i_size_read(inode))
		remaining = min_t(loff_t, PAGE_SIZE, i_size_read(inode) - pos);

	block_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_KERNEL);
	if (!block_buf)
		return -ENOMEM;

	page_buf = kmap_local_page(page);
	while (done < remaining) {
		u64 logical = (pos + done) / CRYEXTS_BLOCK_SIZE;
		u64 physical;
		size_t block_off = (pos + done) % CRYEXTS_BLOCK_SIZE;
		size_t chunk = min_t(size_t, remaining - done,
				     CRYEXTS_BLOCK_SIZE - block_off);

		err = cryexts_resolve_block(inode, logical, false, &physical);
		if (err)
			goto out_unmap_error;

		if (physical) {
			err = cryexts_read_inode_block(inode, physical, block_buf);
			if (err)
				goto out_unmap_error;
			memcpy(page_buf + done, block_buf + block_off, chunk);
		} else {
			memset(page_buf + done, 0, chunk);
		}
		done += chunk;
	}
	if (done < PAGE_SIZE)
		memset(page_buf + done, 0, PAGE_SIZE - done);

	kunmap_local(page_buf);
	kfree(block_buf);
	flush_dcache_page(page);
	SetPageUptodate(page);
	return 0;

out_unmap_error:
	if (done < PAGE_SIZE)
		memset(page_buf + done, 0, PAGE_SIZE - done);
	kunmap_local(page_buf);
	kfree(block_buf);
	return err;
}

static int cryexts_readpage(struct file *file, struct page *page)
{
	int err;

	(void)file;
	err = cryexts_fill_page(page);
	if (err) {
		ClearPageUptodate(page);
		SetPageError(page);
	}
	unlock_page(page);
	return err;
}

static int cryexts_write_begin(struct file *file,
			       struct address_space *mapping, loff_t pos,
			       unsigned int len, unsigned int flags,
			       struct page **pagep, void **fsdata)
{
	struct inode *inode = mapping->host;
	struct page *page;
	int err;

	(void)file;
	*fsdata = NULL;
	if (!len || !cryexts_inode_blocks(inode))
		return -EIO;
	if (pos < 0)
		return -EINVAL;
	if (PAGE_SIZE != CRYEXTS_BLOCK_SIZE)
		return -EOPNOTSUPP;
	if (pos >= cryexts_regular_file_max_size_for_inode(inode) ||
	    len > cryexts_regular_file_max_size_for_inode(inode) - pos)
		return -EFBIG;

	page = grab_cache_page_write_begin(mapping, pos >> PAGE_SHIFT, flags);
	if (!page)
		return -ENOMEM;

	if (!PageUptodate(page)) {
		err = cryexts_fill_page(page);
		if (err)
			goto out_page;
	}

	*pagep = page;
	return 0;

out_page:
	unlock_page(page);
	put_page(page);
	return err;
}

static int cryexts_write_end(struct file *file,
			     struct address_space *mapping, loff_t pos,
			     unsigned int len, unsigned int copied,
			     struct page *page, void *fsdata)
{
	struct inode *inode = mapping->host;
	struct timespec64 now;
	loff_t end = pos + copied;

	(void)file;
	(void)len;
	(void)fsdata;

	if (copied && end > i_size_read(inode))
		i_size_write(inode, end);
	if (copied) {
		now = current_time(inode);
		inode->i_mtime = now;
		inode->i_ctime = now;
		SetPageUptodate(page);
		set_page_dirty(page);
	}
	unlock_page(page);
	put_page(page);
	return copied;
}

static int cryexts_writepage_locked(struct page *page,
				    struct writeback_control *wbc)
{
	struct inode *inode = page->mapping->host;
	struct address_space *mapping = page->mapping;
	char *page_buf;
	loff_t page_start = page_offset(page);
	loff_t size = i_size_read(inode);
	size_t bytes;
	u64 first_logical;
	u64 last_logical;
	u64 logical;
	bool txn_started = false;
	int err;

	if (page_start >= size) {
		unlock_page(page);
		return 0;
	}
	if (PAGE_SIZE != CRYEXTS_BLOCK_SIZE) {
		err = -EOPNOTSUPP;
		goto out_redirty;
	}

	bytes = min_t(loff_t, PAGE_SIZE, size - page_start);
	set_page_writeback(page);
	err = cryexts_journal_begin(inode->i_sb);
	if (err)
		goto out_end_writeback;
	txn_started = true;

	flush_dcache_page(page);
	page_buf = kmap_local_page(page);
	first_logical = page_start / CRYEXTS_BLOCK_SIZE;
	last_logical = (page_start + bytes - 1) / CRYEXTS_BLOCK_SIZE;
	for (logical = first_logical; logical <= last_logical; logical++) {
		u64 block_start = logical * CRYEXTS_BLOCK_SIZE;
		u64 physical;
		size_t page_off = (size_t)(block_start - (u64)page_start);

		err = cryexts_resolve_block(inode, logical, true, &physical);
		if (err)
			goto out_unmap;
		err = cryexts_write_inode_block(inode, physical,
						 page_buf + page_off);
		if (err)
			goto out_unmap;
		err = cryexts_sync_inode_block(inode, physical);
		if (err)
			goto out_unmap;
	}
	kunmap_local(page_buf);

	inode->i_blocks = cryexts_inode_block_sectors(inode);
	err = cryexts_write_inode_to_disk(inode);
	if (err)
		goto out_abort;
	err = cryexts_journal_commit(inode->i_sb);
	txn_started = false;
	if (err)
		goto out_end_writeback;

	ClearPageError(page);
	unlock_page(page);
	end_page_writeback(page);
	return 0;

out_unmap:
	kunmap_local(page_buf);
out_abort:
	if (txn_started)
		cryexts_journal_abort(inode->i_sb);
out_end_writeback:
	mapping_set_error(mapping, err);
	redirty_page_for_writepage(wbc, page);
	SetPageError(page);
	unlock_page(page);
	end_page_writeback(page);
	return err;

out_redirty:
	mapping_set_error(mapping, err);
	redirty_page_for_writepage(wbc, page);
	SetPageError(page);
	unlock_page(page);
	return err;
}

static int cryexts_writepage(struct page *page,
			     struct writeback_control *wbc)
{
	return cryexts_writepage_locked(page, wbc);
}

static int cryexts_writepages_callback(struct page *page,
				       struct writeback_control *wbc,
				       void *data)
{
	(void)data;
	return cryexts_writepage_locked(page, wbc);
}

static int cryexts_writepages(struct address_space *mapping,
			      struct writeback_control *wbc)
{
	return write_cache_pages(mapping, wbc, cryexts_writepages_callback,
				 NULL);
}

const struct address_space_operations cryexts_file_aops = {
	.readpage = cryexts_readpage,
	.writepage = cryexts_writepage,
	.writepages = cryexts_writepages,
	.write_begin = cryexts_write_begin,
	.write_end = cryexts_write_end,
	.set_page_dirty = __set_page_dirty_nobuffers,
};

const char *cryexts_get_link(struct dentry *dentry, struct inode *inode,
			     struct delayed_call *done)
{
	char *target;
	size_t len;
	int err;

	if (!inode)
		return ERR_PTR(-ECHILD);
	len = i_size_read(inode);
	if (!len || len > cryexts_symlink_size_limit())
		return ERR_PTR(-EUCLEAN);

	target = kmalloc(len + 1, GFP_KERNEL);
	if (!target)
		return ERR_PTR(-ENOMEM);

	err = cryexts_read_inode_data(inode, target, len);
	if (err) {
		kfree(target);
		return ERR_PTR(err);
	}
	target[len] = '\0';
	set_delayed_call(done, kfree_link, target);
	return target;
}

static int cryexts_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct inode *inode = file_inode(file);
	int err;

	err = file_write_and_wait_range(file, start, end);
	if (err)
		return err;

	inode_lock(inode);
	err = cryexts_write_inode_to_disk(inode);
	inode_unlock(inode);
	if (err)
		return err;

	err = cryexts_sync_metadata(inode->i_sb);
	if (err)
		return err;
	return 0;
}

ssize_t cryexts_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	return generic_file_read_iter(iocb, to);
}

ssize_t cryexts_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	return generic_file_write_iter(iocb, from);
}

static long cryexts_fallocate(struct file *file, int mode, loff_t offset,
			      loff_t len)
{
	struct inode *inode = file_inode(file);
	loff_t size;
	loff_t end;
	loff_t head_end;
	loff_t tail_start;
	u64 first_full_block;
	u64 end_full_block;
	struct timespec64 now;
	bool txn_started = false;
	int err;

	if (!S_ISREG(inode->i_mode))
		return -EOPNOTSUPP;
	if (mode != (FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE))
		return -EOPNOTSUPP;
	if (offset < 0 || len <= 0)
		return -EINVAL;
	if (offset > LLONG_MAX - len)
		return -EFBIG;
	if (!cryexts_inode_blocks(inode))
		return -EIO;

	inode_lock(inode);
	size = i_size_read(inode);
	if (offset >= size) {
		inode_unlock(inode);
		return 0;
	}

	end = min_t(loff_t, offset + len, size);
	err = file_write_and_wait_range(file, offset, end - 1);
	if (err)
		goto out_unlock;
	err = cryexts_journal_begin(inode->i_sb);
	if (err)
		goto out_unlock;
	txn_started = true;

	head_end = min_t(loff_t, end,
			 ALIGN(offset, (loff_t)CRYEXTS_BLOCK_SIZE));
	if (offset < head_end) {
		u64 physical;

		err = cryexts_resolve_block(inode, offset / CRYEXTS_BLOCK_SIZE,
					    false, &physical);
		if (err)
			goto out_abort;
		if (physical) {
			err = cryexts_zero_file_range(
				inode, physical, offset % CRYEXTS_BLOCK_SIZE,
				head_end - offset);
			if (err)
				goto out_abort;
		}
	}

	tail_start = end & ~((loff_t)CRYEXTS_BLOCK_SIZE - 1);
	if (tail_start < end && tail_start >= head_end) {
		u64 physical;

		err = cryexts_resolve_block(inode,
					    tail_start / CRYEXTS_BLOCK_SIZE,
					    false, &physical);
		if (err)
			goto out_abort;
		if (physical) {
			err = cryexts_zero_file_range(inode, physical, 0,
						      end - tail_start);
			if (err)
				goto out_abort;
		}
	}

	first_full_block = DIV_ROUND_UP_ULL(offset, CRYEXTS_BLOCK_SIZE);
	end_full_block = end / CRYEXTS_BLOCK_SIZE;
	if (first_full_block < end_full_block) {
		err = cryexts_punch_hole_blocks(inode, first_full_block,
						end_full_block);
		if (err)
			goto out_abort;
	}

	inode->i_blocks = cryexts_inode_block_sectors(inode);
	now = current_time(inode);
	inode->i_ctime = now;
	inode->i_mtime = now;
	err = cryexts_write_inode_to_disk(inode);
	if (err)
		goto out_abort;
	err = cryexts_journal_commit(inode->i_sb);
	if (err) {
		txn_started = false;
		goto out_unlock;
	}
	txn_started = false;
	cryexts_invalidate_cache_range(inode, offset, end);
	inode_unlock(inode);
	return 0;

out_abort:
	cryexts_journal_abort(inode->i_sb);
	txn_started = false;
out_unlock:
	if (txn_started)
		cryexts_journal_abort(inode->i_sb);
	if (end > offset)
		cryexts_invalidate_cache_range(inode, offset, end);
	inode_unlock(inode);
	return err;
}

int cryexts_setattr(struct user_namespace *mnt_userns,
		    struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	int err;

	err = setattr_prepare(mnt_userns, dentry, attr);
	if (err)
		return err;

	if ((attr->ia_valid & ATTR_SIZE) &&
	    attr->ia_size > cryexts_regular_file_max_size_for_inode(inode))
		return -EFBIG;

	if ((attr->ia_valid & ATTR_SIZE) && S_ISREG(inode->i_mode)) {
		loff_t old_size = i_size_read(inode);
		loff_t new_size = attr->ia_size;
		u64 keep_blocks;
		u64 tail_block = 0;
		bool txn_started = false;

		if (!cryexts_inode_blocks(inode))
			return -EIO;

		if (new_size < old_size) {
			err = filemap_write_and_wait(inode->i_mapping);
			if (err)
				return err;
			err = cryexts_journal_begin(inode->i_sb);
			if (err)
				return err;
			txn_started = true;
			keep_blocks = new_size ?
				DIV_ROUND_UP_ULL(new_size, CRYEXTS_BLOCK_SIZE) : 0;

			err = cryexts_free_blocks_from(inode, keep_blocks);
			if (err) {
				cryexts_journal_abort(inode->i_sb);
				return err;
			}

			if (new_size && new_size % CRYEXTS_BLOCK_SIZE &&
			    keep_blocks > 0) {
				size_t tail_off = new_size % CRYEXTS_BLOCK_SIZE;

				err = cryexts_resolve_block(inode, keep_blocks - 1,
							    false, &tail_block);
				if (err) {
					cryexts_journal_abort(inode->i_sb);
					return err;
				}
				if (tail_block) {
					err = cryexts_zero_file_range(inode,
								      tail_block,
								      tail_off,
								      CRYEXTS_BLOCK_SIZE - tail_off);
					if (err) {
						cryexts_journal_abort(inode->i_sb);
						return err;
					}
				}
			}
			inode->i_blocks = cryexts_inode_block_sectors(inode);
			setattr_copy(mnt_userns, inode, attr);
			truncate_setsize(inode, attr->ia_size);
			inode->i_ctime = current_time(inode);
			err = cryexts_write_inode_to_disk(inode);
			if (err) {
				cryexts_journal_abort(inode->i_sb);
				return err;
			}
			err = cryexts_journal_commit(inode->i_sb);
			if (err)
				return err;
			return 0;
		}

		inode->i_blocks = cryexts_inode_block_sectors(inode);
	}
	setattr_copy(mnt_userns, inode, attr);
	if (attr->ia_valid & ATTR_SIZE)
		truncate_setsize(inode, attr->ia_size);

	inode->i_ctime = current_time(inode);
	return cryexts_write_inode_to_disk(inode);
}

int cryexts_getattr(struct user_namespace *mnt_userns,
		    const struct path *path, struct kstat *stat,
		    u32 request_mask, unsigned int query_flags)
{
	generic_fillattr(mnt_userns, d_inode(path->dentry), stat);
	return 0;
}

const struct file_operations cryexts_file_operations = {
	.owner = THIS_MODULE,
	.read_iter = cryexts_read_iter,
	.write_iter = cryexts_write_iter,
	.fallocate = cryexts_fallocate,
	.fsync = cryexts_fsync,
	.llseek = generic_file_llseek,
};

const struct inode_operations cryexts_file_inode_operations = {
	.getattr = cryexts_getattr,
	.setattr = cryexts_setattr,
	.listxattr = cryexts_listxattr,
};

const struct inode_operations cryexts_symlink_inode_operations = {
	.get_link = cryexts_get_link,
	.getattr = cryexts_getattr,
	.listxattr = cryexts_listxattr,
};
