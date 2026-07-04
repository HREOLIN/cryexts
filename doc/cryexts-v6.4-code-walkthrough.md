# CRYEXTS v6.4 代码处理说明

## 1. 这份文档看什么

这份文档解释 v6.4 allocator 代码如何工作：

```text
父目录 group
-> inode goal allocation
-> data block soft reservation
-> inspect 验证
```

## 2. 新 inode 创建流程

入口：[inode.c](/D:/Carl/cryptext4/cryexts/inode.c:2402)

函数：

```c
struct inode *cryexts_new_inode(struct inode *dir, umode_t mode,
                                u64 data_block);
```

v6.4 新增流程：

```text
goal_group = cryexts_inode_group_of(dir)
cryexts_alloc_inode_goal(sb, goal_group, &ino)
```

这意味着：

```text
新 inode 优先分配在父目录 inode 所在 group
```

## 3. `cryexts_inode_group_of()`

位置：[inode.c](/D:/Carl/cryptext4/cryexts/inode.c:84)

职责：

- 根据 inode number 计算 inode group

核心公式：

```text
group = (inode->i_ino - 1) / inodes_per_group
```

如果没有 block group，返回 `U64_MAX`。

## 4. `cryexts_alloc_inode_goal()`

位置：[balloc.c](/D:/Carl/cryptext4/cryexts/balloc.c:365)

职责：

- 带目标 group 分配 inode

流程：

1. 如果没有 block group，走 legacy inode allocator
2. 如果有 block group 且启用 prealloc feature，`start_group = goal_group`
3. 从 `start_group` 开始环形扫描 group
4. 找到有空闲 inode 的 group 后调用 `cryexts_alloc_inode_in_group()`
5. 更新 free inode counters
6. 标记 bitmap/super dirty

## 5. 新 inode 如何设置 data locality

新 inode 分配后会初始化：

```text
info->alloc_goal_group = goal_group
```

所以后续这个文件第一次写入数据时，数据块也倾向父目录 group。

这就是：

```text
inode locality
+ data locality
```

两个语义一起生效。

## 6. 数据块分配入口

入口：[inode.c](/D:/Carl/cryptext4/cryexts/inode.c:1266)

函数：

```c
int cryexts_resolve_block(struct inode *inode, u64 logical,
                          bool create, u64 *block);
```

当 `create=true` 且没有现有 mapping 时，v6.4 会调用：

```text
cryexts_alloc_data_block_for_inode()
```

## 7. `cryexts_alloc_data_block_for_inode()`

位置：[inode.c](/D:/Carl/cryptext4/cryexts/inode.c:184)

职责：

- 为 data block 选择更好的目标位置
- 维护 per-inode soft reservation window

流程：

1. 如果 `PREALLOC` feature 没开，回退旧逻辑
2. 如果有 active reservation window，使用 `reservation_next`
3. 否则使用 `alloc_hint_block`
4. 调用 `cryexts_alloc_block_goal()`
5. 分配成功后建立或推进 reservation window

## 8. reservation window 字段

字段位置：[cryexts.h](/D:/Carl/cryptext4/cryexts/cryexts.h:116)

### 8.1 `reservation_start`

当前 soft window 的起始 block。

### 8.2 `reservation_next`

下一次优先尝试分配的 block。

### 8.3 `reservation_end`

当前 soft window 的结束 block。

语义：

```text
[reservation_start, reservation_end)
```

## 9. window 建立例子

第一次写入拿到：

```text
physical=5000
```

那么 v6.4 设置：

```text
reservation_start=5000
reservation_next=5001
reservation_end=5000 + CRYEXTS_RESERVATION_WINDOW_BLOCKS
```

后续写入 logical block 1 时，会优先尝试：

```text
physical=5001
```

## 10. `cryexts_update_alloc_hint()`

位置：[inode.c](/D:/Carl/cryptext4/cryexts/inode.c:111)

职责：

- 更新 `alloc_hint_block`
- 更新 `alloc_goal_group`
- 推进或重置 reservation window

如果新分配的 block 跳出了当前 window，会清空旧 window。

## 11. `cryexts_alloc_inspect`

位置：[tools/cryexts_alloc_inspect.c](/D:/Carl/cryptext4/cryexts/tools/cryexts_alloc_inspect.c:1)

职责：

- 离线读取 image
- 根据 inode number 找到磁盘 inode
- 输出 inode group 和数据分配情况

关键输出：

- `inode[N].group`
- `inode[N].first_data_block`
- `inode[N].first_data_group`
- `inode[N].extent_segments`
- `inode[N].largest_extent_len`

## 12. smoke 测试路径

脚本：[smoke_v6_4_allocator.sh](/D:/Carl/cryptext4/cryexts/scripts/smoke_v6_4_allocator.sh:1)

测试做三件事：

1. 写入一个 96 block 顺序文件
2. 创建 12 个同目录 sibling 文件
3. 用 `cryexts_alloc_inspect` 检查 locality

核心断言：

```text
seq file data group == seq inode group
largest_extent_len >= 64
sibling inode groups 只有一个 group
```

## 13. 当前边界

v6.4 还不是完整 delayed allocation。

它没有：

- page-cache 延迟分配
- unwritten extent
- reservation owner 持久化
- buddy allocator

但它已经让 allocator 具备更真实的方向：

```text
inode 和 data block 都开始围绕 locality 做决策
```
