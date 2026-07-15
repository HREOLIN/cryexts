# CRYEXTS v10.4 加密缓存协同说明

## 1. 版本目标

v10.4 固定 page cache、buffered write 与 policy-aware encryption 的边界，并修复 encrypted filesystem 与 journal 组合时暴露的 metadata 加密越界：

```text
磁盘密文
-> cryexts_read_inode_block 解密
-> page cache 保存明文
-> VFS 读写明文缓存页
-> writeback 读取明文页
-> cryexts_write_inode_block 加密
-> 磁盘密文
```

本版本不修改 on-disk format、AES-CTR IV、KDF、policy key 派生或 inode policy 字段。

## 2. Page cache 的数据语义

page cache 必须保存明文。原因是 VFS、用户态 read/write 和同一页上的多次小写都需要操作普通文件内容。如果缓存密文，每次访问都要重复解密，而且用户态可能直接读到密文。

### 读取

```text
cryexts_readpage
-> cryexts_fill_page
-> cryexts_resolve_block(create=false)
-> cryexts_read_inode_block
-> 选择 inode policy key 并解密
-> 明文复制到 page
-> PageUptodate
```

### 写回

```text
cryexts_writepage_locked
-> 从 page cache 读取明文
-> cryexts_resolve_block(create=true)
-> cryexts_write_inode_block
-> 明文复制到临时 block buffer
-> 选择 inode policy key 并加密临时副本
-> 加密成功后复制到 buffer_head
-> 标记 block buffer dirty
```

加密发生在临时副本中，不会原地修改 page cache，也不会在加密失败时污染已有 `buffer_head`。因此写回后缓存页仍是明文，block buffer 只接收完整密文；临时副本使用 `kfree_sensitive()` 清零释放。

## 3. `cryexts_crypt_buffer()`

文件：`crypto.c`

### 旧行为

函数返回 `void`。Crypto API 失败时只打印日志，调用者仍可能把未加密数据当作成功结果继续读写。

### 新行为

```c
int cryexts_crypt_buffer(struct cryexts_sb_info *sbi, void *buf,
                         size_t len, u64 block, u64 pos);
```

输入：

- `sbi`：当前挂载实例及 master-key transform。
- `buf/len`：需要原地变换的数据范围。
- `block/pos`：构造 AES-CTR counter 的物理块号和块内位置。

输出：

- `0`：未启用加密，或加解密成功。
- `-EACCES`：加密文件系统没有可用 derived key。
- `-EUCLEAN`：磁盘声明加密，但算法或 transform 状态不完整。
- 其他负 errno：内核 Crypto API 返回的实际错误。

职责：统一执行全局 key 的对称变换，并把错误交给上层 I/O 路径处理。

## 4. 同步 skcipher 约束

`cryexts_init_crypto_transform()` 和 `cryexts_init_policy_transform()` 调用：

```c
crypto_alloc_skcipher("ctr(aes)", 0, CRYPTO_ALG_ASYNC)
```

`mask` 中包含 `CRYPTO_ALG_ASYNC`、`type` 对应位为 0，表示排除异步实现。CRYEXTS 当前块 I/O 会在函数返回后立即使用变换结果，没有异步 completion 生命周期，因此必须选择同步 transform。

## 5. 错误传播函数

### `cryexts_crypt_inode_buffer()`

输入为 inode、physical block 和一个可原地修改的 4 KiB buffer。函数判断 regular file/symlink 是否启用 policy I/O：启用时查找 inode 的 policy transform，否则回落到全局 transform。返回 0 或负 errno。

职责是统一 inode policy 选择，read 和 write 不再各自维护一份分支逻辑。它是 `crypto.c` 内部 helper，不改变公共 API。

### `cryexts_read_file_block()`

这是 metadata/raw block 读取 API，供 journal 和 xattr 使用。它只读取并复制磁盘块，不执行数据加密。这样 journal control、descriptor、payload、commit 和 xattr 保持 fsck 可直接解析的格式。

### `cryexts_write_file_block()`

这是 metadata/raw block 写入 API。它把调用者提供的元数据直接复制到 `buffer_head`，不执行数据加密。journal 和 xattr 必须使用该接口，不能误走 inode data cipher。

