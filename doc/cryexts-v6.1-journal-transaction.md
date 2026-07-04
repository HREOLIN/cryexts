# CRYEXTS v6.1 Journal Transaction 说明

## 1. 文档目标

这份文档专门讲 `v6.1` 的 journal transaction 语义。

重点不是重新解释 `v6.0` 的 layout，而是回答：

- `v6.1` 到底补了哪些事务边界
- 新增/使用到的结构体字段分别表示什么
- 新增/关键函数分别负责什么
- mount-time replay 现在到底怎么走

## 2. 先说结论

`v6.1` 仍然是：

```text
单事务 journal v2
```

但它已经把以下三个阶段补清楚了：

1. `begin`
   把 journal 标成 active
2. `commit`
   先尽量把 home metadata 落盘，再把 journal 收敛成 idle/checkpoint 完成状态
3. `replay`
   只对完整合法的 active transaction 做恢复，恢复完成后再进入 idle/checkpoint 完成状态

## 3. 相关结构体字段说明

### 3.1 `struct cryexts_journal_v2_control`

定义位置：
[cryexts_fs.h](/D:/Carl/cryptext4/cryexts/cryexts_fs.h:204)

这个结构体描述的是：

```text
journal 整体状态
```

字段说明如下：

- `magic`
  固定魔数，标识这是 `journal v2 control block`

- `layout_version`
  control block 的布局版本，当前为 `2`

- `block_type`
  block 类型，当前值应为 `CRYEXTS_JOURNAL_V2_BLOCK_CONTROL`

- `flags`
  control 自身的状态位
  当前主要使用 `ACTIVE` 语义

- `features`
  journal v2 支持的功能位
  当前 `v6.1` 仍然只用 baseline

- `checksum`
  control block 自身校验

- `reserved0`
  预留

- `last_sequence`
  最近一次已经分配出去的事务序号上界
  可以理解成：
  “journal 历史上已经走到哪一笔了”

- `active_sequence`
  当前正在等待 commit/replay 处理的事务序号
  如果是 `0`，表示当前没有 active transaction

- `tail_sequence`
  当前 journal 可认为已经回收/推进到的位置
  在 `v6.1` 单事务模型里，idle 时它会等于 `last_sequence`

- `checkpoint_sequence`
  当前已经 checkpoint 完成的位置
  在 `v6.1` 单事务模型里，idle 时它也会等于 `last_sequence`

- `descriptor_block`
  descriptor block 的物理块号

- `payload_start`
  payload 区起始物理块号

- `payload_blocks`
  payload 区总共占多少物理块
  注意它表示的是：
  “payload 区容量”
  不是“当前事务写了多少条”

- `commit_block`
  commit block 的物理块号

- `reserved[56]`
  预留给未来版本扩展

### 3.2 `struct cryexts_sb_info`

定义位置：
[cryexts.h](/D:/Carl/cryptext4/cryexts/cryexts.h:24)

`v6.1` 新增了 4 个 runtime 字段：

- `journal_last_sequence`
  runtime 里缓存 control 的 `last_sequence`

- `journal_active_sequence`
  runtime 里缓存 control 的 `active_sequence`

- `journal_tail_sequence`
  runtime 里缓存 control 的 `tail_sequence`

- `journal_checkpoint_sequence`
  runtime 里缓存 control 的 `checkpoint_sequence`

这 4 个字段的作用是：

- begin 时建立一份内核态 journal 状态
- replay 时把 on-disk control 状态读回 runtime
- commit/reset 时同步更新 runtime 状态

它们不是新增磁盘结构，只是内核态缓存。

## 4. `v6.1` 的关键状态约定

### 4.1 idle 状态

如果 journal 是空闲的，`v6.1` 约定：

```text
active_sequence = 0
tail_sequence = checkpoint_sequence = last_sequence
```

这表示：

- 没有待恢复事务
- 没有“提交了但还没 checkpoint”的残留状态

### 4.2 active transaction 状态

