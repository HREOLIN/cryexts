# CRYEXTS V5.3 代码改动说明

## 1. 这一版的目标

V5.3 的核心目标是把目录查找从“全目录线性扫描”推进到“带 hash 索引过滤的目录查找”。

这版实现的是一个目录索引 MVP，重点包括：

- 增加目录索引 on-disk 格式
- 增加内核侧目录索引构建与查询逻辑
- 增加 `cryextsck` 对目录索引的理解和校验
- 增加 inspect 工具和 smoke 测试

## 2. 改了哪些文件

### 2.1 `cryexts_fs.h`

文件：

- [cryexts_fs.h](/D:/Carl/cryptext4/cryexts/cryexts_fs.h:1)

主要新增：

- `CRYEXTS_FEATURE_INCOMPAT_DIR_INDEX`
- `CRYEXTS_INODE_FLAG_DIR_INDEX`
- `CRYEXTS_DIR_INDEX_MAGIC`
- `CRYEXTS_DIR_INDEX_BUCKETS`
- `struct cryexts_dir_index_block`

这部分定义了目录索引块的磁盘格式，是整个 V5.3 的基础。

### 2.2 `cryexts.h`

文件：

- [cryexts.h](/D:/Carl/cryptext4/cryexts/cryexts.h:1)

主要补充：

- `struct cryexts_inode_info` 中新增 `dir_index_block`

作用是让内核运行时能够明确记录“这个目录 inode 的索引块物理号”。

### 2.3 `dir.c`

文件：

- [dir.c](/D:/Carl/cryptext4/cryexts/dir.c:1)

这是 V5.3 变更最多的文件，主要新增了下面这些逻辑：

- `cryexts_dir_index_feature_enabled()`
  判断超级块是否启用了目录索引 feature
- `cryexts_dir_hash()`
  对文件名做 hash，并混入 `dir_index_seed`
- `cryexts_dir_index_load()`
  从磁盘读取目录索引块
- `cryexts_dir_index_store()`
  把内存中的索引块写回磁盘
- `cryexts_dir_index_ensure()`
  确保目录已经拥有索引块，没有就先分配
- `cryexts_dir_index_rebuild()`
  扫描整个目录，重建 bucket -> block mask 映射

同时还改了目录查找路径：

- `cryexts_find_entry()`

现在它会：

1. 读取目录索引块
2. 对待查名字做 hash
3. 算出 bucket
4. 取 bucket 对应的 `block_mask`
5. 只扫描 mask 命中的目录逻辑块

此外，目录修改路径在成功变更后也会重建索引块，保证索引和真实目录内容一致。

### 2.4 `inode.c`

文件：

- [inode.c](/D:/Carl/cryptext4/cryexts/inode.c:1)

这部分的核心调整是：

- 目录 inode 如果带 `CRYEXTS_INODE_FLAG_DIR_INDEX`
- 那么它的 `indirect_block` 不再当作普通单级 indirect table 来解释
- 而是当作 `dir index block`

配套改动包括：

- block count 计算要把目录索引块算进去
- inode 校验时要允许目录拥有这个索引块
- 释放目录块时要正确处理索引块

这一步非常关键，因为不改这里的话，内核和 `cryextsck` 都会把目录索引块误判成普通 indirect block。

### 2.5 `tools/cryextsck.c`

文件：

- [tools/cryextsck.c](/D:/Carl/cryptext4/cryexts/tools/cryextsck.c:1)

V5.3 对 `cryextsck` 的增强主要有两类。

第一类是“理解目录索引块”：

- 允许目录 inode 带 `DIR_INDEX` flag
- 允许目录 inode 的 `indirect_block` 作为索引块存在
- 不能再把它当普通 indirect table 逐项解读

第二类是“一致性校验”：

- 校验目录索引块的 `magic`
- 校验 `buckets`
- 校验 `dir_blocks`
- 校验每个 live dirent 所在目录逻辑块，必须被对应 bucket 的 mask 覆盖

