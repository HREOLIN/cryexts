# CRYEXTS v6.5 变更说明

## 1. 这一版解决什么问题

v5.3 已经引入 directory hash index：

```text
name -> hash -> bucket -> block mask -> scan candidate directory blocks
```

但 v5.3 的维护方式比较粗：

```text
create / unlink / rename
-> 修改目录项
-> cryexts_dir_index_rebuild()
-> 重新扫描整个目录
```

v6.5 的目标是把目录索引维护推进一步：

```text
目录项新增 -> 增量打开对应 bucket 的 block bit
目录项删除 -> 只在该 block 不再含有同 bucket 项时清 bit
rename/link -> 通过 add/delete 路径自然维护索引
fsck/inspect -> 能观测并校验索引维护结果
```

一句话理解：

```text
v5.3 = 有目录索引，但变化后靠全量重建
v6.5 = 目录索引开始具备增量维护语义
```

## 2. 为什么 v6.5 不直接做真正 HTree

当前目录数据块仍然受 `CRYEXTS_DIRECT_BLOCKS = 12` 限制。也就是说，一个目录最多使用 12 个目录逻辑块。

现有目录索引结构的 mask 是 16 bit：

```c
__le16 block_masks[CRYEXTS_DIR_INDEX_BUCKETS];
```

所以在当前目录存储模型下，16 bit mask 已经能覆盖所有目录逻辑块。

如果现在直接做多层 HTree，会同时牵扯：

- 目录数据块突破 direct-block 上限
- 新 on-disk directory index 格式
- fsck 对新格式的完整理解
- rename/unlink/create 错误回滚语义

这些更适合放到后续版本。v6.5 先把现有索引维护路径打稳，避免后面扩展时地基不稳。

## 3. 修改的代码文件

### 3.1 [dir.c](/D:/Carl/cryptext4/cryexts/dir.c:1)

新增 helper：

- `cryexts_dir_index_add_name()`
- `cryexts_dir_index_remove_name()`
- `cryexts_dir_logical_block_for_bh()`

修改路径：

- `cryexts_add_entry()`
- `cryexts_delete_entry()`

### 3.2 [tools/cryexts_dir_index_inspect.c](/D:/Carl/cryptext4/cryexts/tools/cryexts_dir_index_inspect.c:1)

新增输出：

- `active_buckets`
- `mask_refs`

### 3.3 [tools/cryextsck.c](/D:/Carl/cryptext4/cryexts/tools/cryextsck.c:1)

新增校验：

- directory index 的 `entries` 必须等于真实 live dirent 数量

### 3.4 [scripts/smoke_v6_5_dir_index_maintenance.sh](/D:/Carl/cryptext4/cryexts/scripts/smoke_v6_5_dir_index_maintenance.sh:1)

新增 smoke test，覆盖：

- create 大量目录项
- unlink 一部分目录项
- rename 一部分目录项
- hard link 一部分目录项
- remount 后 lookup 验证
- inspect 校验 index 统计
- `cryextsck` clean

## 4. 新增函数说明

### 4.1 `cryexts_dir_index_add_name()`

位置：[dir.c](/D:/Carl/cryptext4/cryexts/dir.c:183)

职责：

- 在目录项新增成功后更新 directory index
- 根据文件名 hash 算出 bucket
- 将该 bucket 的对应目录逻辑块 bit 置 1
- `entries++`
- 更新 `dir_blocks`
- 写回 index block 并更新 checksum

核心语义：

```text
bucket = hash(name) % 64
block_masks[bucket] |= 1 << logical_block
entries++
```

失败处理：

- 如果 index 缺失、读取失败或写回失败，会回退到 `cryexts_dir_index_rebuild()`
- 这样不会因为增量路径失败就留下明显不一致的索引

### 4.2 `cryexts_dir_index_remove_name()`

位置：[dir.c](/D:/Carl/cryptext4/cryexts/dir.c:216)

职责：

