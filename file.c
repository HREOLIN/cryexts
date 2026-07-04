// SPDX-License-Identifier: GPL-2.0
#include "cryexts.h"
#include <linux/falloc.h>
#include <linux/limits.h>

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
	struct inode *inode = file_inode(iocb->ki_filp);
	char *block_buf;
	loff_t pos = iocb->ki_pos;
	size_t total = 0;
	size_t remaining;
	int err;
	u64 physical;

	if (!cryexts_inode_blocks(inode) || pos >= i_size_read(inode))
		return 0;

	remaining = min_t(size_t, iov_iter_count(to), i_size_read(inode) - pos);
	block_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_KERNEL);
	if (!block_buf)
		return -ENOMEM;

	while (remaining > 0) {
		u64 logical = pos / CRYEXTS_BLOCK_SIZE;
		size_t block_off = pos % CRYEXTS_BLOCK_SIZE;
		size_t chunk = min_t(size_t, remaining,
				     CRYEXTS_BLOCK_SIZE - block_off);
		size_t copied;

		err = cryexts_resolve_block(inode, logical, false, &physical);
		if (err) {
			kfree(block_buf);
			return err;
		}

		if (physical) {
			err = cryexts_read_inode_block(inode, physical, block_buf);
			if (err) {
				kfree(block_buf);
				return err;
			}
		} else {
			memset(block_buf, 0, CRYEXTS_BLOCK_SIZE);
		}

		copied = copy_to_iter(block_buf + block_off, chunk, to);
		total += copied;
		pos += copied;
		remaining -= copied;
		if (copied != chunk)
			break;
	}

	kfree(block_buf);
	iocb->ki_pos = pos;
	return total;
}

ssize_t cryexts_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	char *block_buf;
	struct timespec64 now;
	loff_t pos = iocb->ki_pos;
	size_t total = 0;
	size_t remaining;
	int err = 0;
	bool txn_started = false;

	if (!cryexts_inode_blocks(inode))
		return -EIO;
	if (pos >= cryexts_regular_file_max_size_for_inode(inode))
		return -EFBIG;

	block_buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_KERNEL);
	if (!block_buf)
		return -ENOMEM;

	inode_lock(inode);
	err = cryexts_journal_begin(inode->i_sb);
	if (err)
		goto out_unlock;
	txn_started = true;
	remaining = min_t(size_t, iov_iter_count(from),
			  cryexts_regular_file_max_size_for_inode(inode) - pos);
	while (remaining > 0) {
		u64 logical = pos / CRYEXTS_BLOCK_SIZE;
		u64 physical;
		size_t block_off = pos % CRYEXTS_BLOCK_SIZE;
		size_t chunk = min_t(size_t, remaining,
				     CRYEXTS_BLOCK_SIZE - block_off);
		size_t copied;

		err = cryexts_resolve_block(inode, logical, true, &physical);
		if (err)
			goto out_unlock;

		if (block_off || chunk < CRYEXTS_BLOCK_SIZE) {
			err = cryexts_read_inode_block(inode, physical,
						       block_buf);
			if (err)
				goto out_unlock;
		} else
			memset(block_buf, 0, CRYEXTS_BLOCK_SIZE);

		copied = copy_from_iter(block_buf + block_off, chunk, from);
		if (!copied) {
			if (!total) {
				err = -EFAULT;
				goto out_unlock;
			}
			break;
		}
		err = cryexts_write_inode_block(inode, physical, block_buf);
		if (err)
			goto out_unlock;

		total += copied;
		pos += copied;
		remaining -= copied;
		if (copied != chunk)
			break;
	}

	iocb->ki_pos = pos;
	if (pos > i_size_read(inode))
		i_size_write(inode, pos);
	inode->i_blocks = cryexts_inode_block_sectors(inode);
	now = current_time(inode);
	inode->i_mtime = now;
	inode->i_ctime = now;
	err = cryexts_write_inode_to_disk(inode);
	if (err)
		goto out_unlock;
	err = cryexts_journal_commit(inode->i_sb);
	if (err) {
		txn_started = false;
		goto out_unlock;
	}
	txn_started = false;
	inode_unlock(inode);
	kfree(block_buf);
	return total;

out_unlock:
	if (txn_started)
		cryexts_journal_abort(inode->i_sb);
	inode_unlock(inode);
	kfree(block_buf);
	return err;
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
	inode_unlock(inode);
	return 0;

out_abort:
	cryexts_journal_abort(inode->i_sb);
	txn_started = false;
out_unlock:
	if (txn_started)
		cryexts_journal_abort(inode->i_sb);
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
