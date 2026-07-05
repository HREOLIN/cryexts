# CRYEXTS Version 8 需求设计

## 1. Version 8 为什么要单独设计

到 `v7.3` 为止，CRYEXTS 已经完成了两件很重要的事：

- `Version 6 MVP` 把核心文件系统语义做出来
- `Version 7` 把多块 GDT、raw-device、USB demo 主线接通

也就是说，当前项目已经不再只是“会跑的小型实验文件系统”，而是已经进入下面这个阶段：

```text
核心能力基本齐了
+ 真实设备演示路径已经有了
+ 需要开始收口成可开源、可交付、可评估的工程版本
```

所以 `Version 8` 的重点不应该是继续快速堆新 on-disk feature，
而应该是把项目从：

```text
demo-ready
```

推进到：

```text
open-source-ready
+ evaluation-ready
+ maintainable-ready
```

一句话定义：

```text
Version 8 = CRYEXTS 的发布工程化与评估工程化版本
```

## 2. 当前已经具备的基础

进入 `Version 8` 之前，默认已经具备：

- block groups
- journal v2
- orphan list / metadata checksum
- policy-aware encryption
- extent overflow / extent tree
- scalable directory index
- large xattr
- multi-block GDT
- raw-device / USB demo 路径

所以 `Version 8` 不再解决“能不能做出来”，
而是解决：

```text
怎么把现有能力收成一个别人可以理解、复现、评估、继续维护的项目
```

## 3. Version 8 主目标

`Version 8` 的主目标建议定义为：

```text
冻结一条可解释的格式主线
+ 建立一条可重复的发布/测试主线
+ 建立一套可公开的文档/风险/边界主线
```

更具体地说，`Version 8` 要解决的是下面三类问题：

### 3.1 发布问题

- 别人如何构建
- 别人如何跑 smoke
- 别人如何做 image demo
- 别人如何做 raw-device demo

### 3.2 维护问题

- 哪些版本是兼容的
- 哪些 feature 是实验性的
- 出现问题时先看哪里
- 出现损坏时怎么 replay / fsck / repair

### 3.3 评估问题

- 项目当前适合什么场景
- 明确不适合什么场景
- 想开源需要哪些最少资料
- 想做后续商用评估还缺哪些东西

## 4. Version 8 核心需求

## 4.1 格式与兼容性边界要明确

`Version 8` 必须把当前格式边界写清楚。

### 要求

- 明确当前主格式线是什么
  - 建议以 `Version 6 + Version 7` 已落地结构为主线
- 明确哪些 feature 已经进入默认能力
- 明确哪些 feature 仍然属于实验能力
- 明确不同版本镜像之间的兼容关系
- 明确：
  - `mkfs` 创建的镜像是否允许旧版本 mount
  - 新版本 `cryextsck` 是否向后兼容旧镜像
  - 哪些字段/feature 位以后不能随便改

### 目标

把当前“代码里已经存在”的格式能力，升级成：

```text
文档里明确承诺的格式边界
```

## 4.2 开源仓库基础材料要补齐

如果目标是 GitHub 开源，`Version 8` 必须把最小公开资料补齐。

### 要求

- `README.md`
  - 项目定位
  - 当前能力
  - 风险声明
  - 快速开始
- `LICENSE`
- `CONTRIBUTING.md`
- `SECURITY.md`
- `ROADMAP.md`
- `doc/` 中提供：
  - 架构总览
  - 版本演进总览
  - 格式能力说明
  - demo 使用说明
  - 故障排查说明

### 重点

必须明确写清楚：

```text
CRYEXTS 当前是研究/实验/教学/演示型文件系统，
还是已经建议用于专用测试设备，
还是已经建议承载重要数据
```

这个边界必须说死，不能模糊。

## 4.3 测试矩阵要从“单脚本”升级到“验收矩阵”

现在已经有很多 smoke，但 `Version 8` 要把它们组织成可交付的测试矩阵。

### 要求

至少整理出这几类测试入口：

- 基础格式测试
  - `mkfs`
  - `mount`
  - `umount`
  - `cryextsck`
- 结构能力测试
  - extent tree
  - dir index
  - xattr
  - multi-GDT
- 恢复能力测试
  - journal replay
  - orphan cleanup
  - fsck detect / repair
- 部署路径测试
  - image mode
  - raw-device mode
  - encrypted demo

### 目标

最终形成：

```text
一个“版本验收表”
而不是一堆分散脚本
```

## 4.4 demo 路径要产品化

`v7.3` 已经把 USB / raw-device demo 路径接回来了。
`Version 8` 要把它进一步产品化。

### 要求

- 提供统一 demo 入口
- 提供 image 模式推荐路径
- 提供 raw-device 模式风险提示
- 提供加密演示路径
- 提供失败后的排障步骤
- 提供推荐的 U 盘/loop/image 使用方式

### 目标

让别人第一次看到项目时，可以做到：

