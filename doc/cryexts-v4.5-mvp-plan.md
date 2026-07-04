# CRYEXTS V4.5 MVP 收官计划

## 1. 阶段定位

V4.5 不再继续扩新的功能面，而是把 Version 4 已经具备的能力收口成一个更稳的
MVP：

```text
更强的 journal 结构
+ 更好的 sync/fsync 语义
+ 更完整的 fsck repair / recovery 加固
```

也就是说，V4.5 的重点不是：

- 再加一个大功能

而是：

- 让 V4.0 ~ V4.4 的能力更稳、更可测、更接近一个“第四阶段完成版”

## 2. 为什么 V4.5 很重要

到 V4.4 为止，CRYEXTS 已经具备：

- Version 4 superblock / feature flags
- block groups
- minimal metadata journal + replay
- inline extents regular file
- xattr / inode policy metadata hooks

但还缺一个“收官层”：

- journal header 太弱
- sync 语义偏粗
- fsck 对 recovery 状态的 repair 还不够完整
- journal 损坏时的诊断和处理还不够清晰

V4.5 就是把这些补成一个完整的第四阶段 MVP。

## 3. 推荐实现范围

V4.5 建议只做三条主线：

### 3.1 journal header 加固

目标：

- journal header 自身有更强自描述能力
- mount replay 前能做更严格校验

建议：

- 增加 header checksum
- 增加 header version / reserved consistency
- replay 时校验 `home_blocks[]` 不越界、不落到 journal 自己内部
- replay 时拒绝恢复明显非法的目标块

### 3.2 sync / fsync 语义补强

目标：

- `fsync` 不只是把 inode 写回
- 还要尽量把相关 metadata 路径刷到稳定状态

建议：

- `sync_metadata()` 覆盖：
  - superblock
  - GDT
  - group bitmaps
  - single-group bitmap
  - inode bitmap
  - journal header
- `journal_commit()` 后尽量把关键 header / super 状态落稳
- 让显式 `fsync` 和 clean unmount 的行为更可预期

### 3.3 fsck repair / recovery 加固

目标：

- `cryextsck --repair` 不只是修 bitmap / free count
- 还要能修 recovery 状态与 journal header 的低风险不一致

建议：

- 发现 `needs_recovery` 但 journal header 空时，可清理 recovery 状态
- 发现 journal header 明显损坏时，给出更明确报告
- 在 `--repair` 下支持低风险清空无效 journal header
- repair 后同步清理：
  - `superblock.state`
  - `features_incompat.NEEDS_RECOVERY`

## 4. 不建议在 V4.5 做的事

为了把第四阶段收稳，V4.5 不建议再做：

- extent tree
- 多块 xattr storage
- 多策略加密真正生效
- directory index
- full checkpoint journal
- data journaling

这些都适合 Version 5 以后再展开。

## 5. 预期交付

V4.5 完成后，第四阶段 MVP 应该具备：

```text
v4 superblock
+ block groups
+ metadata journal / replay
+ extent regular file
+ xattr / inode policy metadata
+ stronger sync semantics
+ stronger fsck repair / recovery hardening
```

这时我们就可以比较有底气地说：

```text
Version 4 MVP 完成
```

## 6. 建议拆分

### V4.5.0

- journal header checksum / sanity 校验
- replay 前更严格合法性检查

### V4.5.1

- `sync_metadata()` 覆盖更多 metadata buffer
- `journal_commit()` 落稳关键状态

### V4.5.2

- `cryextsck --repair` 修 recovery / journal header 低风险不一致
- 新增对应 smoke

## 7. 中文一句话总结

V4.5 的任务不是“再长功能”，而是：

```text
把前面做出来的能力打磨成一个真正可交付的第四阶段 MVP
```
