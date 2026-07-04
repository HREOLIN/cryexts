# CRYEXTS V5.0 盘格式基线

## 1. 这一阶段做了什么

V5.0 不是 orphan list、不是 extent tree、也不是 directory index 的真正实现版。

这一阶段只做一件很关键的事：

```text
把 Version 5 后续要用到的磁盘格式入口先稳定下来
```

也就是说，V5.0 的定位是：

```text
Version 5 on-disk format baseline
```

## 2. 当前新增了哪些 superblock 字段

V5.0 在现有 v4 superblock 基础上，新增并正式预留了：

- `orphan_head`
- `policy_table_block`
- `dir_index_seed`
- `metadata_csum_type`
- `journal_sequence`
- `fs_generation`

这些字段当前的作用是：

- 让 `mkfs` 能写出 v5 格式
- 让内核 mount 路径能识别和校验
- 让 `cryextsck` 也能理解这些字段

注意：

```text
现在是“格式先到位”
不是“功能已经完全生效”
```

## 3. 当前新增了哪些 feature flags

### compat

- `PREALLOC`

### incompat

- `DIR_INDEX`
- `ORPHAN_LIST`
- `POLICY_TABLE`
- `EXTENT_TREE`

### ro_compat

- `METADATA_CSUM`
- `LARGE_XATTR`

这些 flag 的意义不是“现在全实现了”，而是：

```text
Version 5 以后这些能力有正式的磁盘格式声明位了
```

## 4. V5.0 当前真实生效的内容

### 4.1 内核 mount 校验

内核现在已经能识别：

- v5 版本号
- 新 compat/incompat/ro_compat 位是否合法
- `orphan_head` 是否越界
- `policy_table_block` 是否落在合法 data area
- `metadata_csum_type` 和 ro_compat flag 是否匹配
- `fs_generation` 是否有效

### 4.2 mkfs

`mkfs.cryexts` 新增了这些开关：

- `-I`
  打开 `DIR_INDEX` feature
- `-O`
  打开 `ORPHAN_LIST` feature
- `-T`
  打开 `POLICY_TABLE` feature
- `-M`
  打开 `METADATA_CSUM` feature

当前它们主要作用是：

- 初始化对应 feature flag
- 初始化对应 superblock 预留字段

### 4.3 cryextsck

`cryextsck` 现在也已经理解：

- v5 superblock 新字段
- v5 feature flags
- 新字段和 flag 的基本一致性关系

## 5. 当前还没有做的内容

V5.0 明确还没做：

- orphan list 真实挂接和清理
- directory index 真实 lookup 路径
- extent tree overflow block
- policy table 真正进入加密路径
- metadata checksum 真实覆盖所有结构

所以正确理解应当是：

```text
V5.0 = 先把未来能力的盘格式门牌号挂好
```

而不是：

```text
这些能力已经实现完毕
```

## 6. 当前 smoke 在验证什么

新增脚本：

- `scripts/smoke_v5_0_layout.sh`

它主要验证：

1. `mkfs.cryexts` 能创建 Version 5 image
2. `cryextsck` 能识别并检查 Version 5 image
3. kernel 能 mount / umount Version 5 image
4. 再次 `cryextsck` 仍然 clean

## 7. 为什么 V5.0 值得单独做

因为后面这些版本都要吃这套格式基线：

- V5.1 orphan list
- V5.2 extent tree
- V5.3 directory index
- V5.4 policy-aware encryption
- V5.5 metadata checksum

如果不先把 v5 的格式和 feature flag 约定定下来，后面每一阶段都会反复返工。

## 8. 下一步建议

V5.0 通过之后，最推荐的下一步就是：

```text
V5.1 orphan list
```

因为它最贴近你现在已经有的 journal / recovery 主线，而且最能提升 crash recovery 的真实质量。
