# CRYEXTS v7.2 变更说明

## 1. 本版本解决的问题

`v7.0` 解决的是“`mkfs` 能写出多块 GDT”。

`v7.1` 解决的是“内核 mount 路径能读取、更新、落盘多块 GDT”。

`v7.2` 继续补齐用户态校验链路，目标是：

```text
mkfs 能创建多块 GDT
-> mount 能使用多块 GDT
-> cryextsck 也能完整读取并校验多块 GDT
```

一句话概括：

```text
v7.2 = 让 fsck 真正理解 multi-GDT，而不是只接受单块 GDT
```

## 2. 本版本没有新增磁盘结构体

这次没有新增新的 on-disk 结构体，也没有改动 superblock、group descriptor 的字段格式。

也就是说：

- `struct cryexts_super_block` 不变
- `struct cryexts_group_desc` 不变
- 多块 GDT 的磁盘布局不变

`v7.2` 改的是 `cryextsck` 的读取模型，不是磁盘格式。

## 3. 修改的关键变量

本次主要修改 `tools/cryextsck.c` 里的 `main()`。

### 3.1 `unsigned char *gdt_region`

- 含义：一段连续内存，用来保存整个 GDT 区域
- 来源：`calloc(group_desc_table_blocks * block_size)`
- 作用：把原本“只读一个 GDT block”的模型，改成“把完整 GDT 区域一次性读入内存”

### 3.2 `uint64_t gdt_bytes`

- 含义：完整 GDT 区域总字节数
- 计算方式：

```text
gdt_bytes = group_desc_table_blocks * CRYEXTS_BLOCK_SIZE
```

- 作用：决定 `calloc()` 和 `read_full()` 读多大一段内存

### 3.3 `struct cryexts_group_desc *groups`

- 含义：group descriptor 数组视图
- `v7.2` 之前：指向单个 `gdt_block`
- `v7.2` 之后：指向完整 `gdt_region`
- 作用：让后面的 `validate_groups()`、`read_inode()`、bitmap 校验、inode 校验继续按 `groups[group]` 访问

这里最关键的一点是：

```text
上层校验逻辑不需要知道 descriptor 跨了几个 GDT block
只要 groups 指向一段完整连续的 descriptor 数组即可
```

## 4. 修改的函数说明

## 4.1 `main(int argc, char **argv)`

- 位置：`tools/cryextsck.c`
- 功能：`cryextsck` 主流程入口

### v7.2 之前的处理方式

旧逻辑是：

1. 先读 superblock
2. 如果发现 `group_desc_table_blocks > 1`
3. 直接报错退出：

```text
multi-block GDT is not yet supported by cryextsck
```

4. 如果只有 1 块 GDT，才读一个 `gdt_block`

这说明旧版 `cryextsck` 的核心假设是：

```text
GDT 只能占 1 个 block
```

### v7.2 之后的处理方式

新逻辑变成：

1. 先读 superblock
2. 如果启用了 block groups，计算完整 GDT 大小
3. 分配 `gdt_region`
4. 从 `group_desc_table_start` 开始，一次性读入整个 GDT 区域
5. 把 `groups` 指向 `gdt_region`
6. 复用现有 `validate_groups(sb, groups)`
7. 在 `out:` 清理路径里统一 `free(gdt_region)`

### 设计原因

原因很简单：

- `validate_groups()` 本来就是按 `groups[group]` 访问
- 只要这段数组在内存里是连续的，上层逻辑根本不需要改
- 因此没必要重写 group 校验器，只要把输入准备正确

这就是 `v7.2` 的最小改法。

## 4.2 `validate_super(const struct cryexts_super_block *sb)`

- 位置：`tools/cryextsck.c`
- 功能：校验 superblock 基本合法性

这个函数本次没有改逻辑，但它在 `v7.2` 里仍然非常关键，因为它已经负责检查：

- `group_desc_table_start` 是否有效
- `group_desc_table_blocks` 是否有效
- GDT 区域是否越界
- GDT 总容量是否足够容纳全部 descriptors

