# CRYEXTS V5.0 代码修改说明

## 1. 版本定位

V5.0 不是 orphan list、directory index、policy table、extent tree 的真正运行时实现。

这一版的目标是：

```text
先把 Version 5 的 on-disk format、feature flags、mkfs、mount 校验、
cryextsck 校验、smoke 测试这条链路打通。
```

所以 V5.0 的核心关键词是：

```text
Version 5 format baseline
```

## 2. 修改了哪些代码

### 2.1 `cryexts_fs.h`

新增或调整了以下内容：

- 新增 `CRYEXTS_VERSION_V5`
- 默认版本宏 `CRYEXTS_VERSION` 提升到 `V5`
- 新增 V5 feature flags
  - `CRYEXTS_FEATURE_COMPAT_PREALLOC`
  - `CRYEXTS_FEATURE_INCOMPAT_DIR_INDEX`
  - `CRYEXTS_FEATURE_INCOMPAT_ORPHAN_LIST`
  - `CRYEXTS_FEATURE_INCOMPAT_POLICY_TABLE`
  - `CRYEXTS_FEATURE_INCOMPAT_EXTENT_TREE`
  - `CRYEXTS_FEATURE_RO_COMPAT_METADATA_CSUM`
  - `CRYEXTS_FEATURE_RO_COMPAT_LARGE_XATTR`
- 扩展 `struct cryexts_super_block`
  - `orphan_head`
  - `policy_table_block`
  - `dir_index_seed`
  - `metadata_csum_type`
  - `journal_sequence`
  - `fs_generation`

这一步的意义是：

- 让磁盘镜像有正式的 V5 superblock 入口
- 为后续 `V5.1 ~ V5.5` 的能力预留稳定字段

### 2.2 `super.c`

主要改动：

- 允许内核 mount 路径识别 `version == 5`
- 扩展支持的 `compat / incompat / ro_compat` 集合
- 校验 V5 新字段的基础合法性
  - `orphan_head`
  - `policy_table_block`
  - `metadata_csum_type`
  - `fs_generation`
- 增加 `cryexts_validate_v5_layout()`
  - 在 GDT 读完后做依赖 block-group / data-area 语义的精校验
- 保留旧版本约束
  - `V4/V3` 镜像若带了 V5 字段，仍会判错

这一步的意义是：

- 让 V5 镜像不只是“能 mkfs”
- 而是“内核真的知道自己在挂载一个 V5 格式”

### 2.3 `tools/mkfs.cryexts.c`

主要改动：

- usage 增加 V5 开关：
  - `-I` 打开 `DIR_INDEX`
  - `-O` 打开 `ORPHAN_LIST`
  - `-T` 打开 `POLICY_TABLE`
  - `-M` 打开 `METADATA_CSUM`
- `-P <policy_id>` 会隐式启用 `POLICY_TABLE`
- 初始化 V5 superblock 字段：
  - `orphan_head = 0`
  - `policy_table_block = 0`
  - `dir_index_seed`
  - `metadata_csum_type`
  - `journal_sequence = 0`
  - `fs_generation = 1`
- 输出信息增加：
  - `Dir index: enabled/disabled`
  - `Orphan list: enabled/disabled`
  - `Policy table: enabled/disabled`
  - `Metadata checksum: enabled/disabled`

这一步的意义是：

- 让 `mkfs.cryexts` 真正能创建带 V5 标记的镜像
- 让后续测试和肉眼审核更容易

### 2.4 `tools/cryextsck.c`

主要改动：

- 允许 `cryextsck` 识别 `version == 5`
- 扩展 V5 feature flags 支持集合
- 校验 V5 superblock 基础字段
- 增加 “早期粗校验 + 读完 group descriptors 后精校验” 的分层
  - 早期先检查范围和 flag/type 关系
  - 后期再检查 `policy_table_block` 是否真落在合法 data area

这一步的意义是：

- 让 `cryextsck` 对 V5 不再是“未知格式”
- 而是能真正理解 V5 baseline metadata

### 2.5 `scripts/smoke_v5_0_layout.sh`

新增 V5.0 smoke：

- 创建 V5 image
- 先跑一次 `cryextsck`
- 挂载 / 卸载
- 再跑一次 `cryextsck`

验证目标是：

```text
mkfs -> fsck -> mount -> umount -> fsck
```

### 2.6 `README.md`

补充了 `Version 5.0 Layout Smoke Test` 章节，方便后续直接按文档测试。

## 3. 修过的 warning

在 V5.0 收尾阶段，还顺手修了一批编译 warning：

### 3.1 `cryexts.h` / `xattr.c`

- 调整 `cryexts_xattr_handlers` 的声明类型
- 修正 `sb->s_xattr = cryexts_xattr_handlers` 的限定符不匹配 warning

### 3.2 `xattr.c`

原先三个函数把 `items[CRYEXTS_XATTR_MAX_ITEMS]` 放在栈上：

- `cryexts_user_xattr_get`
- `cryexts_user_xattr_set`
- `cryexts_listxattr`

后来改成了：

- 统一通过 `cryexts_alloc_xattr_items()` 做堆分配

这样一起消掉了：

- `missing braces around initializer`
- `frame size larger than 1024 bytes`

## 4. V5.0 实际实现了什么

V5.0 真正已经实现的是：

- V5 superblock 格式
- V5 feature flags
- `mkfs` 写入 V5 metadata
- kernel mount 识别并校验 V5 metadata
- `cryextsck` 识别并校验 V5 metadata
- V5 smoke 验证链路

## 5. V5.0 还没有实现什么

V5.0 还没有真正实现这些能力的运行时逻辑：

- orphan list 真正挂链和恢复
- directory index 真正查找路径
- policy table 真正进入多策略加密路径
- extent tree 真正替代当前映射逻辑
- metadata checksum 真正覆盖所有 metadata block

所以最准确的理解是：

```text
V5.0 = 先把 Version 5 的磁盘格式框架立住
```

而不是：

```text
Version 5 的高级功能已经全部生效
```

## 6. 为什么要先做这一版

因为后续这些版本都会依赖它：

- `V5.1 orphan list`
- `V5.2 extent tree`
- `V5.3 directory index`
- `V5.4 policy-aware encryption`
- `V5.5 metadata checksum`

如果没有先把 V5 的 on-disk 格式定下来，后面的每一版都要反复改 superblock / mkfs / fsck / mount 校验。

## 7. 下一步

V5.0 通过后，最自然的下一步就是：

```text
V5.1 orphan list
```

因为它最直接提升 crash recovery 质量，而且和现有 journal / replay 主线最接近。
