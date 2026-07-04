# CRYEXTS v6.5 代码处理说明

## 1. 这份文档看什么

这份文档按代码路径解释 v6.5 的 directory index maintenance：

```text
create/link
-> cryexts_add_entry()
-> cryexts_dir_index_add_name()

unlink/rmdir/rename-old
-> cryexts_delete_entry()
-> cryexts_dir_index_remove_name()

fsck
-> validate_dir_block()
-> 校验 mask 和 entries
```

## 2. 新增 helper 总览

v6.5 在 [dir.c](/D:/Carl/cryptext4/cryexts/dir.c:1) 新增三个 helper：

```c
static int cryexts_dir_index_add_name(struct inode *dir,
                                      const struct qstr *name,
                                      unsigned int logical_block);

static int cryexts_dir_index_remove_name(struct inode *dir,
                                         const struct qstr *name,
                                         struct buffer_head *bh,
                                         unsigned int logical_block);

static int cryexts_dir_logical_block_for_bh(struct inode *dir,
                                            struct buffer_head *bh,
                                            unsigned int *logical_block);
```

它们都是 `static`，只服务于目录实现内部。

## 3. `cryexts_dir_index_add_name()`

位置：[dir.c](/D:/Carl/cryptext4/cryexts/dir.c:183)

参数：

- `dir`：被修改的目录 inode
- `name`：新增的目录项名字
- `logical_block`：这个目录项写入的目录逻辑块号

职责：

- 确认目录索引 feature 是否启用
- 确保目录 inode 已经有 index block
- 读取 index block
- 计算 bucket
- 设置对应 mask bit
- 递增 `entries`
- 写回 index block

核心代码语义：

```text
bucket = cryexts_dir_hash(name) % CRYEXTS_DIR_INDEX_BUCKETS
mask = index.block_masks[bucket]
mask |= 1 << logical_block
index.entries++
store(index)
```

为什么需要 `logical_block`：

```text
block_masks 记录的是目录逻辑块，不是物理块
```

如果新增名字写入 logical block 3，就必须设置 bit3。

失败路径：

```text
index load/store 失败 -> cryexts_dir_index_rebuild()
```

这个设计让增量维护是快路径，全量重建是安全兜底。

## 4. `cryexts_dir_index_remove_name()`

位置：[dir.c](/D:/Carl/cryptext4/cryexts/dir.c:216)

参数：

- `dir`：被修改的目录 inode
- `name`：被删除的目录项名字
- `bh`：被删除目录项所在的 directory block buffer
- `logical_block`：该 buffer 对应的目录逻辑块号

职责：

- 读取 index block
- 计算被删除名字的 bucket
- 扫描同一个 directory logical block
- 判断这个 block 里是否还有同 bucket 的 live dirent
- 必要时清除 mask bit
- 递减 `entries`
- 写回 index block

关键点：

```text
删除一个名字，不代表这个 block 不再属于该 bucket
```

所以不能直接清 bit，必须先扫描同 block。

核心伪代码：

```text
bucket = hash(deleted_name) % 64
bucket_still_in_block = false

for each live dirent in logical_block:
    if hash(dirent.name) % 64 == bucket:
        bucket_still_in_block = true
        break

if !bucket_still_in_block:
    block_masks[bucket] &= ~(1 << logical_block)

entries--
store(index)
```

## 5. `cryexts_dir_logical_block_for_bh()`

位置：[dir.c](/D:/Carl/cryptext4/cryexts/dir.c:283)

参数：

- `dir`：目录 inode
- `bh`：当前 directory block 的 buffer_head
- `logical_block`：输出 logical block number

职责：

- 删除路径里，`cryexts_find_entry()` 返回的是 `buffer_head`
- directory index 需要的是 logical block number
- 这个 helper 负责从 physical block 反查 logical block

核心流程：

```text
for i in 0..dir_blocks-1:
    if cryexts_inode_block_at(dir, i) == bh->b_blocknr:
        logical_block = i
```

## 6. create/link 路径

入口：

- `cryexts_create()`
- `cryexts_link()`
- `cryexts_symlink()`
- `cryexts_mkdir()`

都会进入：

```c
cryexts_add_entry()
```

位置：[dir.c](/D:/Carl/cryptext4/cryexts/dir.c:551)