如果不做这些修改，之前就会出现你已经见过的那类报错：

- `indirect entry points outside data area`
- `directory index block is referenced by multiple inodes`

本质原因就是 fsck 错把目录索引块当成单级 indirect 表来解析了。

### 2.6 `tools/cryexts_dir_index_inspect.c`

文件：

- [tools/cryexts_dir_index_inspect.c](/D:/Carl/cryptext4/cryexts/tools/cryexts_dir_index_inspect.c:1)

这是 V5.3 新增的观测工具，用来直接查看镜像里的目录索引块。

它会输出：

- `inode`
- `index_block`
- `magic`
- `buckets`
- `dir_blocks`
- `entries`
- 所有非零 bucket 的 `mask`

这个工具的价值在于：

- 你不需要进内核调试
- 直接在镜像层面就能看出索引块是否正确构建

### 2.7 `scripts/smoke_v5_3_dir_index.sh`

文件：

- [scripts/smoke_v5_3_dir_index.sh](/D:/Carl/cryptext4/cryexts/scripts/smoke_v5_3_dir_index.sh:1)

这份 smoke 脚本验证的是完整闭环：

1. 创建启用目录索引的 V5 文件系统镜像
2. 先用 `cryextsck` 验证空镜像干净
3. 挂载后创建大目录并写入大量文件
4. 卸载后用 inspect 工具查看索引块
5. 再用 `cryextsck` 做一致性校验

最终成功输出：

```text
v5.3 directory-index smoke test passed
```

### 2.8 `Makefile`

文件：

- [Makefile](/D:/Carl/cryptext4/cryexts/Makefile:1)

主要补充：

- 把 `cryexts_dir_index_inspect` 加入构建目标

这样执行 `make` 时，inspect 工具会一起编译出来。

## 3. 这一版的核心运行逻辑

V5.3 的目录索引机制可以概括成：

```text
目录变化
-> rebuild 整个 dir index block
-> 每个 bucket 记录候选目录逻辑块 mask

目录查找
-> hash(name)
-> bucket
-> 取 block mask
-> 只扫描候选目录逻辑块
-> 精确比较 dirent.name
```

关键点有三个：

- `mask` 存的是目录逻辑块号，不是物理块号
- 物理块号通过 `cryexts_inode_block_at()` 映射
- 当前维护策略是 `rebuild`，不是增量更新

## 4. 为什么这样设计

这版设计刻意保持保守，主要出于三个原因：

- 先做出稳定闭环，比一开始就做复杂多级 htree 更重要
- 目录项格式不需要大改，便于和现有实现兼容
- `cryextsck` 和内核都比较容易同步支持

因此 V5.3 的定位非常明确：

- 它是一个“目录 hash 索引 MVP”
- 不是最终形态的 directory BTree/HTree

## 5. 当前边界

V5.3 还没有实现：

- 多级目录索引节点
- bucket overflow chain
- leaf split
- 动态 rebalance
- 更细粒度的增量维护

所以它解决的是：

- 大目录 lookup 不再全盘线性扫描

而不是：

- 一次性做完完整的高阶目录树结构

## 6. 审代码时最值得重点看的点

如果你要审这一版，我建议重点看下面几个问题：

1. 目录 inode 的 `indirect_block` 语义切换是否一致
2. rebuild 时 `block_masks[bucket] |= (1U << i)` 的逻辑是否和“逻辑块号”完全一致
3. `cryexts_find_entry()` 是否真的只扫描候选逻辑块
4. `cryextsck` 是否完全避免把目录索引块误判成普通 indirect block
5. 目录修改后是否都能触发索引重建

这几个点基本就是 V5.3 的主干。

## 7. 一句话总结

V5.3 新增的不是一个完整目录树，而是一套“能落盘、能查询、能校验、能观察”的目录索引最小闭环。

它已经把目录查找从：

- 全目录扫描

推进成了：

- hash -> bucket -> candidate blocks -> precise dirent match
