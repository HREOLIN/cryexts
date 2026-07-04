/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _CRYEXTS_FS_H
#define _CRYEXTS_FS_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <linux/types.h>
#endif

#define CRYEXTS_MAGIC 0x43525853U /* "CRXS" */
#define CRYEXTS_VERSION_V2 2
#define CRYEXTS_VERSION_V3 3
#define CRYEXTS_VERSION_V4 4
#define CRYEXTS_VERSION_V5 5
#define CRYEXTS_VERSION_V6 6
#define CRYEXTS_VERSION CRYEXTS_VERSION_V5
#define CRYEXTS_BLOCK_SIZE 4096
#define CRYEXTS_SUPER_OFFSET 1024
#define CRYEXTS_LABEL_LEN 16
#define CRYEXTS_UUID_LEN 16
#define CRYEXTS_VOLUME_NAME_LEN 16
#define CRYEXTS_ROOT_INO 1
#define CRYEXTS_GDT_START_BLOCK 1
#define CRYEXTS_BLOCK_BITMAP_BLOCK 1
#define CRYEXTS_INODE_BITMAP_BLOCK 2
#define CRYEXTS_INODE_TABLE_START 3
#define CRYEXTS_INODE_TABLE_BLOCKS 16
#define CRYEXTS_ROOT_INODE_BLOCK CRYEXTS_INODE_TABLE_START
#define CRYEXTS_ROOT_DIR_BLOCK \
	(CRYEXTS_INODE_TABLE_START + CRYEXTS_INODE_TABLE_BLOCKS)
#define CRYEXTS_FIRST_DATA_BLOCK CRYEXTS_ROOT_DIR_BLOCK
#define CRYEXTS_FIRST_FREE_DATA_BLOCK (CRYEXTS_ROOT_DIR_BLOCK + 1)
#define CRYEXTS_NAME_LEN 255
#define CRYEXTS_DIRECT_BLOCKS 12
#define CRYEXTS_INDIRECT_BLOCKS (CRYEXTS_BLOCK_SIZE / sizeof(__le64))
#define CRYEXTS_FILE_BLOCKS_MAX \
	(CRYEXTS_DIRECT_BLOCKS + CRYEXTS_INDIRECT_BLOCKS)
#define CRYEXTS_KEY_MAX 64

#define CRYEXTS_SB_FLAG_ENCRYPTED 0x00000001U
#define CRYEXTS_ENC_FLAG_DATA 0x00000001U

#define CRYEXTS_FS_STATE_CLEAN 0x00000001U
#define CRYEXTS_FS_STATE_NEEDS_RECOVERY 0x00000002U
#define CRYEXTS_FS_STATE_ERRORS 0x00000004U

#define CRYEXTS_FEATURE_COMPAT_NONE 0x00000000U
#define CRYEXTS_FEATURE_COMPAT_HAS_JOURNAL 0x00000001U
#define CRYEXTS_FEATURE_COMPAT_PREALLOC 0x00000002U
#define CRYEXTS_FEATURE_INCOMPAT_SINGLE_INDIRECT 0x00000001U
#define CRYEXTS_FEATURE_INCOMPAT_BLOCK_GROUPS 0x00000002U
#define CRYEXTS_FEATURE_INCOMPAT_NEEDS_RECOVERY 0x00000004U
#define CRYEXTS_FEATURE_INCOMPAT_EXTENTS 0x00000008U
#define CRYEXTS_FEATURE_INCOMPAT_XATTR 0x00000010U
#define CRYEXTS_FEATURE_INCOMPAT_ENCRYPTION_POLICY 0x00000020U
#define CRYEXTS_FEATURE_INCOMPAT_DIR_INDEX 0x00000040U
#define CRYEXTS_FEATURE_INCOMPAT_ORPHAN_LIST 0x00000080U
#define CRYEXTS_FEATURE_INCOMPAT_POLICY_TABLE 0x00000100U
#define CRYEXTS_FEATURE_INCOMPAT_EXTENT_TREE 0x00000200U
#define CRYEXTS_FEATURE_INCOMPAT_JOURNAL_V2 0x00000400U
#define CRYEXTS_FEATURE_RO_COMPAT_NONE 0x00000000U
#define CRYEXTS_FEATURE_RO_COMPAT_METADATA_CSUM 0x00000001U
#define CRYEXTS_FEATURE_RO_COMPAT_LARGE_XATTR 0x00000002U

