// SPDX-License-Identifier: GPL-2.0
#include "cryexts.h"

struct cryexts_xattr_item {
	char name[CRYEXTS_XATTR_MAX_NAME_LEN + 1];
	u16 value_len;
	u8 *value;
};

static int cryexts_xattr_feature_enabled(struct super_block *sb)
{
	u32 incompat = le32_to_cpu(CRYEXTS_SB(sb)->disk_sb->features_incompat);

	return !!(incompat & CRYEXTS_FEATURE_INCOMPAT_XATTR);
}

static int cryexts_policy_feature_enabled(struct super_block *sb)
{
	u32 incompat = le32_to_cpu(CRYEXTS_SB(sb)->disk_sb->features_incompat);

	return !!(incompat & CRYEXTS_FEATURE_INCOMPAT_ENCRYPTION_POLICY);
}

static void cryexts_free_xattr_items(struct cryexts_xattr_item *items,
				     unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		kfree(items[i].value);
}

static struct cryexts_xattr_item *cryexts_alloc_xattr_items(void)
{
	return kcalloc(CRYEXTS_XATTR_MAX_ITEMS,
		       sizeof(struct cryexts_xattr_item),
		       GFP_KERNEL);
}

static bool cryexts_large_xattr_feature_enabled(struct super_block *sb)
{
	u32 ro_compat = le32_to_cpu(CRYEXTS_SB(sb)->disk_sb->features_ro_compat);

	return !!(ro_compat & CRYEXTS_FEATURE_RO_COMPAT_LARGE_XATTR);
}

static int cryexts_parse_xattr_block(const u8 *block,
				     struct cryexts_xattr_item *items,
				     unsigned int base,
				     unsigned int *count,
				     u64 *overflow_block)
{
	const struct cryexts_xattr_block_header *xh;
	unsigned int entries;
	unsigned int used;
	unsigned int offset;
	unsigned int i;

	xh = (const struct cryexts_xattr_block_header *)block;
	if (le32_to_cpu(xh->magic) != CRYEXTS_XATTR_MAGIC)
		return -EUCLEAN;
	entries = le16_to_cpu(xh->entries);
	used = le16_to_cpu(xh->used_bytes);
	if (base + entries > CRYEXTS_XATTR_MAX_ITEMS ||
	    used > CRYEXTS_BLOCK_SIZE - sizeof(*xh))
		return -EUCLEAN;
	if (overflow_block)
		*overflow_block = le64_to_cpu(xh->overflow_block);

	offset = sizeof(*xh);
	for (i = 0; i < entries; i++) {
		const struct cryexts_xattr_entry *xe;
		unsigned int name_len;
		unsigned int value_len;
		unsigned int total_len;

		if (offset + sizeof(*xe) > CRYEXTS_BLOCK_SIZE)
			goto bad;
		xe = (const struct cryexts_xattr_entry *)(block + offset);
		name_len = xe->name_len;
		value_len = le16_to_cpu(xe->value_len);
		total_len = sizeof(*xe) + name_len + value_len;
		if (!name_len || name_len > CRYEXTS_XATTR_MAX_NAME_LEN)
			goto bad;
		if (xe->namespace_id != CRYEXTS_XATTR_NAMESPACE_USER)
			goto bad;
		if (offset + total_len > CRYEXTS_BLOCK_SIZE)
			goto bad;
		memcpy(items[base + i].name, xe->data, name_len);
		items[base + i].name[name_len] = '\0';
		items[base + i].value_len = value_len;
		if (value_len) {
			items[base + i].value = kmemdup(xe->data + name_len,
							 value_len, GFP_KERNEL);
			if (!items[base + i].value) {
				*count = base + i;
				return -ENOMEM;
			}
		}
		offset += total_len;
	}
	if (offset != sizeof(*xh) + used)
		goto bad;
	*count = base + entries;
	return 0;

bad:
	*count = base + i;
	return -EUCLEAN;
}

