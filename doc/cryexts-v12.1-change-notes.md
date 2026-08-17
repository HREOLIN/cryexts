# CRYEXTS v12.1 变更说明

## 版本目标

v12.1 在 v12.0 的 journal v3 ring layout 上实现单 writer 的真实环形分配：每次 metadata transaction 使用 ring 中的一段连续区域，checkpoint 完成后立即回收该段空间。

本版本暂不实现多 transaction 并发。`journal_lock` 仍保证同一时刻只有一笔事务，v12.2 再处理 running transaction 与 checkpoint 并行。

## 磁盘布局

```text
journal_block                 control
ring_start                    descriptor
ring_start + 1 ...            payload blocks
payload_start + entry_count   commit
next_head                     下一笔事务起点
```

一次事务需要 `entry_count + 2` 个连续 block。空间不足以在 ring 尾部放下完整事务时，空 ring 回绕到 `ring_start`；有未回收事务时禁止覆盖并返回 `-ENOSPC`。

## 指针语义

```text
空闲：       head == tail
提交中：     tail = 当前 descriptor，head = 下一次分配位置
checkpoint： 仍保留当前 descriptor/payload/commit 指针
完成：       tail = head，control.state = IDLE
```

checkpoint 成功后不清除旧 descriptor、payload 和 commit。它们已经不再由 `tail` 指向，下一次事务可以直接覆盖，减少额外 metadata I/O。

## 恢复与校验

- replay 先读取 control，再使用 control 中的动态 descriptor、payload 和 commit 指针。
- v12.1 ring 的 `ring_head`、`ring_tail` 必须落在 `[ring_start, ring_end)`；IDLE 状态必须满足 `head == tail`。
- `cryextsck` 保留旧 v3 fixed layout 的严格校验；ring layout 改为校验事务区域连续且位于 ring 内。
- checksum、home block 去重、payload after-image 校验规则不变。

## 验收标准

```text
mkfs -Q -> fsck clean
mount -> 多次 metadata transaction -> unmount
ring head/tail 可推进并回绕
remount 后目录内容完整
最终 cryextsck clean
```

## 已知边界

当前单 writer 会同步完成 checkpoint，因此正常运行时 ring 很快回到空闲状态；真正的并发事务、后台 checkpoint 和 sequence window 留到 v12.2。
