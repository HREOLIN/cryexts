# CRYEXTS V4.4 修改说明

## 1. 这次改动的主线

V4.4 这次我做的不是完整 ACL / 多策略加密，而是一个最小元数据闭环：

- `user.*` xattr
- inode `policy_id`
- 目录 policy 继承
- xattr block 的磁盘布局
- `cryextsck` / `mkfs` / kernel 对这些元数据的一致理解

## 2. 盘格式层改了什么

文件：

- `cryexts_fs.h`

新增了：

- `CRYEXTS_FEATURE_INCOMPAT_XATTR`
- `CRYEXTS_FEATURE_INCOMPAT_ENCRYPTION_POLICY`
- `CRYEXTS_INODE_FLAG_IMMUTABLE`
- `CRYEXTS_INODE_FLAG_APPEND_ONLY`
- `struct cryexts_inode_extra`
- `struct cryexts_xattr_block_header`
- `struct cryexts_xattr_entry`

### 2.1 为什么这样做

这次的核心思路是：

1. feature flag 必须先明确磁盘支持的能力
2. inode 必须预留自己的 policy 元数据
3. xattr block 必须有稳定的单块格式

### 2.2 inode extra 放在哪里

没有新开 inode 扩展结构，而是把 inode `reserved[]` 的最后 12 字节固定为：

- `xattr_block`
- `encryption_policy_id`

这样 extent 区和 extra trailer 就能共存。

## 3. 内存 inode 层改了什么

文件：

- `cryexts.h`
- `inode.c`

新增了内存态字段：

- `xattr_block`
- `encryption_policy_id`

新增了辅助接口：

- `cryexts_inode_xattr_block()`
- `cryexts_inode_policy_id()`
- `cryexts_set_inode_policy_id()`

### 3.1 初始化逻辑

在 `cryexts_init_inode_blocks()` 里：

- 先读 extent / legacy 映射信息
- 再从 inode trailer 里读出：
  - `xattr_block`
  - `encryption_policy_id`

这样 VFS inode 一加载，就已经知道：

- 这个 inode 有没有独立 xattr block
- 这个 inode 当前 policy id 是多少

### 3.2 落盘逻辑

在 `cryexts_write_inode_to_disk()` 里：

- 先按 extent 或 legacy 方式写映射信息
- 再把 trailer 写回：
  - `xattr_block`
  - `encryption_policy_id`

这保证 xattr / policy 元数据和 inode 本体同步持久化。

### 3.3 新 inode 创建时的 policy 继承

在 `cryexts_new_inode()` 里增加了继承逻辑：

- 如果父目录 inode 已经有 policy id
  - 新 inode 继承父目录 policy
- 否则回落到 superblock 的 `default_encryption_policy`

也就是说，V4.4 开始已经有了目录默认策略继承钩子。

## 4. xattr 子系统改了什么

文件：

- `xattr.c`

这是这次新增的核心模块。

### 4.1 主要职责

它负责：

- 解析 xattr block
- 组装 xattr block
- 支持 `getxattr`
- 支持 `setxattr`
- 支持 `listxattr`
- 释放 xattr block

### 4.2 当前 xattr 存储模型

当前每个 inode 最多一个 xattr block。

block 里面的结构是：

- 一个 header
- 多个 entry

entry 记录：

- name_len
- namespace_id
- value_len
- name bytes
- value bytes

目前只支持：

- `user.*`

### 4.3 特殊键 `user.cryexts.policy_id`

这个键不是普通 xattr，而是映射到 inode 里的 `encryption_policy_id`。

所以：

- `getxattr(user.cryexts.policy_id)` 读取的是 inode policy 字段
- `setxattr(user.cryexts.policy_id)` 修改的是 inode policy 字段

这一步让 policy 元数据有了一个标准用户态入口。

### 4.4 journal 关系

`xattr.c` 在写已有 xattr block 前会调用：

- `cryexts_journal_record_block()`

也就是说 xattr block 被当作 metadata 保护，而不是普通文件数据随便写。

### 4.5 删除路径

新增了：

- `cryexts_free_xattr_storage()`

然后在 `dir.c` 的删除释放路径里，把它并入 inode 最终释放流程。

这样删除 inode 时：

- 文件数据块释放
- xattr block 也会一起释放

避免泄漏。

## 5. VFS 接线改了什么

文件：

- `super.c`
- `file.c`
- `dir.c`

### 5.1 super.c

在挂载完成后增加：

- `sb->s_xattr = cryexts_xattr_handlers`

这样 VFS 才能把 `getxattr/setxattr` 路由到 CRYEXTS。

### 5.2 inode operations

给：

- regular file
- symlink
- directory

都补了：

- `.listxattr = cryexts_listxattr`

这样用户态才能列出 xattr 名称。

## 6. mkfs 改了什么

文件：

- `tools/mkfs.cryexts.c`

新增参数：

- `-A`
- `-P policy_id`

含义：

- `-A`
  开启 xattr / policy 特性
- `-P`
  设置 superblock 默认 policy id，并隐式开启 `-A`

同时会把 feature flag 写进 superblock：

- `XATTR`
- `ENCRYPTION_POLICY`

## 7. cryextsck 改了什么

文件：

- `tools/cryextsck.c`

### 7.1 feature flag

现在 `cryextsck` 已经接受：

- `CRYEXTS_FEATURE_INCOMPAT_XATTR`
- `CRYEXTS_FEATURE_INCOMPAT_ENCRYPTION_POLICY`

### 7.2 inode 校验

这次新增了对 inode trailer 的基础检查：

- `xattr_block` 是否位于合法数据区
- xattr block 是否与别的 inode 重复引用
- inode flags 是否只用了允许的位

这一步还没有做到深度解析每个 xattr entry 的全部语义，但已经保证：

```text
xattr block 不会完全脱离 fsck 视野
```

## 8. smoke 测试设计

文件：

- `scripts/smoke_v4_4_xattr_policy.sh`

测试内容包括：

1. 创建带 `-A -P 7` 的镜像
2. 挂载后给目录设置：
   - `user.note`
   - `user.cryexts.policy_id = 42`
3. 在该目录下创建子文件
4. 检查子文件是否继承 `42`
5. 给子文件设置普通 `user.note`
6. remount 后再次检查：
   - `policy_id`
   - 普通 `user.note`
7. 最终 `cryextsck` 仍然 clean

## 9. 当前实现边界

这次刻意没做：

- ACL
- `trusted.*`
- 多块 xattr storage
- xattr block 深度 fsck 修复
- policy id 真正参与加密算法选择

所以它仍然是：

```text
policy-ready metadata MVP
```

## 10. 审核重点

你这次审核可以重点盯这几件事：

1. inode trailer 的 12 字节布局是否稳定
2. extent 区和 trailer 是否互不覆盖
3. xattr block 在删除路径是否能正确回收
4. `user.cryexts.policy_id` 是否真的映射到 inode policy 字段
5. 新 inode 创建时 policy 继承是否只发生在创建时，而不是读时伪造
6. `mkfs`、kernel、`cryextsck` 是否都认识同一组 feature flag

如果这 6 点成立，V4.4 这条主线就算成立了。
