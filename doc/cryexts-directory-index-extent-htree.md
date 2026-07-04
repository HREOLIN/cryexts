# CRYEXTS 目录索引、Extent 和 HTree 的关系

## 1. 先给一句话结论

这三个东西解决的不是同一个问题：

```text
directory index 负责“名字查找”
extent 负责“数据块映射”
HTree 只是 directory index 的一种更成熟的组织方式
```

所以它们不是互相替代关系，而是分层关系。

---

## 2. 三者分别干什么

### 2.1 Directory index

目录索引的任务是：

```text
给定一个文件名，快速缩小要扫描的目录数据块范围
```

在 CRYEXTS v5.3/v6.5 里，它长这样：

```text
name -> hash(name) -> bucket -> block mask -> 候选目录块
```

它不直接保存“文件名 -> inode”的完整映射，只保存：

```text
这个 bucket 可能出现在目录的哪些 logical block 里
```

### 2.2 Extent

Extent 的任务是：

```text
把文件的 logical block / logical range 映射到 physical block
```

它解决的是“文件数据放在哪块盘上”。

例如：

```text
logical block 0..1023 -> physical block 5000..6023
```

Extent 关心的是数据布局，不关心文件名。

### 2.3 HTree

HTree 可以理解成：

```text
一种更标准、更可扩展的 directory index 树
```

它解决的还是目录查找问题，但比“单个 hash index block + mask”更强：

```text
root -> interior -> leaf -> dirent
```

它不是 extent 的子集，也不是 extent 的替代品。

---

## 3. 最容易混淆的点

很多人会把 extent 和 HTree 都叫“tree”，于是会以为它们在做同一件事。

其实不是。

### 3.1 Extent tree 是“存储映射树”

它回答的是：

```text
这个文件的第 N 个逻辑块，对应磁盘上的哪一个物理块？
```

### 3.2 HTree / directory index 是“名字索引树”

它回答的是：

```text
这个名字可能藏在哪些目录块里？
```

### 3.3 它们的输入输出不同

```text
Extent:
  输入 = logical offset
  输出 = physical block

Directory index / HTree:
  输入 = file name hash
  输出 = candidate directory blocks
```

所以它们虽然都像树，但树的“坐标系”完全不同。

---

## 4. 在 CRYEXTS 里它们怎么配合

CRYEXTS 当前的目录查找路径可以理解成三层：

```text
文件名查找层
    -> directory index / hash bucket
目录块定位层
    -> inode block map
数据块定位层
    -> physical block
```

也就是：

```text
lookup("foo")
-> hash("foo")
-> bucket = hash % 64
-> block_masks[bucket]
-> 只扫候选目录 logical block
-> cryexts_inode_block_at(dir, logical_block)
-> 读 physical block
-> 比较 dirent.name
```

这里可以看到：

- directory index 只负责缩小扫描范围
- inode 的 block mapping 负责把 logical block 变成 physical block
- extent 只是在“数据块映射”那一层可能出现的一种更强形式

---

## 5. 目录索引和 extent 的关系

### 5.1 目录索引不等于 extent

目录索引里没有“logical range -> physical range”的语义。

它不是在描述连续数据映射，而是在描述：

```text
bucket A 的名字，可能落在哪些 directory block 里
```

这和 extent 的“范围映射”完全不同。

### 5.2 目录索引可以依赖任何 block mapping

目录索引只关心“目录 logical block”。

这些 logical block 背后的物理位置，可以由不同机制提供：

- 直接块
- 单级间接块
- extent
- extent tree

所以从设计上说，directory index 和 extent 是解耦的：

```text
index 负责找块
mapping 负责把块落到盘上
```

### 5.3 在当前代码里，目录和 extent 的实际关系

在这套 CRYEXTS 代码里：

- regular file 已经在走 extent / extent tree 方向
- directory 现在主要还是按目录块映射来工作
- directory index 只是给目录查找加速，不替代目录块本身的存储结构

也就是说：

```text
extent 主要管文件内容
directory index 主要管名字查找
```

---

## 6. HTree 和当前 directory index 的区别

### 6.1 当前 CRYEXTS directory index

当前实现更像：

```text
单层 hash index + block mask
```

它的结构很简单：

```c
struct cryexts_dir_index_block {
    __le32 magic;
    __le16 buckets;
    __le16 dir_blocks;
    __le32 entries;
    __u8 reserved[16];
    __le16 block_masks[64];
};
```

含义是：

- 每个 bucket 记录哪些目录块可能含有这个 bucket 的名字
- 查找时只扫这些目录块

