# CRYEXTS API 与结构体

本文以 `cryexts_fs.h` 和 `cryexts.h` 为准。`__le16/__le32/__le64` 字段均为磁盘小端格式，读取用 `le*_to_cpu`，写入用 `cpu_to_le*`；带 `packed` 的结构体不能依赖编译器自然对齐。

## 1. 核心常量

| 常量 | 值/含义 |
| --- | --- |
| `CRYEXTS_MAGIC` | `0x43525853`，文件系统 magic |
| `CRYEXTS_BLOCK_SIZE` | 4096 字节 |
| `CRYEXTS_SUPER_OFFSET` | superblock 位于 block 0 的 byte 1024 |
| `CRYEXTS_ROOT_INO` | root inode number 为 1 |
| `CRYEXTS_DEFAULT_BLOCKS_PER_GROUP` | 默认每组 4096 blocks |
| `CRYEXTS_DEFAULT_INODE_TABLE_BLOCKS_PER_GROUP` | 默认每组 4 个 inode-table blocks |
| `CRYEXTS_DIRECT_BLOCKS` | inode 内 12 个 direct slots；当前目录上限也由此决定 |
| `CRYEXTS_EXTENT_TREE_ROOT_REFS` | extent tree v2 最多 4 个 leaf references |
| `CRYEXTS_DIR_INDEX_BUCKETS` | 目录索引 64 个 hash buckets |
| `CRYEXTS_DEFAULT_JOURNAL_BLOCKS` | 默认 journal 最多 512 blocks |
| `CRYEXTS_RESERVATION_WINDOW_BLOCKS` | allocator reservation window 为 64 blocks |

## 2. On-disk 结构体

### 2.1 `struct cryexts_super_block`

文件系统全局描述，位于 block 0 的 1024 字节偏移。

| 字段 | 说明 |
| --- | --- |
| `magic` | CRYEXTS 格式标识，必须等于 `CRYEXTS_MAGIC` |
| `version` | on-disk 格式版本；决定可解释的字段与布局规则 |
| `block_size` | 格式块大小，当前必须为 4096 |
| `inode_size` | 一个 `cryexts_inode` 的磁盘字节数 |
| `blocks_count` | 文件系统总 block 数 |
| `inodes_count` | 文件系统总 inode 数 |
| `free_blocks_count` | 全局空闲 block 计数，是各组计数的汇总 |
| `free_inodes_count` | 全局空闲 inode 计数，是各组计数的汇总 |
| `block_bitmap_block` | group 0 block bitmap 的快速定位字段 |
| `inode_bitmap_block` | group 0 inode bitmap 的快速定位字段 |
| `inode_table_start` | group 0 inode table 起始块 |
| `inode_table_blocks` | group 0 inode table 长度 |
| `root_inode_block` | root inode 所在 inode-table block 的快速定位字段 |
| `root_dir_block` | root directory 的初始数据块 |
| `first_data_block` | 格式层记录的首个数据区域位置；分配仍需检查 GDT/bitmap/保留区 |
| `next_ino` | 兼容/快速分配提示，不是唯一分配真相，bitmap 才是权威状态 |
| `next_data_block` | 兼容/快速分配提示，不替代 block bitmap |
| `label` | 固定长度卷标签 |
| `flags` | superblock 标志，如全盘 encrypted 标志 |
| `key_hash` | mount key verifier，不存储明文密钥 |
| `features_compat` | 可兼容忽略的 feature bit，如 journal/prealloc |
| `features_incompat` | 不理解就禁止挂载的 feature bit，如 block groups/extents/journal v2 |
| `features_ro_compat` | 不理解时最多只读挂载的 feature bit，如 metadata checksum/large xattr |
| `encryption_flags` | 启用的数据加密范围 |
| `encryption_kdf` | 密钥派生算法编号 |
| `encryption_alg` | 数据加密算法编号 |
| `salt` | KDF 使用的 16-byte salt |
| `state` | clean、needs-recovery、errors 状态位 |
| `mount_count` | 累计挂载次数 |
| `max_mount_count` | 建议强制检查前的最大挂载次数 |
| `default_encryption_policy` | 新 inode 默认继承的 policy id |
| `last_mount_time` | 最近挂载时间，Unix seconds |
| `last_write_time` | 最近元数据写入时间 |
| `last_check_time` | 最近 fsck 时间 |
| `journal_block` | journal 区域起始 block |
| `journal_blocks` | journal 区域 block 数 |
| `group_count` | block group 总数 |
| `blocks_per_group` | 标准组 block 数；尾组可更短 |
| `inodes_per_group` | 每组可管理 inode 数 |
| `group_desc_table_start` | GDT 起始 block，当前通常为 1 |
| `group_desc_table_blocks` | GDT 连续 block 数，支持多块 GDT |
| `uuid` | 16-byte 文件系统 UUID |
| `volume_name` | 固定长度卷名 |
| `orphan_head` | orphan inode 单链表头，0 表示空 |
| `policy_table_block` | policy table 所在 block，0 表示未启用 |
| `dir_index_seed` | 目录名称 hash 的文件系统级 seed |
| `metadata_csum_type` | 元数据 checksum 算法编号 |
| `journal_sequence` | 最近持久化 journal sequence |
| `fs_generation` | 文件系统格式/实例 generation，用于校验域 |
| `reserved` | 格式扩展与 checksum 存储空间；不得随意复用 |

