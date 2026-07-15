# CRYEXTS 现阶段 Block Layer 布局总结

## 1. 这份文档说明什么

这份文档只描述一件事：

```text
当前主线代码里，CRYEXTS 在块设备上的实际落盘布局是什么样
```

这里强调的是“当前主线实际布局”，不是历史版本演进。

当前开发节奏虽然已经推进到 `Version 10`，但真正的完整功能镜像主线，仍然主要建立在下面这套格式能力之上：

- block groups
- multi-GDT
- extents / extent tree
- xattr / policy table
- dir index
- metadata checksum
- journal v2

也就是说，今天如果我们创建一个“功能比较完整”的 CRYEXTS 镜像，重点应该按这份文档理解它的 block layer 布局。

补充一个容易混淆的点：

- `mkfs.cryexts` 默认 `fs_version` 仍从编译期默认版本起步
- 一旦启用 `journal v2`，当前 mkfs 会把镜像版本切到 `V6`

所以你平时看到的“完整版功能镜像”，通常就是：

```text
V6 superblock layout
+ V7 之后补强的 multi-GDT / runtime path
+ V8/V9/V10 的工具链、兼容性、观测性与性能演进
```

## 2. 先记住几个固定常量

来自 [cryexts_fs.h](/D:/Carl/cryptext4/cryexts/cryexts_fs.h) 的当前关键常量：

- block size 固定为 `4096`
- superblock 位于 `block 0` 内偏移 `1024`
- 默认每个 block group 的大小是 `4096` blocks
- 默认每个 group 的 inode table 固定占 `4` blocks
- 每个 group 的 inode 数量默认是 `4 * (4096 / sizeof(inode)) = 56`
- GDT 从 `block 1` 开始
- journal 默认预留 `512` blocks

所以从 block layer 角度看，CRYEXTS 现在是一个：

```text
4KiB block
+ group-based metadata layout
+ tail journal area
```

的文件系统。

## 3. 当前主线的总布局总览

如果启用了完整功能集合，整体可以抽象成：

```text
block 0
  -> superblock (位于 block 0 的 1024 偏移)

block 1 .. N
  -> GDT (group descriptor table，可能占多块)

root group metadata
  -> root block bitmap
  -> root inode bitmap
  -> root inode table (4 blocks)
  -> root directory block
  -> policy table block (如果启用)

group 1 .. group K-2
  -> 每个 group 自己的 block bitmap
  -> inode bitmap
  -> inode table (4 blocks)
  -> data area

last group
  -> 前半段仍是本 group metadata + data area
  -> 尾部预留 journal area
     - control
     - descriptor
     - payload area
     - commit
```

一句话理解：

```text
superblock 负责全局入口
GDT 负责描述每个 group
每个 group 自己维护 bitmap/inode table
journal 放在最后一个 group 的尾部
```

## 4. superblock 在 block layer 里的角色

`struct cryexts_super_block` 是整个布局的总入口，最关键的是这些字段：

- `blocks_count`
  - 整个设备一共有多少 blocks
- `block_bitmap_block`
  - root group 的 block bitmap 在哪
- `inode_bitmap_block`
  - root group 的 inode bitmap 在哪
- `inode_table_start`
  - root group 的 inode table 起点
- `inode_table_blocks`
  - root group inode table 占多少 blocks
- `root_dir_block`
  - 根目录数据块位置
- `first_data_block`
  - 数据区起始参考位置
- `journal_block`
  - journal 区起始 block
- `journal_blocks`
  - journal 区总大小
- `group_count`
  - 一共多少个 block groups
- `blocks_per_group`
  - 每个 group 的标准大小
- `inodes_per_group`
  - 每个 group 的 inode 数
- `group_desc_table_start`
  - GDT 起始 block
- `group_desc_table_blocks`
  - GDT 总共占多少 blocks
- `policy_table_block`
  - policy table 所在 block

所以 superblock 并不直接保存所有布局细节，它做的是：

