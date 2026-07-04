# CRYEXTS V5.2 extent overflow 设计与实现说明

## 1. 这次要解决什么问题

V4.3 的 extent 版本只支持 inode 内联的少量 extent。

它的优点是简单，但上限也很明显：

- inode 里只能放几条 extent
- 文件一旦出现更多碎片，就会很快耗尽 inline extent 槽位
- 这会让大文件虽然“支持 extent”，但仍然不够可扩展

所以 V5.2 的目标不是一步做到完整多级 extent tree，而是先把第一步做扎实：

```text
inode inline extents
    +
单层 extent overflow block
```

也就是说，这一版做的是：

- extent root 仍然放在 inode 里
- inline 放不下时，再挂一个 extent overflow block
- read / write / truncate / fsck 都要能理解这个结构

这还是 MVP，但它已经不再是“只能 4 条 inline extent”的雏形了。

## 2. 这一版不做什么

V5.2 明确不做：

- 多级 extent tree
- extent node split / merge 平衡
- extent 索引层
- extent defrag
- 稀疏 extent 优化

这次我们只做：

```text
single-level extent overflow
```

先把真实可用的大文件路径跑通。

## 3. 新的 inode extent root 布局

### 3.1 旧布局的问题

之前 extent inode 的 `reserved[116]` 大致是：

```text
extent header
+ 4 条 inline extent
+ inode extra trailer
```

后来 V4.4 / V5.1 又在 trailer 里放了：

- `xattr_block`
- `encryption_policy_id`
- `next_orphan`

这样 `reserved[]` 已经很紧了。

如果继续硬塞更多 inline extent 或额外指针，就很容易和 trailer 冲突。

### 3.2 V5.2 的新 root 思路

所以 V5.2 在启用 `EXTENT_TREE` 路径时，把 inode root 改成：

```text
reserved[]
├── extent header
├── 3 条 inline extent
├── overflow_block 指针
├── overflow_entries
└── inode extra trailer
```

注意，这里是：

- 老格式：4 条 inline extent
- 新格式：3 条 inline extent + 1 个 overflow block 指针

这样做的意义是：

1. 不破坏原有 trailer 的落点
2. inode 自身仍然保留“root extent header + 少量 inline extent”
3. 为后续更完整的 extent tree 留出稳定演进路径

## 4. overflow block 是什么

overflow block 本质上就是一个“额外的 extent 数组块”。

它的内容布局是：

```text
extent overflow block
├── extent header
└── extent entries[]
```

header 里记录：

- magic
- entries
- max

后面紧跟多条 extent entry。

所以你可以把它理解成：

```text
inode 里的 root extent 表放不下了
-> 再挂一个“第二张 extent 表”
```

但这一版只有这一张额外表，没有再往下挂第二层、第三层。

## 5. 读路径怎么走

### 5.1 逻辑

给定文件逻辑块 `L`：

1. 先扫 inode 里的 inline extents
2. 如果没命中，再扫 overflow block 里的 extents
3. 找到覆盖 `L` 的 extent 后，计算：

```text
physical = physical_start + (L - logical_start)
```

### 5.2 结果

这样对上层 `read_iter()` 来说没有区别。

它仍然只是问：

```text
logical block -> physical block
```

只是 `resolve_block()` 背后的 extent 来源，从“只有 inode inline”变成了：

```text
inode inline + overflow extent block
```

## 6. 写路径怎么走

### 6.1 命中已有 extent

如果逻辑块已经落在某条 extent 覆盖范围里：

- 直接返回已有 physical block

### 6.2 顺序追加

如果是顺序写到最后一条 extent 后面：

- 先尝试分配一个新 data block
- 如果它和最后一条 extent 的 physical 连续
- 就直接把最后一条 extent 的 `length + 1`

这和 V4.3 的思想一致。

### 6.3 需要新建 extent

如果不能并到最后一条 extent：

- 先看 inode inline 还有没有槽位
- inline 有空位就加到 inode root
- inline 满了就分配 / 使用 overflow block
- 新 extent 追加到 overflow extent 数组里

## 7. truncate / shrink 怎么走

这次的收缩路径也要同时理解两层 extent：

```text
inline extents
+ overflow extents
```

当 `truncate` 缩小时：

1. 遍历总 extent 序列
2. 完全落在 EOF 之后的 extent：
   - 释放对应 data blocks
   - 删除 extent entry
3. 部分跨过 EOF 的 extent：
   - 只释放尾部 block
   - 缩短该 extent 的 `length`

如果 overflow extent 都删空了：

- 释放 overflow block 本身
- inode root 里的 `overflow_block = 0`
- `overflow_entries = 0`

## 8. block 计数怎么理解

之前 inline-only extent inode 的 `i_blocks` 只统计 data block。

V5.2 开始，extent overflow block 自己也是 metadata block，也占磁盘块。

所以 regular file 的 block count 需要统计：

- 所有 extent 覆盖的数据块
- 如果存在 overflow block，再加 1 个 metadata block

也就是说：

```text
total blocks
= data blocks
+ extent overflow metadata block
```

这也是 `cryextsck` 后面要校验的一部分。

## 9. 为什么这版还不叫完整 extent tree

因为现在的结构还是：

```text
inode root
-> 一个 overflow extent block
```

它没有：

- index node
- 多级 child node
- 搜索树层级
- split / rebalance

所以更准确地说，它是：

```text
extent overflow block MVP
```

但它已经是从 inline extent 走向真正 extent tree 的第一步。

## 10. `cryextsck` 需要新增什么理解

V5.2 的 `cryextsck` 要新增理解：

- extent root 是旧格式还是新格式
- overflow block 本身是否合法
- overflow header 是否合法
- overflow entries 是否越界
- 逻辑范围是否连续
- physical block 是否落在合法数据区
- overflow block 是否和别的 inode 重复引用
- regular file 的 block count 是否包含 overflow block

也就是说，`fsck` 不只是检查 data blocks，还要检查：

```text
extent metadata ownership
```

## 11. 这版测试要验证什么

V5.2 的 smoke 至少要覆盖：

1. 创建启用 extents 的 V5 镜像
2. 连续写一个足够大的文件，让它超过 inline extent 能力
3. 确认写入 / 读回正确
4. remount 后再次读回正确
5. truncate 后内容与大小正确
6. 最终 `cryextsck` clean

最好让测试文件的 extent 数量明确超过 root inline 条数，这样才能真正覆盖 overflow 路径。

## 12. 这一版在版本演进中的位置

现在 Version 5 的主线已经是：

- V5.0：先把 on-disk 预留字段和 feature flag 摆好
- V5.1：把 orphan list 跑通
- V5.2：把 extent 从 inline-only 升级到 overflow block

这样后面再做：

- 目录索引
- policy table runtime
- 更强的 metadata csum
- 真正多级 extent tree

就都有比较稳定的地基了。