### 2.2 `struct cryexts_group_desc`

每个 block group 一项，连续存放在 GDT。

| 字段 | 说明 |
| --- | --- |
| `group_start` | 本组起始 physical block |
| `blocks_count` | 本组实际 block 数，尾组可小于标准值 |
| `block_bitmap_block` | 本组 block bitmap 的 physical block |
| `inode_bitmap_block` | 本组 inode bitmap 的 physical block |
| `inode_table_start` | 本组 inode table 起始块 |
| `inode_table_blocks` | 本组 inode table 长度 |
| `free_blocks_count` | 本组空闲 block 数 |
| `free_inodes_count` | 本组空闲 inode 数 |
| `used_dirs_count` | 本组已分配目录 inode 数，用于统计/放置策略 |
| `flags` | 组级状态与未来 feature 位 |
| `reserved` | 扩展及 group checksum 存储空间，修改前先检查 `metadata.c` |

### 2.3 Journal v1/v2

`cryexts_journal_header` 是旧格式事务头：`magic` 标识格式，`flags` 表示有效状态，`entry_count` 是事务项数，`checksum` 校验头和列表，`sequence` 标识事务顺序，`reserved` 保留，`home_blocks[]` 列出最终写回位置。

`cryexts_journal_v2_control` 字段：

| 字段 | 说明 |
| --- | --- |
| `magic` | `JNL2` magic |
| `layout_version` | journal v2 layout version |
| `block_type` | control block 类型，必须为 CONTROL |
| `flags` | ACTIVE 等控制状态 |
| `features` | journal v2 能力位 |
| `checksum` | control block checksum |
| `reserved0` | 对齐/扩展保留 |
| `last_sequence` | 已知最近事务 sequence |
| `active_sequence` | 当前未完成事务；0 表示 idle |
| `tail_sequence` | journal 尾部已接受事务 sequence |
| `checkpoint_sequence` | 已写回 home blocks 的 sequence |
| `descriptor_block` | descriptor physical block |
| `payload_start` | payload 区起始块 |
| `payload_blocks` | payload 可用块数 |
| `commit_block` | 固定 commit physical block |
| `reserved` | 扩展保留，必须保持确定性内容参与校验 |

`cryexts_journal_v2_descriptor` 字段：

| 字段 | 说明 |
| --- | --- |
| `magic/layout_version/block_type` | 标识合法 v2 descriptor |
| `flags` | descriptor 状态位 |
| `entry_count` | 本事务 metadata block 数 |
| `checksum` | descriptor 与 home list checksum |
| `reserved0/reserved` | 保留字段 |
| `sequence` | 所属事务 sequence |
| `payload_start` | payload 镜像起始块 |
| `commit_block` | 对应 commit block |
| `home_blocks[]` | 每个 payload 最终 checkpoint 的 physical block |

