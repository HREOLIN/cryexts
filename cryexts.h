/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _CRYEXTS_H
#define _CRYEXTS_H

#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/crypto.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mount.h>
#include <linux/mutex.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/string.h>
#include <linux/time.h>
#include <linux/timekeeping.h>
#include <linux/uio.h>
#include <linux/xattr.h>
#include <crypto/skcipher.h>

#include "cryexts_fs.h"

struct cryexts_sb_info {
	struct super_block *sb;
	struct buffer_head *s_sbh;
	struct buffer_head *block_bitmap_bh;
	struct buffer_head *inode_bitmap_bh;
	struct buffer_head **gdt_bhs;
	struct cryexts_super_block *disk_sb;
	unsigned char *block_bitmap;
	unsigned char *inode_bitmap;
	unsigned char *gdt_storage;
	struct cryexts_group_desc *groups;
	struct buffer_head **group_block_bitmap_bhs;
	struct buffer_head **group_inode_bitmap_bhs;
	unsigned char **group_block_bitmaps;
	unsigned char **group_inode_bitmaps;
	u64 inode_table_start;
	u64 inode_table_blocks;
	u64 block_bitmap_block;
	u64 inode_bitmap_block;
	u64 group_desc_table_start;
	u64 group_desc_table_blocks;
	u64 group_count;
	u64 blocks_per_group;
	u64 inodes_per_group;
	u64 journal_block;
	u64 journal_blocks;
	u64 next_ino;
	u64 next_data_block;
	u64 journal_sequence;
	u64 journal_last_sequence;
	u64 journal_active_sequence;
	u64 journal_tail_sequence;
	u64 journal_checkpoint_sequence;
	bool encrypted;
	bool journal_enabled;
	bool journal_v2;
	bool journal_replaying;
	u32 key_verifier;
	u32 encryption_flags;
	u32 encryption_kdf;
	u32 encryption_alg;
	u8 salt[CRYEXTS_SALT_LEN];
	u8 derived_key[CRYEXTS_DERIVED_KEY_LEN];
	u32 derived_key_len;
	struct cryexts_policy_runtime *policies;
	u16 policy_count;
	unsigned int journal_entry_count;
	u64 journal_home_blocks[CRYEXTS_JOURNAL_MAX_ENTRIES];
	struct crypto_skcipher *skcipher;
	struct mutex alloc_lock;
	struct mutex journal_lock;
};

struct cryexts_policy_runtime {
	u32 policy_id;
	u32 flags;
	u8 context[CRYEXTS_POLICY_CONTEXT_LEN];
	u8 derived_key[CRYEXTS_DERIVED_KEY_LEN];
	struct crypto_skcipher *skcipher;
};

struct cryexts_extent_leaf_cache {
	u64 block;
	u16 entries;
	u32 checksum;
	struct cryexts_extent *extents;
};

struct cryexts_inode_info {
	bool use_extents;
	u32 inode_flags;
	u64 direct[CRYEXTS_DIRECT_BLOCKS];
	u64 indirect_block;
	u16 extent_entries;
	u16 extent_inline_max;
	u16 extent_overflow_entries;
	struct cryexts_extent extents[CRYEXTS_MAX_INLINE_EXTENTS];
	struct cryexts_extent *overflow_extents;
	u64 extent_overflow_block;
	u32 extent_overflow_checksum;
	u16 extent_leaf_count;
	struct cryexts_extent_root_ref extent_root_refs[CRYEXTS_EXTENT_TREE_ROOT_REFS];
	struct cryexts_extent_leaf_cache extent_leaves[CRYEXTS_EXTENT_TREE_ROOT_REFS];
	u64 xattr_block;
	u32 encryption_policy_id;
	u64 next_orphan;
	u64 dir_index_block;
	u64 alloc_hint_block;
	u64 alloc_goal_group;
	u64 reservation_start;
	u64 reservation_next;
	u64 reservation_end;
};

static inline struct cryexts_sb_info *CRYEXTS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

struct cryexts_inode_info *cryexts_inode_blocks(struct inode *inode);
void cryexts_free_inode_blocks(struct inode *inode);
int cryexts_init_inode_blocks(struct inode *inode,
			      struct cryexts_inode *disk_inode);
