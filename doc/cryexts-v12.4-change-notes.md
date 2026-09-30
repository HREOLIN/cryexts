# CRYEXTS v12.4 收尾说明

## 版本目标

v12.4 不再改变 journal 的磁盘格式、事务状态机或锁模型。该版本为 Version 12 增加稳定性发布门槛，验证已有 ring journal、后台 checkpoint 和 v12.3 ordered barrier 在连续 metadata 操作与多次重挂载后的结果一致。

## 稳定性场景

`scripts/smoke_v12_4_stability.sh` 默认使用 128 MiB 镜像和 96 个文件，执行：

```mermaid
flowchart LR
    A[mkfs: ring journal] --> B[创建两个目录]
    B --> C[循环: 写入 + fsync]
    C --> D[循环: 覆盖 + fsync]
    D --> E[rename 与部分 unlink]
    E --> F[卸载 + fsck]
    F --> G[重挂载并校验内容]
    G --> H[第二轮 rename]
    H --> I[卸载 + fsck + journal inspect]
    I --> J[IDLE and head equals tail]
```

每个保留文件的前缀和 page offset `4096` 的覆盖内容都会在第二次挂载中校验。被删除的文件必须不存在。最终检查 journal ring 已完成 checkpoint：

```text
control.idle=1
control.checkpoint_complete=1
control.ring_head == control.ring_tail
cryextsck: ... clean
```

## 执行

```bash
cd ~/cryexts
./scripts/smoke_v12_4_stability.sh
```

可用 `COUNT=160` 增大循环次数；默认值保持在当前每组 56 inode 的镜像限制内，避免测试把 inode 耗尽误判成 journal 故障。

完整 Version 12 验收入口：

```bash
./scripts/smoke_version12_mvp.sh
```

## Version 12 结论

Version 12 MVP 至此具备 ring 分配、after-image redo、已提交事务 replay、后台 checkpoint、ordered flush barrier 和可重复稳定性验证。仍未实现 JBD2/ext4 级多 running transaction 并发、BIO `REQ_FUA` 粒度控制或断电级硬件故障认证；这些应进入后续性能与可靠性版本，而不是继续扩大 v12 的改动面。
