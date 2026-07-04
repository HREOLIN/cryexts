# CRYEXTS V4.3 修改说明

## 1. 这次改动的核心目标

这次 V4.3 的处理，不是把整个文件系统映射层推翻重写，而是先做一个最小可运行版本：

```text
regular file inline extent MVP
```

核心目标是：

- 新 regular file 可以使用 extent
- 旧文件仍然兼容 direct + single indirect
- `cryextsck` 能看懂 extent inode
- `mkfs` 能创建 extent-capable 镜像

## 2. 共享头文件改了什么

文件：

- `cryexts_fs.h`
- `cryexts.h`

新增了：

- `CRYEXTS_FEATURE_INCOMPAT_EXTENTS`
- `CRYEXTS_INODE_FLAG_EXTENTS`
- `struct cryexts_extent_header`
- `struct cryexts_extent`
- `CRYEXTS_MAX_INLINE_EXTENTS`
- `CRYEXTS_EXTENT_FILE_BLOCKS_MAX`

逻辑是：

1. 文件系统级别要能声明“这个磁盘用了 extent”
2. inode 级别要能声明“这个 inode 用的是 extent 映射”
3. 内存态 inode 结构要能同时容纳：
   - legacy direct/indirect
   - inline extents

## 3. inode.c 改了什么

这是这次最核心的文件。

### 3.1 增加 extent inode 判断

新增了：

- `cryexts_disk_inode_uses_extents()`
- `cryexts_inode_uses_extents()`

作用是把两类 inode 分开：

- 磁盘 inode 是否 extent-backed
- VFS inode 是否 extent-backed

### 3.2 `cryexts_init_inode_blocks()`

以前这里只会把：

- `block[12]`
- `indirect_block`

读进内存。

现在如果发现 `inode_flags` 里带 `CRYEXTS_INODE_FLAG_EXTENTS`，就改成：

- 从 `reserved[]` 读 extent header
- 读 extent entries
- 填到内存态 `struct cryexts_inode_info`

也就是说，inode 初始化现在走两条分支：

1. legacy block pointer inode
2. extent inode

### 3.3 `cryexts_inode_first_block()`

以前默认返回 `direct[0]`。

现在如果是 extent inode，就返回第一条 extent 的 `physical_start`。

这样目录等仍然能复用这个接口，而 extent regular file 也不会拿错第一块。

### 3.4 `cryexts_inode_block_count()` / `cryexts_disk_inode_block_count()`

以前 block 数是：

- direct block 个数
- 加上 indirect block 本身
- 再加 indirect entry 指向的数据块

现在如果是 extent inode，就改成：

- 所有 extent 的 `length` 累加

也就是说 extent inode 的 `blocks` 统计基于 extent 覆盖的数据块数。

### 3.5 `cryexts_regular_file_max_size_for_inode()`

以前 regular file 最大大小是固定的：

- `12 direct + 1 single indirect`

现在增加了 inode 级别版本：

- legacy inode 仍然走旧上限
- extent inode 走 `CRYEXTS_EXTENT_FILE_BLOCKS_MAX`

这样旧文件和新文件的能力边界可以共存。

### 3.6 `cryexts_inode_block_at()`

以前逻辑是：

- 先看 direct
- 再看 indirect table

现在如果是 extent inode：

- 顺序扫描 extent 数组
- 找到覆盖这个 logical block 的 extent
- 计算对应 physical block

### 3.7 `cryexts_resolve_block()`

这是本次最关键的函数。

以前它的职责是：

- 给定逻辑块号
- 在 direct / indirect 里找到或创建 physical block

现在 extent inode 分支的新逻辑是：

1. 先扫描已有 extent，看这个逻辑块是否已存在
2. 如果存在，直接返回对应 physical block
3. 如果不存在且不允许创建，返回 0
4. 如果允许创建：
   - 优先尝试把新块并到最后一条 extent
   - 如果物理块不连续，则新建一条 extent
   - 如果 extent 数组满了，直接报 `ENOSPC`

这里我故意保持简单，没有马上上复杂 split/merge。

### 3.8 `cryexts_free_blocks_from()`

以前它只会处理：

- direct blocks 回收
- indirect entries 回收
- indirect block 本身回收

现在 extent inode 的 shrink/truncate 逻辑是：

