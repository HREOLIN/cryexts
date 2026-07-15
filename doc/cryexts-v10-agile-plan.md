# CRYEXTS Version 10 敏捷规划文档

## 1. 规划原则

`Version 10` 不适合一口气大改完再看结果。

因为它牵涉：

- regular file I/O 主路径
- page cache
- buffered write
- writeback
- encryption
- journal
- benchmark

这些东西一旦一次性改太多，最后很容易出现：

```text
功能回退了
性能却不一定真的提升了
```

所以 `Version 10` 必须按敏捷方式推进：

```text
一小步一小步改
+ 每一步都可编译
+ 每一步都可验证
+ 每一步都能量化收益
```

一句话：

```text
Version 10 不追求“大重构一次到位”，
只追求“每个 Sprint 都能落一个稳定增量”。
```

## 2. 敏捷开发模型

我建议采用下面这套最小敏捷模型：

## 2.1 Epic

只有一个总 Epic：

```text
EPIC-V10-PERF
Regular file I/O 性能主线优化
```

## 2.2 Theme

拆成五个 Theme：

1. 性能基线
2. Cached read
3. Buffered write
4. Writeback + journal
5. 性能回归与稳定化

## 2.3 Sprint

每个 Sprint 只做一个能闭环的小主题。

建议每个 Sprint 都包含：

1. 需求确认
2. 最小代码改动
3. smoke 回归
4. benchmark 对比
5. 文档补齐

## 2.4 Definition of Done

每个 Sprint 完成的标准统一为：

- 能编译
- 核心 smoke 不回退
- 新增路径有最小验证脚本
- benchmark 有对比数据
- 文档有 change notes / 设计说明

## 3. Sprint 规划

## 3.1 Sprint 10.0

主题：

- 建性能基线，不改主路径

目标：

- 把“为什么慢”量化清楚
- 固定后续每一步都复用的 benchmark 方法

用户故事：

- 作为开发者，我想知道当前瓶颈在哪里，这样后续优化不是盲改
- 作为开发者，我想每次优化后都有一组固定对比数据

任务：

- 梳理当前 regular file read/write 路径
- 增加 benchmark 脚本
- 固定测试矩阵：
  - image
  - raw-device
  - encrypted
  - plain
- 记录 baseline 数据

交付物：

- `cryexts-v10.0-performance-baseline.md`
- `scripts/bench_v10_0_baseline.sh`

验收：

- 能输出顺序读写、小文件 create/unlink、目录扫描基线
- 能对同一介质重复测试

## 3.2 Sprint 10.1

主题：

- regular file cached read

目标：

- 接入 page cache 读路径

用户故事：

- 作为开发者，我希望 regular file 的重复读取能享受 page cache
- 作为用户，我希望读同一个文件不会每次都重新逐块解密搬运

任务：

- 引入 `address_space_operations`
- 实现 `read_folio`
- regular file read 优先走缓存页
- 保持 extent / sparse / legacy mapping 兼容
- 处理 cached read 下的解密语义

交付物：

- cached read 代码
- 最小 read smoke
- `cryexts-v10.1-change-notes.md`

验收：

- read 相关 smoke 全过
- 重复顺序读比 `v10.0` 有明显改善

## 3.3 Sprint 10.2

主题：

- buffered write 基线

目标：

- 把 regular file 写入从“自定义 block 搬运”推进到 buffered write

用户故事：

- 作为开发者，我希望小写入先落页缓存，减少同步重路径开销
- 作为用户，我希望顺序写入和覆盖写不再这么慢

任务：

- 实现 `write_begin`
- 实现 `write_end`
- 打通 dirty page 基本语义
- 处理 size 扩展与 block allocation

交付物：

- buffered write 代码
- 最小写入 smoke
- `cryexts-v10.2-change-notes.md`

验收：

- write / truncate / remount 不回退
- 顺序写比 `v10.0` 有明显改善

## 3.4 Sprint 10.3

主题：

- writeback 与 metadata/journal 收口

目标：

- buffered write 不只是“能写”，而是“能合理回写”

用户故事：

