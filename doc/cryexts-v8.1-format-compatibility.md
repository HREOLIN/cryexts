# CRYEXTS v8.1 设计说明

## 1. v8.1 的定位

`v8.0` 解决的是：

```text
把仓库入口整理成一个像样的开源项目入口
```

而 `v8.1` 要解决的是另一件同样重要的事：

```text
把当前文件系统格式边界、feature 边界、兼容边界正式写清楚
```

一句话定义：

```text
v8.1 = CRYEXTS 的格式边界与兼容性基线版本
```

## 2. 为什么现在要先做 v8.1

到当前为止，CRYEXTS 已经演进过多个磁盘格式阶段：

- V2
- V3
- V4
- V5
- V6

同时又经历了：

- `Version 6 MVP`
- `Version 7` 的 multi-GDT / raw-device / USB demo 工程化

这会带来一个很现实的问题：

### 2.1 “版本号”和“项目阶段号”已经不是一回事

例如：

- `v7.0 ~ v7.3` 讲的是项目阶段
- 但 `mkfs` / `superblock` 里的 on-disk `version` 仍然可能是 `6`

也就是说：

```text
Version 7 不是在引入 Version 7 on-disk format
而是在围绕 Version 6 format 做工程化和扩展能力补齐
```

如果这个边界不写清楚，外部读者会非常容易误解。

### 2.2 feature flag 已经很多，但还没有统一口径

当前 `cryexts_fs.h` 已经定义了：

- `features_compat`
- `features_incompat`
- `features_ro_compat`

里面已经包含：

- journal
- prealloc
- block groups
- extents
- xattr
- encryption policy
- dir index
- orphan list
- policy table
- extent tree
- journal v2
- metadata checksum
- large xattr

代码里已经支持它们，
但仓库外部还没有一份“统一声明”说明：

- 哪些是稳定主线
- 哪些是历史兼容
- 哪些是默认推荐
- 哪些仍然是实验边界

### 2.3 现在已经到了需要“承诺边界”的阶段

如果只是在代码里支持，而不在文档里承诺边界，
项目永远都停留在：

```text
作者知道怎么用
```

而不是：

```text
别人知道什么是被支持的，什么只是历史遗留或实验能力
```

所以 `v8.1` 的目标不是加新 feature，
而是把现在已经存在的格式能力正式收口。

## 3. v8.1 主目标

`v8.1` 建议只做这一件事：

```text
建立 CRYEXTS 当前格式主线、feature 主线和兼容主线的公开声明
```

拆开看，包含四部分：

1. 定义当前推荐 on-disk format baseline
2. 定义历史版本支持范围
3. 定义 feature flag 语义边界
4. 定义后续哪些字段/位不应该再随便改

## 4. 当前代码里已经能确认的现实情况

这一节不是“设计愿景”，而是基于当前代码的现实状态。

## 4.1 当前可识别的 on-disk version

从 [cryexts_fs.h](/D:/Carl/cryptext4/cryexts/cryexts_fs.h:12) 和
[super.c](/D:/Carl/cryptext4/cryexts/super.c:112) /
[tools/cryextsck.c](/D:/Carl/cryptext4/cryexts/tools/cryextsck.c:492) 可以看出：

当前代码明确识别：

- `CRYEXTS_VERSION_V3`
- `CRYEXTS_VERSION_V4`
- `CRYEXTS_VERSION_V5`
- `CRYEXTS_VERSION_V6`

也就是说，至少在当前实现里：

```text
V3 ~ V6 都属于“代码可识别”的格式版本
```

## 4.2 当前默认版本宏

[cryexts_fs.h](/D:/Carl/cryptext4/cryexts/cryexts_fs.h:17) 当前定义：

```c
#define CRYEXTS_VERSION CRYEXTS_VERSION_V5
```

这意味着：

- `mkfs` 默认版本基线仍然是 `V5`
- 但 `mkfs` 可以在特定选项下生成 `V6`

从 [tools/mkfs.cryexts.c](/D:/Carl/cryptext4/cryexts/tools/mkfs.cryexts.c:439) 可见，
某些新能力会把 `fs_version` 提升到 `V6`。

这说明当前仓库已经存在一个需要被文档解释清楚的现实：

```text
代码支持 V6，
但默认版本宏和推荐部署格式线未必已经完全统一
```

这正是 `v8.1` 要收口的重点。

## 4.3 当前 feature flag 集合

从 [cryexts_fs.h](/D:/Carl/cryptext4/cryexts/cryexts_fs.h:48) 可以整理出：

### `features_compat`

