# CRYEXTS v7.1 smoke 补充说明

## 1. 为什么原始 v7.1 smoke 会停在 inspect 输出后面

原始 `scripts/smoke_v7_1_multi_gdt_mount.sh` 里，断言写的是：

```text
观察第二块 GDT 里的目标 group `free_inodes` 下降
```

但从真实测试结果看，停止时的输出是这种形态：

```text
group[53].free_blocks=4090
group[53].free_inodes=56
...
group[54].free_blocks=3578
group[54].free_inodes=56
```

这里说明了两件事：

1. 第二块 GDT 里的 `group[54]` 确实已经被修改了
2. 被修改的是 `free_blocks`，不是 `free_inodes`

所以脚本停住，不是因为 `v7.1` 失败了，而是因为：

```text
smoke 的断言目标写错了
```

## 2. 为什么这里下降的是 `free_blocks`

这个 smoke 的工作负载是：

1. mount 文件系统
2. 批量创建很多小文件
3. 每个文件写入一小段内容

在这个过程中，系统会分配两类资源：

- inode
- data block

但这两类资源不一定落在同一个 group。

### 更准确的理解

- 新文件的 inode，往往还会优先按目录 locality 策略分配在前面的 group
- 但文件数据块会继续向后分配
- 当数据块推进到 `group[54]` 时，变化的就是：

```text
group[54].free_blocks_count--
```

而不是一定发生：

```text
group[54].free_inodes_count--
```

所以：

```text
看到 group[54].free_blocks 明显下降，
本身就已经证明第二块 GDT 参与了真实元数据更新
```

## 3. 这次输出实际上证明了什么

你给出的结果里：

- `group[53].free_blocks=4090`
- `group[54].free_blocks=3578`
- `checksum == expected_checksum`

这说明：

1. 多块 GDT 已经被 mount 正确读入
2. 第二块 GDT 覆盖到的 group descriptor 确实被运行时更新了
3. 更新后的 checksum 也重新计算正确了
4. 问题只在 smoke 脚本最后的断言条件

一句话总结：

```text
v7.1 核心逻辑大概率是成功的，
失败的是旧版 smoke 对“观察指标”的选择
```

## 4. 正确的 smoke 观察点应该是什么

对于当前这条测试，更合理的断言应该改成：

```text
观察第二块 GDT 中目标 group 的 `free_blocks` 下降
```

也就是脚本里应该比较：

- `free_blocks_before`
- `free_blocks_after`

而不是比较：

- `free_inodes_before`
- `free_inodes_after`

## 5. 修改后的判断逻辑

正确思路是：

1. `mkfs` 后先 inspect，记录目标 group 的 `free_blocks`
2. mount 后批量创建文件
3. umount 后再 inspect
4. 检查：

```text
free_blocks_after < free_blocks_before
```

如果成立，就说明：

```text
第二块 GDT 里的 group descriptor 已经真的被改写并落盘
```

## 6. 结论

你这次跑出来的现象应该理解为：

```text
不是 v7.1 失败，
而是 v7.1 smoke 原始断言写偏了
```

更准确地说：

- `v7.1` 已经证明了多块 GDT 可以被挂载
- 也证明了第二块 GDT 对应的 descriptor 会被更新
- 现在需要把 smoke 从“看 free_inodes”改成“看 free_blocks”
