# CRYEXTS v7.1 变更说明

## 1. 这一版解决了什么问题

`v7.0` 已经能创建多块 GDT 的镜像，但内核挂载路径仍然只按“单块 GDT”理解。

所以 `v7.1` 的目标不是再改 `mkfs`，而是把下面这条链真正打通：

```text
mkfs 写出多块 GDT
-> mount 时完整读入多块 GDT
-> 运行期修改 group descriptor
-> journal / dirty / sync 正确写回所有 GDT blocks
```

一句话概括：

```text
v7.1 = 多块 GDT 从“能创建”升级到“能挂载并参与真实元数据更新”
```

## 2. 新增和调整的结构

## 2.1 `struct cryexts_sb_info`

### `struct buffer_head **gdt_bhs`

- 含义：保存整个 GDT 区域每一个磁盘块对应的 `buffer_head`
- 作用：让内核能逐块管理 GDT，而不是只盯住第一块
- 生命周期：
  - mount 时分配
  - `cryexts_load_group_desc_table()` 中逐块填充
  - umount 时由 `cryexts_release_group_desc_table()` 统一释放

### `unsigned char *gdt_storage`

- 含义：一段连续内存，保存“完整 GDT 镜像”
- 作用：
  - 让 `sbi->groups[group]` 的遍历逻辑保持连续数组语义
  - 让 checksum、allocator、group accessor 不需要感知 descriptor 跨块边界
- 读路径：
  - 从每个 `gdt_bhs[i]->b_data` 拷贝进来
- 写路径：
  - 由 `cryexts_gdt_prepare_write()` 再拆回各个 `gdt_bhs[i]->b_data`

### `struct cryexts_group_desc *groups`

- 含义：运行时 group descriptor 数组视图
- v7.0 之前：直接指向单个 `gdt_bh->b_data`
- v7.1 之后：指向 `gdt_storage`
- 结果：上层 allocator / checksum 代码继续按 `groups[group]` 访问，不必知道底层已经是多块 GDT

## 3. 新增函数说明

## 3.1 `cryexts_load_group_desc_table(struct super_block *sb)`

- 位置：`super.c`
- 功能：在 mount 阶段完整载入 GDT 区域

### 处理流程

1. 根据 `group_desc_table_blocks` 分配 `gdt_bhs`
2. 分配连续内存 `gdt_storage`
3. 对每个 GDT block 执行 `sb_bread()`
4. 把每块内容拷贝进 `gdt_storage`
5. 把 `sbi->groups` 指向连续镜像

### 这样做的原因

如果只保留多个 `buffer_head`，遍历 descriptor 时每次都要自己算“第几个 descriptor 落在哪一块”，上层代码会变得非常碎。

而先拼成一段连续镜像后：

- `cryexts_group_first_block()`
- `cryexts_group_free_blocks()`
- `cryexts_group_free_inodes()`
- `cryexts_update_group_checksums()`

这些现有逻辑基本就能直接复用。

## 3.2 `cryexts_gdt_prepare_write(struct super_block *sb)`

- 位置：`super.c`
- 功能：把运行时的 GDT 镜像变成“可落盘状态”

### 处理内容

1. 若启用 metadata checksum，先调用 `cryexts_update_group_checksums()`
2. 把 `gdt_storage` 按块拆回 `gdt_bhs[i]->b_data`

### 为什么必须有这个函数

因为运行期真正被修改的是：

```text
sbi->groups[group].free_blocks_count
sbi->groups[group].free_inodes_count
```

这些字段都在 `gdt_storage` 里。

而 journal 记录和块写回面对的是：

```text
gdt_bhs[i]->b_data
```

如果不先做这一步，就会出现：

```text
内存镜像是新的
但 journal 记下的仍然是旧的 GDT block
```

## 3.3 `cryexts_release_group_desc_table(struct cryexts_sb_info *sbi)`

- 位置：`super.c`
- 功能：统一释放多块 GDT 相关资源

### 释放内容

- 每个 `gdt_bhs[i]`
- `gdt_bhs` 数组
- `gdt_storage`
- `groups` 指针清空

### 使用时机

- mount 失败回滚
- umount 的 `cryexts_put_super()`

## 4. 已修改的关键函数

## 4.1 `cryexts_fill_super()`