- `CRYEXTS_FEATURE_COMPAT_HAS_JOURNAL`
- `CRYEXTS_FEATURE_COMPAT_PREALLOC`

### `features_incompat`

- `CRYEXTS_FEATURE_INCOMPAT_SINGLE_INDIRECT`
- `CRYEXTS_FEATURE_INCOMPAT_BLOCK_GROUPS`
- `CRYEXTS_FEATURE_INCOMPAT_NEEDS_RECOVERY`
- `CRYEXTS_FEATURE_INCOMPAT_EXTENTS`
- `CRYEXTS_FEATURE_INCOMPAT_XATTR`
- `CRYEXTS_FEATURE_INCOMPAT_ENCRYPTION_POLICY`
- `CRYEXTS_FEATURE_INCOMPAT_DIR_INDEX`
- `CRYEXTS_FEATURE_INCOMPAT_ORPHAN_LIST`
- `CRYEXTS_FEATURE_INCOMPAT_POLICY_TABLE`
- `CRYEXTS_FEATURE_INCOMPAT_EXTENT_TREE`
- `CRYEXTS_FEATURE_INCOMPAT_JOURNAL_V2`

### `features_ro_compat`

- `CRYEXTS_FEATURE_RO_COMPAT_METADATA_CSUM`
- `CRYEXTS_FEATURE_RO_COMPAT_LARGE_XATTR`

同时，`super.c` 和 `cryextsck.c` 都已经对这些已知位做白名单校验。

这意味着：

```text
超出白名单的 feature 位，
当前实现就不应被认为是受支持格式
```

## 4.4 当前已经进入主线的关键格式字段

下列字段已经不是“随便预留的保留位”，而是实际参与运行时语义的字段：

### `struct cryexts_super_block`

来自 [cryexts_fs.h](/D:/Carl/cryptext4/cryexts/cryexts_fs.h:153) 之后。

#### `features_compat`

- 功能：兼容特性位集合
- 当前用途：journal、prealloc

#### `features_incompat`

- 功能：不兼容特性位集合
- 当前用途：block groups、extent、xattr、dir index、policy table、journal v2 等

#### `features_ro_compat`

- 功能：只读兼容特性位集合
- 当前用途：metadata checksum、large xattr

#### `default_encryption_policy`

- 功能：默认 policy id
- 当前用途：policy-aware encryption 默认策略入口

#### `group_desc_table_start`

- 功能：GDT 起始块号
- 当前用途：multi-GDT 主线读取入口

#### `group_desc_table_blocks`

- 功能：完整 GDT 区域占用的块数
- 当前用途：mount / fsck 的 multi-GDT 范围依据

#### `policy_table_block`

- 功能：policy table 所在块号
- 当前用途：policy-aware encryption 元数据入口

这些字段在 `v8.1` 之后应该被正式视为：

```text
已承诺的主线格式字段
```

而不是后续还能随便挪语义的临时字段。

## 5. v8.1 要正式定义的三条边界

## 5.1 版本边界

`v8.1` 需要把版本边界定义成三层：

### 第一层：代码可识别版本

当前建议明确写成：

- `V3`
- `V4`
- `V5`
- `V6`

### 第二层：历史兼容版本

当前建议：

- `V3`
- `V4`

归为：

```text
历史兼容线
```

含义是：

- 代码仍然尽量识别
- 可以保留 mount / fsck 基本支持
- 但不再作为新镜像推荐格式

### 第三层：推荐部署版本

当前建议：

- `V6`

归为：

```text
当前推荐格式主线
```

原因很简单：

- journal v2 在 `V6`
- large xattr 也是 `V6` 时代正式进入
- `Version 7` 的工程化成果本质上也是围绕 `V6` 格式主线展开

也就是说：

```text
v8.1 应把“推荐镜像格式线”正式收口到 V6
```

## 5.2 feature 边界

`v8.1` 需要把 feature 分成三类：

### A 类：推荐主线 feature

建议纳入当前主线的：

- `BLOCK_GROUPS`
- `EXTENTS`
- `XATTR`
- `ENCRYPTION_POLICY`
- `DIR_INDEX`
- `POLICY_TABLE`
- `EXTENT_TREE`
- `JOURNAL_V2`
- `METADATA_CSUM`
- `LARGE_XATTR`
- `HAS_JOURNAL`
- `PREALLOC`

这些 feature 应被视为：

```text
当前推荐格式组合的一部分
```

### B 类：历史兼容 feature

建议保留但不再推荐新建镜像依赖的：

- `SINGLE_INDIRECT`

