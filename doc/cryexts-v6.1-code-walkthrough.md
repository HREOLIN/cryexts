# CRYEXTS v6.1 代码处理说明

## 1. 这份文档的目标

前面的两份 `v6.1` 文档已经分别讲了：

- 版本变更说明
- journal transaction 语义

这份文档不再重复抽象概念，而是专门回答两个问题：

1. 这次代码到底改了哪些处理逻辑
2. 结合真实案例，这些逻辑在运行时是怎么走的

如果你要审核代码，这份文档更适合对着函数一路往下看。

## 2. 先说一句话结论

`v6.1` 做的核心事情不是“再增加 journal 结构”，而是把已有的：

- `control`
- `descriptor`
- `payload`
- `commit`

从“能摆在磁盘上”，推进成：

```text
能正确开始事务
能正确提交事务
能在 mount 时正确回放事务
能在回放后把 journal 收敛回干净状态
```

## 3. 代码改动总览

### 3.1 runtime 状态缓存

文件：
[cryexts.h](/D:/Carl/cryptext4/cryexts/cryexts.h:24)

新增字段：

- `journal_last_sequence`
- `journal_active_sequence`
- `journal_tail_sequence`
- `journal_checkpoint_sequence`

这 4 个字段的作用不是新增 on-disk 格式，而是让内核在内存里明确知道：

- 当前最后一个事务序号是多少
- 当前是否存在活动事务
- 当前 tail/checkpoint 推进到哪里

这样 begin / commit / replay 的状态切换不需要每次临时猜。

### 3.2 mount 时初始化 runtime 状态

文件：
[super.c](/D:/Carl/cryptext4/cryexts/super.c:420)

补上的逻辑：

- 从 superblock 读入 `journal_sequence`
- 初始化 `last_sequence`
- 初始化 idle 状态下的 `tail/checkpoint`

这一步的意义是：

```text
刚 mount 完，内核里就有一份和磁盘一致的 journal 状态视图
```

### 3.3 `journal.c` 是本次最核心的修改点

主要改动集中在：
[journal.c](/D:/Carl/cryptext4/cryexts/journal.c:182)

新增/调整的关键函数：

- `cryexts_journal_v2_set_state()`
- `cryexts_journal_v2_prepare_control()`
- `cryexts_journal_v2_sequence_state_valid()`
- `cryexts_journal_v2_begin()`
- `cryexts_journal_v2_commit()`
- `cryexts_journal_v2_replay()`
- `cryexts_journal_v2_reset_state()`

## 4. 每个关键函数到底改了什么

### 4.1 `cryexts_journal_v2_set_state()`

文件位置：
[journal.c](/D:/Carl/cryptext4/cryexts/journal.c:192)

功能：

- 一次性更新 runtime 中的：
  - `last`
  - `active`
  - `tail`
  - `checkpoint`

为什么要加它：

在 `v6.0` 里，sequence 相关逻辑更多是分散写的。
`v6.1` 把这几个状态收成一个入口，后面 begin/reset/replay 都用这一套，减少状态不一致。

### 4.2 `cryexts_journal_v2_prepare_control()`

文件位置：
[journal.c](/D:/Carl/cryptext4/cryexts/journal.c:204)

`v6.0` 时这个函数只关心：

- `active_sequence`
- `last_sequence`

`v6.1` 扩展成同时写：

- `tail_sequence`
- `checkpoint_sequence`

也就是说，从 `v6.1` 开始，control block 里的 4 个 sequence 字段不再只是摆设，而是每次 reset/begin 都会明确写入。

### 4.3 `cryexts_journal_v2_sequence_state_valid()`

文件位置：
[journal.c](/D:/Carl/cryptext4/cryexts/journal.c:360)

这是 `v6.1` 新增的 control 状态校验函数。

它检查：

- `tail <= checkpoint`
- `checkpoint <= last`
- 如果 journal idle，则：
  - `active == 0`
  - `tail == checkpoint == last`

为什么这个函数重要：

以前 `control` 更多只检查：

- magic
- version
- checksum
- 固定块号

这样只能说明“格式长得像对的”，不能说明“事务状态真的自洽”。

`v6.1` 开始，mount replay 入口会先过这一层。

### 4.4 `cryexts_journal_v2_begin()`

文件位置：
[journal.c](/D:/Carl/cryptext4/cryexts/journal.c:515)

`v6.1` 的 begin 流程是：

1. 计算新事务号 `sequence = previous + 1`
2. 更新 runtime sequence
3. 设置 runtime 状态：
   - `last = previous`
   - `active = sequence`
   - `tail = previous`
   - `checkpoint = previous`
