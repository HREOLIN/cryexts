# CRYEXTS V4.2 Change Review

## 1. Purpose

This document records the actual V4.2 code changes and explains the logic
behind each part, so the implementation can be reviewed against the intended
recovery design.

V4.2 is the first recoverable-metadata milestone, not a full journaled
filesystem.

## 2. What V4.2 Adds

V4.2 adds a minimal metadata journal / replay skeleton on top of V4.1:

- `features_compat.HAS_JOURNAL`
- `features_incompat.NEEDS_RECOVERY`
- `superblock.state.NEEDS_RECOVERY`
- fixed journal area on disk
- metadata block backup before overwrite
- mount-time replay
- `cryextsck` understanding of journal reservation
- deterministic replay smoke test

## 3. Header-Level On-Disk Changes

### 3.1 `cryexts_fs.h`

Added:

- `CRYEXTS_FEATURE_COMPAT_HAS_JOURNAL`
- `CRYEXTS_FEATURE_INCOMPAT_NEEDS_RECOVERY`
- journal constants
- `struct cryexts_journal_header`

Logic:

- superblock needs a formal way to advertise journal support
- mount path needs a formal way to know recovery is required
- on-disk journal header needs a stable block format

## 4. In-Memory Superblock State Changes

### 4.1 `cryexts.h`

Extended `struct cryexts_sb_info` with:

- `sb`
- `journal_block`
- `journal_blocks`
- `journal_sequence`
- `journal_enabled`
- `journal_replaying`
- `journal_entry_count`
- `journal_home_blocks[]`
- `journal_lock`

Logic:

- kernel mount path needs to know where journal lives
- journal transaction path needs small in-memory tracking
- replay path must distinguish normal updates from replay updates
- lock is needed so one metadata transaction owns the current journal state

## 5. New Runtime Journal Module

### 5.1 `journal.c`

Added functions:

- `cryexts_journal_needs_recovery`
- `cryexts_super_set_recovery`
- `cryexts_journal_begin`
- `cryexts_journal_record_block`
- `cryexts_journal_record_bh`
- `cryexts_journal_commit`
- `cryexts_journal_abort`
- `cryexts_journal_replay`

Logic:

#### `cryexts_journal_begin`

- starts a metadata transaction
- sets `needs_recovery`
- clears previous in-memory entry tracking

Meaning:

```text
from this point on, crash recovery may be needed
```

#### `cryexts_journal_record_block`

- reads the old home metadata block
- writes it into next journal payload slot
- updates the journal header

Meaning:

```text
backup old metadata before overwriting real home block
```

#### `cryexts_journal_commit`

- clears journal header
- clears `needs_recovery`

Meaning:

```text
transaction completed; recovery is no longer needed
```

#### `cryexts_journal_replay`

- runs during mount when recovery is needed
- reads journal header
- walks each payload entry
- copies payload back to its recorded home block
- clears journal header and recovery-needed state

Meaning:

```text
restore old metadata images back to their original disk locations
```

## 6. Mount Path Changes

### 6.1 `super.c`

Added logic to:

- load journal metadata from superblock
- initialize journal lock
- call `cryexts_journal_replay(sb)` during mount

Mount flow now is:

```text
read superblock
-> validate superblock
-> load group metadata / bitmaps
-> configure encryption key if needed
-> if needs_recovery, replay journal
-> continue normal mount validation
```

If replay fails:

- mount fails
- filesystem is not allowed to continue as normal

That is the correct safety direction for this MVP.

## 7. Metadata Write-Path Changes

### 7.1 `balloc.c`

Journal hooks were added around:

- bitmap updates
- superblock free-count updates

Logic:

- block allocation and free mutate metadata
- metadata must be backed up before overwrite if we want crash recovery

### 7.2 `inode.c`

Journal hook added before dirtying inode-table blocks.

Logic:

- inode-table blocks are metadata home blocks
- if inode update is interrupted mid-write, replay must be able to restore them

### 7.3 `dir.c`

Namespace operations now use journal transaction boundaries:

