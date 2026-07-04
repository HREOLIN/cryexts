# CRYEXTS V5.6 变更记录

## 1. 版本目标

`v5.6` 对应 `Version 5` 需求里的最后一个子阶段：

- preallocation / locality hint
- 更好的大文件连续分配策略

这一版的定位是：

- 不重做 allocator
- 不引入复杂 reservation window
- 先把最小可工作的 locality 机制做出来

## 2. 代码改动概览

### 2.1 [cryexts.h](/D:/Carl/cryptext4/cryexts/cryexts.h:1)

新增运行时字段：

- `alloc_hint_block`
- `alloc_goal_group`

新增接口：

- `cryexts_alloc_block_goal()`
- `cryexts_set_inode_alloc_hint()`

### 2.2 [balloc.c](/D:/Carl/cryptext4/cryexts/balloc.c:1)

新增：

- `cryexts_first_data_group()`
- `cryexts_prealloc_feature_enabled()`
- `cryexts_alloc_block_goal()`

变化点：

- allocator 现在可以带 `goal_block + goal_group`
- 启用 `PREALLOC` feature 时优先按目标 group 扫描
- 若 `goal_block` 落在该 group 内，则优先从该位置开始找空闲块
- 失败时回退到普通扫描

原有 `cryexts_alloc_block()` 仍保留，但变成对 `cryexts_alloc_block_goal()` 的简单包装。

### 2.3 [inode.c](/D:/Carl/cryptext4/cryexts/inode.c:1)

新增 helper：

- `cryexts_block_group_of()`
- `cryexts_update_alloc_hint()`
- `cryexts_init_inode_alloc_hint()`

变化点：

- inode 初始化时，会根据已有 block 计算 runtime hint
- 新 inode 创建时，会继承父目录的 locality goal
- extent 顺序追加时，会优先尝试从最后一个 extent 后面拿连续块
- direct / indirect 路径分配时，也会带上 hint
- 每次成功拿到新数据块后，都会推进该 inode 的 alloc hint

### 2.4 [dir.c](/D:/Carl/cryptext4/cryexts/dir.c:1)

变化点：

- `mkdir()` 分配目录首块时，改成带 goal 的 block 分配
- 目录扩容拿新目录块时，改成带 goal 的 block 分配
- `dir index block` 分配也走同样策略
- 分配成功后，更新目录 inode 自己的 alloc hint

### 2.5 [tools/mkfs.cryexts.c](/D:/Carl/cryptext4/cryexts/tools/mkfs.cryexts.c:1)

变化点：

- 默认写入 `CRYEXTS_FEATURE_COMPAT_PREALLOC`
- 如果同时启用 journal，会把 `HAS_JOURNAL` 叠加进去，而不是覆盖掉 `PREALLOC`
- 输出里增加：
  - `Prealloc: enabled`

这表示 `PREALLOC` 到这一版才真正从“预留 flag”走向“运行时 feature”。

## 3. 测试脚本

### 3.1 [scripts/smoke_v5_6_prealloc_locality.sh](/D:/Carl/cryptext4/cryexts/scripts/smoke_v5_6_prealloc_locality.sh:1)

脚本验证：

1. `mkfs` 后 `cryextsck` clean
2. 制造轻微碎片背景
3. 对大文件做分块顺序写
4. 用 `cryexts_extent_inspect` 检查 extent 连续性
5. 最后再次 `cryextsck` clean

### 3.2 [scripts/smoke_version5_mvp.sh](/D:/Carl/cryptext4/cryexts/scripts/smoke_version5_mvp.sh:1)

这是 `Version 5` 总 smoke：

- `v5.1 orphan list`
- `v5.2 extent overflow`
- `v5.3 directory index`
- `v5.4 policy-aware encryption`
- `v5.5 metadata checksum`
- `v5.6 prealloc/locality`

用于一次性回归整个 `Version 5` 版本线。

## 4. 这一版的边界

`v5.6` 已实现：

- contiguous run preference
- per-file allocation hint
- directory locality hint
- `PREALLOC` feature 的真实运行时语义

`v5.6` 尚未实现：

- 真正 reservation window
- delayed allocation
- fallocate
- 多 inode 全局协调预留
- 在线 defrag

所以它是：

```text
Version 5 allocator/locality MVP
```

不是完整生产级预分配器。
