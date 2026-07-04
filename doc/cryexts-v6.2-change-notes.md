# CRYEXTS v6.2 变更说明

## 1. 这一版解决了什么问题

`v5.2` 做到的是：

```text
inode inline extents
+ 1 个 overflow extent block
```

它的优点是简单，已经能突破纯 inline extents 的上限；但它仍然有一个明显边界：

- overflow 只有 1 个块
- overflow 满了以后，regular file 的 extent 映射就到顶了
- `fsck/inspect` 也只能理解 “inline + 单 overflow” 这一个模型

`v6.2` 的目标，就是把这条路径推进成一个真正更像 tree 的版本：

```text
inode root
-> 多个 leaf block
-> 每个 leaf 里继续保存 extent header + extent[]
```

一句话结论：

```text
v5.2 = single-overflow extent MVP
v6.2 = multi-leaf extent tree MVP
```

注意这里还是 `MVP`。
这一次不是完整的任意高度平衡 B-tree，也不是 ext4 那种成熟 extent tree 全形态；
但它已经真正突破了 “只能有一个 overflow block” 的上限。

## 2. 修改了哪些代码文件

### 2.1 [cryexts_fs.h](/D:/Carl/cryptext4/cryexts/cryexts_fs.h:1)

新增了 v6.2 需要的 on-disk 常量和结构：

- `CRYEXTS_INODE_FLAG_EXTENT_TREE_V2`
- `CRYEXTS_EXTENT_TREE_V2_DEPTH`
- `CRYEXTS_EXTENT_TREE_ROOT_REFS`
- `CRYEXTS_EXTENT_TREE_V2_FILE_BLOCKS_MAX`
- `CRYEXTS_EXTENT_TREE_V2_ROOT_REFS_OFFSET`
- `struct cryexts_extent_root_ref`

这些定义的意义是：

- inode 上需要一个显式 flag，说明当前 inode 不是旧的 inline/overflow 模式，而是新的 tree-v2 模式
- inode root 里不再直接塞数据 extent，而是保存若干个 leaf 引用
- 每个 leaf 引用要能告诉内核：
  - 这个 leaf 覆盖的起始逻辑块号
  - 这个 leaf 的物理块号
  - 这个 leaf 里有多少条 extent
  - 这个 leaf 的 checksum

### 2.2 [cryexts.h](/D:/Carl/cryptext4/cryexts/cryexts.h:1)

新增运行时缓存结构：

- `struct cryexts_extent_leaf_cache`

并扩展 `struct cryexts_inode_info`，让内存态 inode 能缓存：

- root 里读出来的 leaf 引用
- 每个 leaf 的 block 号
- 每个 leaf 的 extent 数组
- 当前 leaf 个数

这样做以后，运行时访问模型变成：

```text
disk inode reserved[]
-> root refs[]
-> load leaf blocks
-> cache in inode->i_private
```

### 2.3 [metadata.c](/D:/Carl/cryptext4/cryexts/metadata.c:1)

新增：

- `cryexts_extent_leaf_checksum()`

这个函数的职责很直接：

- 给 v6.2 leaf block 计算 metadata checksum
- 让 leaf block 的校验方式和现有 metadata checksum 体系保持一致

### 2.4 [inode.c](/D:/Carl/cryptext4/cryexts/inode.c:1)

这是 v6.2 改动最多的文件。

它补齐了 6 类能力：

1. 识别 tree-v2 inode
2. 从 disk inode 读取 root refs
3. 加载 leaf block 到内存缓存
4. 新增 extent 时，自动追加到最后一个 leaf，必要时分配新 leaf
5. truncate/free blocks 时，能跨 leaf 回收 extent 和 leaf metadata block
6. 落盘时，把 root refs 写回 inode，把 leaf blocks 写回磁盘

### 2.5 [tools/cryexts_extent_inspect.c](/D:/Carl/cryptext4/cryexts/tools/cryexts_extent_inspect.c:1)

增强 inspect 输出，让我们能直接看见：

- 该 inode 是否是 `tree_v2=1`
- `leaf_count`
- 每个 `root_ref[i]`
- 每个 `leaf[i]` 的 header
- 每个 `leaf[i]` 里的 extent entry

### 2.6 [tools/cryextsck.c](/D:/Carl/cryptext4/cryexts/tools/cryextsck.c:1)

`cryextsck` 开始理解 v6.2 的结构语义：

- root header 要匹配 tree-v2 格式
- root ref 的数量、逻辑起点、leaf block 都要合法
- leaf header 要合法
- leaf checksum 要合法
- leaf 里的 extents 要连续覆盖逻辑空间
- leaf metadata block 也必须在数据区内，且块位图标记为 used

## 3. 这次新增的结构体

### 3.1 `struct cryexts_extent_root_ref`

文件位置：[cryexts_fs.h](/D:/Carl/cryptext4/cryexts/cryexts_fs.h:274)

字段说明：

- `logical_start`
  含义：这个 leaf 覆盖的第一段逻辑块号起点。

- `leaf_block`
  含义：leaf block 的物理块号。

- `entries`
  含义：这个 leaf 当前有多少条 extent。

- `checksum`
  含义：这个 leaf block 的 metadata checksum。

