# CRYEXTS v7.3 变更说明

## 1. 本版本解决的问题

`v7.0` 到 `v7.2` 已经把多块 GDT 的三条主链路打通了：

- `mkfs` 能创建多块 GDT
- 内核 mount 路径能读写多块 GDT
- `cryextsck` 能完整校验多块 GDT

所以 `v7.3` 不再新增磁盘结构，而是把之前因为单块 GDT 上限被迫绕开的：

```text
大容量 image / raw-device / USB demo
```

重新接回主线。

一句话概括：

```text
v7.3 = 多块 GDT 能力已经稳定后，恢复大容量 USB / raw-device 演示路径
```

## 2. 本版本没有新增 on-disk 结构

这次没有修改：

- `struct cryexts_super_block`
- `struct cryexts_group_desc`
- journal 格式
- extent / xattr / dir index 结构

`v7.3` 改的是演示脚本和验证路径，不是文件系统格式。

## 3. 修改的脚本

## 3.1 `scripts/smoke_v7_0_usb_demo.sh`

- 功能：Version 7 USB / raw-device 演示的共享实现脚本

这次做了三类改动。

### 改动 A：删除单块 GDT 时代的 raw-device 容量硬限制

旧逻辑里有：

- `MAX_GDT_GROUPS`
- `MAX_RAW_DEVICE_BYTES`
- `check_raw_device_size()`

它们的语义是：

```text
如果目标分区容量超过单块 GDT 能覆盖的上限
脚本直接拒绝继续
```

这在 `v7.0` 早期是合理的，因为当时 `mkfs` 还没完全具备多块 GDT 能力。

但到了 `v7.3`：

- `mkfs` 已支持多块 GDT
- mount 已支持多块 GDT
- `cryextsck` 已支持多块 GDT

继续保留这个限制反而会挡住真实演示路径。

所以这次直接删除该限制。

### 改动 B：新增 `run_gdt_inspect()`

- 功能：统一调用 `cryexts_gdt_inspect`
- image 模式：直接运行
- raw-device 模式：通过 `sudo` 运行

这个函数的作用很简单：

```text
让 USB demo 脚本自己验证
当前格式化出来的目标到底是不是多块 GDT
```

### 改动 C：新增 `verify_gdt_layout()`

- 功能：在 demo 脚本内校验 GDT 布局是否符合预期

这个函数会做：

1. 读取目标大小
2. 调用 `cryexts_gdt_inspect`
3. 解析：
   - `gdt_blocks`
   - `expected_gdt_blocks`
4. 断言：
   - `gdt_blocks` 非空
   - `expected_gdt_blocks` 非空
   - `gdt_blocks == expected_gdt_blocks`
5. 如果目标容量已经超过单块 GDT 上限，再进一步断言：

```text
gdt_blocks > 1
```

这一步的意义是：

```text
v7.3 不只是“能跑 USB demo”
而是“USB demo 确实跑在 multi-GDT 目标上”
```

## 3.2 `scripts/smoke_v7_3_usb_demo.sh`

- 功能：`v7.3` 的用户入口脚本

这个脚本没有重复实现 demo 逻辑，而是只设置 `v7.3` 默认参数，再复用共享脚本。

这是本次刻意保持的最小设计。

### 默认参数

- `SCRIPT_TAG=v7.3`
- `DEMO_TEXT=v7.3 usb demo`
- `IMG_NAME=cryexts-v7_3-demo.img`
- `LABEL=v73demo`
- `SIZE_MB=1024`
- `DIR_FILE_COUNT=128`

这里最关键的是：

```text
SIZE_MB 默认提升到 1024 MiB
```

因为这已经稳定跨过单块 GDT 的容量边界，能自然触发 multi-GDT。

## 4. 修改的关键变量

## 4.1 `SCRIPT_TAG`

- 位置：`scripts/smoke_v7_0_usb_demo.sh`
- 含义：当前脚本的版本标签
- 作用：
  - 打印日志前缀
  - 生成 demo note
  - 让同一份共享脚本能服务于 `v7.0` 和 `v7.3`

## 4.2 `DEMO_TEXT`

- 位置：`scripts/smoke_v7_0_usb_demo.sh`
- 含义：demo 内实际写入文件的文本
- 作用：
  - 首次写入内容
  - remount 后读回校验
  - image 加密模式下的明文检查

## 4.3 `SINGLE_GDT_MAX_BYTES`

- 含义：单块 GDT 理论上能覆盖的最大设备字节数
- 计算方式：

```text
SINGLE_GDT_GROUPS = block_size / sizeof(group_desc)
SINGLE_GDT_MAX_BYTES = SINGLE_GDT_GROUPS * blocks_per_group * block_size
```

- 作用：不再用它“拒绝执行”，而是用它“判断当前测试是否应该已经跨入 multi-GDT”

这就是 `v7.3` 和 `v7.0` 的最大区别：

```text
同一个阈值
以前用于阻止测试
现在用于验证测试是否真的跨过边界
```

## 5. 处理流程

`v7.3` USB demo 的流程现在是：

```text
make
-> mkfs
-> fsck
-> inspect GDT
-> mount
-> 创建目录/文件/link/xattr/大文件/sparse file
-> umount
-> fsck
-> remount
-> 读回校验
-> umount
-> fsck
-> inspect GDT
```

其中新增的关键点是两次 `inspect GDT`：

- 一次在 `mkfs + fsck` 之后
- 一次在最后一次 `fsck` 之后

这样可以确认：

- 格式化时 GDT 布局正确
- 完整演示链路跑完后 GDT 布局依然自洽

## 6. 案例

假设你用 `v7.3` 默认 image 模式：

```bash
./scripts/smoke_v7_3_usb_demo.sh
```

默认会创建：

```text
1024 MiB image
```

由于单块 GDT 覆盖上限大约在 `848 MiB` 左右，因此这次测试会自然落入：

```text
必须使用多块 GDT
```

脚本在 `verify_gdt_layout()` 中会看到：

- `gdt_blocks == expected_gdt_blocks`
- `gdt_blocks > 1`

然后再继续整条 demo 链路。

这说明 `v7.3` 的演示不是停留在小镜像世界，而是真正建立在 multi-GDT 能力之上。

## 7. 建议的测试方式

## 7.1 更安全的 image 模式

```bash
chmod +x scripts/smoke_v7_3_usb_demo.sh
USB_HOST_DIR=/media/$USER/USB ./scripts/smoke_v7_3_usb_demo.sh
```

这条路径更适合先验证：

- 脚本流程
- 多块 GDT
- mount / remount / fsck

## 7.2 更接近真实设备的 raw-device 模式

```bash
DEMO_MODE=raw \
TARGET_DEVICE=/dev/sdX1 \
ACK_RAW_DEVICE=I_UNDERSTAND_THE_RISK \
./scripts/smoke_v7_3_usb_demo.sh
```

这条路径适合在专用测试分区上验证真实块设备部署。

## 8. 版本关系

截至 `v7.3`，Version 7 主线可以理解为：

- `v7.0`：`mkfs` 写出多块 GDT
- `v7.1`：内核挂载并更新多块 GDT
- `v7.2`：`cryextsck` 读取并校验多块 GDT
- `v7.3`：恢复大容量 image / raw-device / USB demo 路径

也就是说：

```text
底层 multi-GDT 打通
-> 再把真实演示路径接回来
```
