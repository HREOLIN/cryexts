# CRYEXTS v9.4 健康检查与可观测性总览

## 1. v9.4 的定位

`v9.0` 解决的是：

```text
把部署基线固定下来
```

`v9.1` 解决的是：

```text
把兼容治理和升级回退规则固定下来
```

`v9.2` 解决的是：

```text
把单次 smoke 推进成可重复执行的 soak 主线
```

`v9.3` 解决的是：

```text
出了问题以后先让谁接管、先检查什么、什么时候能 repair
```

而 `v9.4` 要解决的是下一件同样关键的事：

```text
当你决定“先检查”时，
到底该先看哪个工具、后看哪个工具、怎样把输出串起来
```

一句话定义：

```text
v9.4 = CRYEXTS 的健康检查入口与 inspect 总览基线版本
```

## 2. 为什么现在先做 v9.4

到当前为止，CRYEXTS 已经有不少离线观测工具：

- `cryexts_gdt_inspect`
- `cryexts_journal_inspect`
- `cryexts_policy_inspect`
- `cryexts_extent_inspect`
- `cryexts_dir_index_inspect`
- `cryexts_alloc_inspect`
- `cryexts_xattr_inspect`

能力本身已经够用了。

现在真正缺的是：

```text
先看什么
+ 再看什么
+ 哪个是整盘级
+ 哪个是 inode 级
+ 哪个更适合 mount 前
+ 哪个更适合 fsck 后
```

如果没有这层“检查顺序”，
最容易出现的问题就是：

1. 一出问题就随机挑一个 inspect 工具
2. inode 级问题和整盘级问题混着看
3. 明明应该先 `cryextsck`，却先盯细节结构

所以 `v9.4` 的目标不是新增工具，
而是把当前工具收成一条固定检查主线。

## 3. v9.4 不做什么

为了防止继续发散，`v9.4` 明确不做：

- 新监控系统
- 新 daemon
- 在线遥测平台
- 图形化诊断界面
- 新 scrub 引擎
- 新 fsck 算法

原因很简单：

```text
当前最短路径不是“做更复杂的观测系统”，
而是“先把现有 inspect 工具的使用顺序定下来”
```

## 4. 先把工具分成两类

`v9.4` 的第一条规则非常简单：

```text
先分整盘级，再分 inode 级
```

## 4.1 整盘级工具

这类工具更适合回答：

```text
文件系统整体状态怎么样
元数据大框架有没有出问题
```

当前包括：

- `cryexts_gdt_inspect`
- `cryexts_journal_inspect`
- `cryexts_policy_inspect`

### 入参形态

当前工具使用方式大致是：

- `cryexts_gdt_inspect <image-or-device>`
- `cryexts_journal_inspect <image>`
- `cryexts_policy_inspect <image>`

也就是说，这一类工具通常不需要 inode 号。

## 4.2 inode 级工具

这类工具更适合回答：

```text
某个具体 inode 的结构状态怎么样
这个目录 / 文件 / xattr / extent 到底长什么样
```

当前包括：

- `cryexts_extent_inspect`
- `cryexts_dir_index_inspect`
- `cryexts_alloc_inspect`
- `cryexts_xattr_inspect`

### 入参形态

当前工具使用方式大致是：

- `cryexts_extent_inspect <image> <inode-number>`
- `cryexts_dir_index_inspect <image> <inode-number>`
- `cryexts_alloc_inspect <image> <inode-number>...`
- `cryexts_xattr_inspect <image> <inode-number>`

也就是说，这一类工具通常必须先知道目标 inode。

## 5. 推荐的最小健康检查顺序

这一节是 `v9.4` 的核心。

推荐顺序固定为：

1. 先看失败现象和 `dmesg`
2. 再跑 `cryextsck`
3. 再按问题类型选整盘级 inspect
4. 最后再下钻到 inode 级 inspect

一句话就是：

```text
先总览
+ 再分类
+ 最后下钻
```

## 5.1 第一步：先看现象和 `dmesg`

先回答两个问题：

1. 这是 mount 失败、fsck 报错，还是运行中读写异常
2. 内核已经提示了哪一类问题

`dmesg` 适合先判断：

- I/O error
- checksum mismatch
- replay / recovery 相关提示
- 目录 / xattr / extent 相关报错

这一层不解决问题，
但能帮你先分流。

## 5.2 第二步：先跑 `cryextsck`

`v9.4` 明确建议：

```text
只要不是非常明确的单 inode 逻辑问题，
先跑 cryextsck
```

因为 `cryextsck` 的角色是：

- 全局一致性检查入口
- 问题分类入口
- 之后是否继续 inspect / repair 的分流入口

它能先告诉你：

- replay pending
- orphan cleanup pending
- bitmap / free count mismatch
- checksum mismatch
- 某类结构是否明显不自洽

所以很多时候：

```text
inspect 不是第一步，
fsck 才是第一步
```

## 5.3 第三步：根据问题类别选整盘级 inspect

当 `cryextsck` 或 `dmesg` 已经把问题大致归类后，
再进入对应的整盘级工具。

### A. 怀疑 block group / multi-GDT 问题

优先看：

- `cryexts_gdt_inspect`

它适合回答：

- GDT 区域是否完整
- group checksum 是否一致
- free blocks / free inodes 是否异常
- 目标 group 是否真的被更新

### B. 怀疑 journal / replay / recovery 问题

优先看：

- `cryexts_journal_inspect`

它适合回答：

- journal control / descriptor / commit 结构是否合理
- sequence 是否匹配
- idle / checkpoint 状态是否合理
- recovery 路径是否还挂着待处理状态

### C. 怀疑 policy / encryption 主线问题

优先看：

