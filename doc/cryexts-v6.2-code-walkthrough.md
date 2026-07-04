# CRYEXTS v6.2 代码处理说明

## 1. 这份文档看什么

这份文档专门讲：

```text
一次真实的 regular file 写入、查找、truncate，
代码是怎么一步一步走到 multi-leaf extent tree 的
```

## 2. 场景设定

假设我们在挂载后的文件系统里创建并写入一个大文件：

```text
/mnt/ext/tree_v2.bin
```

并且：

- 它是 regular file
- 文件系统版本已经是 v6 代
- extents feature 已开启
- extent tree feature 已开启

这时我们希望它使用的不是旧模型，而是：

```text
extent tree v2
```

## 3. 新 inode 是怎么决定走 v6.2 的

### 3.1 `cryexts_new_inode()`

文件位置：[inode.c](/D:/Carl/cryptext4/cryexts/inode.c:2146)

职责：

- 创建新 inode 的内存态 `cryexts_inode_info`
- 决定它是不是 extent-backed inode
- 决定它是不是 extent tree v2 inode

关键逻辑可以理解为：

```text
如果：
  这是 regular file
  并且版本 >= v6
  并且 extents feature 已启用
  并且 extent_tree feature 已启用

那么：
  inode_flags |= EXTENTS
  inode_flags |= EXTENT_TREE_V2
  extent_inline_max = CRYEXTS_EXTENT_TREE_ROOT_REFS
```

## 4. 普通写入时如何分配 block

### 4.1 `cryexts_resolve_block()`

文件位置：[inode.c](/D:/Carl/cryptext4/cryexts/inode.c:1123)

职责：

- 先查当前逻辑块是否已经有映射
- 没有映射时，必要时分配新块
- 对 extent inode，把新块追加到 extent 结构

它在 v6.2 下的核心流程是：

1. 线性遍历当前 extent 视图
2. 如果已命中，就直接返回现有 physical block
3. 如果未命中且 `create == false`，返回空映射
4. 如果未命中且 `create == true`
   - 先尝试和最后一条 extent 合并
   - 合并不了就新分配一个块
   - 然后调用 `cryexts_append_extent_entry()`

## 5. 为什么 `cryexts_append_extent_entry()` 还能继续用

### 5.1 统一入口

文件位置：[inode.c](/D:/Carl/cryptext4/cryexts/inode.c:551)

这个函数现在变成了一个统一入口：

- 如果是旧格式 inode，就走旧的 inline/overflow 逻辑
- 如果是 tree-v2 inode，就转到 `cryexts_append_extent_entry_v2()`

## 6. 新 extent 如何进入 leaf

### 6.1 `cryexts_append_extent_entry_v2()`

文件位置：[inode.c](/D:/Carl/cryptext4/cryexts/inode.c:500)

职责：

- 向 tree-v2 inode 追加一个新的 extent

它的处理步骤是：

1. 看当前有没有 leaf
2. 如果没有 leaf，或者最后一个 leaf 已满
   - 分配一个新的 leaf block
3. 对目标 leaf 的 extent 数组做 `krealloc`
4. 把新 extent 填到最后一项
5. `leaf->entries++`
6. 如果是新 leaf，则 `extent_leaf_count++`
7. 调用 `cryexts_refresh_extent_tree_v2_refs()`

### 6.2 `cryexts_alloc_extent_leaf_block()`

文件位置：[inode.c](/D:/Carl/cryptext4/cryexts/inode.c:427)

职责：

- 分配 leaf metadata block
- 清零这个新块
- 把 block 号写回 leaf cache 和 root ref

## 7. root refs 是怎么维护的

### 7.1 `cryexts_refresh_extent_tree_v2_refs()`

文件位置：[inode.c](/D:/Carl/cryptext4/cryexts/inode.c:455)

职责：

- 根据每个 leaf cache 的当前状态，重建 root refs 摘要

它会为每个 leaf 填这些信息：

- `ref->leaf_block`
- `ref->entries`
- `ref->checksum`
- `ref->logical_start`

## 8. mount / iget 时怎么把 v6.2 inode 读回来

### 8.1 `cryexts_init_inode_blocks()`

文件位置：[inode.c](/D:/Carl/cryptext4/cryexts/inode.c:681)

职责：

- 解析磁盘 inode
- 初始化内存态 `cryexts_inode_info`

在 v6.2 分支里，它会：

1. 读 `extent header`
2. 确认 root header参数匹配 v2
3. 从 inode `reserved[]` 中取出 root refs
4. 对每个 root ref 调用 `cryexts_load_extent_leaf()`

### 8.2 `cryexts_load_extent_leaf()`

文件位置：[inode.c](/D:/Carl/cryptext4/cryexts/inode.c:338)

职责：

- 根据 `leaf_block` 读磁盘 leaf
- 校验 leaf header
- 校验 checksum
- 把 extent[] 缓存到内存