因此 `main()` 现在能够直接放心按：

```text
group_desc_table_blocks * block_size
```

去读取完整区域。

## 4.3 `validate_groups(const struct cryexts_super_block *sb, struct cryexts_group_desc *groups)`

- 位置：`tools/cryextsck.c`
- 功能：遍历并校验所有 group descriptor

这个函数本次也没有改接口。

它能直接复用的原因是：

- 它关心的是“第 N 个 group descriptor 的内容”
- 它不关心这个 descriptor 来自 GDT 第几块

所以：

```text
只要 groups[0..group_count-1] 在内存里是连续的
validate_groups() 就天然支持 multi-GDT
```

## 5. 处理流程

## 5.1 `cryextsck` 读取多块 GDT 的流程

```text
open image
-> read superblock
-> validate_super()
-> gdt_bytes = group_desc_table_blocks * block_size
-> calloc(gdt_region, gdt_bytes)
-> read_full(fd, gdt_region, gdt_bytes, group_desc_table_start * block_size)
-> groups = (struct cryexts_group_desc *)gdt_region
-> validate_groups(sb, groups)
-> 后续 inode / bitmap / checksum 校验继续复用 groups[group]
```

## 5.2 为什么这样就够了

因为在 `cryextsck` 看来：

- GDT 在磁盘上也许是 1 块、2 块、3 块
- 但读到内存以后，它只是“一整段 descriptor 数组”

于是逻辑就从：

```text
按磁盘块思维处理 GDT
```

变成：

```text
按连续数组思维处理 GDT
```

这能大幅减少改动量。

## 6. 具体案例

假设：

- `block_size = 4096`
- `sizeof(struct cryexts_group_desc) = 76`
- `group_count = 55`

那么：

```text
descs_per_block = 4096 / 76 = 53
gdt_blocks = ceil(55 / 53) = 2
```

也就是说：

- 第 1 个 GDT block 保存 `group[0..52]`
- 第 2 个 GDT block 保存 `group[53..54]`

### v7.2 之前

旧版 `cryextsck` 看到 `group_desc_table_blocks = 2` 就直接退出。

### v7.2 之后

新版 `cryextsck` 会：

1. 分配 `2 * 4096 = 8192` 字节
2. 把两个 GDT block 都读进 `gdt_region`
3. 让：

```text
groups[0]   -> 第 1 块里的第 0 个 descriptor
groups[52]  -> 第 1 块里的最后一个 descriptor
groups[53]  -> 第 2 块里的第 1 个 descriptor
groups[54]  -> 第 2 块里的第 2 个 descriptor
```

4. 然后 `validate_groups()` 就能像平常一样遍历：

```text
for group = 0 .. 54
```

它不需要知道 `group[53]` 和 `group[54]` 实际来自第二块 GDT。

## 7. 新增测试脚本

本版本新增：

```text
scripts/smoke_v7_2_multi_gdt_fsck.sh
```

### 脚本验证内容

1. 构造一个明确超过单块 GDT 容量的镜像
2. 用 `cryexts_gdt_inspect` 确认：
   - `gdt_blocks > 1`
   - `gdt_blocks == expected_gdt_blocks`
   - 最后一个 group descriptor 的 checksum 正确
3. 运行 `./cryextsck "$IMG"`
4. 断言输出包含：

```text
cryextsck: <image> clean
```

### 这个 smoke 的意义

它验证的不是 mount，而是：

```text
用户态 fsck 链路已经从“拒绝 multi-GDT”
升级成“完整读取 multi-GDT 并通过校验”
```

## 8. 版本关系

截至 `v7.2`，Version 7 的主线可以理解为：

### `v7.0`

- `mkfs` 能创建多块 GDT

### `v7.1`

- 内核 mount 路径能使用多块 GDT
- 运行期修改能正确写回多块 GDT

### `v7.2`

- `cryextsck` 能完整读取并校验多块 GDT

也就是：

```text
格式能创建
-> 内核能使用
-> fsck 能理解
```

这三步到这里基本闭环了。
