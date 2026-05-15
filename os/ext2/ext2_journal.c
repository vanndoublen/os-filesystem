#include "ext2_journal.h"

#include "../fs/buf.h"
#include "ext2.h"

#ifndef EXT2_ENABLE_JOURNAL

// Compile-time disabled — all entry points become no-ops.
void ext2_journal_init(void)                       {}
void ext2_journal_begin(void)                      {}
void ext2_journal_log(uint32 blkno, void *data)    { (void)blkno; (void)data; }
void ext2_journal_commit(void)                     {}
uint32 ext2_journal_first_reserved_block(void)     { return 0xFFFFFFFFu; }

#else  // EXT2_ENABLE_JOURNAL

// In-memory state.
static uint32 journal_sb_blkno;    // block number of the journal superblock
static uint32 journal_log_start;   // first block of the log area
static struct journal_super j_sb;  // cached journal superblock
static int    j_initialized = 0;

uint32 ext2_journal_first_reserved_block(void) {
    if (!j_initialized) return 0xFFFFFFFFu;
    return journal_sb_blkno;
}

// Mark every block in [start, end) as used in the block bitmap if not
// already used. Used to keep balloc from handing out journal blocks.
static void reserve_blocks(uint32 start, uint32 end) {
    for (uint32 blkno = start; blkno < end; blkno++) {
        uint32 g_idx = (blkno - ext2_info.sb.s_first_data_block) /
                       ext2_info.sb.s_blocks_per_group;
        uint32 b_idx = (blkno - ext2_info.sb.s_first_data_block) %
                       ext2_info.sb.s_blocks_per_group;

        struct ext2_group_desc *gd = &ext2_info.gdt[g_idx];
        struct buf *bp = bread(0, gd->bg_block_bitmap);

        uint32 byte = b_idx >> 3;
        uint32 bit  = b_idx & 7;
        if (!(bp->data[byte] & (1u << bit))) {
            bp->data[byte] |= (1u << bit);
            bwrite(bp);
            // Counters in RAM only — same policy as the rest of the fs.
            gd->bg_free_blocks_count--;
            ext2_info.sb.s_free_blocks_count--;
        }
        brelse(bp);
    }
}

void ext2_journal_init(void) {
    // Compute fixed locations at the tail of the filesystem.
    journal_sb_blkno  = ext2_info.sb.s_blocks_count - JOURNAL_TOTAL_BLOCKS;
    journal_log_start = journal_sb_blkno + 1;

    // Keep balloc from ever handing out journal blocks.
    reserve_blocks(journal_sb_blkno,
                   ext2_info.sb.s_blocks_count);

    // Read the journal superblock. If it has our magic, it's already
    // initialized — note whether a transaction needs replay (handled in J4).
    // Otherwise initialize a fresh one.
    struct buf *bp = bread(0, journal_sb_blkno);
    struct journal_super *psb = (struct journal_super *)bp->data;

    if (psb->magic == JOURNAL_MAGIC) {
        j_sb = *psb;
        infof("ext2 journal: found existing (txn_id=%d status=%d)",
              j_sb.txn_id, j_sb.status);
    } else {
        memset(&j_sb, 0, sizeof(j_sb));
        j_sb.magic           = JOURNAL_MAGIC;
        j_sb.status          = JOURNAL_STATUS_CLEAN;
        j_sb.txn_id          = 0;
        j_sb.txn_block_count = 0;
        j_sb.log_blocks      = JOURNAL_LOG_BLOCKS;
        memmove(bp->data, &j_sb, sizeof(j_sb));
        bwrite(bp);
        infof("ext2 journal: initialized fresh (log at blk %d, %d blocks)",
              journal_log_start, JOURNAL_LOG_BLOCKS);
    }
    brelse(bp);

    j_initialized = 1;
}

// Stubs filled in by J2 / J3.
void ext2_journal_begin(void) {}
void ext2_journal_log(uint32 blkno, void *data) { (void)blkno; (void)data; }
void ext2_journal_commit(void) {}

#endif  // EXT2_ENABLE_JOURNAL