u64 cryexts_inode_first_block(struct inode *inode);
unsigned int cryexts_disk_inode_block_count(struct super_block *sb,
					    struct cryexts_inode *disk_inode);
unsigned int cryexts_inode_block_count(struct inode *inode);
unsigned int cryexts_inode_block_sectors(struct inode *inode);
u64 cryexts_regular_file_max_size(void);
u64 cryexts_regular_file_max_size_for_inode(struct inode *inode);
unsigned int cryexts_dir_block_count(struct inode *inode);
u64 cryexts_inode_block_at(struct inode *inode, unsigned int index);
u64 cryexts_disk_inode_indirect_block(struct cryexts_inode *disk_inode);
u64 cryexts_inode_indirect_block(struct inode *inode);
bool cryexts_inode_uses_extents(struct inode *inode);
bool cryexts_disk_inode_uses_extents(struct cryexts_inode *disk_inode);
u64 cryexts_inode_xattr_block(struct inode *inode);
u32 cryexts_inode_policy_id(struct inode *inode);
int cryexts_set_inode_policy_id(struct inode *inode, u32 policy_id);
void cryexts_set_inode_alloc_hint(struct inode *inode, u64 block);
int cryexts_resolve_block(struct inode *inode, u64 logical, bool create,
			  u64 *block);
int cryexts_free_blocks_from(struct inode *inode, u64 keep_blocks);
int cryexts_punch_hole_blocks(struct inode *inode, u64 first_block,
			      u64 end_block);
unsigned int cryexts_inodes_per_block(void);
u64 cryexts_max_inodes(struct super_block *sb);
u64 cryexts_blocks_count(struct super_block *sb);
u64 cryexts_inodes_count(struct super_block *sb);
u64 cryexts_group_first_block(struct super_block *sb, u64 group);
u64 cryexts_group_blocks(struct super_block *sb, u64 group);
u64 cryexts_group_inode_table_start(struct super_block *sb, u64 group);
u32 cryexts_group_inode_table_blocks(struct super_block *sb, u64 group);
u32 cryexts_group_free_blocks(struct super_block *sb, u64 group);
u32 cryexts_group_free_inodes(struct super_block *sb, u64 group);
bool cryexts_has_block_groups(struct super_block *sb);
bool cryexts_prealloc_feature_enabled(struct super_block *sb);
bool cryexts_inode_bitmap_used(struct super_block *sb, u64 ino);
bool cryexts_block_bitmap_used(struct super_block *sb, u64 block);
bool cryexts_bitmap_test(const unsigned char *bitmap, u64 bit);
void cryexts_bitmap_set(unsigned char *bitmap, u64 bit);
void cryexts_bitmap_clear(unsigned char *bitmap, u64 bit);
void cryexts_mark_bitmap_dirty(struct cryexts_sb_info *sbi);
void cryexts_mark_super_dirty(struct super_block *sb);
bool cryexts_data_block_valid(struct super_block *sb, u64 block);
bool cryexts_mode_supported(umode_t mode);
bool cryexts_salt_is_zero(const u8 *salt);
size_t cryexts_symlink_size_limit(void);
bool cryexts_metadata_csum_enabled(struct super_block *sb);
int cryexts_load_group_desc_table(struct super_block *sb);
void cryexts_release_group_desc_table(struct cryexts_sb_info *sbi);
void cryexts_gdt_prepare_write(struct super_block *sb);
void cryexts_update_super_checksum(struct super_block *sb);
int cryexts_verify_super_checksum(struct super_block *sb);
void cryexts_update_group_checksums(struct super_block *sb);
int cryexts_verify_group_checksums(struct super_block *sb);
void cryexts_dir_index_set_checksum(struct super_block *sb, u64 block,
				    struct cryexts_dir_index_block *index);
bool cryexts_dir_index_checksum_valid(struct super_block *sb, u64 block,
				      const struct cryexts_dir_index_block *index);
bool cryexts_policy_table_checksum_valid(
	struct super_block *sb, u64 block,
	const struct cryexts_policy_table_block *pt);
u32 cryexts_extent_overflow_checksum(struct super_block *sb, u64 block,
				     const void *buf);
u32 cryexts_extent_leaf_checksum(struct super_block *sb, u64 block,
				 const void *buf);

int cryexts_set_encryption_key(struct cryexts_sb_info *sbi,
			       const char *options);
