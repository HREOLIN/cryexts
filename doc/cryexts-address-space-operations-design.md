# CRYEXTS `address_space_operations` 架构设计

## 1. `address_space_operations` 是什么

`address_space_operations`，简称 `a_ops`，是 Linux page cache 与具体文件系统之间的适配接口。

Linux 知道如何：

- 创建和查找缓存页。
- 把用户数据复制到缓存页。
- 标记 dirty page。
- 调度 writeback。
- 回收和失效缓存页。

但 Linux 不知道：

- CRYEXTS logical block 如何映射到 physical block。
- sparse hole 如何解释。
- extent tree 如何查找和扩展。
- 数据块如何按 policy 加密。
- block allocation 和 inode 更新如何进入 journal。

因此 Linux 通过 `a_ops` 向 CRYEXTS 提出请求：

```text
Linux 管理 page；
CRYEXTS 负责把 page 与自己的磁盘 block 连接起来。
```

当前实现位置：

- [file.c](../file.c)
- [inode.c](../inode.c)
- [cryexts.h](../cryexts.h)

## 2. 对象关系

```mermaid
classDiagram
    class inode {
        i_size
        i_blocks
        i_mapping
    }

    class address_space {
        host
        a_ops
        cached pages
        writeback error
    }

    class cryexts_file_aops {
        readpage()
        write_begin()
        write_end()
        writepage()
        writepages()
        set_page_dirty()
    }

    class page {
        index
        mapping
        Uptodate
        Dirty
        Writeback
        Error
    }

    inode "1" --> "1" address_space : i_mapping
    address_space "1" --> "1" inode : host
    address_space "1" --> "1" cryexts_file_aops : a_ops
    address_space "1" --> "0..n" page : cache
```

每个 regular-file inode 都有一个 `address_space`：

```text
inode->i_mapping
```

CRYEXTS 在装载已有 regular file 和创建新 regular file 时都设置：

```c
inode->i_mapping->a_ops = &cryexts_file_aops;
```

`mapping->host` 反向指向所属 inode，所以回调拿到 `page` 或 `mapping` 后都能找到文件。

## 3. Page 与 CRYEXTS Block 的关系

当前固定：

```text
PAGE_SIZE          = 4096 bytes
CRYEXTS_BLOCK_SIZE = 4096 bytes
```

所以当前模型是：

```mermaid
flowchart LR
    IDX["page->index"] --> OFF["文件偏移<br/>index × 4096"]
    OFF --> LOGICAL["CRYEXTS logical block<br/>offset ÷ 4096"]
    LOGICAL --> MAP["cryexts_resolve_block"]
    MAP --> PHYSICAL["physical block"]
```

也就是：

```text
一个 page = 一个 CRYEXTS logical block = 4096 bytes
```

例如：

| `page->index` | 文件字节范围 | Logical block |
| ---: | --- | ---: |
| 0 | 0～4095 | 0 |
| 1 | 4096～8191 | 1 |
| 2 | 8192～12287 | 2 |

logical block 还要经过 direct/indirect/extent mapping，才能得到 physical block。

## 4. 当前回调表

```c
const struct address_space_operations cryexts_file_aops = {
	.readpage = cryexts_readpage,
	.writepage = cryexts_writepage,
	.writepages = cryexts_writepages,
	.write_begin = cryexts_write_begin,
	.write_end = cryexts_write_end,
	.set_page_dirty = __set_page_dirty_nobuffers,
};
```

```mermaid
flowchart TB
    AOPS["cryexts_file_aops"]
    READ["readpage<br/>cache miss 填页"]
    BEGIN["write_begin<br/>准备锁定 page"]
    END["write_end<br/>更新内存状态并标脏"]
    DIRTY["set_page_dirty<br/>登记 dirty page"]
    ONE["writepage<br/>单页写回"]
    MANY["writepages<br/>扫描并写回多页"]

    AOPS --> READ
    AOPS --> BEGIN
    AOPS --> END
    END --> DIRTY
    AOPS --> ONE
    AOPS --> MANY
```

| 回调 | 调用者 | CRYEXTS 职责 |
| --- | --- | --- |
| `readpage` | Linux cached-read | 从磁盘装载一个缺失 page |
| `write_begin` | `generic_file_write_iter` | 获得并准备一个锁定 page |
| `write_end` | `generic_file_write_iter` | 结束用户复制、更新 size/time、标脏 |
| `set_page_dirty` | Linux page-cache helper | 把无 buffer-head page 加入 dirty tracking |
| `writepage` | Linux 单页回写 | 把一个 dirty page 持久化 |
| `writepages` | Linux 批量回写 | 扫描指定 mapping/range 的 dirty pages |

## 5. Page 状态机