它不是错误能力，
但已经不再是推荐新格式路线的核心。

### C 类：状态型 feature

建议明确归类的：

- `NEEDS_RECOVERY`

这个位不是“长期格式能力”，
而是：

```text
运行时恢复状态位
```

这个语义必须写清楚，否则很容易和“持久结构能力”混在一起。

## 5.3 兼容性边界

`v8.1` 需要把兼容性明确拆成三种问题。

### 问题 1：新 `mkfs` 创建的镜像，应该用哪条格式线

建议答案：

```text
默认推荐创建 V6 主线镜像
```

### 问题 2：新代码是否继续识别旧镜像

建议答案：

```text
继续识别 V3~V6，
但只把 V6 作为推荐部署格式
```

### 问题 3：哪些 feature 组合不承诺长期兼容

建议答案：

- 历史过渡型 feature 组合可以保留读取支持
- 但后续公开文档只承诺主线组合
- 不要再对所有历史中间态做无限承诺

这一步非常重要，因为它决定了：

```text
项目是否有能力从“什么都支持一点”
收口成“一条可维护的格式主线”
```

## 6. v8.1 建议产出的文档与资料

`v8.1` 不需要大量写新代码，
但应该至少产出下面几类说明。

## 6.1 格式兼容矩阵

建议新增一份明确矩阵，至少说明：

- 哪些 on-disk version 可识别
- 哪些 version 推荐用于新镜像
- 哪些 feature 是推荐主线
- 哪些 feature 仅为历史兼容

## 6.2 推荐格式 profile

建议明确一个“当前推荐 profile”，例如：

```text
V6
+ block groups
+ extents / extent tree
+ xattr
+ encryption policy / policy table
+ dir index
+ journal v2
+ metadata checksum
+ large xattr
```

这能直接告诉外部读者：

```text
今天如果你要创建一个新 CRYEXTS 镜像，
推荐长什么样
```

## 6.3 不推荐组合说明

建议明确写出：

- 哪些旧组合只做历史保留
- 哪些组合不建议用于新镜像
- 哪些路径只适合作为升级/回归测试样本

## 7. v8.1 对代码层的设计要求

这版主要是设计和边界声明，
但它会对后续代码调整提出明确方向。

## 7.1 `mkfs` 的推荐输出线要统一

后续建议方向：

- 让 `mkfs` 的默认推荐输出和文档主线一致
- 如果默认仍不是主线格式，要么改默认，要么把 profile 入口写死

也就是说：

```text
文档说推荐 V6，
代码就不该继续让默认入口长期停在 V5 而不解释
```

## 7.2 mount / fsck 的支持口径要一致

后续建议方向：

- `super.c` 支持的 version / feature 白名单
- `cryextsck` 支持的 version / feature 白名单

这两处要长期保持一致。

否则就会出现：

```text
内核说能挂
fsck 说不认识
```

或者反过来。

## 7.3 feature 位语义不再随便漂移

后续建议方向：

- 已经公开承诺的 feature 位不再改变语义
- 新语义优先新增新位，而不是复用旧位硬改含义

这是 `v8.1` 最重要的长期约束之一。

## 8. v8.1 推荐实施顺序

建议按最小顺序推进：

1. 先写格式兼容文档
2. 再整理 feature 矩阵
3. 再统一 `README / ROADMAP` 里的版本口径
4. 最后再考虑是否调整 `mkfs` 默认 profile

这个顺序的原因是：

- 先说清楚边界
- 再决定代码默认行为是否要跟着改

这样风险最小。

## 9. v8.1 验收标准

`v8.1` 完成的最小标准建议定义为：

### 9.1 外部读者能分清两种“版本”

即：

- 项目阶段版本：`v7.x / v8.x`
- on-disk format version：`V3 / V4 / V5 / V6`

不能再混在一起。

### 9.2 外部读者知道当前推荐格式线

必须明确知道：

```text
当前推荐新镜像格式主线是 V6
```

### 9.3 feature 位有公开边界

外部读者能知道：

- 哪些是主线
- 哪些是历史兼容
- 哪些是状态位

### 9.4 后续代码默认行为有统一方向

即使这一版还不改默认值，
至少文档已经把目标方向写清楚：

```text
默认创建格式最终应与推荐格式主线一致
```

## 10. 一句话总结

如果说 `v8.0` 解决的是：

```text
让仓库像一个公开项目
```

那么 `v8.1` 解决的是：

```text
让这个公开项目第一次正式承诺：
什么格式是主线，什么兼容是历史，什么 feature 是被支持的
```
