# CRYEXTS Version 9 需求设计

## 1. Version 9 为什么要单独设计

到 `v8.4` 为止，CRYEXTS 已经把下面这条线基本收起来了：

- `Version 6` 把核心文件系统语义做出来
- `Version 7` 把 multi-GDT、真实设备 demo 主线打通
- `Version 8` 把开源、评估、测试矩阵、demo 产品化资料补齐

也就是说，项目已经不再停留在：

```text
作者自己能做出来
```

而是已经进入下一阶段：

```text
别人能理解
+ 别人能复现
+ 别人能评估
```

但这还不等于：

```text
别人能长期反复使用
+ 别人能升级
+ 别人能恢复
+ 别人能维护
```

所以 `Version 9` 要解决的问题，不再是“再堆几个 feature”，
而是把 CRYEXTS 从：

```text
evaluation-ready
```

推进到：

```text
deployable-demo-ready
+ lifecycle-ready
+ long-run-stability-ready
```

一句话定义：

```text
Version 9 = CRYEXTS 的部署硬化与生命周期工程化版本
```

## 2. 当前已经具备的基础

进入 `Version 9` 前，默认已经具备：

- block groups / multi-GDT
- journal v2 / mount-time replay
- metadata checksum
- orphan cleanup
- extent tree / sparse file
- scalable directory index
- policy-aware encryption
- large xattr
- raw-device / USB demo
- 开源基础文档
- 测试矩阵与评估资料

所以 `Version 9` 的关注点不应该是：

```text
这个文件系统能不能工作
```

而应该是：

```text
它能不能被稳定部署
它能不能被长期重复演示
它出问题时能不能按固定流程定位和恢复
它升级时会不会把历史镜像搞坏
```

## 3. Version 9 主目标

`Version 9` 的主目标建议定义为：

```text
冻结一条可长期维护的部署主线
+ 建立一条可回归的稳定性主线
+ 建立一套可升级、可恢复、可观测的生命周期主线
```

更具体地说，`Version 9` 要回答下面这些现实问题：

### 3.1 部署问题

- 哪些环境可以稳定挂载
- 哪些内核版本是当前推荐范围
- 驱动如何安装、加载、卸载
- 镜像模式和真实设备模式分别怎么用

### 3.2 生命周期问题

- 旧镜像如何继续使用
- 新代码是否兼容旧格式
- 什么时候允许升级格式
- 升级失败后如何退回

### 3.3 稳定性问题

- 反复 mount/umount 会不会出问题
- 长时间写删混合负载会不会积累损坏
- 断电/异常拔盘之后恢复边界是否清楚

### 3.4 运维问题

- 出问题先看哪里
- 哪些问题可 replay
- 哪些问题必须 fsck
- 哪些问题 repair 前必须先备份

## 4. Version 9 核心需求

## 4.1 部署基线要固定下来

`Version 8` 解决了“别人可以照文档跑起来”，
`Version 9` 要解决“别人重复跑很多次，流程仍然一致”。

### 要求

- 明确推荐部署方式：
  - `image`
  - `loop`
  - `raw-device`
- 明确推荐内核版本范围
- 明确模块构建 / 插入 / 卸载标准流程
- 明确测试设备使用边界
- 明确 mount 参数的推荐组合

### 目标

形成：

```text
一个稳定的部署基线
```

而不是：

```text
每次演示都临时凑环境
```

## 4.2 生命周期与兼容策略要升级成正式规则

`Version 8` 已经把兼容边界写出来，
`Version 9` 要把“边界说明”升级成“操作规则”。

### 要求

- 明确 project version 与 on-disk format version 的关系
- 明确当前默认主线格式
- 明确哪些改动允许不改格式继续前进
- 明确哪些需求一旦做，就必须 bump format
- 明确旧镜像的 mount / fsck / inspect 支持策略
- 提供最小升级流程
- 提供最小回退流程

### 重点

`Version 9` 默认应遵循：

```text
能不改 on-disk format，就不改
```

只有在现有结构确实无法承载目标能力时，
才考虑新的格式版本。

### 目标

把兼容性从：

```text
文档承诺
```

推进成：

```text
版本治理规则
```

## 4.3 稳定性验证要从 smoke 升级到 soak

前面的 smoke 更多是“功能能不能过”，
`Version 9` 要开始回答“跑久了会不会坏”。

### 要求

至少要有下面几类长稳测试：

- mount / umount 循环
- create / write / rename / delete 循环
- 大文件持续追加 / truncate 循环
- 多目录、多小文件压力
- raw-device 反复插拔/重挂载流程
- journal replay 重复恢复流程
- fsck 循环校验流程

### 目标

形成：

```text
最小 soak test 主线
```

而不是只证明：

```text
这次恰好跑通了
```

## 4.4 恢复与 repair 要标准化

如果目标是后续可部署 demo，
那么“坏了怎么办”必须比现在更标准。

### 要求

