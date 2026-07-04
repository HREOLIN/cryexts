# CRYEXTS v6.3 代码处理说明

## 1. 这份文档看什么

这份文档说明 v6.3 的 sparse file 和 punch-hole 是如何在代码里跑起来的。

重点路径：

```text
read hole
write sparse block
punch-hole
fsck sparse-aware validation
```

## 2. 读 hole 的代码路径

入口：[file.c](/D:/Carl/cryptext4/cryexts/file.c:109)

函数：

```c
ssize_t cryexts_read_iter(struct kiocb *iocb, struct iov_iter *to);
```

流程：

1. 根据 `pos` 计算 logical block
2. 调用 `cryexts_resolve_block(inode, logical, false, &physical)`
3. 如果 `physical != 0`，读取真实 block
4. 如果 `physical == 0`，填充一个全 0 block
5. 拷贝到用户 buffer

这里的关键是：

```text
create=false
```

这表示读取时绝对不分配新 block。
找不到映射，就是 hole。

## 3. sparse write 的代码路径

入口：[file.c](/D:/Carl/cryptext4/cryexts/file.c:163)

函数：

```c
ssize_t cryexts_write_iter(struct kiocb *iocb, struct iov_iter *from);
```

流程：

1. 根据 `pos` 计算 logical block
2. 调用 `cryexts_resolve_block(inode, logical, true, &physical)`
3. 如果 mapping 已存在，返回原 physical block
4. 如果 mapping 不存在，分配新 physical block
5. 把新映射插入 extent tree
6. 写入用户数据
7. 如果写入位置超过旧 size，更新 inode size

v6.3 的重点在第 5 步：

```text
新 extent 不再只能追加到最后，而是按 logical 插入
```

## 4. `cryexts_resolve_block()`

位置：[inode.c](/D:/Carl/cryptext4/cryexts/inode.c:1123)

职责：

- 查询 logical block 是否已经有映射
- `create=false` 时，查不到返回 `physical=0`
- `create=true` 时，查不到就分配并插入映射

对 sparse file 来说，它是读写路径共同使用的核心函数。

例子：

```text
logical=256
当前 extent tree 为空
create=true
```

结果：

```text
分配 physical block
插入 extent logical=256 len=1
```

logical block `0..255` 不会被分配。

## 5. `cryexts_append_extent_entry_v2()`

位置：[inode.c](/D:/Carl/cryptext4/cryexts/inode.c:500)

职责：

- 向 tree-v2 inode 插入一条新的 extent
- 保证 extent 按 logical block 排序
- 必要时创建新的 leaf block

v6.2 时它更接近 append-only。
v6.3 后它会先扫描已有 leaf 和 extent，找到第一个 `entry_logical` 大于新 logical 的位置。

伪代码：

```text
for each leaf:
  for each extent:
    if new_logical < extent.logical_start:
      insert here

if no earlier slot:
  append to last leaf
```

这样 sparse 写入可以乱序发生，但落盘结构仍然有序。

## 6. `cryexts_fallocate()`

位置：[file.c](/D:/Carl/cryptext4/cryexts/file.c:259)

职责：

- 实现最小 fallocate 支持
- 当前只支持 punch-hole + keep-size

支持的 mode：

```text
FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE
```

不支持的 mode 会返回：

```text
-EOPNOTSUPP
```

处理流程：

1. 检查 inode 必须是 regular file
2. 检查 mode 必须是 punch-hole + keep-size
3. 把 hole 范围裁剪到当前 inode size 内
4. 开启 journal transaction
5. 对未对齐的头部范围写 0
6. 对未对齐的尾部范围写 0
7. 对完整 block 范围调用 `cryexts_punch_hole_blocks()`
8. 更新 `i_blocks/ctime/mtime`
9. 写回 inode
10. commit journal

## 7. `cryexts_punch_hole_blocks()`

位置：[inode.c](/D:/Carl/cryptext4/cryexts/inode.c:1553)

职责：

- 释放 `[first_block, end_block)` 范围内的完整映射 block
- 修改 extent tree，让被释放的 logical block 变成 hole

它只处理 tree-v2 extent inode。
如果不是 tree-v2，会返回：

```text
-EOPNOTSUPP
```

## 8. punch-hole 修改 extent 的方式

### 8.1 删除整条 extent

命中范围覆盖整条 extent：

```text
memmove 后续 extent
leaf->entries--
```

### 8.2 截掉左侧

hole 覆盖 extent 左边：

```text
extent.logical_start = punch_end
extent.physical_start = old_physical + removed_blocks
extent.length = old_end - punch_end
```

### 8.3 截掉右侧

hole 覆盖 extent 右边：

```text
extent.length = punch_start - logical
```

### 8.4 中间拆分

hole 落在 extent 中间：

```text
左半段留在原 extent
右半段通过 cryexts_insert_extent_after_v2() 插入
中间 block 释放
```

## 9. `cryexts_insert_extent_after_v2()`

位置：[inode.c](/D:/Carl/cryptext4/cryexts/inode.c:550)

职责：

- 在一个 extent 后面插入新 extent
- 主要给 punch-hole split 使用

为什么需要它？

因为从一条 extent 中间挖洞时，一条连续映射会变成两条不连续映射。

例子：

```text
原 extent:
logical=0 physical=100 len=4

punch logical=1

结果:
logical=0 physical=100 len=1
logical=2 physical=102 len=2
```

这里第二条 extent 就是插入出来的。

## 10. `cryexts_validate_inode()`

位置：[inode.c](/D:/Carl/cryptext4/cryexts/inode.c:1520)

v6.3 修改了 extent 校验规则。

旧逻辑：

```text
logical == expected_logical
expected_logical += len
```

新逻辑：

```text
logical >= next_logical
next_logical = logical + len
```

同时增加 EOF 约束：

```text
next_logical <= ceil(inode_size / block_size)
```

所以：

- hole 合法
- 重叠非法
- 乱序非法
- EOF 后映射非法

## 11. `validate_extent_array()`

位置：[tools/cryextsck.c](/D:/Carl/cryptext4/cryexts/tools/cryextsck.c:1389)

`cryextsck` 和内核保持同样语义：

```text
ordered
non-overlapping
sparse-aware
```

它不再报告：

```text
extent logical ranges are not contiguous
```

而是报告：

```text
extent logical ranges overlap or are not sorted
```

## 12. smoke 测试案例

脚本：[smoke_v6_3_sparse_file.sh](/D:/Carl/cryptext4/cryexts/scripts/smoke_v6_3_sparse_file.sh:1)

### 12.1 leading hole

命令核心：

```bash
dd if=tail.bin of=leading-hole.bin bs=4096 seek=256 count=1 conv=notrunc
```

验证：

- 文件大小是 `257 * 4096`
- logical block 0 读出来是全 0
- inspector 能看到 `logical=256`
- `inode_blocks` 明显小于逻辑大小对应的 block 数

### 12.2 punch-hole

命令核心：

```bash
cp punch-src.bin punch-hole.bin
fallocate -p -o 4096 -l 4096 punch-hole.bin
```

验证：

- 文件大小仍然是 16KiB
- logical block 1 读出来是全 0
- inspector 能看到 `logical=0 len=1`
- inspector 能看到 `logical=2 len=2`
- inspector 看不到 `logical=1`

## 13. 一句话理解代码变化

```text
v6.3 没有发明新的 hole 结构，而是让“没有 extent 的 logical 区间”正式成为文件系统语义的一部分。
```
