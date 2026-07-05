# CRYEXTS v9.3 恢复与 Repair 手册

## 1. v9.3 的定位

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

而 `v9.3` 要解决的是下一件必须说死的事：

```text
坏了以后先做什么
+ 谁负责恢复
+ 谁只负责检查
+ 什么时候才能动 --repair
```

一句话定义：

```text
v9.3 = CRYEXTS 的恢复分流与 repair 手册基线版本
```

## 2. 为什么现在先做 v9.3

到当前为止，CRYEXTS 已经有：

- mount-time journal replay
- mount-time orphan cleanup
- `cryextsck`
- `cryextsck --repair`
- 一组 inspect 工具

也就是说，恢复能力本身并不缺。

现在真正缺的是：

```text
出问题时先走哪条路径
+ 什么情况可以继续 mount
+ 什么情况必须先离线 fsck
+ 什么情况绝不能一上来就 repair
```

如果这件事不收口，后面最容易出现两个问题：

1. 一看到报错就先 `--repair`，把现场洗掉
2. 明明 mount-time replay 能解决，却过早转成离线修补

所以 `v9.3` 的目标不是发明新恢复算法，
而是把当前已有恢复能力编排成固定操作顺序。

## 3. v9.3 不做什么

为了防止继续发散，`v9.3` 明确不做：

- 新 journal 结构
- 新 repair 算法
- 激进的数据重建
- 自动目录树重构
- 自动内容级修复
- 自动 format downgrade

原因很简单：

```text
当前最短路径不是“让 repair 更激进”，
而是“先把低风险恢复顺序写清楚”
```

## 4. 四类恢复角色

`v9.3` 先把角色分清楚。

## 4.1 mount-time replay

负责：

- 已提交事务的回放
- recovery 状态下的 journal 路径恢复

特点：

- 发生在 mount 路径
- 偏向“上次异常中断后的事务收尾”

它解决的是：

```text
本来就应该被 replay 的事务
```

它不负责：

- 全盘结构扫描
- 任意元数据重建
- 猜测性修复

## 4.2 mount-time orphan cleanup

负责：

- orphan list 上残留 inode 的继续清理

特点：

- 发生在 mount 路径
- 挂在 replay 之后

它解决的是：

```text
删除/清理过程被中断，
还有 inode 需要继续释放
```

它不负责：

- 任意 bitmap 修复
- 目录树关系修复
- xattr / dir index 深度修复

## 4.3 `cryextsck`

负责：

- 离线一致性检查
- 识别结构不自洽
- 报告具体错误

典型能发现：

- checksum mismatch
- bitmap / free count 不一致
- GDT / policy table / xattr / dir index 异常
- replay pending
- orphan cleanup pending

它解决的是：

```text
先告诉你到底坏在哪
```

它默认不做：

```text
不写盘
```

## 4.4 `cryextsck --repair`

负责：

- 低风险元数据修补
- recovery state / 空 journal header 这类状态残留修补
- free count / bitmap reference 这类统计类修补

它的边界必须写死：

```text
--repair 只承诺低风险修补
不承诺“所有损坏都能修”
```

## 5. 总体恢复原则

`v9.3` 的总原则可以压成三句话：

```text
先停写
+ 先留证
+ 先判断是不是 replay 场景
```

更完整一点就是：

1. 先停止继续写入
2. 先保存现场信息
3. 先判断 mount-time recovery 是否适用
4. 不适用再离线 `cryextsck`
5. 确认属于低风险残留，再考虑 `--repair`

也就是说：

```text
replay 优先于 repair
检查优先于写回
备份优先于冒险修补
```

## 6. 固定处理总流程

建议把所有恢复场景都先走这一条总流程。

## 6.1 第一步：停止继续写入

如果文件系统还挂着：

- 停止业务写入
- 不再继续创建/删除/覆盖测试数据

如果是 raw-device：

- 更要先停

因为在故障现场继续写入，
最容易把“可恢复状态”推进成“更难分析状态”。

## 6.2 第二步：保存现场

至少保存：

- 失败时的脚本输出
- `dmesg`
- 当前 `cryextsck` 输出
- 对应 inspect 输出

如果是 image：

- 先复制 image 备份

如果是 raw-device：

