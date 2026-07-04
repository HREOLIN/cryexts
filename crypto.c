// SPDX-License-Identifier: GPL-2.0
#include "cryexts.h"
#include <crypto/skcipher.h>
#include <linux/scatterlist.h>

static char *cryexts_mount_key;
module_param_named(key, cryexts_mount_key, charp, 0400);

static u32 cryexts_fnv1a_bytes(const void *data, size_t len, u32 seed)
{
	const u8 *p = data;
	u32 hash = seed;

	while (len--) {
		hash ^= *p++;
		hash *= 16777619u;
	}
	return hash;
}

static u32 cryexts_hash_bytes(const void *data, size_t len)
{
	return cryexts_fnv1a_bytes(data, len, 2166136261u);
}

bool cryexts_salt_is_zero(const u8 *salt)
{
	unsigned int i;

	for (i = 0; i < CRYEXTS_SALT_LEN; i++) {
		if (salt[i])
			return false;
	}
	return true;
}

static void cryexts_derive_salted_key(const char *key,
				      const u8 salt[CRYEXTS_SALT_LEN],
				      u8 derived[CRYEXTS_DERIVED_KEY_LEN])
{
	unsigned int round;
	size_t key_len = strlen(key);

	for (round = 0; round < CRYEXTS_DERIVED_KEY_LEN / sizeof(u32); round++) {
		u32 hash;
		u8 round_byte = (u8)round;

		hash = cryexts_fnv1a_bytes(key, key_len,
					   2166136261u ^ (0x9e3779b9u * (round + 1)));
		hash = cryexts_fnv1a_bytes(salt, CRYEXTS_SALT_LEN,
					   hash ^ 0x85ebca6bu);
		hash = cryexts_fnv1a_bytes(&round_byte, sizeof(round_byte),
					   hash ^ 0xc2b2ae35u);

		derived[round * 4 + 0] = (u8)(hash & 0xff);
		derived[round * 4 + 1] = (u8)((hash >> 8) & 0xff);
		derived[round * 4 + 2] = (u8)((hash >> 16) & 0xff);
		derived[round * 4 + 3] = (u8)((hash >> 24) & 0xff);
	}
}

static u32 cryexts_key_verifier(const u8 *derived, size_t len)
{
	return cryexts_hash_bytes(derived, len);
}

static void cryexts_store_be64(u8 *dst, u64 value)
{
	int i;

	for (i = 7; i >= 0; i--) {
		dst[i] = (u8)(value & 0xff);
		value >>= 8;
	}
}

static void cryexts_build_ctr_iv(const u8 salt[CRYEXTS_SALT_LEN], u64 block,
				 u64 pos, u8 iv[16])
{
	u64 counter = block * (CRYEXTS_BLOCK_SIZE / 16) + (pos / 16);

	memcpy(iv, salt, min_t(size_t, CRYEXTS_SALT_LEN, 8));
	memset(iv + 8, 0, 8);
	cryexts_store_be64(iv + 8, counter);
}

static int cryexts_crypt_buffer_with_tfm(struct crypto_skcipher *tfm,
					 const u8 salt[CRYEXTS_SALT_LEN],
					 void *buf, size_t len, u64 block,
					 u64 pos)
{
	struct skcipher_request *req;
	struct scatterlist sg;
	u8 iv[16];
	int err;

	if (!tfm)
		return -EINVAL;

	req = skcipher_request_alloc(tfm, GFP_NOFS);
	if (!req)
		return -ENOMEM;

	cryexts_build_ctr_iv(salt, block, pos, iv);
	sg_init_one(&sg, buf, len);
	skcipher_request_set_crypt(req, &sg, &sg, len, iv);
	err = crypto_skcipher_encrypt(req);
	skcipher_request_free(req);
	return err;
}