- `cryexts_policy_inspect`

它适合回答：

- policy table 是否存在
- default policy 是否合理
- policy id / context 是否落盘一致

## 5.4 第四步：最后才下钻 inode 级 inspect

只有当你已经知道问题更像是某个具体 inode / 目录 / 文件时，
再下钻到 inode 级工具。

### A. 文件映射 / 大文件 / truncate / sparse 问题

优先看：

- `cryexts_extent_inspect`

它适合回答：

- extent 是否连续
- 是否进入 tree v2
- leaf / root 引用是否合理
- truncate / sparse 之后逻辑块映射是否符合预期

### B. 目录查找 / dir index / bucket / mask 问题

优先看：

- `cryexts_dir_index_inspect`

它适合回答：

- 目录索引块是否存在
- bucket / block mask 是否合理
- entries / dir_blocks 是否匹配

### C. inode locality / 预分配 / 分配目标问题

优先看：

- `cryexts_alloc_inspect`

它适合回答：

- sibling group / goal group 是什么
- extent 分布是否体现 locality
- 分配结果是否跑偏

### D. xattr / 大 xattr / overflow 问题

优先看：

- `cryexts_xattr_inspect`

它适合回答：

- xattr root 是否存在
- overflow block 是否存在
- entries / blocks / checksum 是否合理

## 6. 问题类型到工具的最小映射

这一节可以当速查表用。

## 6.1 mount 失败

建议顺序：

1. `dmesg`
2. `cryextsck`
3. 如果怀疑 replay：`cryexts_journal_inspect`
4. 如果怀疑 GDT：`cryexts_gdt_inspect`

## 6.2 `Structure needs cleaning`

建议顺序：

1. `dmesg`
2. `cryextsck`
3. `cryexts_journal_inspect`
4. `cryexts_gdt_inspect`
5. 需要时再下钻 inode 级工具

## 6.3 大文件 / sparse / truncate 行为不对

建议顺序：

1. `cryextsck`
2. `cryexts_extent_inspect`
3. 必要时再结合 `cryexts_alloc_inspect`

## 6.4 目录查找 / readdir / dir index 行为不对

建议顺序：

1. `cryextsck`
2. `cryexts_dir_index_inspect`
3. 如果怀疑分配布局，再看 `cryexts_alloc_inspect`

## 6.5 policy / 加密策略行为不对

建议顺序：

1. `cryextsck`
2. `cryexts_policy_inspect`
3. 结合挂载参数和 `KEY` 场景再判断

## 6.6 xattr 行为不对

建议顺序：

1. `cryextsck`
2. `cryexts_xattr_inspect`

## 6.7 multi-GDT / group 计数异常

建议顺序：

1. `cryextsck`
2. `cryexts_gdt_inspect`

## 7. “整盘问题”和“单 inode 问题”怎么区分

这个判断很关键。

## 7.1 更像整盘问题的信号

如果出现下面这些信号，优先走整盘级工具：

- mount 直接失败
- `Structure needs cleaning`
- checksum mismatch
- replay pending
- orphan cleanup pending
- free count / bitmap 异常
- 多个目录/文件一起表现异常

## 7.2 更像单 inode 问题的信号

如果出现下面这些信号，才优先考虑 inode 级工具：

- 某一个目录 lookup 异常
- 某一个大文件 truncate / sparse 行为异常
- 某一个 inode 的 xattr 异常
- 某个父目录的 locality / allocation 结果异常

## 8. 推荐的最小检查模板

如果你以后想把这套流程变成固定操作，
可以按这个模板执行：

1. 记录问题现象
2. 记录失败命令
3. 采 `dmesg`
4. 跑 `cryextsck`
5. 根据类型选一个整盘级 inspect
6. 如有必要，再选一个 inode 级 inspect
7. 最后再决定是否进入 `v9.3` 的 repair 分流

这套模板的价值是：

```text
避免一上来就陷进结构细节，
也避免只看 fsck 不下钻
```

## 9. 当前工具边界提醒

`v9.4` 也要把几个现实边界说清楚。

### 9.1 inspect 不是 fsck 替代品

inspect 更适合：

- 看结构长什么样
- 看某一类元数据是否符合预期

它不等于：

- 全局一致性检查
- 全量离线修复

### 9.2 inspect 不是 repair

当前 inspect 的角色是：

```text
解释
+ 展示
+ 辅助定位
```

不是：

```text
自动修
```

### 9.3 inode 级 inspect 依赖你先知道目标 inode

这也是为什么 `v9.4` 要强调顺序：

```text
先全局
+ 再分类
+ 最后下钻 inode
```

## 10. v9.4 推荐验收标准

我建议把 `v9.4` 完成定义成下面五件事：

### 10.1 inspect 工具职责边界清楚

即能分清：

- 哪些是整盘级
- 哪些是 inode 级

### 10.2 健康检查顺序固定

即：

- `dmesg`
- `cryextsck`
- 整盘级 inspect
- inode 级 inspect

### 10.3 常见问题都有对应工具入口

至少覆盖：

- mount / replay
- GDT
- extent
- dir index
- policy
- xattr
- alloc

### 10.4 inspect 输出和 `v9.3` 恢复规则可以串起来

也就是：

```text
看完以后知道该继续分析、该只读导出，还是该考虑 repair
```

### 10.5 后续 `v9.5` 的发布门槛有统一检查模板

也就是：

```text
release checklist 里的“观测与健康检查”开始有固定入口
```

## 11. 一句话总结

如果说：

- `v9.3` 解决的是“坏了以后怎么分流恢复”

那么：

```text
v9.4 解决的是：
在真正恢复之前，先用哪组工具建立健康视图，先总览什么，再下钻什么
```
