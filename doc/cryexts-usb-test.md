# CRYEXTS U盘测试文档

## 1. 能不能把当前 V3 MVP 放到移动 U 盘里测

可以，但我建议你分成两种方式理解：

### 方式 A：推荐

```text
在 U 盘里放一个 cryexts 镜像文件，再 loop mount 测试
```

优点：

- 风险最低
- 不会直接覆盖整个 U 盘分区表
- 出问题时只删掉镜像文件即可
- 很适合当前 V3 这种实验性文件系统

### 方式 B：更接近真实块设备

```text
直接把某个 U 盘分区格式化成 cryexts，然后 mount 这个分区
```

优点：

- 更接近真实部署
- 可以直接验证块设备路径

缺点：

- 风险高
- 容易误选设备导致数据被覆盖
- 当前 V3 仍然是实验原型，不适合放重要数据

如果你只是想测试功能，我强烈建议先走 `方式 A`。

## 2. 测试前的重要提醒

- 不要在有重要数据的 U 盘上直接做 raw-device 测试。
- 最好准备一个“专门拿来折腾”的空 U 盘。
- 测试前先确认设备名，不要把系统盘当成 U 盘。
- 当前 CRYEXTS V3 仍然不适合作为长期生产文件系统。

## 3. 当前推荐测试能力

建议重点测试：

- mount / umount
- mkdir / touch / write / read
- 大文件 direct + indirect
- rename
- hard link / symlink
- 加密镜像挂载
- remount 后持久化
- `cryextsck`

## 4. 先确认 U 盘设备

插入 U 盘后，在 Ubuntu 上执行：

```bash
lsblk -o NAME,SIZE,FSTYPE,MOUNTPOINT,MODEL
sudo fdisk -l
```

你需要先明确：

- 整个设备，比如 `/dev/sdb`
- 某个分区，比如 `/dev/sdb1`

如果系统自动挂载了它，先卸载：

```bash
sudo umount /dev/sdb1
```

这里的 `/dev/sdb1` 只是示例，你必须替换成你自己的真实设备名。

## 5. 方式 A：在 U 盘里放镜像文件测试

这是最推荐的测试方式。

### 5.1 准备目录

假设你的 U 盘普通文件系统已经挂在：

```text
/media/$USER/USB
```

准备镜像文件：

```bash
cd ~/cryexts
truncate -s 512M /media/$USER/USB/cryexts-v3.img
```

### 5.2 格式化镜像

普通版：

```bash
./mkfs.cryexts -f /media/$USER/USB/cryexts-v3.img
```

加密版：

```bash
./mkfs.cryexts -f -E "test-key" /media/$USER/USB/cryexts-v3.img
```

### 5.3 先跑 fsck

```bash
./cryextsck /media/$USER/USB/cryexts-v3.img
```

预期：

```text
cryextsck: ... clean
```

或加密卷：

```text
cryextsck: ... clean (encrypted data blocks)
```

### 5.4 挂载镜像

普通版：

```bash
sudo insmod cryexts.ko
sudo mkdir -p /tmp/cryexts-mnt
sudo mount -o loop -t cryexts /media/$USER/USB/cryexts-v3.img /tmp/cryexts-mnt
```

加密版：

```bash
sudo insmod cryexts.ko
sudo mkdir -p /tmp/cryexts-mnt
sudo mount -o loop,key=test-key -t cryexts /media/$USER/USB/cryexts-v3.img /tmp/cryexts-mnt
```

### 5.5 基本功能测试

```bash
sudo mkdir /tmp/cryexts-mnt/dir1
echo "hello usb cryexts" | sudo tee /tmp/cryexts-mnt/dir1/a.txt
cat /tmp/cryexts-mnt/dir1/a.txt
```

### 5.6 大文件测试

```bash
dd if=/dev/urandom of=/tmp/src-1m.bin bs=1K count=1024
sudo cp /tmp/src-1m.bin /tmp/cryexts-mnt/large.bin
sudo cp /tmp/cryexts-mnt/large.bin /tmp/out-1m.bin
cmp /tmp/src-1m.bin /tmp/out-1m.bin
```

### 5.7 rename / hard link / symlink

```bash
sudo mv /tmp/cryexts-mnt/dir1/a.txt /tmp/cryexts-mnt/dir1/b.txt
sudo ln /tmp/cryexts-mnt/dir1/b.txt /tmp/cryexts-mnt/dir1/b_hard.txt
sudo ln -s b.txt /tmp/cryexts-mnt/dir1/b_soft.txt
cat /tmp/cryexts-mnt/dir1/b_hard.txt
readlink /tmp/cryexts-mnt/dir1/b_soft.txt
cat /tmp/cryexts-mnt/dir1/b_soft.txt
stat -c '%h %n' /tmp/cryexts-mnt/dir1/b.txt /tmp/cryexts-mnt/dir1/b_hard.txt
```

### 5.8 卸载后检查

```bash
sudo umount /tmp/cryexts-mnt
./cryextsck /media/$USER/USB/cryexts-v3.img
```

重新挂载再验证一遍：

```bash
sudo mount -o loop -t cryexts /media/$USER/USB/cryexts-v3.img /tmp/cryexts-mnt
ls -la /tmp/cryexts-mnt/dir1
cat /tmp/cryexts-mnt/dir1/b.txt
sudo umount /tmp/cryexts-mnt
sudo rmmod cryexts
```

## 6. 方式 B：直接把 U 盘分区格式化成 cryexts

这个方式风险更高，但更贴近真实块设备。

### 6.1 强烈建议只格式化某个分区

建议：

```text
优先使用 /dev/sdb1 这类分区
不要直接对 /dev/sdb 整盘下手
```

