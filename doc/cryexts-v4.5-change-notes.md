# CRYEXTS V4.5 代码改动说明

## 1. 目标

V4.5 是第四阶段的收官版，重点不是再加一个大功能，而是把前面已经做出来的几条主线收稳：

- block groups
- metadata journal / replay
- extents regular file
- xattr / inode policy

这一版主要增强：

- journal header 自校验
- replay 前合法性检查
- `cryextsck --repair` 对 recovery 状态的低风险修补
- 更完整的 metadata flush

## 2. 本次改动点

### 2.1 `cryexts_fs.h`

新增：

- `CRYEXTS_JOURNAL_CHECKSUM_OFFSET`

作用：

- 给 kernel 工程和 user-space 工具一个统一的 checksum 跳过区定义

### 2.2 `journal.c`

新增核心逻辑：

- `cryexts_journal_checksum()`
- `cryexts_journal_block_is_internal()`
- `cryexts_journal_prepare_header()`
- `cryexts_journal_header_valid()`

增强点：

- `record_block` 写 header 前重算 checksum
- `commit` 清空 header 时也写 checksum
- replay 前检查：
  - header checksum
  - entry_count
  - trailing home block 是否为 0
  - home block 是否越界
  - home block 是否落到 journal 自己
- `journal_begin()` 设置 recovery 后立即做一次 metadata sync
- `journal_commit()` / replay 成功后做 metadata sync
- `journal_abort()` 在“事务尚未真正写入 journal”的场景下，会主动清 recovery 标记，避免留下空 recovery 状态

### 2.3 `super.c`

增强 `cryexts_sync_metadata()`：

- 刷 `gdt_bh`
- 刷 legacy bitmap
- 刷 per-group block bitmap
- 刷 per-group inode bitmap
- 最后 `sync_blockdev()`

这样 `fsync` / `sync_fs` / journal 清理的落盘边界更完整。

### 2.4 `tools/cryexts_journal_inject.c`

增强注入工具：

- 生成 recovery 镜像时同步写 journal header checksum

这样注入出来的是“结构正确但待恢复”的镜像，而不是旧版那种没有 checksum 的 header。

### 2.5 `tools/cryextsck.c`

新增：

- `journal_checksum()`
- `journal_block_is_internal()`
- `validate_journal_header()`
- `repair_recovery_state()`

增强检查：

- 检查 journal header checksum
- 检查 valid/entry_count 是否一致
- 检查 `home_blocks[]` 是否指向合法 data block
- 检查 `home_blocks[]` 是否写回到 journal 区
- 检查 unused trailing entries 是否清零

增强 repair：

- `needs_recovery` 还在，但 journal header 已无效时，可修成“空 header + 清 recovery”
- 空 journal 但 superblock 还挂 recovery 时，可清 recovery
- 非 valid transaction 的脏 checksum，可重建空 header

注意：

- 对于仍然标记 valid 的真实 journal transaction，`--repair` 不会自作主张清掉

## 3. 新增 smoke

新增：

- `scripts/smoke_v4_5_mvp.sh`

它覆盖三类验证：

1. 注入 recovery 镜像，确认 mount replay 仍然成功
2. replay 后继续验证 `xattr/policy` 还能正常工作
3. 手工制造“superblock 还挂 recovery，但 journal header 是坏的空 header”，确认：
   - 普通 `cryextsck` 会报错
   - `cryextsck --repair` 能修好

## 4. 这一版达到的效果

到 V4.5 为止，第四阶段的 MVP 可以更有把握地定义为：

```text
Version 4 MVP =
v4 superblock
+ block groups
+ metadata journal / mount replay
+ inline extents regular file
+ xattr / inode policy metadata
+ stronger recovery validation
+ stronger fsck repair / sync semantics
```

## 5. 你测试时重点观察什么

如果 Ubuntu 上运行 `./scripts/smoke_v4_5_mvp.sh` 成功，理想输出应包含：

- pre-replay fsck failed as expected
- mount-time replay succeeded
- broken journal header detected as expected
- `cryextsck --repair` 后再次 `cryextsck` clean
- v4.5 mvp smoke test passed

如果失败，最有价值的日志是：

- `make` 编译报错
- smoke 脚本完整输出
- `dmesg | tail -100`
