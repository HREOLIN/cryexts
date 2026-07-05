# CRYEXTS v9.0 部署基线说明

## 1. v9.0 的定位

`Version 8` 解决的是：

```text
让别人能看懂、跑通、评估 CRYEXTS
```

而 `v9.0` 要解决的是下一件更现实的事：

```text
让 CRYEXTS 的部署路径固定下来
```

一句话定义：

```text
v9.0 = CRYEXTS 的部署基线冻结版本
```

它不追求新增新的 on-disk feature，
而是先把下面四件事收口：

- 标准部署流程
- 内核 / 设备 / 模式支持边界
- 推荐 mount 参数
- 最小 release checklist

## 2. 为什么现在先做 v9.0

到当前为止，CRYEXTS 已经有：

- `Version 6` 的核心结构与事务主线
- `Version 7` 的 multi-GDT 与真实设备 demo 路径
- `Version 8` 的开源、评估、测试、demo 文档主线

现在最缺的不是：

```text
再加一个新 feature
```

而是：

```text
把“怎么部署、怎么验证、怎么发布”固定成一套标准动作
```

否则后面继续做 `v9.1`、`v9.2` 时，
很容易出现两个问题：

1. 每次演示环境都不一样
2. 每次验收标准都不一样

所以 `v9.0` 的目标很克制：

```text
先冻结部署基线
再在这个基线上继续演进
```

## 3. v9.0 不做什么

为了防止继续发散，`v9.0` 明确不做：

- 新的格式版本
- 新的 journal 结构
- 新的 extent / directory 结构
- 大规模脚本重写
- 自动化安装器
- 大范围设备兼容扩展

原因很简单：

```text
现在最短路径不是“再造新东西”，
而是把现有路径固定成一条可重复的部署主线
```

## 4. v9.0 支持边界

## 4.1 推荐内核与主验证环境

当前推荐基线环境：

- Linux 内核：`Ubuntu 5.15.0-139-generic`
- 运行方式：out-of-tree kernel module
- 工具链：仓库内 `make` + `mkfs.cryexts` + `cryextsck`

这不是说别的环境一定不行，
而是说：

```text
当前真正有连续验证证据的主线环境，
应先以 Ubuntu 5.15 这条线为准
```

## 4.2 推荐部署模式

`v9.0` 把部署模式明确分成三类。

### A. image 模式

模型：

```text
宿主文件系统中的 image 文件
-> loop / file-backed demo
-> mount cryexts
```

适合：

- 第一次验证
- 最低风险 smoke
- 教学 / 文档演示
- 结构回归测试

推荐等级：

```text
最高
```

### B. raw-device 模式

模型：

```text
专用测试分区
-> mkfs.cryexts
-> 直接 mount
```

适合：

- 专用测试 U 盘 / 分区
- 真实块设备路径验证
- USB demo / multi-GDT 演示

风险边界：

- 只能用于专用测试分区
- 不应用于有重要数据的设备
- 必须显式确认目标设备节点

推荐等级：

```text
次高
```

### C. encrypted demo 模式

模型：

```text
image 或 raw-device
+ KEY
-> 验证当前加密路径
```

适合：

- 展示 policy-aware encryption 路径
- 验证明文/密文可观测差异

边界：

- 这是演示与评估路径
- 不等于已经完成生产级安全审计

## 4.3 当前不建议的使用场景

`v9.0` 明确不建议：

- 重要生产数据
- 无备份环境
- 长期正式业务系统
- 合规要求严格的安全场景
- 未经隔离的真实工作 U 盘

建议对外统一表述为：

```text
CRYEXTS 当前适合研究、教学、PoC、专用测试设备和可控 demo 场景，
不建议承载重要生产数据。
```

## 5. 标准部署流程

这一节的目标，是把现在分散在脚本和说明里的流程，
收成一条固定主线。

## 5.1 标准顺序

推荐顺序固定为：

1. 构建模块和工具
2. 跑基础 smoke / fsck
3. 先跑 image 模式
4. 再跑 encrypted image 模式
5. 最后跑 raw-device 模式

也就是：

```text
低风险
-> 中风险
-> 高风险
```

而不是一上来就直接改真实分区。

## 5.2 标准构建步骤

推荐命令：

```bash
make
```

预期产物至少包括：

- `cryexts.ko`
- `mkfs.cryexts`
- `cryextsck`
- 各类 inspect 工具

如果 `make` 失败，`v9.0` 的口径是：

```text
先不要继续 mount / demo，
必须先把构建问题解决
```

## 5.3 标准 image 验证路径

推荐先跑：

- [scripts/smoke_version7_demo.sh](/D:/Carl/cryptext4/cryexts/scripts/smoke_version7_demo.sh:1)
- [scripts/smoke_v7_3_usb_demo.sh](/D:/Carl/cryptext4/cryexts/scripts/smoke_v7_3_usb_demo.sh:1)

如果是最安全路径，优先使用：

```bash
./scripts/smoke_v7_3_usb_demo.sh
```

它的意义不是只证明“脚本绿了”，
而是证明下面这条链路成立：

