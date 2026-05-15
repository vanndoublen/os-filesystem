#ifndef EXT2_JOURNAL_H
#define EXT2_JOURNAL_H

#include "defs.h"
#include "types.h"

// Compile-time toggle. Comment this line out to disable journaling
// (useful for comparing benchmark numbers with vs. without).
#define EXT2_ENABLE_JOURNAL 1

// Journal layout — the last (1 + JOURNAL_LOG_BLOCKS) blocks of the
// filesystem are reserved for journal use:
//   blkno = blocks_count - JOURNAL_TOTAL_BLOCKS      : journal superblock
//   blkno = blocks_count - JOURNAL_LOG_BLOCKS  .. -1 : circular log area
#define JOURNAL_LOG_BLOCKS    64
#define JOURNAL_TOTAL_BLOCKS  (JOURNAL_LOG_BLOCKS + 1)

#define JOURNAL_MAGIC         0x4A524E4Cu   // 'JRNL'

// Journal status flags
#define JOURNAL_STATUS_CLEAN    0
#define JOURNAL_STATUS_PENDING  1  // a transaction is committed but not yet applied

// On-disk journal superblock (one block).
struct journal_super {
    uint32 magic;
    uint32 status;          // JOURNAL_STATUS_*
    uint32 txn_id;          // monotonic transaction id of last commit
    uint32 txn_block_count; // # of (blkno, data) pairs in the pending txn
    uint32 log_blocks;      // size of the log area
    uint32 _reserved[1019];
};
static_assert(sizeof(struct journal_super) == 4096);

// On-disk transaction header (lives at log offset 0 for the current txn).
// Followed by `block_count` data blocks, then a commit marker block.
#define JOURNAL_TXN_MAX_BLOCKS  60  // (4096-16)/4 - some slack
struct journal_txn_header {
    uint32 magic;
    uint32 txn_id;
    uint32 block_count;
    uint32 _pad;
    uint32 blknos[JOURNAL_TXN_MAX_BLOCKS];
    uint32 _trailing[1024 - 4 - JOURNAL_TXN_MAX_BLOCKS];
};
static_assert(sizeof(struct journal_txn_header) == 4096);

#define JOURNAL_COMMIT_MARKER  0xC0FFEEu

// API
void ext2_journal_init(void);
void ext2_journal_begin(void);
void ext2_journal_log(uint32 blkno, void *data);
void ext2_journal_commit(void);

// First reserved-by-journal block number (used by balloc to skip).
uint32 ext2_journal_first_reserved_block(void);

#endif  // EXT2_JOURNAL_H