- create
- mkdir
- unlink
- rmdir
- rename
- link
- symlink

Logic:

- these operations mutate several metadata blocks
- they are the first class of operations where crash recovery matters most

Current model:

```text
journal_begin
-> backup old metadata blocks as they are touched
-> perform metadata update
-> journal_commit on success
-> journal_abort on failure path
```

## 8. mkfs Changes

### 8.1 `tools/mkfs.cryexts.c`

Added:

- fixed journal region allocation for `-G` block-group images
- `features_compat.HAS_JOURNAL`
- `journal_block`
- `journal_blocks`
- journal blocks reserved in bitmap and free-count calculations

Logic:

Version 4.2 needs a known journal region on disk, so mkfs must:

- carve out that region
- mark it used
- subtract it from free counts
- advertise it in superblock

Without this, kernel and fsck would disagree about the disk layout.

## 9. `cryextsck` Changes

### 9.1 journal-aware validation

`cryextsck` was updated to understand:

- `HAS_JOURNAL`
- `NEEDS_RECOVERY`
- journal range validation
- journal reservation in free-count calculations

### 9.2 why this was necessary

At first, V4.2 `mkfs` correctly reserved journal blocks, but `cryextsck`
still counted those blocks as free data blocks.

That caused:

```text
superblock free counts mismatch
```

The fix was to make `cryextsck` treat journal blocks as reserved metadata.

Specifically:

- `reserved_metadata_blocks()` now includes journal blocks
- group free-block expectations subtract journal blocks in the affected group
- reserved-bitmap validation checks journal blocks are marked used
- bitmap repair can re-mark journal blocks if needed

Logic:

```text
mkfs, kernel, and fsck must agree that journal blocks are reserved
```

## 10. New Replay Injection Tool

### 10.1 `tools/cryexts_journal_inject.c`

Added a dedicated offline test helper.

What it does:

1. reads a clean journal-capable image
2. copies the current root-directory home block into journal payload
3. writes a valid journal header
4. sets `NEEDS_RECOVERY`
5. corrupts the actual root-directory home block

Logic:

This creates a deterministic recovery-needed image.

Why this matters:

- if we only mount a clean image, we are not really testing replay
- if we only set `needs_recovery` but do not corrupt metadata, replay success is
  too easy to fake
- by corrupting the real root directory block, mount will only succeed if replay
  truly restores it

## 11. Smoke Test Changes

### 11.1 `scripts/smoke_v4_2_journal_replay.sh`

The smoke test now verifies:

1. clean image starts clean
2. injection creates a dirty/corrupted image
3. pre-replay `cryextsck` fails as expected
4. mount-time replay succeeds
5. post-replay `cryextsck` returns clean

Meaning:

```text
clean
-> inject fake crash state
-> fsck confirms corruption
-> mount replays journal
-> fsck confirms clean again
```

This is much stronger than the previous clean-path-only test.

## 12. Current V4.2 Model

The current journal model is:

```text
fixed metadata undo-log area
+ one header block
+ N payload blocks
+ payload stores old metadata block images
+ header stores payload -> home block mapping
+ mount-time replay copies payload back to home blocks
```

This means:

- V4.2 is closer to an undo-style metadata journal
- V4.2 does have begin / record / commit / replay
- V4.2 does not yet have full checkpointing or multi-transaction ring logging

## 13. What V4.2 Still Does Not Have

Not yet implemented:

- full commit block format
- checkpoint subsystem
- multi-transaction journal ring
- data journaling
- checksums
- revoke records
- batched recovery
- read-only degraded recovery mode

That is acceptable because V4.2's target is:

```text
first prove recoverable metadata lifecycle
```

## 14. Review Summary

When reviewing V4.2, the main correctness rule is:

```text
before metadata overwrite:
    old metadata must already be recoverable from journal

after crash:
    mount-time replay must be able to restore those old metadata blocks
```

And the main consistency rule is:

```text
mkfs, kernel mount, and cryextsck must all agree that journal blocks are
reserved metadata blocks
```

Those are the two most important review points for this stage.