### `cryexts_read_inode_block()`

- regular file/symlink 且 policy table 启用：使用 inode policy transform。
- 其他路径：使用全局 transform，并传播加密错误。

该函数仍是 readpage 装载明文页的唯一加密边界。

### `cryexts_write_inode_block()`

- policy-aware inode 使用对应 policy transform。
- 其他数据使用全局 transform。
- 加密在临时副本中完成，任一变换失败都不会修改或标记 block buffer dirty。

该函数仍是 writeback 把明文页变成磁盘密文的唯一边界。

## 6. Journal control magic 问题

第一次 v10.4 smoke 出现：

```text
cryextsck: journal v2 control magic is invalid
```

根因不是 journal sequence 或 checksum，而是旧的 `cryexts_write_file_block()` 会在 encrypted filesystem 上对所有调用者执行全局加密。v5.4 之后 regular file/symlink 已迁移到 inode-aware I/O，该旧 API 实际只剩 journal/xattr metadata 使用，因此 journal control 被错误写成密文，`cryextsck` 无需密钥也无法读取 `JNL2` magic。

修复后的边界为：

```text
cryexts_read/write_file_block   -> metadata/raw I/O，不加密
cryexts_read/write_inode_block  -> regular/symlink data，按 policy 加密
```

这与 superblock 的 `CRYEXTS_ENC_FLAG_DATA` 语义一致，也恢复了 fsck 对 encrypted image 元数据的离线检查能力。

## 7. 结构体与磁盘兼容性

本版本没有增加或修改结构体字段，没有新增 feature bit，不需要升级 mkfs 或 fsck 格式解析。regular file/symlink 的既有密文格式保持兼容。

旧运行时若曾在 encrypted image 上把 journal/xattr metadata 错误写成密文，因为磁盘没有 feature bit 区分这种错误状态，无法安全自动判断和迁移。此类测试镜像应重新执行 mkfs；`journal v2 control magic is invalid` 的现有镜像也应由 smoke 自动重建，不能继续当作 clean 基线。

## 8. Smoke 测试

脚本：`scripts/smoke_v10_4_encrypted_cache.sh`

覆盖内容：

1. 创建相同配置的 plain 和 encrypted image。
2. 分别执行带 `fsync` 的顺序写与 remount 后顺序读，输出 MB/s。
3. 在 policy 7 和 policy 9 目录中执行 512-byte buffered writes。
4. fsync 前后读取并校验 page-cache 明文。
5. remount 后校验两个 policy 文件内容和继承的 policy id。
6. 验证错误 key 无法挂载。
7. 扫描 raw encrypted image，确认测试明文没有泄漏。
8. 对两个 image 执行最终 fsck。

运行：

```bash
chmod +x scripts/smoke_v10_4_encrypted_cache.sh
./scripts/smoke_v10_4_encrypted_cache.sh
```

可调整基准大小：

```bash
BENCH_MB=32 SIZE_MB=256 ./scripts/smoke_v10_4_encrypted_cache.sh
```

最终成功标志：

```text
v10.4 encrypted page-cache smoke test passed
```

## 9. 性能数据解释

脚本输出：

```text
plain_write_mb_s
plain_read_mb_s
encrypted_write_mb_s
encrypted_read_mb_s
```

写测试通过 Python `os.fsync()` 等待数据落盘，包含 writeback、块分配、加密和当前单页 journal transaction 成本。读写计时都使用 `time.monotonic()`，不解析外部工具输出。读测试在 unmount/remount 后执行，文件 page cache 已重建，但底层 image 仍可能命中宿主机缓存，因此适合比较同机 plain/encrypted 差异，不代表真实 USB 冷读速度。

## 10. 当前边界

- 仍然按 4 KiB block 为每次数据 I/O 创建一个 skcipher request。
- encrypted write 每个 4 KiB block 使用一个临时副本，优先保证缓存页和已有 block buffer 不被失败的原地变换污染。
- writeback 仍为每个 dirty page 提交一个 journal transaction。
- 没有实现跨页加密批处理、request pool 或异步 Crypto API。
- 这些优化应以 v10.4 基准确认加密成为主要瓶颈后再引入。
