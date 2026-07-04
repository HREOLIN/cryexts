# CRYEXTS V5.3 目录索引设计与实现

## 1. 这一版解决什么问题

在 V5.2 之前，目录查找基本还是线性扫描：

- 逐个目录数据块扫描
- 逐个 `dirent.name` 做字符串比较

这意味着目录一旦变大，下面这些路径都会越来越慢：

- `lookup`
- `create`
- `unlink`
- `rename`

V5.3 的目标不是一次性做出完整的多级 htree，而是先做一个可工作的最小版本：

- 引入 `hash-based directory index`
- 给目录单独分配一个 `dir index block`
- 用它记录“某个 hash bucket 可能落在哪些目录逻辑块里”
- 让 `cryextsck` 能识别并校验这个结构

一句话理解：

不是直接存“文件名 -> inode”，而是先存：

`hash(name) -> 候选目录逻辑块集合`

然后查找时只扫描候选块，而不是全目录扫一遍。

## 2. on-disk 结构

### 2.1 新增 feature 和 inode flag

V5.3 依赖两个标记：

- `CRYEXTS_FEATURE_INCOMPAT_DIR_INDEX`
- `CRYEXTS_INODE_FLAG_DIR_INDEX`

含义分别是：

- 超级块的 `DIR_INDEX` feature 表示这个磁盘格式支持目录索引
- inode 的 `DIR_INDEX` flag 表示这个目录当前已经有索引块

### 2.2 目录索引块格式

定义位于 [cryexts_fs.h](/D:/Carl/cryptext4/cryexts/cryexts_fs.h:105)：

```c
struct cryexts_dir_index_block {
    __le32 magic;
    __le16 buckets;
    __le16 dir_blocks;
    __le32 entries;
    __u8 reserved[16];
    __le16 block_masks[CRYEXTS_DIR_INDEX_BUCKETS];
}
```

关键字段含义：

- `magic`
  固定为 `CRYEXTS_DIR_INDEX_MAGIC`，用于识别这是目录索引块
- `buckets`
  当前固定为 `64`
- `dir_blocks`
  这个目录当前用了多少个目录逻辑块
- `entries`
  当前目录里 live dirent 的总数
- `block_masks[bucket]`
  每个 bucket 对应一个 `u16` 的候选块位图

### 2.3 block mask 到底表示什么

这里最关键的一点是：

`block_masks[bucket]` 记录的是目录的“逻辑块号”，不是磁盘物理块号。

例如：

```text
0x0001 = 0000 0000 0000 0001
```

表示：

- bit0 = 1
- 说明这个 bucket 的名字可能出现在目录逻辑块 `0`

再例如：

```text
0x000d = 0000 0000 0000 1101
```

表示：

- 这个 bucket 的名字可能出现在目录逻辑块 `0`
- 也可能在逻辑块 `2`
- 也可能在逻辑块 `3`

真正要访问磁盘物理块时，再通过：

- `cryexts_inode_block_at(dir, logical_block)`

把逻辑块号映射成物理块号。

## 3. 索引块存在哪里

这一版为了尽量少改现有布局，没有额外再发明一套目录索引树节点结构，而是复用了目录 inode 里的：

- `indirect_block`

但这里要注意语义变化：

- 对普通 regular file 来说，`indirect_block` 还是单级间接块
- 对带 `CRYEXTS_INODE_FLAG_DIR_INDEX` 的目录来说，`indirect_block` 表示目录索引块

也就是说，V5.3 里目录索引块暂时复用了原来 indirect 指针的位置。

## 4. 运行时维护逻辑

### 4.1 先采用 rebuild 策略

V5.3 没有做复杂的增量维护，而是采用最稳妥的 `rebuild` 策略：

- 目录发生变化后
- 重新扫描整个目录
- 重新构造一份完整的 `dir index block`

触发时机包括：

- `create`
- `mkdir`
- `unlink`
- 目录相关的变更路径

核心逻辑在 [dir.c](/D:/Carl/cryptext4/cryexts/dir.c:82) 的 `cryexts_dir_index_rebuild()`。

### 4.2 rebuild 时怎么设置 mask

rebuild 时会：

1. 遍历目录每个逻辑目录块 `i`
2. 解析块内每个 live dirent
3. 计算 `hash(name) % 64`
4. 得到对应 bucket
5. 把当前逻辑块号 `i` 写进 bucket 的 bitmask

