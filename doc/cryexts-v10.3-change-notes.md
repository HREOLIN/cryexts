# CRYEXTS v10.3 变更说明

## 1. 版本定位

`v10.3` 把 `v10.2` 的同步 page-cache write-through 推进为真正的 dirty-page writeback：

```text
write_begin
-> 用户数据进入 page cache
-> write_end 标记 dirty
-> writepage/writepages 后台或 fsync 触发落盘
```

本版本的 journal 仍按单页 transaction 提交，没有做跨页批处理。

## 2. 磁盘格式

本版本没有修改任何 on-disk 结构体和 feature flag。

page cache 中保存明文；`writepage` 调用 `cryexts_write_inode_block()` 后，磁盘仍保存现有加密格式。

## 3. `cryexts_write_end()`

文件：[file.c](/D:/Carl/cryptext4/cryexts/file.c)

修改后只负责内存状态：

1. 根据 `copied` 扩展 `i_size`
2. 更新 `mtime/ctime`
3. 设置 `PageUptodate`
4. 调用 `set_page_dirty()`
5. 解锁并释放 page 引用

它不再：

- 分配 physical block
- 写数据块
- 写 inode table
- 提交 journal

因此多个落在同一 page 的小写可以先在 page cache 中合并。

## 4. `cryexts_writepage_locked()`

文件：[file.c](/D:/Carl/cryptext4/cryexts/file.c)

这是 v10.3 的核心落盘函数，输入是一个已经由 writeback 层锁定的 dirty page。

处理步骤：

1. 检查 page 是否还在当前文件大小内
2. 设置 page writeback 状态
3. 启动 journal transaction
4. 根据 page offset 计算 logical block
5. 调用 `cryexts_resolve_block(..., create=true)` 延迟分配 physical block
6. 调用 `cryexts_write_inode_block()` 写入数据
7. 更新 `inode->i_blocks`
8. 调用 `cryexts_write_inode_to_disk()` 持久化 inode size 和映射
9. 提交 journal
10. 结束 page writeback

如果写回失败：

- 调用 `mapping_set_error()` 记录 mapping 错误
- 使用 `redirty_page_for_writepage()` 重新标脏
- 设置 `PageError`
- 让后续 fsync 能感知错误

## 5. `cryexts_writepage()`

这是 `address_space_operations.writepage` 的单页入口，直接复用：

```text
cryexts_writepage_locked(page, wbc)
```

它负责单个 dirty page 的同步写回。

## 6. `cryexts_writepages()`

这是批量 writeback 入口。

当前实现使用内核已有的：

```c
write_cache_pages(mapping, wbc, cryexts_writepages_callback, NULL);
```

内核负责遍历和锁定 dirty pages，CRYEXTS callback 负责每一页的块映射、数据落盘和 journal 提交。

当前没有自定义 dirty-page radix/xarray，也没有重新实现 writeback 扫描器。

## 7. `cryexts_file_aops`

当前 regular file 的 aops 为：

```c
const struct address_space_operations cryexts_file_aops = {
    .readpage = cryexts_readpage,
    .writepage = cryexts_writepage,
    .writepages = cryexts_writepages,
    .write_begin = cryexts_write_begin,
    .write_end = cryexts_write_end,
    .set_page_dirty = __set_page_dirty_nobuffers,
};
```

这形成了完整的最小 page-cache I/O 回路：

- cache miss 读取：`readpage`
- buffered write 准备：`write_begin`
- dirty page 产生：`write_end`
- dirty page 落盘：`writepage/writepages`

### `set_page_dirty`

Linux 5.15 的 `set_page_dirty()` 会通过 `mapping->a_ops->set_page_dirty` 标记文件页。CRYEXTS page cache page 不维护 buffer_head 链，因此使用内核现成的：

```c
__set_page_dirty_nobuffers
```

如果这个回调为空，`write_end()` 调用 `set_page_dirty(page)` 时会跳转到 NULL 地址并触发 kernel Oops。

## 8. `cryexts_fsync()`

fsync 现在首先调用：

```c
file_write_and_wait_range(file, start, end)
```

只有对应 dirty pages 全部完成 writeback 后，才继续：

- 写 inode
- 同步文件系统 metadata

因此 writeback I/O 错误可以返回给调用者。

## 9. truncate 和 punch-hole

`fallocate(PUNCH_HOLE)` 在直接修改底层块之前，会先等待目标范围内 dirty pages 写回。

文件缩小时，`setattr` 会先调用 `filemap_write_and_wait()`，再释放 physical blocks 和截断 page cache。

这样不会出现：

```text
底层 block 已释放
旧 dirty page 随后又写回该 block
```

## 10. unlink 和 rename replacement

regular file 最后一个链接被删除前，unlink 会先等待该 inode 的 dirty pages 写回。

rename 覆盖一个最后链接的 regular file 时，也先做同样处理。

等待发生在 namespace journal transaction 开始之前，避免 writeback 内部再次启动 journal 时产生锁递归。

`cryexts_release_inode_storage()` 还会调用 `truncate_inode_pages()`，确保释放 block 后不再残留可写回的缓存页。

## 11. 当前 journal 边界

当前每个 dirty page 写回时执行一个 transaction：

```text
journal_begin
-> delayed block allocation
-> data block write
-> inode metadata write
-> journal_commit
```

这保证当前 MVP 的恢复语义容易验证，但多页顺序写仍会产生较多 transaction。

跨页 transaction batching 是后续性能优化，不在 v10.3 中提前实现。

## 12. Smoke 测试

脚本：[smoke_v10_3_writeback.sh](/D:/Carl/cryptext4/cryexts/scripts/smoke_v10_3_writeback.sh)

覆盖：

1. 512-byte 小写产生 dirty pages
2. fsync 前从 page cache 读取并校验
3. `fsync()` 触发 writeback
4. `sync` 触发全局 writeback
5. 删除未显式 fsync 的 dirty file
6. remount 后验证持久内容
7. 最终 `cryextsck` clean

执行：

```bash
chmod +x scripts/smoke_v10_3_writeback.sh
./scripts/smoke_v10_3_writeback.sh
```

## 13. 下一阶段

`v10.4` 重点验证和优化：

- encrypted page-cache read/write
- writeback 前加密、缓存中明文的边界
- policy-aware encryption 回归
- plain/encrypted 性能对比