- 作为开发者，我希望 dirty pages 能聚合后回写，而不是每次都走重提交
- 作为开发者，我希望 journal 边界在 writeback 下依然清楚

任务：

- 实现 `writepages`
- 收紧 writeback 时 metadata 更新
- 明确 journal record / commit 边界
- 跑 recovery / replay 回归

交付物：

- writeback 代码
- writeback smoke
- recovery regression
- `cryexts-v10.3-change-notes.md`

验收：

- replay / fsck / encrypted write 不回退
- 顺序写、小文件混合写进一步改善

## 3.5 Sprint 10.4

主题：

- 加密路径与缓存协同

目标：

- 让 page cache + buffered write 下的加密路径稳定下来

用户故事：

- 作为开发者，我希望加密文件在 cached read/buffered write 下语义一致
- 作为用户，我希望加密带来的性能损耗可测、可解释

任务：

- 明确页缓存里保存的是明文还是其他受控状态
- 保证 writeback 前加密、read_folio 后解密语义一致
- 对 policy-aware encryption 做回归
- 输出 plain / encrypted 对比 benchmark

交付物：

- 加密缓存路径代码
- 加密回归脚本
- `cryexts-v10.4-change-notes.md`

验收：

- plain / encrypted 都能稳定读写
- 加密性能损耗有可重复数据

## 3.6 Sprint 10.5

主题：

- 性能稳定化与 Version 10 MVP 收口

目标：

- 固定 Version 10 的 benchmark / regression / 文档出口

用户故事：

- 作为开发者，我希望后续每次性能改动都有对照基线
- 作为项目维护者，我希望知道 Version 10 到底有没有成功

任务：

- 汇总 benchmark 数据
- 固定 regression 脚本
- 补齐 Version 10 MVP 文档
- 列出已知性能边界和后续项

交付物：

- `cryexts-version10-mvp.md`
- `scripts/bench_version10_mvp.sh`

验收：

- Version 10 的收益、边界、后续路线都能说清

## 4. Backlog 优先级

## P0

- 性能基线
- cached read
- buffered write

## P1

- writeback
- journal 协同
- 加密路径协同

## P2

- 更细粒度 observability
- 更完整随机 I/O benchmark
- 更激进优化

## 5. 风险清单

## 5.1 最大风险

最大风险不是“性能没提升”，而是：

```text
性能提升了，
但一致性、replay、加密语义退了
```

所以每个 Sprint 都必须跑这些回归：

- mount / remount
- journal replay
- fsck
- encrypted file I/O
- extent tree file I/O
- sparse file

## 5.2 第二类风险

第二类风险是：

```text
接了 page cache，
但实际上只接了一半
```

比如：

- read 路径 cached 了，write 还在旧模型
- write 路径 buffered 了，但 writeback/journal 语义没收好

所以 `Version 10` 不能跳 Sprint，必须按顺序来。

## 6. 指标体系

每个 Sprint 至少追这几类指标：

- 顺序读 MB/s
- 顺序写 MB/s
- 4K 随机读 IOPS
- 4K 随机写 IOPS
- 小文件 create/unlink 耗时
- encrypted vs plain 差异
- smoke/soak/replay 是否回退

## 7. 文档规则

从 `Version 10` 开始，继续保持你前面的文档标准：

- 需求文档：讲目标、边界、拆分
- change notes：讲本版改了什么
- code walkthrough：讲关键函数和数据路径
- MVP 总结：讲这一阶段是否闭环

如果增加结构体：

- 每个字段讲清作用

如果增加函数：

- 每个关键函数讲清输入、输出、职责、在链路里的位置

## 8. 推荐起步顺序

最稳的起步顺序就是：

1. `v10.0` 先做 benchmark 基线
2. `v10.1` 只做 cached read
3. `v10.2` 再做 buffered write

不要一开始就同时改：

- read
- write
- writeback
- encryption
- journal

那样最容易炸。

## 9. 一句话总结

`Version 10` 的敏捷主线不是：

```text
一次性重写 I/O 子系统
```

而是：

```text
先量化，再接 page cache，
再接 buffered write，
再收 writeback / journal / encryption，
每一步都可回归、可比较、可解释。
```