`cryexts_journal_v2_commit` 字段：

| 字段 | 说明 |
| --- | --- |
| `magic/layout_version/block_type` | 标识合法 v2 commit |
| `flags` | 必须包含 COMMITTED 才表示事务提交完成 |
| `entry_count` | 必须与 descriptor 一致 |
| `checksum` | commit block checksum |
| `reserved0/reserved` | 保留字段 |
| `sequence` | 必须与 control/descriptor 的活动事务一致 |
| `descriptor_block` | 回指 descriptor，防止布局串线 |

### 2.4 Extent 结构

| 结构/字段 | 说明 |
| --- | --- |
| `cryexts_extent_header.magic` | extent 容器 magic `EX` |
| `entries` | 当前有效 entry/ref 数 |
| `max` | 容器最大 entry 数 |
| `reserved` | 保留 |
| `cryexts_extent.logical_start` | 连续映射的首个 logical block |
| `physical_start` | 连续映射的首个 physical block |
| `length` | 连续 block 数，最大 65535 |
| `flags` | extent 状态扩展位 |
| `cryexts_extent_root_ref.logical_start` | leaf 覆盖范围的最小 logical block |
| `leaf_block` | extent leaf 的 physical block |
| `entries` | leaf 中有效 extent 数 |
| `checksum` | leaf 内容 checksum，用于引用时快速校验 |

### 2.5 Inode、目录和扩展信息

`cryexts_inode` 是固定大小磁盘 inode：

| 字段 | 说明 |
| --- | --- |
| `mode` | 文件类型与权限 |
| `links_count` | 硬链接计数 |
| `uid/gid` | 所有者标识 |
| `size` | 文件逻辑字节数 |
| `blocks` | 已占用 512-byte sectors 计数语义 |
| `atime/ctime/mtime` | 秒级时间戳 |
| `block[12]` | direct block，或被 extent root 格式复用的内联区域 |
| `indirect_block` | single-indirect block，或 extent overflow 相关位置 |
| `inode_flags` | EXTENTS、DIR_INDEX、EXTENT_TREE_V2、IMMUTABLE 等 |
| `reserved` | 存放 `cryexts_inode_extra` 及未来扩展；不能整体清零覆盖未知字段 |

`cryexts_inode_extra` 位于 inode reserved 区：`xattr_block` 指向 xattr root，`encryption_policy_id` 选择加密策略，`next_orphan` 链接 orphan list。

`cryexts_dir_entry` 是变长目录项：`inode` 为目标 inode number，`rec_len` 为当前记录占用字节数，`name_len` 为名称长度，`file_type` 为文件类型提示，`name[]` 保存不带终止符的名称。记录按 4 字节对齐。

### 2.6 目录索引

`cryexts_dir_index_block` 独占一个 metadata block：

| 字段 | 说明 |
| --- | --- |
| `magic` | `DIX1` |
| `buckets` | 有效 bucket 数，当前为 64 |
| `dir_blocks` | 目录当前数据 logical block 数 |
| `entries` | 索引覆盖的目录项总数 |
| `reserved` | checksum 与扩展空间，写入需走 checksum helper |
| `block_masks[64]` | 每个 hash bucket 的 16-bit 候选 logical-block 集合 |

例如 bucket 4 的值为 `0x0005`，表示名称 hash 到 bucket 4 的目录项可能位于 logical block 0 或 2。随后仍需读取目录块并逐项比较完整文件名。

### 2.7 Xattr 与 policy

| 结构/字段 | 说明 |
| --- | --- |
| `cryexts_xattr_block_header.magic` | xattr block magic |
| `entries` | 当前 xattr 数量 |
| `used_bytes` | block 内有效字节数 |
| `overflow_block` | 下一个 xattr block；当前模型最多 root + 1 overflow |
| `cryexts_xattr_entry.name_len` | xattr 名称长度 |
| `namespace_id` | namespace 编号，当前主要为 user |
| `value_len` | value 长度 |
| `data[]` | 紧邻存储 name 和 value |
| `cryexts_policy_entry.policy_id` | policy 唯一编号 |
| `flags` | policy 行为位 |
| `context` | 参与派生 per-policy key 的固定上下文 |
| `cryexts_policy_table_block.magic` | policy table magic |
| `entry_count` | 有效 policy 数 |
| `reserved0/reserved` | 保留及 checksum 空间 |
| `entries[]` | 变长 policy 数组 |

