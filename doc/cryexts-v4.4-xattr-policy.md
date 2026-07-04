# CRYEXTS V4.4 xattr 与 inode policy

## 1. 阶段目标

V4.4 的目标是把 Version 4 从“可恢复、可扩展的文件系统原型”继续推进到：

```text
带最小属性层和策略元数据钩子的文件系统原型
```

这次不做 ACL、不做完整多策略加密，也不做复杂安全模型，而是先做最小闭环：

- `user.*` xattr
- inode 级 `policy id`
- 目录 policy 继承
- `cryextsck` 能理解这些新元数据

## 2. 为什么先做这个

到 V4.3 为止，我们已经有：

- block groups
- metadata journal / replay
- inline extents regular file

接下来如果要往“按目录或按 inode 进行加密策略控制”走，仅靠 superblock 里的一个全局
`default_encryption_policy` 已经不够了。

所以 V4.4 的核心不是“马上实现多把 key”，而是先把磁盘格式和 inode 元数据准备好：

- 每个 inode 可以带 policy id
- 用户可以通过 xattr 给对象打标签
- 子文件可以继承父目录的默认策略

## 3. 当前实现范围

V4.4 当前实现的是最小 MVP：

- 新增 `XATTR` incompat feature
- 新增 `ENCRYPTION_POLICY` incompat feature
- 支持 `user.*` xattr
- 支持特殊键 `user.cryexts.policy_id`
- 新建 inode 时继承父目录的 policy id
- 如果父目录没有 policy，则继承 superblock 的 `default_encryption_policy`

当前没有做：

- ACL
- POSIX capability xattr
- `trusted.*` 真正落地
- SELinux / security.* 集成
- 多策略加密真正生效

## 4. inode 元数据布局

V4.4 没有新开 inode 扩展块，而是继续复用 inode 的 `reserved[]` 区域尾部 12 字节：

```text
reserved 尾部:
    xattr_block            8 bytes
    encryption_policy_id   4 bytes
```

这样做的好处：

- 不用重新设计 inode 主体结构
- 不会打散 V4.3 的 inline extent 区
- 可以先把 policy 和 xattr block 指针挂进去

## 5. xattr 数据块设计

V4.4 为每个 inode 最多挂一个 xattr block。

这个 block 是单块格式：

```text
xattr block
    header
    entry0
    entry1
    ...
```

当前 header 记录：

- magic
- entry_count
- used_bytes

每个 entry 记录：

- name_len
- namespace_id
- value_len
- name bytes
- value bytes

当前 namespace 只实现：

- `user.*`

## 6. 特殊 policy xattr

除了普通 `user.*` 键值对，这次还保留了一个特殊语义键：

```text
user.cryexts.policy_id
```

它不是普通字符串标签，而是直接映射到 inode 里的 `encryption_policy_id` 字段。

也就是说：

- `setxattr("user.cryexts.policy_id", "42")`
  实际上是在修改 inode policy id
- `getxattr("user.cryexts.policy_id")`
  实际上是在读取 inode policy id

这样做的好处是：

- 用户态测试非常直观
- 不需要额外发明 ioctl
- 以后把它真正接入加密策略时，也不需要推翻接口

## 7. 目录 policy 继承

V4.4 做的一个关键元数据钩子是：

```text
在某个目录下创建新 inode 时，
新 inode 默认继承父目录的 policy id
```

如果父目录没有显式 policy，则回落到：

```text
superblock.default_encryption_policy
```

这一步非常重要，因为它是以后做“目录级默认加密策略”的基础。

## 8. journal 与 xattr 的关系

xattr block 虽然不是普通文件数据，但它本质上属于 metadata。

所以在 V4.4 里：

- 改 xattr block 前，会先记录 journal
- 改 inode 里的 `xattr_block/policy_id` 前，inode table block 仍走现有 journal 保护

也就是说：

```text
V4.4 复用了 V4.2 的 recovery 机制
```

不会绕开现有 metadata journal。

## 9. `cryextsck` 需要理解什么

V4.4 之后，`cryextsck` 至少需要知道：

- xattr 和 policy feature flag 是合法的
- inode `inode_flags` 允许保留位
- inode 的 `xattr_block` 必须位于合法数据区
- xattr block 不能被多个 inode 重复引用

这一版 `cryextsck` 还没有深入解析每个 xattr entry 的所有语义，但已经先保证：

```text
xattr block 的所有权和基础布局不被忽略
```

## 10. smoke 测试设计

新增脚本：

- `scripts/smoke_v4_4_xattr_policy.sh`

它主要验证：

1. `mkfs.cryexts -A -P 7` 创建带 xattr / policy 特性的镜像
2. 挂载后给目录写入：
   - `user.note`
   - `user.cryexts.policy_id`
3. 在该目录下创建子文件
4. 验证子文件继承了父目录的 policy id
5. 给子文件写普通 `user.note`
6. remount 后再次验证：
   - xattr 仍然存在
   - policy id 仍然正确
7. `cryextsck` 最终仍然 clean

## 11. 当前边界

V4.4 现在仍然是最小属性层，不是完整安全模型。

当前明确没做：

- ACL 权限扩展
- 完整 namespace 分类
- xattr block 的多块扩展
- xattr block 的共享/去重
- xattr 与真正多策略加密联动

所以这版更准确的定位是：

```text
minimal xattr + inode policy metadata MVP
```

## 12. 下一步建议

V4.4 之后可以继续拆：

1. `v4.4.1`
   稳定 `user.*` xattr 路径
2. `v4.4.2`
   加强 `cryextsck` 对 xattr block 内容的校验
3. `v4.4.3`
   把 policy id 真正接入加密层选择逻辑
4. `v4.4.4`
   再考虑 `trusted.*`、更多策略字段、目录默认策略扩展

这样推进会比较稳，也方便逐步审核。
