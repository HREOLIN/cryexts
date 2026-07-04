# CRYEXTS 使用文档

## 1. 文档目标

这份文档用于指导在 Ubuntu Linux 上测试 CRYEXTS 当前阶段的基本能力。

当前建议测试的能力包括：

- 编译内核模块和用户态工具
- 格式化镜像
- 加载和卸载内核模块
- 挂载和卸载文件系统
- 目录创建
- 空文件创建
- 小文件写入和读取
- 多 block 文件写入和读取
- 大目录创建和删除
- 重新挂载后的持久化验证
- 文件系统一致性检查
- 透明加密镜像验证

## 2. 当前环境要求

- 操作系统：Ubuntu Linux
- 当前目标内核：`5.15.0-139-generic`
- 需要安装内核头文件
- 需要 root 权限执行 `insmod`、`mount`、`umount`、`rmmod`

## 3. 当前可执行文件

在项目根目录执行 `make` 后，会生成：

- `cryexts.ko`
- `mkfs.cryexts`
- `cryextsck`

## 4. 编译步骤

进入项目目录：

```bash
cd ~/cryexts
```

编译：

```bash
make clean
make
```

预期结果：

- `cryexts.ko` 编译成功
- `mkfs.cryexts` 编译成功
- `cryextsck` 编译成功

## 5. 手工测试步骤

### 5.1 创建测试镜像

```bash
dd if=/dev/zero of=cryexts.img bs=1M count=64
```

### 5.2 格式化镜像

```bash
./mkfs.cryexts -f cryexts.img
```

预期结果：

- 输出 `Created CRYEXTS filesystem on cryexts.img`

### 5.3 检查镜像结构

```bash
./cryextsck cryexts.img
```

预期结果：

- 输出 `cryextsck: cryexts.img clean`

### 5.4 加载内核模块

```bash
sudo insmod cryexts.ko
```

确认文件系统已注册：

```bash
cat /proc/filesystems | grep cryexts
```

预期结果：

- 输出包含 `cryexts`

### 5.5 创建挂载点

```bash
sudo mkdir -p /tmp/cryexts-mnt
```

### 5.6 挂载镜像

```bash
sudo mount -o loop -t cryexts cryexts.img /tmp/cryexts-mnt
```

查看挂载结果：

```bash
mount | grep cryexts
```

## 6. 基本能力测试

### 6.1 测试根目录

```bash
ls -la /tmp/cryexts-mnt
```

预期结果：

- 能正常列出目录
- 至少包含 `.` 和 `..`

### 6.2 测试创建目录

```bash
sudo mkdir /tmp/cryexts-mnt/dir1
ls -la /tmp/cryexts-mnt
```

预期结果：

- `dir1` 创建成功
- `ls` 能看到 `dir1`

### 6.3 测试创建空文件

```bash
sudo touch /tmp/cryexts-mnt/empty.txt
ls -la /tmp/cryexts-mnt
```

预期结果：

- `empty.txt` 创建成功
- `ls` 能看到 `empty.txt`

### 6.4 测试写入小文件

```bash
echo "hello cryexts" | sudo tee /tmp/cryexts-mnt/dir1/a.txt
```

### 6.5 测试读取小文件

```bash
cat /tmp/cryexts-mnt/dir1/a.txt
```

预期结果：

- 输出 `hello cryexts`

### 6.6 测试多 block 文件

```bash
python3 - <<'PY' > /tmp/cryexts-large.bin
import sys
data = bytearray()
for i in range(32768):
    data.append((i * 13 + 7) & 0xff)
sys.stdout.buffer.write(data)
PY
sudo cp /tmp/cryexts-large.bin /tmp/cryexts-mnt/dir1/big.bin
cmp /tmp/cryexts-large.bin <(sudo cat /tmp/cryexts-mnt/dir1/big.bin)
```

预期结果：

- `cmp` 通过

### 6.7 测试大目录

```bash
sudo mkdir /tmp/cryexts-mnt/bigdir
for i in $(seq 1 100); do sudo touch /tmp/cryexts-mnt/bigdir/file_$i; done
ls /tmp/cryexts-mnt/bigdir | wc -l
```

预期结果：

- 文件数量正常
- 不会因为目录过大而失败