4. 设置 `needs_recovery`
5. 写 active control block
6. 清 descriptor / commit block

这一步代表：

```text
新事务已经开始，但 checkpoint 还停留在上一笔事务
```

### 4.5 `cryexts_journal_v2_commit()`

文件位置：
[journal.c](/D:/Carl/cryptext4/cryexts/journal.c:663)

这是这次最关键的逻辑修正。

#### `v6.0` 的问题

旧顺序更接近：

```text
reset journal
-> clear recovery
-> sync metadata
```

这会有风险：

- journal 已经被清空
- 但 home metadata 还未必稳定落盘

如果这时掉电，那么：

- replay 也回不来了
- home metadata 也可能不完整

#### `v6.1` 的顺序

现在改成：

```text
sync home metadata
-> reset journal 到 idle/checkpoint complete
-> clear recovery
-> sync metadata
```

为什么这样才对：

因为当前这套 journal v2 实现，本质上记录的是：

```text
metadata block 的旧内容
```

也就是更像：

```text
undo-style metadata journal
```

所以正常提交时，必须先保证 home metadata 尽量已经稳定，然后才能把旧备份 log 清掉。

### 4.6 `cryexts_journal_v2_reset_state()`

文件位置：
[journal.c](/D:/Carl/cryptext4/cryexts/journal.c:466)

功能：

- 把 journal 收敛回空闲状态

重置后的含义：

- `active = 0`
- `tail = last`
- `checkpoint = last`
- descriptor 为空
- commit 为空

这一点可以理解成：

```text
checkpoint 已经完成
journal 当前没有挂着的事务
```

### 4.7 `cryexts_journal_v2_replay()`

文件位置：
[journal.c](/D:/Carl/cryptext4/cryexts/journal.c:698)

`v6.1` 的 replay 路径比 `v6.0` 更完整，顺序是：

1. 读 control
2. 校验 control header/checksum/layout
3. 校验 control sequence window
4. 如果 `active == 0`
   - 直接清 recovery 状态
5. 如果 `active != 0`
   - 继续读 descriptor
   - 继续读 commit
6. descriptor / commit / sequence 全匹配，才真正 replay
7. replay 把 payload 拷回 home block
8. 先把恢复后的 home metadata `sync`
9. 再把 journal reset 成 idle

这一条链路正是这次 smoke 里验证通过的核心。

## 5. 工具侧改了什么

### 5.1 `cryexts_journal_inspect`

文件：
[tools/cryexts_journal_inspect.c](/D:/Carl/cryptext4/cryexts/tools/cryexts_journal_inspect.c:1)

新增输出：

- `control.idle`
- `control.checkpoint_complete`

如何理解：

- `idle=1`
  表示 `active_sequence == 0`

- `checkpoint_complete=1`
  表示：
  - idle
  - `tail == checkpoint == last`

这两个字段让 smoke 脚本不用自己去手算 sequence 关系。

### 5.2 `cryextsck`

文件：
[tools/cryextsck.c](/D:/Carl/cryptext4/cryexts/tools/cryextsck.c:787)

`v6.1` 给它补了三类能力：

1. 校验 control 状态窗口是否合法
2. 对 idle 状态做更严格的一致性检查
3. 当 journal 里存在完整但尚未 replay 的事务时，明确报：

```text
journal v2 replay pending
```

也就是说，`cryextsck` 现在能区分：

- journal clean
- journal 格式坏了
- journal 结构合法，但还在 pending recovery

### 5.3 `cryexts_journal_v2_inject`

文件：
[tools/cryexts_journal_v2_inject.c](/D:/Carl/cryptext4/cryexts/tools/cryexts_journal_v2_inject.c:1)

这是 `v6.1` 新增的测试工具。

它会做下面的事：

1. 读取 root directory home block
2. 把旧内容写入 journal payload
3. 写 control
4. 写 descriptor
5. 写 commit
6. superblock 标成 `needs_recovery`
7. 重算 superblock checksum
8. 把 home block 故意破坏

### 5.4 这次调试过程中顺手修掉的两个问题

这两个问题也值得记录，因为它们就是这次 smoke 真正卡住的原因。

#### 问题 1：inject 后 superblock checksum 没更新

现象：

- `cryextsck: superblock checksum mismatch`
- mount 直接失败，连 replay 都进不去

原因：

- `cryexts_journal_v2_inject` 改了 superblock
- 但最开始没有重算 metadata checksum

修复：

- 给工具补上 superblock checksum 重算逻辑

#### 问题 2：descriptor/commit header bytes 常量写错

现象：

- `cryextsck: journal v2 descriptor has non-zero trailing home block entries`