static void cryexts_derive_policy_key(const u8 *master, size_t master_len,
				      u32 policy_id,
				      const u8 context[CRYEXTS_POLICY_CONTEXT_LEN],
				      u8 derived[CRYEXTS_DERIVED_KEY_LEN])
{
	unsigned int round;

	for (round = 0; round < CRYEXTS_DERIVED_KEY_LEN / sizeof(u32); round++) {
		u32 hash;
		u8 round_byte = (u8)round;
		u8 policy_bytes[4];

		policy_bytes[0] = (u8)(policy_id & 0xff);
		policy_bytes[1] = (u8)((policy_id >> 8) & 0xff);
		policy_bytes[2] = (u8)((policy_id >> 16) & 0xff);
		policy_bytes[3] = (u8)((policy_id >> 24) & 0xff);

		hash = cryexts_fnv1a_bytes(master, master_len,
					   2166136261u ^ (0x9e3779b9u * (round + 1)));
		hash = cryexts_fnv1a_bytes(context, CRYEXTS_POLICY_CONTEXT_LEN,
					   hash ^ 0x85ebca6bu);
		hash = cryexts_fnv1a_bytes(policy_bytes, sizeof(policy_bytes),
					   hash ^ 0xc2b2ae35u);
		hash = cryexts_fnv1a_bytes(&round_byte, sizeof(round_byte),
					   hash ^ 0x27d4eb2fu);

		derived[round * 4 + 0] = (u8)(hash & 0xff);
		derived[round * 4 + 1] = (u8)((hash >> 8) & 0xff);
		derived[round * 4 + 2] = (u8)((hash >> 16) & 0xff);
		derived[round * 4 + 3] = (u8)((hash >> 24) & 0xff);
	}
}

bool cryexts_policy_table_enabled(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);

	return !!(le32_to_cpu(sbi->disk_sb->features_incompat) &
		  CRYEXTS_FEATURE_INCOMPAT_POLICY_TABLE) &&
	       !!le64_to_cpu(sbi->disk_sb->policy_table_block);
}

static struct cryexts_policy_runtime *cryexts_find_policy(struct cryexts_sb_info *sbi,
							  u32 policy_id)
{
	u16 i;

	for (i = 0; i < sbi->policy_count; i++) {
		if (sbi->policies[i].policy_id == policy_id)
			return &sbi->policies[i];
	}
	return NULL;
}

bool cryexts_policy_exists(struct super_block *sb, u32 policy_id)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);

	if (!cryexts_policy_table_enabled(sb))
		return true;
	return cryexts_find_policy(sbi, policy_id) != NULL;
}

static int cryexts_init_crypto_transform(struct cryexts_sb_info *sbi)
{
	int err;

	if (!sbi->encrypted)
		return 0;
	if (sbi->skcipher)
		return 0;
	if (sbi->encryption_alg != CRYEXTS_ALG_AES_CTR)
		return -EINVAL;

	sbi->skcipher = crypto_alloc_skcipher("ctr(aes)", 0, 0);
	if (IS_ERR(sbi->skcipher)) {
		err = PTR_ERR(sbi->skcipher);
		sbi->skcipher = NULL;
		pr_err("cryexts: failed to allocate ctr(aes) transform\n");
		return err;
	}

	err = crypto_skcipher_setkey(sbi->skcipher, sbi->derived_key,
				     min_t(u32, sbi->derived_key_len, 32U));
	if (err) {
		pr_err("cryexts: failed to set crypto key\n");
		crypto_free_skcipher(sbi->skcipher);
		sbi->skcipher = NULL;
		return err;
	}

	return 0;
}

static int cryexts_init_policy_transform(struct cryexts_policy_runtime *policy)
{
	int err;

	policy->skcipher = crypto_alloc_skcipher("ctr(aes)", 0, 0);
	if (IS_ERR(policy->skcipher)) {
		err = PTR_ERR(policy->skcipher);
		policy->skcipher = NULL;
		return err;
	}

	err = crypto_skcipher_setkey(policy->skcipher, policy->derived_key, 32U);
	if (err) {
		crypto_free_skcipher(policy->skcipher);
		policy->skcipher = NULL;
		return err;
	}

	return 0;
}

static const char *cryexts_find_mount_key(const char *options)
{
	const char *p = options;
	size_t prefix_len = strlen("key=");

	while (p && *p) {
		if (!strncmp(p, "key=", prefix_len))
			return p + prefix_len;
		p = strchr(p, ',');
		if (p)
			p++;
	}
	return cryexts_mount_key;
}

