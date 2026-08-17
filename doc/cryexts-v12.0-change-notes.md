# CRYEXTS v12.0：journal ring 格式底座

## 目标

v12.0 不实现真正的多事务循环分配，而是先把 journal 的 ring 状态落到磁盘并让整个工具链识别它。这样 v12.1 才能安全地开始移动 head/tail，而不需要再次修改格式。

新 image 使用：

    mkfs.cryexts -Q ...

其中 Q 表示 journal v3 + ring feature。

## 磁盘格式

v3 control block 使用原有保留空间增加四个字段：

| 字段 | 含义 |
|---|---|
| ring_start | ring 第一个可用 journal block，等于 journal_block + 1 |
| ring_end | ring 末尾的排他地址，等于 journal_block + journal_blocks |
| ring_head | 下一笔事务将要分配的位置 |
| ring_tail | 最早未回收事务的位置 |

v12.0 初始化时：

    ring_head = ring_tail = ring_start

新增 incompat feature JOURNAL_RING 和 control feature RING。没有这两个 feature 的历史 v3 image 保持旧格式：ring 字段必须为零。

## 当前语义

v12.0 仍使用 v11 的单事务、固定 descriptor/payload/commit 位置完成 redo commit 和 checkpoint。ring 的 head/tail 当前只表示“已初始化、无未回收事务”的状态，不移动，也不用于重新分配 journal block。

这是一个有意的边界：格式先稳定，分配策略留给 v12.1。当前状态不会把固定 journal 伪装成已经支持多事务循环日志。

## 校验与工具

- 内核 mount 读取并校验 ring feature 与四个位置；
- cryextsck 检查 super feature、control feature、range 及 empty ring 状态；
- cryexts_journal_inspect 输出 journal_ring、ring_start、ring_end、ring_head、ring_tail、ring_valid；
- 旧 v3 control 出现非零 ring 字段会被当成损坏，避免格式混用。

## 测试

    cd ~/cryexts
    chmod +x scripts/smoke_v12_0_journal_ring.sh
    ./scripts/smoke_v12_0_journal_ring.sh

脚本创建 Q 格式 image，检查初始化 ring，挂载后写入并 fsync，验证 journal checkpoint 后 ring 仍处于合法空状态，最后 remount、比对文件和运行 cryextsck。

预期最后输出：

    v12.0 journal ring layout smoke test passed

## 后续

v12.1 开始实现 head/tail 真正推进、journal 空间计算和单 writer ring 分配。v12.2 才允许 running transaction 与旧事务 checkpoint 并存。
