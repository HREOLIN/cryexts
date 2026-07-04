# CRYEXTS v7.0 多块 GDT 基线说明

## 1. 这份文档说明什么

这份文档专门描述：

```text
重构后的 v7.0 在代码层面到底实现了什么
```

核心范围只有一个：

```text
让 mkfs 能写出完整的多块 GDT 区域
```

注意：

- 这不等于内核已经支持多块 GDT 挂载
- 这也不等于 `cryextsck` 已经能完整校验多块 GDT

v7.0 的职责是先把“写布局”这一步打通。

## 2. 布局变化

在旧模型里，group0 基本等价于默认假设：

```text
block 0 : superblock
block 1 : GDT
block 2 : root block bitmap
block 3 : root inode bitmap
block 4..7 : root inode table
block 8 : root directory
```

这套布局隐含了一个前提：

```text
整个 GDT 永远只占 1 个 block
```

这就是单块 GDT 假设。

v7.0 之后，group0 的布局变成：

```text
block 0 : superblock
block 1..N : 完整 GDT 区域
下一块 : root block bitmap
下一块 : root inode bitmap
接下来 4 块 : root inode table
下一块 : root directory
```

也就是说：

- GDT 变大多少
- root group 的元数据就整体向后移动多少

这就是 v7.0 最关键的布局变化。

## 3. 关键字段说明

## 3.1 `struct cryexts_super_block`

### `group_desc_table_start`

- 含义：GDT 区域起始 block
- v7.0 行为：仍然从 block `1` 开始

### `group_desc_table_blocks`

- 含义：整个 GDT 区域占用多少个 block
- v7.0 行为：由 `mkfs` 动态计算，不再固定为 `1`

### `block_bitmap_block`

- 含义：root group 的 block bitmap 所在 block
- v7.0 行为：移动到 GDT 区域后面

### `inode_bitmap_block`

- 含义：root group 的 inode bitmap 所在 block
- v7.0 行为：跟着 root block bitmap 一起后移

### `inode_table_start`

- 含义：root group inode table 起始 block
- v7.0 行为：跟着前面的元数据一起后移

### `root_dir_block`

- 含义：root directory 数据块
- v7.0 行为：不再默认是固定的旧位置

## 3.2 `struct cryexts_group_desc`

这个结构本身在 v7.0 没有重新设计。

变化不在于 descriptor 的字段变了，而在于：

```text
descriptor 的数量增多后，它们可能横跨多个 GDT blocks
```

最重要的字段还是这些：

### `group_start`

- 含义：这个 group 的起始 block

### `blocks_count`

- 含义：这个 group 真实拥有多少个 block

### `block_bitmap_block`

- 含义：该 group 的 block bitmap 位置

### `inode_bitmap_block`

- 含义：该 group 的 inode bitmap 位置

### `inode_table_start`

- 含义：该 group inode table 起始位置

### `free_blocks_count`

- 含义：该 group 当前空闲 block 数

### `free_inodes_count`

- 含义：该 group 当前空闲 inode 数

## 4. 关键函数说明

## 4.1 `tools/mkfs.cryexts.c`

### `main()`

这是 v7.0 最核心的修改点。

它现在负责：

1. 根据 `group_count` 计算 `gdt_blocks`
2. 重新计算 group0 元数据布局
3. 分配一段连续内存保存完整 GDT
4. 把完整 GDT 一次性写出到磁盘

关键公式：

```text
gdt_blocks =
  ceil(group_count * sizeof(struct cryexts_group_desc) / CRYEXTS_BLOCK_SIZE)
```

这意味着：

- 只要 group 数量够大
- GDT 就自然扩展成多块

## 4.2 `super.c`

### `cryexts_validate_super()`

这个函数现在新增了两类校验：

1. GDT 区域范围校验
   - `group_desc_table_start + group_desc_table_blocks`
   - 不能越过整个设备结尾

