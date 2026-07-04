# CRYEXTS V4.0 盘格式与状态位升级

## 1. 这一阶段做了什么

V4.0 不做 block groups 本体，不做 journal replay 本体。

这一阶段只做一件很关键的事：

```text
把 superblock 从 V3 的“最小可用元数据”
升级成 V4 的“可演进元数据骨架”
```

所以 V4.0 的重点不是功能暴增，而是盘格式正式为后续版本打地基。

## 2. 当前新增的 superblock 信息

V4.0 新增并启用：

- `state`
- `mount_count`
- `max_mount_count`
- `last_mount_time`
- `last_write_time`
- `last_check_time`
- `uuid`
- `volume_name`
- `group_count`
- `blocks_per_group`
- `inodes_per_group`
- `default_encryption_policy`
- `journal_block`
- `journal_blocks`

当前它们的定位是：

- 有些字段已经开始写入和校验
- 有些字段还是“先预留格式位”

## 3. 当前 V4.0 的真实语义

### 3.1 仍然是单组布局

虽然加入了 group 相关字段，但当前仍然只有：

```text
group_count = 1
```

也就是说，V4.0 只是先把“以后支持 block groups”需要的字段写进 superblock，
并没有真正实现 per-group allocator。

### 3.2 仍然没有 journal 本体

V4.0 里：

- `journal_block = 0`
- `journal_blocks = 0`

表示：

```text
journal 布局字段已经预留
但 journal 功能本身还没启用
```

### 3.3 state 字段开始进入主流程

当前推荐理解：

- mount 前：image 通常是 `clean`
- mount 后：内核会把它从 `clean` 切走
- umount 时：重新标成 `clean`

这一步非常重要，因为后续 V4.2 的 recovery 流程就要建立在它上面。

## 4. Feature flags 在 V4.0 的意义

V4.0 让 `features_compat / incompat / ro_compat` 进入正式规则，而不再只是摆设。

当前实现上：

- `compat = 0`
- `ro_compat = 0`
- `incompat` 至少包含 `SINGLE_INDIRECT`

也就是说，V4.0 先把：

```text
“挂载时必须做 feature 协商”
```

这件事立起来。

## 5. 为什么 V4.0 值得单独做

因为后续这些能力都要依赖新盘格式：

- V4.1 block groups
- V4.2 journal
- V4.3 extent
- V4.4 xattr / policy

如果 superblock 还是 V3 的最小字段集合，后面每一步都会返工。

所以 V4.0 本质上是在做：

```text
盘格式升级
而不是功能升级
```

## 6. 当前验收重点

V4.0 主要验收：

- `mkfs.cryexts` 能创建 Version 4 image
- kernel 能 mount Version 4 image
- `cryextsck` 能识别并检查 V4.0 新字段
- `state / mount_count / volume_name / uuid` 等元数据写入格式正常
- 旧的 V3 image 不被 V4.0 错误拒绝

## 7. 当前边界

V4.0 还没有：

- 真正的 block groups
- journal transaction
- replay
- extent
- xattr

所以正确理解是：

```text
V4.0 = Version 4 的盘格式起点
```

而不是：

```text
Version 4 的完整功能版
```
