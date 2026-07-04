# CRYEXTS v6.0 变更说明

## 1. 这一版做了什么

`v6.0` 不是把整套 journal 事务系统一次性做完，而是先把 `journal v2` 的磁盘格式、识别逻辑、检查工具和 smoke 验证链路搭起来。

这一版完成的是：

- 定义 `Version 6`
- 定义 `journal v2` on-disk layout
- `mkfs` 可以显式创建 `v6.0 journal v2` 镜像
- 内核 mount/replay 路径可以识别 `journal v2`
- `cryextsck` 可以校验 `journal v2` control / descriptor / commit
- 新增 inspect 工具和对应 smoke 脚本

这一版没有完成的是：

- 完整的 checkpoint 回收机制
- 多事务 ring buffer
- revoke 语义
- 所有写路径都重构成完整事务流水线

一句话说：

```text
v6.0 = journal v2 baseline
```

## 2. 修改了哪些代码

### 2.1 [cryexts_fs.h](/D:/Carl/cryptext4/cryexts/cryexts_fs.h:1)

新增：

- `CRYEXTS_VERSION_V6`
- `CRYEXTS_FEATURE_INCOMPAT_JOURNAL_V2`
- `CRYEXTS_JOURNAL_V2_*` 常量
- `struct cryexts_journal_v2_control`
- `struct cryexts_journal_v2_descriptor`
- `struct cryexts_journal_v2_commit`

这里定义了 `journal v2` 的三类核心块：

- control block
- descriptor block
- commit block

### 2.2 [cryexts.h](/D:/Carl/cryptext4/cryexts/cryexts.h:1)

新增：

- `sb_info->journal_v2`
- `cryexts_journal_uses_v2()`

作用是让运行时能明确区分：

- 旧 `journal v1`
- 新 `journal v2`

### 2.3 [tools/mkfs.cryexts.c](/D:/Carl/cryptext4/cryexts/tools/mkfs.cryexts.c:1)

新增：

- `-J` 选项

当前约定：

- 不加 `-J`，继续按原来方式创建 `v5.x / journal v1`
- 加 `-J`，创建 `version=6` 且带 `journal v2` feature 的镜像

`mkfs` 在 `journal area` 里会初始化：

- control block
- descriptor block 预清零
- commit block 预清零

其中 fresh image 的状态是：

- `active_sequence = 0`
- 没有 pending transaction

### 2.4 [super.c](/D:/Carl/cryptext4/cryexts/super.c:1)

主要改动：

- superblock 校验接受 `version 6`
- layout 校验接受 `JOURNAL_V2`
- 挂载时读取 `journal_v2` feature 到 runtime

也就是说 mount 阶段已经能判断：

- 这是旧 journal
- 还是新 journal v2

### 2.5 [journal.c](/D:/Carl/cryptext4/cryexts/journal.c:1)

这是这一版最核心的实现。

新增了 `journal v2` 的双栈处理逻辑：

- `cryexts_journal_v2_begin()`
- `cryexts_journal_v2_record_block()`
- `cryexts_journal_v2_commit()`
- `cryexts_journal_v2_abort()`
- `cryexts_journal_v2_replay()`

当前语义是：

1. `begin`
   把 superblock 标记为 `needs_recovery`
   写 journal control block
   记录新的 `active_sequence`

2. `record_block`
   把 home block 的旧内容拷贝到 payload 区
   更新 descriptor
   更新 commit

3. `replay`
   只有在：
   - control 合法
   - descriptor 合法
   - commit 合法
   - sequence 匹配

   的情况下，才把 payload 拷回 home block

4. `commit`
   把 control/descriptor/commit 重置回空闲状态
   清掉 `needs_recovery`

所以这版相比 `v4.2 ~ v5.x` 的单 header journal，多了清晰的事务边界：

```text
control -> descriptor -> payload -> commit
```

### 2.6 [tools/cryextsck.c](/D:/Carl/cryptext4/cryexts/tools/cryextsck.c:1)

新增能力：

- 识别 `version 6`
- 识别 `journal v2` feature
- 校验 control block
- 校验 descriptor block
- 校验 commit block
- 校验 sequence 是否一致
- 校验 home block 是否越界 / 是否回指 journal 区

当前 `--repair` 对 `journal v2` 仍然是保守策略：

- 先校验
- 不贸然自动重写 `v2` control/descriptor/commit

这是故意的，因为 `v6.0` 还处在 baseline 阶段，先要保证“报错可信”，再考虑“自动修复激进化”。

### 2.7 [tools/cryexts_journal_inspect.c](/D:/Carl/cryptext4/cryexts/tools/cryexts_journal_inspect.c:1)

这是新加的 inspect 工具。

它的用途是把 `journal v2` 的关键字段直接打印出来，包括：

- control header
- descriptor header
- commit header
- checksum
- expected checksum
- home block 列表

这样在 Ubuntu 上做 smoke 时，你能直接看到磁盘上实际布局是不是符合设计。

### 2.8 [scripts/smoke_v6_0_journal_layout.sh](/D:/Carl/cryptext4/cryexts/scripts/smoke_v6_0_journal_layout.sh:1)

这一版的 smoke 重点不是大规模 runtime crash/recovery，而是验证：

- `mkfs -J` 能否创建 `version 6`
- journal format 是否真的是 `v2`
- control / descriptor / commit block 是否存在
- 三个块的 checksum 是否正确
- fresh image 是否是 `active_sequence = 0`
- `cryextsck` 是否 clean

## 3. journal v2 的布局如何理解

当前固定布局可以理解成：

```text
journal_block + 0  -> control block
journal_block + 1  -> descriptor block
journal_block + 2  -> payload start
...
journal_end   - 1  -> commit block
```

其中：

- control 负责描述整个 journal 当前状态
- descriptor 负责描述“这一笔事务有哪些 home blocks”
- payload 存这些 home block 的镜像内容
- commit 负责声明“这一笔事务已经完整落盘”

## 4. 这一版到底测了什么

`smoke_v6_0_journal_layout.sh` 主要测三件事：

1. 格式能不能被 `mkfs` 正确写出来
2. inspect 工具能不能把 control/descriptor/commit 读出来
3. `cryextsck` 能不能把这份 `v6.0` 镜像识别为 clean

也就是说，这版更偏：

```text
layout / feature / validation baseline
```

而不是：

```text
完整事务压力测试版本
```

## 5. 这一版的边界

需要明确的是：

- `journal v2` 结构已经落盘
- `journal v2` replay 识别路径已经存在
- 但这还不是“ext4/JBD2 等级”的完整事务系统

目前仍然是：

- 单事务区域
- 固定 descriptor / commit block
- 没有真正的多事务循环复用
- 没有 checkpoint 回收推进

所以 `v6.1` 的重点会是：

- 把 `journal v2` 的运行时事务语义继续补完整
- 尤其是 commit / replay / checkpoint 的更完整边界

## 6. 一句话结论

`v6.0` 的意义不是“功能数量很多”，而是：

```text
第一次把 CRYEXTS 的 journal 从“单 header replay log”
推进到“有 control / descriptor / commit 边界的 transaction baseline”
```
