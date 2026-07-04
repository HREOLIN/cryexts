# CRYEXTS Version 7 需求重构

## 1. Version 7 为什么要重构

之前的 `Version 7` 需求主要围绕：

- USB demo
- open-source-ready
- commercial-evaluation-ready

这条线没有错，但它默认了一个前提：

```text
Version 6 的底层布局已经足够稳定、足够可扩展
```

现在实践结果已经说明，这个前提还不成立。

当前最直接、最现实的瓶颈不是“怎么做更好的 USB 演示”，而是：

```text
当前 GDT（group descriptor table）实际上只能按单块处理，
导致文件系统规模上限很低，raw device / 大分区场景无法自然扩展。
```

所以 `Version 7` 需要重构目标。

从现在开始，`Version 7` 不再优先定义为：

```text
demo-ready 版本
```

而应优先定义为：

```text
突破单 GDT 限制、让 CRYEXTS 进入可扩展容量阶段的版本
```

一句话总结：

```text
Version 7 = 从“功能型原型”推进到“布局可扩展原型”
```

## 2. 当前真实瓶颈

### 2.1 `mkfs` 只能生成单块 GDT

当前 `tools/mkfs.cryexts.c` 中有明确限制：

```text
Current v4.1 mkfs supports only one GDT block
```

这意味着：

- `group_count * sizeof(struct cryexts_group_desc)` 不能超过一个 block
- 一旦 group 数量太多，`mkfs` 会直接拒绝格式化
- raw device / 大容量分区场景会在格式化阶段就卡住

### 2.2 内核挂载路径也只按单块 GDT 读取

当前 `super.c` 的 GDT 加载方式是：

- 只读 `group_desc_table_start` 这一块
- `sbi->gdt_bh` 只保存一个 `buffer_head`
- `sbi->groups` 直接指向这一个 block 的 `b_data`

这意味着：

- 即使以后 `mkfs` 写出多块 GDT
- 当前内核也只能看到第一块
- 后续 group descriptor 会完全丢失

### 2.3 `cryextsck` 也只按单块 GDT 处理

当前 `tools/cryextsck.c` 的 GDT 读取逻辑同样是：

- 只从 `group_desc_table_start` 读取一个 block
- 然后把这一个 block 当成完整 GDT

这意味着：

- 多块 GDT 一旦引入
- `cryextsck` 如果不升级，就会误判或漏判

### 2.4 当前容量上限实际上很低

已知条件：

- block size = `4096`
- group descriptor size 约 `76`
- 每个 block 容纳的 descriptor 数量约 `53`
- 每 group 默认 `4096` blocks

因此单 GDT block 能表示的 group 数大约只有：

```text
53 groups
```

对应最大数据规模大约只有：

```text
53 * 4096 * 4096 bytes
≈ 848 MiB
```

这就是为什么：

- 小 image 可以
- 真实大分区不自然
- USB raw-device 场景会很别扭

所以 `Version 7` 的主问题已经很清楚：

```text
先把 CRYEXTS 的 group descriptor 扩展能力做好，
再谈更大设备、稳定 demo 和后续工程化。
```

## 3. Version 7 新目标

重构后，`Version 7` 的主目标定义为：

```text
支持多块 GDT
+ 支持更大 group_count
+ 支持更大 raw device / image
+ 让 mkfs / mount / fsck / checksum / allocator 在多 GDT 条件下保持一致
```

这版的核心不是加新文件系统语义，而是：

- 扩大可支持的容量范围
- 扩大 block-group 数量范围
- 让底层布局从“实验性小规模”升级到“真正可扩展”

## 4. Version 7 核心需求

## 4.1 多块 GDT 生成功能

`mkfs.cryexts` 必须支持：

- 根据 `group_count` 自动计算 `group_desc_table_blocks`
- 当 `group_count * sizeof(group_desc)` 超过一个 block 时，自动分配多个 GDT block
- 正确写出完整 GDT 区域
- 正确更新 superblock 中：
  - `group_desc_table_start`
  - `group_desc_table_blocks`

### 目标

不再出现：

```text
Current v4.1 mkfs supports only one GDT block
```

### 最低要求

- 至少支持跨越 `1` 个 GDT block 的边界
- 能稳定创建明显大于 `848 MiB` 的 CRYEXTS 文件系统