## 3. Runtime 结构体

### 3.1 `struct cryexts_sb_info`

每次挂载一个实例，挂到 `sb->s_fs_info`。

| 字段组 | 字段 | 说明 |
| --- | --- | --- |
| VFS/缓冲 | `sb`, `s_sbh`, `disk_sb` | VFS super、block 0 buffer、其中的磁盘 super 指针 |
| 兼容 bitmap | `block_bitmap_bh`, `inode_bitmap_bh`, `block_bitmap`, `inode_bitmap` | group 0/旧布局快速引用 |
| GDT | `gdt_bhs`, `gdt_storage`, `groups` | 多块 GDT buffer、连续解析副本、group descriptor 数组 |
| group bitmap | `group_*_bitmap_bhs`, `group_*_bitmaps` | 每组 bitmap buffer 与数据指针数组 |
| 布局缓存 | `inode_table_start`, `inode_table_blocks`, `block_bitmap_block`, `inode_bitmap_block` | super 中 group 0 快速字段的 CPU-endian 缓存 |
| 块组参数 | `group_desc_table_start`, `group_desc_table_blocks`, `group_count`, `blocks_per_group`, `inodes_per_group` | 当前挂载的 GDT 与组几何信息 |
| journal 区域 | `journal_block`, `journal_blocks` | journal physical range |
| 分配提示 | `next_ino`, `next_data_block` | allocator 搜索提示，不是权威占用状态 |
| journal sequence | `journal_sequence`, `journal_last_sequence`, `journal_active_sequence`, `journal_tail_sequence`, `journal_checkpoint_sequence` | 当前事务和 checkpoint 状态 |
| feature 状态 | `encrypted`, `journal_enabled`, `journal_v2`, `journal_replaying` | 已解析的运行时开关 |
| 加密全局 | `key_verifier`, `encryption_flags`, `encryption_kdf`, `encryption_alg`, `salt`, `derived_key`, `derived_key_len`, `skcipher` | mount key 与全局 cipher 状态 |
| policy | `policies`, `policy_count` | 已加载 policy runtime 数组 |
| 当前事务 | `journal_entry_count`, `journal_home_blocks` | 内存中的本次 journal home block 集合 |
| 锁 | `alloc_lock`, `journal_lock` | 分配状态与 journal 事务的全局串行化 |

### 3.2 `struct cryexts_policy_runtime`

`policy_id` 和 `flags` 来自磁盘 policy；`context` 是派生上下文；`derived_key` 是内存中的 per-policy key；`skcipher` 是该 policy 的内核 Crypto API transform。密钥材料不得写回磁盘或输出日志。

### 3.3 `struct cryexts_extent_leaf_cache`

`block` 是 leaf physical block，`entries` 是有效 extent 数，`checksum` 是装载时校验值，`extents` 指向内存 extent 数组。该结构属于 inode runtime cache，释放 inode 时必须释放数组。

### 3.4 `struct cryexts_inode_info`

CRYEXTS 私有 inode 映射状态，由通用 `struct inode` 间接持有。