int cryexts_set_encryption_key(struct cryexts_sb_info *sbi,
			       const char *options)
{
	const char *key = cryexts_find_mount_key(options);
	char mount_key[CRYEXTS_KEY_MAX];
	u8 derived[CRYEXTS_DERIVED_KEY_LEN];
	size_t len;
	u32 verifier;

	if (!sbi->encrypted)
		return 0;
	if (!key || !*key) {
		pr_err("cryexts: encrypted filesystem requires key= mount option\n");
		return -EACCES;
	}
	if (sbi->encryption_flags != CRYEXTS_ENC_FLAG_DATA ||
	    sbi->encryption_kdf != CRYEXTS_KDF_SALTED_FNV1A ||
	    sbi->encryption_alg != CRYEXTS_ALG_AES_CTR) {
		pr_err("cryexts: unsupported encryption metadata\n");
		return -EINVAL;
	}

	len = strcspn(key, ",");
	if (!len || len >= CRYEXTS_KEY_MAX) {
		pr_err("cryexts: invalid key length\n");
		return -EINVAL;
	}

	memcpy(mount_key, key, len);
	mount_key[len] = '\0';
	cryexts_derive_salted_key(mount_key, sbi->salt, derived);
	verifier = cryexts_key_verifier(derived, sizeof(derived));
	memzero_explicit(mount_key, sizeof(mount_key));
	if (verifier != sbi->key_verifier) {
		memset(derived, 0, sizeof(derived));
		pr_err("cryexts: wrong encryption key\n");
		return -EACCES;
	}
	memcpy(sbi->derived_key, derived, sizeof(derived));
	sbi->derived_key_len = sizeof(derived);
	memset(derived, 0, sizeof(derived));
	return cryexts_init_crypto_transform(sbi);
}

void cryexts_unload_policy_table(struct cryexts_sb_info *sbi)
{
	u16 i;

	if (!sbi || !sbi->policies)
		return;

	for (i = 0; i < sbi->policy_count; i++) {
		if (sbi->policies[i].skcipher)
			crypto_free_skcipher(sbi->policies[i].skcipher);
		memzero_explicit(sbi->policies[i].derived_key,
				 sizeof(sbi->policies[i].derived_key));
	}
	kfree(sbi->policies);
	sbi->policies = NULL;
	sbi->policy_count = 0;
}

int cryexts_load_policy_table(struct super_block *sb)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(sb);
	struct buffer_head *bh;
	struct cryexts_policy_table_block *pt;
	struct cryexts_policy_runtime *policies;
	u64 block;
	u16 count;
	u16 i;
	int err = 0;

	if (!cryexts_policy_table_enabled(sb))
		return 0;
	if (sbi->policies)
		return 0;

	block = le64_to_cpu(sbi->disk_sb->policy_table_block);
	bh = sb_bread(sb, block);
	if (!bh)
		return -EIO;

	pt = (struct cryexts_policy_table_block *)bh->b_data;
	if (!cryexts_policy_table_checksum_valid(sb, block, pt)) {
		brelse(bh);
		return -EUCLEAN;
	}
	count = le16_to_cpu(pt->entry_count);
	if (le32_to_cpu(pt->magic) != CRYEXTS_POLICY_TABLE_MAGIC ||
	    !count || count > CRYEXTS_POLICY_TABLE_MAX_ENTRIES) {
		brelse(bh);
		return -EUCLEAN;
	}

	policies = kcalloc(count, sizeof(*policies), GFP_KERNEL);
	if (!policies) {
		brelse(bh);
		return -ENOMEM;
	}

	for (i = 0; i < count; i++) {
		struct cryexts_policy_entry *entry = &pt->entries[i];
		u16 j;

		policies[i].policy_id = le32_to_cpu(entry->policy_id);
		policies[i].flags = le32_to_cpu(entry->flags);
		memcpy(policies[i].context, entry->context,
		       sizeof(policies[i].context));

		for (j = 0; j < i; j++) {
			if (policies[j].policy_id == policies[i].policy_id) {
				err = -EUCLEAN;
				goto out_free;
			}
		}

		if (sbi->encrypted) {
			cryexts_derive_policy_key(sbi->derived_key,
						  sbi->derived_key_len,
						  policies[i].policy_id,
						  policies[i].context,
						  policies[i].derived_key);
			err = cryexts_init_policy_transform(&policies[i]);
			if (err)
				goto out_free;
		}
	}

	brelse(bh);
	sbi->policies = policies;
	sbi->policy_count = count;

	if (!cryexts_policy_exists(sb,
				   le32_to_cpu(sbi->disk_sb->default_encryption_policy))) {
		cryexts_unload_policy_table(sbi);
		return -EUCLEAN;
	}

	return 0;

out_free:
	brelse(bh);
	sbi->policies = policies;
	sbi->policy_count = count;
	cryexts_unload_policy_table(sbi);
	return err;
}

