# CRYEXTS v11.4: data=ordered 与 fsync

## 目标

v11.4 把普通文件的数据写回与 inode/extent 元数据提交顺序固定下来：

    page cache 明文
        -> cryexts_write_inode_block() 加密到物理 data buffer
        -> cryexts_sync_inode_block() 等待该 data buffer 写回
        -> cryexts_write_inode_to_disk()
        -> journal_commit() 提交元数据

这是当前单事务模型下的最小 data=ordered 实现，不引入多事务或依赖图。

## 代码变化

### cryexts_sync_inode_block()

crypto.c 新增按物理 block 同步的辅助函数：

1. 通过 sb_getblk() 找到数据块 buffer；
2. 调用 sync_dirty_buffer() 等待该 buffer 写回；
3. 检查 buffer_write_io_error()；
4. 返回错误并释放 buffer。

同步的是加密后的物理数据 buffer，不是 page cache page。page cache 仍然保存明文。

### cryexts_writepage_locked()

每个数据 block 完成 cryexts_write_inode_block() 后立即同步。所有相关 data block 成功后，才写 inode 并执行 journal commit。

    data buffer sync failed
        -> abort journal transaction
        -> mapping_set_error()
        -> redirty page
        -> error returned to writeback/fsync

writeback 回调中没有调用 file_write_and_wait_range()，因此不会等待当前仍持有 writeback 状态的 page。

## fsync 语义

fsync() 先通过 VFS 的 file_write_and_wait_range() 等待目标范围的 page cache writeback。由于 writeback 已经先同步 data buffer、再提交元数据，随后 cryexts_sync_metadata() 完成剩余元数据同步。

覆盖三类场景：

- 新分配数据块：data 持久化先于 inode/extent journal commit；
- 已分配块原地覆盖：fsync 等待对应 data writeback；
- truncate/free：truncate 前等待 page cache，避免旧 dirty page 在释放 block 后再次写回。

## 测试

测试只使用 image，不操作 U 盘：

    cd ~/cryexts
    chmod +x scripts/smoke_v11_4_data_ordered.sh
    ./scripts/smoke_v11_4_data_ordered.sh

测试包含新文件扩展写入并 fsync、原地覆盖并 fsync、脏数据后 truncate、卸载/重新挂载、内容和文件大小校验，以及最终 cryextsck clean。

预期最后输出：

    v11.4 data=ordered fsync smoke test passed

## 边界

- 仍然是单事务 journal；
- 仍使用 buffer cache 的同步接口，尚未实现异步 data dependency tracking；
- 不宣称具备生产级 NAS 的多事务并发、FUA/barrier 设备矩阵和崩溃注入覆盖。
