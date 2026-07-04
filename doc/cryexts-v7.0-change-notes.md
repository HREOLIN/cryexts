# CRYEXTS v7.0 变更说明

## 1. v7.0 的新定义

现在的 `v7.0` 已经不再沿用早期那种：

```text
USB demo baseline
```

的定义。

在 Version 7 需求重构之后，真正的第一阶段目标变成了：

```text
先突破单块 GDT 限制，建立可扩展容量的磁盘布局基线
```

所以这一版的核心不是新增用户可见功能，而是先把底层布局扩展能力立起来：

- 让 `mkfs.cryexts` 能真正写出多块 GDT
- 让当前尚未升级的 mount / fsck 路径“明确拒绝”多块 GDT，而不是误读
- 补齐 inspect / smoke / 文档，形成 Version 7 的新主线入口

一句话概括：

```text
v7.0 = Version 7 的多块 GDT 基线版
```

## 2. 这版具体改了什么

### 2.1 `tools/mkfs.cryexts.c`

`mkfs` 现在把 `group_desc_table_blocks` 当成真实布局参数来处理，不再默认它永远等于 `1`。

新的行为：

- 计算：

```text
gdt_blocks =
  ceil(group_count * sizeof(group_desc) / block_size)
```

- group0 的元数据布局不再写死在 `block 2 / 3 / 4`
- root group 的 block bitmap、inode bitmap、inode table、root dir 会整体后移
- GDT 会先在一段连续内存里组装，再一次性写出完整 GDT 区域
- `mkfs` 输出里新增 `GDT blocks:` 方便排查

这意味着：

- 当 group 数量超过单块 GDT 能容纳的上限时
- `mkfs` 不会再直接报：

```text
Current v4.1 mkfs supports only one GDT block
```

- 而是会真正把多块 GDT 写出来

### 2.2 `super.c`

当前内核挂载路径还没有升级成“读取完整多块 GDT”模型。

所以 v7.0 的策略不是“硬读”，而是“安全拒绝”：

- 校验 GDT 区域是否越界
- 校验 `group_desc_table_blocks * block_size` 是否足够容纳全部 descriptors
- 如果发现 `group_desc_table_blocks > 1`
  - 明确报错
  - 拒绝继续挂载

现在的报错语义是：

```text
multi-block GDT is not yet supported by mount path
```

这样做的目的很明确：

- 防止旧代码只读取第一块 GDT
- 导致后续 descriptor 被静默丢失
- 进而把整个文件系统解释错

### 2.3 `tools/cryextsck.c`

`cryextsck` 当前也还是单块 GDT 模型。

所以 v7.0 同样给它加了两层保护：

- 在 `validate_super()` 中校验完整 GDT 区域的大小是否合法
- 在真正读取 GDT 之前，如果发现 `group_desc_table_blocks > 1`
  - 明确报不支持
  - 不再假装只读第一块继续跑

当前拒绝信息是：

```text
multi-block GDT is not yet supported by cryextsck
```

这说明：

- `mkfs` 已经先走到 Version 7 的第一步
- 但 mount 和 fsck 还处在“兼容保护阶段”

### 2.4 新增工具 `tools/cryexts_gdt_inspect.c`

这个工具是 v7.0 里最重要的新检查工具。

它的作用是：

- 直接读取整段 GDT 区域
- 打印 superblock 里和 GDT 相关的关键字段
- 逐个打印 group descriptor
- 如果启用了 metadata checksum，再打印每个 group 的 stored / expected checksum

重点输出字段包括：

- `gdt_start`
- `gdt_blocks`
- `expected_gdt_blocks`
- `root_block_bitmap`
- `root_inode_bitmap`
- `root_inode_table_start`
- `root_dir_block`
- `group[i].start`
- `group[i].block_bitmap`
- `group[i].inode_table_start`

这个工具的意义是：

- 不依赖 mount
- 不依赖 cryextsck
- 先把“磁盘上到底写成什么样”直接看清楚

### 2.5 新增脚本 `scripts/smoke_v7_0_multi_gdt.sh`

这条脚本是新的 v7.0 主 smoke。

它验证的是：

1. 构造一个跨越单块 GDT 上限的大镜像
2. 用 `mkfs.cryexts -f -G -M` 格式化
3. 用 `cryexts_gdt_inspect` 检查：
   - `gdt_blocks > 1`
   - `expected_gdt_blocks == gdt_blocks`
   - root group 元数据位置已经右移
4. 再确认当前 `cryextsck` 会明确拒绝这个镜像

所以它验证的不是“挂载成功”，而是：

```text
mkfs 已经能写多块 GDT
老路径也会安全失败
```

这正是 v7.0 这个阶段应该达成的目标。

## 3. 这一版暂时还没做什么

v7.0 还没有实现：

- 内核多块 GDT 完整加载
- `sync_metadata()` 覆盖所有 GDT blocks
- 多块 GDT 下的 journal / checksum 全路径同步
- `cryextsck` 完整多块 GDT 读取与校验
- 以 raw-device / USB demo 作为 Version 7 主线目标

也就是说，这一版只是把：

```text
“能不能创建多块 GDT 文件系统”
```

先解决掉。

而：

```text
“内核能不能挂”
“fsck 能不能全理解”
```

会放到后续版本继续推进。

## 4. 为什么要这样分阶段

如果不先拆开，Version 7 会一直把几类问题混在一起：

- USB / raw-device 演示流程问题
- 大容量分区问题
- GDT 布局扩展问题
- mount / fsck 路径升级问题

这样做版本推进会很乱。

重构之后，Version 7 的顺序变成：

### `v7.0`

- `mkfs` 写多块 GDT
- 旧读取路径安全拒绝
- 补 inspect / smoke / 文档

### `v7.1`

- 内核多块 GDT 加载与同步

### `v7.2`

- `cryextsck` 多块 GDT 完整支持

### 更后面

- 再把 raw-device / USB demo 接回主线

## 5. 一句话总结

`v7.0` 解决的是：

```text
让 CRYEXTS 首次具备“写出多块 GDT 文件系统”的能力，
并且让尚未升级的读取路径明确、安全地失败。
```
