# eufs v1 journal transaction record format

Status: Stage D on-disk format plus persistent A/B control integration.
Descriptor/commit and journal-control codecs, mkfs initialization, reader
selection, inactive-copy update, the pure v1 ring-reservation planner, and the
unexposed descriptor/payload body writer exist. The public Store API enforces
body durability before A/B control exposure. First-write plans now classify
ordered data separately; the Store validates image identity, complete
before-images, and allocation ownership, then performs ordered-data sync, body
sync, and control exposure on one fd/lock. The exact-body-bound COMMIT writer
persists only the reserved COMMIT block after exposure. The read-only recovery
classifier strictly validates the selected control's one-transaction range, all
payloads, and the exact COMMIT binding before retaining a validated transaction.
The synchronous recovery executor now discards uncommitted ranges or replays all
committed home after-images, synchronizes them, and only then publishes a clean
checkpoint control. FUSE transaction integration, automatic mount recovery, and
multi-transaction circular reclaim are not implemented yet.

## 0. Global design decisions

This format is one component of a metadata redo journal, not an isolated
serialization exercise.

Redo was selected over undo logging because the Stage C planners already
produce complete final block after-images and replaying those images is
idempotent. Copy-on-write was rejected because it would require a different
tree/root-pointer disk architecture.

The journal records metadata only. Newly allocated regular-file data follows
an ordered rule: write it to its home block and complete `fdatasync` before a
COMMIT that exposes metadata references to that data can become durable. This
avoids metadata references to uninitialized new blocks without the write and
space cost of full data journaling.

A transaction is a classified change set, not every block written by one
system call. For the first `hello` write:

```text
ordered data          = {block 292}
metadata after-images = {block 2, block 3}
```

For empty-file create, directory block 291 is metadata even though it is
physically allocated from the data region. The transaction must merge multiple
changes to the same home block before encoding one final payload.

## 1. Source mechanisms and project boundary

The transaction layout borrows mechanisms from, but does not copy the on-disk
format of:

- xv6 `kernel/log.c`: a header identifies home blocks, complete block images
  are placed in the log, the durable header is the commit point, and recovery
  installs committed blocks to their home locations.
- Linux ext4/JBD2 journal documentation: a descriptor identifies following
  journal payload blocks, a complete transaction ends in a commit block, and a
  transaction without a valid commit or checksum is discarded during replay.
- littlefs `DESIGN.md`: two metadata blocks preserve redundancy, CRC detects
  incomplete/corrupt updates, and revision counts are compared with sequence
  arithmetic to handle integer wrap.

Sources:

- `https://github.com/mit-pdos/xv6-riscv/blob/riscv/kernel/log.c`
- `https://docs.kernel.org/filesystems/ext4/journal.html`
- `https://github.com/littlefs-project/littlefs/blob/master/DESIGN.md#metadata-pairs`

eufs uses its own magic values, little-endian fields, CRC32C rules, UUID
binding, control snapshots, and recovery protocol. It does not copy the
littlefs flash log structure and is not compatible with xv6, JBD2, or littlefs.

## 2. Journal region and ring positions

For any image, the journal region is defined by the main superblock. v1
reserves its first two blocks for the A/B control copies:

```text
journal.start_block + 0    control A
journal.start_block + 1    control B
journal.start_block + 2... transaction ring slots
```

A ring index is zero-based within the transaction ring:

```text
physical block = journal.start_block + 2 + ring_index
```

The current 64 MiB fixture has `journal.start_block=35` and
`journal.block_count=256`, so it provides 254 transaction slots:

```text
ring index 0 -> physical block 37
ring index 1 -> physical block 38
...
ring index 253 -> physical block 290
```

The first-write example uses:

```text
ring 0 / block 37    descriptor for txid 1
ring 1 / block 38    after-image for home block 2
ring 2 / block 39    after-image for home block 3
ring 3 / block 40    commit for txid 1
```

## 3. A/B journal control block

