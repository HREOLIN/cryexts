# CRYEXTS v10.2 变更说明

## 1. 版本定位

`v10.2` 把 regular file 写路径从自定义逐块 `write_iter`，切换到 Linux page cache 写接口：

```text
generic_file_write_iter
  -> write_begin
  -> copy data into page cache page
  -> write_end
```

当前版本采用同步 write-through：`write_end` 会立即把本页内容写入 CRYEXTS 数据块并提交 journal。

所以 `v10.2` 是：

```text
page-cache write interface + synchronous persistence baseline
```

真正的 dirty-page 聚合和异步 writeback 留给 `v10.3`。

## 2. 磁盘格式

本版本没有修改 on-disk format，也没有增加结构体字段。

以下结构保持不变：

- `struct cryexts_super_block`
- `struct cryexts_inode`
- extent / extent tree
- journal v2
- directory index
- xattr / policy table

## 3. `cryexts_fill_page()`

文件：[file.c](/D:/Carl/cryptext4/cryexts/file.c)

功能：把一个 page cache page 对应的文件内容装入内存。

处理流程：

1. 根据 `page_offset(page)` 得到文件偏移
2. 调用 `cryexts_resolve_block(..., create=false)` 完成 logical-to-physical 映射
3. 已分配块通过 `cryexts_read_inode_block()` 读取
4. sparse hole 和 EOF 后区域补零
5. 成功后设置 `PageUptodate`

`v10.1` 的 `readpage` 和 `v10.2` 的 partial-write 准备过程共用这个函数，避免维护两套页装载逻辑。

## 4. `cryexts_write_begin()`

文件：[file.c](/D:/Carl/cryptext4/cryexts/file.c)

函数职责：为一次 page-cache 写入准备已锁定 page。

主要步骤：

1. 检查 inode、偏移和文件最大长度
2. 检查当前固定要求 `PAGE_SIZE == CRYEXTS_BLOCK_SIZE == 4096`
3. 使用 `grab_cache_page_write_begin()` 获取并锁定 page
4. 如果 page 尚未 uptodate，调用 `cryexts_fill_page()` 读取旧内容
5. 把 page 返回给 `generic_file_write_iter()`，由内核完成用户数据拷贝

为什么 partial write 必须先读旧页：

```text
只覆盖一页中的部分字节时，其他字节必须保留原文件内容。
```

## 5. `cryexts_write_end()`

文件：[file.c](/D:/Carl/cryptext4/cryexts/file.c)

函数职责：把已经拷入 page cache 的内容同步写入数据块，并完成 inode/journal 更新。

主要步骤：

1. 如果 `copied == 0`，直接释放 page，不启动事务、不分配 block
2. 启动 journal transaction
3. 根据 `pos` 和 `copied` 找到涉及的 logical blocks
4. 调用 `cryexts_resolve_block(..., create=true)` 分配或查询 physical block
5. 调用 `cryexts_write_inode_block()` 写入完整 4K block
6. 扩展 `i_size`
7. 更新 `i_blocks`、`mtime`、`ctime`
8. 调用 `cryexts_write_inode_to_disk()`
9. 提交 journal transaction
10. 保留 page 为 uptodate、clean cache page

journal 和 block allocation 放在 `write_end()` 而不是 `write_begin()`，避免用户数据拷贝失败为 0 字节时产生无效块分配，也避免 transaction 跨越用户内存拷贝阶段。

当前 page 不留作 dirty page，因为数据已经在 `write_end()` 中同步落盘。

## 6. `cryexts_write_iter()`

修改前：

```text
copy_from_iter
-> read-modify-write block buffer
-> direct block write
```

修改后：

```c
return generic_file_write_iter(iocb, from);
```

这表示 regular file write 正式进入 Linux 通用 page-cache 写入口。

## 7. `cryexts_file_aops`

当前 `address_space_operations` 为：

```c
const struct address_space_operations cryexts_file_aops = {
    .readpage = cryexts_readpage,
    .write_begin = cryexts_write_begin,
    .write_end = cryexts_write_end,
};
```

三个入口分别负责：

- `readpage`：磁盘到 page cache
- `write_begin`：准备并加载缓存页
- `write_end`：分配物理块并把缓存页同步写回

## 8. 最大文件长度

`super.c` 的 `s_maxbytes` 调整为 extent-tree v2 的最大映射范围。

原因是 `generic_file_write_iter()` 会先根据 `s_maxbytes` 做通用写入边界检查。如果仍使用旧 direct/indirect 上限，extent inode 会被过早截断。

每个 inode 的实际上限仍由 `cryexts_regular_file_max_size_for_inode()` 和 `cryexts_resolve_block()` 校验：

- legacy inode 仍受 direct/indirect 上限约束
- extent inode 使用 extent 对应上限

## 9. 加密语义

page cache 中保存明文，磁盘数据块保存密文。

写入路径仍调用 `cryexts_write_inode_block()`，因此现有 policy-aware encryption 没有被绕开。

完整的加密缓存性能与 writeback 协同仍在 `v10.4` 收口。

## 10. 一致性边界

`v10.2` 没有把 page 标记为长期 dirty，也没有把 journal transaction 延后到后台线程。

这意味着：

- 优点：沿用当前同步持久化和 journal 边界，风险较小
- 限制：多页写入仍然按页提交，吞吐优化有限

这是有意保留的阶段边界。`v10.3` 再实现 dirty page、`writepage/writepages` 和 journal 批处理。

## 11. Smoke 测试

脚本：[smoke_v10_2_buffered_write.sh](/D:/Carl/cryptext4/cryexts/scripts/smoke_v10_2_buffered_write.sh)

覆盖场景：

1. 512-byte 小块连续写
2. 读取文件填充 page cache
3. 对缓存文件做非对齐局部覆盖
4. append
5. truncate
6. remount 后校验内容
7. 最终 `cryextsck` clean

执行：

```bash
chmod +x scripts/smoke_v10_2_buffered_write.sh
./scripts/smoke_v10_2_buffered_write.sh
```

## 12. 下一阶段

`v10.3` 的目标是把当前同步 write-through 推进为真正的 writeback：

```text
write_begin/write_end
-> dirty page
-> writepage/writepages
-> 聚合落盘
-> journal 边界收口
```
