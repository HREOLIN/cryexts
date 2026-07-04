# CRYEXTS Version 6 需求分析

## 1. Version 6 目标

到 `Version 5` 为止，`CRYEXTS` 已经完成了这样一条主线：

```text
mkfs -> mount -> create/read/write
-> block groups
-> metadata journal / replay
-> orphan list
-> extent overflow
-> directory index
-> policy-aware encryption
-> metadata checksum
-> locality hint / contiguous preference
```

所以 `Version 5` 的定位更像：

```text
recoverable + scalable + policy-aware filesystem prototype
```

而 `Version 6` 的目标，不再只是“把结构做出来”，而是把这些结构真正推进成：

```text
transaction-complete + sparse-aware + allocator-smarter + maintenance-friendly prototype
```

一句话概括：

```text
Version 6 = 把 Version 5 的“结构型能力”推进成“语义更完整、性能更真实、维护更可控”的下一阶段原型
```

## 2. Version 5 基线

当前我们已经具备：

- `v5` superblock / feature flags
- block groups / group-aware allocator
- metadata journal / mount-time replay
- orphan list cleanup
- extent overflow regular file
- directory hash index MVP
- policy table / policy-aware encryption
- metadata checksum
- prealloc / locality hint MVP
- `cryextsck` 对这些结构的理解

但仍然明显缺少：

- 真正完整的 journal transaction / commit / checkpoint 语义
- sparse file / hole punching / fallocate
- 多级 extent tree
- inode allocator 的 locality 感知
- 更真实的目录索引分裂与增长
- xattr 扩展存储
- 更强的 fsck / repair / inspect / scrub 能力

## 3. Version 6 四条主线

我建议 `Version 6` 聚焦四条线：

### 3.1 事务一致性主线

把当前“教学型 metadata journal”推进成“更完整的 transaction 模型”。

### 3.2 大文件与稀疏文件主线

把当前“extent overflow”推进成“真正可扩展的 extent tree + sparse file 语义”。

### 3.3 分配器与局部性主线

把当前“block locality hint”推进成“inode + data + delayed allocation 风格的更真实分配器”。

### 3.4 可维护性与运维主线

把当前 `cryextsck` 和 inspect 能力推进成更完整的离线检查、修复和诊断体系。

## 4. 总体设计原则

`Version 6` 不建议一开始就追求“功能非常多”，而应坚持：

```text
先补语义边界
再补数据结构层级
再补分配与性能
最后补维护与运维能力
```

更具体地说：

- 先解决“crash 之后哪些状态算正确”
- 再解决“哪些磁盘结构已经不能再靠 MVP 拼接”
- 再解决“怎么更像真实文件系统那样分配和扩展”
- 最后解决“怎么检查、修复、解释这些结构”

## 5. Journal / Transaction 需求

这是 `Version 6` 最值得优先做的一条主线。

### 5.1 当前问题

当前 journal 已经能做：

- metadata block 备份
- mount-time replay
- checksum
- recovery state 切换

但它还不是完整的 transaction 模型。

目前还缺少：

- 明确的 descriptor / commit 边界
- transaction sequence 的真实推进
- checkpoint 概念
- revoke / obsolete entry 的处理
- 跨多个 metadata block 的更清晰回放语义

### 5.2 Version 6 目标

建议补齐：

- `journal descriptor block`
- `journal commit block`
- transaction sequence
- checkpoint / tail 推进的基本语义
- replay 时只回放“descriptor 完整 + commit 完整”的事务

### 5.3 推荐边界

`Version 6` 可以先不做：

- full data journaling
- ordered / writeback / journal 三模式切换
- online log resize

但至少要做成：

```text
metadata transaction 是一条完整闭环：
begin -> record -> commit -> replay -> checkpoint
```

## 6. Mapping / Sparse File 需求

### 6.1 当前问题

`v5.2` 解决了 extent overflow，但本质上还是：

- inode inline extents
- 再加一个 overflow extent block

它还不是标准多级 extent tree。

这意味着：

- 大文件规模还是有限
- sparse file 语义不完整
- truncate / hole punch 的演进空间有限

### 6.2 Version 6 目标

建议推进到：

- extent internal node
- extent leaf block
- extent root in inode
- logical range 查找走 tree
- 支持大于单 overflow block 上限的大文件

### 6.3 Sparse 语义

建议补：

- sparse file
- hole punching
- `fallocate` 预留接口
- unwritten extent 预留语义

这里不一定要一次性全做完，但至少要让结构和代码路径能够支持：

```text
logical offset 有洞
并不要求一定有物理块
```

## 7. Allocator / Locality 需求

### 7.1 当前问题

`v5.6` 已经做了：

- per-file block hint
- directory locality hint
- contiguous preference

但它还没有：

- inode allocator locality
- reservation window
- delayed allocation
- 多文件之间更真实的竞争与回退策略