### 6.2 HTree 更像什么

HTree 通常会有更明显的层次：

```text
root
  -> leaf
     -> dirent
```

它可以在大目录下进一步分裂、扩展、重新平衡。

### 6.3 为什么我们现在还不是完整 HTree

因为当前实现还没有：

- root / leaf 的完整分裂
- bucket split / rebalance
- 多层目录索引节点
- 动态增长的索引树维护算法

所以现在更准确的名字是：

```text
hash-based directory index MVP
```

而不是完整 HTree。

---

## 7. 一个直观例子

假设目录 `/bigdir` 里有很多名字。

### 7.1 directory index 做什么

查找：

```text
lookup("file_123")
```

流程：

```text
1. hash("file_123")
2. bucket = hash % 64
3. 读 block_masks[bucket]
4. 只扫描这些目录 logical block
5. 在候选块里逐个比对 dirent.name
```

### 7.2 extent 做什么

假设这个目录本身有 4 个目录块。

extent / block map 做的是：

```text
logical block 0 -> physical block 100
logical block 1 -> physical block 205
logical block 2 -> physical block 311
logical block 3 -> physical block 412
```

index 并不知道这些 physical block 的细节，它只知道：

```text
bucket 17 可能在 logical block 1 和 3
```

真正读取时，再靠 inode 的映射把 logical block 变成 physical block。

---

## 8. 为什么会觉得它们像

因为它们都在做“缩小搜索范围”。

但缩小的对象不同：

- extent 缩小的是“数据位置搜索”
- directory index / HTree 缩小的是“目录项搜索”

这也是为什么它们都长得像树，却不是同一种树。

---

## 9. 你可以这样记

最简单的记法是：

```text
extent = 文件内容的地图
directory index = 文件名的导航
HTree = 更强的目录导航结构
```

再展开一点：

```text
extent 解决“我该去哪里读/写数据”
directory index 解决“我该去哪里找这个名字”
HTree 解决“目录太大时，怎么把名字查找继续做快”
```

---

## 10. 对应到代码的实际结论

在 CRYEXTS 当前实现里：

- extent 和 directory index 是两条平行线
- directory index 不需要知道 extent 的内部结构
- extent 也不需要知道 directory index 的 bucket mask
- 只有当目录块的实际位置改变时，它们才会在“物理块映射”这一层碰到同一个问题

所以你可以把它们理解成：

```text
extent 是存储层
directory index 是命名空间层
HTree 是命名空间层里更高级的一种组织方式
```

---

## 11. hash、bucket、block_masks[bucket] 到底是什么

这一节专门拆开解释：

```text
hash(name)
-> bucket
-> block_masks[bucket]
-> candidate directory logical blocks
```

### 11.1 hash 是什么

hash 的作用是把一个文件名变成一个数字。

例如：

```text
"file_a" -> hash = 0x12345678
"file_b" -> hash = 0x91abcdef
"dir_x"  -> hash = 0x00aa7711
```

在 CRYEXTS 代码里，hash 入口是：

```c
static u32 cryexts_dir_hash(struct super_block *sb,
                            const char *name,
                            size_t len);
```

它使用类似 FNV-1a 的方式：

```text
初始 hash = 2166136261
先混入 superblock 的 dir_index_seed
再逐字节混入文件名
每混入一个字节，就乘以 16777619
```

为什么要混入 `dir_index_seed`：

```text
同一个文件名，在不同 filesystem image 上可以得到不同 hash 分布
```

这可以避免所有文件系统都用完全一样的 hash 分布。

### 11.2 bucket 是什么

bucket 是 hash 的分组结果。

当前 CRYEXTS 固定有：

```text
CRYEXTS_DIR_INDEX_BUCKETS = 64
```

所以：

```text
bucket = hash(name) % 64
```

例子：

```text
hash("file_a") = 1000
bucket = 1000 % 64 = 40

hash("file_b") = 1001
bucket = 1001 % 64 = 41

hash("dir_x") = 1064
bucket = 1064 % 64 = 40
```

注意：

```text
不同名字可能落到同一个 bucket
```

这就是 hash collision / bucket collision。

所以 bucket 不是最终答案，它只是缩小搜索范围。

### 11.3 block_masks[bucket] 是什么

`block_masks[bucket]` 是一个 16 bit 位图。

它记录：

```text
这个 bucket 的名字，可能出现在哪些 directory logical block 里
```

例如：

```text
block_masks[40] = 0x0005
```

二进制：

```text
0000 0000 0000 0101
```

含义：