```text
给出全局入口
+ 告诉内核 GDT 在哪
+ 告诉内核 root metadata 在哪
+ 告诉内核 journal 在哪
```

## 5. GDT 与 block groups 的关系

### 5.1 GDT 是什么

GDT 就是一张数组表，里面每一项都是一个 `struct cryexts_group_desc`。

每个 descriptor 描述一个 group 的元数据位置和资源计数，例如：

- `group_start`
- `blocks_count`
- `block_bitmap_block`
- `inode_bitmap_block`
- `inode_table_start`
- `inode_table_blocks`
- `free_blocks_count`
- `free_inodes_count`
- `used_dirs_count`

### 5.2 为什么现在必须支持 multi-GDT

当前一个 descriptor 的大小大约是 `76` bytes。
一个 `4096` 字节 block 大约只能放 `53` 个 descriptors。

所以当 group 数量超过这个上限时：

```text
GDT 就不可能只占 1 个 block
```

当前主线已经支持：

```text
GDT = block 1 开始的一整段连续区域
group_desc_table_blocks = ceil(group_count * desc_size / block_size)
```

这也是 v7 主线以后最关键的 block layer 变化之一。

## 6. root group 的特殊布局

root group 和普通 group 不完全一样。

在启用 block groups 时，mkfs 的布局公式是：

```text
block 0
  superblock

block 1 .. (1 + gdt_blocks - 1)
  GDT region

block (1 + gdt_blocks)
  root block bitmap

block (1 + gdt_blocks + 1)
  root inode bitmap

next 4 blocks
  root inode table

next 1 block
  root directory data block

next 1 block
  policy table block (if enabled)
```

所以 root group 的 metadata 会随着 `gdt_blocks` 变大而整体后移。

这点非常重要，因为它意味着：

```text
root_block_bitmap_block
root_inode_bitmap_block
root_inode_table_start
root_dir_block
都不再是固定常量位置
```

它们必须由 superblock 字段动态给出。

## 7. 普通 group 的标准模板

对于 `group > 0` 的普通组，当前主线采用统一模板：

```text
group_start + 0
  -> block bitmap

group_start + 1
  -> inode bitmap

group_start + 2 .. +5
  -> inode table (4 blocks)

剩余部分
  -> data area
```

也就是说，普通 group 的前 6 个 blocks 基本固定给 group metadata：

- `1` block bitmap
- `1` inode bitmap
- `4` inode table

因此每个完整 group 默认可供普通数据使用的初始空间大约是：

```text
4096 - 6 = 4090 blocks
```

这也是你之前经常看到：

```text
free_blocks=4090
free_inodes=56
```

的直接原因。

## 8. 为什么每个 group 只有 56 个 inode

这不是因为 bitmap 用了 `uint64`，而是因为当前 inode table 就只给了 `4` 个 blocks。

公式很直接：

```text
inodes_per_group =
  inode_table_blocks_per_group * (block_size / inode_size)
```

当前默认值是：

- `inode_table_blocks_per_group = 4`
- `block_size = 4096`
- `sizeof(struct cryexts_inode)` 约为 `292`

所以：

```text
4096 / 292 = 14
4 * 14 = 56
```

因此这里的瓶颈不是 bitmap 位宽，而是：

```text
每个 group 只预留了 4 个 inode table blocks
```

## 9. policy table 的位置

如果启用了 policy table，当前布局是：

```text
policy_table_block = root_dir_block + 1
```

也就是说它放在 root group 里，紧跟在 root directory block 后面。

它的职责不是存文件数据，而是维护：

- `policy_id`
- `context`
- default policy

从 block layer 角度看，它就是 root group 中的一个特殊元数据块。

## 10. journal v2 的物理布局

当前完整功能镜像的 journal 放在：

```text
最后一个 group 的尾部
```

mkfs 的布局规则是：

- 先计算最后一个 group 有多少空间
- 再从尾部切出 `journal_blocks`
- 默认最大预留 `512` blocks

