# CRYEXTS 通往 U 盘 Demo / 开源 / 商用的版本路线

## 1. 这份文档回答什么问题

你现在关心的不是“下一个技术点做什么”，而是更实际的问题：

1. 还要几个版本，才能把 `CRYEXTS` 放到 U 盘上稳定演示
2. 如果后面要放到 GitHub 开源，甚至考虑商用，还缺哪些东西

所以这份文档的目标是：

- 定义一个“小目标版本线”
- 说明每一步为什么必要
- 把“技术可跑通”和“可开源/可商用”分开看

## 2. 先说结论

如果你的目标是：

```text
在一块专门测试用的 U 盘上，
可以稳定地 mkfs / mount / 写文件 / 拔插后再挂载 / fsck / 演示加密
```

那我建议至少还要：

```text
Version 6
+ 一个 Version 7 的“发布与稳定化阶段”
```

更具体地说：

- `Version 6`：把事务语义、extent tree、sparse、allocator 再做完整
- `Version 7`：专门做“可部署 demo / 可开源 / 可维护”的稳定化与发布工程

所以不是“再 1 个小版本”就够，而是建议：

```text
v6 = 技术语义补完
v7 = 演示级/发布级收尾
```

## 3. 为什么 Version 5 还不够直接上 U 盘长期演示

`Version 5` 已经很强了，能做：

- orphan
- extents
- dir index
- policy-aware encryption
- metadata checksum
- locality hint

但它依然更像：

```text
功能型原型
```

而不是：

```text
可长期反复插拔演示的设备文件系统
```

主要还差这些：

### 3.1 transaction 语义还不够完整

现在 journal 已经能 replay，但还不是完整的 descriptor / commit / checkpoint 模型。

对于 U 盘场景，这很重要，因为 U 盘 demo 最容易遇到：

- 非正常断电
- 未卸载就拔盘
- 正在写时中断

如果 transaction 边界不够完整，演示可信度会打折。

### 3.2 extent tree 还不够大

现在 `v5.2` 是 extent overflow MVP，不是真正多级 tree。

如果你要在 U 盘上放：

- 大文件
- 稀疏镜像
- 反复复制/删除

这个上限迟早会碰到。

### 3.3 allocator 还偏 MVP

`v5.6` 已经有 locality hint，但还没有：

- inode locality
- delayed allocation
- reservation window

对于 U 盘这种相对慢、容易碎片化的介质，连续分配收益会更明显。

### 3.4 运维工具还不够完整

如果现场演示时出问题，你需要的不只是 `cryextsck` 能报错，而是：

- 能 inspect journal
- 能 inspect extent tree
- 能说明哪里坏了
- 能做低风险 repair

这类“解释能力”对 demo 和开源都非常重要。

## 4. 我建议的“小目标路线”

### 阶段 A：Version 6

目标：

```text
让文件系统从“结构可用”
升级到“语义完整”
```

建议包含：

- `V6.0 journal v2 layout`
- `V6.1 commit / replay / checkpoint MVP`
- `V6.2 multi-level extent tree`
- `V6.3 sparse file / hole`
- `V6.4 allocator 继续升级`

这一阶段完成后，你会得到：

- 更可信的 crash/recovery 行为
- 更像真实文件系统的大文件映射
- 更合理的 U 盘数据布局

### 阶段 B：Version 7

目标：

```text
专门为“真实 U 盘演示 + GitHub 开源 + 后续商用准备”做稳定化
```

这一阶段不是继续横向加功能，而是补：

- USB/raw-device 真实流程文档
- 发布脚本
- 回归脚本
- 长时间压力测试
- 故障注入测试
- LICENSE / CONTRIBUTING / SECURITY / SUPPORT 文档
- 兼容性声明和风险边界

## 5. 建议的 Version 7 定位

我建议你把 `Version 7` 直接定义成：

```text
demo-ready + open-source-ready + commercial-evaluation-ready
```

也就是说，`Version 7` 不一定先追求特别多新结构，而是要把这些现实问题补齐。

## 6. Version 7 需要做什么