```mermaid
stateDiagram-v2
    [*] --> Absent
    Absent --> LockedNotUptodate: cache miss / grab page
    LockedNotUptodate --> UptodateClean: readpage 或 fill_page 成功
    LockedNotUptodate --> Error: 读取或解密失败

    UptodateClean --> LockedUptodate: write_begin
    LockedUptodate --> Dirty: 用户复制 + write_end
    Dirty --> Writeback: Linux writeback 调度
    Writeback --> UptodateClean: 写盘和 journal 成功
    Writeback --> DirtyError: I/O 失败 + redirty
    DirtyError --> Writeback: 后续重试

    UptodateClean --> Absent: reclaim/invalidate/truncate
    Dirty --> Absent: 先 writeback，再 truncate/invalidate
    Error --> Absent: 丢弃错误 page
```

关键状态：

- `PageUptodate`：page 内容对应文件当前逻辑数据，可以交给用户读取。
- `PageDirty`：page cache 比磁盘更新，需要 writeback。
- `PageWriteback`：该 page 正在执行持久化。
- `PageError`：最近一次读取或写回失败。

## 6. 读取架构

### 6.1 Linux 主流程

```mermaid
sequenceDiagram
    participant U as 用户 read()
    participant G as generic_file_read_iter
    participant M as address_space/page cache
    participant C as cryexts_readpage
    participant D as CRYEXTS block I/O

    U->>G: read(fd, buffer, length)
    G->>M: 查找 page index
    alt PageUptodate
        M-->>G: 返回缓存明文
    else page 缺失或未 Uptodate
        M->>C: 传入已锁定 page
        C->>D: fill_page -> resolve/read/decrypt
        D-->>C: page 内容
        C->>M: SetPageUptodate + unlock_page
        M-->>G: 返回缓存明文
    end
    G-->>U: copy_to_iter 结果
```

### 6.2 `cryexts_readpage()` 契约

Linux 传入：

- 已加入 `mapping` 的 page。
- 已锁定 page。
- page 尚不能直接交给用户。

CRYEXTS 必须：

1. 调用 `cryexts_fill_page()` 填充内容。
2. 成功时设置 PageUptodate。
3. 失败时清 Uptodate、设置 PageError。
4. 无论成功失败都解锁 page。
5. 返回 0 或负 errno。

### 6.3 `cryexts_fill_page()`

```mermaid
flowchart TD
    P["锁定 page"] --> POS["page_offset(page)"]
    POS --> SIZE["根据 i_size 计算有效字节"]
    SIZE --> RESOLVE["cryexts_resolve_block(create=false)"]
    RESOLVE --> EXISTS{"physical != 0?"}
    EXISTS -- 否 --> ZERO["sparse hole 补零"]
    EXISTS -- 是 --> READ["cryexts_read_inode_block"]
    READ --> DECRYPT["按 inode policy 解密"]
    DECRYPT --> COPY["复制到 page"]
    ZERO --> COPY
    COPY --> EOF["EOF 后区域补零"]
    EOF --> UP["SetPageUptodate"]
```

`cryexts_fill_page()` 只负责内容，不解锁 page。这样 `readpage` 和 `write_begin` 都可以复用它，同时保持各自的 page 锁契约。

## 7. Buffered Write 架构

```mermaid
sequenceDiagram
    participant U as 用户 write()
    participant G as generic_file_write_iter
    participant B as cryexts_write_begin
    participant P as Page Cache
    participant E as cryexts_write_end

    U->>G: write(fd, data)
    G->>B: mapping + pos + len
    B->>P: grab_cache_page_write_begin
    alt page 未 Uptodate
        B->>B: cryexts_fill_page
    end
    B-->>G: 返回锁定 page
    G->>P: copy_from_iter 到 page
    G->>E: pos/len/copied/page
    E->>E: 扩展 i_size，更新 mtime/ctime
    E->>P: SetPageUptodate + set_page_dirty
    E->>P: unlock_page + put_page
    E-->>G: 返回 copied
    G-->>U: write 返回
```

### 7.1 `cryexts_write_begin()` 契约

输入：

- `mapping`：目标 inode 的 address space。
- `pos/len`：本次写入范围。
- `flags`：page-cache 获取标志。

成功输出：

- `*pagep` 指向已锁定 page。
- page 内容是 Uptodate。
- page 引用由后续 `write_end` 接管。

失败责任：

- 已取得 page 时必须 unlock + put。
- 不得把失败 page 留给 generic write 路径。

### 7.2 为什么先读旧 page

假设 page 原内容为：

```text
[0........................4095]
```

用户只覆盖 `[1024..1535]` 共 512 bytes。其余 3584 bytes 必须保留，所以未 Uptodate page 要先从磁盘加载完整旧内容，再覆盖局部范围。