- 至少先确认是否需要做块级备份/镜像

这一步的意义是：

```text
后续即使 repair，也还有原始现场可回头分析
```

## 6.3 第三步：判断是否属于 mount-time recovery 场景

优先问两个问题：

1. 这是异常断电/中断后的第一次重新挂载吗
2. 错误是否明显指向 replay / orphan cleanup 主线

如果答案偏向“是”，
优先考虑：

```text
让 mount-time replay / orphan cleanup 先接管
```

而不是一上来就 `--repair`。

## 6.4 第四步：如果 mount 失败，再转离线 `cryextsck`

只要出现：

- mount 失败
- `Structure needs cleaning`
- 明显的 checksum / GDT / xattr / dir index 报错

就应该优先：

```text
离线 cryextsck
```

这一步的目标不是立刻修，
而是先分类：

- replay pending
- orphan cleanup pending
- bitmap/free count mismatch
- journal header / recovery state 残留
- 更深层结构异常

## 6.5 第五步：只有在低风险场景才考虑 `--repair`

符合下面几类情况时，才进入 `--repair` 考虑范围：

- free count 不一致
- bitmap 引用状态不一致
- recovery state 残留但没有有效待回放事务
- journal header 空残留 / 明显无效残留

如果属于下面这类，
不要把 `--repair` 当默认动作：

- 可能仍有有效 journal transaction
- 目录结构语义不清
- 数据块引用冲突
- 复杂 xattr / dir index / extent 结构异常

## 7. `--repair` 当前建议边界

这一节是 `v9.3` 的核心。

## 7.1 建议认为“适合 repair”的问题

当前可以明确认为更适合 `--repair` 的，是下面几类低风险问题。

### A. superblock free count 不一致

特点：

- 统计值不对
- 真实引用关系还能扫描出来

这类问题更像：

```text
声明状态和真实状态不一致
```

而不是“真实内容已经不可判定”。

### B. bitmap 引用状态不一致

特点：

- inode/block 实际被引用
- bitmap 没正确反映

这类问题属于：

```text
元数据标记修补
```

### C. recovery state 残留

比如：

- `NEEDS_RECOVERY` 还挂着
- 但 journal 已经不是有效待回放事务

这类问题适合保守清理状态残留。

### D. 空 journal / 无效 journal header 残留

比如：

- header 已空
- flag / entry_count 明显无效
- 但 superblock 还留着 recovery 状态

这类场景也属于低风险修补。

## 7.2 不建议直接 repair 的问题

这类问题默认不要一上来就 `--repair`。

### A. 可能仍然存在有效待回放事务

这是最重要的一条。

如果 journal 看起来仍可能有效，
那优先级应该是：

```text
保留 replay 机会
```

而不是把事务头直接抹掉。

### B. 目录树关系异常

例如：

- 目录项关系不清
- link count 关系复杂异常

这类问题不适合靠当前低风险 repair 乱猜。

### C. 数据块冲突 / 内容级问题

例如：

- 一个块被多 inode 共享引用
- 内容已经不可信

这不是当前 repair 的承诺范围。

### D. 深层结构异常

例如：

- dir index 深度不一致
- xattr 链异常但难以判断真实值
- extent 结构解释冲突

这类问题更适合：

- 先只读分析
- 先导出数据
- 再考虑后续更强工具

## 8. 常见场景处理手册

这一节直接给固定处理法。

## 8.1 场景 A：异常断电后第一次重新挂载

现象：

- 上次是异常中断
- 本次是第一次重新 mount

建议流程：

1. 先不要手动 `--repair`
2. 先看是否属于正常 replay / orphan cleanup 场景
3. 如果 mount 成功，立即：
   - 只做最小验证
   - 再 `umount`
   - 再跑 `cryextsck`
4. 如果 mount 后 `cryextsck` clean，按正常恢复处理

一句话：

```text
第一次重挂，先给 replay / orphan cleanup 机会
```

## 8.2 场景 B：mount 报 `Structure needs cleaning`

现象：

- mount 直接失败
- 内核或用户态看到 `Structure needs cleaning`

建议流程：