## 4.2 内核多块 GDT 加载能力

挂载路径必须支持完整读取多块 GDT。

### 当前问题

当前只有：

- `sbi->gdt_bh`
- `sbi->groups = (struct cryexts_group_desc *)sbi->gdt_bh->b_data`

这套模型只适合单块 GDT。

### Version 7 要求

内核需要升级成以下两种模型之一：

### 方案 A：GDT `buffer_head` 数组

引入类似：

- `struct buffer_head **gdt_bhs`
- 每块 GDT 各自一个 `buffer_head`

优点：

- 和块缓存语义贴近
- 后续 checksum / dirty / sync 更直观

### 方案 B：集中内存镜像 + 多块回写

引入类似：

- 一段连续内存保存完整 GDT 副本
- 同时维护每个 GDT block 的写回逻辑

优点：

- 遍历 descriptor 更简单

### 推荐

`Version 7` 推荐优先使用：

```text
buffer_head 数组 + groups 连续内存镜像
```

即：

- 保留每个磁盘块的 `buffer_head`
- 同时维护一份完整 GDT 内存视图，供校验、遍历、分配器读取

这样更利于：

- checksum 更新
- sync
- `cryextsck` 语义对齐
- 后续 debug

## 4.3 superblock 信息必须真正生效

当前 `group_desc_table_blocks` 字段虽然存在，但没有变成完整的运行时能力。

`Version 7` 要求这个字段真正参与：

- mount 时的读取范围计算
- checksum 遍历范围
- sync 范围
- `put_super` 释放范围
- `cryextsck` 读取范围

一句话说：

```text
group_desc_table_blocks 不能只是“磁盘上有字段”，
而必须成为真正控制 GDT 生命周期的核心参数。
```

## 4.4 多块 GDT 下的 checksum 一致性

当前 group descriptor checksum 已经存在，但默认是在单块 GDT 视角下工作。

`Version 7` 需要明确：

- 所有 group descriptor 的 checksum 都必须可更新
- 多块 GDT 下更新 bitmap / free count 后，受影响的 descriptor 必须被正确回写
- `sync_metadata()` 必须覆盖所有 GDT blocks
- `cryextsck` 必须验证全部 groups 的 checksum

### 重点

这里不是“校验算法是否变化”，而是：

```text
校验覆盖范围必须从“第一块 GDT”
扩展到“整个 GDT 区域”
```

## 4.5 分配器必须在大 group_count 下继续正确工作

多块 GDT 解决的不是只有 `mkfs`。

如果 `group_count` 变大，以下路径都必须重新确认：

- `cryexts_group_first_block()`
- `cryexts_group_blocks()`
- `cryexts_group_inode_table_start()`
- `cryexts_group_free_blocks()`
- `cryexts_group_free_inodes()`
- `cryexts_alloc_inode_goal()`
- `cryexts_alloc_block_goal()`
- `cryexts_mark_bitmap_dirty()`

### 要求

在 group 数显著变大时：

- 分配器不能越界
- free count 不能错
- locality 策略仍然成立
- journal / checksum 路径不能漏写对应 descriptor

## 4.6 `cryextsck` 多块 GDT 支持

`cryextsck` 必须升级成完整多块 GDT 读取模型。

### 要求

- 按 `group_desc_table_blocks * block_size` 读取完整 GDT 区域
- 能遍历全部 group descriptors
- 校验：
  - group range
  - bitmap block
  - inode bitmap block
  - inode table range
  - free block / inode count
  - checksum

### 目标

保证：

```text
mkfs 能创建多块 GDT 文件系统
内核能挂载
cryextsck 也能完整理解
```

## 4.7 大容量测试正式进入主线

之前 `Version 7` 偏重 USB demo。

现在重构后，测试主线应该改成：

### 第一类：跨越单 GDT 边界的 image 测试

例如：

- `900 MiB`
- `1 GiB`
- `2 GiB`

重点验证：

- `mkfs`
- `mount`
- `mkdir/write/read`
- `umount/remount`
- `cryextsck`

### 第二类：多 group 行为测试

重点验证：

- group 遍历
- 分配器跨 group 工作
- free count 更新
- checksum 更新

### 第三类：raw device 测试

等多块 GDT 稳定后，再恢复 raw-device / USB demo 主线。

