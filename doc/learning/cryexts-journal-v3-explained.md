# CRYEXTS journal v3 详解：control / descriptor / payload / commit

> 本文用一次"创建文件"的具体事务，解释 journal v3 里四个容易混淆的概念：
> `control`（控制块）、`descriptor`（描述符/清单）、`payload`（数据区）、`commit`（提交块）。
> 依据源码：`cryexts_fs.h`、`journal.c`（`cryexts_journal_v3_commit` / `cryexts_journal_v3_replay`）、`tools/mkfs.cryexts.c`。

## 版本记录

| 版本 | 日期 | 说明 |
| --- | --- | --- |
| v1.0 | 2026-08-17 | 初始版本：整理 journal v3 四个块角色、一次事务示例、恢复三场景与 ring 差异 |

---

## 0. journal 到底在解决什么

一次文件系统操作往往会改**多个**元数据块。比如创建一个文件，要同时改：

```text
目录数据块     写入新的 dir_entry
inode bitmap   把某个 inode 标记为已用
inode table    写入新 inode 记录
```

如果系统在"改了目录、但还没改 inode bitmap"时断电，重启后目录里有个文件，但 bitmap 说这个 inode 是空的，元数据就自相矛盾了。

journal 的解法是：**先把这一组新内容写到一个安全的临时区，全部写好后打一个"生效戳"，再慢慢搬回真正的位置（home block）**。
崩溃后只认"打了生效戳"的那批，没打完的就丢掉。

`journal v3` 是一个 **redo（重做）日志**：临时区里存的是**新内容**（after-image），恢复时把它重新覆盖回 home block。

## 1. 四个角色一句话定义

```text
control      = 总导航 / 总开关：整个 journal 当前是什么状态、东西都放在哪
descriptor   = 清单：这一笔事务改了哪几个 home block、各自的校验和
payload      = 货：那些 home block 的新内容，按清单顺序排好
commit       = 生效戳：证明"这批货齐全且有效，可以用了"
```

对应 `cryexts_fs.h` 里的 `block_type`：

```text
CRYEXTS_JOURNAL_V3_BLOCK_CONTROL    = 1
CRYEXTS_JOURNAL_V3_BLOCK_DESCRIPTOR = 2
CRYEXTS_JOURNAL_V3_BLOCK_COMMIT     = 3
```

每个块开头都有 `magic = "JNL3"` + `block_type`，内核靠这两个字段认出"这块是干嘛的"。

## 2. 固定布局（mkfs -R，非 ring）长什么样

假设 128 MiB 镜像，`journal_block = 32256`，`journal_blocks = 512`：

```text
block 32256        control         （固定）
block 32257        descriptor      （固定）
block 32258..32766 payload area    （固定，最多 509 块）
block 32767        commit          （固定）
```

这是 `mkfs.cryexts.c` 里 `set_journal_v3_control()` 写的初始状态：

```c
descriptor_block = journal_block + 1;                  // 32257
payload_start    = journal_block + 2;                  // 32258
payload_blocks   = journal_blocks - 3;                 // 509
commit_block     = journal_block + journal_blocks - 1; // 32767
```

初始 `control.state = IDLE`，`descriptor.entry_count = 0`，`commit.flags = 0`。

## 3. 一次具体事务：创建 /a.txt 改 3 个块

假设 root 目录块 = block 8，inode bitmap = block 3，inode table = block 4。创建 `a.txt` 这笔事务的 sequence = 1，改了 3 个 home block：

```text
home block 8   目录块：新增一条 dirent  "a.txt" -> inode 60
home block 3   inode bitmap：把 bit 59 置 1（inode 60 已分配）
home block 4   inode table：写入 inode 60 的 struct cryexts_inode
```

### 3.1 先写 payload（货先落地）

内核把 3 个 home block 的**新内容**依次读到内存，算校验和，然后写到 payload 区：

