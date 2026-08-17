# CRYEXTS

[![License: GPL v2](https://img.shields.io/badge/License-GPL_v2-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)

CRYEXTS is a clean restart of the experimental filesystem work. The first
milestone keeps the scope intentionally small:

```text
mkfs.cryexts -> insmod -> mount -> ls -> umount
```

The current kernel target is Ubuntu `5.15.0-139-generic`.

## Build

```bash
make
```

This builds:

- `cryexts.ko`
- `mkfs.cryexts`

## Phase 1 Smoke Test

```bash
dd if=/dev/zero of=cryexts.img bs=1M count=64
./mkfs.cryexts -f cryexts.img
sudo insmod cryexts.ko
mkdir -p /tmp/cryexts-mnt
sudo mount -o loop -t cryexts cryexts.img /tmp/cryexts-mnt
ls -la /tmp/cryexts-mnt
sudo umount /tmp/cryexts-mnt
sudo rmmod cryexts
```

Expected result for phase 1: `ls -la` shows the root directory with `.` and
`..`, and `dmesg` has no oops or panic.

You can run the same flow with:

```bash
chmod +x scripts/smoke_phase1.sh
./scripts/smoke_phase1.sh
```

## Phase 2 Smoke Test

Phase 2 adds a deliberately small persistent path for directories and one-block
regular files:

```bash
chmod +x scripts/smoke_phase2.sh
./scripts/smoke_phase2.sh
```

Expected result for phase 2: `mkdir`, `touch`, `tee > file`, `cat file`, and a
remount persistence check all succeed.

## Phase 3 Smoke Test

Phase 3 focuses on consistency checking and early error rejection:

```bash
chmod +x scripts/smoke_phase3.sh scripts/corrupt_phase3.sh
./scripts/smoke_phase3.sh
./scripts/corrupt_phase3.sh
```

Expected result for phase 3: clean images pass `cryextsck`, and corrupted
images fail before causing kernel crashes.

## Phase 4 Smoke Test

Phase 4 adds a minimal transparent encryption MVP for regular file data blocks:

```bash
chmod +x scripts/smoke_phase4.sh
./scripts/smoke_phase4.sh
```

Expected result for phase 4: encrypted images require the correct `key=` mount
option, file reads return plaintext, and the raw image does not contain the
written plaintext string.

## Version 2.0 Layout Smoke Test

Version 2.0 bumps the on-disk format and introduces fixed block/inode bitmap
metadata. The allocator still uses the previous sequential path until V2.1.

```bash
chmod +x scripts/smoke_v2_0_layout.sh
./scripts/smoke_v2_0_layout.sh
```

Expected result for V2.0: `mkfs.cryexts` creates a version 2 image,
`cryextsck` validates the bitmap metadata, and the existing mount/read/write
path still works.

## Version 2.1 Bitmap Smoke Test

Version 2.1 switches inode/block allocation and release to bitmap-driven paths.

```bash
chmod +x scripts/smoke_v2_1_bitmap.sh
./scripts/smoke_v2_1_bitmap.sh
```

Expected result for V2.1: deleted files and directories release resources,
re-created objects still work after remount, and `cryextsck` stays clean.

## Version 2.2 Direct-Block Smoke Test

Version 2.2 upgrades regular files from the previous single-block path to
multi-block direct-block I/O.

```bash
chmod +x scripts/smoke_v2_2_direct_blocks.sh
./scripts/smoke_v2_2_direct_blocks.sh
```

Expected result for V2.2: multi-block file write/read, remount persistence,
truncate, and encrypted multi-block write/read all succeed.

## Version 2.3 Large Directory Smoke Test

Version 2.3 upgrades directories from one block to multiple direct blocks.

```bash
chmod +x scripts/smoke_v2_3_large_dir.sh
./scripts/smoke_v2_3_large_dir.sh
```

Expected result for V2.3: a single directory can hold hundreds of entries,
`ls` spans multiple directory blocks, and delete/recreate stays clean.

## Version 2.4 cryextsck Smoke Test

Version 2.4 enhances `cryextsck` with bitmap/reference consistency checks and
low-risk repair.

```bash
chmod +x scripts/smoke_v2_4_cryextsck.sh
./scripts/smoke_v2_4_cryextsck.sh
```

Expected result for V2.4: clean images pass, corrupted bitmap images fail, and
`--repair` can restore the low-risk metadata mismatch.

## Version 2.5 Encryption-Layer Smoke Test

Version 2.5 keeps the XOR MVP data path but reorganizes encryption metadata
into a `salt + kdf + verifier + algorithm` flow that is easier to evolve.

```bash
chmod +x scripts/smoke_v2_5_encryption.sh
./scripts/smoke_v2_5_encryption.sh
```

Expected result for V2.5: encrypted images validate with `cryextsck`, the
correct key mounts successfully, the wrong key is rejected, multi-block reads
stay correct, and plaintext is not visible in the raw image.

## Version 3.0 Single-Indirect Smoke Test

Version 3.0 upgrades regular files from 12 direct blocks to
`12 direct + 1 single indirect block`.

```bash
chmod +x scripts/smoke_v3_0_indirect_file.sh
./scripts/smoke_v3_0_indirect_file.sh
```

Expected result for V3.0: a 1MB file can be copied in and back out correctly,
the direct/indirect boundary stays correct, truncate still works, and
`cryextsck` reports a clean image.

## Version 3.1 Rename Smoke Test

Version 3.1 adds same-directory rename, cross-directory move, and replacement
paths for files and empty directories.

```bash
chmod +x scripts/smoke_v3_1_rename.sh
./scripts/smoke_v3_1_rename.sh
```

Expected result for V3.1: `mv` works across the tested rename paths, illegal
directory self-subtree moves are rejected, remount stays consistent, and
`cryextsck` remains clean.

## Version 3.2 fsync Smoke Test

Version 3.2 adds a minimal `fsync` / `sync_fs` path for explicit metadata
writeback.

```bash
chmod +x scripts/smoke_v3_2_fsync.sh
./scripts/smoke_v3_2_fsync.sh
```

Expected result for V3.2: `fsync()` can be called successfully, file data is
still correct after remount, and `cryextsck` reports a clean image.

## Version 3.3 Crypto API Smoke Test

Version 3.3 replaces the previous XOR data path with a Linux Crypto API based
encrypted data-block path while preserving the existing mount key / verifier
metadata flow.

```bash
chmod +x scripts/smoke_v3_3_crypto_api.sh
./scripts/smoke_v3_3_crypto_api.sh
```

Expected result for V3.3: the correct key mounts successfully, the wrong key
is rejected, multi-block encrypted reads stay correct, and plaintext is not
visible in the raw image.

## Version 3.4 hard link / symlink Smoke Test

Version 3.4 adds hard link, symlink, and correct multi-name inode link-count
handling.

```bash
chmod +x scripts/smoke_v3_4_links.sh
./scripts/smoke_v3_4_links.sh
```

Expected result for V3.4: `ln` and `ln -s` both work, unlink only frees an
inode when the last hard link is removed, remount stays consistent, and
`cryextsck` reports a clean image.

## Version 4.0 Layout Smoke Test

Version 4.0 upgrades the superblock format with state, mount counters, uuid,
volume metadata, and Version 4 feature-flag rules while still keeping the
filesystem on a single-group layout.

```bash
chmod +x scripts/smoke_v4_0_layout.sh
./scripts/smoke_v4_0_layout.sh
```

Expected result for V4.0: `mkfs.cryexts` creates a Version 4 image, the
kernel can mount and unmount it, `cryextsck` validates the Version 4 metadata,
and the image remains clean after remount.

## Version 4.1 Block-Group Smoke Test

Version 4.1 upgrades the on-disk layout from a single global bitmap region to
a real block-group based layout with group descriptor metadata and per-group
bitmaps.

```bash
chmod +x scripts/smoke_v4_1_block_groups.sh
./scripts/smoke_v4_1_block_groups.sh
```

Expected result for V4.1: `mkfs.cryexts -G` creates a multi-group image, the
kernel can mount it, basic mkdir/write/read still works, and `cryextsck`
recognizes the block-group metadata.

## Version 4.2 Journal Smoke Test

Version 4.2 adds the first minimal metadata-journal and recovery-state
framework on top of the V4.1 block-group layout.

```bash
chmod +x scripts/smoke_v4_2_journal_replay.sh
./scripts/smoke_v4_2_journal_replay.sh
```

Expected result for V4.2: the script first injects a synthetic
`needs_recovery` journal image, the next mount replays the saved metadata
block automatically, metadata-heavy operations such as mkdir/write/rename
still work, and the final `cryextsck` reports a clean image.

## Version 5.0 Layout Smoke Test

Version 5.0 introduces the Version 5 on-disk format baseline: new superblock
reserved fields, expanded feature flags, and matching kernel / `cryextsck`
validation. At this stage, orphan list, directory index, policy table, and
extent tree are format-level groundwork rather than fully active runtime
features.

```bash
chmod +x scripts/smoke_v5_0_layout.sh
./scripts/smoke_v5_0_layout.sh
```

Expected result for V5.0: `mkfs.cryexts` creates a Version 5 image,
`cryextsck` validates it as clean, the kernel can mount and unmount it, and a
second `cryextsck` pass still reports a clean image.

## Version 5.2 Extent Overflow Smoke Test

Version 5.2 extends the inline-only extent mapping into a single-level extent
overflow design: the inode keeps a small extent root, and larger fragmented
files may spill into one overflow extent block.

```bash
chmod +x scripts/smoke_v5_2_extent_tree.sh
./scripts/smoke_v5_2_extent_tree.sh
```

Expected result for V5.2: a large regular file grows beyond the inode's inline
extent root, the smoke script confirms `overflow_block` is non-zero via the
offline inspector, remount still preserves the mapping, truncate still works,
and `cryextsck` reports a clean image before and after the test.

## Version 6.2 Multi-Leaf Extent Tree Smoke Test

Version 6.2 upgrades the regular-file extent mapping model from
`inline + single overflow block` into a multi-leaf extent tree MVP with an
inode root and multiple extent leaf blocks.

```bash
chmod +x scripts/smoke_v6_2_extent_tree.sh
./scripts/smoke_v6_2_extent_tree.sh
```

Expected result for V6.2: the test forces many short extents, the offline
inspector reports `tree_v2=1` with `leaf_count >= 2`, truncate still works,
and `cryextsck` reports a clean image before and after the truncate path.

## Version 6.3 Sparse File / Punch-Hole Smoke Test

Version 6.3 makes the extent tree sparse-aware: missing logical ranges are
valid holes, reads from holes return zeroes, and `fallocate -p` can release
mapped full blocks while keeping the file size unchanged.

```bash
chmod +x scripts/smoke_v6_3_sparse_file.sh
./scripts/smoke_v6_3_sparse_file.sh
```

Expected result for V6.3: a write at a high logical offset creates a leading
hole without allocating all earlier blocks, `fallocate -p` removes the middle
mapping from a file, remount preserves both holes, and `cryextsck` reports a
clean image before and after the test.

## Version 6.4 Allocator Locality Smoke Test

Version 6.4 upgrades allocator policy with inode goal-group allocation and a
per-inode soft reservation window for regular-file data blocks.

```bash
chmod +x scripts/smoke_v6_4_allocator.sh
./scripts/smoke_v6_4_allocator.sh
```

Expected result for V6.4: same-directory sibling inodes are allocated in one
group, the sequential test file's first data block is in the same group as its
inode, the largest extent is at least one reservation window, and `cryextsck`
reports a clean image.

## Version 6.5 Directory-Index Maintenance Smoke Test

Version 6.5 upgrades directory-index updates from full rebuilds after every
namespace change to an incremental add/remove maintenance path with fsck
cross-checking.

```bash
chmod +x scripts/smoke_v6_5_dir_index_maintenance.sh
./scripts/smoke_v6_5_dir_index_maintenance.sh
```

Expected result for V6.5: create, unlink, rename, and hard-link operations keep
the directory index masks and entry count consistent, remount lookup still
works, and `cryextsck` reports a clean image.

## Version 10.0 Performance Baseline Smoke Test

Version 10.0 does not change the regular-file I/O path yet. It freezes a
repeatable benchmark baseline so later page-cache and buffered-write work can
be measured against one fixed reference.

```bash
chmod +x scripts/smoke_v10_0_performance_baseline.sh
./scripts/smoke_v10_0_performance_baseline.sh
```

Expected result for V10.0: the script builds and mounts CRYEXTS, reports
baseline numbers for sequential write/read, small-file create/unlink, and
directory scan, then unmounts cleanly and `cryextsck` still reports a clean
filesystem.

## Version 10.1 Cached-Read Smoke Test

Version 10.1 moves regular-file reads onto the Linux page-cache path while
keeping the old direct-write path temporarily coherent by invalidating cached
pages after overwrites.

```bash
chmod +x scripts/smoke_v10_1_cached_read.sh
./scripts/smoke_v10_1_cached_read.sh
```

Expected result for V10.1: repeated reads of the same file stay correct,
overwriting a cached file does not return stale data on the next read, remount
still works, and `cryextsck` reports a clean filesystem after the smoke.

## Version 10.2 Page-Cache Write Baseline Smoke Test

Version 10.2 moves regular-file writes onto the Linux
`generic_file_write_iter` plus `write_begin/write_end` path. This stage uses
synchronous write-through; dirty-page aggregation and asynchronous writeback
remain scoped to Version 10.3.

```bash
chmod +x scripts/smoke_v10_2_buffered_write.sh
./scripts/smoke_v10_2_buffered_write.sh
```

Expected result for V10.2: small writes, unaligned overwrite, append, truncate,
and remount persistence all preserve exact file contents, and `cryextsck`
reports a clean filesystem after the smoke.

## Version 10.3 Writeback Smoke Test

Version 10.3 changes `write_end` to leave dirty page-cache pages and adds
`writepage/writepages` for delayed block allocation, encrypted data write,
inode persistence, and per-page journal commit.

```bash
chmod +x scripts/smoke_v10_3_writeback.sh
./scripts/smoke_v10_3_writeback.sh
```

Expected result for V10.3: dirty small writes are readable before fsync,
`fsync` and `sync` persist exact contents across remount, unlinking a dirty
file does not resurrect it, and `cryextsck` reports a clean filesystem.

## Current Scope

Implemented:

- Minimal on-disk superblock.
- Minimal root inode.
- Minimal root directory block.
- Kernel filesystem registration as `cryexts`.
- Block-device mount path.
- Root directory iteration for `ls`.
- Fixed inode table for small test files.
- Append-only block allocation.
- `mkdir`, `touch`, small-file write, small-file read.
- mount-time superblock/inode/dirent validation.
- `cryextsck` check-only tool.
- corrupt-image rejection test.
- `mkfs.cryexts -E <key>` encrypted-volume marker.
- `mount -o key=<key>` encrypted-volume key verification.
- transparent encryption/decryption for regular file data blocks.
- CRYEXTS version 2 superblock layout.
- Fixed block bitmap and inode bitmap metadata blocks.
- V2.0 layout validation in the kernel and `cryextsck`.
- V2.1 bitmap-driven inode/block allocation and release.
- V2.2 multi-block regular files via 12 direct blocks.
- V2.3 multi-block directories via 12 direct blocks.
- V2.4 enhanced cryextsck consistency checks.
- V2.5 structured encryption metadata with salt, KDF id, algorithm id, and
  derived-key verifier.
- V3.0 single-indirect block support for larger regular files.
- V3.1 rename and cross-directory directory-consistency updates.
- V3.2 minimal fsync/sync_fs metadata writeback path.
- V3.3 Linux Crypto API backed encrypted regular-file data path.
- V3.4 hard link, symlink, and correct hard-link lifetime handling.
- V4.0 superblock layout and state/feature groundwork for Version 4.
- V4.1 block-group metadata and group-aware allocation groundwork.
- V5.0 superblock / feature-flag baseline for future orphan, indexed-directory,
  policy-table, and extent-tree work.
- V5.1 orphan list mount-time cleanup.
- V5.2 single overflow extent block for larger extent-backed regular files.
- V6.2 multi-leaf extent tree MVP for larger extent-backed regular files.
- V6.3 sparse file reads and punch-hole support.
- V6.4 inode/data locality and per-inode soft reservation windows.
- V6.5 incremental directory-index maintenance for namespace updates.

Not implemented yet:

- production-grade cryptography
- filename encryption