当 `cryexts_journal_v2_begin()` 开始一笔事务后：

```text
last_sequence = 上一笔完成事务
active_sequence = 新事务序号
tail_sequence = 上一笔完成事务
checkpoint_sequence = 上一笔完成事务
```

含义是：

- 新事务已经开始
- 但旧的 checkpoint 边界还停留在上一笔完成位置

### 4.3 replay 完成后的状态

mount-time replay 成功以后，会进入：

```text
last_sequence = active_sequence
active_sequence = 0
tail_sequence = last_sequence
checkpoint_sequence = last_sequence
```

也就是：

```text
恢复完成，journal 收敛回 idle
```

## 5. 关键函数说明

### 5.1 `cryexts_journal_v2_set_state()`

位置：
[journal.c](/D:/Carl/cryptext4/cryexts/journal.c:192)

功能：

- 把 `last / active / tail / checkpoint` 四个 runtime 状态一次性写入 `sb_info`

输入：

- `last_sequence`
- `active_sequence`
- `tail_sequence`
- `checkpoint_sequence`

输出：

- 无返回值

作用点：

- begin
- reset_state
- replay 读 control 后

### 5.2 `cryexts_journal_v2_prepare_control()`

位置：
[journal.c](/D:/Carl/cryptext4/cryexts/journal.c:204)

功能：

- 组装一块新的 control block 内存镜像

输入：

- `flags`
- `active_sequence`
- `last_sequence`
- `tail_sequence`
- `checkpoint_sequence`

输出：

- 写满 `buf` 指向的 4KB block

作用点：

- begin 时写 active control
- reset_state 时写 idle control

### 5.3 `cryexts_journal_v2_sequence_state_valid()`

位置：
[journal.c](/D:/Carl/cryptext4/cryexts/journal.c:360)

功能：

- 检查 control block 的四个 sequence 字段关系是否自洽

当前校验规则：

- `tail <= checkpoint`
- `checkpoint <= last`
- 如果 idle，则必须 `tail == checkpoint == last`
- 如果 active，则 `active_sequence` 不能跳到一个明显离谱的未来值

输出：

- `true`
  状态窗口合理
- `false`
  control 状态损坏或不符合 `v6.1` 语义

作用点：

- mount-time replay 入口

### 5.4 `cryexts_journal_v2_begin()`

位置：
[journal.c](/D:/Carl/cryptext4/cryexts/journal.c:515)

功能：

- 开始一笔新的 journal v2 事务

核心动作：

1. 递增 sequence
2. 设置 runtime 状态为 active
3. 把文件系统标成 `needs_recovery`
4. 写 control block
5. 清空 descriptor/commit block

输出：

- `0`
  开始成功
- 负错误码
  开始失败

### 5.5 `cryexts_journal_v2_commit()`

位置：
[journal.c](/D:/Carl/cryptext4/cryexts/journal.c:663)

功能：

- 结束当前事务，并把 journal 收敛回 idle/checkpoint 完成状态

`v6.1` 的关键逻辑：

```text
先 sync home metadata
再 reset journal
再清 recovery
再 sync metadata
```

这是 `v6.1` 相比 `v6.0` 最重要的行为变化。

原因：

- 当前实现记录到 payload 的是 home metadata 的“旧内容”
- 也就是更接近 undo journal

所以正常 commit 时，正确顺序应该是：

- 先尽量保证 home metadata 已稳定
- 再清掉 journal

否则可能出现：

- home metadata 未稳
- journal 已被清空

这样 crash 后两边都不可靠。

### 5.6 `cryexts_journal_v2_reset_state()`

位置：
[journal.c](/D:/Carl/cryptext4/cryexts/journal.c:466)

功能：

- 把 control / descriptor / commit 重置成空事务状态

核心效果：

- `active_sequence = 0`
- `tail_sequence = last_sequence`
- `checkpoint_sequence = last_sequence`
- descriptor 为空
- commit 为空

这一步就是 `v6.1` 里“checkpoint 完成”的最终落盘动作。