static int cryexts_load_xattrs(struct inode *inode,
			       struct cryexts_xattr_item *items,
			       unsigned int *count)
{
	struct super_block *sb = inode->i_sb;
	u64 block = cryexts_inode_xattr_block(inode);
	u64 overflow_block = 0;
	u64 secondary_overflow = 0;
	u8 *buf;
	int err;

	*count = 0;
	if (!block)
		return 0;

	buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	err = cryexts_read_file_block(sb, block, buf);
	if (err)
		goto out;
	err = cryexts_parse_xattr_block(buf, items, 0, count, &overflow_block);
	if (err || !overflow_block)
		goto out;

	if (!cryexts_large_xattr_feature_enabled(sb)) {
		err = -EUCLEAN;
		goto out;
	}

	err = cryexts_read_file_block(sb, overflow_block, buf);
	if (err)
		goto out;
	err = cryexts_parse_xattr_block(buf, items, *count, count,
					    &secondary_overflow);
	if (!err && secondary_overflow)
		err = -EUCLEAN;
out:
	kfree(buf);
	return err;
}

static unsigned int cryexts_xattr_item_size(const struct cryexts_xattr_item *item)
{
	return sizeof(struct cryexts_xattr_entry) + strlen(item->name) +
	       item->value_len;
}

static int cryexts_pack_xattr_block(u8 *buf, struct cryexts_xattr_item *items,
				    unsigned int start, unsigned int entries,
				    u64 overflow_block)
{
	struct cryexts_xattr_block_header *xh;
	unsigned int offset;
	unsigned int i;

	memset(buf, 0, CRYEXTS_BLOCK_SIZE);
	xh = (struct cryexts_xattr_block_header *)buf;
	xh->magic = cpu_to_le32(CRYEXTS_XATTR_MAGIC);
	xh->entries = cpu_to_le16(entries);
	xh->overflow_block = cpu_to_le64(overflow_block);

	offset = sizeof(*xh);
	for (i = 0; i < entries; i++) {
		struct cryexts_xattr_entry *xe;
		unsigned int name_len = strlen(items[start + i].name);
		unsigned int total_len =
			sizeof(*xe) + name_len + items[start + i].value_len;

		if (offset + total_len > CRYEXTS_BLOCK_SIZE)
			return -ENOSPC;
		xe = (struct cryexts_xattr_entry *)(buf + offset);
		xe->name_len = name_len;
		xe->namespace_id = CRYEXTS_XATTR_NAMESPACE_USER;
		xe->value_len = cpu_to_le16(items[start + i].value_len);
		memcpy(xe->data, items[start + i].name, name_len);
		if (items[start + i].value_len)
			memcpy(xe->data + name_len, items[start + i].value,
			       items[start + i].value_len);
		offset += total_len;
	}

	xh->used_bytes = cpu_to_le16(offset - sizeof(*xh));
	return 0;
}

