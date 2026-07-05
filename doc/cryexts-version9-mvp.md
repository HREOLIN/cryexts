# CRYEXTS Version 9 MVP 总结

## 1. 一句话结论

截至 `v9.5`：

```text
Version 9 的工程化 MVP 已完成
```

也就是说，`Version 9` 这一轮的目标，
已经从：

```text
一组分散的工程化诉求
```

推进到了：

```text
有部署基线
+ 有兼容治理
+ 有 soak 主线
+ 有恢复手册
+ 有健康检查入口
+ 有发布门槛
```

但同时也要把边界说清楚：

```text
工程化 MVP 完成
不等于已经达到长期生产部署或直接商用发布
```

`v9.5` 更准确的定位是：

```text
Version 9 工程主线闭环完成
```

## 2. Version 9 到底完成了什么

`Version 9` 这一轮的主线不是新增 on-disk 结构，
而是把 CRYEXTS 从：

```text
能跑、能演示、能评估
```

推进到：

```text
能按固定规则部署
+ 能按固定规则升级/回退
+ 能按固定规则长稳验证
+ 能按固定规则恢复与排障
+ 能按固定规则判定是否可发布
```

从版本角度看：

### 2.1 `v9.0`

主题：

- 部署基线冻结

完成内容：

- 固定 image / encrypted / raw-device 的推荐顺序
- 固定推荐验证环境和支持边界
- 固定最小 mount 参数口径
- 固定最小 release checklist 骨架

这版的意义：

```text
先把“怎么部署”这件事固定下来
```

### 2.2 `v9.1`

主题：

- 兼容治理与生命周期规则

完成内容：

- 分清 project version 和 format version
- 固定当前推荐主线格式为 `V6`
- 明确“什么改动不 bump / 什么改动必须考虑 bump”
- 固定最小升级 / 回退规则

这版的意义：

```text
先把“怎么治理版本和兼容”写成规则
```

### 2.3 `v9.2`

主题：

- soak 与长稳测试

完成内容：

- 固定 mount / umount loop
- 固定写删循环
- 固定 replay / fsck 循环
- 固定 raw-device 循环
- 固定 quick soak / full soak / raw soak 三类 profile

这版的意义：

```text
把“单次 smoke”推进成“可重复执行的稳定性主线”
```

### 2.4 `v9.3`

主题：

- 恢复与 repair 手册

完成内容：

- 分清 replay / orphan cleanup / fsck / `--repair` 的角色
- 固定 `Structure needs cleaning` 的处理顺序
- 固定 replay pending / orphan pending / free count mismatch / checksum mismatch 的分流
- 固定“先停写、先留证、后 repair”的基本原则

这版的意义：

```text
把“坏了以后怎么处理”从经验判断推进成固定手册
```

### 2.5 `v9.4`

主题：

- 健康检查与可观测性总览

完成内容：

- 把 inspect 工具分成整盘级和 inode 级两类
- 固定 `dmesg -> cryextsck -> 整盘级 inspect -> inode 级 inspect` 的顺序
- 固定问题类型到工具的最小映射

这版的意义：

```text
把“先看哪个工具”这件事固定下来
```

### 2.6 `v9.5`

主题：

- 发布门槛与最小性能基线

完成内容：

- 固定构建、smoke、soak、恢复、部署、文档这几类发布门槛
- 固定“可以发布 / 只能内部验证 / 不应发布”的三档判定
- 固定最小性能观察口径
- 把 `Version 9 MVP` 最终收成一条完整发布主线

这版的意义：

```text
把前面所有工程化成果收成“这一版能不能发”的最终口径
```

## 3. 所以 Version 9 MVP 的定义是什么

现在可以把 `Version 9 MVP` 明确定义成：

```text
部署流程固定
+ 兼容治理固定
+ soak 主线固定
+ 恢复 / repair 分流固定
+ 健康检查顺序固定
+ 发布门槛固定
```

换句话说，`Version 9` 的重点不是“多了几个新功能点”，
而是：

```text
规则更完整
+ 过程更可重复
+ 工程边界更清晰
```

## 4. 目前已经具备的能力

如果现在问：

```text
CRYEXTS 到 v9.5 已经具备了什么？
```

可以比较稳地回答：

- 已经有固定的 image / encrypted / raw-device 部署主线
- 已经有固定的兼容治理与升级/回退规则
- 已经有固定的 soak 与长稳验证主线
- 已经有固定的 recovery / repair 分流手册
- 已经有固定的健康检查与 inspect 使用顺序
- 已经有固定的发布门槛与最小性能观察口径
- 前面的 `Version 6 / Version 7 / Version 8` 成果已经能被收进一条工程主线

也就是说：

```text
现在的 CRYEXTS 不再只是“作者知道怎么推进的项目”，
而是已经开始像一条可维护、可复现、可交付的工程版本线
```

## 5. 当前 smoke / soak / recovery 维度如何理解

`Version 9` 的价值，不在于又多写了几个脚本，
而在于把已有验证面重新组织清楚了。

### 5.1 部署维度

例如：

- `smoke_version7_demo.sh`
- `smoke_v7_3_usb_demo.sh`

这类验证现在被放进：

```text
固定部署主线
```