也就是说：

```text
先解布局扩展能力
再回到真实设备演示
```

## 5. 建议的结构调整

## 5.1 建议新增/调整的运行时结构

### `struct cryexts_sb_info`

当前关键字段：

- `struct buffer_head *gdt_bh`
- `struct cryexts_group_desc *groups`

建议调整为：

- `struct buffer_head **gdt_bhs`
  - 含义：保存每一个 GDT block 的 `buffer_head`
- `void *gdt_storage`
  - 含义：完整 GDT 的连续内存镜像
- `struct cryexts_group_desc *groups`
  - 含义：指向 `gdt_storage` 中的 descriptor 数组

### 字段职责

#### `gdt_bhs`

- 用于逐块同步
- 用于逐块 `mark_buffer_dirty`
- 用于逐块释放

#### `gdt_storage`

- 用于统一读取全部 descriptors
- 用于 allocator / checksum / validate 访问

#### `groups`

- 保持上层调用接口不变
- 降低对 allocator / helper 的改动量

## 5.2 建议新增/调整的核心函数

### `cryexts_load_gdt(struct super_block *sb)`

功能：

- 读取全部 GDT blocks
- 填充 `gdt_bhs`
- 构建 `gdt_storage`
- 设置 `groups`

### `cryexts_sync_gdt(struct super_block *sb)`

功能：

- 把 `gdt_storage` 拆回各个 GDT blocks
- 逐块更新 `buffer_head`
- 逐块 `sync_dirty_buffer`

### `cryexts_unload_gdt(struct cryexts_sb_info *sbi)`

功能：

- 释放 `gdt_bhs`
- 释放 `gdt_storage`
- 清空 `groups`

### `cryexts_verify_group_checksums(struct super_block *sb)`

功能：

- 对完整 GDT 区域中的全部 descriptors 做校验

### `cryexts_update_group_checksums(struct super_block *sb)`

功能：

- 更新完整 GDT 区域中的全部 descriptors checksum

## 6. Version 7 非目标

为了防止继续发散，重构后的 `Version 7` 不应优先做：

- snapshot
- reflink
- dedupe
- quota
- online resize
- 更复杂的加密新算法
- 更大范围的 open-source 包装工作
- 更大的 USB demo 脚本矩阵

原因很简单：

```text
如果底层 GDT 扩展能力还没打通，
继续追这些上层目标会让版本主线失焦。
```

## 7. 重构后的 Version 7 版本拆分

## 7.1 `v7.0`

主题：

- 多块 GDT 设计与 `mkfs` 支持

交付：

- `mkfs.cryexts` 支持写出多块 GDT
- 新文档说明 GDT 布局演进
- 首个跨单 GDT 边界的大镜像 smoke

## 7.2 `v7.1`

主题：

- 内核多块 GDT 挂载与同步

交付：

- 内核可读取完整 GDT
- `sync_metadata()` / `put_super()` / checksum 覆盖全部 GDT blocks
- 挂载/卸载/重挂载 smoke

## 7.3 `v7.2`

主题：

- `cryextsck` 与校验路径升级

交付：

- `cryextsck` 多块 GDT 读取与校验
- checksum / free count / layout 检查在大 group_count 下成立

## 7.4 `v7.3`

主题：

- 大容量 raw-device / USB 回归

交付：

- 恢复大分区 raw-device 测试
- 恢复更真实的 USB demo 路径
- 重新评估发布工程化问题

## 8. Version 7 MVP 定义

重构后的 `Version 7 MVP` 建议定义为：

```text
CRYEXTS 可以稳定支持跨越单 GDT block 边界的文件系统规模，
并且 mkfs / mount / allocator / checksum / cryextsck
都能在多块 GDT 条件下保持一致。
```

更具体地说，至少要达到：

- 能创建大于 `848 MiB` 的 CRYEXTS 文件系统
- 能正常挂载
- 能进行基础目录和文件操作
- 能正常卸载并重挂载
- `cryextsck` 仍然 clean

## 9. 一句话总结

如果 `Version 6` 解决的是：

```text
把文件系统主要能力做出来
```

那么重构后的 `Version 7` 要解决的是：

```text
把文件系统的底层布局扩展能力做出来，
让它不再被单 GDT block 卡死
```