Physical journal blocks `journal.start_block + 0` and `+1` contain complete
control snapshots. They are not journaled because journaling the journal's own
location state would create a recursive dependency.

Every integer is little-endian. CRC32C covers the complete 4096-byte block
with the checksum field at offset 124 temporarily set to zero.

| Offset | Size | Field | v1 rule |
|---:|---:|---|---|
| 0 | 8 | magic | ASCII `EUFSJCT1` |
| 8 | 4 | format version | `1` |
| 12 | 4 | header size | `128` |
| 16 | 4 | block size | `4096` |
| 20 | 4 | ring block count | at least `3`; equals journal count minus two controls |
| 24 | 16 | filesystem UUID | must match the main superblock |
| 40 | 8 | generation | compared with 64-bit sequence arithmetic |
| 48 | 4 | head | next append ring index |
| 52 | 4 | tail | oldest uncheckpointed ring index |
| 56 | 4 | used blocks | `0..ring_blocks` |
| 60 | 4 | state flags | `0` |
| 64 | 8 | next transaction id | nonzero |
| 72 | 8 | compatible features | `0` in v1 |
| 80 | 8 | read-only compatible features | `0` in v1 |
| 88 | 8 | incompatible features | `0` in v1 |
| 96 | 28 | reserved | all zero |
| 124 | 4 | control CRC32C | complete block, this field zeroed |
| 128 | 3968 | reserved | all zero |

The ring accounting invariant is:

```text
head == (tail + used_blocks) % ring_blocks
```

`head == tail` alone is ambiguous. `used_blocks=0` means empty, while
`used_blocks=ring_blocks` means full. Nonempty v1 state uses at least three
blocks because the smallest transaction is descriptor + one payload + commit.

Two valid generations are ordered using modulo-2^64 sequence arithmetic:

```text
UINT64_MAX -> 0
```

is a forward increment. A difference of exactly `2^63` is unordered/ambiguous
and produces `EUCLEAN`; choosing either side would be an unsupported guess.
Equal generations are accepted only when every semantic control field is
identical. Equal generation with different state is split-brain corruption.

Selection first rejects invalid magic/version/ranges/UUID/ring size/CRC. A
newer raw generation with invalid CRC is not a candidate. If both copies are
invalid, selection fails. `mkfs` writes identical generation-0 clean controls
with `head=tail=used=0` and `next_transaction_id=1`, synchronizes them, and only
then publishes the main superblock. The reader verifies both copies against the
main superblock UUID and ring geometry. Persisting a later state to the
older/inactive copy is implemented by `JournalControlStore`: it holds an
exclusive image lock, requires an exact generation successor, writes one full
4 KiB snapshot to the opposite copy, calls `fdatasync`, and only then advances
its in-memory selected state. A write/sync error latches reload-required;
callers must stop and reopen/reselect instead of assuming the old copy won.

## 4. Transaction shape

A transaction containing `N` metadata home blocks consumes exactly `N + 2`
journal blocks:

```text
descriptor -> N complete 4 KiB payload blocks -> commit
```

Payload blocks contain raw 4 KiB metadata after-images and do not contain an
embedded header. Their target home block, ring position, and CRC32C are stored
in the descriptor. This preserves a complete home-block image and permits
direct idempotent replay with one full-block write.

The v1 descriptor can hold at most:

```text
(4096 - 64) / 16 = 252 entries
```

## 5. Descriptor block

Every integer is little-endian. CRC32C covers the complete 4096-byte block
with the checksum field at offset 60 temporarily set to zero.

| Offset | Size | Field | v1 rule |
|---:|---:|---|---|
| 0 | 8 | magic | ASCII `EUFSJDS1` |
| 8 | 4 | format version | `1` |
| 12 | 4 | header size | `64` |
| 16 | 4 | record type | `1` (`DESCRIPTOR`) |
| 20 | 4 | entry count | `1..252` |
| 24 | 8 | transaction id | nonzero |
| 32 | 16 | filesystem UUID | must match the image |
| 48 | 4 | transaction block count | `entry_count + 2` |
| 52 | 4 | entries offset | `64` |
| 56 | 4 | flags | `0` |
| 60 | 4 | descriptor CRC32C | complete block, this field zeroed |
| 64 | variable | descriptor entries | `entry_count * 16` bytes |
| after entries | remaining | reserved | all zero |