- 在目录项删除后更新 directory index
- 计算被删除名字所属 bucket
- 扫描被删除项所在的目录逻辑块
- 如果该 block 里还有同 bucket 的其他名字，则保留 bit
- 如果没有同 bucket 名字了，才清掉 bit
- `entries--`

为什么删除不能直接清 bit：

```text
bucket 17, block 3 里可能有：
file_a -> bucket 17
file_b -> bucket 17
file_c -> bucket 22
```

如果删除 `file_a`，不能直接清掉 bucket 17 的 bit3，因为 `file_b` 仍然在 block 3。

所以 v6.5 的删除逻辑是：

```text
删除 dirent
-> 扫描同一个 directory logical block
-> 如果同 bucket 还存在其他 live dirent
   -> mask bit 保持 1
-> 否则
   -> mask bit 清 0
```

### 4.3 `cryexts_dir_logical_block_for_bh()`

位置：[dir.c](/D:/Carl/cryptext4/cryexts/dir.c:283)

职责：

- 根据 `buffer_head->b_blocknr` 反查它属于目录的第几个 logical block
- 删除路径需要这个 logical block number 去更新对应 mask bit

核心流程：

```text
for logical in dir_blocks:
    if cryexts_inode_block_at(dir, logical) == bh->b_blocknr:
        return logical
```

## 5. 结构体字段说明

v6.5 没有新增 on-disk structure。它继续使用 v5.3 的目录索引块：

```c
struct cryexts_dir_index_block {
    __le32 magic;
    __le16 buckets;
    __le16 dir_blocks;
    __le32 entries;
    __u8 reserved[16];
    __le16 block_masks[CRYEXTS_DIR_INDEX_BUCKETS];
} __attribute__((packed));
```

字段含义：

- `magic`：固定为 `CRYEXTS_DIR_INDEX_MAGIC`，识别目录索引块
- `buckets`：bucket 数量，当前固定为 64
- `dir_blocks`：当前目录使用的目录数据块数量
- `entries`：目录中 live dirent 数量，包含 `.` 和 `..`
- `reserved`：预留字段；开启 metadata checksum 时，前 4 字节存放 checksum
- `block_masks`：每个 bucket 对应一个 16 bit mask，每个 bit 对应一个目录逻辑块

mask 语义：

```text
block_masks[17] = 0x0005
binary = 0000 0000 0000 0101

bit0 = 1 -> logical directory block 0 里有 bucket 17 的名字
bit2 = 1 -> logical directory block 2 里有 bucket 17 的名字
```

注意：这里的 bit position 是目录逻辑块号，不是物理块号。真正物理块号通过：

```text
cryexts_inode_block_at(dir, logical_block)
```

从 inode 的 block mapping 中取得。

## 6. v6.5 测试内容

运行：

```bash
chmod +x scripts/smoke_v6_5_dir_index_maintenance.sh
./scripts/smoke_v6_5_dir_index_maintenance.sh
```

测试步骤：

1. 创建 v6 image，启用 block group、extent、dir index、policy table、metadata checksum、journal v2
2. 创建一个测试目录
3. 创建 320 个较长文件名，让目录扩展到多个 directory block
4. 删除前 40 个文件
5. rename 30 个文件
6. 创建 10 个 hard link
7. 卸载后用 `cryexts_dir_index_inspect` 检查索引
8. 用 `cryextsck` 校验 entries、mask、checksum
9. remount 后验证 lookup 仍然工作
10. 再次 `cryextsck` clean

预期结果：

```text
v6.5 directory-index maintenance smoke test passed
```

## 7. 当前边界

v6.5 已实现：

- directory index 增量 add
- directory index 增量 remove
- rename/link/unlink 通过 add/delete 路径维护索引
- inspect 输出索引密度
- fsck 校验 entries 是否匹配真实目录项数量

v6.5 尚未实现：

- 真正多层 HTree
- bucket split
- directory block 数量突破 12
- index leaf block
- `block_masks` 从 16 bit 扩展到更大覆盖范围

这些建议放到 v6.6 或 v6.7 做，因为那时需要同时升级目录数据存储模型。
