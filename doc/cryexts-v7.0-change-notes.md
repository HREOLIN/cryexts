# CRYEXTS v7.0 变更说明

## 1. 这一版做了什么

`v7.0` 不是新的磁盘结构版本，而是 `Version 7` 的第一步：

```text
USB demo baseline
```

这一版重点不是增加新的 inode / extent / journal 结构，而是把 `Version 6 MVP` 收口成一组更稳定的 demo 入口和文档入口。

完成内容：

- 新增 `scripts/smoke_version6_mvp.sh`
- 新增 `scripts/smoke_v7_0_usb_demo.sh`
- 新增 `scripts/smoke_version7_demo.sh`
- 新增 USB demo guide
- 补一份 `v7.0` 变更说明

## 2. 为什么 v7.0 不先加新功能

因为 `Version 6` 刚刚完成的是“功能闭环”。

现在最需要补的不是：

- 更多 feature

而是：

- 更稳定的演示路径
- 更清楚的风险边界
- 更标准化的脚本入口

所以 `v7.0` 的定位是：

```text
先把已有能力变成可部署、可演示、可复现
```

## 3. 新增脚本说明

### 3.1 `smoke_version6_mvp.sh`

作用：

- 串跑 `v6.0` 到 `v6.6`
- 用一个脚本证明 `Version 6` 基线完整

### 3.2 `smoke_v7_0_usb_demo.sh`

作用：

- 建立 `v7.0` USB demo baseline
- 支持：
  - image mode
  - raw-device mode

image mode：

```text
在 U 盘外层宿主文件系统里放一个 cryexts 镜像文件
-> loop mount
```

raw-device mode：

```text
直接对真实分区做 mkfs/mount
```

### 3.3 `smoke_version7_demo.sh`

作用：

- 先跑 `Version 6` 基线
- 再跑 `v7.0` demo baseline

这让 `Version 7` 的演示不是脱离技术基线单独存在，而是建立在完整版本线之上。

## 4. `smoke_v7_0_usb_demo.sh` 在测什么

它会覆盖：

- `mkfs.cryexts`
- `cryextsck`
- mount / umount / remount
- 普通文件写入和读取
- hard link / symlink / rename
- 简单 xattr
- 大文件复制
- sparse file
- 多目录项创建
- sync 后的一致性检查

如果设置了 `KEY`，还会额外走一条加密 demo 路径。

## 5. 这一版的核心设计决定

`v7.0` 刻意不引入新的 on-disk 结构。

原因是：

- `Version 6` 刚刚完成结构型 MVP
- `Version 7` 首先需要解决的是“怎么把它拿出去稳定演示”

所以 `v7.0` 更像一版：

```text
release engineering / demo engineering baseline
```

## 6. image mode 和 raw mode 的关系

`v7.0` 明确支持两种模式，但优先级不同。

### 6.1 image mode

这是默认模式。

优点：

- 风险最低
- 便于脚本化
- 更适合当前实验型阶段

### 6.2 raw mode

这是更接近真实部署的模式。

但这版明确做了保护：

- 需要 `TARGET_DEVICE`
- 需要 `ACK_RAW_DEVICE=I_UNDERSTAND_THE_RISK`
- 只接受分区风格设备名

这说明 `v7.0` 已经开始把“设备级安全防呆”纳入脚本设计。

## 7. 文档方面补了什么

新增：

- [doc/cryexts-usb-demo-guide.md](/D:/Carl/cryptext4/cryexts/doc/cryexts-usb-demo-guide.md:1)

这份文档专门解释：

- `v7.0` 的定位
- 两种 demo 方式
- 脚本参数
- 执行顺序
- 风险边界
- 需要提供给后续定制流程的 U 盘信息

## 8. 当前边界

`v7.0` 已经实现：

- `Version 6` 总 smoke 入口
- `Version 7` USB demo baseline 入口
- image / raw 两种模式的统一脚本框架
- 基础风险防呆
- 基础使用说明

`v7.0` 还没有实现：

- 真机长期 stress
- 更系统化 fault injection
- 完整开源发布文档包
- 商用评估材料包

所以这版最准确的定位是：

```text
Version 7 的第一步
```

不是：

```text
Version 7 全部完成
```

## 9. 下一步自然会接什么

`v7.0` 之后最自然的推进顺序是：

1. `v7.1`
   stress / fault injection baseline

2. `v7.2`
   open source packaging

3. `v7.3`
   commercial evaluation package

## 10. 一句话总结

如果 `Version 6` 解决的是：

```text
文件系统功能怎么成立
```

那么 `v7.0` 解决的是：

```text
这些能力怎么被稳定地拿去演示和交付
```
