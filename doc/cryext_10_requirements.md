# CRYEXTS Version 10 需求设计

## 1. Version 10 为什么单独立项

到 `v9.5` 为止，CRYEXTS 已经把下面这些事情基本做出来了：

- on-disk format 主线已经稳定
- journal / replay / orphan / fsck / inspect / repair 主线已经闭环
- raw-device / USB demo 已经能跑
- 文档、smoke、soak、发布门槛已经成体系

但现在最明显的短板已经不是“功能缺失”，而是：

```text
能用，但是慢
```

尤其是 regular file 的 `read/write` 路径，当前更像教学型/MVP 路径：

- 自己在 `read_iter/write_iter` 里逐块搬运
- 每次 I/O 都走 `sb_bread/sb_getblk`
- 没有真正接入 page cache 主路径
- 没有 read-ahead / writeback / dirty page 聚合
- 小写入容易变成 read-modify-write
- 写路径和 metadata/journal 路径耦合偏重

所以 `Version 10` 的目标不是继续横向堆新功能，而是把 CRYEXTS 从：

```text
功能闭环文件系统
```

推进到：

```text
有基础性能优化能力的文件系统
```

一句话定义：

```text
Version 10 = CRYEXTS 的读写性能优化主线版本
```

## 2. Version 10 的核心问题

`Version 10` 主要回答四个问题：

### 2.1 读路径为什么慢

当前 regular file 读取路径没有真正使用 Linux page cache 主线，导致：

- 重复读无法有效命中缓存语义
- 缺少内核通用 read-ahead
- 每次读取都走自定义 block 级搬运和解密

### 2.2 写路径为什么慢

当前写路径更偏同步式 metadata-first 更新模型，导致：

- 小写入频繁 read-modify-write
- 写请求不能借助页缓存合并
- 脏页不能自然进入 writeback 聚合
- 元数据与事务提交路径过重

### 2.3 当前加密路径为什么会进一步放大开销

当前数据块加密在 block I/O 层逐块执行，意味着：

- page cache 未接通前，很难利用更自然的页级缓存收益
- 每个 block 的读写都要经历一次单独的加/解密路径

### 2.4 如何在不破坏现有一致性能力的前提下做优化

`Version 10` 不能为了速度把下面这些能力破坏掉：

- journal v2
- mount-time replay
- metadata checksum
- policy-aware encryption
- extent tree
- dir index

所以它的要求不是“重写整个文件系统”，而是：

```text
把 regular file I/O 主路径接回 Linux 更标准的缓存与回写模型，
同时保住现有 format 和恢复语义。
```

## 3. Version 10 总目标

我建议把 `Version 10` 的总目标定义成：

```text
先完成 regular file 缓存化 I/O 主线，
再完成可对比、可回归、可解释的性能基线，
最后再逐步把加密、journal、writeback 的交互收紧。
```

更具体一点说，`Version 10` 要实现：

1. regular file 接入 page cache 主路径
2. 建立基础 writeback 机制
3. 保留现有一致性边界
4. 建立性能 benchmark 主线
5. 能明确观察“优化前后到底快了多少”

## 4. Version 10 核心需求

## 4.1 regular file 读路径要接入 page cache

这是 `Version 10` 的第一优先级。

### 要求

- regular file 不再长期依赖自定义逐块 `read_iter`
- 引入 `address_space_operations`
- 至少补齐：
  - `read_folio`
  - 必要的页装载逻辑
- regular file 读取优先走 Linux page cache
- 保持现有 block mapping / extent tree / legacy direct+indirect 兼容

### 目标

从：

```text
每次 read 都自己搬块
```

推进到：

```text
页缓存接管 regular file 读路径
```

## 4.2 regular file 写路径要进入 buffered write 主线

这是 `Version 10` 的第二优先级。

### 要求

- 引入：
  - `write_begin`
  - `write_end`
  - `writepages`
- regular file 默认写入走 buffered write
- 小写入不再长期停留在“每次 write_iter 直接搬 4KB block”的模型
- 脏页可以聚合后进入 writeback
- 合理处理页脏化后的 inode size / blocks / mtime / ctime 更新

### 目标

从：

```text
每次 write 请求都立刻走重路径
```

推进到：

```text
先进入页缓存，再进入聚合回写
```

## 4.3 block mapping 层要稳定服务 page cache

page cache 接进来以后，block mapping 不能乱。

### 要求

- 现有 `cryexts_resolve_block()` 继续作为核心 block mapping 入口
- 明确：
  - `create=false` 给读路径用
  - `create=true` 给 write_begin / 页扩展路径用
- extent tree inode
- legacy direct/indirect inode
- sparse file / hole

这些路径都要保证：

```text
page cache 请求逻辑块时，mapping 语义稳定
```

### 目标