#define CRYEXTS_DEFAULT_JOURNAL_BLOCKS 512U
#define CRYEXTS_JOURNAL_V2_MIN_BLOCKS 4U
#define CRYEXTS_JOURNAL_MAGIC 0x4a4e4c31U /* "JNL1" */
#define CRYEXTS_JOURNAL_FLAG_VALID 0x00000001U
#define CRYEXTS_JOURNAL_HEADER_BYTES 64U
#define CRYEXTS_JOURNAL_CHECKSUM_OFFSET (sizeof(__le32) * 3)
#define CRYEXTS_JOURNAL_MAX_ENTRIES \
	((CRYEXTS_BLOCK_SIZE - CRYEXTS_JOURNAL_HEADER_BYTES) / sizeof(__le64))
#define CRYEXTS_JOURNAL_V2_MAGIC 0x4a4e4c32U /* "JNL2" */
#define CRYEXTS_JOURNAL_V2_LAYOUT_VERSION 2U
#define CRYEXTS_JOURNAL_V2_FEATURE_BASELINE 0x00000001U
#define CRYEXTS_JOURNAL_V2_FLAG_ACTIVE 0x00000001U
#define CRYEXTS_JOURNAL_V2_FLAG_COMMITTED 0x00000002U
#define CRYEXTS_JOURNAL_V2_BLOCK_CONTROL 1U
#define CRYEXTS_JOURNAL_V2_BLOCK_DESCRIPTOR 2U
#define CRYEXTS_JOURNAL_V2_BLOCK_COMMIT 3U

#define CRYEXTS_KDF_NONE 0U
#define CRYEXTS_KDF_SALTED_FNV1A 1U

#define CRYEXTS_ALG_NONE 0U
#define CRYEXTS_ALG_XOR 1U
#define CRYEXTS_ALG_AES_CTR 2U

#define CRYEXTS_SALT_LEN 16
#define CRYEXTS_DERIVED_KEY_LEN 32

#define CRYEXTS_FT_UNKNOWN 0
#define CRYEXTS_FT_REG_FILE 1
#define CRYEXTS_FT_DIR 2
#define CRYEXTS_FT_SYMLINK 3

#define CRYEXTS_DEFAULT_BLOCKS_PER_GROUP 4096U
#define CRYEXTS_DEFAULT_INODE_TABLE_BLOCKS_PER_GROUP 4U
#define CRYEXTS_DEFAULT_INODES_PER_GROUP \
	(CRYEXTS_DEFAULT_INODE_TABLE_BLOCKS_PER_GROUP * \
	 (CRYEXTS_BLOCK_SIZE / sizeof(struct cryexts_inode)))

#define CRYEXTS_INODE_FLAG_EXTENTS 0x00000001U
#define CRYEXTS_INODE_FLAG_IMMUTABLE 0x00000002U
#define CRYEXTS_INODE_FLAG_APPEND_ONLY 0x00000004U
#define CRYEXTS_INODE_FLAG_DIR_INDEX 0x00000008U
#define CRYEXTS_INODE_FLAG_EXTENT_TREE_V2 0x00000010U
#define CRYEXTS_EXTENT_MAGIC 0x4558U /* "EX" */
#define CRYEXTS_LEGACY_INLINE_EXTENTS 4U
#define CRYEXTS_EXTENT_ROOT_INLINE_EXTENTS 3U
#define CRYEXTS_MAX_INLINE_EXTENTS CRYEXTS_LEGACY_INLINE_EXTENTS
#define CRYEXTS_MAX_EXTENT_BLOCKS 65535U
#define CRYEXTS_EXTENT_FILE_BLOCKS_MAX \
	(CRYEXTS_MAX_INLINE_EXTENTS * CRYEXTS_MAX_EXTENT_BLOCKS)
#define CRYEXTS_EXTENT_TREE_V2_DEPTH 1U
#define CRYEXTS_EXTENT_TREE_ROOT_REFS 4U
#define CRYEXTS_RESERVATION_WINDOW_BLOCKS 64U

#define CRYEXTS_XATTR_MAGIC 0x58415454U /* "XATT" */
#define CRYEXTS_XATTR_NAMESPACE_USER 1U
#define CRYEXTS_XATTR_MAX_ITEMS 32U
#define CRYEXTS_XATTR_MAX_NAME_LEN 247U
#define CRYEXTS_XATTR_POLICY_NAME "cryexts.policy_id"
#define CRYEXTS_DIR_INDEX_MAGIC 0x44495831U /* "DIX1" */
#define CRYEXTS_DIR_INDEX_BUCKETS 64U
#define CRYEXTS_POLICY_TABLE_MAGIC 0x50544c31U /* "PTL1" */
#define CRYEXTS_POLICY_CONTEXT_LEN 8U
#define CRYEXTS_METADATA_CSUM_NONE 0U
#define CRYEXTS_METADATA_CSUM_FNV1A32 1U

