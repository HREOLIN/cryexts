# CRYEXTS v8.2 设计说明

## 1. v8.2 的定位

`v8.0` 解决的是：

```text
把仓库入口整理成一个像样的开源项目入口
```

`v8.1` 解决的是：

```text
把格式边界、feature 边界、兼容边界正式说清楚
```

而 `v8.2` 要解决的是第三个现实问题：

```text
现在虽然已经有很多 smoke 和恢复能力，
但它们还更像“作者自己熟悉的一组脚本”，
还不是一套对外可交付的测试矩阵与恢复矩阵
```

一句话定义：

```text
v8.2 = CRYEXTS 的测试矩阵与恢复矩阵基线版本
```

## 2. 为什么现在先做 v8.2

当前仓库已经有一长串 smoke 脚本：

- phase 系列
- `v2.x`
- `v3.x`
- `v4.x`
- `v5.x`
- `v6.x`
- `v7.x`

同时，项目还已经具备：

- mount-time replay
- orphan cleanup
- `cryextsck`
- `cryextsck --repair`
- 多种 inspect 工具

从能力上看，已经不缺“能测什么”。
缺的是：

```text
怎么把这些测试组织成一个外部读者看得懂、执行得了、验收得清楚的矩阵
```

### 2.1 当前 smoke 是“按版本堆叠”的，不是“按验收维度组织”的

现在的脚本命名主要是按版本线展开，例如：

- `smoke_v6_1_journal_transaction.sh`
- `smoke_v6_2_extent_tree.sh`
- `smoke_v7_3_usb_demo.sh`

这对作者自己很自然，
但对外部评估者不够友好，因为别人更关心：

- 我怎么验证格式路径
- 我怎么验证恢复路径
- 我怎么验证部署路径
- 我怎么知道失败后先看哪里

### 2.2 当前恢复能力已经存在，但缺少“恢复口径矩阵”

现在已经可以分辨多种恢复相关路径：

- mount-time journal replay
- orphan list cleanup
- `cryextsck` 一致性检查
- `cryextsck --repair` 低风险修复

但这些能力还没有整理成一张清楚的表：

```text
什么问题由 mount 负责
什么问题由 fsck 负责
什么问题只检测不修
什么问题要先备份镜像再动手
```

### 2.3 当前 inspect 工具已经不少，但排障入口还分散

现在已经有：

- `cryexts_extent_inspect`
- `cryexts_dir_index_inspect`
- `cryexts_policy_inspect`
- `cryexts_journal_inspect`
- `cryexts_alloc_inspect`
- `cryexts_xattr_inspect`
- `cryexts_gdt_inspect`

但还没有一份对外矩阵说明：

- 目录索引问题先看哪个
- GDT 问题先看哪个
- journal 问题先看哪个
- xattr 问题先看哪个

所以 `v8.2` 的目标很明确：

```text
不是新增测试能力
而是把已有测试与恢复能力组织成公开矩阵
```

## 3. v8.2 主目标

`v8.2` 建议只做两件事：

1. 建立测试矩阵
2. 建立恢复矩阵

拆开就是四个交付方向：

- 测试入口矩阵
- 验收结果矩阵
- 恢复责任矩阵
- 排障入口矩阵

## 4. v8.2 不做什么

为了防止继续发散，`v8.2` 明确不做：

- 重写现有 smoke 脚本体系
- 引入测试框架
- 引入 CI 平台耦合
- 新增复杂 fault injection 基础设施
- 新增 benchmark 系统
- 新增新的恢复算法

原因很简单：

```text
现有脚本和工具已经够用，
v8.2 先做的是“把已有能力编目和归类”，
不是再造一套新系统。
```

## 5. v8.2 要解决的四张矩阵

## 5.1 测试入口矩阵

这张矩阵要回答：

```text
如果我是第一次接触这个仓库，
我该从哪些脚本入口验证哪些能力
```

建议至少分成四类入口。

### A 类：格式与基础路径

建议覆盖：

- `mkfs`
- `mount`
- `umount`
- `cryextsck`

代表脚本：

- `smoke_v5_0_layout.sh`
- `smoke_v6_0_journal_layout.sh`
- `smoke_v7_2_multi_gdt_fsck.sh`

### B 类：结构能力路径

建议覆盖：

- extent tree
- sparse file
- dir index
- xattr
- multi-GDT

代表脚本：

- `smoke_v6_2_extent_tree.sh`
- `smoke_v6_3_sparse_file.sh`
- `smoke_v6_5_dir_index_maintenance.sh`
- `smoke_v6_6_large_xattr.sh`
- `smoke_v7_1_multi_gdt_mount.sh`

### C 类：恢复路径

建议覆盖：

- journal replay
- orphan cleanup
- fsck detect
- fsck repair

代表脚本：

- `smoke_v4_2_journal_replay.sh`
- `smoke_v5_1_orphan_list.sh`
- `smoke_v2_4_cryextsck.sh`
- `smoke_v6_1_journal_transaction.sh`

### D 类：部署路径

建议覆盖：

- image mode demo
- raw-device demo
- encrypted demo

代表脚本：

- `smoke_v7_3_usb_demo.sh`
- `smoke_v2_5_encryption.sh`
- `smoke_v5_4_policy_crypto.sh`

## 5.2 验收结果矩阵

这张矩阵要回答：

```text
每一类测试过了，到底代表什么能力已经被证明
```

这一步很重要，因为“脚本 pass”本身不够，
要把 pass 的含义写清楚。

### 示例

#### `smoke_v6_2_extent_tree.sh` pass

代表：

- extent tree 可以生成多 leaf
- 大文件映射可跨多 leaf
- truncate 后结构仍然自洽
- `cryextsck` 能理解并通过

