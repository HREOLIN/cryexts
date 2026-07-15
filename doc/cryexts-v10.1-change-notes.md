# CRYEXTS v10.1 变更说明

## 1. 这一版做了什么

`v10.1` 只做一件事：

```text
把 regular file 读路径接到 Linux page cache
```

这一版不碰：

- buffered write
- writeback
- journal 与 writeback 的重新收口

也就是说，`v10.1` 的目标不是“把 Version 10 全部做完”，而是先把：

```text
cached read
```

这个最小闭环落下来。

## 2. 为什么先做这个

在 `v10.0` 之前，普通文件读取一直走自定义逐块搬运路径：

- `read_iter`
- `cryexts_resolve_block()`
- `cryexts_read_inode_block()`
- `copy_to_iter()`

这条路径能工作，但有两个问题：

1. 重复读取同一个文件时，收益拿不到标准 page cache
2. 后面的 buffered write / writeback 无法建立在标准缓存读路径之上

所以 `v10.1` 的最小推进方向就是：

```text
先让 regular file read 进入 page cache 主路径
```

## 3. 这一版没有修改 on-disk 格式

`v10.1` 没有新增任何新的磁盘结构体，也没有修改：

- superblock
- inode
- group descriptor
- dir index block
- journal v2

所以这是一次：

```text
纯运行时 I/O 路径改动
```

不是 format 变更。

## 4. 关键代码变化

## 4.1 `cryexts_file_aops`

位置：

- [file.c](/D:/Carl/cryptext4/cryexts/file.c)
- [cryexts.h](/D:/Carl/cryptext4/cryexts/cryexts.h)

新增：

```c
const struct address_space_operations cryexts_file_aops = {
	.readpage = cryexts_readpage,
};
```

### 作用

给 regular file 提供 page cache 读取入口。

### 含义

之前 regular file 只有 `file_operations.read_iter`；
现在 regular file 同时有：

- `file_operations`
- `address_space_operations`

这样 `generic_file_read_iter()` 才能通过 page cache 调到文件系统自己的页装载逻辑。

## 4.2 `cryexts_readpage()`

位置：

- [file.c](/D:/Carl/cryptext4/cryexts/file.c)

### 功能

读取一个 page cache page 对应的文件内容，并填充该 page。

### 处理流程

1. 根据 `page_offset(page)` 计算这页对应的文件偏移
2. 根据文件大小计算本页有效字节数
3. 按 `CRYEXTS_BLOCK_SIZE` 分段循环
4. 通过 `cryexts_resolve_block(inode, logical, false, &physical)` 找物理块
5. 如果 `physical != 0`
   - 调 `cryexts_read_inode_block()` 读磁盘块
   - 拷贝到 page buffer
6. 如果 `physical == 0`
   - 说明是 hole
   - 直接补零
7. 页尾超出 EOF 的部分也补零
8. `SetPageUptodate(page)` 并 `unlock_page(page)`

### 为什么这样做

这样做能同时兼容：

- legacy direct/indirect mapping
- extent / extent tree
- sparse file hole read
- encrypted read path

因为底层仍然统一走：

```text
cryexts_resolve_block()
+ cryexts_read_inode_block()
```

没有重写 block mapping 语义。

## 4.3 `cryexts_read_iter()`

位置：

- [file.c](/D:/Carl/cryptext4/cryexts/file.c)

### 修改前

`cryexts_read_iter()` 自己做：

- 逐块 resolve
- 逐块 read
- 逐块 copy_to_iter

### 修改后

直接改成：

```c
return generic_file_read_iter(iocb, to);
```

### 含义

这表示 regular file 读路径不再自己绕开页缓存，而是正式切到 Linux 通用 cached-read 主线。

## 4.4 regular file inode 挂接 `a_ops`

位置：

- [inode.c](/D:/Carl/cryptext4/cryexts/inode.c)

在下面两个场景里，regular file 都会设置：

```c
inode->i_mapping->a_ops = &cryexts_file_aops;
```

### 场景 A：`cryexts_init_inode()`

挂载后从磁盘读出 inode，初始化 VFS inode 时。

### 场景 B：`cryexts_new_inode()`

新建 regular file inode 时。

### 作用

保证：

- 已有文件
- 新建文件

都会通过同一套 page cache 读路径。

## 4.5 `cryexts_invalidate_cache_range()`

位置：

- [file.c](/D:/Carl/cryptext4/cryexts/file.c)

### 功能

把某个文件范围内已有的 page cache 页失效掉。

底层调用：

```c
invalidate_inode_pages2_range()
```

### 为什么必须加这个

因为 `v10.1` 只接了 cached read，
但当前 write 路径还是旧的“直接写磁盘块”模型，没有接 buffered write。

也就是说：

- 读：走 page cache
- 写：绕过 page cache，直接落盘

如果写完不主动失效缓存页，就可能出现：

```text
磁盘上已经是新数据
page cache 里还是旧数据
下一次 read 读到旧页
```

### 当前调用点

#### A. `cryexts_write_iter()`

写成功后，对本次写入范围做缓存失效。

如果中途部分写入后失败，也会对已写入范围做一次失效，避免脏旧页残留。

#### B. `cryexts_fallocate()`

`punch hole / zero range` 修改了底层块内容后，也对对应范围做缓存失效。

## 5. 这一版的语义边界

`v10.1` 现在提供的是：

```text
cached read + direct write cache invalidation
```

不是：

```text
cached read + buffered write + writeback
```

所以这版的定位很明确：

- 读已经正式进入 page cache
- 写还没进入 page cache
- 为了保证一致性，写后主动 invalidate

这是一个过渡版，但它是正确的过渡版。

## 6. 为什么这版仍然值得单独做

因为它已经解决了两个很实际的问题：

1. repeated read 能走标准缓存命中路径
2. 后面 `v10.2` 做 buffered write 时，不用再从头改 regular file read 主线

所以 `v10.1` 的价值不是“最终性能已经完全到位”，而是：

```text
先把 read path 从 MVP 私有路径，切到 Linux 正规 cached read 主线
```

## 7. 这版新增的 smoke

新增脚本：

- [scripts/smoke_v10_1_cached_read.sh](/D:/Carl/cryptext4/cryexts/scripts/smoke_v10_1_cached_read.sh)

### 验证内容

1. 创建 image 并 mkfs
2. mount CRYEXTS
3. 写入初始文件
4. 连续读两次同一个文件
5. 覆盖文件第一页
6. 再读一次文件
7. 验证读到的是新内容，而不是旧缓存
8. umount 后再跑 `cryextsck`

### 这个 smoke 重点在测什么

不是测极限吞吐，而是测：

```text
cached read 已经接通
+ 直写路径不会把 page cache 弄脏成旧数据
```

## 8. 这一版之后，下一步该做什么

`v10.1` 落地后，最自然的下一步就是：

```text
v10.2 buffered write
```

也就是把当前“写完主动 invalidate”这条过渡路线，推进成：

```text
写也正式进入 page cache
+ 后台 writeback 落盘
```

## 9. 一句话总结

```text
v10.1 没有改磁盘格式，
它做的是把 regular file 读路径正式接到 Linux page cache，
并用写后缓存失效保证当前 direct-write 模型下的读写一致性。
```
