# CRYEXTS V4.2 Journal Layout and Logic

## 1. Purpose

This document explains the exact logic of the current V4.2 journal MVP:

- what is stored in journal
- how header and payload are laid out
- what replay actually does
- what commit means in the current implementation
- what is still missing compared with a full journaling filesystem

This is a teaching-oriented metadata recovery journal, not a full ext4-style
journal subsystem.

## 2. Current Journal Area Layout

The journal occupies one fixed contiguous region on disk:

```text
journal_block ... journal_block + journal_blocks - 1
```

Its internal layout is:

```text
journal_block
    = journal header block

journal_block + 1
    = payload #0

journal_block + 2
    = payload #1

journal_block + 3
    = payload #2

...
```

So the current journal has:

- one header block
- multiple payload blocks

Each payload block stores one full old metadata-block image.

## 3. Header Structure

The journal header is defined by `struct cryexts_journal_header`.

Key fields:

- `magic`
- `flags`
- `entry_count`
- `sequence`
- `home_blocks[]`

Meaning:

- `magic`
  Identifies the block as a CRYEXTS journal header.
- `flags`
  Currently the important bit is `CRYEXTS_JOURNAL_FLAG_VALID`.
- `entry_count`
  Number of payload blocks currently recorded in the transaction.
- `sequence`
  Simple transaction sequence number.
- `home_blocks[i]`
  The real disk block number that payload `i` should restore during replay.

So header works like an index:

```text
payload #0 -> restore to home_blocks[0]
payload #1 -> restore to home_blocks[1]
payload #2 -> restore to home_blocks[2]
```

## 4. Payload Meaning

Payload does not store logical operations such as:

- mkdir
- rename
- unlink

Instead, payload stores full old metadata block images.

Example:

```text
home block 8 = root directory metadata block
```

Before modifying block 8:

1. read block 8
2. copy its entire old content into `journal_block + 1`
3. record `home_blocks[0] = 8`

Then if crash happens later, replay can do:

```text
copy payload #0 back to block 8
```

So payload is physical block-image backup, not logical operation log.

## 5. Why This Is an Undo-Style Journal

The current journal stores the old metadata before modification.

That means recovery does:

```text
restore old metadata block image back to home location
```

This is closer to:

```text
undo log
```

Not:

```text
redo log
```

Because replay restores the filesystem to the pre-update state, instead of
re-applying the new update.

## 6. Runtime Flow

### 6.1 Begin

When a metadata transaction starts:

- `cryexts_journal_begin(sb)`
- sets recovery-needed state
- clears in-memory journal-entry tracking for this transaction

Conceptually:

```text
transaction starts
-> filesystem becomes recovery-sensitive
```

### 6.2 Record

Before overwriting a metadata block:

- `cryexts_journal_record_block(sb, home_block)`
- reads the current home block
- writes the old block image into the next payload slot
- writes/updates header

Conceptually:

```text
old metadata block
-> copy into payload
-> remember its original home block in header
```

### 6.3 Commit

When metadata update completes successfully:

- `cryexts_journal_commit(sb)`
- writes a cleared/invalidated journal header
- clears `needs_recovery`

In the current model, commit means:

```text
this transaction no longer needs crash recovery
```

It does not mean a full ext4-style committed-transaction lifecycle exists.

### 6.4 Abort

If the transaction path fails before success:

- `cryexts_journal_abort(sb)`
- clears in-memory transaction tracking
- unlocks journal state

This is still minimal. It is not a full rollback framework by itself.

### 6.5 Replay

At mount time:

1. read superblock
2. if `needs_recovery` is set, call `cryexts_journal_replay(sb)`
3. read journal header
4. check `magic`, `flags`, `entry_count`
5. for each payload:
   - read payload block
   - write it back to the corresponding `home_blocks[i]`
6. clear journal header
7. clear `needs_recovery`

Conceptually:

```text
payload #i
-> copy back to home_blocks[i]
-> overwrite corrupted home metadata
```

## 7. Example Mapping

Suppose:

- `journal_block = 24064`
- `entry_count = 2`
- `home_blocks[0] = 8`
- `home_blocks[1] = 3`

Then:

```text
block 24064 = header
block 24065 = payload #0 = old image of home block 8
block 24066 = payload #1 = old image of home block 3
```

Replay means:

```text
copy block 24065 -> block 8
copy block 24066 -> block 3
```

## 8. Is There Commit?

Yes, but very simplified.

Current `commit` means:

- journal header is cleared
- recovery state is cleared
- this transaction is considered complete

Current commit does not include:

- separate commit block
- transaction checksum
- transaction barrier ordering logic
- committed-but-not-checkpointed state

So there is a commit step, but it is an MVP commit.

## 9. Is There Checkpoint?

Not as a separate subsystem.

In a more complete journaling design, commit and checkpoint are different:

- commit
  transaction is durably present in journal
- checkpoint
  home blocks are durably updated and journal space can be reclaimed

Current CRYEXTS V4.2 does not separate these phases formally.

Its current flow is closer to:

```text
backup old metadata into journal
-> overwrite home metadata directly
-> if success, clear journal
-> if crash, replay old metadata back
```

So current V4.2 has:

- begin
- record
- commit
- replay

But it does not yet have:

- checkpoint queue
- multi-transaction ring
- journal head/tail management
- revoke records
- partial committed transaction tracking

## 10. Current Boundaries

What V4.2 does provide:

- metadata recovery state
- fixed journal area
- mount-time replay
- deterministic recovery test

What V4.2 does not yet provide:

- full transaction engine
- data journaling
- multi-transaction batching
- checksummed journal
- checkpoint subsystem
- advanced replay conflict handling

## 11. Summary

The current V4.2 journal can be summarized as:

```text
fixed metadata undo-log region
+ one header block
+ N payload blocks
+ payload stores old metadata block images
+ header tells replay where each payload belongs
+ mount-time replay copies payload back to home blocks
```

This is enough to prove:

- recovery state can be represented on disk
- mount can detect recovery-needed state
- journal replay can restore corrupted metadata

That is exactly the intended V4.2 milestone.