### 5.7 `cryexts_journal_v2_replay()`

位置：
[journal.c](/D:/Carl/cryptext4/cryexts/journal.c:698)

功能：

- mount-time 恢复 journal v2 中的未完成事务

处理流程：

1. 读 control
2. 校验 control layout/checksum
3. 校验 control sequence window
4. 如果 `active_sequence == 0`
   说明 journal 已经 idle，直接清 recovery 状态即可
5. 如果 `active_sequence != 0`
   继续读 descriptor 和 commit
6. 只有 descriptor / commit / sequence 全部匹配时才 replay
7. replay 完成后先把恢复后的 home metadata sync 下去
8. 再 reset_state，把 journal 收敛回 idle

### 5.8 `cryexts_journal_v2_set_sequence()`

位置：
[journal.c](/D:/Carl/cryptext4/cryexts/journal.c:182)

功能：

- 更新 runtime `journal_sequence`
- 同时把 superblock 里的 `journal_sequence` 一起更新

注意：

`v6.1` 里这个函数还会同步更新 `journal_last_sequence`

因为对当前单事务模型来说，superblock 记录的 `journal_sequence`
本质上就是“最近已知的 last sequence”。

## 6. 新工具说明

### 6.1 `cryexts_journal_v2_inject`

位置：
[tools/cryexts_journal_v2_inject.c](/D:/Carl/cryptext4/cryexts/tools/cryexts_journal_v2_inject.c:1)

功能：

- 在一个 `journal v2` image 上构造一笔待恢复事务

它做的事是：

1. 读 root dir home block
2. 把它写入 journal payload
3. 写 control
4. 写 descriptor
5. 写 commit
6. superblock 标 `needs_recovery`
7. 再把 root dir home block 故意清坏

这样下一次 mount 时，如果 replay 真生效，就会把 root dir block 恢复回来。

### 6.2 `cryexts_journal_inspect`

位置：
[tools/cryexts_journal_inspect.c](/D:/Carl/cryptext4/cryexts/tools/cryexts_journal_inspect.c:1)

`v6.1` 新增输出：

- `control.idle`
- `control.checkpoint_complete`

如何理解：

- `control.idle = 1`
  表示 `active_sequence == 0`

- `control.checkpoint_complete = 1`
  表示 journal 不但 idle，而且：
  `tail == checkpoint == last`

这个字段特别适合 smoke 脚本做断言。

## 7. `v6.1` smoke 在测什么

脚本：
[scripts/smoke_v6_1_journal_transaction.sh](/D:/Carl/cryptext4/cryexts/scripts/smoke_v6_1_journal_transaction.sh:1)

### 第一段：fresh image

验证：

- `mkfs -J` 后 fresh image 是 idle
- `checkpoint_complete = 1`
- `cryextsck` clean

### 第二段：正常事务提交

验证：

- mount
- 创建文件并写入
- umount
- inspect 看 journal 是否回到 idle/checkpoint complete
- `cryextsck` clean

这段是在验证：

```text
正常 commit 后 journal 会不会回到干净状态
```

### 第三段：注入待恢复事务

验证：

- 人工制造 active transaction
- `cryextsck` 在 replay 前应报错
- mount 触发 replay
- replay 后 inspect 再次看到 idle/checkpoint complete
- `cryextsck` clean

这段是在验证：

```text
active transaction -> mount-time replay -> checkpoint complete
```

## 8. 这一版的边界

虽然 `v6.1` 已经比 `v6.0` 明确很多，但还是要清楚：

它不是完整 JBD2。

当前仍然没有：

- 多事务并存
- tail 真正循环推进
- checkpoint queue
- revoke
- log wrap-around

所以这版最准确的定位是：

```text
journal v2 single-transaction semantics MVP
```

## 9. 一句话结论

`v6.1` 做的不是“加更多 journal 结构”，而是：

```text
把 v6.0 已经存在的 control / descriptor / commit 结构，
真正串成一条能解释、能检查、能恢复的单事务链路。
```
