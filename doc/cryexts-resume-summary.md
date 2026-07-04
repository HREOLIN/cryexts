# 自研文件系统项目总结（NAS 核心研发岗位版）

## 1. 这段经历最应该被看见的核心事实

我不是“参与过一个文件系统相关项目”，而是：

> 我从零开始，持续研制了一个可以真实格式化、挂载、读写、校验、恢复、演进的 Linux 文件系统 `CRYEXTS`。

这件事本身，应该是这段项目经历最核心的价值。

因为这意味着我做的不是：

- 调一个现有文件系统接口
- 在 ext4 外面包一层逻辑
- 只做用户态存储工具

而是直接做了文件系统最底层、最核心的事情：

- 自己定义磁盘格式
- 自己实现内核态文件系统驱动
- 自己实现 `mkfs`
- 自己实现 `fsck`
- 自己实现 inspect / smoke / recovery 工具链
- 自己推进版本演进

如果要面向 NAS 核心研发岗位表达，这段经历的第一句话就应该是：

> 独立持续研制自定义 Linux 文件系统，并把它做到了可以在真实块设备上运行和验证的程度。

## 2. 项目名称

`CRYEXTS`：自研 Linux 文件系统

## 3. 项目定位

`CRYEXTS` 不是课堂性质的小 toy，也不是简单的 FUSE 练手，而是一个基于 Linux 内核 VFS 路径持续演进的自定义文件系统实现。

我围绕它完成了一个完整文件系统从无到有的核心闭环：

```text
磁盘格式设计
-> 内核态挂载与读写
-> 元数据分配与持久化
-> 一致性恢复
-> 离线检查与诊断
-> 版本化演进
-> 原始设备与 USB 场景验证
```

这件事情对 NAS 核心研发岗位的重要性在于：

- 这不是“会调用存储接口”
- 而是“真的做过文件系统”

## 4. 我真正做成了什么

我把一个自研文件系统从最初只能：

```text
mkfs -> insmod -> mount -> ls -> umount
```

逐步推进到具备下面这些核心能力：

- 支持真实 `mkfs.cryexts` 格式化镜像和块设备
- 支持内核模块加载、挂载、卸载、重挂载
- 支持目录与文件基础语义：
  - `lookup`
  - `create`
  - `mkdir`
  - `unlink`
  - `rmdir`
  - `rename`
  - `link`
  - `symlink`
  - `read`
  - `write`
  - `truncate`
- 支持 block bitmap / inode bitmap 管理
- 支持 block group 布局
- 支持 journal 与 mount-time replay
- 支持 orphan cleanup
- 支持 extents、extent tree、多叶子映射
- 支持 sparse file / hole
- 支持 directory hash index
- 支持 policy-aware encryption
- 支持 large xattr
- 支持 metadata checksum
- 支持 `cryextsck` 一致性检查
- 支持 inspect 工具与 smoke 脚本闭环
- 已推进到 raw device / U 盘场景验证

换句话说，这不是“写了几个文件系统实验函数”，而是：

> 我真的把一个文件系统研制出来，并持续扩展成了一个具备核心语义、恢复能力和工具链的原型系统。

## 5. 这段经历为什么能证明“我懂文件系统”

### 5.1 因为我不是只写接口，而是自己设计文件系统结构

我自己设计和维护过这些核心 on-disk 结构：

- superblock
- inode
- directory entry
- block bitmap
- inode bitmap
- group descriptor
- journal block layout
- extent metadata
- directory index block
- xattr block / overflow block
- policy table

这意味着我面对过文件系统最本质的问题：

- 什么信息必须持久化
- 什么字段是运行时状态，什么字段是恢复时依据
- 一个新特性加进来，磁盘格式、挂载路径、恢复路径、fsck 路径哪些都要一起改

### 5.2 因为我不是只会读写文件，而是实现了完整数据路径

我不是停留在“文件系统概念理解”，而是在内核里真正实现过这些路径：

- `fill_super`
- `iget`
- inode 持久化
- 目录项查找与插入
- 块分配与释放
- block mapping / extent resolution
- sparse hole 处理
- xattr 读写
- journal begin / record / commit / replay

所以我对文件系统的理解，不是停留在“知道 ext4 / xfs 有什么概念”，而是：

> 我真的做过一个文件系统请求从 VFS 到磁盘块落盘的全过程。

### 5.3 因为我实际处理过一致性与恢复问题

文件系统真正难的部分，从来不是“先写进去”，而是：

- 异常中断后还能不能恢复
- metadata 是否还能自洽
- 是否能做离线检查
- 损坏后是报错、只读，还是尝试修复

我在项目里真实推进过这些问题：

