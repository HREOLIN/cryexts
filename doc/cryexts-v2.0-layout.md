# CRYEXTS V2.0 磁盘布局说明

## 1. 阶段目标

V2.0 的目标是完成磁盘格式整理，为后续 V2.1 bitmap 分配器打地基。

这一阶段已经引入：

- filesystem version bump 到 `2`。
- block bitmap 固定在 block `1`。
- inode bitmap 固定在 block `2`。
- inode table 从 block `3` 开始。
- root directory 作为第一个 data block。
- superblock 中记录 inode/block bitmap 位置、inode 总数、free inode count 和 feature flags。

V2.0 暂不切换真实分配器。当前 create/write 仍沿用 Version 1 的 `next_ino` 和 `next_data_block` 顺序分配逻辑。V2.1 会把分配和释放改为真正读写 bitmap。

## 2. V2.0 布局

```text
+-----------------------------+
| block 0 super/reserved      |
+-----------------------------+
| block 1 block bitmap        |
+-----------------------------+
| block 2 inode bitmap        |
+-----------------------------+
| block 3 inode table start   |
+-----------------------------+
| block 4 inode table         |
+-----------------------------+
| ...                         |
+-----------------------------+
| block 10 inode table end    |
+-----------------------------+
| block 11 root directory     |
+-----------------------------+
| block 12 first free data    |
+-----------------------------+
```

当前常量：

- `CRYEXTS_VERSION = 2`
- `CRYEXTS_BLOCK_BITMAP_BLOCK = 1`
- `CRYEXTS_INODE_BITMAP_BLOCK = 2`
- `CRYEXTS_INODE_TABLE_START = 3`
- `CRYEXTS_INODE_TABLE_BLOCKS = 16`
- `CRYEXTS_ROOT_DIR_BLOCK = 11`
- `CRYEXTS_FIRST_DATA_BLOCK = 11`
- `CRYEXTS_FIRST_FREE_DATA_BLOCK = 12`

## 3. mkfs 行为

`mkfs.cryexts` 会：

- 写入 V2 superblock。
- 初始化 block bitmap。
- 初始化 inode bitmap。
- 清空 inode table。
- 写入 root inode。
- 写入 root directory。
- 标记 block `0..11` 为已使用。
- 标记 root inode 为已使用。

## 4. 内核挂载校验

内核挂载时会校验：

- magic/version/block size/inode size。
- block bitmap 和 inode bitmap 的固定位置。
- inode table 范围。
- root directory 是否等于 first data block。
- first free data block 前的布局是否一致。
- feature flags 是否为当前支持范围。
- 加密卷是否带有 key hash。

## 5. cryextsck 校验

`cryextsck` 会校验：

- V2 superblock 基本字段。
- bitmap block 位置。
- 保留 block 是否在 block bitmap 中标记为 used。
- root inode 是否在 inode bitmap 中标记为 used。
- root inode 和 root directory 结构。
- 现有 inode 和 dirent 的基本合法性。

V2.0 不强制校验新增文件与 bitmap 的引用一致性，因为真实 bitmap 分配器在 V2.1 才实现。

## 6. 测试

```bash
chmod +x scripts/smoke_v2_0_layout.sh
./scripts/smoke_v2_0_layout.sh
```

预期输出：

```text
v2.0 layout smoke test passed
```

建议 V2.0 通过后，再继续执行 Phase 4 加密回归：

```bash
./scripts/smoke_phase4.sh
```