原因：

- `struct cryexts_journal_v2_descriptor`
- `struct cryexts_journal_v2_commit`

实际头大小已经是 `72B`
但常量还写成 `64U`

这会导致：

- `home_blocks[]` 起始位置解释错位
- trailing entry 检查扫错地址

修复：

文件：
[cryexts_fs.h](/D:/Carl/cryptext4/cryexts/cryexts_fs.h:251)

把：

- `CRYEXTS_JOURNAL_V2_DESCRIPTOR_BYTES`
- `CRYEXTS_JOURNAL_V2_COMMIT_BYTES`

从 `64U` 改成 `72U`

## 6. 用真实案例解释这次代码怎么工作

下面用这次 `smoke_v6_1_journal_transaction.sh` 的真实过程讲一遍。

脚本位置：
[scripts/smoke_v6_1_journal_transaction.sh](/D:/Carl/cryptext4/cryexts/scripts/smoke_v6_1_journal_transaction.sh:1)

### 阶段 1：fresh image

执行：

- `mkfs -J`
- `cryexts_journal_inspect`
- `cryextsck`

预期输出特征：

- `control.active_sequence=0`
- `control.checkpoint_complete=1`
- `cryextsck: clean`

说明：

```text
新镜像没有待恢复事务
journal 是空闲且完全 checkpoint 的
```

### 阶段 2：正常写入 `txn.txt`

脚本会：

1. mount 文件系统
2. 创建 `txn.txt`
3. 写入内容 `v6.1-journal-txn`
4. `umount`
5. inspect journal

这时 journal 内部实际发生的是：

1. `cryexts_write_iter()`
2. `cryexts_journal_begin()`
3. metadata 修改路径里多次 `journal_record_bh()`
4. `cryexts_write_inode_to_disk()`
5. `cryexts_journal_commit()`

而 `v6.1` 的 commit 会确保：

- home metadata 先尽量落盘
- journal 再 reset 成 idle

所以 inspect 会看到：

- `last_sequence=2`
- `active_sequence=0`
- `tail=2`
- `checkpoint=2`

这正表示：

```text
事务做完了
journal 又回到了干净状态
```

### 阶段 3：人为注入待恢复事务

脚本执行：

```bash
./cryexts_journal_v2_inject "$IMG"
```

此时工具会：

- 先把 root dir 旧内容写入 payload
- 再写 control/descriptor/commit
- 再把 root dir home block 清坏

所以这时 image 的真实状态是：

```text
home block 坏了
journal 里有完整旧备份
superblock 标记 needs_recovery
```

这时 `cryextsck` 会报：

```text
journal v2 replay pending
```

这是对的，不是失败。

它的含义是：

```text
现在镜像不是 clean
但 journal 结构是成立的
等 mount replay 来恢复
```

### 阶段 4：mount-time replay

随后脚本再次 mount。

这时内核流程是：

1. mount 读取 superblock
2. 发现 `needs_recovery`
3. 进入 `cryexts_journal_v2_replay()`
4. 校验 control
5. 校验 descriptor
6. 校验 commit
7. 从 payload 取出旧 root dir block
8. 拷回 home block
9. `sync`
10. `reset_state`
11. 清 `needs_recovery`

所以你最终看到：

- `mount-time v6.1 replay succeeded`
- `control.active_sequence=0`
- `control.checkpoint_complete=1`
- `cryextsck: clean`

这就说明：

```text
待恢复事务已经回放成功
journal 重新回到 idle/checkpoint 完成状态
```

### 阶段 5：为什么脚本最后要检查 `txn.txt`

这一步很关键。

最开始脚本写过一次：

```text
txn.txt = "v6.1-journal-txn"
```

后来注入工具只破坏了：

- root directory home block

它并没有破坏已经正常提交过的文件内容事务。

所以 replay 完成后，正确结果应该是：

- `txn.txt` 还在
- 内容不变

这就说明：

```text
replay 恢复的是坏掉的 metadata
不是把整个文件系统回滚到更早状态
```

## 7. 最后如何理解这次修改

如果压缩成一句最贴近代码的话：

```text
v6.1 把 “写 metadata 前记录旧块、mount 时按完整事务回放、回放后收敛回干净状态”
这条链真正打通了。
```

再简单一点：

- `v6.0` 解决“journal v2 长什么样”
- `v6.1` 解决“journal v2 怎么真正工作”

## 8. 一句话总结

`v6.1` 最值得记录的不是新增了几个字段，而是：

```text
它把 CRYEXTS 的 journal v2 从“格式可读”
推进成了“事务可跑、恢复可验、状态可解释”的第一版可用实现。
```
