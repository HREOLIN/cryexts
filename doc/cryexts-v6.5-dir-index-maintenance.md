# CRYEXTS v6.5 Directory Index Maintenance

## 1. 这份文档看什么

这份文档专门解释 v6.5 的 directory hash index 维护逻辑：

```text
create -> add index bit
unlink -> maybe clear index bit
rename -> delete old name + add new name
link   -> add another name
fsck   -> 重新扫描目录，验证 index 是否可信
```

v6.5 没有改变磁盘格式。它改变的是索引维护方式。

## 2. 目录索引的基本模型

当前目录索引块是：

```text
64 个 bucket
每个 bucket 一个 16 bit mask
每个 mask bit 对应一个 directory logical block
```

例如：

```text
block_masks[9] = 0x000b
```

二进制：

```text
0000 0000 0000 1011
```

含义：

```text
bucket 9 的名字可能出现在 logical block 0
bucket 9 的名字可能出现在 logical block 1
bucket 9 的名字可能出现在 logical block 3
```

查询一个名字时：

```text
hash(name) % 64 = bucket
mask = block_masks[bucket]
只扫描 mask 中 bit=1 的目录逻辑块
```

## 3. logical block 和 physical block 的关系

mask 里存的是 directory logical block number，不是 physical block number。

例如：

```text
mask bit 3 = 1
```

这只表示：

```text
目录 logical block 3 里有这个 bucket 的候选名字
```

真正读取磁盘时，还要调用：

```c
cryexts_inode_block_at(dir, 3)
```

得到 physical block。

所以路径是：

```text
bucket mask bit
-> logical directory block
-> inode block mapping
-> physical block
-> read dirent
```

## 4. 新增文件时如何维护 mask

假设新增：

```text
/idx/file_001
```

它被写入目录 logical block 2。

hash 结果：

```text
hash("file_001") % 64 = 17
```

v6.5 会执行：

```text
block_masks[17] |= 1 << 2
entries++
```

如果原来：

```text
block_masks[17] = 0x0001
```

新增后：

```text
block_masks[17] = 0x0005
```

含义：

```text
bucket 17 的名字现在出现在 logical block 0 和 logical block 2
```

对应函数：

```c
cryexts_dir_index_add_name()
```

## 5. 删除文件时为什么不能直接清 bit

这是 v6.5 最重要的点。

假设 logical block 2 里有三个目录项：

```text
file_a -> bucket 17
file_b -> bucket 17
file_c -> bucket 23
```

当前：

```text
block_masks[17] bit2 = 1
```

现在删除：

```text
unlink("file_a")
```

如果直接执行：

```text
block_masks[17] &= ~(1 << 2)
```

那就错了，因为 `file_b` 还在 logical block 2，而且它也属于 bucket 17。

所以 v6.5 的删除逻辑是：

```text
1. 先清空 file_a 的 dirent
2. 扫描 file_a 所在的 logical block
3. 看这个 block 里是否还有 bucket 17 的 live dirent
4. 如果还有，bit2 保持 1
5. 如果没有，bit2 清 0
6. entries--
```

对应函数：

```c
cryexts_dir_index_remove_name()
```

## 6. rename 如何维护索引

rename 本质上分两类。

同目录 rename：

```text
/idx/a -> /idx/b
```

路径：

```text
cryexts_add_entry(new name)
-> cryexts_dir_index_add_name("b")

cryexts_delete_entry(old name)
-> cryexts_dir_index_remove_name("a")
```

跨目录 rename：

```text
/old/a -> /new/b
```

路径：

```text
new_dir add "b"
-> 更新 new_dir 的 index

old_dir delete "a"
-> 更新 old_dir 的 index
```

所以 v6.5 没有单独写一个 rename-index 函数，而是把 create/delete 的底层路径维护正确，让 rename 自然复用。

## 7. hard link 如何维护索引

hard link 新增的是一个目录项，不新增 inode 数据。

例如：

```text
ln /idx/file_200 /idx/hardlink_200
```

对 directory index 来说，它和 create 一个新名字类似：

```text
新增 name -> hash -> bucket -> set mask bit -> entries++
```

所以 hard link 也复用：

```c
cryexts_add_entry()
```

## 8. fsck 如何确认索引是对的

v6.5 的 `cryextsck` 会扫描整个目录数据区。

对于每个 live dirent：

```text
bucket = hash(name) % 64
logical_block = 当前正在扫描的目录逻辑块
要求 block_masks[bucket] 里对应 bit 必须是 1
```

新增校验：

```text
index.entries == 真实 live dirent 数量
```

这能抓住两类错误：

- mask bit 缺失：lookup 可能漏查
- entries 不匹配：增量 add/delete 计数错误

## 9. inspect 输出如何理解

v6.5 的 `cryexts_dir_index_inspect` 输出新增：

```text
active_buckets=...
mask_refs=...
```

含义：

- `active_buckets`：至少有一个 bit 被设置的 bucket 数量
- `mask_refs`：所有 bucket mask 中 bit=1 的总数

例子：

```text
entries=292
dir_blocks=4
active_buckets=63
mask_refs=171
```

理解：

```text
目录里有 292 个 live dirent
目录占用 4 个 logical block
64 个 bucket 里用了 63 个
所有 bucket 到 logical block 的引用共有 171 条
```

`mask_refs` 通常小于 `entries`，因为同一个 bucket 在同一个 block 里可能有多个名字，但 mask 只记录一次。

## 10. 一个完整例子

假设目录 `/idx` 现在有 3 个 logical block。

新增：

```text
file_300 -> bucket 12 -> logical block 2
```

索引变化：

```text
block_masks[12] |= 0x0004
entries += 1
```

删除：

```text
file_020 -> bucket 8 -> logical block 0
```

如果 block 0 仍有 bucket 8 的其他名字：

```text
block_masks[8] 不变
entries -= 1
```

如果 block 0 没有 bucket 8 的其他名字：

```text
block_masks[8] &= ~0x0001
entries -= 1
```

rename：

```text
file_050 -> renamed_050
```

可能发生：

```text
old bucket = 3
new bucket = 41
```

索引变化：

```text
bucket 41 增加 new name 所在 block bit
bucket 3 根据 old name 所在 block 是否还有同 bucket 项，决定是否清 bit
entries 先 +1 再 -1，最终不变
```

这就是 v6.5 的 directory index maintenance 语义。