```text
mkfs
-> fsck
-> mount
-> 文件/目录/大文件/稀疏文件/索引路径
-> umount
-> remount
-> fsck clean
```

## 5.4 标准 encrypted demo 路径

推荐命令：

```bash
KEY=demo-key ./scripts/smoke_v7_3_usb_demo.sh
```

pass 的含义应理解为：

- 当前加密挂载路径成立
- 数据可正确写入和读回
- 演示明文不直接裸露在原始镜像中

但要明确：

```text
这只证明“演示加密路径成立”，
不代表已经完成生产级加密安全承诺
```

## 5.5 标准 raw-device 路径

推荐命令：

```bash
DEMO_MODE=raw \
TARGET_DEVICE=/dev/sdX1 \
ACK_RAW_DEVICE=I_UNDERSTAND_THE_RISK \
./scripts/smoke_v7_3_usb_demo.sh
```

执行前必须满足：

1. 目标分区是专用测试分区
2. 设备节点已经确认无误
3. 已经先跑通 image 模式

`v9.0` 对 raw-device 的口径是：

```text
raw-device 是高风险验证路径，
不是第一次接触 CRYEXTS 时的默认入口
```

## 6. 推荐 mount 参数

`v9.0` 不引入复杂参数矩阵，
只先固定“推荐基线”。

## 6.1 推荐基线参数

推荐先用最小参数：

```bash
mount -t cryexts <device-or-image> <mountpoint>
```

如果验证加密路径，再显式补：

```bash
mount -t cryexts -o key=<demo-key> <device-or-image> <mountpoint>
```

`v9.0` 的原则是：

```text
默认参数优先
+ 可读、可复现优先
+ 不先引入复杂 mount 组合
```

## 6.2 当前推荐口径

### 非加密场景

推荐：

- 不额外加自定义 mount 参数
- 先验证默认挂载主线

### 加密场景

推荐：

- 只增加 `key=...`
- 不同时叠很多演示无关参数

### raw-device 场景

推荐：

- 仍然保持最小 mount 组合
- 风险控制应靠设备确认和流程控制
- 不靠复杂参数“补救”

## 6.3 为什么现在先收紧参数组合

因为当前最重要的不是：

```text
把 mount 选项做得花哨
```

而是：

```text
把默认主线验证稳定
```

这会直接降低三类成本：

- 文档成本
- 排障成本
- 回归成本

## 7. 最小发布清单

`v9.0` 开始，不建议再用“感觉差不多就发”的方式推进版本。

至少要有一张最小 release checklist。

## 7.1 构建检查

发布前至少确认：

- `make` 成功
- `cryexts.ko` 可生成
- `mkfs.cryexts` 可生成
- `cryextsck` 可生成
- inspect 工具可生成

## 7.2 基础功能检查

至少确认：

- image 模式 smoke 通过
- remount 路径通过
- `cryextsck` clean
- 基础目录 / 文件 / rename / link 路径通过

## 7.3 结构能力检查

至少确认：

- extent tree 路径可验证
- dir index 路径可验证
- xattr 路径可验证
- multi-GDT 路径可验证

## 7.4 恢复能力检查

至少确认：

- journal replay 路径可验证
- orphan / recovery 相关路径可验证
- `cryextsck` 能给出一致结果

## 7.5 demo 检查

至少确认：

- image demo 通过
- encrypted demo 通过
- raw-device demo 在专用测试介质上通过

## 7.6 文档检查

至少确认：

- 当前版本文档已补
- 风险边界没有写模糊
- 支持边界没有写过头
- 推荐运行顺序已写清楚

## 8. v9.0 推荐验收标准

我建议把 `v9.0` 完成定义成下面四件事：

### 8.1 部署入口固定

别人不需要口头问作者，也知道：

- 先 image
- 再 encrypted
- 最后 raw-device

### 8.2 支持边界固定

别人能明确知道：

- 当前推荐环境是什么
- 当前推荐模式是什么
- 当前不建议拿去做什么

### 8.3 mount 口径固定

别人不会一上来就拼一堆额外参数，
而是先按默认主线验证。

### 8.4 发布门槛固定

后续每个小版本继续演进前，
至少有一张可重复执行的最小发布清单。

## 9. v9.0 和后续版本的关系

`v9.0` 不是终点，
而是 `Version 9` 这条线的起点。

它给后面版本提供的是：

### 给 `v9.1`

- 兼容治理基线

### 给 `v9.2`

- soak test 的统一部署入口

### 给 `v9.3`

- recovery / repair 的标准操作上下文

### 给 `v9.4`

- 健康检查的固定对象和固定顺序

### 给 `v9.5`

- release checklist 和性能基线的落点

也就是说：

```text
v9.0 先把“怎么部署”这件事冻结，
后面的版本再在这条冻结主线上继续加稳定性、恢复和治理能力
```

## 10. 一句话总结

如果说：

- `Version 8` 解决的是“别人能看懂、能跑、能评估”

那么：

```text
v9.0 解决的是：
把 CRYEXTS 的部署方式、支持边界、mount 口径和发布门槛先固定下来，
让后续版本不再建立在临时流程之上
```