```text
拿到仓库
-> 按文档构建
-> 跑通 image demo
-> 在专用测试分区上跑通 raw demo
```

## 4.5 repair / recovery 口径要清晰

当前已经有 journal、fsck、repair 能力，但对外口径还需要收紧。

### 要求

- 明确：
  - 哪些错误 mount-time replay 负责
  - 哪些错误 `cryextsck` 负责
  - 哪些错误只检测不修复
- 明确 `--repair` 的使用边界
- 提供最小恢复手册
  - “异常断电后先做什么”
  - “出现 Structure needs cleaning 后先做什么”
  - “什么时候应该先镜像备份再 repair”

### 目标

把当前恢复能力从“源码作者知道怎么用”，
变成：

```text
外部评估者也能按文档执行
```

## 4.6 观测与排障能力要补齐

`Version 8` 不一定要上复杂监控，
但至少要把最常用的定位手段补完整。

### 要求

- 整理 inspect 工具用途
  - `cryexts_extent_inspect`
  - `cryexts_dir_index_inspect`
  - `cryexts_policy_inspect`
  - `cryexts_journal_inspect`
  - `cryexts_alloc_inspect`
  - `cryexts_xattr_inspect`
  - `cryexts_gdt_inspect`
- 提供“什么问题看什么工具”的对照表
- 提供最小 `dmesg + inspect + fsck` 排障流程

### 目标

让排障从：

```text
靠作者记忆
```

变成：

```text
靠文档化流程
```

## 4.7 商用评估边界要明确

你后面的长期目标是“最好可以商用”，
所以 `Version 8` 要开始把“可商用评估”和“可直接商用”这两个概念拆开。

### 要求

明确区分：

- `open-source-ready`
- `demo-ready`
- `commercial-evaluation-ready`
- `production-ready`

### 当前建议口径

`Version 8` 的现实目标应该是：

```text
commercial-evaluation-ready
```

而不是：

```text
production-commercial-ready
```

### 评估包至少应包含

- 许可证说明
- 第三方依赖说明
- 已知限制清单
- 支持范围说明
- 风险声明
- 测试覆盖说明
- 恢复能力说明

## 4.8 性能与稳定性需要有最小基线

`Version 8` 不要求你现在就把 benchmark 做成完整论文级别，
但至少需要有最小的稳定性/性能展示。

### 要求

- 长时间 mount/write/remove 的基本压力测试
- 大文件读写测试
- 多目录项测试
- 多次 remount / fsck 循环测试
- 至少一份简短 benchmark / observation 文档

### 目标

不是追求“性能最好”，而是提供：

```text
可展示、可比较、可复现的最小稳定性与性能基线
```

## 5. Version 8 非目标

为了防止继续发散，`Version 8` 建议明确不优先做：

- snapshot
- reflink
- dedupe
- quota
- online resize
- mmap 语义深挖
- 高级 ACL
- 更复杂的加密策略体系
- 新一轮大规模 on-disk 结构改版

原因很简单：

```text
Version 8 的价值不在于再发明更多 feature，
而在于把现有 feature 收成可解释、可交付、可维护的版本。
```

## 6. 建议的 Version 8 版本拆分

## 6.1 `v8.0`

主题：

- 仓库公开化基线

交付：

- `README`
- `LICENSE`
- `CONTRIBUTING`
- `SECURITY`
- `ROADMAP`
- 项目定位与风险声明

## 6.2 `v8.1`

主题：

- 格式边界与兼容性声明

交付：

- 格式主线文档
- feature / incompat / compat 边界整理
- 版本兼容策略说明

## 6.3 `v8.2`

主题：

- 测试矩阵与恢复矩阵

交付：

- smoke 统一入口
- 测试矩阵文档
- repair / recovery 手册

## 6.4 `v8.3`

主题：

- demo 产品化

交付：

- image demo 标准流程
- raw-device demo 标准流程
- 加密 demo 标准流程
- 故障排查说明

## 6.5 `v8.4`

主题：

- 商用评估资料包

交付：

- known limitations
- support boundary
- evaluation checklist
- baseline stability/perf note

## 7. Version 8 MVP 定义

我建议把 `Version 8 MVP` 定义成：

```text
CRYEXTS 不只是“作者自己能跑通”，
而是已经具备：

1. 别人可以按文档构建与测试
2. 别人可以按文档跑 image / raw-device demo
3. 别人可以按文档理解格式边界与风险边界
4. 别人可以按文档做最基本的故障定位与恢复尝试
```

也就是说，`Version 8 MVP` 的关键不是新增结构，
而是项目工程成熟度第一次成型。

## 8. 一句话总结

如果说：

- `Version 6` 解决的是“核心语义做出来”
- `Version 7` 解决的是“多块 GDT 和真实设备演示路径打通”

那么：

```text
Version 8 要解决的是：
把 CRYEXTS 从“作者驱动的实验项目”
推进到“别人可以理解、复现、评估、继续维护的开源工程项目”
```
