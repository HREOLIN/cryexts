# CRYEXTS v8.3 设计说明

## 1. v8.3 的定位

`v8.0` 解决的是：

```text
让仓库入口像一个公开项目
```

`v8.1` 解决的是：

```text
让格式边界和兼容边界说得清楚
```

`v8.2` 解决的是：

```text
让测试、恢复、排障矩阵组织清楚
```

而 `v8.3` 要解决的是下一件很现实的事：

```text
让 CRYEXTS 的 demo 路径不再只是“作者知道怎么跑的脚本”，
而是一个别人拿到仓库后可以理解、选择、执行、复现的产品化入口
```

一句话定义：

```text
v8.3 = CRYEXTS 的 demo 产品化基线版本
```

## 2. 为什么现在先做 v8.3

到 `v7.3` 为止，CRYEXTS 已经有了真实可跑的 demo 主线：

- image mode
- raw-device mode
- encrypted demo
- multi-GDT 场景

而且脚本层已经有一个很好的基础：

- [scripts/smoke_v7_0_usb_demo.sh](/D:/Carl/cryptext4/cryexts/scripts/smoke_v7_0_usb_demo.sh:1)
- [scripts/smoke_v7_3_usb_demo.sh](/D:/Carl/cryptext4/cryexts/scripts/smoke_v7_3_usb_demo.sh:1)

问题不是“没有 demo”，
而是“demo 还没有产品化”。

### 2.1 当前 demo 仍然偏作者视角

现在你我当然知道这些路径的意义：

- image mode 最安全
- raw-device 更接近真实设备
- `KEY=` 可以走加密路径
- `SIZE_MB=1024` 可以自然进入 multi-GDT

但外部读者第一次看到时，不一定知道：

- 该先跑哪个模式
- 什么情况下该切 raw-device
- 什么情况下要带 key
- 哪些参数只是调试参数，哪些是推荐 profile

### 2.2 当前脚本是共享实现，但对外入口还没完全“产品化”

现在的做法其实已经很对：

- `smoke_v7_0_usb_demo.sh` 是共享实现
- `smoke_v7_3_usb_demo.sh` 只是套一层默认参数

这正是一个很好的产品化起点。

`v8.3` 不应该推翻它，
而应该继续沿着这个思路走：

```text
共享实现不动大骨架
+ 对外入口分 profile
+ 文档把 profile 讲清楚
```

### 2.3 当前 demo 路径已经接近“功能上可用”，但还不够像“可交付流程”

真正面向外部时，别人更关心：

- 先跑哪个
- 一条命令怎么跑
- pass 代表什么
- 失败了先看哪一步
- 风险在哪里

所以 `v8.3` 的任务很明确：

```text
不是发明新 demo 功能，
而是把现有 demo 路径包装成标准流程
```

## 3. v8.3 主目标

`v8.3` 建议只做三件事：

1. 定义 demo profile
2. 定义统一 demo 入口
3. 定义 demo 风险与排障口径

拆开看，就是五个交付方向：

- 标准 demo 入口
- image demo profile
- raw-device demo profile
- encrypted demo profile
- demo 故障排查说明

## 4. v8.3 不做什么

为了防止继续发散，`v8.3` 明确不做：

- 新增新的 on-disk feature
- 新增新的部署模式
- 引入 GUI demo
- 重写共享 demo 脚本主逻辑
- 建立复杂交互式安装器
- 扩大到大量设备兼容矩阵

原因很简单：

```text
当前最短路径不是“再造新 demo 系统”，
而是把已有 demo 路径收成标准产品化入口
```

## 5. v8.3 要解决的核心问题

## 5.1 定义清楚“demo profile”

所谓 profile，可以简单理解成：

```text
一组推荐参数 + 一种目标场景
```

`v8.3` 建议把 demo 明确分成三类 profile。

### A 类：Image Demo Profile

#### 目标场景

- 第一次验证
- 风险最低
- 最适合公开展示和教学

#### 推荐特征

- 使用宿主文件系统中的 image 文件
- loop mount
- 默认优先跑 multi-GDT image

#### 推荐入口

例如：

