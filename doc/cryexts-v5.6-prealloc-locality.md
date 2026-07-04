# CRYEXTS V5.6 预分配与局部性设计说明

## 1. 这一版要解决什么问题

`v5.5` 之前，`cryexts` 虽然已经有 block group allocator，也有 `next_data_block` 这样的顺序 hint，但它更像：

- “全局下一块从哪里开始找”

而不是：

- “这个文件下一块最好接在它自己上一个块后面”
- “这个目录下新文件最好跟父目录落在同一片区域”

所以 `v5.6` 的目标不是做完整 ext4 级别的 reservation window，也不是做 buddy allocator，而是先把最小可工作的 locality 机制做起来。

一句话概括：

```text
v5.6 = 在现有 bitmap/group allocator 之上，补一层 per-file / per-directory 的分配 hint
```

## 2. 这一版实际实现了什么

这一版实现了三件核心事情：

1. regular file 顺序写时，优先尝试从“上一个已分配数据块的后一个 block”开始拿块
2. 新建 inode 时，继承父目录的 locality goal，让同目录新文件更容易落在相邻 group
3. 目录自身增长时，新目录块和 dir index block 也尽量沿着当前目录的 hint 分配

也就是说，这一版的 `prealloc` 更准确地说是：

```text
contiguous run preference + locality hint
```

还不是：

```text
真正预留一整段未来块、带回收和竞争处理的 reservation window
```

## 3. 运行时数据结构怎么变了

在 [cryexts.h](/D:/Carl/cryptext4/cryexts/cryexts.h:60) 的 `struct cryexts_inode_info` 里，新增了两个运行时字段：

- `alloc_hint_block`
- `alloc_goal_group`

它们都只是内存态 hint，不写回磁盘。

可以把它理解成：

- `alloc_hint_block`
  表示“下次如果还要给这个 inode 分配块，优先从哪个物理 block 附近开始找”
- `alloc_goal_group`
  表示“这个 inode 更偏向哪个 block group”

## 4. 分配器怎么改的

核心变化在 [balloc.c](/D:/Carl/cryptext4/cryexts/balloc.c:332)。

这一版新增了：

- `cryexts_alloc_block_goal()`

它相比老的 `cryexts_alloc_block()`，多了两个输入：

- `goal_block`
- `goal_group`

分配顺序大致变成：

```text
如果启用了 PREALLOC feature：
    优先从 goal_group 开始扫描
    如果 goal_block 正好落在这个 group
        就从 goal_block 开始找空闲块
    否则从该 group 内的默认 hint 开始找

如果上面找不到：
    再回退到普通 group 扫描
```

所以这版不是“绝对保证分配到连续块”，而是：

- 有空闲连续块时，优先拿到
- 没有时，自动回退，不影响正确性

## 5. 文件写路径怎么利用这个 hint

核心逻辑在 [inode.c](/D:/Carl/cryptext4/cryexts/inode.c:651) 的 `cryexts_resolve_block()`。

### 5.1 extent 路径

对于 extent inode：

1. 先看逻辑块是否已经被某个 extent 覆盖
2. 如果没有，并且是顺序追加
3. `cryexts_try_merge_last_extent()` 会优先尝试从
   `last_physical + last_len`
   这个位置拿新块
4. 如果真的拿到了紧邻块，就把最后一个 extent 直接 `length + 1`

也就是说：

```text
logical 连续 + physical 连续
-> 不新增 extent entry
-> 直接扩展最后一个 extent
```

如果拿不到紧邻块，再退回新建 extent entry。

### 5.2 direct / indirect 路径

对于非 extent 路径：

- direct block 分配也会带上 `alloc_hint_block + alloc_goal_group`
- indirect data block 分配也一样

所以这版 locality 不是只服务于 extent，老路径也受益。

## 6. 新 inode 为什么会继承父目录 locality

逻辑在 [inode.c](/D:/Carl/cryptext4/cryexts/inode.c:1470) 的 `cryexts_new_inode()`。

这里做的是：

- 如果父目录已经有 `alloc_goal_group`
  子 inode 继承它
- 如果父目录没有明确 goal，但能拿到父目录首块
  就按父目录首块所在 group 作为子 inode 的 goal
- 同时把父目录当前的 `alloc_hint_block` 也传给子 inode

这意味着：

```text
/data/project/
    file_a
    file_b
    file_c
```

这几个文件更容易分配到同一个 group，或者相邻 group。

## 7. 目录为什么也要更新 hint

这版还补了目录自己的 hint 推进。

在 [dir.c](/D:/Carl/cryptext4/cryexts/dir.c:87) 和 [dir.c](/D:/Carl/cryptext4/cryexts/dir.c:494)：

- 分配 `dir index block` 后，会更新目录 inode 的 alloc hint
- 目录扩容拿到新的目录数据块后，也会更新目录 inode 的 alloc hint

这样后续：

- 同目录下继续 `create`
- 目录继续增长

都更容易沿着同一片区域分配。

## 8. PREALLOC feature 在这一版如何理解

`PREALLOC` 这个 compat feature 在 `v5.0` 就已经预留了。

到了 `v5.6`，它才第一次真正和运行时行为绑定起来：

- `mkfs.cryexts` 现在会正式写入 `CRYEXTS_FEATURE_COMPAT_PREALLOC`
- allocator 会检查这个 flag
- 只有启用这个 flag 时，才优先按 `goal_block / goal_group` 走 locality 路径

所以你可以理解为：

```text
v5.0 的 PREALLOC = 只是格式声明
v5.6 的 PREALLOC = 开始有真实运行时语义
```

## 9. 这一版没有实现什么

这点很重要，避免审核时误判范围。

`v5.6` 还没有实现：

- 真正多块 reservation window
- delayed allocation
- fallocate
- 在线碎片整理
- 多 inode 之间的全局预留协调
- “先保留一大段块，以后慢慢消费”的完整生命周期管理

所以这版是：

```text
locality/prealloc MVP
```

不是完整生产级预分配器。

## 10. smoke 测了什么

对应脚本是 [scripts/smoke_v5_6_prealloc_locality.sh](/D:/Carl/cryptext4/cryexts/scripts/smoke_v5_6_prealloc_locality.sh:1)

它主要测三件事：

1. 先人为制造一点碎片背景
2. 再对一个大文件做分块顺序写入
3. 用 `cryexts_extent_inspect` 检查 extent 结果，确认至少出现“较大的连续 extent”

脚本里不会要求“必须只有 1 个 extent”，因为真实 allocator 受：

- 目录元数据占位
- 之前制造的碎片
- group 内已有分配

这些因素影响。

所以它检查的是：

- 文件可读写一致
- `cryextsck` clean
- extent 连续段足够大，说明 locality 确实起作用

## 11. 一句话结论

`v5.6` 把 `Version 5` 最后一块 MVP 拼图补上了：

```text
不是把 allocator 重写成复杂系统，
而是在现有结构上加上“按文件、按目录、更偏向连续和邻近区域”的分配策略。
```