bool cryexts_policy_table_enabled(struct super_block *sb);
int cryexts_load_policy_table(struct super_block *sb);
void cryexts_unload_policy_table(struct cryexts_sb_info *sbi);
bool cryexts_policy_exists(struct super_block *sb, u32 policy_id);
void cryexts_crypt_buffer(struct cryexts_sb_info *sbi, void *buf,
			  size_t len, u64 block, u64 pos);
int cryexts_read_file_block(struct super_block *sb, u64 block, void *buf);
int cryexts_write_file_block(struct super_block *sb, u64 block,
			     const void *buf);
int cryexts_read_inode_block(struct inode *inode, u64 block, void *buf);
int cryexts_write_inode_block(struct inode *inode, u64 block,
			      const void *buf);
int cryexts_sync_metadata(struct super_block *sb);
u32 cryexts_journal_checksum(const void *buf, size_t len);
int cryexts_journal_begin(struct super_block *sb);
int cryexts_journal_record_bh(struct super_block *sb, struct buffer_head *bh);
int cryexts_journal_record_block(struct super_block *sb, u64 home_block);
int cryexts_journal_commit(struct super_block *sb);
void cryexts_journal_abort(struct super_block *sb);
int cryexts_journal_replay(struct super_block *sb);
bool cryexts_journal_uses_v2(struct super_block *sb);
bool cryexts_orphan_feature_enabled(struct super_block *sb);
int cryexts_orphan_set(struct super_block *sb, u64 ino);
int cryexts_orphan_clear(struct super_block *sb, u64 ino);
int cryexts_orphan_cleanup(struct super_block *sb);
bool cryexts_journal_needs_recovery(struct super_block *sb);
void cryexts_super_set_recovery(struct super_block *sb, bool needed);

int cryexts_validate_inode(struct super_block *sb,
			   struct cryexts_inode *disk_inode, u64 ino);
int cryexts_validate_dir_block(struct inode *dir);

int cryexts_alloc_inode(struct super_block *sb, u64 *ino);
int cryexts_alloc_inode_goal(struct super_block *sb, u64 goal_group,
			     u64 *ino);
int cryexts_free_inode(struct super_block *sb, u64 ino);
int cryexts_alloc_block_goal(struct super_block *sb, u64 goal_block,
			     u64 goal_group, u64 *block);
int cryexts_alloc_block(struct super_block *sb, u64 *block);
int cryexts_free_block(struct super_block *sb, u64 block);
int cryexts_load_bitmaps(struct super_block *sb);
void cryexts_unload_bitmaps(struct cryexts_sb_info *sbi);

struct cryexts_inode *cryexts_get_disk_inode(struct super_block *sb,
					     u64 ino,
					     struct buffer_head **bhp);
int cryexts_write_inode_to_disk(struct inode *inode);
struct inode *cryexts_iget(struct super_block *sb, u64 ino);
struct inode *cryexts_new_inode(struct inode *dir, umode_t mode,
				u64 data_block);
int cryexts_release_inode_storage(struct inode *inode);
void cryexts_evict_inode(struct inode *inode);

int cryexts_setattr(struct user_namespace *mnt_userns,
		    struct dentry *dentry, struct iattr *attr);
int cryexts_getattr(struct user_namespace *mnt_userns,
		    const struct path *path, struct kstat *stat,
		    u32 request_mask, unsigned int query_flags);
ssize_t cryexts_read_iter(struct kiocb *iocb, struct iov_iter *to);
ssize_t cryexts_write_iter(struct kiocb *iocb, struct iov_iter *from);
const char *cryexts_get_link(struct dentry *dentry, struct inode *inode,
			     struct delayed_call *done);
int cryexts_free_xattr_storage(struct inode *inode);
ssize_t cryexts_listxattr(struct dentry *dentry, char *buffer, size_t size);

extern const struct xattr_handler *cryexts_xattr_handlers[];

extern const struct file_operations cryexts_dir_operations;
extern const struct file_operations cryexts_file_operations;
extern const struct inode_operations cryexts_dir_inode_operations;
extern const struct inode_operations cryexts_file_inode_operations;
extern const struct inode_operations cryexts_symlink_inode_operations;

#endif /* _CRYEXTS_H */
