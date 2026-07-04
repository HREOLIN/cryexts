# CRYEXTS v7.0 USB Demo Guide

## 1. 这份文档解决什么问题

`Version 7.0` 的目标不是继续增加新的磁盘结构，而是把 `Version 6 MVP` 已有能力收口成：

```text
可稳定演示的 USB / raw-device baseline
```

这份文档回答的是：

- 如何用 U 盘里的镜像文件做最安全的 demo
- 如何在确认风险后，切到真实分区 raw-device 模式
- `v7.0` 的 smoke 脚本需要哪些参数
- 当前哪些行为属于可演示范围，哪些还不应承诺

## 2. v7.0 的定位

一句话理解：

```text
v7.0 = USB demo baseline
```

它重点做的是：

- 统一 demo 入口脚本
- 统一使用流程
- 统一安全边界

它没有做的是：

- 新的 on-disk feature
- 新的 journal 格式
- 新的 extent 结构

所以 `v7.0` 更像“工程化入口版本”，不是“结构升级版本”。

## 3. 推荐的两种 demo 方式

### 3.1 方式 A：U 盘普通文件系统里放 CRYEXTS 镜像

这是最推荐的方式。

模型是：

```text
U 盘原本还是 ext4 / exfat / ntfs
-> 在里面放一个 cryexts image file
-> loop mount 这个 image
```

优点：

- 风险最低
- 不会直接覆盖整个 U 盘分区
- 更适合当前实验型文件系统阶段

适合：

- 第一次演示
- 跑脚本
- 跑加密和恢复 demo

### 3.2 方式 B：真实 U 盘分区直接格式化为 CRYEXTS

模型是：

```text
/dev/sdb1
-> mkfs.cryexts
-> mount -t cryexts /dev/sdb1
```

优点：

- 更接近真实块设备部署

风险：

- 一旦设备选错，会直接覆盖分区内容
- 当前阶段仍然不适合重要数据

适合：

- 已经在镜像模式跑通后
- 专用测试 U 盘
- 你明确知道目标设备节点

## 4. v7.0 新脚本

### 4.1 `scripts/smoke_version6_mvp.sh`

作用：

- 串跑 `v6.0` 到 `v6.6`
- 证明技术基线完整

### 4.2 `scripts/smoke_v7_0_usb_demo.sh`

作用：

- 跑 `v7.0` USB demo baseline
- 支持 image mode 和 raw-device mode

### 4.3 `scripts/smoke_version7_demo.sh`

作用：

- 先跑 `Version 6 MVP`
- 再跑 `v7.0 USB demo`

这意味着 `Version 7` 的对外演示链路不是“跳过前面技术版本”，而是建立在 `Version 6` 已完成的能力之上。

## 5. `smoke_v7_0_usb_demo.sh` 参数说明

### 5.1 通用参数

- `DEMO_MODE`
  可选值：
  - `auto`
  - `image`
  - `raw`

  默认值：
  ```text
  auto
  ```

  语义：
  - 如果设置了 `TARGET_DEVICE`，自动走 `raw`
  - 如果 `USB_HOST_DIR` 存在，自动走 `image`
  - 否则脚本会停止，并提示你下一步该怎么跑

- `MNT`
  挂载点，默认：
  ```text
  /tmp/cryexts-mnt
  ```

- `KEY`
  可选。设置后，脚本会创建加密卷并用对应 key 挂载。

- `LABEL`
  文件系统卷标，默认 `v70demo`。

### 5.2 image mode 参数

- `USB_HOST_DIR`
  外层宿主目录，默认：
  ```text
  /media/$USER/USB
  ```

- `IMG_NAME`
  镜像文件名，默认：
  ```text
  cryexts-v7_0-demo.img
  ```

- `IMG_PATH`
  最终镜像路径，默认：
  ```text
  $USB_HOST_DIR/$IMG_NAME
  ```

- `SIZE_MB`
  镜像大小，默认 `256`。

### 5.3 raw-device mode 参数

- `TARGET_DEVICE`
  必填，例如：
  ```text
  /dev/sdb1
  ```

- `ACK_RAW_DEVICE`
  必填，必须显式设置成：
  ```text
  I_UNDERSTAND_THE_RISK
  ```

这是为了防止误操作。

## 6. 推荐的执行顺序

### 6.1 第一步：先跑 Version 6 技术基线

```bash
cd ~/cryexts
chmod +x scripts/smoke_version6_mvp.sh
./scripts/smoke_version6_mvp.sh
```

### 6.2 第二步：跑 image mode USB demo

假设 U 盘已经挂在：

```text
/media/$USER/USB
```

执行：

```bash
chmod +x scripts/smoke_v7_0_usb_demo.sh
./scripts/smoke_v7_0_usb_demo.sh
```

这里虽然没有显式写 `DEMO_MODE=image`，但因为 `USB_HOST_DIR` 存在，`auto` 会自动选择 image mode。

如果想跑加密 demo：

```bash
KEY=usb-demo-key ./scripts/smoke_v7_0_usb_demo.sh
```

### 6.3 第三步：确认 image mode 稳定后，再切 raw mode

```bash
DEMO_MODE=raw \
TARGET_DEVICE=/dev/sdb1 \
ACK_RAW_DEVICE=I_UNDERSTAND_THE_RISK \
./scripts/smoke_v7_0_usb_demo.sh
```

## 7. image mode 具体在测什么

`v7.0` 脚本在 image mode 下会做：

- `mkfs.cryexts`
- `cryextsck`
- mount
- 普通文件写入
- hard link / symlink / rename
- 简单 xattr
- 大文件复制
- sparse file
- 多目录项创建
- `sync`
- `umount`
- 再 `cryextsck`
- remount 后再次验证

如果启用 `KEY`，还会额外检查：

- 镜像文件里不应直接 grep 到明显明文

## 8. raw-device mode 具体在测什么

raw-device mode 的测试内容和 image mode 基本一致，区别只在外层介质：

- image mode：外层是普通文件
- raw mode：外层是实际块设备分区

所以 raw-device mode 的重点不是“多一个功能”，而是：

```text
证明同样的演示链路在真实分区路径下也能成立
```

## 9. 当前安全边界

一定要明确：

- 当前仍然是实验型文件系统
- 不要在有重要数据的 U 盘上直接测试
- raw mode 必须只对专用测试分区操作
- 当前不应承诺生产级稳定性和数据安全性

建议你的对外表述是：

```text
CRYEXTS v7.0 适合专用测试 U 盘、loop image 和研究/演示场景，
不建议用于重要生产数据。
```

## 10. 你后面需要提供给我的 U 盘信息

如果你要我继续帮你把 raw-device 流程收得更实，我后面会需要你给我：

- `lsblk -o NAME,SIZE,FSTYPE,MOUNTPOINT,MODEL`
- `sudo fdisk -l`
- 你准备用的目标分区，例如 `/dev/sdb1`
- 是否需要加密 demo

有了这些信息，我就可以继续把：

- raw-device 安全步骤
- 你的专用演示命令
- 可能的风险点

进一步定制到你的设备环境上。

## 11. 一句话总结

`v7.0` 不是在升级文件系统结构，而是在升级：

```text
它的演示入口、部署入口和对外可操作性
```

这正是从 `Version 6 功能 MVP` 走向 `Version 7 demo-ready` 的第一步。