### 6.8 测试重新挂载后的持久化

先卸载：

```bash
sudo umount /tmp/cryexts-mnt
```

重新挂载：

```bash
sudo mount -o loop -t cryexts cryexts.img /tmp/cryexts-mnt
```

再次检查：

```bash
ls -la /tmp/cryexts-mnt
ls -la /tmp/cryexts-mnt/dir1
cat /tmp/cryexts-mnt/dir1/a.txt
```

预期结果：

- `dir1` 仍然存在
- `empty.txt` 仍然存在
- `a.txt` 仍然存在
- `cat` 仍然输出 `hello cryexts`

## 7. 自动化测试

### 7.1 第一阶段脚本

```bash
chmod +x scripts/smoke_phase1.sh
./scripts/smoke_phase1.sh
```

### 7.2 第二阶段脚本

```bash
chmod +x scripts/smoke_phase2.sh
./scripts/smoke_phase2.sh
```

### 7.3 第三阶段脚本

```bash
chmod +x scripts/smoke_phase3.sh
./scripts/smoke_phase3.sh
```

### 7.4 坏镜像测试脚本

```bash
chmod +x scripts/corrupt_phase3.sh
./scripts/corrupt_phase3.sh
```

### 7.5 第四阶段透明加密 MVP 脚本

```bash
chmod +x scripts/smoke_phase4.sh
./scripts/smoke_phase4.sh
```

预期结果：

- 输出 `phase4 smoke test passed`
- 正确 key 可以挂载和读取明文
- 错误 key 会被拒绝挂载
- 镜像文件中搜不到写入的明文内容

### 7.6 Version 2.0 磁盘布局脚本

```bash
chmod +x scripts/smoke_v2_0_layout.sh
./scripts/smoke_v2_0_layout.sh
```

预期结果：

- 输出 `v2.0 layout smoke test passed`
- `mkfs.cryexts` 创建 Version 2 镜像
- `cryextsck` 能检查 V2 bitmap 元数据
- 现有挂载、目录创建和小文件读写链路仍然可用

### 7.7 Version 2.1 bitmap 分配器脚本

```bash
chmod +x scripts/smoke_v2_1_bitmap.sh
./scripts/smoke_v2_1_bitmap.sh
```

预期结果：

- 输出 `v2.1 bitmap smoke test passed`
- 删除文件后重新创建文件仍能正常写读
- 删除目录后重新创建目录仍能正常使用
- `cryextsck` 仍返回 clean

### 7.8 Version 2.2 多 block 文件脚本

```bash
chmod +x scripts/smoke_v2_2_direct_blocks.sh
./scripts/smoke_v2_2_direct_blocks.sh
```

预期结果：

- 输出 `v2.2 direct-block smoke test passed`
- 32KB 文件写入和读取成功
- 重挂载后仍可正确读取

### 7.9 Version 2.3 大目录脚本

```bash
chmod +x scripts/smoke_v2_3_large_dir.sh
./scripts/smoke_v2_3_large_dir.sh
```

预期结果：

- 输出 `v2.3 large-directory smoke test passed`
- 大目录可正常创建和删除

### 7.10 Version 2.4 cryextsck 脚本

```bash
chmod +x scripts/smoke_v2_4_cryextsck.sh
./scripts/smoke_v2_4_cryextsck.sh
```

预期结果：

- 输出 `v2.4 cryextsck smoke test passed`

### 7.11 Version 2.5 加密层脚本

```bash
chmod +x scripts/smoke_v2_5_encryption.sh
./scripts/smoke_v2_5_encryption.sh
```

预期结果：

- 输出 `v2.5 encryption smoke test passed`
- 正确 key 可挂载
- 错误 key 被拒绝
- 多 block 加密文件读写成功
- 镜像中搜不到明文

## 8. 第四阶段加密手工测试

### 8.1 创建加密镜像

```bash
dd if=/dev/zero of=cryexts.img bs=1M count=64
./mkfs.cryexts -f -E "test-key" cryexts.img
./cryextsck cryexts.img
```

预期结果：

- `mkfs.cryexts` 输出 `Encrypted: yes`
- `cryextsck` 输出 `clean (encrypted data blocks)`

