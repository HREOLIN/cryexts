# CRYEXTS v6.6 large xattr / xattr overflow

## 1. 这一版解决什么问题

在 `v4.4 ~ v6.5` 之间，CRYEXTS 的 xattr 还是单块模型：

```text
inode
-> xattr_block
-> 一个 block 里塞下全部 user.* xattr
```

这样有两个明显限制：

- xattr 总量一旦超过一个 block，就直接 `-ENOSPC`
- `cryextsck` 只能看到“这个 inode 挂了一个 xattr block”，但看不到更大的存储层次

`v6.6` 的目标是把它推进到一个稳定的 MVP：

```text
inode
-> root xattr block
   -> overflow_block
-> spill xattr block
```

一句话理解：

```text
v6.6 = xattr 从单块，升级到 root + 1 overflow 的双块模型
```

注意这一版的边界很明确：

- 支持“总 xattr 容量超过一个 block”
- 不支持“无限级联 overflow”
- 不支持“单个超大 value 跨多个 block 拆分”

也就是说，`v6.6` 是一个低风险、可检查、可调试的 large xattr MVP。

## 2. on-disk 结构变化

### 2.1 `struct cryexts_xattr_block_header`

位置：[cryexts_fs.h](/D:/Carl/cryptext4/cryexts/cryexts_fs.h:307)

`v6.6` 前：

```c
struct cryexts_xattr_block_header {
    __le32 magic;
    __le16 entries;
    __le16 used_bytes;
    __u8 reserved[8];
} __attribute__((packed));
```

`v6.6` 后：

```c
struct cryexts_xattr_block_header {
    __le32 magic;
    __le16 entries;
    __le16 used_bytes;
    __le64 overflow_block;
} __attribute__((packed));
```

字段说明：

- `magic`
  作用：固定值 `CRYEXTS_XATTR_MAGIC`，用于识别这是 xattr block。

- `entries`
  作用：当前 block 里存了多少个 xattr entry。
  注意：这里只统计“本 block 内”的 entry 数量，不是整个 inode 的总数。

- `used_bytes`
  作用：从 header 之后开始，实际用了多少字节。
  `fsck` 会检查：
  `sizeof(header) + used_bytes` 是否正好覆盖所有 entry。

- `overflow_block`
  作用：如果当前 block 后面还有 spill block，就指向那个 block。
  语义：
  - `0`：没有 overflow
  - 非 `0`：这是一个 large xattr inode，后面还有第二块

### 2.2 `CRYEXTS_XATTR_MAX_ITEMS`

位置：[cryexts_fs.h](/D:/Carl/cryptext4/cryexts/cryexts_fs.h:118)

新增：

```c
#define CRYEXTS_XATTR_MAX_ITEMS 32U
```

作用：

- 之前内核里最多只允许 16 个 xattr item
- `v6.6` 提升到 32 个，给双块模型留出空间

这不是理论上限，只是当前 MVP 的工程上限。

## 3. 存储模型如何工作

### 3.1 root block 和 overflow block 的关系

假设某个文件有很多个 `user.attrXX`：

```text
user.attr00
user.attr01
...
user.attr17
```

写入时流程是：

```text
setxattr
-> 把所有 xattr item 先放到内存数组
-> 尽量往 root block 里装
-> 装不下的剩余 item 放到 overflow block
-> root header.overflow_block = overflow block number
```

所以磁盘上不是“一个 entry 被拆成两半”，而是：

```text
前几个完整 entry 放 root
后几个完整 entry 放 overflow
```

这点非常重要。

`v6.6` 没有做的是：

```text
一个 value 前 2000 bytes 在 root
后 2000 bytes 在 overflow
```

当前不允许这种拆法。

### 3.2 为什么这一版只做一个 overflow block

原因是要控制复杂度。

如果一开始就做：

```text
root -> overflow1 -> overflow2 -> overflow3 -> ...
```

那就要同时解决：

- 多级链表损坏检测
- journal 更新顺序
- 删除/缩小时的 block 回收
- fsck 链完整性
- inspect 展示

这样风险会一下子拉高很多。

所以 `v6.6` 先固定成：

```text
root -> overflow
```

`cryextsck` 也会明确检查：

- root 可以有 `overflow_block`
- overflow block 自己不允许再指向下一个 overflow

如果它再链出去，就报错。

## 4. 关键函数说明

### 4.1 `cryexts_large_xattr_feature_enabled()`

位置：[xattr.c](/D:/Carl/cryptext4/cryexts/xattr.c:34)

职责：

- 检查 superblock 的 `features_ro_compat`
- 判断是否开启 `CRYEXTS_FEATURE_RO_COMPAT_LARGE_XATTR`

作用：

- root block 里如果带 `overflow_block`
- 但 filesystem 又没开 `LARGE_XATTR`
- 那么内核会把它当成损坏处理

### 4.2 `cryexts_parse_xattr_block()`

位置：[xattr.c](/D:/Carl/cryptext4/cryexts/xattr.c:40)

职责：

- 解析一个 xattr block
- 校验 header
- 校验每个 entry 的长度和边界
- 把 entry 读到内存数组中
- 顺便返回 `overflow_block`

参数语义：

- `items`
  目标数组

- `base`
  从数组的哪个下标开始写

- `count`
  返回当前累计的 item 数量

- `overflow_block`
  返回 header 里的 `overflow_block`

为什么要有 `base`：

因为 `v6.6` 读取时要分两次装载：

```text
先解析 root block -> items[0...n)
再解析 overflow   -> items[n...m)
```

### 4.3 `cryexts_load_xattrs()`

位置：[xattr.c](/D:/Carl/cryptext4/cryexts/xattr.c:101)

职责：

