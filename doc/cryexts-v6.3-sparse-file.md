# CRYEXTS v6.3 Sparse File / Hole 设计说明

## 1. sparse file 是什么

sparse file 的核心是：

```text
文件的逻辑大小可以很大
但不是每一个逻辑 block 都必须有真实物理 block
```

没有真实物理 block 的逻辑区间叫做：

```text
hole
```

读取 hole 时，文件系统应该返回 0。

## 2. CRYEXTS 如何表达 hole

CRYEXTS 不新增专门的 hole 结构。
hole 由 extent 之间的 logical gap 自然表达。

例如：

```text
file size = 16KiB

extent[0]: logical=0 physical=100 len=1
extent[1]: logical=3 physical=200 len=1
```

含义：

```text
logical block 0 -> physical block 100
logical block 1 -> hole
logical block 2 -> hole
logical block 3 -> physical block 200
```

所以 v6.3 的重点不是增加新 on-disk struct，
而是改变 extent 校验和维护规则。

## 3. v6.3 的 extent invariant

### 3.1 必须保持的规则

extent 必须满足：

```text
logical_start 单调递增
不同 extent 不能重叠
length 不能为 0
length 不能超过 CRYEXTS_MAX_EXTENT_BLOCKS
physical range 必须都在 data area 内
physical range 必须在 bitmap 中标记为 used
extent 覆盖范围不能超过 inode size
```

### 3.2 不再要求的规则

v6.3 不再要求：

```text
next_extent.logical_start == previous.logical_start + previous.length
```

只要求：

```text
next_extent.logical_start >= previous.logical_start + previous.length
```

中间大于的部分就是 hole。

## 4. 读取流程

读取路径仍然从 `cryexts_read_iter()` 开始。

流程：

```text
cryexts_read_iter()
-> logical = pos / block_size
-> cryexts_resolve_block(create=false)
-> 如果找到 extent，返回 physical block
-> 如果找不到 extent，返回 physical=0
-> physical=0 时填充 0 到用户 buffer
```

所以 sparse read 不需要单独存储 hole。
只要查不到映射，就代表这是 hole。

## 5. 写入流程

写入路径仍然从 `cryexts_write_iter()` 开始。

流程：

```text
cryexts_write_iter()
-> logical = pos / block_size
-> cryexts_resolve_block(create=true)
-> 如果已有映射，覆盖写入
-> 如果没有映射，分配新 physical block
-> 插入 extent tree
```

v6.3 的关键变化是：

```text
新 extent 插入时按 logical 排序
```

这让下面的写入顺序也安全：

```text
先写 logical=256
再写 logical=0
```

最终 tree 仍然是：

```text
logical=0
logical=256
```

## 6. punch-hole 流程

用户态命令：

```bash
fallocate -p -o 4096 -l 4096 file.bin
```

对应 VFS 调用：

```text
file_operations.fallocate
```

CRYEXTS 处理入口：

```text
cryexts_fallocate()
```

只支持：

```text
FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE
```

也就是挖洞但不改变文件大小。

## 7. punch-hole 的四种 extent 变化

假设 hole 范围是：

```text
[punch_start, punch_end)
```

### 7.1 完整删除 extent

原来：

```text
extent logical=10 len=2
```

hole：

```text
[10, 12)
```

结果：

```text
删除整条 extent
释放对应 physical blocks
```

### 7.2 删除 extent 左侧

原来：

```text
extent logical=10 physical=100 len=4
```

hole：

```text
[10, 12)
```

结果：

```text
extent logical=12 physical=102 len=2
```

### 7.3 删除 extent 右侧

原来：

```text
extent logical=10 physical=100 len=4
```

hole：

```text
[12, 14)
```

结果：

```text
extent logical=10 physical=100 len=2
```

### 7.4 从 extent 中间挖洞

原来：

```text
extent logical=10 physical=100 len=4
```

hole：

```text
[11, 12)
```

结果：

```text
extent logical=10 physical=100 len=1
extent logical=12 physical=102 len=2
```

这就是 `cryexts_insert_extent_after_v2()` 的作用。

## 8. 结构体字段说明

v6.3 没有新增 on-disk 结构体。
它复用 v6.2 的 extent tree v2 结构：

### 8.1 `struct cryexts_extent`

定义位置：[cryexts_fs.h](/D:/Carl/cryptext4/cryexts/cryexts_fs.h:139)

字段：

- `logical_start`：这个 extent 覆盖的第一个逻辑 block
- `physical_start`：这个 extent 对应的第一个物理 block
- `length`：连续映射长度，单位是 block
- `flags`：预留标志位，当前 v6.3 仍为 0

### 8.2 `struct cryexts_extent_root_ref`

定义位置：[cryexts_fs.h](/D:/Carl/cryptext4/cryexts/cryexts_fs.h:146)

字段：

- `logical_start`：leaf 中第一条 extent 的 logical 起点
- `leaf_block`：leaf metadata block 的物理块号
- `entries`：leaf 中 extent 数量
- `checksum`：leaf block checksum，metadata checksum 开启时有效

## 9. 和 v6.2 的关系

`v6.2` 解决的是：

```text
extent entry 不够放
```

`v6.3` 解决的是：

```text
logical block 不一定都要映射 physical block
```

两者合起来，regular file mapping 才开始接近真实文件系统语义。

## 10. 当前边界

v6.3 是 MVP，不是完整 ext4 fallocate。

当前支持：

- sparse write
- sparse read zero-fill
- truncate grow 形成 hole
- punch-hole + keep-size

当前不支持：

- `FALLOC_FL_KEEP_SIZE` 预分配 unwritten extent
- delayed allocation
- 跨 leaf split 的复杂 rebalance
- 任意高度 extent B-tree

这些可以放到后续 v6.x 继续推进。