### 8.2 使用正确 key 挂载

```bash
sudo insmod cryexts.ko
sudo mkdir -p /tmp/cryexts-mnt
sudo mount -o loop,key=test-key -t cryexts cryexts.img /tmp/cryexts-mnt
```

### 8.3 写入并读取文件

```bash
sudo mkdir /tmp/cryexts-mnt/dir1
echo "secret message" | sudo tee /tmp/cryexts-mnt/dir1/a.txt
cat /tmp/cryexts-mnt/dir1/a.txt
```

预期结果：

- `cat` 输出 `secret message`

### 8.4 验证镜像中没有文件明文

```bash
sudo umount /tmp/cryexts-mnt
grep -a "secret message" cryexts.img || echo "plaintext not found"
```

预期结果：

- 输出 `plaintext not found`

### 8.5 验证错误 key 会失败

```bash
sudo mount -o loop,key=wrong-key -t cryexts cryexts.img /tmp/cryexts-mnt
```

预期结果：

- `mount` 失败
- `dmesg | tail -50` 中能看到 `cryexts: wrong encryption key`

## 9. 清理步骤

卸载文件系统：

```bash
sudo umount /tmp/cryexts-mnt
```

卸载模块：

```bash
sudo rmmod cryexts
```

删除测试镜像：

```bash
rm -f cryexts.img cryexts-corrupt.img
```

## 10. 常见问题排查

### 10.1 `insmod` 失败

查看内核日志：

```bash
dmesg | tail -120
```

### 10.2 `mount` 失败

检查：

- 镜像是否重新格式化过
- 当前 `cryexts.ko` 是否和当前镜像格式匹配
- `cryextsck` 是否报告错误

建议执行：

```bash
./cryextsck cryexts.img
dmesg | tail -120
```

### 10.3 `ls`、`mkdir`、`touch`、`cat` 失败

建议执行：

```bash
dmesg | tail -120
```

并记录失败命令的终端输出。

## 11. 当前限制

当前版本仍有明确限制：

- 普通文件暂时只支持 12 个 direct block
- 目录暂时只支持 12 个 direct block
- 空间分配仍然是 bitmap + 顺序扫描
- 删除后不会回收 inode/block 的历史内容
- `cryextsck` 目前只修复低风险元数据问题
- 透明加密当前只加密普通文件数据 block，不加密文件名和目录项
- 透明加密当前使用实验 XOR stream，不是生产级密码学算法

## 12. 建议测试顺序

建议按下面顺序测试：

1. `make`
2. `./mkfs.cryexts -f cryexts.img`
3. `./cryextsck cryexts.img`
4. `sudo insmod cryexts.ko`
5. `sudo mount -o loop -t cryexts cryexts.img /tmp/cryexts-mnt`
6. `ls -la /tmp/cryexts-mnt`
7. `mkdir`
8. `touch`
9. `write/read`
10. `umount`
11. `remount`
12. `cryextsck`
13. `./scripts/smoke_phase4.sh`
14. `./scripts/smoke_v2_0_layout.sh`
15. `./scripts/smoke_v2_1_bitmap.sh`
16. `./scripts/smoke_v2_2_direct_blocks.sh`
17. `./scripts/smoke_v2_3_large_dir.sh`
18. `./scripts/smoke_v2_4_cryextsck.sh`
19. `./scripts/smoke_v2_5_encryption.sh`

## 13. 参考文档

- [需求文档](D:/Carl/cryptext4/cryexts/doc/myfs-requirements.md:1)
- [Phase 2 架构图](D:/Carl/cryptext4/cryexts/doc/cryexts-phase2-architecture.md:1)
- [Phase 4 MVP 文档](D:/Carl/cryptext4/cryexts/doc/cryexts-phase4-mvp.md:1)
- [V2.0 磁盘布局说明](D:/Carl/cryptext4/cryexts/doc/cryexts-v2.0-layout.md:1)
- [V2.1 Bitmap 说明](D:/Carl/cryptext4/cryexts/doc/cryexts-v2.1-bitmap.md:1)
- [V2.5 加密层说明](D:/Carl/cryptext4/cryexts/doc/cryexts-v2.5-encryption-layer.md:1)
