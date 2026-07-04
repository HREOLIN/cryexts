# CRYEXTS V5.4 代码改动说明

## 1. 这一版的目标

V5.4 的目标很明确：

- 让 `policy_id` 不再只是 inode 上的标签
- 让它真正参与 regular file / symlink 的数据加密

换句话说，V5.4 要把之前的：

- policy metadata

推进成：

- policy-aware encryption runtime

## 2. 改了哪些文件

### 2.1 `cryexts_fs.h`

文件：

- [cryexts_fs.h](/D:/Carl/cryptext4/cryexts/cryexts_fs.h:1)

新增内容主要有：

- `CRYEXTS_POLICY_TABLE_MAGIC`
- `CRYEXTS_POLICY_CONTEXT_LEN`
- `struct cryexts_policy_entry`
- `struct cryexts_policy_table_block`

这部分定义了 policy table 的真实磁盘格式。

### 2.2 `cryexts.h`

文件：

- [cryexts.h](/D:/Carl/cryptext4/cryexts/cryexts.h:1)

主要新增：

- `struct cryexts_policy_runtime`
- `struct cryexts_sb_info` 中的 policy runtime 数组
- policy table 相关函数声明
- inode-aware block I/O 接口声明

这些改动的目的，是让内核在 mount 后能缓存整张 policy table，并为每个 policy 持有自己的 crypto transform。

### 2.3 `tools/mkfs.cryexts.c`

文件：

- [tools/mkfs.cryexts.c](/D:/Carl/cryptext4/cryexts/tools/mkfs.cryexts.c:1)

这是 V5.4 非常关键的一处改动。

之前：

- `POLICY_TABLE` feature 只是标记
- `policy_table_block` 仍然是 0

现在：

- `mkfs` 会真正分配 `policy_table_block`
- 它会把这个 block 标记为已使用
- 会往里面写完整的 policy table header 和 entries
- root inode 的 `encryption_policy_id` 也会写成 `default_encryption_policy`

这使得 policy table 从“格式预留”变成了“真实存在的元数据块”。

### 2.4 `crypto.c`

文件：

- [crypto.c](/D:/Carl/cryptext4/cryexts/crypto.c:1)

这是 V5.4 的核心运行时改动。

主要新增了几块逻辑：

- `cryexts_policy_table_enabled()`
  判断当前文件系统是否启用了真实 policy table
- `cryexts_load_policy_table()`
  mount 时读取并解析 policy table block
- `cryexts_unload_policy_table()`
  umount 时释放每个 policy 的 crypto transform
- `cryexts_policy_exists()`
  运行时判断某个 policy id 是否存在
- `cryexts_derive_policy_key()`
  由文件系统 master key 再派生 per-policy key
- `cryexts_read_inode_block()`
  inode-aware 数据块读取
- `cryexts_write_inode_block()`
  inode-aware 数据块写入

这里的核心变化是：

- 加密不再只看“这个文件系统是否 encrypted”
- 还会看“这个 inode 属于哪个 policy”

然后用不同的 policy key 去跑 `AES-CTR`。

### 2.5 `super.c`

文件：

- [super.c](/D:/Carl/cryptext4/cryexts/super.c:1)

主要新增：

- mount 时在设置好 master key 后加载 policy table
- umount 时释放 policy runtime 资源
- V5 layout 校验中要求：
  - 开了 `POLICY_TABLE` feature 时，`policy_table_block` 不能为 0

这一步保证了：

- 不是只靠 inode 自己写个 policy id
- 而是整个 mount 期都真正拥有 policy 运行状态

### 2.6 `inode.c`

文件：

- [inode.c](/D:/Carl/cryptext4/cryexts/inode.c:1)

这版在 inode 侧补了两个关键点：

1. inode 校验时检查：
   - `encryption_policy_id` 必须存在于 policy table
2. 新建 inode 的 policy 继承逻辑调整为：
   - 父目录有非零 policy 时继承父目录
   - 否则回落到 superblock 的默认 policy

这个细节很重要，因为之前如果 root 没显式带 policy，后续创建链路会把 0 一路传下去，导致默认 policy 实际不生效。

### 2.7 `xattr.c`

文件：

- [xattr.c](/D:/Carl/cryptext4/cryexts/xattr.c:1)