void cryexts_crypt_buffer(struct cryexts_sb_info *sbi, void *buf,
			  size_t len, u64 block, u64 pos)
{
	int err;

	if (!sbi->encrypted || !sbi->derived_key_len)
		return;
	if (sbi->encryption_alg != CRYEXTS_ALG_AES_CTR || !sbi->skcipher)
		return;
	err = cryexts_crypt_buffer_with_tfm(sbi->skcipher, sbi->salt, buf, len,
					 block, pos);
	if (err)
		pr_err("cryexts: crypto transform failed (%d)\n", err);
}

int cryexts_read_file_block(struct super_block *sb, u64 block, void *buf)
{
	struct buffer_head *bh = sb_bread(sb, block);

	if (!bh)
		return -EIO;
	memcpy(buf, bh->b_data, CRYEXTS_BLOCK_SIZE);
	brelse(bh);
	cryexts_crypt_buffer(CRYEXTS_SB(sb), buf, CRYEXTS_BLOCK_SIZE, block, 0);
	return 0;
}

int cryexts_write_file_block(struct super_block *sb, u64 block,
			     const void *buf)
{
	struct buffer_head *bh = sb_getblk(sb, block);

	if (!bh)
		return -EIO;
	lock_buffer(bh);
	memcpy(bh->b_data, buf, CRYEXTS_BLOCK_SIZE);
	cryexts_crypt_buffer(CRYEXTS_SB(sb), bh->b_data, CRYEXTS_BLOCK_SIZE,
			     block, 0);
	set_buffer_uptodate(bh);
	mark_buffer_dirty(bh);
	unlock_buffer(bh);
	brelse(bh);
	return 0;
}

static bool cryexts_policy_io_enabled(struct inode *inode)
{
	return CRYEXTS_SB(inode->i_sb)->encrypted &&
	       cryexts_policy_table_enabled(inode->i_sb) &&
	       (S_ISREG(inode->i_mode) || S_ISLNK(inode->i_mode));
}

int cryexts_read_inode_block(struct inode *inode, u64 block, void *buf)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(inode->i_sb);
	struct cryexts_policy_runtime *policy;
	struct buffer_head *bh;
	int err;

	bh = sb_bread(inode->i_sb, block);
	if (!bh)
		return -EIO;
	memcpy(buf, bh->b_data, CRYEXTS_BLOCK_SIZE);
	brelse(bh);

	if (!cryexts_policy_io_enabled(inode)) {
		cryexts_crypt_buffer(sbi, buf, CRYEXTS_BLOCK_SIZE, block, 0);
		return 0;
	}

	policy = cryexts_find_policy(sbi, cryexts_inode_policy_id(inode));
	if (!policy || !policy->skcipher)
		return -EUCLEAN;

	err = cryexts_crypt_buffer_with_tfm(policy->skcipher, sbi->salt, buf,
					     CRYEXTS_BLOCK_SIZE, block, 0);
	if (err)
		return err;
	return 0;
}

int cryexts_write_inode_block(struct inode *inode, u64 block, const void *buf)
{
	struct cryexts_sb_info *sbi = CRYEXTS_SB(inode->i_sb);
	struct cryexts_policy_runtime *policy;
	struct buffer_head *bh;
	int err;

	bh = sb_getblk(inode->i_sb, block);
	if (!bh)
		return -EIO;

	lock_buffer(bh);
	memcpy(bh->b_data, buf, CRYEXTS_BLOCK_SIZE);

	if (!cryexts_policy_io_enabled(inode)) {
		cryexts_crypt_buffer(sbi, bh->b_data, CRYEXTS_BLOCK_SIZE, block, 0);
	} else {
		policy = cryexts_find_policy(sbi, cryexts_inode_policy_id(inode));
		if (!policy || !policy->skcipher) {
			unlock_buffer(bh);
			brelse(bh);
			return -EUCLEAN;
		}
		err = cryexts_crypt_buffer_with_tfm(policy->skcipher, sbi->salt,
						    bh->b_data, CRYEXTS_BLOCK_SIZE,
						    block, 0);
		if (err) {
			unlock_buffer(bh);
			brelse(bh);
			return err;
		}
	}

	set_buffer_uptodate(bh);
	mark_buffer_dirty(bh);
	unlock_buffer(bh);
	brelse(bh);
	return 0;
}
