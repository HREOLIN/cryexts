# CRYEXTS V5.3 Hash Directory 说明

## 1. 先说结论

V5.3 实现的不是完整的多级 htree，而是一个单层的 `hash directory MVP`。

它的目标不是一步到位做成：

- 根节点
- 多级索引节点
- 叶子节点分裂
- 动态 rebalance

而是先完成最重要的一步：

- 查找时不再全目录线性扫描
- 先 hash
- 再只扫少数几个候选目录块

## 2. 它和传统线性目录查找的区别

原来的查找方式可以抽象成：

```text
lookup(name)
-> 扫目录逻辑块 0
-> 扫目录逻辑块 1
-> 扫目录逻辑块 2
-> ...
-> 每个块里逐个比较 dirent.name
```

这叫全目录线性扫描。

V5.3 改成：

```text
lookup(name)
-> hash(name)
-> bucket = hash % 64
-> 读 bucket 对应的 block mask
-> 只扫描 mask 里标记过的目录逻辑块
-> 在候选块里做精确字符串比较
```

所以它不是“直接通过 hash 找到 inode”，而是“先通过 hash 缩小搜索范围”。

## 3. 什么是 bucket

当前固定有：

- `64` 个 bucket

每个文件名会先做一次 hash，然后：

- `bucket = hash % 64`

这意味着不同名字可能会落入同一个 bucket，这就是第一层冲突。

例如：

- `file_1` 和 `dog_9`
- 虽然名字完全不同
- 但它们可能都会落到 `bucket 17`

这很正常。

## 4. 什么是第二层冲突

即使同一个 bucket，目录项也不一定都在同一个目录数据块里。

例如一个大目录用了 10 个目录逻辑块：

```text
block0
block1
block2
...
block9
```

现在假设：

- 有些落在 `bucket 17` 的名字在 `block0`
- 有些在 `block3`
- 有些在 `block7`
- 有些在 `block9`

那么 `bucket 17` 对应的 mask 就可能是：

```text
0x0289
```

二进制近似理解为：

```text
bit0 = 1
bit3 = 1
bit7 = 1
bit9 = 1
```

它表示的不是“具体哪个文件在这里”，而是：

- `bucket 17` 的候选目录逻辑块集合是 `{0, 3, 7, 9}`

这就是你之前问到的“第二层冲突”的本质：

- 同一 bucket 里的名字，仍可能散落在多个目录逻辑块里

## 5. block mask 到底怎么理解

`block_masks[bucket]` 是一个 `u16` 位图。

每一位对应一个目录逻辑块号：

- bit0 -> 目录逻辑块 0
- bit1 -> 目录逻辑块 1
- bit2 -> 目录逻辑块 2
- ...

例如：

```text
0x0005 = 0000 0000 0000 0101
```

表示：

- bit0 = 1 -> 这个 bucket 在逻辑块 0 里有名字
- bit2 = 1 -> 这个 bucket 在逻辑块 2 里也有名字
- 其他位 = 0 -> 其他目录逻辑块里没有

所以：

- mask 记录的是“这个 bucket 可能在哪些逻辑块里”
- 不是记录“这个 bucket 对应哪个物理块号”
- 也不是记录“文件名偏移”

## 6. 逻辑块号和物理块号怎么对应

这里必须区分清楚：

- mask 里的 bit position 是目录逻辑块号
- 真正磁盘上的块号是物理块号

比如：

- `bit3 = 1`

表示：

- 要去扫描目录的逻辑块 3

但逻辑块 3 并不等于磁盘物理块 3。

真正读盘前，代码会调用：

- `cryexts_inode_block_at(dir, 3)`

把“目录逻辑块 3”映射成“这个目录 inode 第 3 个数据块实际落在哪个物理块号上”。

所以查找过程其实是：

```text
bucket -> mask -> 逻辑块号 -> cryexts_inode_block_at() -> 物理块号
```

## 7. `cryexts_inode_block_at()` 在这里的作用