- 读取 inode 的 root xattr block
- 如果 root header 里有 `overflow_block`
  - 检查 `LARGE_XATTR` feature
  - 再读取 overflow block
- 把两块里的 xattr 合并成一个内存视图

流程：

```text
inode.xattr_block
-> read root
-> parse root
-> if overflow_block != 0
   -> read overflow
   -> parse overflow
```

### 4.4 `cryexts_pack_xattr_block()`

位置：[xattr.c](/D:/Carl/cryptext4/cryexts/xattr.c:137)

职责：

- 把内存中的 xattr item 数组序列化成一个 block
- 填好 header
- 填好 entry
- 填好 `used_bytes`

这是 `v6.6` 新增的核心 helper。

之前 `cryexts_write_xattrs()` 直接一边遍历一边往 buffer 写。
现在为了支持 root/overflow 两块，序列化逻辑被抽出来复用。

### 4.5 `cryexts_write_xattrs()`

位置：[xattr.c](/D:/Carl/cryptext4/cryexts/xattr.c:171)

职责：

- 根据总 item 数和总大小，决定哪些 item 放 root，哪些放 overflow
- 必要时分配 overflow block
- 先写 overflow，再写 root
- 最后把 inode 元数据落盘

为什么“先写 overflow，再写 root”：

因为 root block 的 header 里会指向 overflow。
如果先写 root，再写 overflow，中间 crash 的话，root 会指向一个还没写好的 block。

所以顺序更稳的是：

```text
1. overflow block 写好
2. root block 写入正确的 overflow_block 指针
3. inode 持久化
```

### 4.6 `cryexts_free_xattr_storage()`

位置：[xattr.c](/D:/Carl/cryptext4/cryexts/xattr.c:566)

职责：

- 先读 root xattr block
- 看它是否带 `overflow_block`
- 如果有，先释放 overflow
- 再释放 root

所以删除 inode 时，`v6.6` 已经不会只回收第一块了。

## 5. `cryextsck` 做了什么补强

### 5.1 `validate_xattr_block()`

位置：[tools/cryextsck.c](/D:/Carl/cryptext4/cryexts/tools/cryextsck.c:1443)

职责：

- 读取一个 xattr block
- 检查 `magic / entries / used_bytes`
- 检查每个 entry 的 `name_len / value_len / namespace`
- 返回这个 block 的 `overflow_block`

### 5.2 inode 校验新增内容

位置：[tools/cryextsck.c](/D:/Carl/cryptext4/cryexts/tools/cryextsck.c:1580)

`v6.6` 之后，`cryextsck` 对 xattr 的检查不再只是：

```text
xattr_block 有没有越界
xattr_block 有没有被多个 inode 重复引用
```

而是继续向下检查：

- root xattr block header 是否合法
- root xattr entries 是否逐个合法
- root 的 `overflow_block` 是否要求 `LARGE_XATTR` feature
- overflow block 是否也合法
- overflow block 是否被多个 inode 重复引用
- overflow block 是否又链向第三块

最后这一条的语义是：

```text
v6.6 只允许 root + 1 overflow
overflow 再链下去，就视为损坏
```

## 6. 新增 inspect 工具

工具：[cryexts_xattr_inspect](/D:/Carl/cryptext4/cryexts/tools/cryexts_xattr_inspect.c:1)

用法：

```bash
./cryexts_xattr_inspect <image> <inode-number>
```

输出内容包括：

- `xattr_root_block`
- root block 的 `entries / used_bytes / overflow_block`
- root 内每个 item 的 `name / name_len / value_len`
- overflow block 的同类信息

这能帮助你直接看清：

```text
哪些 xattr 留在 root
哪些 xattr spill 到 overflow
```

## 7. 一个具体案例

假设我们给文件写 18 个 xattr：

```text
user.attr00 = 220 bytes
user.attr01 = 220 bytes
...
user.attr17 = 220 bytes
```

因为每个 entry 自己也要带：

- `struct cryexts_xattr_entry`
- 名字本身
- value

所以 root block 不一定能装下 18 个。

这时布局可能是：

```text
inode
-> xattr_block = 120

block 120 (root)
  header.entries = 13
  header.overflow_block = 121
  item0..item12

block 121 (overflow)
  header.entries = 5
  header.overflow_block = 0
  item13..item17
```

查询时流程是：

```text
getxattr("user.attr15")
-> load root
-> root 里没找到
-> follow overflow_block
-> load overflow
-> 找到 attr15
```

列举时流程是：

```text
listxattr
-> load root items
-> load overflow items
-> 合并成一个名字列表返回给 VFS
```

## 8. smoke test 做了什么

脚本：[smoke_v6_6_large_xattr.sh](/D:/Carl/cryptext4/cryexts/scripts/smoke_v6_6_large_xattr.sh:1)

步骤：

1. 创建带 `xattr / policy / metadata checksum / journal v2` 的 v6 镜像
2. mount
3. 创建一个普通文件
4. 通过 Python 连续写入多个 `user.attrXX`
5. 立即 `getxattr/listxattr` 校验
6. 卸载后运行 `cryexts_xattr_inspect`
7. 检查 root 已经指向 `overflow_block`
8. 跑 `cryextsck`
9. remount 后再次读回全部 xattr
10. 再次 `cryextsck`

预期结果：

```text
v6.6 large-xattr smoke test passed
```

## 9. 当前边界

`v6.6` 已经实现：

- xattr 双块存储
- root + 1 overflow
- mount 后透明读取
- 删除路径完整回收
- inspect 可观测
- fsck 深度检查

`v6.6` 还没有实现：

- 多级 xattr chaining
- 单 value 跨块拆分
- xattr block checksum
- xattr repair
- xattr dedupe / shared block

所以这一版最准确的定位是：

```text
large xattr MVP
```
