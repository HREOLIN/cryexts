# CRYEXTS v10.0 变更说明

## 1. v10.0 的定位

`Version 10` 的主线是性能优化。

但 `v10.0` 这一版先不急着改内核 I/O 路径，也不急着直接上
`page cache / writeback / buffered write`。

它先解决一个更基础的问题：

```text
后面到底拿什么证明“优化真的生效了”
```

所以 `v10.0` 的准确定义是：

```text
性能基线版本
```

一句话：

```text
v10.0 = 先把 benchmark、测试口径、验收口径固定下来
```

## 2. 这一版解决了什么问题

在 `v10.0` 之前，CRYEXTS 虽然已经可以：

- mount
- read/write
- raw-device demo
- USB demo
- replay / fsck / inspect

但“慢”这件事还停留在经验判断层面。

例如我们已经能感觉到：

- regular file `read/write` 没有走标准 page cache 主路径
- 当前写路径偏同步、偏逐块
- 加密和 journal 会进一步放大 I/O 成本

但如果没有固定 benchmark，就会出现三个问题：

1. 每次测试参数不同，结果不可比
2. image、raw-device、plain、encrypted 混在一起，结论不稳定
3. 后面即使做了 `v10.1/v10.2`，也很难量化收益

所以 `v10.0` 的价值不是“让文件系统变快”，而是：

```text
先把“怎么测性能”这件事工程化
```

## 3. 这一版具体改了什么

## 3.1 新增性能基线脚本

新增脚本：

- [scripts/smoke_v10_0_performance_baseline.sh](/D:/Carl/cryptext4/cryexts/scripts/smoke_v10_0_performance_baseline.sh)

这个脚本的职责很单纯：

- 自动构建模块和工具
- 自动准备 image 或 raw-device 目标
- mount 当前 CRYEXTS
- 运行一组固定 benchmark
- 输出统一格式的结果
- 卸载后再跑一次 `cryextsck`

它不负责：

- 修性能
- 调策略
- 自动调优参数

它只负责：

```text
把基线测准
```

## 3.2 benchmark 口径固定

`v10.0` 固定了下面几类最小性能测试：

### A. 顺序写

用 `dd if=/dev/zero ... conv=fsync`

目的：

- 避开当前 `O_DIRECT` 不稳定/不支持路径
- 先测当前 buffered/sync 组合下的最小顺序写性能

### B. 顺序读

用 `dd if=file of=/dev/null`

目的：

- 测最基础顺序读吞吐

### C. 小文件 create

循环创建固定数量的小文件。

目的：

- 观察 namespace + inode/block allocation + dir index 路径开销

### D. 小文件 unlink

循环删除同样数量的小文件。

目的：

- 观察 unlink / orphan / dir-index 更新成本

### E. 目录扫描

对填充后的目录执行 `find`。

目的：

- 观察 `readdir` / dir index / namespace scan 基线

## 3.3 测试模式固定

`v10.0` 脚本支持两种模式：

### image 模式

默认模式，适合：

- 日常开发
- 快速重复 benchmark
- 功能改动后的回归对比

### raw-device 模式

适合：

- U 盘
- 专用分区
- 真实块设备验证

但它仍然要求显式确认风险。

## 3.4 结果输出格式固定

脚本会输出统一格式的 key=value 结果，例如：

- `seq_write_MBps=...`
- `seq_read_MBps=...`
- `small_create_files_per_sec=...`
- `small_unlink_files_per_sec=...`
- `dir_scan_entries_per_sec=...`

这样后面：

- `v10.1 cached read`
- `v10.2 buffered write`
- `v10.3 writeback`

都可以直接和 `v10.0` 对比。

## 4. 这一版为什么不直接改 page cache

这是 `v10.0` 最重要的边界。

`Version 10` 的方向当然是：

- `read_folio`
- `write_begin`
- `write_end`
- `writepages`

但 `v10.0` 故意不先碰这些代码。

原因很简单：

```text
不先建立基线，后面的优化收益就没有可解释的参照物
```

所以 `v10.0` 的策略是：

```text
先测量，再动刀
```

## 5. 推荐测试口径

`v10.0` 之后，建议所有 Version 10 的性能改动都优先复用这套口径：

### 介质维度

- image
- raw-device

### 加密维度

- plain
- encrypted

### 负载维度

- 顺序读
- 顺序写
- 小文件 create/unlink
- 目录扫描

这样后面每一版都能回答：

```text
到底是哪里快了
到底是 image 快了还是 raw-device 快了
到底 plain 和 encrypted 差多少
```

## 6. 这一版没有做什么

`v10.0` 明确没有做：

- page cache 接入
- buffered write 接入
- writeback 改造
- journal 与 writeback 协同重构
- 加密页缓存语义重做

这些都留给后面的：

- `v10.1`
- `v10.2`
- `v10.3`
- `v10.4`

## 7. v10.0 的验收标准

我建议把 `v10.0` 的完成标准定义成：

1. 能稳定跑性能基线脚本
2. image 模式能输出完整 benchmark 结果
3. raw-device 模式在稳定介质上能输出完整 benchmark 结果
4. benchmark 后 `cryextsck` 仍然 clean
5. 文档已经固定 benchmark 口径

一句话：

```text
v10.0 完成，不代表性能已经优化，
只代表性能优化这条主线终于有了统一起跑线。
```

## 8. 一句话总结

如果说：

- `v9.x` 解决的是部署、恢复、soak、release 工程化

那么：

```text
v10.0 解决的是：
先把性能优化变成一条“可测、可比、可回归”的工程主线
```