2. GDT 容量校验
   - `group_desc_table_blocks * block_size`
   - 必须足够容纳 `group_count` 个 descriptor

### `cryexts_fill_super()`

这个函数在 v7.0 的策略是：

```text
先拒绝多块 GDT，而不是错误读取
```

也就是说：

- 如果 `group_desc_table_blocks > 1`
- 当前 mount 路径直接返回不支持

原因很简单：

- 现有代码还只会读取第一块 GDT
- 如果继续挂载，会把后面的 descriptors 丢掉

所以 v7.0 的设计哲学是：

```text
先让错误显式化
不要让错误静默发生
```

## 4.3 `tools/cryextsck.c`

### `validate_super()`

和内核一样，先补完整 GDT 的范围与容量校验。

### `main()`

目前 `cryextsck` 依然只支持单块 GDT 读取模型。

所以 v7.0 的处理是：

- 发现 `group_desc_table_blocks > 1`
- 直接报：

```text
multi-block GDT is not yet supported by cryextsck
```

这样就不会继续拿第一块 GDT 假装当整张表去校验。

## 4.4 `tools/cryexts_gdt_inspect.c`

这是 v7.0 新增的辅助工具。

作用：

- 直接从磁盘读取整段 GDT
- 打印 superblock 里与 GDT 相关的关键信息
- 打印每个 group descriptor
- 如果 metadata checksum 启用，还会打印 stored / expected checksum

这个工具很重要，因为它不依赖：

- mount
- fsck

所以即使后两者暂时还没升级，我们也能先验证：

```text
mkfs 到底有没有把多块 GDT 写对
```

## 5. smoke 流程说明

`scripts/smoke_v7_0_multi_gdt.sh` 的验证逻辑是：

1. 计算一个足以跨越单块 GDT 上限的镜像大小
2. 创建镜像
3. 执行：

```bash
./mkfs.cryexts -f -G -M <image>
```

4. 执行：

```bash
./cryexts_gdt_inspect <image>
```

5. 验证：
   - `gdt_blocks > 1`
   - `expected_gdt_blocks == gdt_blocks`
   - root bitmap 已经不在旧固定位置

6. 再执行：

```bash
./cryextsck <image>
```

7. 确认 `cryextsck` 当前会明确拒绝

所以这个 smoke 的设计目标不是“系统已经全链路支持多块 GDT”，而是：

```text
证明 v7.0 已经把“写多块 GDT”这条能力线拉起来了
```

## 6. 一个具体例子

假设：

- `sizeof(group_desc) = 76`
- `block_size = 4096`

那么单个 GDT block 最多能容纳：

```text
4096 / 76 = 53 个 descriptor
```

如果现在要创建一个拥有 `55` 个 groups 的文件系统：

```text
required_gdt_blocks = ceil(55 * 76 / 4096) = 2
```

这时布局就会变成：

```text
block 0 : superblock
block 1..2 : GDT
block 3 : root block bitmap
block 4 : root inode bitmap
block 5..8 : root inode table
block 9 : root directory
```

而不是旧模型里的：

```text
block 1 : GDT
block 2 : root block bitmap
block 3 : root inode bitmap
block 4..7 : root inode table
block 8 : root directory
```

这个例子就能直观看出：

- 一旦 GDT 扩展成多块
- root group 的元数据布局必须整体后移
- 旧代码如果还按固定 block 解释，必然读错

## 7. v7.0 的边界

最后再强调一次：

v7.0 已经完成的是：

- 多块 GDT 写出能力
- superblock / GDT 基础一致性校验
- inspect 工具
- 新 smoke
- 老路径安全拒绝

v7.0 还没有完成的是：

- 多块 GDT 挂载
- 多块 GDT 同步写回
- 多块 GDT fsck 全量支持

所以最准确的一句话是：

```text
v7.0 是 Version 7 的“多块 GDT 产出基线”，
不是多块 GDT 的最终完成版。
```