## 9. 为什么 `cryexts_extent_entry()` 很关键

### 9.1 统一线性视图

文件位置：[inode.c](/D:/Carl/cryptext4/cryexts/inode.c:186)

职责：

- 把多 leaf 结构包装成一个“线性 extent 数组视图”

很多原有逻辑都可以继续写成：

```text
for i in total_entries:
    extent = cryexts_extent_entry(blocks, i)
```

## 10. truncate 时怎么跨 leaf 回收

### 10.1 `cryexts_free_blocks_from()`

文件位置：[inode.c](/D:/Carl/cryptext4/cryexts/inode.c:1282)

在 v6.2 分支里，它的处理流程是：

1. 从 `leaf_index = 0` 开始扫
2. 对每个 leaf 内的每条 extent：
   - 完整落在 `keep_blocks` 之前，保留
   - 完整落在 `keep_blocks` 之后，整条释放并删除
   - 部分跨过边界，就只保留前半段
3. 如果某个 leaf 被删空：
   - 释放这个 leaf metadata block
   - 调用 `cryexts_drop_extent_leaf()`
4. 最后再刷新 root refs

### 10.2 `cryexts_drop_extent_leaf()`

文件位置：[inode.c](/D:/Carl/cryptext4/cryexts/inode.c:476)

职责：

- 删除一个空 leaf
- 把后面的 leaf 向前挪
- 清理末尾缓存槽位
- 重新刷新 root refs

## 11. 落盘时是怎么写回 root 和 leaf 的

### 11.1 `cryexts_write_inode_to_disk()`

文件位置：[inode.c](/D:/Carl/cryptext4/cryexts/inode.c:1964)

在 v6.2 分支下，它的动作顺序是：

1. 清空 `disk_inode->reserved`
2. 写入 root extent header
3. 先遍历所有 leaf，调用 `cryexts_write_extent_leaf()`
4. 再把 `extent_root_refs[]` 整体拷回 inode `reserved[]`

### 11.2 `cryexts_write_extent_leaf()`

文件位置：[inode.c](/D:/Carl/cryptext4/cryexts/inode.c:384)

职责：

- 重写 leaf block header
- 写出 extent[]
- 重新计算 checksum
- 回填对应 root ref 的 checksum

## 12. fsck 是怎么理解它的

### 12.1 `validate_inode()`

文件位置：[tools/cryextsck.c](/D:/Carl/cryptext4/cryexts/tools/cryextsck.c:1481)

v6.2 分支的检查顺序可以概括成：

1. 看 inode flags，是不是 `EXTENT_TREE_V2`
2. 检查 root header
3. 逐个 root ref 检查
4. 读取 leaf block
5. 检查 leaf header
6. 必要时检查 leaf checksum
7. 调 `validate_extent_array()` 检查 leaf 里的 extent[]

### 12.2 `validate_extent_array()`

职责：

- 检查 extent 的逻辑空间是否连续
- 检查 extent 的物理块是否都位于数据区内
- 检查这些物理块是否在位图中标记为 used
- 更新 `used_blocks`

## 13. 一个完整写入案例 walkthrough

假设文件一开始为空。

### 第一次写入

1. `cryexts_new_inode()` 创建 regular inode
2. 它被标记为 `EXTENT_TREE_V2`
3. `cryexts_resolve_block(logical=0, create=true)`
4. 当前没有 extent
5. 分配 data block
6. `cryexts_append_extent_entry_v2()` 发现还没有 leaf
7. 分配 `leaf[0]`
8. 在 `leaf[0]` 中追加第一条 extent
9. `cryexts_refresh_extent_tree_v2_refs()` 把 `root_ref[0]` 建起来

### 持续顺序写入

1. 如果物理块连续，就继续扩展最后一条 extent 的 `length`
2. 如果不连续，就在当前 leaf 中追加新 extent
3. 如果当前 leaf 满了，就创建 `leaf[1]`
4. `root_ref[1]` 随之出现

### 卸载落盘

1. `cryexts_write_extent_leaf()` 先把所有 leaf 写盘
2. `cryexts_write_inode_to_disk()` 再把 root refs 写回 inode

### 重新挂载

1. `cryexts_init_inode_blocks()` 读 root refs
2. `cryexts_load_extent_leaf()` 把每个 leaf 读回来
3. 后续读写继续走统一 extent 视图

### truncate

1. `cryexts_free_blocks_from()` 回收后半段 extent
2. 空 leaf 会被释放
3. root refs 被重新压紧
4. 再落盘

## 14. 一句话结论

从代码视角看，`v6.2` 的本质是：

```text
把“extent 映射表”从单块思维升级成了
“root 摘要 + leaf 内容”的两层模型，
并让创建、查找、落盘、重挂载、truncate、fsck
都开始理解这件事
```
