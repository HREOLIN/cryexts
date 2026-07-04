# CRYEXTS v6.4 Allocator 设计说明

## 1. v6.4 的定位

v6.4 的目标是：

```text
让分配策略更像真实文件系统
```

它不追求一次完成 ext4 级别的 buddy allocator，而是先补齐两个非常关键的行为：

- inode locality
- data block soft reservation

## 2. inode locality

### 2.1 为什么需要 inode locality

如果同一个目录下的文件 inode 分散在很多 group：

```text
/dir/a -> inode group 0
/dir/b -> inode group 5
/dir/c -> inode group 2
```

目录扫描、stat、元数据访问就会更分散。

更理想的情况是：

```text
/dir/a -> inode group 0
/dir/b -> inode group 0
/dir/c -> inode group 0
```

### 2.2 v6.4 如何做

创建新 inode 时：

```text
cryexts_new_inode(dir, mode, data_block)
-> goal_group = cryexts_inode_group_of(dir)
-> cryexts_alloc_inode_goal(sb, goal_group, &ino)
```

也就是说：

```text
子文件/子目录优先继承父目录 inode 所在 group
```

如果这个 group 没有空闲 inode，再回退扫描其它 group。

## 3. data block soft reservation

### 3.1 为什么不用真正 reservation

真正 reservation 的意思是：

```text
提前把一段 block 标记为属于某个 inode
```

这会立刻带来很多复杂问题：

- crash 后未使用 reservation 怎么回收
- 多 inode 竞争同一区间怎么处理
- journal 是否要记录 reservation owner
- truncate/unlink 是否要释放 reservation

v6.4 先做 soft reservation。

### 3.2 soft reservation 是什么

soft reservation 不提前占用 bitmap。
它只是记录：

```text
下一次这个 inode 优先从哪里找 block
```

所以它更像：

```text
per-inode allocation window hint
```

### 3.3 window 字段

运行时字段在 `struct cryexts_inode_info` 中：

```c
u64 reservation_start;
u64 reservation_next;
u64 reservation_end;
```

含义：

```text
[reservation_start, reservation_end) 是当前 inode 的 soft window
reservation_next 是下一次优先尝试的位置
```

## 4. data block 分配流程

入口：

```text
cryexts_resolve_block()
```

当 `create=true` 且 logical block 没有映射时：

```text
cryexts_alloc_data_block_for_inode()
```

流程：

1. 如果 feature 没开，走旧的 goal allocator
2. 如果当前 inode 有 active window，优先从 `reservation_next` 分配
3. 如果没有 window，用 `alloc_hint_block` 或 `alloc_goal_group` 分配
4. 分配成功后，建立或推进 window
5. 写入 extent/direct/indirect 映射

## 5. window 如何建立

假设分配到了 physical block `1000`。

v6.4 会建立：

```text
reservation_start = 1000
reservation_next  = 1001
reservation_end   = min(1000 + CRYEXTS_RESERVATION_WINDOW_BLOCKS, group_end)
```

这里 `64` 来自：

```text
CRYEXTS_RESERVATION_WINDOW_BLOCKS
```

它是 v6.4 MVP 的固定窗口大小。

## 6. window 如何失效

如果一次分配结果不在当前 window 内：

```text
physical < reservation_start
physical >= reservation_end
```

说明 allocator 已经跳出了原来的连续范围。

这时会调用：

```text
cryexts_reset_reservation()
```

然后根据新 physical block 建立新的 window。

## 7. 和 extent merge 的关系

extent merge 仍然存在。

顺序写时路径大致是：

```text
logical=N
-> 尝试和最后一条 extent 合并
-> 如果能拿到 physical+1，直接扩大 extent
-> 如果不能，再走 soft reservation / goal allocator
```

所以 v6.4 不是替代 extent merge，而是给 merge 失败后的分配策略提供更好的 locality。

## 8. 为什么 metadata block 不走 data reservation

v6.4 只让 data block 使用 soft reservation。

这些 metadata block 不走 data reservation：

- extent leaf block
- extent overflow block
- indirect metadata block

原因是：

```text
metadata block 插入数据窗口中，会打断 regular file 的连续数据布局
```

## 9. Inspect 工具

新增：

```text
cryexts_alloc_inspect
```

用法：

```bash
./cryexts_alloc_inspect image inode...
```

输出示例：

```text
blocks_per_group=4096
inodes_per_group=448
inode[3].group=0
inode[3].first_data_group=0
inode[3].extent_segments=1
inode[3].largest_extent_len=96
```

含义：

- inode 在 group 0
- 文件第一个 data block 也在 group 0
- 文件只有 1 个 extent segment
- 最大连续 extent 长度是 96 block

## 10. 一句话理解

```text
v6.4 不是把 block 先抢占下来，而是给每个 inode 一个“下一段尽量连续分配”的方向感。
```