Each 16-byte entry is:

| Entry offset | Size | Field | v1 rule |
|---:|---:|---|---|
| 0 | 4 | home block | nonzero; unique within the transaction |
| 4 | 4 | payload ring index | unique within the transaction |
| 8 | 4 | payload CRC32C | checksum of the complete 4 KiB payload |
| 12 | 4 | flags | `0` |

Multiple modifications of the same home block must be merged into one final
after-image before descriptor encoding. Duplicate home blocks are rejected.

## 6. Commit block

Every integer is little-endian. CRC32C covers the complete 4096-byte block
with the checksum field at offset 60 temporarily set to zero.

| Offset | Size | Field | v1 rule |
|---:|---:|---|---|
| 0 | 8 | magic | ASCII `EUFSJCM1` |
| 8 | 4 | format version | `1` |
| 12 | 4 | header size | `64` |
| 16 | 4 | record type | `2` (`COMMIT`) |
| 20 | 4 | entry count | must equal the descriptor |
| 24 | 8 | transaction id | must equal the descriptor |
| 32 | 16 | filesystem UUID | must equal the descriptor and image |
| 48 | 4 | transaction block count | must equal the descriptor |
| 52 | 4 | descriptor ring index | identifies the bound descriptor |
| 56 | 4 | descriptor CRC32C | must equal the decoded descriptor CRC |
| 60 | 4 | commit CRC32C | complete block, this field zeroed |
| 64 | 4032 | reserved | all zero |

A structurally valid commit that does not match the descriptor transaction
id, UUID, counts, ring position, or descriptor checksum is corruption and
must produce `EUCLEAN`. It must not be treated as an uncommitted transaction.

## 7. First-write ordered transaction

For the current `hello` first-write fixture:

```text
write newly allocated data block 292
fdatasync(image)

append descriptor for home blocks 2 and 3
append both complete metadata after-images
fdatasync(image)

persist inactive A/B control with:
  head = one slot past the reserved COMMIT position
  used_blocks = 4
  next_transaction_id = 2
fdatasync(image)                 recovery-visible boundary

append matching commit
fdatasync(image)                 commit point

replay/write home blocks 2 and 3
fdatasync(image)

checkpoint and synchronous reclaim via ResolveRecovery
```

Block 292 is not stored in the metadata journal. The ordered-data rule
requires it to be durable before the commit record can become durable.

Descriptor/payload blocks are written and synchronized while their ring slots
are still outside the selected control's `[tail, head)` range. The exposure
control is then persisted before COMMIT. Publishing control before the body
would create an avoidable visible-but-empty reservation; publishing it after
COMMIT could leave a durable COMMIT outside the recovery scan range. This A/B
control publication protocol is eufs-specific; xv6 and JBD2 support the
body-before-commit mechanism but do not define this eufs control format.

Recovery must not replay any payload until the complete descriptor, every
payload CRC, and a matching valid commit have all been verified. Replaying a
valid individual payload before commit validation would apply part of an
uncommitted transaction.

For v1, a nonempty selected range has exactly one grammar:

```text
[tail, head) = descriptor + N payloads + expected COMMIT slot
used_blocks  = N + 2
```

The descriptor must decode at `tail`, all derived positions must remain within
that exact range, and the end position must equal `head`. Recovery never scans
forward for another magic value. A missing/torn COMMIT or a valid stale COMMIT
with another txid leaves the transaction uncommitted. Once control exposed the
range, invalid descriptor/payload CRC is corruption (`EUCLEAN`), because those
blocks were synchronized before exposure. A self-valid COMMIT for the current
txid with mismatched UUID/count/length/descriptor position/descriptor CRC is
also corruption. Only a fully matching COMMIT permits replay.

