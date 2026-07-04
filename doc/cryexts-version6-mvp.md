# CRYEXTS Version 6 MVP 总结

## 1. 一句话结论

截至 `v6.6`：

```text
Version 6 的功能型 MVP 已完成
```

也就是说，`Version 6` 这一轮原本规划的核心能力，已经从：

```text
设计目标
```

推进到了：

```text
有代码
+ 有 smoke
+ 有 inspect / fsck 可观测性
+ 已经能串成一条完整版本线
```

但同时也要清楚：

```text
功能 MVP 完成
不等于已经达到“长期真实生产使用”或“直接商用发布”
```

`v6.6` 更准确的定位是：

```text
Version 6 功能面闭环完成
```

## 2. Version 6 到底完成了什么

`Version 6` 这一轮的主线是：

- journal 事务边界更完整
- extent 映射能力继续扩大
- sparse file 语义补齐
- allocator / locality 更像真实文件系统
- directory index 进入增量维护
- xattr / inspect / fsck 工具链补强

从版本角度看：

### 2.1 `v6.0`

主题：

- journal v2 layout baseline

完成内容：

- `Version 6` superblock / feature 识别
- `journal v2` 的 control / descriptor / commit 磁盘布局
- `mkfs` 能创建 `v6` journal v2 镜像
- mount / `cryextsck` / inspect 能识别新 journal 布局

这版的意义：

```text
先把 journal v2 的骨架搭起来
```

### 2.2 `v6.1`

主题：

- journal v2 transaction / replay

完成内容：

- transaction begin / commit / replay 语义收紧
- 只回放完整事务
- mount-time replay 打通
- sequence / checkpoint / idle 状态收敛

这版的意义：

```text
从“有 journal v2 结构”
推进到“journal v2 真的能工作”
```

### 2.3 `v6.2`

主题：

- multi-leaf extent tree

完成内容：

- regular file 的 extent 映射从 root + overflow，推进到多 leaf
- inode root 持有 `extent_root_refs[]`
- leaf block 持有真正 extent 数组
- `cryextsck` / inspect 能理解 leaf 结构

这版的意义：

```text
把文件映射能力从“小范围扩展”
推进到“真正更像 tree 的映射层”
```

### 2.4 `v6.3`

主题：

- sparse file / hole

完成内容：

- read hole 返回 0
- punch hole / truncate 能正确修改 extent 映射
- extent tree 不再要求逻辑块完全连续覆盖

这版的意义：

```text
让 extent tree 从连续映射表
升级成能表达洞区间的稀疏映射
```

### 2.5 `v6.4`

主题：

- allocator / locality

完成内容：

- inode / data block 分配更明确地围绕 locality
- 软 reservation window
- alloc hint / alloc goal group 路径更完整

这版的意义：

```text
让分配器不只是“能分配”
而是“开始有连续性和局部性意识”
```

### 2.6 `v6.5`

主题：

- directory index maintenance

完成内容：

- create / unlink / rename / hardlink 开始增量维护 dir index
- bucket mask 不再只靠全量 rebuild
- `cryextsck` 能验证 entries 和实际 live dirent 数

这版的意义：

```text
目录索引从“会建”
推进到“会持续维护”
```

### 2.7 `v6.6`

主题：

- large xattr / inspect / fsck 补强

完成内容：

- xattr 从单块模型升级到 `root + 1 overflow`
- `cryexts_xattr_inspect`
- `cryextsck` 深度检查 xattr block / overflow
- smoke 覆盖 large xattr 场景

这版的意义：

```text
把 Version 6 最后一块“元数据可扩展性 + 工具可观测性”补齐
```

## 3. 所以 Version 6 MVP 的定义是什么

现在可以把 `Version 6 MVP` 明确定义成：

```text
journal transaction 边界完整
+ extent tree 映射能力显著扩大
+ sparse file 语义可表达
+ allocator / locality 有基本优化
+ directory index 能增量维护
+ xattr / fsck / inspect 工具链能理解这些结构
```

换句话说，`Version 6` 的重点不是“多了几个功能点”，而是：

```text
映射更可扩展
事务更可信
元数据更可检查
```

## 4. 目前已经具备的能力

如果你现在问：

```text
这个文件系统到 v6.6 已经能做什么？
```

可以比较稳地回答：

- 能挂载 `Version 6` 镜像
- 能使用 `journal v2`
- crash 后 mount-time replay 有基本能力
- regular file 可以使用 multi-leaf extent tree
- 稀疏文件 / hole 语义已经成立
- 分配器有 locality 和 reservation window
- directory hash index 已经不是“只建不维护”
- xattr 已经不再局限单块
- `cryextsck` 不再只是浅层扫，而是开始理解这些结构
- 已经有一组按版本推进的 smoke 脚本