| 字段 | 说明 |
| --- | --- |
| `use_extents` | 当前 inode 是否采用 extent 映射 |
| `inode_flags` | CPU-endian inode feature flags |
| `direct[12]` | direct physical blocks |
| `indirect_block` | single-indirect physical block |
| `extent_entries` | 内联/当前 extent 总项数语义 |
| `extent_inline_max` | 当前格式允许的内联 extent 数 |
| `extent_overflow_entries` | overflow block 有效项数 |
| `extents[]` | inode 内缓存的 inline extents |
| `overflow_extents` | overflow extent 内存数组 |
| `extent_overflow_block` | overflow metadata physical block |
| `extent_overflow_checksum` | overflow block checksum |
| `extent_leaf_count` | extent tree v2 leaf 数 |
| `extent_root_refs[]` | inode 内的 leaf references |
| `extent_leaves[]` | 已装载 leaf 的 runtime cache |
| `xattr_block` | xattr root physical block |
| `encryption_policy_id` | 文件数据加密策略 |
| `next_orphan` | orphan 链接 |
| `dir_index_block` | 目录索引 metadata physical block |
| `alloc_hint_block` | 下一次相邻分配的 physical hint |
| `alloc_goal_group` | 优先分配 group |
| `reservation_start/next/end` | 预分配窗口范围与当前游标 |

## 4. VFS Operation API

| 对象 | 回调 | 功能 |
| --- | --- | --- |
| `cryexts_super_ops` | `put_super/statfs/sync_fs/drop_inode/evict_inode` | 挂载实例释放、统计、同步和 inode 回收 |
| `cryexts_dir_operations` | `iterate_shared/llseek/read` | 目录遍历与偏移处理 |
| `cryexts_dir_inode_operations` | `lookup/create/mkdir/link/symlink/rename/unlink/rmdir/getattr/setattr/listxattr` | 完整目录命名空间入口 |
| `cryexts_file_operations` | `read_iter/write_iter/fallocate/fsync/llseek` | 普通文件数据和空间管理入口 |
| `cryexts_file_aops` | `readpage/writepage/writepages/write_begin/write_end/set_page_dirty` | page cache 与 writeback 接口 |
| `cryexts_file_inode_operations` | `getattr/setattr/listxattr` | 普通文件元数据操作 |
| `cryexts_symlink_inode_operations` | `get_link/getattr/listxattr` | 符号链接读取与属性 |
| `cryexts_xattr_handlers` | user xattr handler | VFS xattr namespace 接入 |

## 5. 跨模块 API

以下函数由 `cryexts.h` 导出。返回 `int/ssize_t` 的函数通常以 0/非负值表示成功，以 Linux 负 errno 表示失败；返回指针的 inode API 可能使用 `ERR_PTR`。

### 5.1 inode 与块映射

| API | 功能与主要输入/输出 |
| --- | --- |
| `cryexts_inode_blocks` | 取得 inode 私有映射状态；未初始化时可返回 NULL |
| `cryexts_init_inode_blocks` | 从磁盘 inode 解析 direct/indirect/extent/xattr/policy 到 runtime 状态 |
| `cryexts_free_inode_blocks` | 释放 runtime 映射缓存，不等于释放磁盘数据块 |
| `cryexts_inode_first_block` | 返回 inode 首个已映射 physical block，空文件为 0 |
| `cryexts_disk_inode_block_count` | 根据磁盘 inode 统计映射数据 block 数 |
| `cryexts_inode_block_count` | 根据 runtime inode 统计映射数据 block 数 |
| `cryexts_inode_block_sectors` | 转换为 VFS `i_blocks` 使用的 512-byte sector 数 |
| `cryexts_regular_file_max_size` | 返回格式理论最大普通文件字节数 |
| `cryexts_regular_file_max_size_for_inode` | 按当前 inode 映射格式返回实际最大字节数 |
| `cryexts_dir_block_count` | 返回目录有效 logical data block 数 |
| `cryexts_inode_block_at` | 把指定 logical index 查询为 physical block，不创建映射 |
| `cryexts_disk_inode_indirect_block` / `cryexts_inode_indirect_block` | 读取磁盘/runtime single-indirect block 位置 |
| `cryexts_inode_uses_extents` / `cryexts_disk_inode_uses_extents` | 判断 runtime/磁盘 inode 是否启用 extent |
| `cryexts_inode_xattr_block` | 返回 xattr root block |
| `cryexts_inode_policy_id` / `cryexts_set_inode_policy_id` | 获取或更新 inode encryption policy |
| `cryexts_set_inode_alloc_hint` | 设置后续数据块分配 locality hint |
| `cryexts_resolve_block` | 核心 logical-to-physical API；`create=true` 时可分配并建立映射 |
| `cryexts_free_blocks_from` | truncate 使用，释放 `keep_blocks` 之后的映射和块 |
| `cryexts_punch_hole_blocks` | 释放指定 logical block 闭区间并保留文件大小 |