v1 holds the journal commit lock through synchronous checkpoint, so a new
planner accepts only `used_blocks=0`. Nonzero `used_blocks` returns `-EBUSY`
until recovery/checkpoint resolves the exposed transaction. Supporting several
committed-but-uncheckpointed transactions would require an asynchronous
checkpoint queue, delayed block reuse, and revoke or an equivalent mechanism;
those are outside v1.

## 8. Current evidence and missing work

Implemented and tested:

- descriptor and commit fixed-width little-endian codecs;
- A/B control fixed-width codec and pure selection algorithm;
- mkfs initialization of both physical A/B control blocks;
- reader-side UUID/ring binding and real-image fallback/rejection tests;
- persistent inactive-copy update with exclusive locking;
- pure clean-control ring reservation with nonzero-head wrap and exact fill;
- unexposed descriptor/payload writes and one body `fdatasync` on the same
  Store-owned fd and exclusive lock used for later control exposure;
- stale-plan/forbidden-target rejection, complete payload CRC binding,
  short-write/`EINTR` completion, and uncertain-I/O latching;
- a public API that cannot expose a reservation before its exact body is
  durable in the same Store;
- first-write classification as ordered data `{292}` and metadata `{2,3}`;
- UUID/size/full-before-image validation before ordered data mutation;
- ordered-data sync, unexposed-body sync, and control exposure under one Store
  fd and exclusive image lock;
- allocation-ownership conflict, cross-image identical-byte, and staged
  data/body write/sync error tests;
- exact-body-bound COMMIT construction and durable write after control
  exposure, with no caller-supplied identity or position fields;
- stale-COMMIT-slot overwrite, wrapped physical placement, precondition and
  duplicate rejection, plus COMMIT write/sync uncertainty tests;
- A-to-B-to-A alternation, generation wrap, short-write, and sync-error tests;
- empty/full distinction through `used_blocks`;
- wrap-aware generation comparison and half-range ambiguity rejection;
- complete-block descriptor and commit CRC32C;
- descriptor-to-commit binding;
- duplicate home-block and payload-position rejection;
- bit-flip and nonzero-reserved-byte rejection;
- read-only recovery classification into empty, uncommitted, or committed;
- strict descriptor-at-tail, contiguous-payload, exact-length/head, UUID, txid,
  home-target, payload-CRC, and COMMIT-binding validation;
- missing/torn/stale-txid COMMIT classification without exposing payloads;
- wrapped transaction parsing, failure-output atomicity, whole-image no-write
  checks, and rejection of a valid later transaction after a corrupt tail;
- synchronous EMPTY/no-op, UNCOMMITTED/discard, and COMMITTED/replay/checkpoint
  execution on a newly opened Store;
- complete home after-image writes followed by one home `fdatasync`, then an A/B
  clean control with unchanged head/next-txid and `tail=head, used=0`;
- partial home replay, home-sync uncertainty, torn checkpoint control,
  checkpoint-sync uncertainty, retry, and second-recovery convergence tests;
- real first-write recovery to inode 2, allocated block 292, size 5, and
  `hello`, while stale ring bytes remain outside the clean range.

Not implemented and therefore not claimable:

- FUSE transaction integration;
- automatic mount recovery or daemon crash failpoints;
- asynchronous/multi-transaction reclaim and revoke;
- crash consistency of create or write.

Focused normal and ASan/UBSan classifier evidence:

- `evidence/runtime/stage-d-recovery-classifier-normal-raw-20260723.log`
- `evidence/runtime/stage-d-recovery-classifier-sanitize-raw-20260723.log`
- `evidence/runtime/stage-d-recovery-executor-normal-raw-20260723.log`
- `evidence/runtime/stage-d-recovery-executor-sanitize-raw-20260723.log`