- 明确 replay 负责的边界
- 明确 fsck 负责的边界
- 明确 repair 负责的边界
- 明确哪些场景只允许只读挂载/导出数据
- 提供异常断电后的固定处理流程
- 提供 `Structure needs cleaning` 的标准排障流程
- 提供 repair 前备份建议

### 目标

让恢复流程从：

```text
靠作者经验判断
```

变成：

```text
按手册一步一步执行
```

## 4.5 观测与健康检查要补一层汇总能力

inspect 工具已经不少了，
`Version 9` 没必要上复杂监控系统，
但至少要补一层“健康摘要”能力。

### 要求

- 整理现有 inspect 工具的职责边界
- 提供统一的检查顺序
- 给出“健康检查入口”
  - superblock
  - GDT
  - journal
  - extent
  - xattr
  - dir index
- 提供异常时的最小采集信息清单
  - `dmesg`
  - `cryextsck`
  - 对应 inspect 输出

### 目标

形成一套：

```text
先检查什么
+ 再检查什么
+ 最后怎么定位
```

的固定顺序。

## 4.6 发布与回归门槛要固定

如果以后继续往 `v10+` 走，
那从 `Version 9` 开始就不能再靠“感觉差不多”发布。

### 要求

- 定义版本发布前的最小验收门槛
- 定义必须通过的 smoke 集
- 定义必须通过的 soak 集
- 定义 raw-device 验收要求
- 定义文档同步要求
- 定义已知问题登记要求

### 目标

形成：

```text
可重复的 release checklist
```

## 4.7 性能基线要从“能展示”升级到“能对比”

`Version 8` 的性能目标是“先有最小基线”，
`Version 9` 则应该至少做到“同一版本之间可比较”。

### 要求

- 固定一套最小 benchmark 场景
- 固定测试介质与参数口径
- 记录：
  - 顺序写
  - 顺序读
  - 小文件创建
  - 目录扫描
  - remount 后一致性检查耗时
- 记录碎片化观察
- 记录加密与非加密的差异

### 目标

不是追求极限性能，
而是建立：

```text
可重复、可比较、可回归的性能观察基线
```

## 4.8 安全与商用前置边界要再收紧

如果后续真要面向 NAS / 存储 / 商业评估方向继续推进，
`Version 9` 需要把“能不能碰重要数据”的边界说得更死。

### 要求

- 明确当前不建议承载的场景
- 明确加密策略的边界
- 明确未覆盖的安全能力
- 明确当前不承诺的并发/崩溃场景
- 明确支持边界与免责边界

### 目标

把项目定位从：

```text
看起来好像快可商用了
```

收紧成：

```text
哪些能用
+ 哪些不能用
+ 哪些只是评估用途
```

## 5. Version 9 非目标

为了防止路线再次发散，`Version 9` 建议明确不优先做：

- snapshot
- reflink
- dedupe
- quota
- online resize
- 多设备 RAID
- 网络分布式能力
- 高级权限模型
- 大规模格式重写

原因很简单：

```text
Version 9 的价值不在于再横向堆功能，
而在于把现有能力沉淀成可部署、可维护、可回归的主线。
```

## 6. 建议的 Version 9 版本拆分

## 6.1 `v9.0`

主题：

- 部署基线冻结

交付：

- 标准部署流程文档
- 内核/设备/模式支持边界
- mount 参数建议
- 基础 release checklist

## 6.2 `v9.1`

主题：

- 生命周期与兼容治理

交付：

- project version / format version 规则
- 升级 / 回退手册
- 兼容矩阵收紧

## 6.3 `v9.2`

主题：

- 稳定性 soak 测试

交付：

- mount/umount loop
- 写删循环
- raw-device 循环
- fsck / replay 循环

## 6.4 `v9.3`

主题：

- 恢复与 repair 标准化

交付：

- 恢复手册
- repair 使用边界
- 异常场景分流表

## 6.5 `v9.4`

主题：

- 健康检查与观测汇总

交付：

- inspect 使用总览
- 健康检查入口
- 故障采集清单

## 6.6 `v9.5`

主题：

- 发布门槛与性能基线

交付：

- soak + smoke 验收表
- 最小 benchmark 文档
- 稳定性结论与已知限制

## 7. Version 9 MVP 定义

我建议把 `Version 9 MVP` 定义成：

```text
CRYEXTS 不只是“能演示”，
而是已经具备：

1. 有固定部署流程
2. 有固定升级/回退规则
3. 有固定恢复/repair 流程
4. 有最小 soak 测试主线
5. 有可重复的发布与验收门槛
```

也就是说，`Version 9 MVP` 的关键不是新结构，
而是：

```text
把项目变成一条可以持续演进的工程主线
```

## 8. 一句话总结

如果说：

- `Version 6` 解决的是“核心语义做出来”
- `Version 7` 解决的是“真实设备和 multi-GDT 主线打通”
- `Version 8` 解决的是“开源、评估、demo、文档收口”

那么：

```text
Version 9 要解决的是：
把 CRYEXTS 从“可展示、可评估的实验文件系统项目”
推进到“可长期部署演示、可持续维护、可版本治理的工程主线”
```