这版把 `user.cryexts.policy_id` 的 setter 收紧了。

新增规则：

- 改成不存在的 policy id，拒绝
- 非空 regular file / symlink 改 policy，拒绝

原因是防止：

- 旧数据用旧 policy 加密
- 用户中途改 policy
- 读路径用新 policy 解旧密文

这种情况会直接把数据语义弄坏，所以当前选择最保守的做法。

### 2.8 `file.c`

文件：

- [file.c](/D:/Carl/cryptext4/cryexts/file.c:1)

这里的变化是把 regular file / symlink 数据路径切到 inode-aware I/O：

- `cryexts_read_file_block()` -> `cryexts_read_inode_block()`
- `cryexts_write_file_block()` -> `cryexts_write_inode_block()`

涉及路径包括：

- 普通 read/write
- symlink payload 读写
- truncate 时尾块清零

这一步是“policy 真正进数据路径”的落点。

### 2.9 `dir.c`

文件：

- [dir.c](/D:/Carl/cryptext4/cryexts/dir.c:1)

这里主要是把 symlink 创建时的内核内写入也切到 inode-aware block I/O。

否则普通 regular file 会按 policy 加密，但 symlink 还是走旧的全局 key，语义就不一致了。

### 2.10 `tools/cryextsck.c`

文件：

- [tools/cryextsck.c](/D:/Carl/cryptext4/cryexts/tools/cryextsck.c:1)

这版新增了 policy table 的检查逻辑：

- `validate_policy_table()`
- `policy_exists_in_table()`

主要检查：

- feature 开了但 block 没有，报错
- block 越界，报错
- magic 错，报错
- entry 数量非法，报错
- policy id 重复，报错
- default policy 不存在，报错
- inode 引用不存在的 policy，报错

这样 `cryextsck` 现在已经能理解：

- superblock -> policy table
- inode -> policy table

这两层关系。

### 2.11 新增 inspect 和 smoke

新增文件：

- [tools/cryexts_policy_inspect.c](/D:/Carl/cryptext4/cryexts/tools/cryexts_policy_inspect.c:1)
- [scripts/smoke_v5_4_policy_crypto.sh](/D:/Carl/cryptext4/cryexts/scripts/smoke_v5_4_policy_crypto.sh:1)

inspect 用来离线查看：

- policy table block 号
- default policy
- policy entry 数
- 每个 policy 的 context

smoke 用来验证：

1. policy table 真落盘
2. 默认 policy 真继承
3. 不同 policy 的相同明文不会以明文形式出现在 raw image

### 2.12 `Makefile`

文件：

- [Makefile](/D:/Carl/cryptext4/cryexts/Makefile:1)

主要补充：

- 把 `cryexts_policy_inspect` 加入 tools 构建目标

## 3. 这一版的核心运行逻辑

V5.4 的核心逻辑可以抽象成：

```text
mkfs
-> 写 superblock
-> 写 policy_table_block
-> root inode 带 default policy

mount
-> 验证 master key
-> 读取 policy table
-> 为每个 policy 派生 runtime key

regular file / symlink I/O
-> 取 inode.policy_id
-> 找到对应 policy runtime
-> 用该 policy key 跑 AES-CTR
```

## 4. 当前实现边界

V5.4 还没有做：

- policy table 在线更新
- policy key rotation
- metadata 按 policy 加密
- filename encryption
- 多用户 keyring 集成

所以这一版的准确定位是：

- `multi-policy encryption MVP`

不是最终形态。

## 5. 审代码时最值得看的点

如果你要审这一版，我建议重点看这几个地方：

1. `mkfs` 是否真的为 policy table 预留了块并写进 bitmap
2. root inode 默认 policy 是否真的写入
3. mount 时 policy table 是否在设置好 master key 后再加载
4. regular file / symlink 是否都切到了 inode-aware I/O
5. `xattr` 改 policy 的边界是否能阻止“旧密文配新 policy”
6. `cryextsck` 是否已经理解 inode -> policy table 的引用关系

## 6. 一句话总结

V5.4 的本质改动不是“又多了一个 metadata block”，而是：

- 让 policy table 真正落盘
- 让 policy id 真正决定 regular file / symlink 的加密行为

这也是 V5 阶段里 policy-aware encryption 第一次真正闭环。