#### `smoke_v7_1_multi_gdt_mount.sh` pass

代表：

- multi-GDT 镜像可 mount
- 第二块 GDT 覆盖到的 group 能参与真实运行时更新
- remount 后目录/文件仍然可验证

#### `smoke_v7_3_usb_demo.sh` pass

代表：

- 实际 image/raw-device 路径可构建、挂载、读写、卸载、复挂载
- demo 链路不是只停留在小镜像场景
- 最终 `cryextsck` 仍 clean

所以 `v8.2` 要把每条关键 smoke 的“pass 语义”补清楚。

## 5.3 恢复责任矩阵

这张矩阵要回答：

```text
出问题时，到底应该由谁负责恢复
```

建议至少明确分成四类。

### A 类：mount-time replay 负责的问题

典型例子：

- 已提交 journal transaction 需要在下次 mount 时回放
- recovery 状态仍然挂在 superblock / journal header 上

特点：

- 偏向“上次异常断电后的提交恢复”
- 发生在 mount 路径

### B 类：orphan cleanup 负责的问题

典型例子：

- 上次中断时残留 orphan inode
- 下次 mount 需要自动清理 orphan list

特点：

- 偏向“删除未完成后的清理”
- 发生在 mount 路径

### C 类：`cryextsck` 负责的问题

典型例子：

- bitmap / reference 不一致
- GDT / policy table / xattr / dir index 结构不自洽
- checksum mismatch

特点：

- 偏向“离线一致性检查”
- 适合 mount 失败后进入

### D 类：`cryextsck --repair` 可尝试修的问题

建议明确限制在：

- 低风险 recovery 状态修补
- 低风险 superblock / bitmap / journal header 残留修补

必须明确说明：

```text
--repair 不是“所有损坏都能修”
只承诺低风险修补
```

## 5.4 排障入口矩阵

这张矩阵要回答：

```text
出现不同类型问题时，第一眼应该看哪个工具
```

建议整理成对照表。

### 目录索引问题

优先看：

- `cryexts_dir_index_inspect`
- `cryextsck`

### extent / 大文件映射问题

优先看：

- `cryexts_extent_inspect`
- `cryextsck`

### policy / encryption 问题

优先看：

- `cryexts_policy_inspect`
- `cryextsck`

### journal / replay 问题

优先看：

- `cryexts_journal_inspect`
- `dmesg`
- `cryextsck`

### allocator / locality 问题

优先看：

- `cryexts_alloc_inspect`

### xattr 问题

优先看：

- `cryexts_xattr_inspect`
- `cryextsck`

### multi-GDT 问题

优先看：

- `cryexts_gdt_inspect`
- `cryextsck`

## 6. v8.2 建议产出的文档

`v8.2` 最终不应该只是这一份设计说明，
而应该至少沉淀出下面几类可交付文档。

## 6.1 测试矩阵文档

建议内容：

- 测试类别
- 对应脚本
- 运行前提
- pass 含义
- 失败后先看哪里

## 6.2 恢复矩阵文档

建议内容：

- 问题类型
- 首选处理路径
- 是否需要先做镜像备份
- 是否适合 `--repair`
- 是否只能检测不能修

## 6.3 排障速查文档

建议内容：

- 常见报错
- 优先查看的 inspect 工具
- `dmesg` 关注点
- 是否建议先离线 `cryextsck`

## 7. v8.2 对现有脚本体系的设计要求

`v8.2` 不要求重写脚本，
但会对脚本组织方式提出方向要求。

## 7.1 脚本要能归类

后续建议把现有脚本明确归入：

- 基础格式类
- 结构能力类
- 恢复类
- 部署类

这样以后就不是“只按版本找脚本”，
而是也能“按能力找脚本”。

## 7.2 总入口脚本要逐步形成验收入口

当前已经有：

- `smoke_version5_mvp.sh`
- `smoke_version6_mvp.sh`
- `smoke_version7_demo.sh`

后续建议继续形成：

- `version8` 验收入口

但 `v8.2` 先不急着实现复杂总控，
先把矩阵和口径整理清楚。

## 7.3 每条关键 smoke 都应该有“pass 语义”

这不是要求重写脚本，
而是要求文档必须解释：

```text
这个脚本通过后，到底意味着什么
```

否则矩阵就只是脚本目录的另一种抄写方式。

## 8. v8.2 推荐实施顺序

建议按最小顺序推进：

1. 先整理现有脚本分组
2. 再写测试矩阵
3. 再写恢复矩阵
4. 再写排障速查
5. 最后视需要增加一个 `version8` 验收总入口

这个顺序的原因很简单：

- 先整理现状
- 再形成对外说明
- 最后才考虑脚本聚合

## 9. v8.2 验收标准

`v8.2` 完成的最小标准建议定义为：

### 9.1 外部读者知道“该跑哪些脚本”

不再需要作者口头说明，
别人也能知道：

- 结构能力验证跑什么
- 恢复能力验证跑什么
- 部署验证跑什么

### 9.2 外部读者知道“pass 代表什么”

脚本通过不再只是一个绿色结果，
而是有明确能力语义。

### 9.3 外部读者知道“出问题先找谁”

即能分清：

- mount-time replay
- orphan cleanup
- `cryextsck`
- `cryextsck --repair`

各自负责什么。

### 9.4 外部读者知道“先看哪个工具”

即能根据问题类别快速定位：

- dir index
- extent
- journal
- xattr
- policy
- GDT

对应的 inspect 入口。

## 10. 一句话总结

如果说：

- `v8.0` 解决的是“怎么让别人看懂仓库入口”
- `v8.1` 解决的是“怎么让别人看懂格式边界”

那么：

```text
v8.2 解决的是：
怎么让别人看懂这套项目应该怎么测、怎么恢复、怎么排障
```
