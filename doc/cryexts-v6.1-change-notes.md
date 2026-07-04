# CRYEXTS v6.1 变更说明

## 1. 这一版解决了什么问题

`v6.0` 已经把 `journal v2` 的磁盘布局搭起来了：

- `control`
- `descriptor`
- `payload`
- `commit`

但是 `v6.0` 还更像“结构就位”，不是“事务边界就位”。

`v6.1` 的目标是把它推进到：

```text
单事务 journal v2 真正可工作的 MVP
```

这一版重点补了三件事：

1. `commit` 阶段先把 home metadata 尽量落稳，再清空 journal 状态
2. `tail_sequence / checkpoint_sequence` 不再只是占位字段，而是有明确 idle 语义
3. `replay` / `fsck` / `inspect` 都开始检查 journal v2 的状态窗口是否合理

一句话：

```text
v6.0 = journal v2 layout baseline
v6.1 = journal v2 single-transaction commit/replay/checkpoint MVP
```

## 2. 修改了哪些文件

### 2.1 [cryexts.h](/D:/Carl/cryptext4/cryexts/cryexts.h:1)

给 `struct cryexts_sb_info` 新增了 4 个 runtime journal 状态字段：

- `journal_last_sequence`
- `journal_active_sequence`
- `journal_tail_sequence`
- `journal_checkpoint_sequence`

这些字段不写入新的 on-disk 结构体，只是在内核运行时缓存 `control block` 里对应的语义状态，方便：

- begin 时建立 active transaction
- commit 后推进 checkpoint/tail
- replay 时把控制块状态读回 runtime

### 2.2 [super.c](/D:/Carl/cryptext4/cryexts/super.c:1)

挂载时补上了 runtime journal 状态初始化：

- 从 superblock 读取 `journal_sequence`
- 初始化 `last_sequence`
- 初始化 idle 状态下的 `tail_sequence`
- 初始化 idle 状态下的 `checkpoint_sequence`

这一步的作用是让 `v6.1` 在 mount 后立即拥有一套一致的 journal runtime 视图。

### 2.3 [journal.c](/D:/Carl/cryptext4/cryexts/journal.c:1)

这是 `v6.1` 的核心修改。

新增/调整的关键函数：

- `cryexts_journal_v2_set_state()`
- `cryexts_journal_v2_sequence_state_valid()`
- `cryexts_journal_v2_prepare_control()` 参数扩展
- `cryexts_journal_v2_begin()`
- `cryexts_journal_v2_commit()`
- `cryexts_journal_v2_replay()`
- `cryexts_journal_v2_reset_state()`

其中最关键的行为变化是：

#### `cryexts_journal_v2_commit()`

旧逻辑更接近：

```text
先把 journal control/descriptor/commit 清空
再 sync metadata
```

这会带来一个边界问题：

- 如果 journal 先被清空
- 但 home metadata 还没真正落稳

那么崩溃后就既没有 journal 可回放，也不一定有完整 home metadata。

`v6.1` 改成：

```text
先 sync home metadata
再 reset journal 到 checkpoint 完成状态
再清理 recovery 标志
再 sync metadata
```

这更符合当前这套实现真实的 `undo-style metadata journal` 语义。

#### `cryexts_journal_v2_replay()`

新增了对 control 状态窗口的校验：

- `tail_sequence <= checkpoint_sequence`
- `checkpoint_sequence <= last_sequence`
- idle 状态下：
  - `active_sequence == 0`
  - `tail_sequence == checkpoint_sequence == last_sequence`

如果这些关系不成立，直接判为 journal 状态损坏。

### 2.4 [tools/mkfs.cryexts.c](/D:/Carl/cryptext4/cryexts/tools/mkfs.cryexts.c:1)

完善了 fresh image 的 `journal v2 control` 初始化：

- `last_sequence = 0`
- `active_sequence = 0`
- `tail_sequence = 0`
- `checkpoint_sequence = 0`