struct cryexts_super_block {
	__le32 magic;
	__le32 version;
	__le32 block_size;
	__le32 inode_size;
	__le64 blocks_count;
	__le64 inodes_count;
	__le64 free_blocks_count;
	__le64 free_inodes_count;
	__le64 block_bitmap_block;
	__le64 inode_bitmap_block;
	__le64 inode_table_start;
	__le64 inode_table_blocks;
	__le64 root_inode_block;
	__le64 root_dir_block;
	__le64 first_data_block;
	__le64 next_ino;
	__le64 next_data_block;
	char label[CRYEXTS_LABEL_LEN];
	__le32 flags;
	__le32 key_hash;
	__le32 features_compat;
	__le32 features_incompat;
	__le32 features_ro_compat;
	__le32 encryption_flags;
	__le32 encryption_kdf;
	__le32 encryption_alg;
	__u8 salt[CRYEXTS_SALT_LEN];
	__le32 state;
	__le32 mount_count;
	__le32 max_mount_count;
	__le32 default_encryption_policy;
	__le64 last_mount_time;
	__le64 last_write_time;
	__le64 last_check_time;
	__le64 journal_block;
	__le64 journal_blocks;
	__le64 group_count;
	__le64 blocks_per_group;
	__le64 inodes_per_group;
	__le64 group_desc_table_start;
	__le64 group_desc_table_blocks;
	__u8 uuid[CRYEXTS_UUID_LEN];
	char volume_name[CRYEXTS_VOLUME_NAME_LEN];
	__le64 orphan_head;
	__le64 policy_table_block;
	__le64 dir_index_seed;
	__le64 metadata_csum_type;
	__le64 journal_sequence;
	__le64 fs_generation;
	__u8 reserved[96];
} __attribute__((packed));

struct cryexts_group_desc {
	__le64 group_start;
	__le64 blocks_count;
	__le64 block_bitmap_block;
	__le64 inode_bitmap_block;
	__le64 inode_table_start;
	__le32 inode_table_blocks;
	__le32 free_blocks_count;
	__le32 free_inodes_count;
	__le32 used_dirs_count;
	__le32 flags;
	__u8 reserved[16];
} __attribute__((packed));

struct cryexts_journal_header {
	__le32 magic;
	__le32 flags;
	__le32 entry_count;
	__le32 checksum;
	__le64 sequence;
	__u8 reserved[40];
	__le64 home_blocks[];
} __attribute__((packed));

struct cryexts_journal_v2_control {
	__le32 magic;
	__le16 layout_version;
	__le16 block_type;
	__le32 flags;
	__le32 features;
	__le32 checksum;
	__le32 reserved0;
	__le64 last_sequence;
	__le64 active_sequence;
	__le64 tail_sequence;
	__le64 checkpoint_sequence;
	__le64 descriptor_block;
	__le64 payload_start;
	__le64 payload_blocks;
	__le64 commit_block;
	__u8 reserved[56];
} __attribute__((packed));

struct cryexts_journal_v2_descriptor {
	__le32 magic;
	__le16 layout_version;
	__le16 block_type;
	__le32 flags;
	__le32 entry_count;
	__le32 checksum;
	__le32 reserved0;
	__le64 sequence;
	__le64 payload_start;
	__le64 commit_block;
	__u8 reserved[24];
	__le64 home_blocks[];
} __attribute__((packed));

struct cryexts_journal_v2_commit {
	__le32 magic;
	__le16 layout_version;
	__le16 block_type;
	__le32 flags;
	__le32 entry_count;
	__le32 checksum;
	__le32 reserved0;
	__le64 sequence;
	__le64 descriptor_block;
	__u8 reserved[32];
} __attribute__((packed));

#define CRYEXTS_JOURNAL_V2_DESCRIPTOR_BYTES 72U
#define CRYEXTS_JOURNAL_V2_COMMIT_BYTES 72U
#define CRYEXTS_JOURNAL_V2_MAX_ENTRIES \
	((CRYEXTS_BLOCK_SIZE - CRYEXTS_JOURNAL_V2_DESCRIPTOR_BYTES) / \
	 sizeof(__le64))

struct cryexts_extent_header {
	__le16 magic;
	__le16 entries;
	__le16 max;
	__le16 reserved;
} __attribute__((packed));

