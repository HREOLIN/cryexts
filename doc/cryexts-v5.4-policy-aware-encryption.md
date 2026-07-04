# CRYEXTS V5.4 策略化加密设计说明

## 1. 这一版解决什么问题

在 V4.4 到 V5.3 之间，`policy_id` 已经存在了，但它主要还是元数据：

- inode 里有 `encryption_policy_id`
- 目录可以把 policy 继承给新建子项
- `user.cryexts.policy_id` 可以读写

但是实际数据加密路径仍然只有一套“全文件系统统一 key”。

这意味着：

- 不同 inode 虽然 `policy_id` 不同
- 但 regular file / symlink 的数据块仍然走同一把加密 key

V5.4 做的事情，就是把这个缺口补上：

- 让 `policy_id` 真正进入数据加密路径
- 让 `policy table block` 真正落盘
- 让 mount 时真正加载 policy table
- 让 regular file / symlink 的数据按不同 policy 派生不同 key

一句话总结：

V5.4 把“policy 只是标签”推进成了“policy 真正影响数据如何加密”。

## 2. 整体思路

V5.4 仍然保留原来的两层结构：

1. 文件系统级主密钥
2. 基于 policy 的二级派生密钥

也就是说：

```text
mount key
-> superblock salt
-> filesystem master derived key
-> policy table entry(context + policy_id)
-> per-policy derived key
-> regular file / symlink data block encryption
```

所以现在不是“每个 inode 直接自带一把独立密钥”，而是：

- 先有全盘挂载密钥
- 再根据 `policy_id + policy context` 派生出不同的 policy key

## 3. 新增的 on-disk 结构

### 3.1 policy table block

V5.0 只是预留了：

- `superblock.policy_table_block`

但那时这个 block 还没有真正写进去。

V5.4 新增了真实结构：

- [cryexts_fs.h](/D:/Carl/cryptext4/cryexts/cryexts_fs.h:250) `struct cryexts_policy_table_block`
- [cryexts_fs.h](/D:/Carl/cryptext4/cryexts/cryexts_fs.h:243) `struct cryexts_policy_entry`

结构可以理解成：

```text
policy table block
├── magic
├── entry_count
└── entries[]
    ├── policy_id
    ├── flags
    └── context[8]
```

其中每个 `policy entry` 表示：

- 一个合法的 `policy_id`
- 以及它对应的派生上下文 `context`

### 3.2 superblock 的关系

superblock 里已有两个关键字段：

- `default_encryption_policy`
- `policy_table_block`

V5.4 让这两个字段真正联动起来：

- `default_encryption_policy` 必须存在于 policy table 中
- `policy_table_block` 必须是真实有效的数据块

## 4. mkfs 现在做了什么

V5.4 之后，`mkfs.cryexts` 在启用 `-T` 或 `-P` 时，不再只是打 feature flag。

它现在会真正：

1. 预留一个 `policy_table_block`
2. 把它标记进 block bitmap
3. 在 superblock 里写入 `policy_table_block`
4. 初始化整张 policy table
5. 把 root inode 的 `encryption_policy_id` 写成默认 policy

这样后续 mount 进内核时，就不是面对一个“空洞的 feature”，而是面对一张真实可加载的 policy 表。

## 5. mount 时怎么处理

挂载阶段新增了一条路径：

```text
read superblock
-> validate super
-> load bitmaps
-> verify mount key
-> load policy table
-> journal replay
-> orphan cleanup
-> normal mount
```

其中 `load policy table` 做的主要事情是：

- 读取 `policy_table_block`
- 校验 `magic`
- 校验 `entry_count`
- 校验 `policy_id` 不重复
- 为每个 policy 派生一把运行时 key
- 为每个 policy 初始化一个 `ctr(aes)` transform

这样到正常 I/O 阶段时，内核已经拥有：

- 文件系统 master key
- 每个 policy 对应的 runtime crypto context

## 6. regular file / symlink 数据加密怎么变化了

V5.4 之前，regular file 数据块的路径是：

- `cryexts_read_file_block()`
- `cryexts_write_file_block()`

这条路径只有一套全局 key。

V5.4 之后，regular file 和 symlink 的数据路径改成：

- `cryexts_read_inode_block()`
- `cryexts_write_inode_block()`

它们会先看：

- 这个 inode 的 `policy_id` 是多少
- 是否启用了 policy table
- 是否是 regular file / symlink

然后选择：

- 对应 policy 的 key 做 AES-CTR 加解密

所以现在如果两个文件：

- 明文完全一样
- 但 `policy_id` 不同

那么它们在磁盘上的密文就会不同。

## 7. 哪些路径仍然不走 policy key

这一点要说清楚。

V5.4 只把 policy-aware encryption 接进了：

- regular file data blocks
- symlink payload blocks

下面这些仍然不走 per-policy key：

- 目录块
- xattr block
- journal block
- inode table
- superblock / group metadata

也就是说，这一版解决的是“数据路径 policy 生效”，不是“所有元数据都按 policy 隔离加密”。

## 8. policy 变更约束

V5.4 新增了一个重要约束：

- 非空 regular file / symlink 不允许直接改 `policy_id`

原因很直接：

- 旧数据已经用原 policy 加密
- 如果中途直接改 policy
- 后续读路径会用新 policy 去解旧密文
- 数据就会被错误解密

所以当前策略是：

- 空文件、空 symlink 可以改 policy
- 已经有数据的 regular file / symlink 返回 `-EBUSY`

这是一个很保守但很稳的 MVP 方案。

## 9. `cryextsck` 现在会检查什么

V5.4 之后，`cryextsck` 增强了对 policy table 的理解。

它会检查：

- 开了 `POLICY_TABLE` feature 时，`policy_table_block` 不能为 0
- `policy_table_block` 必须落在合法 data area
- policy table 的 `magic` 正确
- `entry_count` 合法
- `policy_id` 不重复
- `default_encryption_policy` 必须存在于 policy table
- inode 的 `encryption_policy_id` 必须引用到 policy table 里存在的 policy

这意味着 `cryextsck` 现在不仅知道“有 policy table 这个概念”，还知道：

- inode 和 policy table 之间的引用关系是否自洽

## 10. inspect 和 smoke

### 10.1 inspect

V5.4 新增：

- [tools/cryexts_policy_inspect.c](/D:/Carl/cryptext4/cryexts/tools/cryexts_policy_inspect.c:1)

它会打印：

- `policy_table_block`
- `default_policy`
- `magic`
- `entries`
- 每个 `policy_id` 的 `context`

### 10.2 smoke

V5.4 新增：

- [scripts/smoke_v5_4_policy_crypto.sh](/D:/Carl/cryptext4/cryexts/scripts/smoke_v5_4_policy_crypto.sh:1)

这份 smoke 的目标是验证三件事：

1. policy table 真正落盘并可被 inspect
2. 默认 policy 会继承到新建目录和文件
3. 两个不同 policy 的文件写入相同明文后，raw image 里看不到明文

## 11. 当前边界

这版仍然是 MVP，暂时没有做：

- 文件名加密
- 元数据按 policy 加密
- 动态增删 policy table entry
- 用户态更新 policy context
- policy key rotation

所以 V5.4 的准确定位是：

- 多 policy 元数据真正进入数据加密路径
- 不是完整的企业级多租户加密体系

## 12. 一句话总结

V5.4 做成的核心闭环是：

- policy table 真正落盘
- mount 真正加载 policy
- inode policy 真正影响 regular file / symlink 的数据加密

这也是 `policy-aware encryption` 第一次真正从元数据层走进 I/O 数据路径。