### 6.2 确保分区未挂载

```bash
lsblk -o NAME,SIZE,FSTYPE,MOUNTPOINT
sudo umount /dev/sdb1
```

### 6.3 格式化分区

普通版：

```bash
cd ~/cryexts
./mkfs.cryexts -f /dev/sdb1
```

加密版：

```bash
./mkfs.cryexts -f -E "test-key" /dev/sdb1
```

### 6.4 检查分区

```bash
./cryextsck /dev/sdb1
```

### 6.5 挂载分区

普通版：

```bash
sudo insmod cryexts.ko
sudo mkdir -p /tmp/cryexts-mnt
sudo mount -t cryexts /dev/sdb1 /tmp/cryexts-mnt
```

加密版：

```bash
sudo insmod cryexts.ko
sudo mkdir -p /tmp/cryexts-mnt
sudo mount -t cryexts -o key=test-key /dev/sdb1 /tmp/cryexts-mnt
```

后续测试命令和 `方式 A` 一样。

## 7. 推荐的 U 盘测试顺序

建议按这个顺序测：

1. 先在本地文件 `cryexts.img` 上跑所有 smoke script
2. 再在 `U 盘里的镜像文件` 上重复一遍核心流程
3. 最后才尝试 `U 盘真实分区`

这个顺序最好，因为它把风险从低到高逐步放大。

## 8. 推荐执行的命令清单

### 8.1 先确认当前版本基本稳定

```bash
cd ~/cryexts
make
./scripts/smoke_v3_0_indirect_file.sh
./scripts/smoke_v3_1_rename.sh
./scripts/smoke_v3_2_fsync.sh
./scripts/smoke_v3_3_crypto_api.sh
./scripts/smoke_v3_4_links.sh
```

### 8.2 再做 U 盘镜像文件测试

```bash
truncate -s 512M /media/$USER/USB/cryexts-v3.img
./mkfs.cryexts -f /media/$USER/USB/cryexts-v3.img
./cryextsck /media/$USER/USB/cryexts-v3.img
sudo insmod cryexts.ko
sudo mkdir -p /tmp/cryexts-mnt
sudo mount -o loop -t cryexts /media/$USER/USB/cryexts-v3.img /tmp/cryexts-mnt
```

### 8.3 再做真实分区测试

```bash
./mkfs.cryexts -f /dev/sdb1
./cryextsck /dev/sdb1
sudo mount -t cryexts /dev/sdb1 /tmp/cryexts-mnt
```

## 9. 加密 U 盘测试

如果你要测加密版，建议优先用“U 盘里的镜像文件”方式。

### 9.1 创建加密镜像

```bash
truncate -s 512M /media/$USER/USB/cryexts-enc.img
./mkfs.cryexts -f -E "usb-test-key" /media/$USER/USB/cryexts-enc.img
./cryextsck /media/$USER/USB/cryexts-enc.img
```

### 9.2 正确 key 测试

```bash
sudo insmod cryexts.ko
sudo mkdir -p /tmp/cryexts-mnt
sudo mount -o loop,key=usb-test-key -t cryexts /media/$USER/USB/cryexts-enc.img /tmp/cryexts-mnt
echo "secret usb data" | sudo tee /tmp/cryexts-mnt/secret.txt
cat /tmp/cryexts-mnt/secret.txt
sudo umount /tmp/cryexts-mnt
```

### 9.3 明文检查

```bash
grep -a "secret usb data" /media/$USER/USB/cryexts-enc.img || echo "plaintext not found"
```

### 9.4 错误 key 测试

```bash
sudo mount -o loop,key=wrong-key -t cryexts /media/$USER/USB/cryexts-enc.img /tmp/cryexts-mnt
```

预期：

- 挂载失败
- `dmesg | tail -50` 里能看到 `wrong encryption key`

## 10. 当前版本在 U 盘测试时的限制

当前 V3 虽然可以测，但你要心里有数：

- 还没有 journal
- 掉电恢复能力有限
- 还不是生产级加密文件系统
- 还没有 block groups
- 还没有 extent
- 不建议把唯一数据放进去长期保存

所以比较合理的用途是：

```text
验证文件系统原型在真实可移动块设备上的行为
```

而不是：

```text
把它当日常 U 盘文件系统长期使用
```

## 11. 常见问题排查

### 11.1 `mount` 失败

先看：

```bash
dmesg | tail -120
./cryextsck <image-or-device>
```

### 11.2 设备名选错

一定先看：

```bash
lsblk -o NAME,SIZE,FSTYPE,MOUNTPOINT,MODEL
```

不要凭感觉直接写 `/dev/sdb`。

### 11.3 自动挂载干扰

桌面环境可能会自动挂载 U 盘原有分区。

先卸载再测：

```bash
sudo umount /dev/sdb1
```

### 11.4 测完后系统不认这个分区

这是正常的，因为 `cryexts` 不是系统默认识别文件系统。

你需要自己：

- 用 `cryexts.ko` 去 mount
- 或者重新格式化回 ext4 / exfat / vfat

## 12. 测试后清理

```bash
sudo umount /tmp/cryexts-mnt || true
sudo rmmod cryexts || true
```

如果你是镜像文件测试：

```bash
rm -f /media/$USER/USB/cryexts-v3.img
rm -f /media/$USER/USB/cryexts-enc.img
```

## 13. 推荐结论

如果你现在要在真实移动介质上验证 V3，我建议：

1. 先做 `U 盘里的镜像文件` 测试
2. 再做 `U 盘分区直挂` 测试
3. 加密版和非加密版都各测一遍
4. 每次测试前后都跑一次 `cryextsck`

这是当前阶段最稳、最适合研发验证的方式。
