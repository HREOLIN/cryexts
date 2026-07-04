# CRYEXTS V5.1 orphan list 设计与实现说明

## 1. 这一版要解决什么问题

V5.0 只是把 orphan list 相关字段预留到了磁盘格式里：

- superblock 的 `orphan_head`
- inode extra 区准备承载 `next_orphan`

但当时还没有真正运行时逻辑。

V5.1 的目标是把它变成一个最小可工作的恢复闭环：

```text
unlink / rmdir / rename victim 最后引用删除
-> inode 先挂入 orphan list
-> 再释放 block / xattr / inode
-> mount 时若发现 orphan_head 非空，执行 orphan cleanup
```

## 2. 当前实现边界

这一版先实现：

- orphan list 的磁盘落点
- orphan set / clear
- mount-time orphan cleanup
- `unlink`
- `rmdir`
- `rename` 覆盖 victim 且 victim link count 归零
- `cryextsck` 能理解 orphan 链

这一版暂时没有完整覆盖：

- 普通 `truncate shrink` 的 orphan 保护
- extent tree 未来更复杂的 orphan case
- 自动注入 orphan 故障镜像的专用用户态工具

所以 V5.1 当前更准确的理解是：

```text
先把最后引用删除类场景做成 crash-safe cleanup 骨架
```

## 3. on-disk 结构怎么变了

### 3.1 superblock

继续使用 V5.0 已经引入的：

- `orphan_head`

它表示：

```text
当前 orphan 单链表的头 inode 号
```

### 3.2 inode extra

在 `struct cryexts_inode_extra` 新增：

- `next_orphan`

所以每个 orphan inode 在磁盘上都可以串起来：

```text
superblock.orphan_head -> inodeA.next_orphan -> inodeB.next_orphan -> ...
```

## 4. 代码改了哪些地方

### 4.1 `cryexts_fs.h`

扩展 `struct cryexts_inode_extra`：

- `next_orphan`

### 4.2 `cryexts.h`

新增接口：

- `cryexts_orphan_feature_enabled()`
- `cryexts_orphan_set()`
- `cryexts_orphan_clear()`
- `cryexts_orphan_cleanup()`
- `cryexts_release_inode_storage()`

### 4.3 `inode.c`

主要改动：

- 读 inode 时把 `next_orphan` 读入内存态 `cryexts_inode_info`
- 写 inode 时把 `next_orphan` 写回磁盘
- inode 校验时检查 `next_orphan` 合法性
- 新增 `cryexts_release_inode_storage()`
  - 统一释放 data blocks
  - 统一释放 xattr block
  - 清零 `size` / `i_blocks`
  - 再落盘 inode

### 4.4 `dir.c`

把原来“最后一个名字删掉后直接 free inode”的路径改成：

1. `cryexts_orphan_set()`
2. `cryexts_release_inode_storage()`
3. `cryexts_orphan_clear()`
4. `cryexts_free_inode()`

当前接入的路径有：

- `unlink`
- `rmdir`
- `rename` 覆盖掉旧 victim 且 victim link count 归零

### 4.5 `journal.c`

新增 orphan 核心逻辑：

- `cryexts_orphan_feature_enabled()`
- `cryexts_orphan_set()`
- `cryexts_orphan_clear()`
- `cryexts_orphan_cleanup()`

并且在 mount 路径上形成：

```text
journal replay
-> orphan cleanup
-> normal mount
```

这说明 orphan cleanup 被放在 replay 之后，是合理的：

- 先把 journal 内的 metadata 恢复好
- 再基于恢复后的 inode 状态去清 orphan 链

### 4.6 `super.c`

mount 逻辑增加：

- 若启用了 orphan feature 且 `orphan_head != 0`
  - 先开启 journal transaction
  - 再执行 `cryexts_orphan_cleanup()`
  - 成功后再 commit

这样 orphan cleanup 本身也被现有 recovery 主线保护起来。

### 4.7 `tools/cryextsck.c`

新增 orphan 链检查：

- `orphan_head` 是否在合法 inode 范围
- orphan 链是否成环
- orphan inode 是否已经 free
- orphan inode 是否自指
- orphan 链长度是否异常

## 5. 当前恢复语义怎么理解

### 正常路径

比如一个普通文件最后一个名字被删掉：

```text
unlink
-> i_nlink 变成 0
-> 把 inode 挂到 orphan list
-> 释放数据块和 xattr
-> 从 orphan list 摘掉
-> free inode
```

### 崩溃路径

如果在中间掉电，例如：

```text
已经挂到 orphan list
但还没完全释放完 inode/block
```

那么下次 mount：

```text
journal replay
-> orphan cleanup
-> 继续完成 inode 释放
```

这样 orphan list 的价值就体现出来了：

```text
它记录了“还有哪些 inode 需要继续清理”
```

## 6. 为什么这一版还没做 truncate orphan

因为 truncate 比 unlink 更细：

- 可能只是文件缩小，不是彻底删除
- 可能需要保留 inode，只回收一部分 blocks
- 未来 extent tree 还会让 shrink 语义更复杂

所以这次先把：

```text
last-link delete / rmdir / rename victim
```

这条最清晰的 orphan recovery 主线做稳。

后面再把普通 `truncate shrink` 接进 orphan 机制，会更稳。

## 7. 当前你可以怎么验收

V5.1 smoke 最理想的验证目标是：

1. 创建启用 orphan feature 的 V5 image
2. 正常 mount / unlink / umount
3. `cryextsck` 仍然 clean
4. 注入一个“orphan_head 非空但 inode 未完全清理”的镜像
5. 下次 mount 自动做 orphan cleanup
6. 再次 `cryextsck` clean

## 8. 当前版本的价值

V5.1 的意义不在于功能很多，而在于它把 CRYEXTS 的恢复语义往真实文件系统推进了一步：

```text
从“journal replay only”
推进到
“journal replay + orphan cleanup”
```

这会直接影响后续：

- `V5.2 extent tree`
- `V5.3 directory index`
- `V5.4 policy-aware encryption`

因为这些能力一旦进入真实写路径，最终都要落回 crash recovery 语义。