### 5.2 几何、bitmap 与 allocator

| API | 功能 |
| --- | --- |
| `cryexts_inodes_per_block` | 计算一个 block 可容纳的磁盘 inode 数 |
| `cryexts_max_inodes` / `cryexts_blocks_count` / `cryexts_inodes_count` | 返回当前文件系统容量参数 |
| `cryexts_group_first_block` / `cryexts_group_blocks` | 返回组起点和实际长度 |
| `cryexts_group_inode_table_start` | 返回指定组 inode table 的起始 physical block |
| `cryexts_group_inode_table_blocks` | 返回指定组 inode table 的 block 数 |
| `cryexts_group_free_blocks` | 返回指定组的空闲 block 计数 |
| `cryexts_group_free_inodes` | 返回指定组的空闲 inode 计数 |
| `cryexts_has_block_groups` / `cryexts_prealloc_feature_enabled` | 查询 feature 开关 |
| `cryexts_bitmap_test` | 测试内存 bitmap 中指定 bit 是否置位 |
| `cryexts_bitmap_set` | 将内存 bitmap 中指定 bit 置位 |
| `cryexts_bitmap_clear` | 将内存 bitmap 中指定 bit 清零 |
| `cryexts_inode_bitmap_used` / `cryexts_block_bitmap_used` | 按全局 inode/block 查询对应组 bitmap |
| `cryexts_mark_bitmap_dirty` / `cryexts_mark_super_dirty` | 标记相关 buffer 待写回并维护 checksum |
| `cryexts_data_block_valid` | 验证 physical block 可作为数据块，排除越界与保留区 |
| `cryexts_alloc_inode_goal` | 优先从目标 group 分配 inode，返回 inode number |
| `cryexts_alloc_inode` | 无显式 goal 的 inode 分配包装 |
| `cryexts_free_inode` | 清 bitmap、更新组/全局计数 |
| `cryexts_alloc_block_goal` | 按 goal block/group 与 reservation 分配 physical block |
| `cryexts_alloc_block` | 无显式 goal 的 block 分配包装 |
| `cryexts_free_block` | 释放 physical block 并更新计数 |
| `cryexts_load_bitmaps` / `cryexts_unload_bitmaps` | mount/unmount 每组 bitmap 生命周期 |

### 5.3 Super、GDT 与 checksum

| API | 功能 |
| --- | --- |
| `cryexts_load_group_desc_table` / `cryexts_release_group_desc_table` | 装载/释放跨多个 block 的 GDT |
| `cryexts_gdt_prepare_write` | 更新 GDT checksum 并标记所有 GDT buffers dirty |
| `cryexts_update_super_checksum` / `cryexts_verify_super_checksum` | 生成/验证 super checksum |
| `cryexts_update_group_checksums` / `cryexts_verify_group_checksums` | 生成/验证每个 group descriptor checksum |
| `cryexts_dir_index_set_checksum` / `cryexts_dir_index_checksum_valid` | 写入/验证目录索引 checksum |
| `cryexts_policy_table_checksum_valid` | 验证 policy table block |
| `cryexts_extent_overflow_checksum` / `cryexts_extent_leaf_checksum` | 计算 extent metadata block checksum |
| `cryexts_metadata_csum_enabled` | 查询 metadata checksum feature |
| `cryexts_sync_metadata` | 将 super/GDT/bitmap 等脏元数据同步到设备 |

### 5.4 Journal 与 orphan