static int cryexts_write_xattrs(struct inode *inode,
				struct cryexts_xattr_item *items,
				unsigned int count)
{
	struct super_block *sb = inode->i_sb;
	u8 *root_buf;
	u8 *overflow_buf = NULL;
	u64 root_block = cryexts_inode_xattr_block(inode);
	u64 overflow_block = 0;
	u64 old_overflow_block = 0;
	bool root_new = false;
	bool overflow_new = false;
	unsigned int root_entries = 0;
	unsigned int overflow_entries = 0;
	unsigned int used;
	unsigned int i;
	int err;

	if (!count) {
		return cryexts_free_xattr_storage(inode);
	}

	if (!root_block) {
		err = cryexts_alloc_block(sb, &root_block);
		if (err)
			return err;
		root_new = true;
	}

	root_buf = kzalloc(CRYEXTS_BLOCK_SIZE, GFP_KERNEL);
	if (!root_buf) {
		if (root_new)
			cryexts_free_block(sb, root_block);
		return -ENOMEM;
	}

	if (!root_new) {
		err = cryexts_read_file_block(sb, root_block, root_buf);
		if (err)
			goto out_free;
		old_overflow_block = le64_to_cpu(
			((struct cryexts_xattr_block_header *)root_buf)->overflow_block);
		overflow_block = old_overflow_block;
	}

	for (i = 0; i < count; i++) {
		unsigned int item_size = cryexts_xattr_item_size(&items[i]);

		if (item_size > CRYEXTS_BLOCK_SIZE - sizeof(struct cryexts_xattr_block_header)) {
			err = -ENOSPC;
			goto out_free;
		}
	}

	used = sizeof(struct cryexts_xattr_block_header);
	for (i = 0; i < count; i++) {
		unsigned int item_size = cryexts_xattr_item_size(&items[i]);

		if (used + item_size > CRYEXTS_BLOCK_SIZE)
			break;
		used += item_size;
		root_entries++;
	}
	overflow_entries = count - root_entries;
	if (overflow_entries) {
		if (!cryexts_large_xattr_feature_enabled(sb)) {
			err = -ENOSPC;
			goto out_free;
		}
		if (!overflow_block) {
			err = cryexts_alloc_block(sb, &overflow_block);
			if (err)
				goto out_free;
			overflow_new = true;
		}
		overflow_buf = kzalloc(CRYEXTS_BLOCK_SIZE, GFP_KERNEL);
		if (!overflow_buf) {
			err = -ENOMEM;
			goto out_free;
		}
		err = cryexts_pack_xattr_block(overflow_buf, items, root_entries,
					       overflow_entries, 0);
		if (err)
			goto out_free;
	}

	err = cryexts_pack_xattr_block(root_buf, items, 0, root_entries,
				       overflow_entries ? overflow_block : 0);
	if (err)
		goto out_free;

	if (overflow_entries) {
		err = cryexts_journal_record_block(sb, overflow_block);
		if (err)
			goto out_free;
		err = cryexts_write_file_block(sb, overflow_block, overflow_buf);
		if (err)
			goto out_free;
	}

	err = cryexts_journal_record_block(sb, root_block);
	if (err)
		goto out_free;
	err = cryexts_write_file_block(sb, root_block, root_buf);
	if (err)
		goto out_free;

	cryexts_inode_blocks(inode)->xattr_block = root_block;
	inode->i_ctime = current_time(inode);
	err = cryexts_write_inode_to_disk(inode);
	if (!err && !overflow_entries && old_overflow_block)
		err = cryexts_free_block(sb, old_overflow_block);
	kfree(root_buf);
	kfree(overflow_buf);
	return err;

out_free:
	kfree(root_buf);
	kfree(overflow_buf);
	if (root_new)
		cryexts_free_block(sb, root_block);
	if (overflow_new)
		cryexts_free_block(sb, overflow_block);
	return err;
}

static int cryexts_find_xattr_item(struct cryexts_xattr_item *items,
				   unsigned int count, const char *name)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		if (!strcmp(items[i].name, name))
			return i;
	}
	return -1;
}

static int cryexts_policy_xattr_get(struct inode *inode, void *buffer, size_t size)
{
	char tmp[32];
	int len;

	len = scnprintf(tmp, sizeof(tmp), "%u", cryexts_inode_policy_id(inode));
	if (!buffer)
		return len;
	if (size < len)
		return -ERANGE;
	memcpy(buffer, tmp, len);
	return len;
}

static int cryexts_policy_xattr_set(struct inode *inode, const void *value,
				    size_t size)
{
	char tmp[32];
	u32 policy = 0;
	u32 current_policy = cryexts_inode_policy_id(inode);
	int err;

	if (value) {
		if (!size || size >= sizeof(tmp))
			return -EINVAL;
		memcpy(tmp, value, size);
		tmp[size] = '\0';
		err = kstrtou32(tmp, 10, &policy);
		if (err)
			return err;
	}
	if (policy == current_policy)
		return 0;
	if (cryexts_policy_table_enabled(inode->i_sb) &&
	    !cryexts_policy_exists(inode->i_sb, policy))
		return -EINVAL;
	if ((S_ISREG(inode->i_mode) || S_ISLNK(inode->i_mode)) &&
	    (i_size_read(inode) > 0 || cryexts_inode_block_count(inode) > 0))
		return -EBUSY;
	err = cryexts_set_inode_policy_id(inode, policy);
	if (err)
		return err;
	inode->i_ctime = current_time(inode);
	return cryexts_write_inode_to_disk(inode);
}