不重写 block mapping 语义，只把它从“只服务自定义 read_iter/write_iter”，升级成“服务 page cache + writeback”。

## 4.4 加密路径要和 page cache 路径重新对齐

这是 `Version 10` 的关键难点之一。

### 要求

- 明确加密发生在：
  - folio/page 装载时的解密
  - writeback 落盘前的加密
- 不允许出现：
  - page cache 中保存加密态内容，但 VFS 读到密文
  - 同一页在不同路径下出现明文/密文语义不一致
- policy-aware encryption 继续生效
- root key / policy key / CTR block 语义不变

### 目标

把当前“block I/O 层逐块加解密”收敛到：

```text
能和缓存页生命周期对齐的加密语义
```

## 4.5 journal 与 writeback 的关系要重新收口

page cache 接进来以后，不能再沿用“每次 write_iter 自己 commit 一次”的思路。

### 要求

- 明确 buffered write 下：
  - 哪些 metadata 修改需要 journal record
  - 哪些时刻真正 commit
- 至少保证：
  - truncate
  - block allocation
  - extent tree leaf / overflow 更新
  - inode size 扩展
  - writeback 落盘

不会把当前 journal 语义搞乱

### 目标

从：

```text
同步式 write_iter + journal_commit
```

推进到：

```text
页缓存写入 + 有边界的 metadata 事务处理
```

## 4.6 benchmark 主线必须正式建立

没有 benchmark，性能优化就容易变成“感觉快了”。

### 要求

固定一套最小 benchmark：

- 顺序写
- 顺序读
- 4K 随机读
- 4K 随机写
- 小文件 create/unlink
- 目录扫描
- 加密开/关对比
- image / raw-device 两种介质对比

推荐统一使用：

- `fio`
- `dd`
- `time/find`

并固定输出格式。

### 目标

形成：

```text
Version 10 每个阶段都能量化比较的性能基线
```

## 4.7 observability 要补“性能视角”

现在 inspect 偏结构视角，`Version 10` 还需要性能视角。

### 要求

- 至少能分清当前 I/O 路径是不是 page cache
- 至少能分清是不是 buffered write / writeback
- 最小化打印关键 trace/log：
  - read path
  - write path
  - writeback
  - encryption
  - journal

### 目标

避免后面优化时只能靠猜。

## 5. Version 10 非目标

为了不发散，`Version 10` 明确不优先做：

- 新 on-disk format 大改
- snapshot / reflink / dedupe
- quota
- online resize
- 多设备 RAID
- 网络分布式能力
- 完整 DAX / O_DIRECT
- 极限 benchmark 平台化

一句话：

```text
先把 buffered I/O + page cache 主线做对，
别一上来就追求所有高阶 I/O 模式。
```

## 6. Version 10 版本拆分建议

## 6.1 `v10.0`

主题：

- 性能基线与 I/O 路径建模

交付：

- 明确当前慢点分析
- 固定 benchmark 脚本
- 固定 image / raw-device 测试方法
- 固定性能验收口径

## 6.2 `v10.1`

主题：

- regular file page cache 读路径

交付：

- `read_folio`
- regular file cached read 主线
- 加密读路径与页装载打通
- 基础读性能提升

## 6.3 `v10.2`

主题：

- buffered write 主线

交付：

- `write_begin`
- `write_end`
- 脏页写入基础链路
- inode size 扩展语义打通

## 6.4 `v10.3`

主题：

- writeback 与 metadata 收口

交付：

- `writepages`
- block allocation / extent update / inode update 与 writeback 协同
- journal 边界重新固定

## 6.5 `v10.4`

主题：

- 加密路径与缓存页协同优化

交付：

- page cache 下加解密语义固定
- policy-aware encryption 与 buffered I/O 对齐
- encrypted / non-encrypted 基线对比

## 6.6 `v10.5`

主题：

- 性能稳定化与回归基线

交付：

- benchmark 结果沉淀
- regression 基线
- Version 10 MVP 总结

## 7. Version 10 MVP 定义

我建议把 `Version 10 MVP` 定义成：

```text
CRYEXTS regular file 已经接入 page cache + buffered write 主线，
并且：
1. 读写语义不回退
2. journal/replay 不回退
3. encryption 不回退
4. benchmark 能量化看到优化收益
```

更直接一点：

```text
Version 10 MVP = 先把“慢但能用”推进到“明显更快且还能用”
```

## 8. 一句话总结

如果说：

- `Version 6` 解决的是核心功能闭环
- `Version 7` 解决的是 multi-GDT 与真实设备 demo
- `Version 8` 解决的是开源、评估与文档
- `Version 9` 解决的是部署、恢复、soak、release 工程化

那么：

```text
Version 10 解决的是：
把 CRYEXTS 从“功能型文件系统原型”
推进到“具备基础性能优化能力的文件系统原型”
```