| API | 功能 |
| --- | --- |
| `cryexts_journal_checksum` | 计算 journal buffer checksum |
| `cryexts_journal_begin` | 开始单个 metadata 事务并建立活动 sequence |
| `cryexts_journal_record_block` | 将 physical home block 加入当前事务，去重并检查容量 |
| `cryexts_journal_record_bh` | 以 `buffer_head` 包装记录其 home block |
| `cryexts_journal_commit` | 写 descriptor/payload/commit，checkpoint，并更新 control/super 状态 |
| `cryexts_journal_abort` | 丢弃内存活动事务并恢复可继续工作的状态 |
| `cryexts_journal_replay` | mount-time 验证并恢复已提交未 checkpoint 的事务 |
| `cryexts_journal_uses_v2` | 查询 journal v2 feature |
| `cryexts_journal_needs_recovery` / `cryexts_super_set_recovery` | 查询或设置 needs-recovery 状态 |
| `cryexts_orphan_feature_enabled` | 查询 orphan list feature |
| `cryexts_orphan_set` / `cryexts_orphan_clear` | inode 加入/移出 orphan 链 |
| `cryexts_orphan_cleanup` | mount 时释放遗留 orphan inode 存储 |

### 5.5 加密与 policy I/O

| API | 功能 |
| --- | --- |
| `cryexts_salt_is_zero` | 检查 KDF salt 是否全零 |
| `cryexts_set_encryption_key` | 解析 mount options，派生并校验文件系统密钥 |
| `cryexts_policy_table_enabled` / `cryexts_policy_exists` | 查询 policy table 和 policy id |
| `cryexts_load_policy_table` / `cryexts_unload_policy_table` | 装载/释放 runtime policy 与 cipher |
| `cryexts_crypt_buffer` | 使用全局算法按 block/offset 原地对称变换；成功返回 0，加密状态或 Crypto API 失败返回负 errno |
| `cryexts_read_file_block` / `cryexts_write_file_block` | journal/xattr 使用的 metadata/raw block I/O，不执行文件数据加密 |
| `cryexts_read_inode_block` / `cryexts_write_inode_block` | inode-aware 数据 I/O，选择 policy 并透明解密/加密 |

### 5.6 inode 生命周期与 VFS 数据 API

| API | 功能 |
| --- | --- |
| `cryexts_get_disk_inode` | 定位 inode table 中的磁盘 inode，并返回持有的 buffer_head |
| `cryexts_write_inode_to_disk` | 把 VFS inode 和私有映射状态序列化并纳入 journal |
| `cryexts_iget` | 按 inode number 装载、校验并初始化 VFS inode |
| `cryexts_new_inode` | 分配并初始化新 inode，继承目录的 group/policy 语义 |
| `cryexts_release_inode_storage` | 释放 inode 的 data/extent/xattr 等磁盘资源 |
| `cryexts_evict_inode` | VFS eviction；link count 为 0 时完成最终回收 |
| `cryexts_validate_inode` | 校验磁盘 inode 类型、大小、块引用与 feature 一致性 |
| `cryexts_validate_dir_block` | 校验目录项布局、名称和引用 inode |
| `cryexts_mode_supported` | 限制支持的 inode 类型 |
| `cryexts_symlink_size_limit` | 返回符号链接数据上限 |
| `cryexts_read_iter` / `cryexts_write_iter` | 接入 generic page-cache iterator |
| `cryexts_setattr` / `cryexts_getattr` | VFS 属性更新和读取，包含 truncate 处理 |
| `cryexts_get_link` | 读取符号链接目标并注册释放回调 |
| `cryexts_free_xattr_storage` | 释放 xattr root/overflow blocks |
| `cryexts_listxattr` | 按 VFS 约定列举 xattr 名称 |

## 6. API 使用规则

- 修改磁盘元数据前先 `cryexts_journal_begin`，把所有 home blocks 纳入事务，成功后 commit，错误路径 abort。
- block/inode 分配释放只能走 `balloc.c` API，禁止直接改某一个 bitmap 而漏掉 group/super counter 与 checksum。
- 文件数据 I/O 优先走 inode-aware API；只有明确不带 inode policy 的场景才使用 file-system-level block API。
- logical block 映射统一走 `cryexts_resolve_block`，禁止在 file/dir 层复制 extent 查找逻辑。
- 所有 on-disk 数值必须做 endian 转换，reserved 字段可能承载 checksum，复用前必须搜索 `metadata.c`、mkfs、fsck 和 inspect 工具。