- journal replay
- orphan cleanup
- metadata checksum
- `cryextsck`
- inspect 工具
- recovery smoke
- raw device / USB 场景调试

这部分经验和 NAS 核心研发岗位高度相关，因为 NAS 系统本质上非常看重：

- 一致性
- 可恢复性
- 长期稳定性
- 异常场景下的可诊断性

## 6. 这段经历和 NAS 核心研发岗位的匹配点

如果面试官问“为什么你适合 NAS 核心研发”，我认为这段项目经历最有说服力的地方是：

### 6.1 我做过真正的文件系统，不只是上层存储业务

我接触和解决的是 NAS 底层最核心的问题类型：

- 磁盘布局
- 元数据组织
- 块分配
- 目录索引
- 文件映射
- 崩溃恢复
- 一致性校验
- 原始设备调试

### 6.2 我已经形成了“存储核心问题”的思维方式

我在项目里逐步建立的不是单点技能，而是一种更接近 NAS / 存储内核岗位的思路：

- 先看磁盘结构是否合理
- 再看运行时更新路径是否完整
- 再看异常中断后的恢复语义是否闭环
- 最后看工具是否能验证和诊断

这个思路和 NAS 核心研发的真实工作方式是非常接近的。

### 6.3 我不是只做 demo，而是会做工程闭环

我不是只把文件系统“跑起来”，还围绕它做了完整配套：

- `mkfs.cryexts`
- `cryextsck`
- inspect 工具
- smoke 测试
- 版本设计文档
- 恢复与 USB 场景验证

这说明我做底层系统时，不是只关注主路径功能，而是会主动补：

- 构建
- 验证
- 诊断
- 演进

## 7. 简历中最应该突出的话术

下面这些表述比“我做了文件系统相关开发”更强，也更准确。

### 推荐表述 1

独立持续研制自定义 Linux 文件系统 `CRYEXTS`，从零设计磁盘格式与内核态数据路径，完成 `mkfs / mount / read-write / journal replay / fsck / inspect / smoke` 全链路闭环，并推进到真实块设备与 USB 场景验证。

### 推荐表述 2

从零实现自研 Linux 文件系统，覆盖 superblock、inode、bitmap、block group、journal、extent tree、directory index、xattr、加密策略等核心模块，具备文件系统设计、实现、恢复与验证的完整实践经验。

### 推荐表述 3

主导自研文件系统 `CRYEXTS` 的版本化演进，不仅实现基础挂载读写，还逐步补齐崩溃恢复、一致性校验、目录索引、大文件映射、稀疏文件、元数据校验和离线诊断能力，体现出较强的 NAS / 存储内核方向技术潜力。

## 8. 简历亮点条目

这些条目更适合直接写进简历。

- 从零研制自定义 Linux 文件系统 `CRYEXTS`，独立完成磁盘格式设计、内核态实现、用户态工具链与验证体系建设
- 基于 Linux VFS 实现文件系统主路径，支持目录、文件、链接、重命名、truncate、remount 等核心语义
- 设计并实现 superblock、inode、bitmap、block group、journal、extent tree、directory index、xattr、policy table 等关键 on-disk 结构
- 实现 journal v2、mount-time replay、orphan cleanup、metadata checksum 与 `cryextsck`，形成文件系统一致性与恢复能力闭环
- 实现从 direct / indirect 到 extent tree / sparse file 的文件映射演进，具备较强的底层存储结构抽象能力
- 推进目录 hash index 与 large xattr 支持，处理大目录与扩展元数据场景下的更新、校验与维护问题
- 建立 `mkfs + fsck + inspect + smoke` 工具链，并在 loop image、raw device、USB 设备场景中进行持续验证

## 9. 技能关键词

- 自研文件系统
- Linux Kernel
- Linux VFS
- File System Internals
- On-disk Layout Design
- Journaling
- Crash Recovery
- Metadata Consistency
- Metadata Checksum
- Extent Tree
- Sparse File
- Directory Hash Index
- XAttr
- Policy-based Encryption
- Block Allocator
- fsck
- Raw Device Debugging
- Storage Systems Programming
- C

## 10. 面试时最值得强调的结论

如果只能用几句话总结这段项目经历，我建议这样说：

> 这段经历最能证明我的，不是我“了解文件系统概念”，而是我真的从零研制过一个 Linux 文件系统。  
> 我自己定义磁盘结构，自己实现内核态读写与元数据更新路径，自己补 journal / replay / fsck / inspect / smoke，最后把它推进到真实块设备验证。  
> 这让我对文件系统最核心的几个问题，包括元数据组织、映射结构、分配器、一致性恢复、离线校验，形成了比较系统的理解。  
> 我认为这和 NAS 核心研发岗位的底层能力要求是高度匹配的。

