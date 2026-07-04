# CRYEXTS V4.5 Journal 加固原理

## 1. 这一版在解决什么问题

V4.2 已经有了最小 journal replay，但它还存在三个明显薄弱点：

- journal header 只有 `magic/flags/entry_count/sequence`，没有自校验
- mount replay 前没有严格检查 `home_blocks[]` 是否越界、是否写回到 journal 自己
- `cryextsck --repair` 还不理解 recovery 状态和 journal header 的关系

V4.5 的目标，就是把这三块补完整。

## 2. journal header 新增了什么

`struct cryexts_journal_header` 现在把第四个字段正式定义为：

```text
checksum
```

它覆盖整块 journal header block，但会跳过 `checksum` 字段自己。

当前实现使用的是一个轻量级 FNV-1a 风格 32-bit 校验。

它不是强密码学哈希，但对于当前 MVP 足够解决两个问题：

- header 被随手改坏时，能快速发现
- replay 前能判断 header 是否“结构自洽”

## 3. 为什么要校验整个 header block

因为我们真正想保护的不是单个字段，而是整张“payload -> home block”的映射表。

当前 journal header 的核心语义是：

```text
payload #0 -> home_blocks[0]
payload #1 -> home_blocks[1]
...
```

如果只校验 `entry_count`，但 `home_blocks[]` 被改坏，replay 仍然可能把旧元数据写回错误位置。

所以这次的 checksum 覆盖了：

- `magic`
- `flags`
- `entry_count`
- `sequence`
- `reserved`
- `home_blocks[]`

这样一来，只要 header 任何关键内容不一致，replay 就会拒绝继续。

## 4. mount replay 前现在检查什么

`cryexts_journal_replay()` 现在不是“只要看起来像 journal 就回放”，而是先做一轮合法性判断：

1. `magic` 必须正确
2. `entry_count` 不能超过 `journal_blocks - 1`
3. `checksum` 必须匹配
4. `home_blocks[i]` 必须：
   - 非 0
   - 小于 `blocks_count`
   - 位于合法 data area
   - 不能落在 journal 自己的 block 范围内
5. header 里未使用的 trailing `home_blocks[]` 必须为 0

只要有一项不成立，就返回 `-EUCLEAN`，挂载失败。

这背后的原则很简单：

```text
不允许 replay 把损坏扩散到别的位置
```

## 5. 为什么要禁止写回到 journal 自己

因为 journal 区本身是“恢复素材库”。

如果 `home_blocks[i]` 被改成 journal 区内的 block，比如：

```text
payload #0 -> journal_block + 5
```

那 replay 就会开始覆盖 journal 自己的内容，形成“恢复过程破坏恢复数据”的递归问题。

所以 V4.5 明确禁止：

```text
home_block 落在 [journal_block, journal_block + journal_blocks)
```

## 6. checksum 在事务路径里怎么更新

现在 journal 相关路径都会在写 header 前重算 checksum：

- `cryexts_journal_record_block()`
- `cryexts_journal_commit()`
- replay 后清空 header
- 注入测试工具 `cryexts_journal_inject`

也就是说，header 每次发生结构变化时，checksum 都同步刷新。

## 7. `cryextsck --repair` 这次新增了什么能力

V4.5 不是让 `cryextsck` 直接帮你 replay journal。

它做的是“低风险修补”：

### 场景 A

superblock 说自己 `needs_recovery`，但 journal header 连 magic 都不对。

这时通常表示：

- recovery 标记残留
- journal header 已经空了或坏了

`--repair` 会：

- 清掉 recovery state
- 重建一个空 journal header

### 场景 B

journal header 是空事务：

- `flags = 0`
- `entry_count = 0`

但 superblock 还留着 `needs_recovery`。

这时 `--repair` 会：

- 清掉 recovery state

### 场景 C

journal header checksum 不对，但 header 本身已经不是 valid transaction。

这类情况说明：

- 当前没有真实待回放事务
- 但 header 残留脏值

这时 `--repair` 会：

- 重建一个空 header
- 如果 superblock 还挂着 recovery，也一起清掉

## 8. `--repair` 为什么不修 valid journal transaction

因为如果 header 还标记为：

```text
CRYEXTS_JOURNAL_FLAG_VALID
```

那意味着磁盘上可能真的有一笔待恢复事务。

这时候 `fsck` 不能擅自“猜测修复”，否则可能把本来还能回放的事务直接抹掉。

所以 V4.5 的原则是：

```text
只修低风险状态残留
不碰可能仍然有效的真实事务
```

## 9. sync 语义为什么也要一起补

如果只加 checksum，但 `sync_metadata()` 还是只刷 superblock 和旧 bitmap，那么会出现这种情况：

- journal header 已更新
- group bitmap / GDT 还没稳定落盘

这样 recovery 和 free count 之间还是会出现观察不一致。

所以 V4.5 把 `sync_metadata()` 扩展为尽量覆盖：

- superblock
- GDT
- legacy bitmap
- per-group block bitmap
- per-group inode bitmap
- 最后 `sync_blockdev()`

这样 `fsync`、`journal commit`、clean unmount` 的语义会更闭环。

## 10. 一句话理解 V4.5

V4.5 的本质不是“加了个 checksum 字段”，而是：

```text
让 journal 从“能 replay”升级成“先确认自己可信，再 replay”
```