v6.5 修改后流程：

```text
1. 查重，确认名字不存在
2. 在已有 directory block 里找空闲 rec_len
3. 如果没有空间，则分配新的 directory block
4. 写入 dirent
5. 写回目录 inode
6. cryexts_dir_index_add_name(dir, name, logical_block)
```

这里第 6 步是 v6.5 的核心变化。

## 7. unlink/rmdir 路径

入口：

- `cryexts_unlink()`
- `cryexts_rmdir()`
- `cryexts_rename()` 删除 old name 或 victim name

都会进入：

```c
cryexts_delete_entry()
```

位置：[dir.c](/D:/Carl/cryptext4/cryexts/dir.c:681)

v6.5 修改后流程：

```text
1. cryexts_find_entry() 找到目标 dirent 和 bh
2. cryexts_dir_logical_block_for_bh() 反查 logical block
3. journal record 当前 directory block
4. 清空 dirent inode/name_len/file_type
5. 写回目录 inode
6. cryexts_dir_index_remove_name(dir, name, bh, logical_block)
```

这里第 2 步和第 6 步是 v6.5 的核心变化。

## 8. rename 路径

入口：

```c
cryexts_rename()
```

位置：[dir.c](/D:/Carl/cryptext4/cryexts/dir.c:1138)

v6.5 没有新增专门的 rename index 更新函数，因为 rename 已经拆成：

```text
new_dir add new name
old_dir delete old name
```

如果目标名字已存在，还会先：

```text
new_dir delete victim name
```

所以 rename 的索引维护自然落到：

```text
cryexts_dir_index_add_name()
cryexts_dir_index_remove_name()
```

这比给 rename 单独写一套索引逻辑更不容易出现路径分叉。

## 9. inspect 工具变化

文件：[tools/cryexts_dir_index_inspect.c](/D:/Carl/cryptext4/cryexts/tools/cryexts_dir_index_inspect.c:1)

原有输出：

- `inode`
- `index_block`
- `magic`
- `buckets`
- `dir_blocks`
- `entries`
- `bucket[N]=0x....`

v6.5 新增：

- `active_buckets`
- `mask_refs`

计算逻辑：

```text
active_buckets = 非 0 bucket mask 的数量
mask_refs = 所有 bucket mask 中 bit=1 的总数
```

用途：

- 判断 hash 是否分散到多个 bucket
- 判断目录项是否跨多个 directory block
- 辅助 smoke test 验证索引不是空壳

## 10. fsck 变化

文件：[tools/cryextsck.c](/D:/Carl/cryptext4/cryexts/tools/cryextsck.c:2008)

原有校验：

```text
对每个 live dirent:
    bucket = hash(name) % 64
    要求 block_masks[bucket] 包含当前 logical block bit
```

v6.5 新增：

```text
live_entries++
最后要求 index.entries == live_entries
```

如果不匹配，会报告：

```text
cryextsck: directory index entry count mismatch
```

这个校验专门保护增量维护路径。

## 11. smoke test 走读

文件：[scripts/smoke_v6_5_dir_index_maintenance.sh](/D:/Carl/cryptext4/cryexts/scripts/smoke_v6_5_dir_index_maintenance.sh:1)

测试创建：

```text
FILE_COUNT=320
DELETE_COUNT=40
RENAME_COUNT=30
LINK_COUNT=10
```

预期 live entries：

```text
2 + FILE_COUNT - DELETE_COUNT + LINK_COUNT
```

这里的 `2` 是：

```text
.
..
```

rename 不改变 entries，因为它本质上是 add new name + delete old name。

测试还要求：

```text
dir_blocks >= 2
active_buckets >= 32
mask_refs >= active_buckets
至少一个 bucket mask 有多个 bit
```

这样可以确认：

- 目录确实跨多个 block
- bucket 不是只有一个
- mask 不是一直 `0x0001`
- 增量维护后 fsck 仍然 clean

## 12. 当前实现边界

v6.5 的边界很明确：

```text
优化维护路径，不改变磁盘格式
```

它没有实现：

- directory index v2
- index leaf block
- bucket split
- directory block 数量超过 12

原因是这些会要求目录数据存储也一起升级。v6.5 先把 namespace 变更下的索引一致性做稳。