```text
payload @ 32258  = block 8 的新内容（目录）         checksum = 0x9f2a...
payload @ 32259  = block 3 的新内容（inode bitmap）  checksum = 0x77c1...
payload @ 32260  = block 4 的新内容（inode table）   checksum = 0x1e40...
```

同时边写边把这些信息记到 descriptor 的 entries 里。

### 3.2 写 descriptor（清单）

`block 32257` 里最终是：

```c
struct cryexts_journal_v3_descriptor {
    magic           = "JNL3"
    block_type      = DESCRIPTOR
    sequence        = 1
    entry_count     = 3
    payload_start   = 32258
    commit_block    = 32767
    checksum        = <整块 descriptor 的校验和>

    entries[0] = { home_block = 8, payload_checksum = 0x9f2a..., flags = 0 }
    entries[1] = { home_block = 3, payload_checksum = 0x77c1..., flags = 0 }
    entries[2] = { home_block = 4, payload_checksum = 0x1e40..., flags = 0 }
    entries[3..250] = 全 0（空闲 entry 必须为 0）
};
```

可以把它理解为：**"这次要改 8、3、4 这三个 home block，新内容分别在 32258、32259、32260，各自的校验和如下"**。

### 3.3 写 control（状态推进到 PREPARED）

`block 32256` 被重写为：

```c
struct cryexts_journal_v3_control {
    magic                = "JNL3"
    block_type           = CONTROL
    state                = PREPARED        // 关键
    features             = REDO
    last_sequence        = 0               // 上一笔已完成的序号
    active_sequence      = 1               // 当前正在做的事务
    checkpoint_sequence  = 0               // 已经落回 home 的序号
    descriptor_block     = 32257
    payload_start        = 32258
    payload_blocks       = 3               // 这笔事务的 payload 块数
    commit_block         = 32767
    checksum             = <整块 control 的校验和>
};
```

control 就是**总导航**：mount 时先读它，就知道"journal 现在是什么状态、descriptor/payload/commit 分别在哪"。

注意 `state = PREPARED` **不等于**已提交。它只是说"东西基本摆好了"。

### 3.4 写 commit（唯一的生效戳）

`block 32767` 被写为：

```c
struct cryexts_journal_v3_commit {
    magic              = "JNL3"
    block_type         = COMMIT
    flags              = COMMITTED      // 关键：生效标志
    entry_count        = 3
    sequence           = 1
    descriptor_block   = 32257
    descriptor_checksum = <descriptor 块的校验和>   // 指向同一份清单
    payload_checksum    = <3 个 payload 串起来的聚合校验和>
    checksum            = <整块 commit 的校验和>
};
```

**commit block 是唯一的提交点。** 它到不了盘，这笔事务就不算数；它到了盘，恢复时才会重放。

它里面存 `descriptor_checksum` 和 `payload_checksum`，形成一条防篡改链：

```text
commit  ->  验证 descriptor（descriptor_checksum 对不对）
        ->  验证每个 payload（entries[i].payload_checksum 对不对）
        ->  再验证所有 payload 的聚合和（payload_checksum 对不对）
```

任何一环对不上，恢复就直接报 `-EUCLEAN`，**绝不猜着写 home block**。

### 3.5 checkpoint：把 payload 搬回 home block

写 `control.state = CHECKPOINTING`，然后：

```text
payload @ 32258  ->  home block 8
payload @ 32259  ->  home block 3
payload @ 32260  ->  home block 4
```

搬完后清掉 superblock 的 `NEEDS_RECOVERY`，最后写 `control.state = IDLE`，这笔事务结束。

完整顺序就是 `journal.c` 里 `cryexts_journal_v3_commit()` 的执行顺序：

```text
1 写 payload（新内容 + 单块校验和）
2 写 descriptor（清单）
3 写 control = PREPARED
4 写 commit = COMMITTED        <- 唯一提交点
5 写 control = CHECKPOINTING
6 payload 搬回 home block
7 写 control = IDLE
```