1. 不要继续反复 mount 试运气
2. 先保存 `dmesg`
3. 先离线跑 `cryextsck`
4. 先分类到底是：
   - replay pending
   - orphan cleanup pending
   - checksum mismatch
   - 结构异常
5. 只有在明确属于低风险残留时，才考虑 `--repair`

一句话：

```text
`Structure needs cleaning` 先离线检查，不先盲修
```

## 8.3 场景 C：`cryextsck` 报 `journal v2 replay pending`

现象：

- 离线检查明确提示 replay pending

建议流程：

1. 不要第一反应就是 `--repair`
2. 先判断这是不是一次预期中的 mount-time replay 场景
3. 如果是，优先走正常 mount-time replay
4. replay 成功后再离线 `cryextsck`

这类场景最关键的一条是：

```text
pending 说明“还有机会回放”，
不是“应该马上抹掉”
```

## 8.4 场景 D：`cryextsck` 报 `orphan cleanup pending`

现象：

- orphan head 非空
- orphan 链需要清理

建议流程：

1. 优先让 mount-time orphan cleanup 接管
2. cleanup 完成后再离线检查
3. 不把它默认当成需要 `--repair` 的第一顺位

一句话：

```text
orphan pending 优先交给 orphan cleanup，不优先交给 repair
```

## 8.5 场景 E：`cryextsck` 报 free count / bitmap mismatch

现象：

- free inode / free block count 不一致
- bitmap 引用状态和真实引用不一致

建议流程：

1. 先保存检查结果
2. 确认没有更深层结构问题
3. 这类场景通常可以进入低风险 `--repair` 候选
4. repair 后必须再次跑 `cryextsck`

一句话：

```text
统计和标记类不一致，通常是 repair 更合适的场景
```

## 8.6 场景 F：checksum mismatch

现象：

- superblock / GDT / policy table / journal header 等 checksum mismatch

建议流程：

1. 先看属于哪类元数据
2. 如果还牵涉 replay / recovery，先不要急着 repair
3. 如果是深层结构 checksum 异常，优先：
   - 保存现场
   - inspect
   - 离线分析
4. raw-device 场景下优先做备份

一句话：

```text
checksum mismatch 先分类，不要把它们都当成同一种小问题
```

## 9. 只读导出优先的场景

下面这些场景，`v9.3` 建议优先考虑：

```text
只读挂载 / 数据导出 / 镜像备份
```

而不是先修。

### 建议优先只读处理的情况

- 重要数据场景
- raw-device 真机介质
- 复杂目录关系异常
- 深层 extent / xattr / dir index 结构异常
- 明显超出当前 repair 承诺边界

因为在这些场景里，
当前第一目标不是“让文件系统立刻变 clean”，
而是：

```text
先保住数据和现场
```

## 10. 建议的现场采集清单

出问题时至少采这些：

- 错误发生时间
- 失败命令
- `dmesg`
- `cryextsck` 输出
- 对应 inspect 输出
- 是否是 image 还是 raw-device
- 是否启用了 `KEY`
- 是否是第一次 replay 后重挂

这样后面你再回头分析，才不会只剩一句：

```text
好像坏了
```

## 11. v9.3 推荐验收标准

我建议把 `v9.3` 完成定义成下面五件事：

### 11.1 恢复角色分工明确

即能分清：

- replay 做什么
- orphan cleanup 做什么
- `cryextsck` 做什么
- `--repair` 做什么

### 11.2 `--repair` 边界明确

不再把它误解成万能修复器。

### 11.3 `Structure needs cleaning` 有固定处理顺序

不再靠临时判断。

### 11.4 常见恢复场景有固定分流手册

至少覆盖：

- 异常断电
- replay pending
- orphan pending
- free count / bitmap mismatch
- checksum mismatch

### 11.5 后续 `v9.4` / `v9.5` 有固定恢复口径可复用

也就是：

```text
健康检查和发布门槛，
开始建立在同一套恢复规则之上
```

## 12. 一句话总结

如果说：

- `v9.0` 解决的是“怎么部署”
- `v9.1` 解决的是“怎么治理兼容”
- `v9.2` 解决的是“怎么做长稳验证”

那么：

```text
v9.3 解决的是：
出了问题以后，先让谁接管、先检查什么、什么时候能 repair、什么时候必须先保现场
```