- 如果整个 extent 都在 `keep_blocks` 之后，整条删掉并释放块
- 如果只删一部分，就缩短 extent 的 `length`

### 3.9 `cryexts_validate_inode()`

以前只认识 legacy inode。

现在 extent inode 会额外检查：

- inode flag 是否合法
- extent header 是否正确
- extent 数量是否越界
- extent length 是否为 0
- logical range 是否连续
- physical block 是否合法且 bitmap 已标记使用
- legacy direct / indirect 字段是否都为空
- `blocks` 计数是否匹配 extent 覆盖的块数

### 3.10 `cryexts_write_inode_to_disk()`

以前写回磁盘时，只会写：

- direct block 数组
- indirect block
- inode_flags = 0

现在：

- 如果是 extent inode
  - direct/indirect 清零
  - `inode_flags = CRYEXTS_INODE_FLAG_EXTENTS`
  - extent header 写到 `reserved[]`
  - extent entries 写到 `reserved[]`
- 如果是 legacy inode
  - 保持原逻辑

### 3.11 `cryexts_new_inode()`

现在的规则是：

- 只有 regular file
- 只有文件系统开启 `EXTENTS` incompat feature
- 才默认新建成 extent inode

目录、符号链接仍然不走 extent。

如果创建时已经带 `data_block`，比如某些场景直接给第一块数据块：

- extent inode 会生成第一条 extent
- legacy inode 仍然塞 `direct[0]`

## 4. file.c 改了什么

这里主要改了大小上限判断。

以前所有 regular file 都用一个固定上限：

- `cryexts_regular_file_max_size()`

现在改成：

- `cryexts_regular_file_max_size_for_inode(inode)`

这样 extent inode 能获得更大的逻辑空间，而 legacy inode 不受影响。

涉及的位置：

- `write_iter`
- `setattr(truncate)`

## 5. super.c 改了什么

这里只补了一件必要的小事：

- superblock 校验时，把 `CRYEXTS_FEATURE_INCOMPAT_EXTENTS` 加进允许列表

否则 extent 镜像会在 mount 前直接被 superblock 校验拦掉。

## 6. mkfs.cryexts 改了什么

新增了：

- `-X`

语义是：

- 创建 extent-capable 镜像

实现上：

- `features_incompat` 不再只写 `SINGLE_INDIRECT`
- 如果带 `-X`，就写 `EXTENTS`

也就是说：

- 旧镜像可以不带 `-X`
- 新测试镜像可以显式带 `-X`

这样切换更清晰。

## 7. cryextsck 改了什么

### 7.1 superblock feature 识别

`cryextsck` 现在允许识别：

- `CRYEXTS_FEATURE_INCOMPAT_EXTENTS`

### 7.2 extent inode 校验

`validate_inode()` 现在加了 extent 分支。

逻辑是：

1. 如果 inode flag 带 `EXTENTS`
2. 那就按 extent inode 规则检查
3. 不再按 direct/indirect inode 规则检查

并且会把 extent 覆盖到的每个 data block 记到 `block_seen[]`，这样：

- 可以发现重复引用
- `free count` 重算仍然正确

## 8. 新增测试脚本

新增：

- `scripts/smoke_v4_3_extents.sh`

测试流程是：

1. `mkfs.cryexts -G -X`
2. `cryextsck` 先验证 clean
3. 挂载后写入一个较大的随机文件
4. 比较读回内容是否一致
5. truncate 到较小尺寸
6. remount 后再读一次
7. 结束后 `cryextsck` 仍然 clean

这个脚本主要验证：

- extent 写入
- extent 读取
- extent truncate
- remount 持久化

## 9. 这次实现的边界

这次特意没有做：

- extent tree
- 超过 inline extent 数量后的溢出设计
- 稀疏 extent
- 复杂的 extent split/merge

这样做是为了先保证：

```text
extent regular file 这条最小主线先跑通
```

## 10. 审核建议

你审核这次代码时，建议重点看这几件事：

1. extent inode 和 legacy inode 是否真正分流清楚
2. extent 写回到磁盘时，`reserved[]` 编码是否一致
3. truncate/free block 时，extent length 和 free bitmap 是否同步
4. `cryextsck` 是否能正确识别 extent block ownership
5. extent inode 的 `blocks` 计数是否始终和真实引用块数一致

这五点是当前 V4.3 第一版最容易出错的地方。
