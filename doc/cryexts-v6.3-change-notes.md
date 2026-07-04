# CRYEXTS v6.3 变更说明

## 1. 这一版解决了什么问题

`v6.2` 已经把 regular file 的映射推进到 multi-leaf extent tree：

```text
inode root
-> extent leaf block
-> extent[]
```

但是 `v6.2` 仍然保留了一个旧假设：

```text
extent 的 logical range 必须从 0 开始连续排列
```

这会导致 sparse file 不成立。
例如只在 1MiB 位置写入 4KiB 数据时，合理的映射应该是：

```text
logical block 0..255  没有 extent，读出来是 0
logical block 256     有真实物理块
```

`v6.3` 的目标就是把这个语义补齐：

```text
logical size 和 physical allocation 解耦
hole 不占物理块
读取 hole 返回 0
punch-hole 可以释放中间的已映射 block
```

## 2. 核心语义变化

### 2.1 旧规则

旧规则要求：

```text
extent[0].logical_start == 0
extent[n].logical_start == previous.logical_start + previous.length
```

也就是说，extent 之间不能有 gap。

### 2.2 新规则

`v6.3` 改成：

```text
extent[n].logical_start >= previous.logical_start + previous.length
```

含义是：

- extent 仍然必须按 logical block 排序
- extent 之间不能重叠
- extent 之间可以有 gap
- gap 就是 sparse hole
- extent 不能超过 inode size 覆盖的逻辑块范围

## 3. 修改了哪些代码文件

### 3.1 [inode.c](/D:/Carl/cryptext4/cryexts/inode.c:1)

这一版主要改动都在 inode 映射层。

新增和调整的函数：

- `cryexts_append_extent_entry_v2()`
- `cryexts_insert_extent_after_v2()`
- `cryexts_punch_hole_blocks()`
- `cryexts_validate_inode()`

`cryexts_append_extent_entry_v2()` 从原来的 append-only 变成 sorted insert。
这样可以支持这种写入顺序：

```text
先写 logical=256
再写 logical=0
```

最终 extent tree 仍然保持：

```text
logical=0
logical=256
```

而不是错误地变成：

```text
logical=256
logical=0
```

### 3.2 [file.c](/D:/Carl/cryptext4/cryexts/file.c:1)

新增 `file_operations.fallocate`：

```text
cryexts_fallocate()
```

当前只支持：

```text
FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE
```

也就是：

- 可以挖洞
- 文件大小不变
- 完整 block 范围释放物理块
- 非 block 对齐的边缘范围写 0

### 3.3 [cryexts.h](/D:/Carl/cryptext4/cryexts/cryexts.h:1)

新增导出原型：

```c
int cryexts_punch_hole_blocks(struct inode *inode, u64 first_block,
                              u64 end_block);
```

它给 `file.c` 的 fallocate 路径调用。

### 3.4 [tools/cryextsck.c](/D:/Carl/cryptext4/cryexts/tools/cryextsck.c:1)

`cryextsck` 的 extent 校验从：

```text
必须连续
```

改成：

```text
必须有序、不能重叠、允许 hole
```

新增检查：

```text
extent mapping 不能超过 inode size
```

也就是说，sparse 文件可以 size 很大、真实 block 很少；
但不能存在 EOF 后还被 extent 映射的脏数据。

### 3.5 [scripts/smoke_v6_3_sparse_file.sh](/D:/Carl/cryptext4/cryexts/scripts/smoke_v6_3_sparse_file.sh:1)

新增 smoke test，覆盖：

- leading hole
- hole read returns zero
- sparse 文件的 `i_blocks` 小于逻辑大小对应的 block 数
- `fallocate -p` punch-hole
- punch 后中间 logical block 不再有 extent
- remount 后 hole 仍然读 0
- `cryextsck` 前后均 clean

## 4. 新增函数说明

### 4.1 `cryexts_insert_extent_after_v2()`

文件位置：[inode.c](/D:/Carl/cryptext4/cryexts/inode.c:550)

职责：

- 在指定 leaf 的某个 extent 后面插入一个新 extent
- 用于 punch-hole 把一条 extent 拆成左右两条

参数说明：

- `blocks`：当前 inode 的内存映射信息
- `leaf_index`：目标 leaf 在 inode root refs 中的下标
- `entry_index`：被拆分的 extent 下标
- `logical`：新插入右半段 extent 的 logical 起点
- `physical`：新插入右半段 extent 的 physical 起点
- `len`：新插入右半段 extent 长度

### 4.2 `cryexts_punch_hole_blocks()`

文件位置：[inode.c](/D:/Carl/cryptext4/cryexts/inode.c:1553)

职责：

- 按 logical block 范围释放已经映射的物理块
- 删除完整落入 hole 的 extent
- 缩短部分相交的 extent
- 必要时把一条 extent 拆成左右两条

范围语义：

```text
[first_block, end_block)
```

也就是包含 `first_block`，不包含 `end_block`。

### 4.3 `cryexts_fallocate()`

文件位置：[file.c](/D:/Carl/cryptext4/cryexts/file.c:259)

职责：

- 接收 VFS fallocate 请求
- 只处理 punch-hole + keep-size
- 对 block 对齐的中间区域调用 `cryexts_punch_hole_blocks()`
- 对非 block 对齐的边缘区域调用 `cryexts_zero_file_range()`
- 更新 `ctime/mtime/i_blocks`
- 写回 inode
- 走 journal begin/commit 边界

## 5. 典型案例

### 5.1 在 1MiB 位置写入 4KiB

命令：

```bash
dd if=tail.bin of=/mnt/sparse/leading-hole.bin bs=4096 seek=256 count=1 conv=notrunc
```

结果：

```text
inode_size = 257 * 4096
extent = logical=256 physical=X len=1
```

逻辑块 `0..255` 没有 extent。
读取这些区域时，`cryexts_read_iter()` 调用 `cryexts_resolve_block(create=false)` 得到 `physical=0`，
于是返回一个全 0 block。

### 5.2 对中间 block punch-hole

初始映射：

```text
extent: logical=0 physical=100 len=4
```

执行：

```bash
fallocate -p -o 4096 -l 4096 /mnt/sparse/punch-hole.bin
```

表示挖掉 logical block 1。

结果：

```text
extent: logical=0 physical=100 len=1
extent: logical=2 physical=102 len=2
```

logical block 1 不再有映射。
读 logical block 1 时返回 0。

## 6. 验收方式

运行：

```bash
chmod +x scripts/smoke_v6_3_sparse_file.sh
./scripts/smoke_v6_3_sparse_file.sh
```

期望结果：

```text
v6.3 sparse file and punch-hole smoke test passed
```

并且 `cryextsck` 在测试前后都应报告 clean。

## 7. 一句话总结

```text
v6.3 让 extent tree 从“连续映射表”升级成“可以表达 hole 的稀疏映射表”。
```