### 3.2 `struct cryexts_extent_leaf_cache`

文件位置：[cryexts.h](/D:/Carl/cryptext4/cryexts/cryexts.h:88)

字段说明：

- `block`
  含义：该 leaf 对应的磁盘块号。

- `entries`
  含义：该 leaf 当前缓存了多少条 extent。

- `checksum`
  含义：最近一次为这个 leaf 计算并缓存的 checksum。

- `extents`
  含义：该 leaf 读入内存后的 extent 数组。

## 4. 关键函数职责

### 4.1 `cryexts_load_extent_leaf()`

职责：

- 从 `leaf_block` 读一个 extent leaf
- 校验 header
- 必要时校验 checksum
- 把 extent[] 复制到内存缓存

### 4.2 `cryexts_write_extent_leaf()`

职责：

- 把一个内存态 leaf 重新序列化成磁盘块
- 重新写 header
- 重新计算 checksum
- 更新对应的 root ref checksum

### 4.3 `cryexts_alloc_extent_leaf_block()`

职责：

- 当最后一个 leaf 已满时，分配一个新的 leaf metadata block
- 初始化它的 block 号
- 把这个 block 号记录进 root ref

### 4.4 `cryexts_refresh_extent_tree_v2_refs()`

职责：

- 根据内存态 leaf cache，回填 root refs

它会更新：

- `leaf_block`
- `entries`
- `checksum`
- `logical_start`

### 4.5 `cryexts_append_extent_entry_v2()`

职责：

- 向 tree-v2 inode 追加一条新的 extent
- 如果最后一个 leaf 还有空间，就直接追加进去
- 如果最后一个 leaf 满了，就分配新 leaf，再把新 extent 放进去

### 4.6 `cryexts_extent_total_entries()`

职责：

- 对旧格式：统计 `inline + overflow`
- 对新格式：统计所有 leaf 里的 entry 总数

### 4.7 `cryexts_extent_entry()`

职责：

- 对旧格式：返回 inline/overflow 中的第 `index` 条 extent
- 对新格式：把多个 leaf 视为一个线性 extent 序列，再返回第 `index` 条

### 4.8 `cryexts_free_blocks_from()`

职责：

- truncate 时，按 `keep_blocks` 回收多余 extent
- 当某个 leaf 被删空时，连 leaf metadata block 一起回收

### 4.9 `cryexts_write_inode_to_disk()`

职责：

- 对旧格式：写 inline extents 和 overflow 指针
- 对 v6.2：写 root header、root refs，并先把各个 leaf 落盘

### 4.10 `cryexts_validate_inode()`

职责：

- 在内核 mount/iget 路径上校验 v6.2 inode 是否自洽

## 5. 一个具体例子

假设我们往一个文件里写入大量数据，已经超出了单个 leaf 的容量。

v6.2 的结构可能会变成：

```text
inode root
  root_ref[0] -> leaf block 4115, logical_start = 0
  root_ref[1] -> leaf block 6220, logical_start = 5800

leaf[0]
  extent 0: logical 0    -> physical 20,   len 1024
  extent 1: logical 1024 -> physical 4090, len 512

leaf[1]
  extent 0: logical 5800 -> physical 9000, len 800
  extent 1: logical 6600 -> physical 12000, len 900
```

查找逻辑块 `logical = 6000` 时：

1. 先看 root refs
2. 发现 `6000` 落在 `root_ref[1]` 对应 leaf 的范围里
3. 进入 `leaf[1]`
4. 在 `leaf[1]` 的 extent 数组里找到覆盖 `6000` 的 extent
5. 再算出具体的物理块号

## 6. v6.2 smoke 测试到底测了什么

新增脚本：[scripts/smoke_v6_2_extent_tree.sh](/D:/Carl/cryptext4/cryexts/scripts/smoke_v6_2_extent_tree.sh:1)

它验证的不是简单的 “大文件写成功”，而是 4 件更具体的事：

1. 创建 v6.2 能力开启的镜像
2. 对目标文件按 4KB 一块一块写入
3. 每写完 1 块，就立刻创建 1 个 blocker file 占掉下一段连续空间
4. 这样目标文件更容易形成大量 `len=1` 的 extent，并跨过 `170` 条 extent 的 leaf 上限
5. 用 `cryexts_extent_inspect` 检查结构
6. 做一次 truncate，再跑 `cryextsck`

重点检查：

- `tree_v2=1`
- `leaf_count >= 2`
- `root_ref[1].leaf_block != 0`
- `leaf[1].header.entries > 0`

## 7. 这一版还没有做什么

`v6.2` 依然没有做到：

- 任意高度多层 extent tree
- leaf split / internal split / rebalance
- 真正的 B-tree 插入算法
- 稀疏文件 hole/unwritten extent 语义

所以准确表述应该是：

```text
v6.2 已经实现 multi-leaf extent tree MVP
但还不是 full extent B-tree
```

## 8. 一句话结论

`v6.2` 的价值，不是“又加了一个新块类型”，而是把 regular file 的 extent 映射真正从：

```text
单 inode + 单 overflow block
```

推进到了：

```text
inode root + multiple leaf blocks
```