```text
bit0 = 1 -> directory logical block 0 里有 bucket 40 的名字
bit2 = 1 -> directory logical block 2 里有 bucket 40 的名字
```

所以查询 bucket 40 的名字时，只需要扫描：

```text
logical block 0
logical block 2
```

不用扫描整个目录。

### 11.4 block_masks 不是存 inode

这一点很重要。

`block_masks[bucket]` 不存：

```text
name -> inode
```

它只存：

```text
bucket -> candidate logical blocks
```

真正找到 inode 仍然要进入目录块里逐个比较：

```text
dirent.name == target name
```

匹配成功后，才读取：

```text
dirent.inode
```

---

## 12. block_mask 是如何从空目录构建出来的

这里用一个目录从空到变大的过程讲。

为了方便理解，假设目录最多有这些 logical block：

```text
logical block 0
logical block 1
logical block 2
...
```

当前 CRYEXTS 的 mask 是 16 bit，所以最多表达：

```text
logical block 0..15
```

当前目录实现本身最多是 12 个 direct directory blocks，所以 16 bit 够用。

### 12.1 空目录刚创建时

目录刚创建时，不是真的完全空。

它至少有：

```text
.
..
```

目录数据块大概是：

```text
logical block 0:
  "."
  ".."
```

构建 index 时，会扫描 block 0。

假设：

```text
hash(".")  % 64 = 3
hash("..") % 64 = 9
```

那么：

```text
block_masks[3] |= 1 << 0
block_masks[9] |= 1 << 0
```

也就是：

```text
block_masks[3] = 0x0001
block_masks[9] = 0x0001
```

其他 bucket 还是 0。

### 12.2 新增第一个文件

新增：

```text
touch a.txt
```

这个目录项通常还能放在 logical block 0。

假设：

```text
hash("a.txt") % 64 = 17
```

那么 index 变成：

```text
block_masks[17] |= 1 << 0
```

结果：

```text
block_masks[17] = 0x0001
```

意思是：

```text
bucket 17 的名字可能在 logical block 0
```

### 12.3 继续新增很多文件，但还在 block 0

假设继续新增：

```text
b.txt
c.txt
d.txt
```

它们也都还在 logical block 0。

如果：

```text
hash("b.txt") % 64 = 17
hash("c.txt") % 64 = 20
hash("d.txt") % 64 = 20
```

那么：

```text
block_masks[17] |= 1 << 0
block_masks[20] |= 1 << 0
block_masks[20] |= 1 << 0
```

注意：

```text
block_masks[17] 原来已经是 0x0001
再次 OR 0x0001 后，仍然是 0x0001
```

这说明：

```text
同一个 bucket 在同一个 logical block 里有多个名字，mask 只记录一次
```

所以 `entries` 可能很多，但 `mask_refs` 不一定很多。

### 12.4 block 0 放满后，目录扩展到 block 1

当 logical block 0 放不下新的 dirent 时，目录会分配新的 directory block。

现在目录变成：

```text
logical block 0:
  .
  ..
  a.txt
  b.txt
  c.txt
  d.txt
  ...

logical block 1:
  x.txt
```

假设：

```text
hash("x.txt") % 64 = 17
```

那么：

```text
block_masks[17] |= 1 << 1
```

如果原来：

```text
block_masks[17] = 0x0001
```

现在变成：

```text
block_masks[17] = 0x0003
```

二进制：

```text
0000 0000 0000 0011
```

含义：

```text
bucket 17 的名字可能在 logical block 0
bucket 17 的名字也可能在 logical block 1
```

### 12.5 目录继续增长到 block 2

新增：

```text
y.txt
```

它写入 logical block 2。

假设：

```text
hash("y.txt") % 64 = 17
```

那么：

```text
block_masks[17] |= 1 << 2
```

结果：

```text
block_masks[17] = 0x0007
```

二进制：

```text
0000 0000 0000 0111
```

含义：

```text
bucket 17 可能出现在 logical block 0 / 1 / 2
```

这就是 block mask 一点点长出来的过程。

---

## 13. 查询时如何使用 block_masks[bucket]

假设现在要查：

```text
lookup("y.txt")
```

流程是：

```text
hash("y.txt") % 64 = 17
candidate_mask = block_masks[17]
```

如果：

```text
candidate_mask = 0x0007
```

就说明需要扫描：

```text
logical block 0
logical block 1
logical block 2
```

代码语义是：

```c
if (candidate_mask && !(candidate_mask & (1U << i)))
    continue;
```

也就是：

```text
如果当前 logical block i 不在 mask 里，就跳过
```

扫描候选块时，仍然要逐个比较名字：

