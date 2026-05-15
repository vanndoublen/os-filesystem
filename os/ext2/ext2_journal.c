#include "ext2_journal.h"

#include "../fs/buf.h"
#include "ext2.h"

#ifdef EXT2_ENABLE_JOURNAL

// On-disk fixed locations.
static uint32 journal_sb_blkno;
static uint32 journal_log_start;
static struct journal_super j_sb;
static int    j_initialized = 0;

// In-memory pending transaction. We only store the buf pointers (pinned via
// bpin) and their blknos; the modified data lives in the bufs themselves.
#define J_MAX_ENTRIES 32
static struct {
    int    active;
    uint32 count;
    uint32 blknos[J_MAX_ENTRIES];
    struct buf *bufs[J_MAX_ENTRIES];
} j_txn;

uint32 ext2_journal_first_reserved_block(void) {
    if (!j_initialized) return 0xFFFFFFFFu;
    return journal_sb_blkno;
}

// Return 1 iff `blkno` is free in the bitmap.
static int block_is_free(uint32 blkno) {
    uint32 g_idx = (blkno - ext2_info.sb.s_first_data_block) /
                   ext2_info.sb.s_blocks_per_group;
    uint32 b_idx = (blkno - ext2_info.sb.s_first_data_block) %
                   ext2_info.sb.s_blocks_per_group;
    if (g_idx >= ext2_info.num_groups) return 0;

    struct ext2_group_desc *gd = &ext2_info.gdt[g_idx];
    struct buf *bp = bread(0, gd->bg_block_bitmap);
    uint32 byte = b_idx >> 3;
    uint32 bit  = b_idx & 7;
    int free   = !(bp->data[byte] & (1u << bit));
    brelse(bp);
    return free;
}

// Scan from the end of the filesystem backwards, looking for the highest
// contiguous run of `needed` free blocks. Returns the starting blkno, or
// 0 if no such run exists.
static uint32 find_journal_area(uint32 needed) {
    if (ext2_info.sb.s_blocks_count < needed)
        return 0;

    uint32 run_end = ext2_info.sb.s_blocks_count;
    uint32 run_len = 0;

    for (int64 blkno = (int64)ext2_info.sb.s_blocks_count - 1;
         blkno >= (int64)ext2_info.sb.s_first_data_block;
         blkno--) {
        if (block_is_free((uint32)blkno)) {
            run_len++;
            if (run_len == needed)
                return (uint32)blkno;
        } else {
            run_len = 0;
            run_end = (uint32)blkno;
            (void)run_end;
        }
    }
    return 0;
}

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
            gd->bg_free_blocks_count--;
            ext2_info.sb.s_free_blocks_count--;
        }
        brelse(bp);
    }
}

// Persist the cached j_sb to disk.
static void write_j_sb(void) {
    struct buf *bp = bread(0, journal_sb_blkno);
    memmove(bp->data, &j_sb, sizeof(j_sb));
    bwrite(bp);
    brelse(bp);
}

// J4 — Replay any committed-but-not-applied transaction on mount.
// Triggered when status == PENDING at mount time.
static void journal_replay(void) {
    if (j_sb.status != JOURNAL_STATUS_PENDING)
        return;
    if (j_sb.txn_block_count == 0 || j_sb.txn_block_count > J_MAX_ENTRIES) {
        infof("ext2 journal: PENDING but bad block_count=%d; skipping replay",
              j_sb.txn_block_count);
        j_sb.status = JOURNAL_STATUS_CLEAN;
        j_sb.txn_block_count = 0;
        write_j_sb();
        return;
    }

    infof("ext2 journal: replaying txn %d (%d blocks)",
          j_sb.txn_id, j_sb.txn_block_count);

    // Read transaction header.
    struct buf *hb = bread(0, journal_log_start);
    struct journal_txn_header th;
    memmove(&th, hb->data, sizeof(th));
    brelse(hb);

    if (th.magic != JOURNAL_MAGIC || th.txn_id != j_sb.txn_id) {
        infof("ext2 journal: header mismatch; skipping replay");
        j_sb.status = JOURNAL_STATUS_CLEAN;
        j_sb.txn_block_count = 0;
        write_j_sb();
        return;
    }

    // Re-apply each logged block.
    for (uint32 i = 0; i < th.block_count && i < JOURNAL_TXN_MAX_BLOCKS; i++) {
        uint32 dst = th.blknos[i];
        struct buf *src = bread(0, journal_log_start + 1 + i);
        struct buf *dbp = bread(0, dst);
        memmove(dbp->data, src->data, BSIZE);
        bwrite(dbp);
        brelse(dbp);
        brelse(src);
    }

    j_sb.status = JOURNAL_STATUS_CLEAN;
    j_sb.txn_block_count = 0;
    write_j_sb();
    infof("ext2 journal: replay complete");
}

