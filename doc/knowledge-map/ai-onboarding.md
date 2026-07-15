# AI 快速学习与修改指南

本文给第一次进入 CRYEXTS 的 AI 或开发者一条最短但安全的学习路径。目标不是记住所有历史版本，而是先找到当前格式事实、实际调用链和验证入口。

## 1. 首次读取顺序

### 第一轮：建立边界

1. `Makefile`：确认内核模块对象和用户态工具。
2. `cryexts_fs.h`：只读常量、feature bit 和全部 on-disk 结构。
3. `cryexts.h`：读 `cryexts_sb_info`、`cryexts_inode_info` 和跨模块 API。
4. 本目录的 `architecture.md`：把结构映射到 mount/read/write/namespace/recovery。

### 第二轮：跟主链路

1. `super.c::cryexts_fill_super`：挂载顺序与资源生命周期。
2. `inode.c::cryexts_iget`、`cryexts_write_inode_to_disk`、`cryexts_resolve_block`：inode 和映射主干。
3. `file.c::cryexts_file_aops`：当前 page-cache/writeback 数据路径。
4. `dir.c::cryexts_dir_inode_operations`：命名空间入口。
5. `journal.c::cryexts_journal_begin/commit/replay`：一致性边界。

### 第三轮：按任务补读

只在任务涉及对应区域时读 `balloc.c`、`crypto.c`、`xattr.c`、`metadata.c` 和工具源码。历史 `doc/cryexts-v*.md` 用于理解演进原因，不作为当前实现的首要事实来源。

## 2. 修改路由表

| 修改目标 | 必读源码 | 同步检查 |
| --- | --- | --- |
| superblock/feature bit/格式版本 | `cryexts_fs.h`, `super.c`, `tools/mkfs.cryexts.c` | `cryextsck.c`、所有 inspect、兼容策略、checksum |
| GDT/块组扩容 | `super.c`, `balloc.c`, `mkfs.cryexts.c` | 多块 GDT 边界、尾组、fsck group counter |
| inode 字段/生命周期 | `cryexts_fs.h`, `cryexts.h`, `inode.c` | mkfs root inode、fsck、evict/orphan、endian |
| extent/大文件/稀疏文件 | `inode.c`, `metadata.c` | extent inspect、truncate、hole punch、checksum、max size |
| read/write 性能 | `file.c`, `inode.c`, `crypto.c` | page locking、dirty/writeback、fsync、稀疏洞、加密 |
| allocator/locality | `balloc.c`, `inode.c` | bitmap/counter/checksum、保留区、journal 尾区 |
| 目录与索引 | `dir.c`, `inode.c` | 12-block 上限、hash mask、rename/unlink、dir inspect/fsck |
| journal/recovery | `journal.c`, `super.c` | control/descriptor/commit 顺序、inject 工具、pre/post fsck |
| xattr/policy | `xattr.c`, `crypto.c`, `inode.c` | root+overflow 上限、policy xattr、listxattr、密钥泄漏 |
| checksum | `metadata.c` | mkfs、fsck、reserved offset、所有写入点 |

## 3. 开始修改前的五个问题

1. 改的是 VFS/runtime 行为，还是 on-disk 格式？后者必须同步 mkfs、fsck、inspect 和 feature/version。
2. 这项元数据修改是否被 journal 覆盖？所有错误路径是否 abort？
3. 是否同时更新 bitmap、group counter、super counter 和 checksum？
4. 是否跨越目录 12 blocks、extent 4 leaves、xattr 2 blocks、journal payload 容量等当前上限？
5. 加密 inode 是否仍只通过 inode-aware data I/O 读写？

## 4. 不能破坏的不变量

### 格式与范围

- 所有 physical block 必须 `< blocks_count`，且不能误分配 super/GDT/bitmap/inode-table/policy/journal。
- GDT 项数必须覆盖 `group_count`，GDT block 数必须按结构体总字节数向上取整。
- 尾组长度使用实际剩余 blocks，不能假定总是 4096。
- inode number 与 group/local index 转换必须一致，inode 1 是 root。

### 分配与持久化

- bitmap 是占用状态真相；`next_*`、goal、reservation 只是提示。
- 分配或释放后，组级和全局 free counter 必须成对更新。
- dirty buffer 必须在 checksum 更新后提交，不能把未更新校验的数据写盘。
- truncate/unlink 的块释放需要与 orphan/journal 顺序配合，防止 crash 后泄漏或双重释放。

### 映射与目录

- extent 按 logical_start 有序且不重叠，相邻可合并时保持规范化。
- `cryexts_resolve_block(create=false)` 对 hole 返回 physical 0，而不是随意报 I/O 错误。
- 目录索引不是权威数据；lookup 最终必须比较完整名称。
- 增删改目录项必须同步 `entries`、`dir_blocks`、bucket mask 和 checksum。

### Page cache 与并发

- `write_begin` 成功返回时 page 必须锁定；`write_end` 负责 dirty、unlock 和 put。
- `writepage` 必须成对执行 `set_page_writeback/end_page_writeback`，错误时 redirty 并设置 mapping error。
- `cryexts_file_aops.set_page_dirty` 不得为空。
- 当前实现假定 `PAGE_SIZE == 4096`；改变这一点需要重新设计一页多块/一块多页处理。