里理解，而不是零散 demo。

### 5.2 稳定性维度

例如：

- mount / umount loop
- 写删循环
- replay / fsck 循环
- raw-device 循环

这类验证现在被放进：

```text
soak 主线
```

里理解，而不是单次通过。

### 5.3 恢复维度

例如：

- replay pending
- orphan cleanup pending
- `cryextsck`
- `cryextsck --repair`

这类验证现在被放进：

```text
固定恢复分流规则
```

里理解，而不是靠作者临场判断。

### 5.4 观测维度

例如：

- `cryexts_gdt_inspect`
- `cryexts_journal_inspect`
- `cryexts_policy_inspect`
- `cryexts_extent_inspect`
- `cryexts_dir_index_inspect`
- `cryexts_alloc_inspect`
- `cryexts_xattr_inspect`

这类工具现在被放进：

```text
固定健康检查顺序
```

里理解，而不是随机挑一个看。

### 5.5 发布维度

例如：

- 构建结果
- 关键 smoke
- 关键 soak
- 恢复路径
- 关键观测
- 最小性能记录

这类结果现在被放进：

```text
固定发布门槛
```

里理解，而不是“差不多就发”。

## 6. 当前边界也要说清楚

`Version 9 MVP` 虽然完成了，
但它仍然是工程化 MVP，不是 production filesystem。

### 6.1 部署方面

虽然已经有 raw-device 主线，
但仍然主要适合：

- 专用测试介质
- PoC
- 教学 / 研究 / 演示

还不适合：

- 重要生产数据
- 无备份环境
- 长期正式业务系统

### 6.2 恢复方面

虽然已经有 replay / orphan cleanup / `cryextsck` / `--repair`，
但仍然坚持：

```text
低风险 repair 原则
```

也就是说，当前没有承诺：

- 万能修复
- 激进目录树重建
- 内容级数据恢复

### 6.3 性能方面

虽然 `v9.5` 已经固定了最小性能观察口径，
但还没有做到：

- 系统化 benchmark 平台
- 多设备矩阵
- 长周期性能回归系统

### 6.4 发布方面

虽然已经有发布门槛，
但当前更准确的定位仍然是：

```text
工程化闭环完成
不等于生产级承诺完成
```

## 7. 面向 U 盘 demo，这意味着什么

如果目标是：

```text
把 CRYEXTS 部署到专用 U 盘或专用测试分区上做稳定 demo
```

那么 `Version 9 MVP` 已经足够作为：

```text
工程化 demo 基线
```

因为它已经不只是“功能有了”，
而是同时具备：

- 部署顺序
- 风险边界
- 长稳验证思路
- 恢复手册
- 健康检查入口
- 发布判定规则

也就是说：

```text
它已经足够像一个“可以反复演示、反复验证、反复解释”的文件系统工程原型
```

但如果目标是：

```text
长期真实数据承载
```

那后面还应该再做一轮更强的生产化补齐。

## 8. 面向 GitHub 开源，这意味着什么

如果目标是：

```text
把 CRYEXTS 作为一个持续演进的开源文件系统项目维护
```

那么 `Version 9 MVP` 的价值非常大，因为它已经形成了：

- 一条明确的部署主线
- 一条明确的兼容治理主线
- 一条明确的稳定性验证主线
- 一条明确的恢复与排障主线
- 一条明确的发布主线

别人看到的不再只是：

```text
这里有一堆代码和脚本
```

而是：

```text
这个文件系统到了什么阶段
应该怎么部署
应该怎么验证
出了问题怎么处理
这一版凭什么算完成
```

这会显著提升项目的可读性、可信度和可维护性。

## 9. 面向“商用评估”，这意味着什么

如果你说“后面最好能商用”，
那我建议把 `Version 9` 看作：

```text
商用前工程治理成熟点
```

而不是：

```text
立即可商用版本
```

因为商用前通常还要继续补：

- 更长时间的 soak
- 更强的故障注入
- 更完整的 benchmark
- 更正式的支持矩阵
- 更强的安全审计
- 更稳的升级迁移策略

所以 `Version 9 MVP` 的现实定位更像：

```text
可稳定演示
+ 可系统评估
+ 可继续往更正式版本推进
```

## 10. 建议的下一步

如果按最稳的方式往前推，我建议：

1. 先补一份 `Version 9 MVP` 统一验收总入口说明或总脚本。

例如可以考虑后续形成：

```text
scripts/smoke_version9_mvp.sh
```

用来串：

- 部署验证
- 关键 smoke
- 关键 soak
- 关键恢复
- 关键观测

2. 再开始规划 `Version 10`。

更建议围绕三类事情推进：

- 更强的发布自动化
- 更强的稳定性 / benchmark 证据
- 更接近真实部署场景的工程补齐

## 11. 最终总结

现在可以比较稳地说：

```text
CRYEXTS 到 v9.5，Version 9 MVP 已完成
```

它的意义不是“已经变成完整商用文件系统”，而是：

```text
这个文件系统已经跨过了“能跑、能演示、能评估”的阶段，
进入了“有部署规则、有兼容规则、有稳定性规则、有恢复规则、有发布规则”的阶段。
```

这就是 `Version 9 MVP` 最重要的价值。
