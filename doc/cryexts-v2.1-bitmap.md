# CRYEXTS V2.1 Bitmap 分配器说明

## 1. 阶段目标

V2.1 的目标是把 V2.0 的 bitmap 元数据真正用起来，完成最小可复用分配器。

核心变化：

- inode 分配由 inode bitmap 驱动。
- block 分配由 block bitmap 驱动。
- 删除文件/目录后释放 inode 和 block。
- `statfs` 返回真实 free count。
- `cryextsck` 能检查部分 bitmap 使用情况。

## 2. 当前能力

已实现：

- `mkdir` 分配 inode 和目录 block。
- `touch/create` 分配 inode。
- `unlink/rmdir` 释放 inode。
- 文件 truncate 到 0 时释放 data block。
- mount 时加载 block/inode bitmap。
- 卸载时回收 bitmap buffer。
- `statfs` 使用真实 `free_inodes_count`。

暂不实现：

- 多 block 文件扩展。
- 目录多 block 扩展。
- 复杂回收策略。
- bitmap 压缩或多 block bitmap。
- 失败回滚的完整事务化。

## 3. 测试

```bash
chmod +x scripts/smoke_v2_1_bitmap.sh
./scripts/smoke_v2_1_bitmap.sh
```

预期输出：

```text
v2.1 bitmap smoke test passed
```

## 4. 风险

V2.1 仍然是最小 bitmap 分配器，风险主要在：

- 删除路径回收顺序。
- 失败路径资源泄漏。
- bitmap 与 inode table 不一致。
- 目前仍然未支持多 block 文件和大目录。