```bash
USB_HOST_DIR=/media/$USER/USB ./scripts/smoke_v7_3_usb_demo.sh
```

#### 语义

pass 代表：

- 项目可在最安全的演示路径下完整运行
- `mkfs -> fsck -> mount -> write/read -> remount -> fsck` 成立

### B 类：Raw-Device Demo Profile

#### 目标场景

- 专用测试分区
- 更接近真实块设备部署

#### 推荐特征

- 明确要求 `TARGET_DEVICE`
- 明确要求 `ACK_RAW_DEVICE`
- 强制风险确认

#### 推荐入口

例如：

```bash
DEMO_MODE=raw \
TARGET_DEVICE=/dev/sdX1 \
ACK_RAW_DEVICE=I_UNDERSTAND_THE_RISK \
./scripts/smoke_v7_3_usb_demo.sh
```

#### 语义

pass 代表：

- CRYEXTS 不只是能在 loop image 上跑
- 也能在真实块设备分区路径上完成完整 demo 链路

### C 类：Encrypted Demo Profile

#### 目标场景

- 展示当前加密路径
- 展示 mount key / policy-aware 能力

#### 推荐特征

- 明确带 `KEY`
- 明确检查密文中不应直接出现明文

#### 推荐入口

例如：

```bash
KEY=demo-key USB_HOST_DIR=/media/$USER/USB ./scripts/smoke_v7_3_usb_demo.sh
```

#### 语义

pass 代表：

- 加密卷可正确创建和挂载
- 数据路径可解密读回
- 原始 image 中不直接暴露演示明文

## 5.2 统一 demo 入口

`v8.3` 不建议为每种 demo 再写一堆完全独立的新脚本，
而建议继续沿用当前已经很好的结构：

- 一个共享实现脚本
- 多个 profile 入口

也就是说，方向应该是：

```text
shared implementation
-> image profile wrapper
-> raw profile wrapper
-> encrypted profile wrapper
```

这样做的好处很直接：

- 逻辑只维护一份
- 默认参数按 profile 收口
- 对外入口更清楚

## 5.3 把“参数”升级成“产品语义”

现在脚本里参数已经很多：

- `DEMO_MODE`
- `USB_HOST_DIR`
- `TARGET_DEVICE`
- `ACK_RAW_DEVICE`
- `KEY`
- `SIZE_MB`
- `LABEL`
- `DIR_FILE_COUNT`

对作者来说这些是正常参数，
但对外应该进一步解释成：

### 哪些是基础参数

- `DEMO_MODE`
- `USB_HOST_DIR`
- `TARGET_DEVICE`
- `KEY`

### 哪些是风险边界参数

- `ACK_RAW_DEVICE`

### 哪些是 demo 强度参数

- `SIZE_MB`
- `DIR_FILE_COUNT`
- `LARGE_MB`
- `SPARSE_SIZE_MB`

也就是说，`v8.3` 需要把“脚本参数表”
升级成：

```text
带语义分类的产品参数说明
```

## 5.4 明确 demo 的标准执行顺序

`v8.3` 需要给出一个标准化的推荐顺序，
避免别人一上来就直接 raw-device。

建议顺序固定为：

1. 先跑 `Version 6/7` 基础 smoke
2. 再跑 image demo
3. image 稳定后再跑 encrypted image demo
4. 最后再跑 raw-device demo

这一顺序的意义是：

```text
先低风险验证
-> 再中风险验证
-> 最后再上真实块设备路径
```

## 5.5 明确 demo 的 pass 语义

`v8.3` 必须说明：

### image demo pass

说明：

- 宿主文件系统上的 image 部署路径成立
- demo 不是只停在格式化阶段
- remount 和 fsck 也成立

### raw-device demo pass

说明：

- 真实块设备路径成立
- 不是只在 loop 场景下工作

### encrypted demo pass

说明：

- 当前加密路径在 demo 维度成立
- 但不等于已经完成生产级安全审计

这个边界必须写清楚，不能模糊。

## 5.6 明确 demo 的失败处理流程

`v8.3` 应该把 demo 失败时的第一轮排障流程写死。

