# CRYEXTS v9.2 Soak 与长稳测试说明

## 1. v9.2 的定位

`v9.0` 解决的是：

```text
把部署基线固定下来
```

`v9.1` 解决的是：

```text
把兼容治理、升级回退规则固定下来
```

而 `v9.2` 要解决的是下一件很现实的事：

```text
不只是证明“这次跑通了”，
而是开始证明“反复跑很多次也不会很快坏”
```

一句话定义：

```text
v9.2 = CRYEXTS 的最小 soak / 长稳测试基线版本
```

## 2. 为什么现在先做 v9.2

到当前为止，CRYEXTS 已经有很多 smoke：

- 基础格式 smoke
- extent / dir index / xattr / multi-GDT smoke
- journal replay / orphan / fsck 路径
- image / raw-device demo 路径

这些 smoke 很有价值，
但它们主要回答的是：

```text
功能今天能不能过
```

`v9.2` 开始要补的是另一类证据：

```text
同一条路径连续跑 10 次、50 次、100 次后，
是否还能保持 clean、可挂载、可恢复
```

也就是说，`v9.2` 不再只关心：

- 功能有没有
- 单次脚本绿不绿

而是开始关心：

- 重复挂载会不会积累状态错误
- 反复写删会不会很快把结构搞坏
- journal replay 反复发生后是否仍然自洽
- raw-device 演示路径是否只是“一次性通过”

## 3. v9.2 不做什么

为了防止继续发散，`v9.2` 明确不做：

- 新测试框架
- 新 CI 系统
- 新 fault injection 子系统
- 大规模 benchmark 平台
- 新 on-disk feature
- 新恢复算法

原因很简单：

```text
现有 smoke 已经够多，
v9.2 最短路径是把它们组织成循环和长稳 profile，
不是再造一套新测试基础设施
```

## 4. v9.2 的核心目标

`v9.2` 建议只做一件事：

```text
把“单次 smoke”升级成“可重复执行的 soak 主线”
```

拆开看，就是四类最小长稳路径：

1. mount / umount loop
2. 写删循环
3. replay / fsck 循环
4. raw-device 循环

## 5. v9.2 的四条 soak 主线

## 5.1 mount / umount loop

这是最基础的一条。

它回答的问题是：

```text
连续挂载和卸载很多次，
superblock / recovery state / metadata 路径会不会逐渐漂掉
```

### 建议模型

```text
mkfs
-> mount
-> 做少量读写
-> umount
-> fsck
-> 重复 N 次
```

### 重点观察

- 每轮是否都能正常 mount
- 每轮 `umount` 后是否仍 clean
- 每轮 `cryextsck` 是否一致
- 是否出现残留 recovery / orphan 状态

### pass 语义

如果这条 loop 能稳定通过，说明：

- 基础挂载生命周期是稳定的
- 不是“第一次挂能用，反复挂就开始积累错误”

## 5.2 写删循环

这是最能暴露元数据积累问题的一条。

它回答的问题是：

```text
连续 create / write / rename / unlink / mkdir / rmdir，
目录、inode、bitmap、extent、dir index 会不会逐渐不一致
```

### 建议模型

```text
mount
-> 批量创建文件/目录
-> 写入小文件和大文件
-> rename / link / unlink
-> 删除一部分
-> sync
-> umount
-> fsck
-> 重复 N 次
```

### 建议覆盖点

- 小文件
- 大文件
- 稀疏文件
- 多目录项
- rename / link / symlink
- xattr 基本路径

### 重点观察

- free blocks / free inodes 是否持续合理
- dir index 是否还能命中
- extent tree 是否还能被正确检查
- `cryextsck` 是否保持 clean

### pass 语义

如果这条循环能稳定通过，说明：

- CRYEXTS 的核心元数据更新不是“一次性样品”
- 重复写删后仍能维持基本自洽

## 5.3 replay / fsck 循环

这是恢复主线的最小 soak。

它回答的问题是：

```text
journal replay、orphan cleanup、离线 fsck 这些恢复路径，
是不是只对单次案例有效，还是能反复处理
```

### 建议模型

```text
创建测试镜像
-> 制造 recovery 场景
-> mount-time replay
-> umount
-> fsck
-> 再制造 recovery 场景
-> 重复 N 次
```

如果现阶段不做复杂故障注入，
也可以先从现有脚本循环开始：

- `smoke_v6_1_journal_transaction.sh`
- `smoke_v5_1_orphan_list.sh`
- `smoke_v2_4_cryextsck.sh`

### 重点观察

- replay 后是否总能回到 clean
- `NEEDS_RECOVERY` 状态是否能正确清掉
- orphan cleanup 是否会残留脏状态
- `cryextsck` 报告是否稳定一致

### pass 语义

如果这条循环能稳定通过，说明：

- 恢复路径不是“演示一次就好”
- 恢复语义在重复场景下也基本站得住

## 5.4 raw-device 循环

这是最接近真实部署的一条，
但也是风险最高的一条。

它回答的问题是：