### 6.1 Demo-Ready

这是“能拿 U 盘给别人演示”的最低要求。

建议补：

- raw partition 的标准化操作文档
- 真机 U 盘 smoke 脚本
- 安全卸载 / 非正常拔盘 / 恢复演示脚本
- 加密 U 盘演示脚本
- 性能采样脚本

你最终应该能做出这种演示：

```text
格式化 U 盘分区
-> 挂载 cryexts
-> 写文件 / 目录 / 大文件 / 加密文件
-> 模拟异常中断
-> 再挂载自动恢复
-> fsck clean 或低风险修复
```

### 6.2 Open-Source-Ready

如果要上 GitHub，这一块不能缺。

建议补：

- `LICENSE`
- `README` 的项目定位和风险声明
- `CONTRIBUTING.md`
- `SECURITY.md`
- `CODE_OF_CONDUCT.md`
- 清晰的版本路线图
- 支持哪些内核版本的说明
- 已知限制列表

尤其要明确写清楚：

```text
这是研究/教学/实验型文件系统，
还是已经建议用于测试设备，
还是建议用于重要数据
```

这个边界一定要说清楚。

### 6.3 Commercial-Evaluation-Ready

“适合以后商用”和“现在就能商用”完全不是一回事。

建议先做到：

- 代码许可证清晰
- 第三方依赖许可证清晰
- 加密方案来源和限制清晰
- 风险声明清晰
- 回归测试和故障注入覆盖足够
- 基础性能和稳定性报告可展示

如果以后真考虑商用，还会继续缺：

- 更完整安全审计
- 性能 benchmark
- 长期兼容维护策略
- 数据迁移与升级策略
- 灾难恢复手册

所以当前更现实的目标应是：

```text
commercial-evaluation-ready
```

不是：

```text
production-commercial-ready
```

## 7. 我建议的版本拆分

### Version 6

技术补完阶段：

- `v6.0` journal v2 layout
- `v6.1` 完整事务边界
- `v6.2` multi-level extent tree
- `v6.3` sparse file / hole
- `v6.4` allocator upgrade
- `v6.5` scalable dir index
- `v6.6` xattr / inspect / fsck 补强

### Version 7

发布与稳定化阶段：

- `v7.0` USB demo baseline
- `v7.1` long-run stress / fault injection
- `v7.2` open source packaging
- `v7.3` commercial evaluation package

## 8. U 盘 Demo 的建议验收标准

我建议你把“U 盘 demo 完成”定义成下面这些都能过：

### 8.1 基础能力

- U 盘分区可 `mkfs.cryexts`
- 可正常 `mount/umount`
- 可创建目录 / 小文件 / 大文件
- 可跑加密分区演示

### 8.2 恢复能力

- 正常卸载后可 clean remount
- 非正常断开后可 replay / orphan cleanup
- `cryextsck` 能 clean 或给出明确可修复结果

### 8.3 数据结构能力

- 大文件可超出 `v5.2` 的 extent overflow 限制
- 稀疏文件语义成立
- allocator 不会很快碎片化到不可演示

### 8.4 文档能力

- 有专门的 USB 使用说明
- 有风险声明
- 有已知限制
- 有问题定位步骤

## 9. 如果目标是 GitHub 开源，最少要补哪些文档

我建议至少新增这些：

- `doc/cryexts-version-roadmap-to-usb-demo.md`
- `doc/cryext_7_requirements.md`
- `README` 增加项目状态声明
- `LICENSE`
- `CONTRIBUTING.md`
- `SECURITY.md`
- `ROADMAP.md`

## 10. 一句话总结

如果你的目标是：

```text
把 cryexts 放到 U 盘上做可靠 demo，
并且未来走向 GitHub 开源，甚至具备商用评估价值
```

那我建议路线是：

```text
Version 6：先把技术语义补完
Version 7：再做 demo-ready / open-source-ready / commercial-evaluation-ready
```

也就是说：

```text
还差一个技术补完大版本
+ 一个发布稳定化大版本
```

这是最稳、也最符合你最终目标的路线。