关键代码就是：

```c
index.block_masks[bucket] |= cpu_to_le16(1U << i);
```

它的含义非常直接：

- 当前这个名字属于 bucket `bucket`
- 它出现在目录逻辑块 `i`
- 那就把 `block_masks[bucket]` 的第 `i` 位设置为 `1`

## 5. 查询路径怎么走

V5.3 之后，目录查找仍然从 `cryexts_find_entry()` 进入，只是中间多了一层 hash 索引过滤。

查询流程可以理解成：

```text
lookup("file_123")
-> cryexts_find_entry()
-> 读取 dir index block
-> hash("file_123")
-> bucket = hash % 64
-> 取 block_masks[bucket]
-> 得到候选目录逻辑块号集合
-> 对每个候选逻辑块调用 cryexts_inode_block_at()
-> 转成物理块号并读取目录数据块
-> 在候选块里逐个比较 dirent.name
-> 找到后返回 inode
```

注意这里仍然可能有 hash 冲突，所以流程不是：

- `hash -> 直接定位唯一目录项`

而是：

- `hash -> 缩小扫描范围 -> 在候选块里精确比对名字`

## 6. 一个具体例子

假设某个目录现在用了 4 个目录逻辑块：

```text
logical block 0: ., .., aa, ab, ac
logical block 1: dog, duck, door
logical block 2: file_100, file_101, file_102
logical block 3: zoo, zip, zero
```

假设索引块里有：

```text
bucket[17] = 0x0004
bucket[30] = 0x0005
```

含义是：

- `bucket[17] = 0x0004`
  只可能出现在逻辑块 2
- `bucket[30] = 0x0005`
  可能出现在逻辑块 0 和逻辑块 2

如果现在查：

```text
lookup("file_101")
```

并且：

```text
hash("file_101") % 64 = 17
```

那么查询就只需要扫描逻辑块 2，不需要扫描整个目录。

## 7. `cryextsck` 现在会检查什么

V5.3 之后，`cryextsck` 已经理解目录索引块的存在。

它会额外检查：

- 目录 inode 带 `DIR_INDEX` flag 时，`indirect_block` 必须存在
- 这个块必须是合法的数据块
- 索引块的 `magic / buckets / dir_blocks` 必须正确
- 每个 live dirent 的 hash bucket 所对应的 mask，必须覆盖它所在的目录逻辑块

如果目录项真实存在，但索引块没有把它所在块标进 mask，就会报：

- `directory index misses a live dirent block`

这保证了：

- 目录数据块和目录索引块之间是一致的

## 8. inspect 工具和 smoke 测试

### 8.1 inspect 工具

V5.3 新增了：

- [tools/cryexts_dir_index_inspect.c](/D:/Carl/cryptext4/cryexts/tools/cryexts_dir_index_inspect.c:1)

它会打印：

- 目录 inode 号
- `index_block`
- `magic`
- `buckets`
- `dir_blocks`
- `entries`
- 所有非零 bucket 的 mask

### 8.2 smoke 脚本

V5.3 smoke 位于：

- [scripts/smoke_v5_3_dir_index.sh](/D:/Carl/cryptext4/cryexts/scripts/smoke_v5_3_dir_index.sh:1)

它的步骤是：

1. 创建启用 `DIR_INDEX` 的 V5 镜像
2. 先跑一次 `cryextsck`
3. 挂载镜像
4. 创建一个大目录并塞入很多文件
5. 卸载镜像
6. 用 `cryexts_dir_index_inspect` 查看目录索引块
7. 再跑一次 `cryextsck`

最终期望输出：

```text
v5.3 directory-index smoke test passed
```

## 9. 当前实现边界

这一版仍然是 MVP，不是完整 ext4 htree。

当前还没有做：

- 多级 index node
- bucket overflow chain
- leaf split / rebalance
- 更细粒度的增量更新

所以 V5.3 的准确定位应该是：

- 单层 hash 目录索引
- 单个 index block
- 按 bucket 维护候选逻辑块集合

它已经能显著减少大目录查找的扫描范围，但还不是完整的多级目录 BTree/HTree。

## 10. 一句话总结

V5.3 做成的本质是：

- 用 hash 把目录查找先缩小到少数几个候选目录逻辑块
- 再在这些块里做精确的 dirent 名字匹配

也因此，这一版是“可运行、可校验、可观测”的目录索引 MVP。