```text
真实块设备路径是不是可重复使用的，
而不只是一次 raw demo
```

### 建议模型

```text
专用测试分区
-> mkfs
-> mount
-> 写入演示负载
-> umount
-> fsck
-> remount
-> 再次验证
-> 重复 N 次
```

### 约束

- 只用于专用测试分区
- 必须先通过 image soak
- 失败时优先停写、取证、离线检查

### 重点观察

- raw-device 是否持续可 mount
- 多轮 remount 后是否仍 clean
- 大目录 / 大文件路径是否持续成立
- 设备路径下的 checksum / GDT / journal 是否稳定

### pass 语义

如果这条循环能稳定通过，说明：

- CRYEXTS 在真实块设备路径上不是“一次性演示”
- 它已经开始具备重复部署演示的稳定性证据

## 6. v9.2 的最小实现原则

`v9.2` 不建议一上来写一大堆新脚本。

最短路径应该是：

```text
复用现有 smoke
+ 外面包一层 loop
+ 每轮后补 fsck / clean 判定
```

也就是说，方向应该优先是：

- 复用 `smoke_version6_mvp.sh`
- 复用 `smoke_version7_demo.sh`
- 复用 `smoke_v6_1_journal_transaction.sh`
- 复用 `smoke_v7_3_usb_demo.sh`

而不是重新发明每个结构测试。

## 7. 建议的 soak profile

为了让后面脚本收口更简单，
`v9.2` 可以先把 profile 语义写死。

## 7.1 `quick soak`

目标：

- 本地快速验证
- 每次改动后先跑一轮

建议：

- 轮数少
- 主要跑 image 路径
- 覆盖 mount / 写删 / fsck

### 推荐语义

```text
开发中快速回归
```

## 7.2 `full soak`

目标：

- 版本前验收
- 稳定性基线

建议：

- 轮数更多
- 覆盖 replay / raw-device
- 每轮都要求 `cryextsck` 一致

### 推荐语义

```text
发布前长稳验证
```

## 7.3 `raw soak`

目标：

- 专门验证真实块设备路径

建议：

- 仅在专用测试介质运行
- 不和日常开发默认绑死

### 推荐语义

```text
高风险、真实设备、专用验收
```

## 8. v9.2 的观察指标

`v9.2` 先不追求复杂 metrics，
但至少要统一观察下面几项：

### 8.1 功能结果

- 是否成功 mount
- 是否成功 umount
- 是否成功 remount
- 是否成功完成目标负载

### 8.2 一致性结果

- `cryextsck` 是否 clean
- 是否出现 replay pending
- 是否出现 checksum mismatch
- 是否出现 `Structure needs cleaning`

### 8.3 结构结果

- extent inspect 是否仍能解释
- dir index inspect 是否仍能解释
- GDT inspect 是否仍一致

### 8.4 内核结果

- `dmesg` 是否出现明显错误
- 是否出现 I/O error
- 是否出现 recovery 状态残留

## 9. 失败后的固定处理顺序

`v9.2` 必须明确：

```text
soak 失败时先停、先看、先留证，
不要一上来就 repair
```

建议顺序：

1. 记录失败轮次
2. 保存失败前后的脚本输出
3. 保存 `dmesg`
4. 先离线跑 `cryextsck`
5. 再跑对应 inspect
6. 最后才判断是否值得 `--repair`

这条规则很重要，
因为 soak 最大的价值就是发现“积累性问题”，
一上来就 repair 容易把现场洗掉。

## 10. v9.2 与前面版本的关系

`v9.2` 不是孤立的。

### 依赖 `v9.0`

- 固定部署基线
- 固定 image / encrypted / raw-device 顺序

### 依赖 `v9.1`

- 固定兼容规则
- 固定升级 / 回退口径

### 复用 `v8.2`

- 测试矩阵
- 恢复矩阵
- 排障矩阵

所以 `v9.2` 本质上不是重新发明测试体系，
而是把前面的矩阵推进到：

```text
重复执行
+ 长时间观察
+ 稳定性结论
```

## 11. v9.2 推荐验收标准

我建议把 `v9.2` 完成定义成下面四件事：

### 11.1 有明确的 soak 主线

别人知道：

- 哪些 loop 是基础长稳
- 哪些 loop 是恢复长稳
- 哪些 loop 是 raw-device 长稳

### 11.2 soak pass 不再只是“脚本绿了”

而是明确代表：

- 可重复挂载
- 可重复写删
- 可重复恢复
- 可重复离线检查

### 11.3 soak 失败有固定排障顺序

不是失败了再临时想下一步。

### 11.4 后续 `v9.5` 的发布门槛有稳定性依据

也就是：

```text
release checklist 里开始有 soak 这一栏
```

## 12. 一句话总结

如果说：

- `v9.0` 解决的是“怎么部署”
- `v9.1` 解决的是“怎么治理版本和兼容”

那么：

```text
v9.2 解决的是：
怎么证明 CRYEXTS 不是只会单次演示，
而是开始具备可重复运行、可重复恢复、可重复验收的稳定性主线
```