### 7.3 `cryexts_write_end()` 契约

`write_end` 不执行磁盘 I/O，只完成内存状态转换：

```text
Locked + Uptodate
-> 更新 page 内容已完成
-> 更新 inode size/time
-> Dirty + Uptodate
-> unlock + put
```

`copied == 0` 时不扩展文件、不标脏，但仍释放 page。

## 8. 为什么需要 `set_page_dirty`

当前使用：

```c
.set_page_dirty = __set_page_dirty_nobuffers
```

CRYEXTS 的 page cache page 没有维护每页 `buffer_head` 链，因此使用 Linux 提供的 no-buffer dirty helper。

```mermaid
flowchart LR
    END["cryexts_write_end"] --> SET["set_page_dirty(page)"]
    SET --> AOPS["mapping->a_ops->set_page_dirty"]
    AOPS --> NOBUF["__set_page_dirty_nobuffers"]
    NOBUF --> TAG["设置 PageDirty + mapping dirty tag"]
    TAG --> WB["writeback 能找到该 page"]
```

如果这个回调为空：

- `set_page_dirty()` 无法完成 CRYEXTS page 的标准标脏。
- Linux 5.15 上可能通过空函数指针触发 kernel Oops。
- writeback 也无法可靠发现新数据。

## 9. Writeback 架构

### 9.1 触发来源

```mermaid
flowchart LR
    DIRTY["Dirty pages"]
    BG["后台 writeback"]
    FSYNC["fsync/fdatasync"]
    SYNC["sync/syncfs"]
    PRESSURE["内存压力"]
    UMOUNT["卸载同步"]
    AOPS["writepage/writepages"]

    DIRTY --> BG --> AOPS
    DIRTY --> FSYNC --> AOPS
    DIRTY --> SYNC --> AOPS
    DIRTY --> PRESSURE --> AOPS
    DIRTY --> UMOUNT --> AOPS
```

CRYEXTS 不需要自己建立后台线程；Linux 已有 writeback worker。

### 9.2 `writepage` 与 `writepages`

```text
writepage  = 写回一个由 Linux 选中的 page
writepages = 写回 mapping 中某个范围/数量的 dirty pages
```

当前 `writepages` 直接复用：

```c
write_cache_pages(mapping, wbc, cryexts_writepages_callback, NULL);
```

Linux 负责扫描、锁页和 writeback_control；CRYEXTS callback 复用 `cryexts_writepage_locked()` 完成每一页的实际持久化。

### 9.3 单页写回流程

```mermaid
flowchart TD
    LOCKED["Linux 传入锁定 dirty page"]
    RANGE["检查 page 是否仍在 i_size 内"]
    WB["set_page_writeback"]
    TX["cryexts_journal_begin"]
    MAP["logical block 计算"]
    RESOLVE["cryexts_resolve_block(create=true)"]
    ALLOC["必要时分配 physical block/extent"]
    DATA["cryexts_write_inode_block"]
    CRYPT["明文临时副本 -> AES-CTR 密文"]
    INODE["更新 i_blocks + 写磁盘 inode"]
    COMMIT["cryexts_journal_commit"]
    DONE["unlock_page + end_page_writeback"]

    LOCKED --> RANGE --> WB --> TX --> MAP --> RESOLVE --> ALLOC --> DATA --> CRYPT --> INODE --> COMMIT --> DONE
```

writeback 阶段才执行 physical block allocation，因此当前具有基础 delayed-allocation 语义：用户 write 返回时 page 可以是 dirty，但 logical block 尚未分配 physical block。

## 10. Writeback 错误路径

```mermaid
flowchart TD
    FAIL["mapping/allocation/data/inode/journal 失败"]
    ABORT["有活动事务则 journal_abort"]
    MAPERR["mapping_set_error(mapping, err)"]
    REDIRTY["redirty_page_for_writepage"]
    PAGEERR["SetPageError"]
    UNLOCK["unlock_page"]
    END["end_page_writeback<br/>仅已开始 writeback 时"]
    RETRY["后续 writeback 可重试"]
    FSYNC["fsync 可读取 mapping error"]

    FAIL --> ABORT --> MAPERR --> REDIRTY --> PAGEERR --> UNLOCK
    UNLOCK --> END --> RETRY
    MAPERR --> FSYNC
```

错误处理有两个目标：

1. 不把失败 page 错误地变成 clean。
2. 让同步调用者最终能看到失败。

## 11. `fsync()` 如何与 a_ops 配合