## 5. 当前 smoke 维度如何理解

`Version 6` 不是只有“能编过”。
它已经逐步形成了三种验证面：

### 5.1 布局 / inspect 类

例如：

- `smoke_v6_0_journal_layout.sh`
- `smoke_v6_2_extent_tree.sh`
- `smoke_v6_6_large_xattr.sh`

这类测试重点看：

- 磁盘格式是否真的写对
- inspect 工具是否能把结构打印出来

### 5.2 运行时行为类

例如：

- `smoke_v6_1_journal_transaction.sh`
- `smoke_v6_3_sparse_file.sh`
- `smoke_v6_4_allocator.sh`
- `smoke_v6_5_dir_index_maintenance.sh`

这类测试重点看：

- mount 后真实行为是否正确
- remount 后语义是否保留

### 5.3 fsck / recovery 类

几乎每一版都把 `cryextsck` 串进流程里，用来验证：

- 新结构 `cryextsck` 能否理解
- 映像卸载后是否仍然 clean

这意味着现在的 `Version 6` 已经不是“只有内核路径有代码”，而是：

```text
内核路径
+ mkfs
+ inspect
+ cryextsck
+ smoke
一起形成闭环
```

## 6. 当前边界也要说清楚

`Version 6 MVP` 虽然完成了，但它仍然是 MVP，不是 full production filesystem。

### 6.1 journal 方面

还没有做到：

- 多事务 ring buffer 级别的复杂调度
- revoke 语义
- full data journaling
- 更激进的 repair

### 6.2 extent / file mapping 方面

还没有做到：

- 更深层级的 extent tree
- unwritten extent 语义
- 真正完整的 `fallocate` 生态

### 6.3 directory 方面

还没有做到：

- 真正多层 HTree
- 超大目录的完整 index leaf 扩展
- 远超当前目录 block 模型的扩展能力

### 6.4 xattr 方面

还没有做到：

- 多级 xattr chain
- 单个大 value 跨块拆分
- xattr checksum / repair

### 6.5 工程化方面

还没有做到：

- 全量版本回归总脚本完全收口
- 长时间 soak test
- USB 真机长期挂载测试
- 开源发布前的文档整理、许可证、兼容矩阵、风险声明

## 7. 面向 U 盘 demo，这意味着什么

如果目标是：

```text
把 CRYEXTS 部署到你的 U 盘上做 demo
```

那么 `Version 6 MVP` 已经足够作为：

```text
功能演示基线
```

因为它已经能展示：

- 自定义 journaling
- extent tree
- sparse file
- locality allocator
- directory index
- policy-aware xattr / encryption 元数据演进

也就是说：

```text
它已经足够像一个“完整故事线”的文件系统原型
```

但如果是：

```text
长期真实数据承载
```

那还应该再做一轮工程化收尾。

## 8. 面向 GitHub 开源，这意味着什么

如果你的目标是：

```text
后面放到 GitHub 上开源
```

那么 `Version 6 MVP` 的价值非常大，因为它已经形成了：

- 清晰版本演进路线
- 每版对应 smoke
- 每版对应文档
- 每版对应磁盘结构和代码解释

这对开源项目非常重要。

因为别人看到的不只是：

```text
这里有一堆代码
```

而是：

```text
这个文件系统是如何一步步演进出来的
每一版解决了什么问题
每一版怎么验证
```

这会显著提升可读性和可信度。

## 9. 对“商用”这件事要怎么理解

如果你说“最好后面商用”，那我建议把 Version 6 看作：

```text
技术原型成熟点
```

而不是：

```text
立即可商用版本
```

因为商用前通常还要补：

- 故障注入和长稳测试
- 向后兼容策略
- 修复策略 / 升级策略
- 更多异常路径审计
- 文档、许可、维护流程

所以 `Version 6 MVP` 的现实定位更像：

```text
可演示
+ 可开源
+ 可继续迭代到更强版本
```

## 10. 建议的下一步

如果按最稳的方式往前推，我建议：

1. 补一个 `Version 6 MVP` 总 smoke。

建议脚本名：

```text
scripts/smoke_version6_mvp.sh
```

2. 做一轮 `v5 + v6` 串跑回归。

3. 开始规划 `Version 7`，但先不要急着铺太大功能面。

更建议围绕三类事情推进：

- 稳定性
- 可恢复性
- 开源可交付性

## 11. 最终总结

现在可以比较稳地说：

```text
CRYEXTS 到 v6.6，Version 6 MVP 已完成
```

它的意义不是“已经变成完整商用文件系统”，而是：

```text
这个文件系统已经跨过了“只有零散功能点”的阶段，
进入了“有事务边界、有可扩展映射、有元数据工具链、有完整版本故事线”的阶段。
```

这就是 `Version 6 MVP` 最重要的价值。