static int cryexts_user_xattr_get(const struct xattr_handler *handler,
				  struct dentry *unused, struct inode *inode,
				  const char *name, void *buffer, size_t size)
{
	struct cryexts_xattr_item *items;
	unsigned int count = 0;
	int idx;
	int err;

	items = cryexts_alloc_xattr_items();
	if (!items)
		return -ENOMEM;

	if (!name || !*name)
		goto out_inval;
	if (!cryexts_xattr_feature_enabled(inode->i_sb) &&
	    strcmp(name, CRYEXTS_XATTR_POLICY_NAME))
		goto out_eopnotsupp;
	if (!strcmp(name, CRYEXTS_XATTR_POLICY_NAME)) {
		if (!cryexts_policy_feature_enabled(inode->i_sb))
			goto out_eopnotsupp;
		err = cryexts_policy_xattr_get(inode, buffer, size);
		goto out_free;
	}

	err = cryexts_load_xattrs(inode, items, &count);
	if (err)
		goto out_free;
	idx = cryexts_find_xattr_item(items, count, name);
	if (idx < 0) {
		cryexts_free_xattr_items(items, count);
		err = -ENODATA;
		goto out_free_items_only;
	}
	if (!buffer)
		err = items[idx].value_len;
	else if (size < items[idx].value_len)
		err = -ERANGE;
	else {
		memcpy(buffer, items[idx].value, items[idx].value_len);
		err = items[idx].value_len;
	}
	cryexts_free_xattr_items(items, count);
	goto out_free_items_only;

out_inval:
	err = -EINVAL;
	goto out_free;
out_eopnotsupp:
	err = -EOPNOTSUPP;
out_free:
	kfree(items);
	return err;
out_free_items_only:
	kfree(items);
	return err;
}

static int cryexts_user_xattr_set(const struct xattr_handler *handler,
				  struct user_namespace *mnt_userns,
				  struct dentry *unused, struct inode *inode,
				  const char *name, const void *value,
				  size_t size, int flags)
{
	struct cryexts_xattr_item *items;
	unsigned int count = 0;
	int idx;
	int err;

	items = cryexts_alloc_xattr_items();
	if (!items)
		return -ENOMEM;

	if (!name || !*name)
		goto out_inval;
	if (!strcmp(name, CRYEXTS_XATTR_POLICY_NAME)) {
		if (!cryexts_policy_feature_enabled(inode->i_sb))
			goto out_eopnotsupp;
		err = cryexts_journal_begin(inode->i_sb);
		if (err)
			goto out_free;
		err = cryexts_policy_xattr_set(inode, value, size);
		if (err) {
			cryexts_journal_abort(inode->i_sb);
			goto out_free;
		}
		err = cryexts_journal_commit(inode->i_sb);
		goto out_free;
	}
	if (!cryexts_xattr_feature_enabled(inode->i_sb))
		goto out_eopnotsupp;
	if (strlen(name) > CRYEXTS_XATTR_MAX_NAME_LEN)
		goto out_erange;

	err = cryexts_load_xattrs(inode, items, &count);
	if (err)
		goto out_free;
	idx = cryexts_find_xattr_item(items, count, name);
	if ((flags & XATTR_CREATE) && idx >= 0) {
		err = -EEXIST;
		goto out;
	}
	if ((flags & XATTR_REPLACE) && idx < 0) {
		err = -ENODATA;
		goto out;
	}

	if (!value) {
		if (idx < 0) {
			err = -ENODATA;
			goto out;
		}
		kfree(items[idx].value);
		memmove(&items[idx], &items[idx + 1],
			(count - idx - 1) * sizeof(items[0]));
		count--;
	} else if (idx >= 0) {
		u8 *new_value = NULL;

		if (size) {
			new_value = kmemdup(value, size, GFP_KERNEL);
			if (!new_value) {
				err = -ENOMEM;
				goto out;
			}
		}
		kfree(items[idx].value);
		items[idx].value = new_value;
		items[idx].value_len = size;
	} else {
		if (count >= CRYEXTS_XATTR_MAX_ITEMS) {
			err = -ENOSPC;
			goto out;
		}
		strscpy(items[count].name, name, sizeof(items[count].name));
		items[count].value_len = size;
		if (size) {
			items[count].value = kmemdup(value, size, GFP_KERNEL);
			if (!items[count].value) {
				err = -ENOMEM;
				goto out;
			}
		}
		count++;
	}

	err = cryexts_journal_begin(inode->i_sb);
	if (err)
		goto out;
	err = cryexts_write_xattrs(inode, items, count);
	if (err) {
		cryexts_journal_abort(inode->i_sb);
		goto out;
	}
	err = cryexts_journal_commit(inode->i_sb);

out:
	cryexts_free_xattr_items(items, count);
	kfree(items);
	return err;
out_inval:
	err = -EINVAL;
	goto out_free;
out_eopnotsupp:
	err = -EOPNOTSUPP;
	goto out_free;
out_erange:
	err = -ERANGE;
out_free:
	kfree(items);
	return err;
}