建议统一成：

1. 看脚本停在哪个 step
2. 看 `dmesg | tail -n 120`
3. 再看对应 inspect 工具
4. 再决定是否离线 `cryextsck`

例如：

### 失败在 `mkfs` / `fsck after mkfs`

优先看：

- `cryextsck`
- `cryexts_gdt_inspect`

### 失败在 `mount`

优先看：

- `dmesg`
- `cryextsck`
- `cryexts_journal_inspect`

### 失败在目录 / xattr / 文件操作

优先看：

- `dmesg`
- 对应 inspect 工具
- `cryextsck`

### 失败在最终 remount / fsck

优先看：

- `cryextsck`
- `cryexts_gdt_inspect`
- `cryexts_journal_inspect`

## 6. v8.3 建议产出的文档

`v8.3` 最终不应该只停在这份设计文档，
而应该至少沉淀出下面几类可交付材料。

## 6.1 Demo Guide

一份面向外部读者的主说明，内容至少包括：

- demo 有哪几类
- 推荐顺序
- 风险说明
- 快速命令
- pass 含义

## 6.2 Demo Profile 说明

一份更短的 profile 对照表，内容至少包括：

- image
- raw-device
- encrypted

每个 profile 的：

- 目标场景
- 推荐命令
- 风险等级
- 失败后先看哪里

## 6.3 Demo Troubleshooting

一份最小排障说明，内容至少包括：

- 常见失败点
- 对应工具
- 先做什么，不要先做什么

## 7. v8.3 对脚本体系的设计要求

`v8.3` 不要求大改脚本逻辑，
但会对脚本组织提出明确方向。

## 7.1 共享实现继续保留

当前这种结构是对的：

- `smoke_v7_0_usb_demo.sh` 负责主要实现
- `smoke_v7_3_usb_demo.sh` 负责 profile 包装

`v8.3` 的方向应是继续保留这种结构，
不要把逻辑重新复制到多个脚本里。

## 7.2 入口脚本要逐步按 profile 命名

后续建议方向：

- 保留共享实现脚本
- 对外入口尽量体现 profile 语义

例如以后更清晰的入口可能会是：

- `smoke_demo_image.sh`
- `smoke_demo_raw.sh`
- `smoke_demo_encrypted.sh`

但 `v8.3` 先不要求立刻重命名一切，
先把 profile 语义写清楚。

## 7.3 脚本输出要面向外部读者

当前脚本已经有：

- step 日志
- fail hint

这是正确方向。

`v8.3` 后续建议继续保持：

- 失败时明确告诉用户下一步先看什么
- 不要只打印一堆内部细节

## 8. v8.3 推荐实施顺序

建议按最小顺序推进：

1. 先整理 demo profile
2. 再整理统一 demo guide
3. 再整理 demo troubleshooting
4. 最后视需要补更清晰的入口 wrapper

这个顺序的原因很简单：

- 先说清楚怎么用
- 再说清楚出问题怎么办
- 最后才调整脚本门面

## 9. v8.3 验收标准

`v8.3` 完成的最小标准建议定义为：

### 9.1 外部读者知道该先跑哪个 demo

不能再依赖作者口头解释，
别人也能知道：

- 先 image
- 再 encrypted image
- 最后 raw-device

### 9.2 外部读者知道每种 demo 的风险边界

必须明确知道：

- image 风险最低
- raw-device 风险更高
- encrypted demo 不是生产级安全承诺

### 9.3 外部读者知道 pass 代表什么

不是“脚本绿了就完事”，
而是知道：

- image demo pass 代表什么
- raw-device demo pass 代表什么
- encrypted demo pass 代表什么

### 9.4 外部读者知道失败先看哪里

即使不懂代码，
也能先按文档做第一轮排障。

## 10. 一句话总结

如果说：

- `v8.0` 让别人看懂仓库入口
- `v8.1` 让别人看懂格式边界
- `v8.2` 让别人看懂怎么测和怎么恢复

那么：

```text
v8.3 解决的是：
让别人真正能把 CRYEXTS 当成一个“可执行、可复现、可演示”的产品化 demo 来使用
```