这样 `v6.1` 的 fresh image 从一开始就是一个“完全 checkpoint 完成”的空 journal。

### 2.5 [tools/cryexts_journal_inspect.c](/D:/Carl/cryptext4/cryexts/tools/cryexts_journal_inspect.c:1)

新增两个便于人工检查的输出字段：

- `control.idle`
- `control.checkpoint_complete`

它们的目的不是替代 `fsck`，而是为了让 smoke 脚本和人工审阅更快判断：

- 当前 journal 是否处于 idle
- 当前 checkpoint/tail 是否已经推进到最后

### 2.6 [tools/cryextsck.c](/D:/Carl/cryptext4/cryexts/tools/cryextsck.c:1)

增加了 `journal v2 control` 的状态窗口校验：

- `tail_sequence` 不能超过 `checkpoint_sequence`
- `checkpoint_sequence` 不能超过 `last_sequence`
- 如果 journal 处于 idle，那么必须满足：
  - `tail_sequence == checkpoint_sequence == last_sequence`

这意味着 `v6.1` 开始，`cryextsck` 不只检查 layout 和 checksum，也开始检查 journal control 的事务语义是否自洽。

### 2.7 [tools/cryexts_journal_v2_inject.c](/D:/Carl/cryptext4/cryexts/tools/cryexts_journal_v2_inject.c:1)

这是 `v6.1` 新增的测试注入工具。

它会人为构造一笔“待 mount replay 的 v2 事务”：

- 把 root dir block 的旧内容复制到 journal payload
- 写 control / descriptor / commit
- 把 superblock 标成 `needs_recovery`
- 故意破坏 home block

这样下次 mount 时，内核就必须依赖 `journal v2 replay` 恢复 home block。

### 2.8 [scripts/smoke_v6_1_journal_transaction.sh](/D:/Carl/cryptext4/cryexts/scripts/smoke_v6_1_journal_transaction.sh:1)

这是 `v6.1` 的 smoke 脚本。

它验证三段状态：

1. fresh image
   - `active_sequence = 0`
   - `checkpoint_complete = 1`
2. 正常 mount + 写入 + umount 之后
   - journal 重新回到 idle/checkpoint 完成状态
3. 人工注入待恢复事务，再 mount replay 之后
   - journal 再次回到 idle/checkpoint 完成状态
   - `cryextsck` clean

## 3. 这版的核心逻辑怎么理解

这一版最重要的不是“多了几个字段”，而是补清楚了：

```text
什么时候算 active transaction
什么时候算 committed but not yet reset
什么时候算 checkpoint complete
```

当前 `v6.1` 的约定是：

- `active_sequence != 0`
  表示 journal 里有一笔待处理事务
- `active_sequence == 0`
  表示当前 journal idle
- idle 时：
  - `tail_sequence == checkpoint_sequence == last_sequence`
  这表示 journal 已经完全收敛，没有历史事务需要继续关注

因为当前还是单事务模型，所以：

- 不存在多事务 ring buffer
- 不存在真正的 tail 回收队列
- 不存在已提交未 checkpoint 的事务队列

所以 `tail/checkpoint` 在 `v6.1` 里的意义是：

```text
把 control block 的事务完成状态表达清楚
并为以后多事务 journal 预留一致语义
```

## 4. 这版没有做什么

`v6.1` 仍然没有做：

- 多事务循环 journal
- checkpoint 列表 / checkpoint 队列
- revoke record
- journal 空间复用策略
- descriptor block 链式扩展

所以这一版仍然是：

```text
single-transaction journal v2
```

只是它现在的：

- commit 边界
- replay 判定
- checkpoint 完成状态

都比 `v6.0` 明确得多。

## 5. 一句话结论

`v6.1` 的价值不是“把 journal 一次做成 ext4 JBD2”，而是：

```text
先把当前这套 journal v2 的单事务语义补完整，
让 commit / replay / checkpoint_complete 这三件事真正说得通、验得清、测得到。
```