- 旧行为：如果 `group_desc_table_blocks > 1`，直接报不支持
- 新行为：调用 `cryexts_load_group_desc_table()` 读入完整 GDT，再继续做 group checksum 校验

这意味着：

```text
v7.0: 多块 GDT 镜像会被 mount 拒绝
v7.1: 多块 GDT 镜像可以被 mount 正常识别
```

## 4.2 `cryexts_mark_bitmap_dirty()`

- 位置：`balloc.c`
- 功能：当 inode/block 分配位图变化时，顺带把 GDT 的 free count 变化纳入 journal 和 dirty path

### v7.1 之前

- 只会处理一个 `gdt_bh`

### v7.1 之后

1. 先调用 `cryexts_gdt_prepare_write()`
2. 遍历所有 `gdt_bhs[i]`
3. 对每个 GDT block：
   - `cryexts_journal_record_bh()`
   - `mark_buffer_dirty()`

### 实际意义

只要某个 group 的 `free_blocks_count` / `free_inodes_count` 改了，不管这个 group descriptor 落在第几个 GDT block，都会被正确记录。

## 4.3 `cryexts_sync_metadata()`

- 位置：`super.c`
- 功能：把元数据统一刷盘

### v7.1 之后的 GDT 路径

1. 如果有 `gdt_bhs`，先 `cryexts_gdt_prepare_write()`
2. 遍历所有 GDT blocks
3. 对每一块执行：
   - `mark_buffer_dirty()`
   - `sync_dirty_buffer()`

### 结果

多块 GDT 不再只刷第一块，而是整段区域都参与同步。

## 4.4 `cryexts_put_super()`

- 旧行为：只 `brelse(sbi->gdt_bh)`
- 新行为：调用 `cryexts_release_group_desc_table()`

这样 umount 时不会再遗漏后续 GDT blocks 的释放。

## 5. 运行时案例

下面用一个具体例子说明 v7.1 的处理方式。

假设：

- `group_count = 55`
- `sizeof(group_desc) = 76`
- `block_size = 4096`

则：

```text
descs_per_block = 4096 / 76 = 53
gdt_blocks = ceil(55 * 76 / 4096) = 2
```

也就是说：

- `group[0..52]` 落在第 1 个 GDT block
- `group[53..54]` 落在第 2 个 GDT block

### mount 时

1. `cryexts_fill_super()`
2. `cryexts_load_group_desc_table()`
3. 读 GDT block 1 和 GDT block 2
4. 拼成一段连续 `gdt_storage`
5. `sbi->groups[54]` 可以直接访问，不需要知道它来自第二块

### 分配新 inode 时

如果分配器最终用到了 `group[54]`：

1. `sbi->groups[54].free_inodes_count--`
2. `cryexts_mark_bitmap_dirty()`
3. `cryexts_gdt_prepare_write()`
4. `group[54]` 对应的那部分 descriptor 被写回第二个 GDT block 的 `b_data`
5. 第二个 GDT block 被 journal 记录并标脏

这就说明：

```text
第二块 GDT 不只是“被读到了”
而是真的参与了运行时元数据更新
```

## 6. smoke 测试说明

新增脚本：

```text
scripts/smoke_v7_1_multi_gdt_mount.sh
```

### 这条脚本验证什么

1. 创建一个明确跨越单块 GDT 上限的大镜像
2. 确认 `gdt_blocks > 1`
3. mount 多块 GDT 镜像
4. 批量创建文件，制造真实 inode 分配
5. umount 后再次 inspect
6. 观察第二块 GDT 里的目标 group `free_inodes_count` 下降
7. remount 后再校验文件内容存在

### 它验证的重点

不是只验证：

```text
mount 不报错
```

而是验证：

```text
多块 GDT mount 成功
+ allocator 修改了跨块 group descriptor
+ 更新结果被真正写回磁盘
```

## 7. v7.1 之后的边界

这一版完成的是“内核侧多块 GDT 支持”。

还没完成的是：

- `cryextsck` 完整多块 GDT 支持
- 多块 GDT 场景下的 fsck 校验与 repair

所以版本推进关系现在很清楚：

- `v7.0`：mkfs 能创建多块 GDT
- `v7.1`：kernel 能挂载并更新多块 GDT
- `v7.2`：cryextsck 理解并校验多块 GDT