struct cryexts_extent {
	__le64 logical_start;
	__le64 physical_start;
	__le32 length;
	__le32 flags;
} __attribute__((packed));

struct cryexts_extent_root_ref {
	__le64 logical_start;
	__le64 leaf_block;
	__le16 entries;
	__le32 checksum;
} __attribute__((packed));

#define CRYEXTS_EXTENTS_PER_BLOCK \
	((CRYEXTS_BLOCK_SIZE - sizeof(struct cryexts_extent_header)) / \
	 sizeof(struct cryexts_extent))
#define CRYEXTS_EXTENT_TREE_V2_FILE_BLOCKS_MAX \
	((uint64_t)CRYEXTS_EXTENT_TREE_ROOT_REFS * CRYEXTS_EXTENTS_PER_BLOCK * \
	 CRYEXTS_MAX_EXTENT_BLOCKS)
#define CRYEXTS_EXTENT_ROOT_OVERFLOW_OFFSET \
	(sizeof(struct cryexts_extent_header) + \
	 CRYEXTS_EXTENT_ROOT_INLINE_EXTENTS * sizeof(struct cryexts_extent))
#define CRYEXTS_EXTENT_ROOT_OVERFLOW_ENTRIES_OFFSET \
	(CRYEXTS_EXTENT_ROOT_OVERFLOW_OFFSET + sizeof(__le64))
#define CRYEXTS_EXTENT_ROOT_OVERFLOW_CSUM_OFFSET \
	(CRYEXTS_EXTENT_ROOT_OVERFLOW_ENTRIES_OFFSET + sizeof(__le16))
#define CRYEXTS_EXTENT_TREE_V2_ROOT_REFS_OFFSET \
	(sizeof(struct cryexts_extent_header))
#define CRYEXTS_EXTENT_TREE_FILE_BLOCKS_MAX \
	((CRYEXTS_EXTENT_ROOT_INLINE_EXTENTS + CRYEXTS_EXTENTS_PER_BLOCK) * \
	 CRYEXTS_MAX_EXTENT_BLOCKS)

struct cryexts_inode_extra {
	__le64 xattr_block;
	__le32 encryption_policy_id;
	__le64 next_orphan;
} __attribute__((packed));

struct cryexts_xattr_block_header {
	__le32 magic;
	__le16 entries;
	__le16 used_bytes;
	__le64 overflow_block;
} __attribute__((packed));

struct cryexts_xattr_entry {
	__u8 name_len;
	__u8 namespace_id;
	__le16 value_len;
	char data[];
} __attribute__((packed));

struct cryexts_dir_index_block {
	__le32 magic;
	__le16 buckets;
	__le16 dir_blocks;
	__le32 entries;
	__u8 reserved[16];
	__le16 block_masks[CRYEXTS_DIR_INDEX_BUCKETS];
} __attribute__((packed));

struct cryexts_policy_entry {
	__le32 policy_id;
	__le32 flags;
	__u8 context[CRYEXTS_POLICY_CONTEXT_LEN];
} __attribute__((packed));

struct cryexts_policy_table_block {
	__le32 magic;
	__le16 entry_count;
	__le16 reserved0;
	__u8 reserved[24];
	struct cryexts_policy_entry entries[];
} __attribute__((packed));

#define CRYEXTS_POLICY_TABLE_HEADER_BYTES 32U
#define CRYEXTS_POLICY_TABLE_MAX_ENTRIES \
	((CRYEXTS_BLOCK_SIZE - CRYEXTS_POLICY_TABLE_HEADER_BYTES) / \
	 sizeof(struct cryexts_policy_entry))

struct cryexts_inode {
	__le16 mode;
	__le16 links_count;
	__le32 uid;
	__le32 gid;
	__le64 size;
	__le64 blocks;
	__le64 atime;
	__le64 ctime;
	__le64 mtime;
	__le64 block[CRYEXTS_DIRECT_BLOCKS];
	__le64 indirect_block;
	__le32 inode_flags;
	__u8 reserved[116];
} __attribute__((packed));

struct cryexts_dir_entry {
	__le64 inode;
	__le16 rec_len;
	__u8 name_len;
	__u8 file_type;
	char name[];
} __attribute__((packed));

#define CRYEXTS_DIR_ENTRY_HEADER_SIZE \
	((unsigned int)sizeof(struct cryexts_dir_entry))

static inline unsigned int cryexts_dir_rec_len(unsigned int name_len)
{
	return (CRYEXTS_DIR_ENTRY_HEADER_SIZE + name_len + 3) & ~3U;
}

#endif /* _CRYEXTS_FS_H */
