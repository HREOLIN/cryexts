# CRYEXTS 需求分析

## 1. 项目目标

CRYEXTS 是一个面向 Linux 的自研块设备文件系统。第一阶段目标不是追求复杂特性，而是先实现一个可编译、可加载、可格式化、可挂载、可读写的最小文件系统闭环。

当前建议把 CRYEXTS 定位为：

- 一个 ext2-like 的学习型和实验型文件系统。
- 运行在 Linux 内核模块中，通过 VFS 暴露目录、文件和读写能力。
- 使用用户态 `mkfs.cryexts` 初始化磁盘镜像或块设备。
- 后续逐步加入一致性检查、错误恢复和透明加密。

## 2. 目标运行环境

- 操作系统：Ubuntu Linux。
- 当前目标内核：`5.15.0-139-generic`。
- 加载方式：out-of-tree kernel module。
- 设备类型：loop image 或真实块设备分区。
- 挂载方式：`mount -t cryexts <device> <mountpoint>`。

第一阶段应固定 Linux 5.15 API，避免同时兼容多个内核版本导致实现变复杂。

## 3. 第一阶段 MVP 范围

第一阶段只实现最小可用路径：

```text
mkfs.cryexts -> insmod -> mount -> ls -> mkdir -> touch -> write -> read -> umount
```

### 3.1 用户态格式化工具

`mkfs.cryexts` 需要完成：

- 写入 CRYEXTS superblock。
- 写入 group descriptor。
- 初始化 block bitmap。
- 初始化 inode bitmap。
- 初始化 inode table。
- 创建 root inode。
- 创建 root directory data block。
- 正确标记保留 inode、root inode、元数据 block 和已使用 data block。
- 拒绝过小镜像或布局无法容纳元数据的设备。

### 3.2 内核模块加载

内核模块需要完成：

- 编译生成 `.ko`。
- `insmod` 或 `modprobe` 成功加载。
- 注册文件系统类型 `CRYEXTS`。
- `rmmod` 成功卸载。
- 加载和卸载过程中无 kernel warning、oops、panic。

### 3.3 挂载和卸载

挂载路径需要完成：

- 读取 superblock。
- 校验 magic、block size、feature flags、group descriptor。
- 初始化 in-memory superblock 信息。
- 读取 root inode。
- 创建 `sb->s_root`。
- 支持只读挂载和读写挂载。
- `umount` 后释放 superblock、buffer、cache 等资源。

### 3.4 目录能力

目录能力需要完成：

- `ls` 能列出 root directory。
- root directory 至少包含 `.` 和 `..`。
- `mkdir` 能创建新目录。
- `rmdir` 能删除空目录。
- `lookup` 能根据目录项找到 inode。
- 目录项长度、对齐、`rec_len`、`name_len`、`file_type` 必须和磁盘格式一致。

### 3.5 普通文件能力

普通文件能力需要完成：

- `touch` 能创建空文件。
- `unlink` 能删除普通文件。
- `echo hello > file` 能写入数据。
- `cat file` 能读出写入数据。
- 文件大小、block 映射、inode block 指针、`i_blocks` 能正确更新。
- 卸载后重新挂载，文件和目录仍然存在。

## 4. 第一阶段不做的能力

为了让主链路先稳定，第一阶段暂不实现：

- 透明加密。
- xattr。
- POSIX ACL。
- quota。
- DAX。
- journal。
- fsck 修复工具。
- rename 的复杂语义。
- hard link 的完整边界测试。
- symlink 的完整边界测试。
- 在线 resize。
- 高性能目录索引。

这些能力可以保留设计空间，但不要阻塞 MVP。

## 5. 磁盘布局需求

CRYEXTS 第一阶段采用简单 ext2-like 布局：

```text
+----------------------+
| boot/reserved area   |
+----------------------+
| superblock           |
+----------------------+
| group descriptors    |
+----------------------+
| block bitmap         |
+----------------------+
| inode bitmap         |
+----------------------+
| inode table          |
+----------------------+
| data blocks          |
+----------------------+
```

### 5.1 Superblock

Superblock 至少需要保存：

- magic number。
- block size。
- inode size。
- blocks count。
- inodes count。
- free blocks count。
- free inodes count。
- first data block。
- blocks per group。
- inodes per group。
- first normal inode。
- feature flags。

### 5.2 Inode

Inode 至少需要保存：

- file mode。
- uid/gid。
- file size。
- atime/ctime/mtime。
- link count。
- block count。
- direct block pointers。
- optional indirect block pointers。
- flags。

第一阶段建议只保证 direct block 写读路径稳定，间接块可以放到第二阶段。

### 5.3 Directory Entry

目录项至少需要保存：

- inode number。
- record length。
- name length。
- file type。
- file name。

目录项必须按 4 字节对齐，且一个目录 block 内不能出现 `rec_len = 0`。

### 5.4 Bitmap

Bitmap 需要满足：