### 加密与安全

- 磁盘不保存明文 key 或 derived key。
- metadata 不使用文件数据 cipher；文件数据不能绕过 inode policy。
- 日志和调试输出不得打印密钥材料。

## 5. 工具与测试地图

### 构建和基础校验

```bash
make
./tools/mkfs.cryexts ...
./cryextsck image-or-device
```

项目 Makefile 实际把工具构建到仓库根目录，因此执行位置以 Makefile 结果为准。

### Inspect 工具

| 工具 | 用途 |
| --- | --- |
| `cryexts_gdt_inspect` | 查看多块 GDT、各组范围、bitmap/table 和 free counter |
| `cryexts_extent_inspect` | 查看 inode extent root、leaf、逻辑到物理映射 |
| `cryexts_dir_index_inspect` | 查看目录 index block、bucket masks、entries |
| `cryexts_journal_inspect` | 查看 control/descriptor/commit、sequence、checksum |
| `cryexts_alloc_inspect` | 查看 allocator 与组级空间状态 |
| `cryexts_xattr_inspect` | 查看 xattr root/overflow 内容 |
| `cryexts_policy_inspect` | 查看 policy table，不输出密钥 |

### Inject 工具

| 工具 | 用途 |
| --- | --- |
| `cryexts_journal_inject` | 构造 journal v1 恢复场景 |
| `cryexts_journal_v2_inject` | 构造 v2 已提交未 checkpoint 场景 |
| `cryexts_orphan_inject` | 构造 orphan cleanup 场景 |

Inject 会有意制造 fsck 异常，只能用于测试镜像，不能对唯一数据副本运行。

### 当前主线 smoke

| 脚本 | 验证目标 |
| --- | --- |
| `smoke_v10_0_performance_baseline.sh` | 基准框架与顺序读写指标 |
| `smoke_v10_1_cached_read.sh` | page cache 重复读取与覆盖后一致性 |
| `smoke_v10_2_buffered_write.sh` | generic buffered write 路径 |
| `smoke_v10_3_writeback.sh` | dirty page、writeback、卸载后持久化 |
| `smoke_v10_4_encrypted_cache.sh` | 缓存明文、policy-aware 加密、raw 泄漏检查及 plain/encrypted 对比 |
| `smoke_version10_demo.sh` | version 10 当前组合入口 |

格式/恢复相关修改还应选择对应 v5-v9 smoke 回归。默认使用 image 测试；raw USB 测试仅作为额外部署验证，并要求再次确认设备名，禁止默认指向整盘或系统盘。

## 6. 最小验证矩阵

每次代码变更至少完成与风险匹配的检查：

| 变更级别 | 最小验证 |
| --- | --- |
| 文档 | 链接、函数名、字段名、`git diff --check` |
| 单模块 runtime 行为 | 编译模块 + 对应 smoke + 最终 fsck clean |
| allocator/inode/目录 | 对应 inspect + create/write/truncate/unlink/remount + fsck |
| on-disk 格式 | mkfs + inspect + fsck + mount/remount + corruption/recovery case |
| journal/recovery | pre-replay fsck 预期失败 + mount replay + post-replay fsck clean |
| page cache/writeback | 多次 cached read + small writes + fsync + unmount/remount + 内容比对 |
| raw device | image 全通过后再做；额外检查 `dmesg` 无 Oops、I/O error、disconnect |

## 7. 故障定位顺序

1. 保存完整 smoke 输出，不要只看最后一行。
2. 查看最新 `dmesg`，先排除 kernel Oops、USB disconnect 和底层 I/O error。
3. 运行 `cryextsck` 判断是 pending replay、索引不一致还是块引用损坏。
4. 用对应 inspect 工具查看磁盘事实。
5. 从第一个异常结构反向追踪唯一写入点和 journal 覆盖范围。
6. 修复共享根因，不在每个调用者重复加临时保护。

典型例子：v10.3 小写入导致 `set_page_dirty -> NULL` Oops，根因不是 Python 负载，而是 `cryexts_file_aops` 未提供 `set_page_dirty`。正确修复是在 aops 中统一配置 `__set_page_dirty_nobuffers`。

## 8. AI 变更交付模板

完成实现后，应简洁报告：

```text
目标：本次解决什么问题
格式：是否改变 on-disk format / feature bit
实现：核心调用链在哪里改变
一致性：journal、bitmap/counter、checksum 如何保证
验证：运行了哪些 smoke/inspect/fsck，结果如何
边界：仍保留哪些明确上限或未验证环境
```

若当前环境无法编译 Linux 模块，必须明确写“仅完成静态校验”，不能把源码检查描述为测试通过。

## 9. 文档维护规则

- 新增 on-disk 结构体：在 `api-and-structures.md` 逐字段说明，写清位置、大小、endianness、校验和兼容策略。
- 新增跨模块函数：记录职责、输入输出、错误语义、主要调用者和是否要求持有锁/事务。
- 改变主调用链或能力边界：更新 `architecture.md` 的图和不变量。
- 新增版本实现：版本文档记录动机和案例；知识地图只更新最终主线状态，不复制版本历史。
