# CRYEXTS V4.3 Inline Extents

## 1. 阶段目标

V4.3 的目标不是一步做到完整 extent tree，而是先把 CRYEXTS 的 regular
file 映射层从：

```text
direct blocks + single indirect
```

推进到：

```text
inline extent regular file MVP
```

这一版重点是：

- 让新建 regular file 可以使用 extent
- 保持旧的 direct + single indirect 文件仍可读写
- 让 journal 和 `cryextsck` 也能理解 extent inode

## 2. 为什么要做这一版

当前 direct + single indirect 的问题是：

- 连续分配的 block 无法紧凑表达
- 大文件需要越来越多 block 指针
- 以后如果要继续往 extent tree、稀疏文件走，当前结构会越来越吃力

extent 的优势是：

```text
一段连续的物理 block
-> 用一条 extent 记录表示
```

这样更适合：

- 大文件
- 连续分配
- 后续更强的映射层设计

## 3. 当前实现范围

V4.3 当前实现的是最小 inline extent 版本：

- 新增 `CRYEXTS_FEATURE_INCOMPAT_EXTENTS`
- 新增 inode flag `CRYEXTS_INODE_FLAG_EXTENTS`
- regular file 支持 inline extents
- 旧 regular file 仍然兼容 direct + single indirect
- `mkfs.cryexts -X` 创建 extent-capable 镜像
- `cryextsck` 支持 extent inode 校验

这版没有做：

- extent tree
- 稀疏文件优化
- 多级索引
- extent 溢出块

## 4. 磁盘格式思路

### 4.1 superblock feature flag

增加：

- `CRYEXTS_FEATURE_INCOMPAT_EXTENTS`

含义是：

- 如果镜像声明启用了 extents
- 那么旧驱动如果不认识这个特性，就必须拒绝挂载

### 4.2 inode extent 标记

增加：

- `CRYEXTS_INODE_FLAG_EXTENTS`

含义是：

- 这个 inode 不是 legacy block-pointer inode
- 而是 extent-backed inode

### 4.3 extent 存储位置

这次没有单独加 extent tree block，而是直接复用了 inode 里的
`reserved[116]` 区域，放一个小型 inline extent 数组。

布局思路：

```text
inode.reserved
    = extent header
    + inline extent entries
```

## 5. extent 结构

当前一条 extent 记录包含：

- `logical_start`
- `physical_start`
- `length`
- `flags`

含义：

- `logical_start`
  文件逻辑块起点
- `physical_start`
  磁盘物理块起点
- `length`
  连续块数量

也就是说，一条 extent 表示：

```text
文件逻辑块 [L, L+len)
映射到磁盘物理块 [P, P+len)
```

## 6. 当前实现策略

### 6.1 混合模式

V4.3 不是全盘强制换成 extent，而是混合模式：

- 老 inode 可以继续走 legacy direct + indirect
- 新建 regular file 在启用 `EXTENTS` 特性的镜像上默认走 extent

这样做的好处：

- 风险小
- 容易调试
- 回归范围可控

### 6.2 inline extents 数量限制

当前只支持：

- `CRYEXTS_MAX_INLINE_EXTENTS = 4`

也就是说，每个 extent inode 现在最多只能有 4 条 extent。

这是一个非常明确的 MVP 约束，先保证逻辑跑通，再考虑扩展。

### 6.3 连续写优化

当前 extent 分配的核心逻辑是：

1. 如果写入逻辑块正好接在最后一条 extent 后面
2. 并且新分配的物理块也正好连续
3. 那就直接扩展最后一条 extent 的 `length`

否则：

1. 新建一条 extent
2. 填入新的 `logical_start / physical_start / length`

这一步是 extent 的核心收益来源。

## 7. 读写逻辑

### 7.1 读

给定逻辑块 `L`：

1. 顺序扫描 extent 数组
2. 找到哪个 extent 覆盖 `L`
3. 计算真实物理块：

```text
physical = physical_start + (L - logical_start)
```

### 7.2 写

给定逻辑块 `L` 且允许创建：

1. 先查现有 extent 是否已经覆盖
2. 如果覆盖，直接返回对应物理块
3. 如果没有覆盖：
   - 尝试与最后一条 extent 合并
   - 合并不了就新建一条 extent

### 7.3 truncate

缩小时：

1. 找到 EOF 后面需要回收的 extent 范围
2. 释放对应 data block
3. 缩短或删除 extent entry

## 8. 与 journal 的关系

extent 也是 metadata。

所以 inode 里的 extent 数组一旦变化，本质上就是：

- inode table block 被修改了

因此 V4.2 的 journal 机制仍然适用：

- 改 inode extent 前，先保护 inode table 元数据
- 改 bitmap/free count 时，仍然走 journal 路径

也就是说：

```text
V4.3 并没有绕开 journal
而是复用了 V4.2 的 metadata recovery 能力
```

## 9. `cryextsck` 如何理解 extent inode

这一版里，`cryextsck` 会区分两类 regular file：

1. legacy inode
2. extent inode

对于 extent inode，会检查：

- inode flag 是否合法
- extent header 是否合法
- extent 条目数量是否越界
- extent length 是否为 0
- extent logical range 是否连续
- extent physical block 是否在合法数据区
- extent block 是否和别的 inode 重复引用
- inode `blocks` 计数是否匹配 extent 引用块数

## 10. 现阶段边界

这版 V4.3 的明确边界是：

- 只做 regular file
- 只做 inline extents
- 只做顺序扫描 extent 数组
- 不做 extent tree
- 不做复杂 extent split/merge 策略

所以它是：

```text
extent regular file MVP
```

不是完整 extent 文件系统。

## 11. 测试脚本

当前新增了：

- `scripts/smoke_v4_3_extents.sh`

测试目标：

- `mkfs.cryexts -X` 生成 extent-capable 镜像
- 大文件写入和读出正确
- truncate 后大小正确
- remount 后数据仍然正确
- `cryextsck` 保持 clean

## 12. 下一步建议

V4.3 之后推荐继续拆成：

1. `v4.3.0`
   先把 extent 元数据与识别逻辑跑通
2. `v4.3.1`
   extent regular file 读写跑通
3. `v4.3.2`
   truncate / fsck / recovery 路径加固
4. `v4.3.3`
   再考虑 extent 溢出、更多条目、或者 extent tree

这样推进会比一次性重写映射层稳很多。