这个函数的作用可以简单理解成：

- 给我一个 inode
- 再给我一个文件或目录的逻辑块号
- 我返回它实际对应的磁盘物理块号

对目录来说，当前 V5.3 目录数据仍然放在 inode 的 direct block 里，所以它会根据：

- `inode->block[0]`
- `inode->block[1]`
- `inode->block[2]`
- ...

把逻辑目录块映射成物理块号。

因此，mask 只负责“过滤候选逻辑块”，真正读哪块盘还是靠 inode 的 block mapping。

## 8. block mask 是怎么维护出来的

当前维护方式不是增量修补，而是整目录重建。

目录变化后，`cryexts_dir_index_rebuild()` 会：

1. 扫描目录逻辑块 0
2. 扫描目录逻辑块 1
3. 扫描目录逻辑块 2
4. ...
5. 对每个 live dirent 做 hash
6. 算出 bucket
7. 把当前逻辑块号的 bit 置 1

关键语句是：

```c
index.block_masks[bucket] |= cpu_to_le16(1U << i);
```

其中：

- `bucket` 是名字 hash 后落入的桶号
- `i` 是当前正在扫描的目录逻辑块号

举例：

如果当前正在扫描 `logical block 2`，里面发现一个名字属于 `bucket 17`，那就执行：

```text
bucket[17] 的 bit2 = 1
```

如果后来在 `logical block 5` 里又发现另一个名字也属于 `bucket 17`，那就再把：

```text
bucket[17] 的 bit5 = 1
```

这样最终 `bucket[17]` 就知道自己需要覆盖哪些目录逻辑块。

## 9. 一个 1 万目录项的例子

假设一个大目录里有 `10000` 个目录项，已经撑到了 `10` 个目录逻辑块。

那么目录本体还是：

```text
dir logical block 0
dir logical block 1
...
dir logical block 9
```

而目录索引块只是多了一张“候选块分布表”：

```text
bucket[0]  -> mask
bucket[1]  -> mask
...
bucket[63] -> mask
```

比如：

```text
bucket[17] = bit0 | bit3 | bit7 | bit9
bucket[22] = bit1 | bit2
bucket[40] = bit4 | bit5 | bit8
```

如果要查 `lookup("file_123")`，并且它 hash 后落到 `bucket 17`，那么流程就变成：

```text
lookup("file_123")
-> bucket 17
-> 读 mask
-> 得到候选逻辑块 {0, 3, 7, 9}
-> 分别映射到物理块号
-> 只扫描这 4 个目录块
```

相比直接扫描 10 个目录块，范围已经缩小了。

## 10. 为什么现在所有 bucket 可能都显示 `0x0001`

你之前的测试输出里，所有 bucket 都是：

```text
bucket[x]=0x0001
```

这不代表出错，反而说明：

- 当前目录索引已经建立
- 但这个目录实际只用了 `1` 个目录逻辑块

所以无论哪个 bucket 被命中，它的候选块集合都只能是：

```text
{ logical block 0 }
```

于是每个 bucket 的 mask 都是：

```text
0x0001
```

等目录再大一些，跨多个逻辑块之后，你就会看到：

- `0x0003`
- `0x0005`
- `0x0019`

这类多 bit 的 mask。

## 11. 当前实现为什么叫 hash directory，而不是完整 BTree

因为它现在只有一层索引：

- `bucket -> candidate block mask`

还没有：

- 多级节点
- 精确叶子指针
- 节点分裂
- 动态平衡

所以更准确的说法是：

- 这是一个单层 hash directory
- 也是未来更完整 directory htree 的第一步

## 12. 一句话总结

V5.3 的目录 hash 机制，本质上就是：

- 先按名字 hash 到 bucket
- 再用 bucket 对应的 block mask 找到少量候选目录逻辑块
- 最后通过 `cryexts_inode_block_at()` 映射物理块并做精确 dirent 名字匹配

所以它解决的是“减少扫描范围”，不是“完全消灭冲突”。