- inode bitmap 中保留 inode 必须提前置位。
- root inode 必须置位。
- block bitmap 中 superblock、group descriptor、bitmap、inode table、root directory data block 必须置位。
- 分配 inode/block 时必须先检查 bitmap，再更新 group descriptor 和 superblock 计数。

## 6. 内核 VFS 接口需求

第一阶段至少需要实现：

- `file_system_type`：注册 `CRYEXTS`。
- `super_operations`：释放、同步、statfs、write inode。
- `inode_operations`：lookup、create、mkdir、rmdir、unlink、getattr、setattr。
- `file_operations`：read、write、llseek、fsync。
- `address_space_operations`：read_folio、write_begin、write_end、writepages。

所有接口先以 Linux 5.15 为准。

## 7. 透明加密需求草案

透明加密不进入第一阶段 MVP，但需求先记录如下：

- 用户写入明文，磁盘保存密文。
- 用户读取时自动解密。
- 文件名是否加密需要单独决策。
- 加密粒度优先考虑 file data block。
- key 管理不能硬编码在内核模块中。
- 加密元数据需要版本号，避免后续格式无法升级。
- 加密失败必须返回错误，不能写入半加密数据。

透明加密建议在读写链路稳定后再接入，否则排查问题会非常困难。

## 8. 验收标准

第一阶段完成时，需要能通过以下测试：

```bash
make
sudo insmod cryexts.ko
dd if=/dev/zero of=cryexts.img bs=1M count=64
./mkfs.cryexts -f cryexts.img
mkdir -p /tmp/cryexts-mnt
sudo mount -o loop -t cryexts cryexts.img /tmp/cryexts-mnt
ls -la /tmp/cryexts-mnt
sudo mkdir /tmp/cryexts-mnt/dir1
echo hello | sudo tee /tmp/cryexts-mnt/dir1/a.txt
cat /tmp/cryexts-mnt/dir1/a.txt
sudo umount /tmp/cryexts-mnt
sudo rmmod cryexts
```

预期结果：

- 所有命令成功。
- `cat` 输出 `hello`。
- `dmesg` 中没有 oops、panic、use-after-free、NULL pointer dereference。
- 重新挂载后，`dir1/a.txt` 仍然存在且内容正确。

## 9. 已知设计风险

- 内核版本 API 不匹配会导致无法编译。
- `mkfs` 和内核侧磁盘结构不一致会导致挂载失败或数据损坏。
- inode bitmap 没有正确保留 inode 会导致 `touch/mkdir` 分配非法 inode。
- block bitmap 没有正确保留元数据 block 会导致文件数据覆盖 superblock、bitmap 或 inode table。
- 错误路径如果调用 `panic()`，实验阶段可能直接打崩测试机。
- xattr、ACL、quota、DAX 等高级特性会扩大排查范围，应在 MVP 后再逐个打开。

## 10. 分阶段路线图

### 阶段 0：重新整理工程

目标：

- 保留旧代码作为参考。
- 固定目标内核版本。
- 删除或关闭第一阶段不需要的复杂特性。
- 建立最小测试脚本。

验收：

- `make` 可以在 Ubuntu 5.15 上完成。
- 模块可以 `insmod` 和 `rmmod`。

### 阶段 1：格式化和挂载

目标：

- `mkfs.cryexts` 写出一致的磁盘布局。
- 内核模块可以挂载 CRYEXTS 镜像。
- `ls` 能看到 root directory。

验收：

- `mount -t cryexts` 成功。
- `ls -la` 成功。
- `umount` 成功。

### 阶段 2：目录和普通文件

目标：

- 实现 `mkdir/rmdir`。
- 实现 `touch/unlink`。
- 实现普通文件 read/write。

验收：

- `mkdir` 后 `ls` 能看到目录。
- `touch` 后 `ls` 能看到文件。
- `echo > file` 后 `cat file` 内容正确。
- 重新挂载后数据仍在。

### 阶段 3：一致性和错误处理

目标：

- 增加更严格的 superblock/group/inode/dirent 校验。
- 移除实验阶段危险的 panic 路径。
- 设计只检查不修复的 `cryextsck`。

验收：

- 坏 magic、坏 inode、坏 dirent 能返回明确错误。
- 错误不会导致内核崩溃。

### 阶段 4：透明加密

目标：

- 在 read/write 路径接入加密和解密。
- 设计 key 管理接口。
- 设计加密元数据格式。

验收：

- 用户读写仍是明文体验。
- 磁盘镜像中看不到明文文件内容。
- 错误 key 不能读出正确内容。

## 11. 旧代码处理建议

不要直接删除旧代码。建议先选择一种方式：

- 方式 A：保留旧代码，逐步删减为 MVP。
- 方式 B：把旧代码移动到 `legacy/`，重新创建最小实现。
- 方式 C：新建一个干净分支，从需求文档开始重写。

推荐方式 B 或 C。这样我们能重新开始设计，同时还能回头参考旧实现里已经写过的 ext2-like 逻辑。

