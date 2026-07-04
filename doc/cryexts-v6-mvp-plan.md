# CRYEXTS Version 6 MVP 规划

## 1. 这份文档的定位

[cryext_6_requirements.md](/D:/Carl/cryptext4/cryexts/doc/cryext_6_requirements.md:1) 是总需求。

这份文档更关注三件事：

- `Version 6` 先做什么
- 为什么这个顺序最稳
- 什么叫 `Version 6 MVP 完成`

## 2. Version 6 的核心判断

`Version 5` 解决的是：

- 结构有了
- feature flag 有了
- 基本运行时路径有了

但 `Version 6` 要解决的是：

- 这些结构到底有没有完整事务语义
- 大文件和稀疏文件能不能继续往上长
- allocator 是不是更接近真实文件系统
- 诊断和修复是不是也跟得上

所以 `Version 6` 不应该从“再加一个新结构”开始，而应该从：

```text
把已有结构之间的语义关系补完整
```

开始。

## 3. 为什么建议从 journal 先做

这是最关键的一点。

当前 `cryexts` 的很多结构都已经比较像真实文件系统：

- orphan
- extent overflow
- dir index
- policy table
- metadata checksum

但如果 transaction 边界还是过于轻量，那么这些结构越复杂，crash 后恢复就越难解释。

所以 `Version 6` 的顺序应该是：

```text
先补 journal transaction
再扩 extent tree
再做 sparse/delayed allocation
```

这能最大限度减少返工。

## 4. 推荐版本拆分

### 4.1 V6.0

目标：

- 定义 journal v2 磁盘布局
- 引入 descriptor / commit / sequence / checkpoint 这些基础概念
- mount 和 fsck 能识别新布局

这一版重点是：

```text
先把格式和语义边界定下来
```

还不要求一上来就把所有 commit 路径改完。

### 4.2 V6.1

目标：

- metadata transaction commit 真正生效
- replay 只认完整事务
- checkpoint tail 推进 MVP

这一版重点是：

```text
让新 journal layout 真正工作
```

### 4.3 V6.2

目标：

- 多级 extent tree
- internal node / leaf block
- extent tree-aware `cryextsck`

这一版重点是：

```text
让 regular file mapping 真正突破单 overflow block 上限
```

### 4.4 V6.3

目标：

- sparse file
- hole punching
- `fallocate` 预留或最小支持

这一版重点是：

```text
让逻辑空间和物理空间真正脱钩
```

### 4.5 V6.4

目标：

- inode allocator locality
- delayed allocation MVP
- reservation window / 更稳的连续分配

这一版重点是：

```text
从“能分配”走向“分配得更像真实文件系统”
```

### 4.6 V6.5

目标：

- directory index 可增长
- bucket split
- create/rename/unlink 的 index 更新路径更真实

### 4.7 V6.6

目标：

- xattr overflow / large xattr
- inspect / scrub / fsck 工具补齐

## 5. Version 6 MVP 定义

我建议把 `Version 6 MVP` 定义成：

```text
journal 事务边界完整
+ multi-level extent tree
+ sparse file 基本语义
+ allocator locality/delayed-allocation MVP
+ cryextsck 能理解这些结构
```

如果这五块都完成，就可以说：

```text
Version 6 已经从“结构型原型”进入“语义型原型”
```

## 6. 验收标准建议

### 6.1 事务一致性

至少要能验证：

- commit 前 crash 不算事务完成
- commit 后 replay 可恢复
- checkpoint 后旧事务可安全跳过

### 6.2 extent tree

至少要能验证：

- 文件规模明显突破 `v5.2` 上限
- lookup / write / truncate 都走 tree
- `cryextsck` 能识别 internal node / leaf

### 6.3 sparse file

至少要能验证：

- 稀疏区域 read 回零
- 稀疏区域不强制占真实物理块
- truncate / hole punch 后映射自洽

### 6.4 allocator

至少要能验证：

- inode/data locality 都能观察到
- 顺序写连续性比 `v5.6` 更稳定
- 同目录多文件更容易聚在同 group 或相邻 group

## 7. 建议测试结构

建议到 `Version 6` 时，每个小版本都至少有：

- 一个 layout / inspect 类 smoke
- 一个 runtime 行为 smoke
- 一个 fsck / recovery 类 smoke

建议最后总脚本类似：

```text
smoke_v6_0_*.sh
smoke_v6_1_*.sh
...
smoke_version6_mvp.sh
```

## 8. 一句话结论

`Version 6` 不是继续“横向铺新功能”，而是：

```text
把 Version 5 的成果收拢成更完整的事务、映射、分配和维护体系
```

这是最值得做，也最不容易返工的一条路线。
