# CRYEXTS V4.1 block groups 与 group-aware allocator

## 1. 这一阶段的目标

V4.0 只是把 Version 4 的 superblock 地基立起来。

V4.1 才开始真正把磁盘从：

```text
单组 + 全盘一个 bitmap 区
```

推进到：

```text
多组 + 每组自己的 bitmap / inode table
```

这一步是 CRYEXTS 从教学型小盘布局迈向更真实 ext2/ext4 风格的重要拐点。

## 2. 当前结构变化

V4.1 引入：

- `group descriptor table`
- 每组自己的：
  - block bitmap
  - inode bitmap
  - inode table
- 多组 free count
- group-aware inode/block allocation

## 3. 当前 allocator 的语义

旧模型：

```text
全盘顺序扫 bitmap
```

V4.1 模型：

```text
先找合适的 group
再在该 group 内找空闲 inode / block
```

当前版本是“最小多组版”：

- 已经不是单 bitmap
- 但还没有复杂局部性策略和 buddy allocator

## 4. 当前 V4.1 的落地方式

当前最小实现里：

- `mkfs.cryexts -G` 创建 block-group 布局
- superblock 打开 `BLOCK_GROUPS` incompat flag
- mount 时读取 group descriptor table
- inode 定位按：
  - `ino -> group -> inode table block`
- block / inode 分配按：
  - 逐组扫描 free count
  - 在目标组的 bitmap 内找空位

## 5. 这一步为什么重要

后续这些能力都依赖它：

- V4.2 journal 的分组元数据更新
- V4.3 extent 的连续块分配
- 更大目录和更大文件
- 更接近真实磁盘局部性

## 6. 当前边界

V4.1 还没有：

- journal
- replay
- extent
- xattr
- directory index
- 更复杂的组选择策略

所以正确理解是：

```text
V4.1 = 真正进入多组磁盘布局
```

而不是：

```text
已经拥有 ext4 那种成熟分配器
```