## 4. 恢复时这三个块怎么用（对应三种崩溃）

mount 时 `cryexts_journal_v3_replay()` 先读 control，看 `state`：

### 场景 A：commit 没写成功就崩了

```text
control.state = PREPARED，但 commit 块不完整 / 没有 COMMITTED 标志
```

**丢弃整笔事务**，home block 保持旧值。因为 commit 是唯一生效证明。

### 场景 B：commit 写成功，但还没 checkpoint

```text
control.state = COMMITTED（或 CHECKPOINTING），commit 完整有效
home block 还是旧值，payload 里是新值
```

**replay**：把 payload 逐块拷回 home block。文件操作在重启后仍然"生效"。

### 场景 C：checkpoint 做了一半又崩了

```text
部分 home block 已经写入新值，commit 仍然完整有效
```

**再 replay 一遍**。因为 payload 是完整的 4 KiB after-image，覆盖一次和覆盖两次结果一样，所以重放是幂等的。

这就是 redo journal 的关键好处：**恢复动作永远是"payload -> home"，重复执行无害**，不需要记"恢复到第几块了"。

## 5. ring 版（mkfs -Q，v12）的区别

ring 版 control 还是固定放在 `journal_block + 0`，但 descriptor / payload / commit 不再固定，而是每笔事务在 ring 里现分配一段连续区域：

```text
control @ journal_block+0（固定，多存了 ring_start/end/head/tail）

ring 内一笔事务：
  [ descriptor ][ payload × entry_count ][ commit ]   = entry_count + 2 块
```

对应 `cryexts_journal_v3_ring_allocate()`：

```c
descriptor_block = start;             // ring_head
payload_start    = start + 1;
commit_block     = payload_start + entries;
next_head        = commit_block + 1;
```

control 里的 `ring_head` / `ring_tail` 记录"下一次从哪分配 / 最早未回收的是哪笔"，这就是 v12.1 做的环形分配。

但无论 fixed 还是 ring，**四个角色的含义完全一样**：

```text
control    还是导航，只是现在还要导航 ring 的 head/tail
descriptor 还是清单
payload    还是新内容
commit     还是生效戳
```

## 6. 一句话记忆

```text
control     = "journal 的目录页 + 开关"，mount 先看它
descriptor  = "这笔事务的购物清单"，改了哪些块、每块校验和
payload     = "清单对应的新内容块"
commit      = "盖章生效"，唯一提交点，恢复只认盖过章的事务
```

## 7. 关键结构体字段速查

| 结构体 | 关键字段 | 含义 |
| --- | --- | --- |
| `cryexts_journal_v3_control` | `state` | IDLE / ACTIVE / PREPARED / COMMITTED / CHECKPOINTING |
| | `active_sequence` / `last_sequence` / `checkpoint_sequence` | 当前事务 / 上一笔已完成 / 已落回 home 的序号 |
| | `descriptor_block` / `payload_start` / `payload_blocks` / `commit_block` | 各区域位置 |
| | `ring_head` / `ring_tail` / `ring_start` / `ring_end` | ring 分配指针（v12） |
| `cryexts_journal_v3_descriptor` | `entry_count` | 本事务改了几个 home block |
| | `entries[].home_block` / `entries[].payload_checksum` / `entries[].flags` | 每个 home block 与对应 payload 的校验和 |
| `cryexts_journal_v3_commit` | `flags = COMMITTED` | 生效标志 |
| | `descriptor_checksum` / `payload_checksum` | 指向并校验同一份清单与 payload |

## 8. 阅读源码顺序建议

```text
cryexts_fs.h          -> 看 v3 control / descriptor / entry / commit 结构体
tools/mkfs.cryexts.c  -> set_journal_v3_*() 看初始 clean 状态怎么落盘
journal.c             -> cryexts_journal_v3_begin / record_block / commit
                      -> cryexts_journal_v3_replay 看恢复决策
```
