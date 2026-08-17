# CRYEXTS v11.5：Version 11 MVP 回归收口

## 版本定位

v11.5 不新增磁盘格式，也不改变 journal v3 的 replay 算法。它把 v11.0 到 v11.4 已实现的能力收敛成一个可重复执行的验收入口，确认：

- metadata redo journal 可以提交、丢弃、重放和重复重放；
- data=ordered 的数据块先于引用它的 inode/extent 元数据提交；
- page cache、writeback、加密和历史布局没有回归。

## 统一入口

在 Ubuntu 上执行：

    cd ~/cryexts
    chmod +x scripts/smoke_version11_mvp.sh
    ./scripts/smoke_version11_mvp.sh

脚本只使用 image，不操作 U 盘。它按以下顺序执行现有 smoke：

| 阶段 | 覆盖内容 |
|---|---|
| legacy v2 | 历史布局创建、挂载、读写和 fsck |
| journal v2 | 旧 journal 格式的布局和 checksum |
| v10.5 | plain/encrypted page cache、writeback、fsync、错误 key 和密文检查 |
| v11.3 | committed、uncommitted、partial checkpoint 三种 v3 故障场景 |
| v11.4 | 新分配、原地覆盖、truncate、fsync 和 remount |
| soak | 重复执行两轮 v3 replay/fsck，检查恢复结果稳定 |

## 验收结果

所有子脚本成功后，统一入口最后输出：

    version11 MVP smoke test passed

每一轮 recovery 场景还必须满足：

    cryextsck: image clean
    control.state=0
    control.idle=1
    descriptor.entry_count=0
    commit.entry_count=0

## 设计边界

当前 Version 11 MVP 采用固定 journal 区域和单 metadata transaction。v11.5 的 soak 是回归验证，不是生产级压力测试；它没有引入循环日志、多事务并发、异步 data dependency tracking 或 FUA/barrier 设备矩阵。

这些功能应在确有性能或硬件可靠性需求时单独规划，避免把 MVP 的验收脚本误认为生产级 NAS 存储栈认证。
