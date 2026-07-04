# CRYEXTS V4.2 metadata journal / replay

## Goal

V4.2 adds the first minimal recovery path on top of V4.1 block groups:

```text
metadata journal + needs_recovery + mount replay
```

This stage is intentionally a small MVP, not a full ext4-style journal.

## What V4.2 introduces

- `features_compat.HAS_JOURNAL`
- `features_incompat.NEEDS_RECOVERY`
- `superblock.state.NEEDS_RECOVERY`
- `journal_block`
- `journal_blocks`
- mount-time journal replay

## Current journal model

The current implementation uses a minimal metadata journal area:

- journal header block
- journal payload blocks
- each payload block stores one old metadata block image

Conceptually:

```text
before metadata overwrite:
    copy old metadata block -> journal
    set needs_recovery
    overwrite home metadata block

after transaction success:
    clear journal header
    clear needs_recovery
```

If the machine stops between those two phases, next mount replays the journal
back to the home metadata blocks and restores filesystem consistency.

## Replay smoke strategy

The upgraded `scripts/smoke_v4_2_journal_replay.sh` now verifies replay more
directly instead of only exercising the clean path:

1. `mkfs.cryexts -G` creates a journal-capable Version 4 image.
2. `cryexts_journal_inject` copies the current root-directory block into the
   journal payload area.
3. The injector writes a valid journal header, sets
   `features_incompat.NEEDS_RECOVERY` and `superblock.state.NEEDS_RECOVERY`,
   then corrupts the on-disk root-directory home block.
4. A pre-mount `cryextsck` is expected to fail, proving the image is no longer
   clean.
5. The next kernel mount must succeed only if mount-time replay restores the
   corrupted root-directory block from the journal payload.
6. After unmount, `cryextsck` must report the image clean again.

This gives V4.2 a deterministic replay test instead of a "journal feature
exists" test.

## Scope of this MVP

Covered direction:

- superblock recovery state
- bitmap / inode-table / directory-block journaling hooks
- namespace metadata transaction boundary
- mount replay

Not yet covered as a production-grade journal:

- checksummed transactions
- multi-transaction ring log
- ordered data mode
- journal batching
- selective partial replay conflict handling

## Design intent

This version is meant to prove the recovery state machine:

```text
clean -> metadata update starts -> needs_recovery
-> commit success -> clean
-> crash in middle -> next mount replay -> clean
```

That gives CRYEXTS its first real "recoverable filesystem prototype" step.
