# CRYEXTS V5.2 代码变更说明

## 1. 这一版改了什么

V5.2 把原先的：

```text
inline extents only
```

升级成：

```text
inode root inline extents
+ single overflow extent block
```

这意味着现在 extent regular file 不再只受 inode 内少量 extent 槽位限制。

## 2. 关键代码修改

### 2.1 `cryexts_fs.h`

新增或明确了：

- `CRYEXTS_LEGACY_INLINE_EXTENTS`
- `CRYEXTS_EXTENT_ROOT_INLINE_EXTENTS`
- `CRYEXTS_EXTENTS_PER_BLOCK`
- `CRYEXTS_EXTENT_ROOT_OVERFLOW_OFFSET`

核心作用是把：

- 老的 4-inline 布局
- 新的 3-inline + overflow 指针布局

都定义清楚。

### 2.2 `cryexts.h`

扩展了 `struct cryexts_inode_info`：

- `extent_inline_max`
- `extent_overflow_entries`
- `overflow_extents`
- `extent_overflow_block`

这让内存态 inode 能同时持有：

- inline extents
- overflow extents
- overflow block 元数据

### 2.3 `inode.c`

这是本次主修改点。

新增了几类逻辑：

- 识别旧 extent root 和新 extent root
- 读取 overflow extent block
- 写回 overflow extent block
- append extent 时 inline 满后切到 overflow
- truncate 时同时处理 inline 和 overflow
- block count 统计包含 overflow metadata block
- inode 校验支持新旧两种 extent root 格式

### 2.4 `tools/cryextsck.c`

这次同步让 `cryextsck` 也理解：

- 新 extent root header
- overflow block 指针与 entry 数量
- overflow block header
- overflow extent entries
- overflow block ownership
- regular file block count 需要包含 overflow block

### 2.5 `scripts/smoke_v5_2_extent_tree.sh`

新增 smoke：

- 写入较大的 extent 文件
- 迫使文件超过 inline-only 能力
- remount 后再次校验
- truncate 后再次校验
- 前后 `cryextsck` clean

## 3. 为什么这次要改 inode root

之前 `reserved[116]` 里除了 extent 之外，还有：

- `xattr_block`
- `encryption_policy_id`
- `next_orphan`

也就是说 trailer 已经不是空白区了。

如果还继续把更多 inline extent 硬塞进去，会和 trailer 抢空间。

所以 V5.2 才改成：

```text
extent header
+ 3 inline extents
+ overflow block 指针
+ overflow entries
+ inode extra trailer
```

这是一种更稳定的演进方式。

## 4. 和旧镜像的关系

这次实现保留了旧格式兼容：

- 老 extent inode 仍然可以按 4-inline 方式解析
- 新 extent overflow 路径只在新 root 格式下使用

所以这次不是粗暴替换，而是：

```text
old inline-only format
+ new overflow-capable format
coexist
```

## 5. 当前边界

这一版仍然没有做：

- 多级 extent tree
- extent rebalance
- 真正的 extent index node

所以它的准确定位是：

```text
extent overflow MVP
```

但对 Version 5 来说，这已经把“大文件映射能力”往前推进了关键一步。

## 6. 补充：v5.2 overflow 测试中的 journal/sync 修正

在把 `smoke_v5_2_extent_tree.sh` 改成“分块写 + 强制命中 overflow”之后，
我们暴露出了一个更底层的问题：

```text
write_iter
-> write_inode_to_disk
-> journal_record_block
-> sync_metadata
-> sync_blockdev
```

这会让普通写热路径直接进入整块设备同步。

在碎片化、大文件、反复更新 inode 的场景下，这种策略很容易把当前写线程自己卡在
块设备写回里，表现为：

- 用户态 `dd` 卡住
- 内核日志出现 `task dd blocked for more than 120 seconds`
- 调用栈落在 `cryexts_sync_metadata -> sync_blockdev`

所以这里做了一个最小修正：

- `cryexts_journal_record_block()` 不再在 record 热路径里调用整盘
  `cryexts_sync_metadata()`
- 改为只同步：
  - 当前 journal payload block
  - 当前 journal header block

也就是说，把：

```text
journal record
-> full device sync
```

改成：

```text
journal record
-> sync current journal payload
-> sync current journal header
```

这样：

- journal 记录顺序仍然有基本保证
- 但不会把每一次普通文件写都拖进整盘 flush

而真正的较重落盘边界，仍然保留给：

- `fsync`
- `sync_fs`
- `journal_commit`
- `umount`

这一步不是在改 extent overflow 格式，而是在给 v5.2 的真实写入路径“降同步粒度”，
避免测试阶段被写回死等拖住。

## 7. 补充：普通文件写路径补齐 journal 事务边界

在 overflow 验证真正跑通之后，`cryexts_extent_inspect` 已经能确认：

- `inline_entries = 3`
- `overflow_block != 0`
- `overflow_entries != 0`

这说明 extent overflow 路径本身已经工作。

但随后 `cryextsck` 报出：

```text
journal header is valid but superblock is not in recovery state
```

这暴露的不是 extent 结构问题，而是普通文件写路径的 journal 边界不完整。

之前目录/xattr 这类 metadata-heavy 操作，本来就有：

```text
journal_begin()
-> metadata update
-> journal_commit()
```

但 regular file 的 `write_iter()` 之前并没有完整包进这条边界里。
它虽然会在 block 分配、inode 落盘时触发 `journal_record_bh()`，
但没有显式事务收口，于是可能出现：

```text
journal header 仍然 valid
但 superblock recovery state 已经被清掉
```

所以这里补了两个点：

### 7.1 `write_iter()` 补齐事务边界

现在 regular file 写路径改成：

```text
journal_begin
-> extent/block allocate
-> data write
-> inode size / i_blocks / extent update
-> write_inode_to_disk
-> journal_commit
```

如果中途失败，则：

```text
journal_abort
```

### 7.2 `truncate shrink` 补齐事务边界

对于 regular file 的缩小操作：

- `free_blocks_from()`
- tail block zeroing
- inode size / block count 更新
- inode 落盘

现在也放到同一笔 journal 事务里。

这样 `truncate` 不再只是“修改了一半 metadata，然后依赖零散 record”，
而是有了一个清晰的 begin/commit 收口。

### 7.3 这一修正的目标

这一步的目标不是让 journal 变成完整成熟实现，而是先保证：

```text
regular file data-path metadata update
也有明确事务边界
```

这样 `v5.2` 在做：

- overflow extent 扩展
- remount
- truncate
- fsck

时，journal header / superblock recovery state 的语义会更一致。