```mermaid
sequenceDiagram
    participant U as 用户 fsync()
    participant F as cryexts_fsync
    participant L as Linux filemap/writeback
    participant A as cryexts_file_aops
    participant M as CRYEXTS metadata

    U->>F: fsync(file)
    F->>L: file_write_and_wait_range
    L->>A: writepage/writepages
    A-->>L: writeback result
    L-->>F: mapping error 或成功
    F->>M: cryexts_write_inode_to_disk
    F->>M: cryexts_sync_metadata
    F-->>U: 0 或 errno
```

顺序不能反过来。必须先把 dirty data pages 写回，再完成 inode 和全局 metadata 同步。

## 12. Truncate 与 Page Cache

直接释放 block 前必须先处理 dirty pages：

```mermaid
flowchart TD
    TRUNC["truncate 缩小文件"]
    WAIT["filemap_write_and_wait"]
    TX["journal_begin"]
    FREE["释放目标范围 physical blocks"]
    TAIL["清零最后部分 block 尾部"]
    SIZE["更新 inode size/mapping"]
    COMMIT["journal_commit"]
    CACHE["truncate_setsize / 丢弃越界 pages"]

    TRUNC --> WAIT --> TX --> FREE --> TAIL --> SIZE --> COMMIT --> CACHE
```

否则可能发生：

```text
block 已释放给其他 inode
-> 原文件旧 dirty page 后续写回
-> 覆盖其他文件数据
```

同样的等待规则还用于 punch hole、删除最后链接和 rename 覆盖。

## 13. 加密与 a_ops 的边界

```mermaid
flowchart LR
    subgraph MEMORY["内存"]
        PAGE["Page Cache<br/>始终明文"]
        TEMP["临时 4 KiB buffer"]
    end

    subgraph CRY["CRYEXTS"]
        POLICY["inode policy_id"]
        MAP["physical block"]
    end

    subgraph LINUX["Linux Crypto/Block"]
        AES["AES-CTR"]
        BH["buffer_head 密文"]
    end

    DISK["磁盘密文 data block"]

    PAGE --> TEMP
    POLICY --> AES
    MAP --> AES
    TEMP --> AES --> BH --> DISK
```

`write_end` 不加密，因为 page cache 必须保持明文。加密发生在 `writepage` 调用的 `cryexts_write_inode_block()` 中，而且只修改临时副本。

读取时，磁盘密文先在 block buffer 中读出并解密，之后才填入 page cache。

## 14. Journal 与 a_ops 的边界

Page cache 只管理文件数据页，不知道一次首次写入还会修改：

- block bitmap。
- group/super free counter。
- extent root/leaf。
- inode size 和 `i_blocks`。
- metadata checksum。

因此当前每个 dirty page writeback 包含一个 CRYEXTS transaction：

```text
journal_begin
-> resolve/allocate/update extent
-> 写 data block
-> 写 inode metadata
-> journal_commit
```

数据 block 本身不进入 metadata journal；journal 保护的是映射和分配等元数据更新。

当前优点是边界明确，限制是多页顺序写会产生多个 transaction。

## 15. 没有实现的 a_ops

当前没有实现：

| 回调/机制 | 当前状态 |
| --- | --- |
| `read_folio` | Linux 5.15 主线仍使用 `readpage` |
| `readahead` | 没有专门预读回调 |
| `direct_IO` | 不支持绕过 page cache 的 DIO |
| `bmap` | 不向用户暴露传统 block mapping 接口 |
| `invalidatepage/releasepage` | 没有 CRYEXTS 私有 page metadata 需要释放 |
| `migratepage` | 未提供文件系统专用页面迁移逻辑 |
| iomap | 当前继续使用自有 `cryexts_resolve_block` |

这些接口只有在明确功能或性能需求出现时才需要增加。

## 16. 当前设计限制

- 只给 regular file 设置 `cryexts_file_aops`。
- 强制一页等于一个文件系统 block。
- `cryexts_fill_page()` 每次分配一个 4 KiB 临时 block buffer。
- `write_cache_pages()` 扫描多页，但实际 callback 逐页提交。
- 每页 writeback 单独 journal commit。
- 加密每个 data block 分配临时副本和 skcipher request。
- 没有 readahead、folio 批处理和跨页事务。

## 17. 最终设计理解

```mermaid
flowchart LR
    LINUX["Linux address_space<br/>管理 page 生命周期"]
    AOPS["cryexts_file_aops<br/>定义 page 与 block 的转换"]
    FORMAT["CRYEXTS 磁盘格式<br/>mapping/allocator/journal/crypto"]

    LINUX -->|何时读页/写页| AOPS
    AOPS -->|如何定位和持久化| FORMAT
    FORMAT -->|page 内容或 errno| AOPS
    AOPS -->|完成状态| LINUX
```

一句话总结：

```text
address_space_operations 是 Linux 缓存页世界与 CRYEXTS 磁盘块世界之间的翻译层。
```