ssize_t cryexts_listxattr(struct dentry *dentry, char *buffer, size_t size)
{
	struct inode *inode = d_inode(dentry);
	struct cryexts_xattr_item *items;
	unsigned int count = 0;
	size_t total = 0;
	unsigned int i;
	int err;

	items = cryexts_alloc_xattr_items();
	if (!items)
		return -ENOMEM;

	if (cryexts_policy_feature_enabled(inode->i_sb))
		total += sizeof(XATTR_USER_PREFIX CRYEXTS_XATTR_POLICY_NAME);

	if (!cryexts_xattr_feature_enabled(inode->i_sb)) {
		if (!buffer)
			goto out_no_xattr;
		if (size < total)
			goto out_erange;
		if (cryexts_policy_feature_enabled(inode->i_sb))
			memcpy(buffer, XATTR_USER_PREFIX CRYEXTS_XATTR_POLICY_NAME,
			       sizeof(XATTR_USER_PREFIX CRYEXTS_XATTR_POLICY_NAME));
		goto out_no_xattr;
	}

	err = cryexts_load_xattrs(inode, items, &count);
	if (err)
		goto out_free;
	for (i = 0; i < count; i++)
		total += strlen(XATTR_USER_PREFIX) + strlen(items[i].name) + 1;

	if (!buffer) {
		cryexts_free_xattr_items(items, count);
		goto out_free_total;
	}
	if (size < total) {
		cryexts_free_xattr_items(items, count);
		goto out_erange_free_items;
	}

	total = 0;
	if (cryexts_policy_feature_enabled(inode->i_sb)) {
		memcpy(buffer + total, XATTR_USER_PREFIX CRYEXTS_XATTR_POLICY_NAME,
		       sizeof(XATTR_USER_PREFIX CRYEXTS_XATTR_POLICY_NAME));
		total += sizeof(XATTR_USER_PREFIX CRYEXTS_XATTR_POLICY_NAME);
	}
	for (i = 0; i < count; i++) {
		size_t prefix_len = strlen(XATTR_USER_PREFIX);
		size_t name_len = strlen(items[i].name);

		memcpy(buffer + total, XATTR_USER_PREFIX, prefix_len);
		total += prefix_len;
		memcpy(buffer + total, items[i].name, name_len);
		total += name_len;
		buffer[total++] = '\0';
	}

	cryexts_free_xattr_items(items, count);
	goto out_free_total;

out_no_xattr:
	err = total;
	goto out_free;
out_erange_free_items:
	err = -ERANGE;
	goto out_free;
out_free_total:
	err = total;
out_free:
	kfree(items);
	return err;
out_erange:
	err = -ERANGE;
	kfree(items);
	return err;
}

int cryexts_free_xattr_storage(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	u64 block = cryexts_inode_xattr_block(inode);
	u64 overflow_block = 0;
	u8 *buf;
	int err;

	if (!block)
		return 0;

	buf = kmalloc(CRYEXTS_BLOCK_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	err = cryexts_read_file_block(sb, block, buf);
	if (!err)
		overflow_block = le64_to_cpu(
			((struct cryexts_xattr_block_header *)buf)->overflow_block);
	kfree(buf);
	if (err)
		return err;

	if (overflow_block) {
		err = cryexts_free_block(sb, overflow_block);
		if (err)
			return err;
	}
	err = cryexts_free_block(sb, block);
	if (err)
		return err;
	cryexts_inode_blocks(inode)->xattr_block = 0;
	return 0;
}

static const struct xattr_handler cryexts_xattr_user_handler = {
	.prefix = XATTR_USER_PREFIX,
	.get = cryexts_user_xattr_get,
	.set = cryexts_user_xattr_set,
};

const struct xattr_handler *cryexts_xattr_handlers[] = {
	&cryexts_xattr_user_handler,
	NULL,
};