journal v2 内部固定为：

```text
journal_block + 0
  -> control block

journal_block + 1
  -> descriptor block

journal_block + 2 .. journal_block + journal_blocks - 2
  -> payload area

journal_block + journal_blocks - 1
  -> commit block
```

也就是：

```text
control -> descriptor -> payload -> commit
```

### 10.1 为什么开头是 control

因为 `control` 记录的是整个 journal 区域的全局状态：

- 当前是不是 journal v2
- 当前 active sequence 是多少
- descriptor 在哪
- payload 从哪开始
- commit 在哪

所以 mount 时要先读 control，再决定是否继续读取 descriptor 和 commit。

### 10.2 payload 存什么

payload 不是“逻辑操作日志”，而是：

```text
home metadata block 的块镜像副本
```

所以 replay 时，本质上是把 payload 里的镜像块拷回对应 home block。

## 11. 数据区和普通文件的关系

block layer 只负责回答：

```text
某个逻辑文件块最终映射到哪个物理 block
```

当前主线里，这一层的物理数据承载方式主要有：

- 普通 direct/indirect 历史路径
- extent
- extent tree v2

无论上层是普通文件、目录块、xattr overflow，最后都要落到某个 data block 上。

所以可以这样理解：

- block groups 决定“去哪个 group 找空间”
- allocator 决定“在这个 group 的哪个 block 上分配”
- extent / inode block map 决定“这个文件如何引用这些物理 blocks”

## 12. 一个 128MiB 镜像的典型例子

你前面经常跑的 128MiB 镜像，大致是：

- 总 blocks: `32768`
- blocks per group: `4096`
- group count: `8`

这时 GDT 通常还只占 1 block，于是前几个块看起来像：

```text
block 0   : superblock (offset 1024)
block 1   : GDT
block 2   : root block bitmap
block 3   : root inode bitmap
block 4-7 : root inode table
block 8   : root directory
block 9   : policy table (if enabled)
...
last group tail
  -> journal area
```

而当设备继续变大、group 数超过单块 GDT 能容纳的 descriptor 数时：

```text
block 1..N 就会一起变成 GDT region
root group metadata 随之整体后移
```

这就是“multi-GDT 对 block layout 的直接影响”。

## 13. 现阶段 block layer 的职责边界

如果只从 block layer 看，当前主线主要负责下面几件事：

1. 定义整盘的物理布局
2. 把设备切成多个 block groups
3. 为每个 group 保留 bitmap 和 inode table
4. 维护 root group 的特殊元数据位置
5. 维护 GDT 和每个 group descriptor
6. 在最后一个 group 尾部预留 journal v2
7. 为 inode/extent/xattr/dir block 提供实际物理块承载空间

一句话总结：

```text
现阶段 CRYEXTS 的 block layer，本质上就是
“superblock + multi-GDT + per-group metadata + tail journal + data blocks”
这一整套物理空间组织系统。
```

## 14. 你后面看代码时，建议按这个顺序理解

如果你要继续顺着源码往下啃，推荐顺序是：

1. [cryexts_fs.h](/D:/Carl/cryptext4/cryexts/cryexts_fs.h)
   - 先看 superblock / group_desc / journal v2 结构
2. [tools/mkfs.cryexts.c](/D:/Carl/cryptext4/cryexts/tools/mkfs.cryexts.c)
   - 看镜像是怎么按这个布局写出来的
3. [super.c](/D:/Carl/cryptext4/cryexts/super.c)
   - 看 mount 时怎么把这些布局读进来
4. [balloc.c](/D:/Carl/cryptext4/cryexts/balloc.c)
   - 看 group allocator 怎么基于这套布局分配 blocks
5. [journal.c](/D:/Carl/cryptext4/cryexts/journal.c)
   - 看 tail journal 的 begin / record / commit / replay

这样你会更容易把“磁盘布局”和“运行时分配逻辑”对上。
