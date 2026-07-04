# CRYEXTS V5.5 代码改动说明

## 1. 这一版的目标

`V5.5` 的目标很明确：

- 把 `metadata checksum` 从磁盘格式预留，推进成真正工作的运行时能力

重点不是新增新的 namespace / mapping 功能，而是增强已有 v5 元数据结构的一致性校验能力。

## 2. 新增与修改的核心文件

### 2.1 `metadata.c`

文件：

- [metadata.c](/D:/Carl/cryptext4/cryexts/metadata.c:1)

这是这一版新增的公共 helper。

主要提供：

- `cryexts_metadata_csum_enabled()`
- `cryexts_update_super_checksum()`
- `cryexts_verify_super_checksum()`
- `cryexts_update_group_checksums()`
- `cryexts_verify_group_checksums()`
- `cryexts_dir_index_set_checksum()`
- `cryexts_dir_index_checksum_valid()`
- `cryexts_policy_table_checksum_valid()`
- `cryexts_extent_overflow_checksum()`

这一步把 checksum 算法从“散落在各处”整理成了统一入口。

### 2.2 `cryexts_fs.h`

文件：

- [cryexts_fs.h](/D:/Carl/cryptext4/cryexts/cryexts_fs.h:1)

补充了：

- `CRYEXTS_METADATA_CSUM_FNV1A32`
- extent overflow checksum 在 inode reserved 内的 offset 宏

作用是把 `v5.5` 的 on-disk checksum 布局正式固定下来。

### 2.3 `cryexts.h`

文件：

- [cryexts.h](/D:/Carl/cryptext4/cryexts/cryexts.h:1)

主要新增：

- metadata checksum helper 的声明
- `struct cryexts_inode_info` 里的 `extent_overflow_checksum`

这让内核内存态 inode 也能携带 overflow block 的 checksum。

### 2.4 `super.c`

文件：

- [super.c](/D:/Carl/cryptext4/cryexts/super.c:1)

这一版在这里做了几件关键事：

- mount 时验证 superblock checksum
- 读取 GDT 后验证 group checksum
- `sync_metadata()` 前刷新 super / group checksum
- unmount 和 state 变化时同步更新 super checksum
- 更严格校验 `metadata_csum_type`，目前只接受 `FNV1A32`

### 2.5 `balloc.c`

文件：

- [balloc.c](/D:/Carl/cryptext4/cryexts/balloc.c:1)

这里的核心变化是：

- `cryexts_mark_bitmap_dirty()` 在写 GDT 前先重算 group checksum
- `cryexts_mark_super_dirty()` 在写 super 前先重算 super checksum

也就是说，free count 更新不再只是“字段改了就 dirty”，而是：

```text
字段改了
-> checksum 也一起更新
-> 再 journal / dirty
```

### 2.6 `dir.c`

文件：

- [dir.c](/D:/Carl/cryptext4/cryexts/dir.c:1)

这一版把 directory index block 接进了 checksum 逻辑：

- `cryexts_dir_index_load()` 读取时先验 checksum
- `cryexts_dir_index_store()` 写回时先写 checksum

这样 hash 目录索引不再只是“magic 正确就算过”，而是对整个 index block 做一致性校验。

### 2.7 `inode.c`

文件：

- [inode.c](/D:/Carl/cryptext4/cryexts/inode.c:1)

这里是 `extent overflow` 的主战场。

主要新增：

- 从 inode reserved 读写 `overflow_checksum`
- `cryexts_load_extent_overflow()` 读取 overflow block 时校验 checksum
- `cryexts_write_extent_overflow()` 写回 overflow block 时重算 checksum
- `cryexts_write_inode_to_disk()` 把 checksum 一起落回 inode
- `cryexts_validate_inode()` 增加 overflow checksum 校验

这一步让 `v5.2` 的 overflow extent block 真正具备“损坏可检测”的能力。

### 2.8 `crypto.c`

文件：

- [crypto.c](/D:/Carl/cryptext4/cryexts/crypto.c:304)

这里补了一步：

- `cryexts_load_policy_table()` 在加载 policy table 时校验 checksum

所以 `v5.4` 已经工作的 policy table，在 `v5.5` 之后不只是“能加载”，还要“checksum 也对”。

### 2.9 `tools/mkfs.cryexts.c`

文件：

- [tools/mkfs.cryexts.c](/D:/Carl/cryptext4/cryexts/tools/mkfs.cryexts.c:1)

这一版让 `mkfs` 真正写出 checksum：

- superblock checksum
- group descriptor checksum
- policy table checksum

也就是说，`-M` 不再只是设置：

- `features_ro_compat`
- `metadata_csum_type`

而是会真正生成带 checksum 的 image。

### 2.10 `tools/cryextsck.c`

文件：

- [tools/cryextsck.c](/D:/Carl/cryptext4/cryexts/tools/cryextsck.c:1)

这是离线校验侧最大的增强点。

新增理解：

- superblock checksum
- group descriptor checksum
- policy table checksum
- directory index checksum
- extent overflow checksum
- metadata checksum type 的精确校验

这意味着 `cryextsck` 现在除了做结构合法性检查，还能做完整性检查。

### 2.11 `Makefile`

文件：

- [Makefile](/D:/Carl/cryptext4/cryexts/Makefile:1)

主要改动：

- 把 `metadata.o` 加入模块构建列表

### 2.12 新增 smoke

文件：

- [scripts/smoke_v5_5_metadata_checksum.sh](/D:/Carl/cryptext4/cryexts/scripts/smoke_v5_5_metadata_checksum.sh:1)

这份 smoke 组合了：

- v5.2 extent overflow
- v5.3 dir index
- v5.4 policy table
- v5.5 metadata checksum corruption check

## 3. 这版的核心运行逻辑

可以把 `V5.5` 的主线理解成：

```text
mkfs
-> 写 super/group/policy checksum

mount
-> 验 super checksum
-> 验 group checksum
-> load policy table 时验 checksum

directory lookup
-> load dir index block
-> 验 checksum

extent overflow load
-> 读 inode 里记录的 overflow checksum
-> 读 overflow block
-> 重新计算并比对

fsck
-> 离线重复上述关键校验
```

## 4. 当前边界

这一版还没有做：

- inode table checksum
- xattr block checksum
- bitmap checksum
- metadata checksum repair
- 多算法 metadata checksum

所以 `V5.5` 不是“所有 metadata 都已有校验”，而是：

- 先把最关键、最像 v5 新引入结构的那几类 metadata 跑通

## 5. 一句话总结

`V5.5` 的本质不是“又多了几个 reserved 字节的用途”，而是：

- 让 super / group / policy / dir-index / extent-overflow 这些元数据开始具备统一的完整性校验语义

这使得 `cryexts` 的 `Version 5` 从“结构更像真实文件系统”，进一步走向“出错时也更像真实文件系统那样能发现问题”。
