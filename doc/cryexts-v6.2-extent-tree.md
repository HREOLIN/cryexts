# CRYEXTS v6.2 Multi-Leaf Extent Tree 设计说明

## 1. 设计目标

`v6.2` 的目标不是一步到位做完整 B-tree，而是先把 `v5.2` 的：

```text
inline extents + 1 overflow block
```

升级成：

```text
inode root + 多个 leaf block
```

## 2. 总体结构

### 2.1 inode root

在 `v6.2` 里，inode `reserved[]` 的前部不再存普通 inline extent 数组，而是存：

- `struct cryexts_extent_header`
- `struct cryexts_extent_root_ref refs[CRYEXTS_EXTENT_TREE_ROOT_REFS]`

可以把它理解成：

```text
inode root 只做目录页
leaf block 才是实际存 extent entry 的地方
```

### 2.2 leaf block

每个 leaf block 的磁盘布局是：

```text
extent header
+ extent[0]
+ extent[1]
+ ...
```

## 3. on-disk 结构体逐字段说明

### 3.1 `struct cryexts_extent_header`

文件位置：[cryexts_fs.h](/D:/Carl/cryptext4/cryexts/cryexts_fs.h:160)

字段说明：

- `magic`
  含义：extent 结构魔数。

- `entries`
  含义：当前块里实际有多少个有效条目。
  对 root 来说，它表示 leaf 引用数。
  对 leaf 来说，它表示 extent entry 数。

- `max`
  含义：当前块最多能容纳多少个条目。
  对 root 来说，固定是 `CRYEXTS_EXTENT_TREE_ROOT_REFS`。
  对 leaf 来说，固定是 `CRYEXTS_EXTENTS_PER_BLOCK`。

- `reserved`
  含义：在 v6.2 root 上被复用为 `tree depth`。
  当前固定是 `CRYEXTS_EXTENT_TREE_V2_DEPTH = 1`。

### 3.2 `struct cryexts_extent_root_ref`

文件位置：[cryexts_fs.h](/D:/Carl/cryptext4/cryexts/cryexts_fs.h:274)

字段说明：

- `logical_start`
  这个 leaf 覆盖的第一个逻辑块号。

- `leaf_block`
  该 leaf 的物理块号。

- `entries`
  该 leaf 当前包含多少条 extent。

- `checksum`
  该 leaf block 的 metadata checksum。

## 4. 内存结构逐字段说明

### 4.1 `struct cryexts_extent_leaf_cache`

文件位置：[cryexts.h](/D:/Carl/cryptext4/cryexts/cryexts.h:88)

字段说明：

- `block`
  leaf 的物理块号。

- `entries`
  leaf 当前缓存的 extent 数。

- `checksum`
  这个 leaf 最近一次写盘时的 checksum。

- `extents`
  指向 leaf 中 extent 数组的内存副本。

## 5. 查找流程

假设我们要查找逻辑块 `logical = 7000`。

### 第一步：先找 leaf

root refs 可能像这样：

```text
root_ref[0].logical_start = 0
root_ref[1].logical_start = 4096
root_ref[2].logical_start = 8192
```

那 `logical = 7000` 就会落到：

```text
4096 <= 7000 < 8192
```

也就是 `root_ref[1]` 对应的 leaf。

### 第二步：再在 leaf 里找 extent

进入 `leaf[1]` 后，再顺序扫描它的 extent[]，找到覆盖 `7000` 的那一条 extent，再算出物理块号。

一句话概括：

```text
root 负责缩小搜索范围
leaf 负责找到最终映射
```

## 6. 写入增长流程

### 6.1 最后一条 extent 可合并

如果：

- 当前逻辑块正好接在最后一条 extent 后面
- 新分配到的物理块也正好连续

那么直接扩展最后一条 extent 的 `length`。

### 6.2 最后一条 extent 不可合并

如果不能合并，就要追加新的 extent entry。

分两种情况：

1. 最后一个 leaf 还有空间
   直接把新 extent 追加到最后一个 leaf

2. 最后一个 leaf 已满
   分配新 leaf block
   在 root 里新增一个 `root_ref`
   把新的 extent 放进这个新 leaf

## 7. truncate / 回收流程

当执行 truncate 时：

1. 扫描各个 leaf
2. 找到逻辑范围在 `keep_blocks` 之后的 extent
3. 释放对应物理块
4. 如有需要缩短 extent 长度
5. 如果某个 leaf 被删空了，就把 leaf metadata block 本身也释放掉
6. 最后重新整理 root refs

## 8. 为什么 `root_ref.logical_start` 很重要

如果 root ref 里只有 `leaf_block`，没有 `logical_start`，那查找时就只能把所有 leaf 全扫一遍。

加上 `logical_start` 后，root 才能表达：

```text
这个 leaf 负责从哪个逻辑块号开始的一段范围
```

于是查找时能先做一层粗定位。

## 9. 为什么这还不是完整的 extent B-tree

虽然名字里有 tree，但 `v6.2` 还不是 full B-tree，原因是：

- 当前深度固定为 1
- root 只引用 leaf，不引用 internal node
- 没有 split/rebalance/merge 算法
- leaf 个数上限固定为 `CRYEXTS_EXTENT_TREE_ROOT_REFS`

所以更准确地说，它是：

```text
two-level extent tree MVP
```

## 10. 一个带数字的例子

假设：

- `CRYEXTS_EXTENT_TREE_ROOT_REFS = 4`
- 每个 leaf 最多装 `170` 条 extent

那么 v6.2 最多就能保存：

```text
4 * 170 = 680 条 extent
```

而 `v5.2` 的上限大致是：

```text
3 条 inline + 1 个 overflow block
```

所以 v6.2 提升的不是一丁点，而是把映射条目上限显著拉高了。

## 11. smoke 测试如何证明它真的工作了

`v6.2` smoke 的关键不是只看 “文件写进去没报错”，而是要看 inspect 输出。

如果输出类似：

```text
tree_v2=1
leaf_count=3
root_ref[0].leaf_block=4115
root_ref[1].leaf_block=6220
root_ref[2].leaf_block=9012
leaf[0].header.entries=170
leaf[1].header.entries=170
leaf[2].header.entries=24
```

就说明：

- inode 已经不是旧的 overflow 模式
- root 下确实挂了多个 leaf
- extent entry 已经跨多个 leaf 分布

这也是为什么 smoke 不是简单写一个几十 MB 文件，而是会：

1. 按 4KB 粒度逐块写目标文件
2. 每写完 1 块，就立刻创建 1 个 blocker 小文件
3. 强行打断后续的连续物理分配

这样更容易得到大量 `len=1` 或很短的 extent，足够把第一个 leaf 填满，并逼出第二个 leaf。

## 12. 一句话结论

`v6.2` 的 multi-leaf extent tree，可以理解成：

```text
先用一个低风险的两层树模型，
把 v5.2 “单 overflow block” 的硬上限打破，
并为 v6.3 之后继续做 sparse file / hole punch / 更强映射语义打基础
```
