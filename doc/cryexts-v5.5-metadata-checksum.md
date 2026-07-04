# CRYEXTS V5.5 metadata checksum 设计说明

## 1. 这一版解决什么问题

到 `V5.4` 为止，`cryexts` 已经有了：

- block groups
- journal header checksum
- extent overflow
- directory index
- policy table

但是这些结构大多还是“能读能写”，还不是“每次读都能确认自己没坏”。

`V5.5` 的目标就是把这个缺口补上：

- 让关键 metadata 在落盘时带 checksum
- 让 mount / inode load / dir lookup / `cryextsck` 都能校验 checksum
- 让 `metadata checksum` 这个 `ro_compat feature` 不再只是 flag，而是真正生效

一句话：

```text
V5.5 = 给 v5 已经引入的关键元数据结构补上一条统一的一致性校验链
```

## 2. 这次覆盖了哪些 metadata

当前 `V5.5` 首批覆盖：

- superblock
- group descriptor
- policy table block
- directory index block
- extent overflow block
- journal header

其中：

- `journal header` 之前已经有 checksum，这一版把它纳入“统一 metadata 校验语义”
- `extent overflow block` 因为现有 block 内部没有独立预留 32-bit checksum 槽位，所以它的 checksum 存在 inode 的 extent-root 预留区里

## 3. checksum 的基本思路

当前实现使用：

```text
metadata_csum_type = FNV1A32
```

计算输入不是只有 block 内容，还会额外混入：

- `fs_generation`
- metadata block 号
- metadata 类型 tag

所以可以理解成：

```text
checksum = FNV1a32(
    fs_generation + blocknr + metatype + metadata_bytes
)
```

这样做的作用是：

- 同样内容搬到不同 block，不会得到完全一样的 checksum
- 不同类型 metadata，不会共享同一套“裸内容校验值”

## 4. 各结构的 checksum 放在哪里

### 4.1 superblock

放在：

- `superblock.reserved[0..3]`

校验范围：

- `struct cryexts_super_block`
- 跳过 checksum 自己这 4 个字节

### 4.2 group descriptor

放在：

- `group_desc.reserved[0..3]`

每个 group descriptor 各自有一个 checksum。

### 4.3 policy table block

放在：

- `policy_table_block.reserved[0..3]`

校验范围：

- 整个 policy table block
- 跳过 reserved 开头那 4 字节 checksum

### 4.4 directory index block

放在：

- `dir_index_block.reserved[0..3]`

校验范围：

- 整个 directory index block
- 跳过 reserved 开头那 4 字节 checksum

### 4.5 extent overflow block

这块稍微特殊。

overflow extent block 自己的 block header 里没有合适的 32-bit checksum 槽位，所以当前做法是：

- overflow block 的 checksum 不放在 overflow block 自己里面
- 而是放在 inode 的 extent-root 预留区里

也就是：

```text
inode.reserved
├── extent root header
├── inline extents
├── overflow_block
├── overflow_entries
└── overflow_checksum
```

所以校验流程是：

```text
先从 inode 里读出 overflow_checksum
-> 再读取 overflow block
-> 重新计算这个 block 的 checksum
-> 对比是否一致
```

## 5. kernel 侧什么时候校验

### 5.1 mount 时

mount 主线现在会做：

```text
read super
-> validate super fields
-> verify super checksum
-> read GDT
-> verify group checksums
-> load bitmaps
-> load policy table
-> verify policy table checksum
-> journal replay / orphan cleanup
-> mount
```

### 5.2 目录 lookup 时

目录如果启用了 `dir index`：

```text
load dir index block
-> verify directory index checksum
-> 再继续 hash lookup
```

### 5.3 inode extent 初始化时

如果 inode 使用了 `extent tree overflow`：

```text
load extent overflow block
-> 读取 inode 里记录的 overflow_checksum
-> 重新计算 overflow block checksum
-> 不一致则返回 EUCLEAN
```

## 6. 写盘时怎么维护 checksum

### 6.1 superblock

只要 superblock 元数据变化，比如：

- `next_ino`
- `next_data_block`
- `state`
- `free_blocks_count`
- `free_inodes_count`

在写回前都会先更新 super checksum。

### 6.2 group descriptor

group free count 变化后：

- 先重算对应的 group descriptor checksum
- 再进入 journal / dirty buffer

### 6.3 dir index block

每次 rebuild / store dir index：

- 先生成 block mask
- 再写入 checksum
- 再 mark dirty

### 6.4 extent overflow block

每次写 overflow extents：

- 先把 extent header + entries 写到 block
- 计算这个 overflow block 的 checksum
- 缓存在 inode memory state
- 最后写 inode 时把 checksum 一起落盘

## 7. `cryextsck` 现在会检查什么

`V5.5` 之后，`cryextsck` 除了看结构字段合法性，还会额外检查：

- superblock checksum
- group descriptor checksum
- policy table checksum
- directory index checksum
- extent overflow checksum
- journal header checksum

所以它现在不只是检查：

```text
这个字段像不像对的
```

还会检查：

```text
这个 metadata block 是不是被完整、原样地保存下来了
```

## 8. smoke 测试如何验证

`smoke_v5_5_metadata_checksum.sh` 做了三类事：

1. 创建启用 `-M` 的 v5.5 image
2. 在挂载态实际跑出：
   - policy table
   - directory index
   - extent overflow
3. 对原始镜像再复制一份，主动篡改 directory index checksum，确认 `cryextsck` 会报错

所以这份 smoke 既覆盖了：

- 正常写入
- 正常读回
- clean fsck

也覆盖了：

- checksum 被破坏时的失败路径

## 9. 当前边界

这一版还没有做：

- inode table 自身 checksum
- xattr block checksum
- block bitmap / inode bitmap checksum
- 自动 repair metadata checksum
- 多种 checksum 算法协商

所以 `V5.5` 的准确定位是：

```text
metadata checksum MVP
```

不是最终版的全覆盖 metadata protection。

## 10. 一句话总结

`V5.5` 做成的核心闭环是：

```text
mkfs 写出 checksum
-> kernel 读 metadata 时校验 checksum
-> fsck 离线校验 checksum
-> 主动篡改后能被检测出来
```

这说明 `metadata checksum` 到这一版已经不再只是 superblock 上的一个开关，而是真正进入了 `cryexts` 的运行时和离线检查路径。
