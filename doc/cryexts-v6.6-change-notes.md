# CRYEXTS v6.6 变更说明

## 1. 这一版做了什么

`v6.6` 把 `Version 6` 路线图里的 large xattr / inspect / fsck 补强落到了代码上。

这一版完成的是：

- xattr 从单块模型升级到 `root + 1 overflow`
- `mkfs` 在 `v6 + xattr` 场景下打开 `LARGE_XATTR`
- `cryextsck` 能深度检查 xattr block 内容
- 新增 `cryexts_xattr_inspect`
- 新增 `smoke_v6_6_large_xattr.sh`

一句话总结：

```text
v6.6 = large xattr MVP + xattr inspect/fsck 可观测性补齐
```

## 2. 修改的文件

### 2.1 核心内核路径

- [cryexts_fs.h](/D:/Carl/cryptext4/cryexts/cryexts_fs.h:1)
- [xattr.c](/D:/Carl/cryptext4/cryexts/xattr.c:1)

### 2.2 工具链

- [tools/mkfs.cryexts.c](/D:/Carl/cryptext4/cryexts/tools/mkfs.cryexts.c:1)
- [tools/cryextsck.c](/D:/Carl/cryptext4/cryexts/tools/cryextsck.c:1)
- [tools/cryexts_xattr_inspect.c](/D:/Carl/cryptext4/cryexts/tools/cryexts_xattr_inspect.c:1)
- [Makefile](/D:/Carl/cryptext4/cryexts/Makefile:1)

### 2.3 测试与文档

- [scripts/smoke_v6_6_large_xattr.sh](/D:/Carl/cryptext4/cryexts/scripts/smoke_v6_6_large_xattr.sh:1)
- [doc/cryexts-v6.6-large-xattr.md](/D:/Carl/cryptext4/cryexts/doc/cryexts-v6.6-large-xattr.md:1)

## 3. 结构体字段变化

### 3.1 `struct cryexts_xattr_block_header`

位置：[cryexts_fs.h](/D:/Carl/cryptext4/cryexts/cryexts_fs.h:307)

新增字段：

- `overflow_block`
  含义：当前 xattr block 的 spill block 指针
  规则：
  - `0` 表示没有 overflow
  - 非 `0` 表示后面还有一块 xattr block

这意味着 `v6.6` 的 xattr 存储层次第一次具备了“块间跳转”的能力。

### 3.2 `CRYEXTS_XATTR_MAX_ITEMS`

位置：[cryexts_fs.h](/D:/Carl/cryptext4/cryexts/cryexts_fs.h:118)

新增常量：

- `CRYEXTS_XATTR_MAX_ITEMS = 32`

含义：

- 当前内核内存视图一次最多装载 32 个 xattr item
- 足够覆盖当前 `root + overflow` MVP

## 4. 函数级修改

### 4.1 `xattr.c`

新增/修改的关键函数：

- `cryexts_large_xattr_feature_enabled()`
  功能：判断 large xattr feature 是否开启。

- `cryexts_parse_xattr_block()`
  功能：解析一个 xattr block，并返回可能存在的 `overflow_block`。

- `cryexts_load_xattrs()`
  功能：按 `root -> overflow` 顺序装载 xattr。

- `cryexts_pack_xattr_block()`
  功能：把内存中的 xattr item 序列化为一个 block。

- `cryexts_write_xattrs()`
  功能：自动决定哪些 item 落在 root，哪些落在 overflow。

- `cryexts_free_xattr_storage()`
  功能：删除 inode 时同时回收 root 和 overflow。

### 4.2 `tools/cryextsck.c`

新增：

- `validate_xattr_block()`
  功能：深度校验单个 xattr block。

增强：

- inode 校验路径现在会继续检查：
  - root block 内容
  - overflow block 内容
  - overflow 是否越界
  - overflow 是否多重引用
  - overflow 是否继续链出第三块

### 4.3 `tools/mkfs.cryexts.c`

调整：

- 只有在 `fs_version >= v6` 且启用了 xattr 时，才设置
  `CRYEXTS_FEATURE_RO_COMPAT_LARGE_XATTR`

这样不会把旧版镜像错误地标成“large xattr capable”。

## 5. 一个最重要的设计决定

`v6.6` 没有把 overflow 指针塞回 inode `reserved[]`。

原因是：

- `v6.2 extent tree v2` 已经大量占用了 inode `reserved[]`
- regular file / dir / orphan / extent 元数据在那里已经很挤

所以这次选择的是：

```text
把 overflow 指针挂到 xattr block header 自己身上
```

这个方案的好处是：

- 不和 inode 布局打架
- 老镜像天然兼容
- regular file / dir / extent-tree-v2 都能直接复用

## 6. 当前版本的限制

`v6.6` 是有意收敛过范围的：

- 只支持一层 overflow
- overflow block 不允许再链下一个
- 单个 xattr value 不能跨块拆分

所以：

```text
large xattr 是支持了
无限扩展 xattr 还没有支持
```

## 7. 建议你在 Ubuntu 上怎么验证

运行：

```bash
chmod +x scripts/smoke_v6_6_large_xattr.sh
./scripts/smoke_v6_6_large_xattr.sh
```

重点观察：

- `cryexts_xattr_inspect` 是否打印 `root.overflow_block`
- `overflow.entries` 是否非 0
- remount 后全部 `getxattr` 是否还能读回
- `cryextsck` 是否 clean

## 8. 这版之后的自然下一步

如果后面继续推进 xattr 子系统，比较自然的顺序是：

1. `v6.7`
   做多级 overflow 或 xattr chain

2. `v6.8`
   做 xattr metadata checksum

3. `v6.9`
   做 xattr repair / scrub

这样路线会比较稳，不会一下子把 journal、fsck、repair 全缠在一起。