```text
de->name_len == name->len
memcmp(de->name, name->name, name->len) == 0
```

找到后返回 `de->inode`。

---

## 14. 删除时 block_mask 怎么维护

删除比新增更容易出错。

假设：

```text
logical block 2:
  y.txt -> bucket 17
  z.txt -> bucket 17
```

当前：

```text
block_masks[17] = 0x0007
```

现在删除：

```text
unlink("y.txt")
```

不能直接：

```text
block_masks[17] &= ~(1 << 2)
```

因为 `z.txt` 还在 logical block 2，也属于 bucket 17。

所以 v6.5 的逻辑是：

```text
1. 先清空 y.txt 的 dirent
2. 重新扫描 logical block 2
3. 看 block 2 里还有没有 bucket 17 的 live dirent
4. 如果还有，bit2 保持 1
5. 如果没有，bit2 才清 0
```

对应代码是：

```c
cryexts_dir_index_remove_name()
```

这就是为什么删除不能只看被删除的那个名字，而必须看同一个 block 里是否还有同 bucket 的其他名字。

---

## 15. 当前 CRYEXTS 和真正 HTree 的维护差异

当前 CRYEXTS 还不是完整 HTree，它更像：

```text
single index block
+ 64 buckets
+ 16-bit block masks
```

维护方式是：

```text
新增名字 -> 设置 bucket 的 block bit
删除名字 -> 必要时清 bucket 的 block bit
rename -> add new name + delete old name
```

真正 HTree 的维护会更复杂。

### 15.1 真正 HTree 从空目录开始

小目录阶段，可能还是线性目录：

```text
directory block 0:
  .
  ..
  a
  b
  c
```

当目录变大后，文件系统会建立 HTree root。

root 里不再只是简单 bitmask，而是类似：

```text
hash range -> child block
```

例如：

```text
root:
  hash >= 0x00000000 -> leaf block A
  hash >= 0x40000000 -> leaf block B
  hash >= 0x80000000 -> leaf block C
```

### 15.2 HTree 新增文件时

新增：

```text
new_file
```

流程大概是：

```text
1. 计算 hash(new_file)
2. 在 root 中找到对应 hash range
3. 进入对应 leaf
4. 把 dirent 放入 leaf 指向的目录数据块
5. 如果 leaf / bucket 太满，就 split
```

### 15.3 HTree split 是什么

假设某个 leaf 太满：

```text
leaf A:
  hash 0x1000 name a
  hash 0x1100 name b
  hash 0x1200 name c
  ...
```

继续插入放不下了，就会分裂：

```text
leaf A 拆成 leaf A 和 leaf B
一部分 hash 留在 A
另一部分 hash 移到 B
root 增加一条指向 B 的索引项
```

root 从：

```text
hash >= 0x0000 -> leaf A
```

变成：

```text
hash >= 0x0000 -> leaf A
hash >= 0x1200 -> leaf B
```

这才是 HTree 真正的“树维护”。

### 15.4 当前 CRYEXTS 为什么还没做到这个

因为当前 CRYEXTS 的 index block 只有：

```text
bucket -> block mask
```

它没有：

```text
hash range -> child index node
```

也没有：

```text
leaf split
root split
rebalance
```

所以当前版本更像是 HTree 之前的 MVP：

```text
先用 hash 把目录扫描范围缩小
暂时不做真正树分裂
```

---

## 16. 从空目录到 N 个文件，当前 CRYEXTS 的完整维护过程

可以把过程压缩成这个表：

```text
阶段 1: mkdir dir
  创建 logical block 0
  写入 . 和 ..
  计算 . / .. 的 bucket
  设置 block_masks[bucket] bit0

阶段 2: 创建少量文件
  dirent 继续放在 logical block 0
  对每个 name 计算 bucket
  设置 block_masks[bucket] bit0

阶段 3: block 0 放满
  分配 logical block 1
  新 dirent 写入 block 1
  设置 block_masks[bucket] bit1

阶段 4: 目录继续变大
  分配 logical block 2 / 3 / ...
  新名字落在哪个 logical block
  就设置对应 bucket 的对应 bit

阶段 5: 删除文件
  清空 dirent
  扫描同 logical block
  如果同 bucket 已经没有其他名字
  才清掉对应 bit

阶段 6: rename
  先 add 新名字，设置新 bucket bit
  再 delete 老名字，必要时清老 bucket bit
```

一句话总结：

```text
block_mask 不是预先算好的，而是目录项写入哪个 logical block，就把该名字 bucket 对应的 bit 设置起来。
```

这就是 `block_masks[bucket]` 的构建过程。