### 7.2 Version 6 目标

建议推进到：

- inode allocator 支持 `goal_group`
- regular file delayed allocation MVP
- reservation window / run 预留
- directory tree 级别的 locality

也就是说，`Version 6` 的 allocator 要开始同时关心：

- inode 放哪里
- data block 放哪里
- extent 怎么尽量连续

### 7.3 暂不追求

先不做：

- full buddy allocator
- online defrag
- background allocator daemon

## 8. Directory / Namespace 需求

### 8.1 当前问题

`v5.3` 的目录索引已经能做 hash-based lookup MVP，但还比较“平”：

- 固定 bucket 数
- block mask 方式
- 更像轻量索引块

它还不是更真实的 HTree/多层目录索引。

### 8.2 Version 6 目标

建议推进到：

- bucket split / growth
- index root + leaf 风格
- 更大的目录块覆盖范围
- rename/unlink/create 下更稳定的更新路径

### 8.3 推荐边界

可以先不做真正多层 B-tree，但至少要做成：

- index 可扩展
- bucket 不再固定死在一个小 mask 模型上
- `cryextsck` 能理解分裂后的结构

## 9. xattr / Policy / Encryption 需求

### 9.1 xattr

`v4.4/v5.4` 之后，xattr 和 policy 已经够用来支撑多策略加密，但存储模型还是单块为主。

建议 `Version 6` 推进：

- large xattr
- xattr overflow / spill block
- xattr checksum / repair

### 9.2 policy / encryption

`Version 5` 已经实现：

- policy table
- policy id 生效
- 目录 policy 继承

`Version 6` 建议新增：

- online policy table update 预留
- key rotation metadata 预留
- per-policy stats / inspect

仍然可以先不做：

- 文件名加密
- authenticated encryption
- keyring 深度集成

## 10. Integrity / Repair / Observability 需求

### 10.1 `cryextsck`

`Version 6` 的 `cryextsck` 目标应从“理解结构”进一步推进到：

- 理解 journal transaction 边界
- 理解 multi-level extent tree
- 理解 sparse / hole
- 理解扩展后的 directory index
- 理解 large xattr

### 10.2 建议新增工具

建议增加几类 inspect 工具：

- `cryexts_journal_inspect`
- `cryexts_extent_tree_inspect`
- `cryexts_bitmap_inspect`
- `cryexts_xattr_inspect`

### 10.3 Repair 边界

`Version 6` 仍建议坚持低风险 repair：

- 修 super/group free count
- 修 recovery state
- 修明显孤立的 journal tail/head 不一致
- 修空洞但可明确判断的 bitmap mismatch

先不要自动修：

- extent tree 拓扑损坏
- 复杂目录索引错链
- xattr 语义冲突

## 11. Version 6 推荐分阶段路线

### V6.0

- journal v2 layout baseline
- descriptor / commit / sequence / checkpoint 结构预留
- mount / fsck 能识别新 journal 结构

### V6.1

- metadata transaction commit 真实生效
- replay 只认完整事务
- checkpoint / tail 推进 MVP

### V6.2

- multi-level extent tree MVP
- extent internal node / leaf
- `cryextsck` extent tree 校验

### V6.3

- sparse file
- hole punching
- `fallocate` / unwritten extent 预留

### V6.4

- inode allocator locality
- delayed allocation MVP
- reservation window / 更真实连续分配

### V6.5

- scalable directory index
- bucket split / grow
- index-aware create/unlink/rename

### V6.6

- large xattr / xattr overflow
- 更完整 inspect / fsck / scrub 工具链

## 12. Version 6 MVP 建议定义

如果给 `Version 6` 设一个清晰的 MVP，我建议是：

```text
journal transaction 边界完整
+ multi-level extent tree
+ sparse file 基本语义
+ inode/data locality 一起生效
+ cryextsck 能理解并诊断这些新结构
```

它不要求一开始就“特别快”，但必须要求：

```text
事务更完整
映射更可扩展
分配更真实
诊断更可靠
```

## 13. Version 6 非目标

`Version 6` 不建议一上来追求：

- snapshot
- reflink
- dedupe
- full data journaling
- online resize
- quota
- POSIX ACL 全实现
- fs-verity / fscrypt 完整兼容

这些都很容易把版本目标拖散。

## 14. 推荐下一步

如果按“最稳的推进顺序”，我建议从这里开始：

1. 先做 `V6.0 journal v2 layout`
2. 再做 `V6.1 transaction commit / replay`
3. 然后做 `V6.2 multi-level extent tree`
4. 再做 `V6.3 sparse file`
5. 然后再补 `V6.4 allocator`

核心原因很简单：

```text
先把 crash 语义做完整
再把映射结构做大
最后再做性能和局部性优化
```

这条路线最不容易返工。
