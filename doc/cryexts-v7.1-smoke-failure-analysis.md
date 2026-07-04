# CRYEXTS v7.1 smoke 失败原因分析

## 1. 问题背景

`scripts/smoke_v7_1_multi_gdt_mount.sh` 的目标，本来是想验证：

```text
v7.1 之后，多块 GDT 不只是能被 mkfs 写出来，
还能够被内核 mount、更新、写回，并且在第二块 GDT 覆盖到的 group 上体现出来。
```

这条 smoke 的设计思路原本是：

1. 先 `mkfs`
2. 用 `cryexts_gdt_inspect` 记录某个目标 group 的计数
3. mount 文件系统
4. 批量创建文件
5. umount
6. 再次 inspect
7. 比较目标 group 的计数是否下降

表面上看，这个设计没有问题。

但结合完整日志来看，当前脚本选择的目标 group 和工作负载规模都不对，导致脚本失败。

## 2. 从完整日志里能看到什么

这次镜像的关键信息是：

```text
Groups: 55
GDT blocks: 2
Journal: start=224768 blocks=512
group[54].start=221184
group[54].blocks=4096
```

而第一次 `inspect` 里，`group[54]` 已经是：

```text
group[54].free_blocks=3578
group[54].free_inodes=56
```

注意，这个数值出现在 mount 之前，也就是刚 `mkfs` 之后。

这说明：

```text
group[54]` 在 smoke 开始前就已经不是“满空闲”状态了
```

## 3. 为什么 `group[54].free_blocks` 一开始就是 3578

原因是 journal 放在了尾部，而尾部正好落在 `group[54]` 里。

日志里：

```text
Journal: start=224768 blocks=512
group[54].start=221184
```

`group[54]` 的 block 范围是：

```text
[221184, 225279]
```

journal 的 block 范围是：

```text
[224768, 225279]
```

也就是说，journal 的 512 个块全部占用了 `group[54]` 尾部空间。

而每个普通 group 初始大约有：

```text
free_blocks = 4090
```

所以：

```text
4090 - 512 = 3578
```

这正好就是日志里的：

```text
group[54].free_blocks=3578
```

结论：

```text
group[54].free_blocks=3578 不是 smoke 创建文件造成的变化，
而是 mkfs 时 journal 预留造成的初始状态。
```

## 4. 为什么当前 smoke 还是会失败

脚本目前比较的是：

```bash
free_blocks_after < free_blocks_before
```

而比较对象是：

```text
target_group_index = DESCS_PER_BLOCK + 1 = 54
```

也就是：

```text
比较 group[54].free_blocks 前后是否下降
```

但从日志看：

- mount 前：`group[54].free_blocks=3578`
- 批量创建 220 个文件后：`group[54].free_blocks` 仍然是 `3578`

所以这个断言失败，脚本就因为 `set -euo pipefail` 直接退出了，没有打印最后的：

```text
v7.1 multi-GDT mount smoke test passed
```

## 5. 为什么 220 个文件没有打到 `group[54]`

这是整个 smoke 设计里最关键的误判点。

### 5.1 inode 分配并没有推进到第 54 组

日志里每组 inode 数量是：

```text
inodes_per_group = 56
```

`group[0]` 初始有 root inode，所以它起始可用 inode 大约是：

```text
55
```

`group[1..]` 每组大约是：

```text
56
```

如果目标是让新文件 inode 真正分配到 `group[54]`，至少要先耗掉：

```text
group[0]  : 55
group[1]  : 56
...
group[53] : 56
```

也就是至少：

```text
55 + 53 * 56 = 3023
```

所以要真正碰到 `group[54]` 的 inode，至少要创建：

```text
3024` 个文件左右
```

但当前脚本只有：

```text
FILE_COUNT=220
```

这远远不够。

### 5.2 日志已经证明 inode 只打到了前几组

第二次 `inspect` 里，真正发生变化的是：

```text
group[0].free_inodes=0
group[1].free_inodes=0
group[2].free_inodes=0
group[3].free_inodes=3
```

这说明 220 个文件创建后：

- inode 分配只推进到了 `group[3]`
- 根本没接近 `group[54]`

### 5.3 数据块分配也没有推进到 `group[54]`

第二次 `inspect` 里，明显变化的是：

```text
group[0].free_blocks=3865
```

而 `group[54].free_blocks` 没变。

说明当前这些小文件的数据块主要还是落在前面的 group，尤其是 `group[0]`。

所以：

```text
这次 workload 既没有把 inode 分配推进到 group[54]，
也没有把数据块分配推进到 group[54]。
```

## 6. 这次 smoke 失败，能不能说明 v7.1 实现失败

不能直接这样下结论。

从日志反而能看出一些偏正面的信号：

1. 文件系统成功 mount 了，否则后面不可能写入这么多文件。
2. 第二次 inspect 能正常读出完整多块 GDT。
3. 所有打印出来的 group checksum 仍然满足：

```text
checksum == expected_checksum
```

这说明：

- 多块 GDT 的读路径是通的
- 至少一部分 group descriptor 的更新和 checksum 重算是正常的

因此更准确的判断是：

```text
当前失败的是 smoke 设计，
而不是已经足以从这份日志直接证明 v7.1 多块 GDT 实现失败。
```

## 7. 当前 smoke 的根本问题

当前这条 smoke 同时有两个设计问题。

### 问题 1：目标 group 选错了

脚本把目标 group 固定选成：

```text
group[54]
```

但这个 group 本身已经被 journal 占用了 512 个块，不适合作为“前后变化是否命中”的观测点。

### 问题 2：工作负载太小

`FILE_COUNT=220` 太小，只能把 inode 消耗推进到 `group[3]` 左右，远达不到 `group[54]`。

所以即使 `v7.1` 实现没问题，脚本也会失败。

## 8. 更合理的 v7.1 smoke 设计方向

要验证“第二块 GDT 覆盖到的 group descriptor 会被真正更新”，更合理的方案有两个。

## 8.1 方案 A：验证 inode 分配推进到第二块 GDT

思路：

1. 创建一个多块 GDT 镜像
2. mount 后大量创建空文件
3. 创建文件数提升到 `3024+`
4. 观察 `group[54].free_inodes` 下降

优点：

- 逻辑直观
- 不依赖大数据写入
- 更容易明确证明“inode descriptor 已经跨到第二块 GDT”

这是更推荐的方案。

## 8.2 方案 B：验证 block 分配推进到第二块 GDT

思路：

1. 创建较少数量的文件
2. 但给每个文件写入足够大的数据
3. 把前面 group 的数据块不断消耗掉
4. 最终观察第二块 GDT 某个 group 的 `free_blocks` 下降

缺点：

- workload 更重
- 结果更依赖分配策略
- 不如 inode 方案稳定

## 9. 对当前日志的最终结论

这份日志最准确的结论应该写成：

```text
smoke_v7_1_multi_gdt_mount.sh 当前失败，
失败原因不是因为日志已经证明 v7.1 内核实现错误，
而是因为：

1. 观测目标 group[54] 本身已被 journal 预占用；
2. FILE_COUNT=220 不足以把 inode 或 block 分配推进到 group[54]；
3. 因此前后比较条件不成立，脚本断言失败退出。
```

## 10. 一句话总结

一句话总结就是：

```text
当前 v7.1 smoke 失败，不是因为“第二块 GDT 更新坏了”，
而是因为“测试样本太小，根本没打到第二块 GDT 想观察的目标 group”。
```