void ext2_journal_init(void) {
    // Find a contiguous free run for the journal area. Scanning from the
    // end keeps the journal out of the typical "early data block" zone
    // where mke2fs places the root directory and other initial content.
    uint32 sb = find_journal_area(JOURNAL_TOTAL_BLOCKS);
    if (sb == 0) {
        infof("ext2 journal: no free run for journal area; disabling");
        j_initialized = 0;
        return;
    }
    journal_sb_blkno  = sb;
    journal_log_start = journal_sb_blkno + 1;

    reserve_blocks(journal_sb_blkno, journal_sb_blkno + JOURNAL_TOTAL_BLOCKS);

    struct buf *bp = bread(0, journal_sb_blkno);
    struct journal_super *psb = (struct journal_super *)bp->data;

    if (psb->magic == JOURNAL_MAGIC) {
        j_sb = *psb;
        infof("ext2 journal: found existing (txn_id=%d status=%d)",
              j_sb.txn_id, j_sb.status);
        brelse(bp);
        journal_replay();
    } else {
        memset(&j_sb, 0, sizeof(j_sb));
        j_sb.magic      = JOURNAL_MAGIC;
        j_sb.status     = JOURNAL_STATUS_CLEAN;
        j_sb.log_blocks = JOURNAL_LOG_BLOCKS;
        memmove(bp->data, &j_sb, sizeof(j_sb));
        bwrite(bp);
        brelse(bp);
        infof("ext2 journal: initialized fresh (sb at blk %d, log at blk %d, %d blocks)",
              journal_sb_blkno, journal_log_start, JOURNAL_LOG_BLOCKS);
    }

    j_initialized = 1;
}

void ext2_journal_begin(void) {
    if (!j_initialized) return;
    // Nested begins are a no-op: outermost begin/commit wins.
    if (j_txn.active) return;
    j_txn.active = 1;
    j_txn.count  = 0;
}

void ext2_journal_write(struct buf *bp) {
    if (!j_initialized || !j_txn.active) {
        // No active transaction — fall through to a normal bwrite.
        bwrite(bp);
        return;
    }

    // Coalesce: if this block is already in the txn, the buffer cache
    // hands the caller the same pinned buf, so its in-memory data already
    // reflects the latest modification. Nothing to do.
    for (uint32 i = 0; i < j_txn.count; i++) {
        if (j_txn.blknos[i] == bp->blockno)
            return;
    }

    if (j_txn.count >= J_MAX_ENTRIES) {
        // Transaction too large; fall back to direct write. Atomicity is
        // weakened for this entry but the rest of the txn is unaffected.
        bwrite(bp);
        return;
    }

    bpin(bp);  // keep this buf in cache (with our modifications) until commit
    j_txn.blknos[j_txn.count] = bp->blockno;
    j_txn.bufs[j_txn.count]   = bp;
    j_txn.count++;
    // Deliberately no bwrite — real disk write is deferred to commit.
}

void ext2_journal_commit(void) {
    if (!j_initialized || !j_txn.active) return;
    if (j_txn.count == 0) {
        j_txn.active = 0;
        return;
    }

    // 1) Write transaction header to journal log block 0.
    struct buf *hb = bread(0, journal_log_start);
    memset(hb->data, 0, BSIZE);
    struct journal_txn_header *th = (struct journal_txn_header *)hb->data;
    th->magic       = JOURNAL_MAGIC;
    th->txn_id      = ++j_sb.txn_id;
    th->block_count = j_txn.count;
    for (uint32 i = 0; i < j_txn.count; i++)
        th->blknos[i] = j_txn.blknos[i];
    bwrite(hb);
    brelse(hb);

    // 2) Write each pinned data block's contents to its log slot.
    for (uint32 i = 0; i < j_txn.count; i++) {
        struct buf *src = bread(0, j_txn.blknos[i]);   // cache hit (pinned)
        struct buf *dst = bread(0, journal_log_start + 1 + i);
        memmove(dst->data, src->data, BSIZE);
        bwrite(dst);
        brelse(dst);
        brelse(src);
    }

    // 3) Commit marker: SB → PENDING. From this instant, the txn is
    //    durable in the journal; replay-on-mount will finish it if we
    //    crash before step 5.
    j_sb.status          = JOURNAL_STATUS_PENDING;
    j_sb.txn_block_count = j_txn.count;
    write_j_sb();

    // 4) Apply to real locations. Pinned bufs hold the modifications; we
    //    re-bread (cache hit) to get a lockable handle, bwrite, then
    //    bunpin to release the long-lived ref.
    for (uint32 i = 0; i < j_txn.count; i++) {
        struct buf *rb = bread(0, j_txn.blknos[i]);
        bwrite(rb);
        brelse(rb);
        bunpin(j_txn.bufs[i]);
        j_txn.bufs[i] = NULL;
    }

    // 5) SB → CLEAN: transaction fully retired.
    j_sb.status          = JOURNAL_STATUS_CLEAN;
    j_sb.txn_block_count = 0;
    write_j_sb();

    j_txn.count  = 0;
    j_txn.active = 0;
}

#else  // !EXT2_ENABLE_JOURNAL

void ext2_journal_init(void)                    {}
void ext2_journal_begin(void)                   {}
void ext2_journal_write(struct buf *bp)         { bwrite(bp); }
void ext2_journal_commit(void)                  {}
uint32 ext2_journal_first_reserved_block(void)  { return 0xFFFFFFFFu; }

#endif  // EXT2_ENABLE_JOURNAL
