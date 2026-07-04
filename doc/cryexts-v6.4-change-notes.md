# CRYEXTS v6.4 变更说明

## 1. 这一版解决了什么问题

`v5.6` 已经有了最小 locality hint：

```text
alloc_hint_block
alloc_goal_group
```

它能让顺序写尽量从上一次分配位置继续找，也能让目录相关 block 倾向同一个 group。

`v6.4` 在这个基础上继续推进 allocator：

```text
inode allocator locality
+ per-inode soft reservation window
+ allocator inspect tool
```

一句话理解：

```text
v5.6 = block 分配带 hint
v6.4 = inode 和 data block 都更明确地围绕 locality 分配
```

## 2. 修改了哪些代码文件

### 2.1 [cryexts.h](/D:/Carl/cryptext4/cryexts/cryexts.h:1)

`struct cryexts_inode_info` 新增运行时字段：

- `reservation_start`
- `reservation_next`
- `reservation_end`

新增接口：

- `cryexts_prealloc_feature_enabled()`
- `cryexts_alloc_inode_goal()`

### 2.2 [balloc.c](/D:/Carl/cryptext4/cryexts/balloc.c:1)

新增：

- `cryexts_alloc_inode_goal()`

变化点：

- inode allocator 现在可以接收 `goal_group`
- 如果启用 `PREALLOC` feature，会优先从 `goal_group` 分配 inode
- 如果目标 group 没有空闲 inode，再环形扫描其它 group
- 原来的 `cryexts_alloc_inode()` 保留，变成 `goal_group = U64_MAX` 的包装

### 2.3 [inode.c](/D:/Carl/cryptext4/cryexts/inode.c:1)

新增 helper：

- `cryexts_inode_group_of()`
- `cryexts_reset_reservation()`
- `cryexts_alloc_data_block_for_inode()`

变化点：

- 新 inode 创建时，优先从父目录 inode 所在 group 分配
- 新 inode 的 data allocation goal 也优先继承父目录 inode group
- regular file 分配数据块时，会维护一个 soft reservation window
- reservation window 不写盘，只是内存中的 allocator hint

### 2.4 [tools/cryexts_alloc_inspect.c](/D:/Carl/cryptext4/cryexts/tools/cryexts_alloc_inspect.c:1)

新增 allocator inspect 工具，用于观察：

- inode 所在 group
- inode size / blocks
- 首个 data block
- 首个 data block 所在 group
- extent segment 数量
- 最大 extent 长度

### 2.5 [scripts/smoke_v6_4_allocator.sh](/D:/Carl/cryptext4/cryexts/scripts/smoke_v6_4_allocator.sh:1)

新增 smoke test，验证：

- 顺序写文件的 data group 和 inode group 一致
- 顺序写能获得较长连续 extent
- 同目录小文件 inode 聚在同一 group
- `cryextsck` 最终 clean

## 3. 新增字段说明

### 3.1 `reservation_start`

位置：[cryexts.h](/D:/Carl/cryptext4/cryexts/cryexts.h:116)

含义：

```text
当前 inode soft reservation window 的起始 physical block
```

如果为 0，表示当前 inode 没有 active reservation window。

### 3.2 `reservation_next`

含义：

```text
下一次数据块分配优先尝试的 physical block
```

顺序写时，这个值通常会不断向后推进。

### 3.3 `reservation_end`

含义：

```text
当前 soft reservation window 的结束位置
```

范围语义：

```text
[reservation_start, reservation_end)
```

注意它是 soft window：

- 不提前占用 bitmap
- 不写入磁盘
- 不需要 crash recovery
- 失败时可以回退到普通 allocator 扫描

## 4. 新增函数说明

### 4.1 `cryexts_prealloc_feature_enabled()`

位置：[balloc.c](/D:/Carl/cryptext4/cryexts/balloc.c:359)

职责：

- 检查 superblock `features_compat`
- 判断 `CRYEXTS_FEATURE_COMPAT_PREALLOC` 是否启用

这个函数从 `static` 改为全局可用，因为 inode 层也需要知道 allocator locality 是否开启。

### 4.2 `cryexts_alloc_inode_goal()`

位置：[balloc.c](/D:/Carl/cryptext4/cryexts/balloc.c:365)

职责：

- 带目标 group 分配 inode
- 优先扫描 `goal_group`
- 失败后继续扫描其它 group

参数：

- `sb`：文件系统 superblock
- `goal_group`：期望分配 inode 的 group；`U64_MAX` 表示无目标
- `ino`：输出 inode number

### 4.3 `cryexts_inode_group_of()`

位置：[inode.c](/D:/Carl/cryptext4/cryexts/inode.c:84)

职责：

- 根据 inode number 计算当前 inode 所在 group

公式：

```text
inode_group = (inode->i_ino - 1) / inodes_per_group
```

### 4.4 `cryexts_reset_reservation()`

位置：[inode.c](/D:/Carl/cryptext4/cryexts/inode.c:101)

职责：

- 清空 inode 的 soft reservation window

会把下面字段都置为 0：

- `reservation_start`
- `reservation_next`
- `reservation_end`

### 4.5 `cryexts_alloc_data_block_for_inode()`

位置：[inode.c](/D:/Carl/cryptext4/cryexts/inode.c:184)

职责：

- 为 regular file / data path 分配数据块
- 优先使用 inode 的 reservation window
- 若 window 不存在或用完，则根据 alloc hint 建立新的 window

这个函数只用于 data block。
extent leaf、overflow block、indirect metadata block 仍然走普通 goal allocator。

## 5. 典型案例

### 5.1 同目录 inode locality

假设父目录 `/alloc` 的 inode 在 group 0。

v6.4 创建同目录文件：

```text
/alloc/sibling_1.txt
/alloc/sibling_2.txt
/alloc/sibling_3.txt
```

会优先调用：

```text
cryexts_alloc_inode_goal(sb, goal_group=0, &ino)
```

所以这些 inode 更容易落在 group 0。

### 5.2 顺序写 soft reservation

写入 96 个 block：

```text
seq-reserved.bin logical block 0..95
```

第一个数据 block 分配成功后，inode 会形成一个窗口：

```text
reservation_start = physical
reservation_next  = physical + 1
reservation_end   = physical + CRYEXTS_RESERVATION_WINDOW_BLOCKS
```

后续 block 优先从 `reservation_next` 继续尝试。
如果 bitmap 中这些位置连续空闲，就会形成一个较大的 extent。

## 6. 当前边界

v6.4 仍然不是完整生产级 allocator。

已实现：

- inode goal-group allocation
- per-inode soft reservation window
- data block locality 更稳定
- allocator inspect 工具

未实现：

- 真正提前占用 bitmap 的 reservation
- reservation owner 冲突处理
- delayed allocation 的 page-cache 集成
- buddy allocator
- 后台 defrag

## 7. 验收方式

运行：

```bash
chmod +x scripts/smoke_v6_4_allocator.sh
./scripts/smoke_v6_4_allocator.sh
```

期望输出：

```text
v6.4 allocator locality smoke test passed
```

## 8. 一句话总结

```text
v6.4 把 allocator 从“带 hint 的 block 分配”推进到“inode/data 都围绕 locality 的软预留分配”。
```
