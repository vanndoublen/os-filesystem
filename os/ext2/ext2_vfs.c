#include "ext2.h"
#include "ext2_journal.h"

#include "../fs/buf.h"

// VFS mount handle for our ext2 filesystem.
static struct superblock ext2_sb_vfs;

// Forward decls.
static struct inode *ext2_iget(struct superblock *sb, uint32 ino);
static void          ext2_write_inode(struct inode *ind);
static void          ext2_delete_inode(struct inode *inode);
static void          ext2_free_inode(struct inode *inode);

static int ext2_lookup(struct inode *parent, struct dentry *dentry);
static int ext2_create(struct inode *parent, struct dentry *dentry);
static int ext2_unlink(struct inode *parent, struct dentry *dentry);
static int ext2_mkdir(struct inode *parent, struct dentry *dentry);
static int ext2_rmdir(struct inode *parent, struct dentry *dentry);
static int ext2_mkfifo(struct inode *parent, struct dentry *dentry);

static int ext2_file_read(struct file *file, void *__either buf, loff_t len);
static int ext2_file_write(struct file *file, void *__either buf, loff_t len);
static int ext2_file_iterate(struct file *file, void *__either buf, loff_t len);

// FIFO (named pipe) ring buffer attached lazily to a FIFO inode's `private`.
#define FIFO_BUFSIZE 512
struct ext2_fifo {
    spinlock_t lock;
    char       data[FIFO_BUFSIZE];
    uint32     nread;
    uint32     nwrite;
};

static int fifo_read(struct file *file, void *__either buf, loff_t len);
static int fifo_write(struct file *file, void *__either buf, loff_t len);

static struct file_operations ext2_fifo_ops = {
    .close = generic_file_close,
    .read  = fifo_read,
    .write = fifo_write,
};

// -------- op tables --------

static struct sb_operations ext2_sb_ops = {
    .free_inode   = ext2_free_inode,
    .write_inode  = ext2_write_inode,
    .delete_inode = ext2_delete_inode,
};

static struct inode_operations ext2_inode_ops = {
    .lookup = ext2_lookup,
    .create = ext2_create,
    .unlink = ext2_unlink,
    .mkdir  = ext2_mkdir,
    .rmdir  = ext2_rmdir,
    .mkfifo = ext2_mkfifo,
};

static struct file_operations ext2_regfile_ops = {
    .close = generic_file_close,
    .read  = ext2_file_read,
    .write = ext2_file_write,
};

static struct file_operations ext2_dirfile_ops = {
    .close   = generic_file_close,
    .iterate = ext2_file_iterate,
};

// -------- entry point (overrides the weak fs_mount_root) --------

void ext2_vfs_init(void) {
    infof("ext2_vfs_init: ext2 vfs init");

    ext2_init();
    ext2_journal_init();

    ext2_sb_vfs.name    = "ext2";
    ext2_sb_vfs.ops     = &ext2_sb_ops;
    ext2_sb_vfs.private = &ext2_info;
    ext2_sb_vfs.list    = NULL;
    spinlock_init(&ext2_sb_vfs.lock, "ext2 sb list lock");

    struct inode *root = ext2_iget(&ext2_sb_vfs, EXT2_ROOT_INO);
    iunlock(root);
    ext2_sb_vfs.root = root;

    assert(rootfs == NULL);
    rootfs = &ext2_sb_vfs;
}

void fs_mount_root(void) {
    ext2_vfs_init();
}

// -------- VFS inode load / persist --------

static struct inode *ext2_iget(struct superblock *sb, uint32 ino) {
    if (ino == 0 || ino > ext2_info.sb.s_inodes_count)
        return NULL;

    struct inode *ind = iget_locked(sb, ino);

    struct ext2_inode din;
    ext2_iload(ino, &din);

    ind->imode = ext2_mode_to_imode(din.i_mode);
    if (ind->imode & IMODE_DIR) {
        ind->fops = &ext2_dirfile_ops;
    } else if (ind->imode & IMODE_REG) {
        ind->fops = &ext2_regfile_ops;
    } else if (ind->imode & IMODE_FIFO) {
        ind->fops = &ext2_fifo_ops;
    } else {
        panic("ext2_iget: unknown on-disk inode mode");
    }
    ind->iops    = &ext2_inode_ops;
    // Preserve `private` across re-iget calls: for FIFO inodes it holds the
    // shared struct ext2_fifo ring buffer, which must stay attached for as
    // long as the inode is cached.
    ind->size    = din.i_size;
    ind->nlinks  = din.i_links_count;
    return ind;
}

static void ext2_write_inode(struct inode *ind) {
    assert(holdingsleep(&ind->lock));
    struct ext2_inode din;
    ext2_iload(ind->ino, &din);
    din.i_size        = ind->size;
    din.i_links_count = ind->nlinks;
    ext2_iwrite_raw(ind->ino, &din);
}

static void ext2_free_inode(struct inode *inode) {
    // FIFO inodes own a struct ext2_fifo page in `private`.
    if ((inode->imode & IMODE_FIFO) && inode->private != NULL) {
        kfreepage((void *)KVA_TO_PA(inode->private));
        inode->private = NULL;
    }
}

static void ext2_delete_inode(struct inode *inode) {
    assert(inode->nlinks == 0);

    // Free all data + indirect blocks.
    ext2_itrunc(inode);

    // Mark inode dead on disk.
    struct ext2_inode din;
    ext2_iload(inode->ino, &din);
    din.i_links_count = 0;
    din.i_dtime       = 1;  // any non-zero value
    din.i_mode        = 0;
    ext2_iwrite_raw(inode->ino, &din);

    // Return the inode number to the free pool.
    ext2_ifree(inode->ino);
}

// -------- directory entry helpers --------

// Length of a directory entry record needed for a given name length, padded
// to a 4-byte boundary.
static inline uint32 de_reclen(uint32 name_len) {
    return 8 + ((name_len + 3) & ~3u);
}

// Iterate over the directory and call cb for each entry. cb returns 1 to
// stop the walk early. Returns -ENOENT if cb never claimed an entry.
//
// (This isn't used directly — it's documentation in code form for the three
// loops below. The duplication keeps the inner state simple.)

// Find `name` in `parent`. Returns the inode number on success, -ENOENT
// otherwise.
static int ext2_dir_find(struct inode *parent, const char *name, uint32 *out_ino) {
    uint32 name_len = strlen(name);
    uint32 size     = parent->size;
    uint32 pos      = 0;

    while (pos < size) {
        uint32 blkno;
        int ret = ext2_iaddr(parent, pos, &blkno);
        if (ret < 0)
            return ret;

        struct buf *bp = bread(0, blkno);
        uint32 boff    = 0;
        while (boff < ext2_info.block_size) {
            struct ext2_dir_entry *de =
                (struct ext2_dir_entry *)(bp->data + boff);
            if (de->rec_len == 0) {
                brelse(bp);
                return -ENOENT;
            }
            if (de->inode != 0 && de->name_len == name_len
                && memcmp(de->name, name, name_len) == 0) {
                *out_ino = de->inode;
                brelse(bp);
                return 0;
            }
            boff += de->rec_len;
        }
        brelse(bp);
        pos += ext2_info.block_size;
    }
    return -ENOENT;
}

// Add `name -> ino` to `parent`. Splits a slack entry, or extends the
// directory by a new block.
static int ext2_dir_add(struct inode *parent, const char *name, uint32 ino,
                        uint8 file_type) {
    uint32 name_len = strlen(name);
    if (name_len == 0 || name_len > 255)
        return -EINVAL;
    uint32 needed = de_reclen(name_len);

    uint32 size = parent->size;
    uint32 pos  = 0;

    while (pos < size) {
        uint32 blkno;
        int ret = ext2_iaddr(parent, pos, &blkno);
        if (ret < 0)
            return ret;

        struct buf *bp = bread(0, blkno);
        uint32 boff    = 0;
        while (boff < ext2_info.block_size) {
            struct ext2_dir_entry *de =
                (struct ext2_dir_entry *)(bp->data + boff);
            if (de->rec_len == 0)
                break;

            uint32 actual = (de->inode == 0) ? 0 : de_reclen(de->name_len);
            uint32 slack  = de->rec_len - actual;

            // Reuse: this entry slot is free (inode == 0) and has enough room.
            if (de->inode == 0 && de->rec_len >= needed) {
                de->inode     = ino;
                de->name_len  = name_len;
                de->file_type = file_type;
                memmove(de->name, name, name_len);
                ext2_journal_write(bp);
                brelse(bp);
                return 0;
            }

            // Split: there's enough slack at the tail of this record.
            if (slack >= needed) {
                uint32 old_rec_len = de->rec_len;
                de->rec_len        = actual;

                struct ext2_dir_entry *nde =
                    (struct ext2_dir_entry *)(bp->data + boff + actual);
                nde->inode     = ino;
                nde->rec_len   = old_rec_len - actual;
                nde->name_len  = name_len;
                nde->file_type = file_type;
                memmove(nde->name, name, name_len);

                ext2_journal_write(bp);
                brelse(bp);
                return 0;
            }

            boff += de->rec_len;
        }
        brelse(bp);
        pos += ext2_info.block_size;
    }

    // No slack anywhere; extend the directory by one fresh block.
    assert(parent->size % ext2_info.block_size == 0);
    uint32 new_pos = parent->size;

    uint32 blkno;
    int ret = ext2_iaddr(parent, new_pos, &blkno);
    if (ret < 0)
        return ret;

    struct buf *bp = bread(0, blkno);
    memset(bp->data, 0, ext2_info.block_size);
    struct ext2_dir_entry *de = (struct ext2_dir_entry *)bp->data;
    de->inode     = ino;
    de->rec_len   = ext2_info.block_size;
    de->name_len  = name_len;
    de->file_type = file_type;
    memmove(de->name, name, name_len);
    ext2_journal_write(bp);
    brelse(bp);

    parent->size += ext2_info.block_size;

    struct ext2_inode pdin;
    ext2_iload(parent->ino, &pdin);
    pdin.i_size = parent->size;
    ext2_iwrite_raw(parent->ino, &pdin);
    return 0;
}

// Remove `name` from `parent`. Coalesces with the previous record when
// possible (otherwise zero out the inode field so the slot becomes reusable).
static int ext2_dir_remove(struct inode *parent, const char *name,
                           uint32 *out_ino) {
    uint32 name_len = strlen(name);
    uint32 size     = parent->size;
    uint32 pos      = 0;

    while (pos < size) {
        uint32 blkno;
        int ret = ext2_iaddr(parent, pos, &blkno);
        if (ret < 0)
            return ret;

        struct buf *bp = bread(0, blkno);
        uint32 boff    = 0;
        struct ext2_dir_entry *prev = NULL;
        while (boff < ext2_info.block_size) {
            struct ext2_dir_entry *de =
                (struct ext2_dir_entry *)(bp->data + boff);
            if (de->rec_len == 0)
                break;

            if (de->inode != 0 && de->name_len == name_len
                && memcmp(de->name, name, name_len) == 0) {
                *out_ino = de->inode;
                if (prev != NULL) {
                    prev->rec_len += de->rec_len;
                } else {
                    de->inode = 0;
                }
                ext2_journal_write(bp);
                brelse(bp);
                return 0;
            }
            prev = de;
            boff += de->rec_len;
        }
        brelse(bp);
        pos += ext2_info.block_size;
    }
    return -ENOENT;
}

// Returns 1 if the directory has no entries beyond "." and "..".
static int ext2_dir_is_empty(struct inode *dir) {
    uint32 size = dir->size;
    uint32 pos  = 0;

    while (pos < size) {
        uint32 blkno;
        int ret = ext2_iaddr(dir, pos, &blkno);
        if (ret < 0)
            return 0;

        struct buf *bp = bread(0, blkno);
        uint32 boff    = 0;
        while (boff < ext2_info.block_size) {
            struct ext2_dir_entry *de =
                (struct ext2_dir_entry *)(bp->data + boff);
            if (de->rec_len == 0)
                break;
            if (de->inode != 0) {
                int is_dot   = (de->name_len == 1 && de->name[0] == '.');
                int is_dotdot =
                    (de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.');
                if (!is_dot && !is_dotdot) {
                    brelse(bp);
                    return 0;
                }
            }
            boff += de->rec_len;
        }
        brelse(bp);
        pos += ext2_info.block_size;
    }
    return 1;
}

// -------- inode_operations --------

static int ext2_lookup(struct inode *parent, struct dentry *dentry) {
    uint32 ino;
    int ret = ext2_dir_find(parent, dentry->name, &ino);
    if (ret < 0)
        return ret;
    dentry->ind = ext2_iget(parent->sb, ino);
    if (dentry->ind == NULL)
        return -ENOENT;
    return 0;
}

static int ext2_create_common(struct inode *parent, struct dentry *dentry,
                              uint16 mode, uint8 file_type) {
    uint32 ino = ext2_ialloc(mode);
    if (ino == 0)
        return -ENOSPC;

    int ret = ext2_dir_add(parent, dentry->name, ino, file_type);
    if (ret < 0) {
        ext2_ifree(ino);
        return ret;
    }
    dentry->ind = ext2_iget(parent->sb, ino);
    if (dentry->ind == NULL)
        return -ENOENT;
    return 0;
}

static int ext2_create(struct inode *parent, struct dentry *dentry) {
    ext2_journal_begin();
    int ret = ext2_create_common(parent, dentry, EXT2_S_IFREG | 0644,
                                 EXT2_FT_REG_FILE);
    ext2_journal_commit();
    return ret;
}

static int ext2_mkfifo(struct inode *parent, struct dentry *dentry) {
    ext2_journal_begin();
    int ret = ext2_create_common(parent, dentry, EXT2_S_IFIFO | 0644,
                                 EXT2_FT_FIFO);
    ext2_journal_commit();
    return ret;
}

static int ext2_mkdir_inner(struct inode *parent, struct dentry *dentry) {
    uint32 ino = ext2_ialloc(EXT2_S_IFDIR | 0755);
    if (ino == 0)
        return -ENOSPC;

    int ret = ext2_dir_add(parent, dentry->name, ino, EXT2_FT_DIR);
    if (ret < 0) {
        ext2_ifree(ino);
        return ret;
    }

    struct inode *new_dir = ext2_iget(parent->sb, ino);
    if (new_dir == NULL) {
        ext2_ifree(ino);
        return -ENOENT;
    }

    // Initialize the new directory's first data block with "." and "..".
    uint32 blkno;
    ret = ext2_iaddr(new_dir, 0, &blkno);
    if (ret < 0) {
        iunlockput(new_dir);
        return ret;
    }

    struct buf *bp = bread(0, blkno);
    memset(bp->data, 0, ext2_info.block_size);

    struct ext2_dir_entry *dot = (struct ext2_dir_entry *)bp->data;
    dot->inode     = ino;
    dot->rec_len   = 12;
    dot->name_len  = 1;
    dot->file_type = EXT2_FT_DIR;
    dot->name[0]   = '.';

    struct ext2_dir_entry *dotdot =
        (struct ext2_dir_entry *)(bp->data + 12);
    dotdot->inode     = parent->ino;
    dotdot->rec_len   = ext2_info.block_size - 12;
    dotdot->name_len  = 2;
    dotdot->file_type = EXT2_FT_DIR;
    dotdot->name[0]   = '.';
    dotdot->name[1]   = '.';

    ext2_journal_write(bp);
    brelse(bp);

    new_dir->size   = ext2_info.block_size;
    new_dir->nlinks = 2;  // one from `.`, one from parent's entry

    struct ext2_inode din;
    ext2_iload(ino, &din);
    din.i_size        = ext2_info.block_size;
    din.i_links_count = 2;
    din.i_blocks      = ext2_info.block_size / 512;
    ext2_iwrite_raw(ino, &din);

    iunlockput(new_dir);

    // Parent gains a link from the new directory's `..` entry.
    parent->nlinks++;
    struct ext2_inode pdin;
    ext2_iload(parent->ino, &pdin);
    pdin.i_links_count = parent->nlinks;
    ext2_iwrite_raw(parent->ino, &pdin);
    return 0;
}

static int ext2_mkdir(struct inode *parent, struct dentry *dentry) {
    ext2_journal_begin();
    int ret = ext2_mkdir_inner(parent, dentry);
    ext2_journal_commit();
    return ret;
}

static int ext2_unlink_inner(struct inode *parent, struct dentry *dentry) {
    // Note: by the time we get here, VFS has already iunlockput(dentry->ind),
    // so that pointer is dangling. Use the name to remove the parent entry
    // and ext2_iget to re-acquire a clean handle for the child.
    uint32 ino;
    int ret = ext2_dir_remove(parent, dentry->name, &ino);
    if (ret < 0)
        return ret;

    struct inode *child = ext2_iget(parent->sb, ino);
    if (child == NULL)
        return -ENOENT;
    child->nlinks--;
    imarkdirty(child);
    iunlockput(child);
    return 0;
}

static int ext2_unlink(struct inode *parent, struct dentry *dentry) {
    ext2_journal_begin();
    int ret = ext2_unlink_inner(parent, dentry);
    ext2_journal_commit();
    return ret;
}

static int ext2_rmdir_inner(struct inode *parent, struct dentry *dentry) {
    // dentry->ind is ref-held but unlocked here. Re-acquire via ext2_iget so
    // we can safely walk the directory contents and update fields.
    struct inode *child = ext2_iget(parent->sb, dentry->ind->ino);
    if (child == NULL)
        return -ENOENT;

    if (!(child->imode & IMODE_DIR)) {
        iunlockput(child);
        return -EINVAL;
    }
    if (!ext2_dir_is_empty(child)) {
        iunlockput(child);
        return -ENOTEMPTY;
    }

    uint32 ino;
    int ret = ext2_dir_remove(parent, dentry->name, &ino);
    if (ret < 0) {
        iunlockput(child);
        return ret;
    }

    // Drop the directory's links to zero: the parent's entry and the
    // implicit self `.` link both go away at once.
    child->nlinks = 0;
    imarkdirty(child);
    iunlockput(child);

    // Parent loses the link contributed by our `..` entry.
    parent->nlinks--;
    imarkdirty(parent);
    return 0;
}

static int ext2_rmdir(struct inode *parent, struct dentry *dentry) {
    ext2_journal_begin();
    int ret = ext2_rmdir_inner(parent, dentry);
    ext2_journal_commit();
    return ret;
}

// -------- file_operations --------

static int ext2_file_read(struct file *file, void *__either buf, loff_t len) {
    struct inode *ind = file_inode(file);
    assert(ind);
    if (!(ind->imode & IMODE_REG))
        return -EINVAL;

    ilock(ind);
    int ret = ext2_iread(ind, file->pos, buf, len);
    iunlock(ind);
    if (ret < 0)
        return ret;
    file->pos += ret;
    return ret;
}

static int ext2_file_write(struct file *file, void *__either buf, loff_t len) {
    struct inode *ind = file_inode(file);
    assert(ind);
    if (!(ind->imode & IMODE_REG))
        return -EINVAL;

    ilock(ind);
    ext2_journal_begin();
    int ret = ext2_iwrite(ind, file->pos, buf, len);
    ext2_journal_commit();
    iunlock(ind);
    if (ret < 0)
        return ret;
    file->pos += ret;
    return ret;
}

// Convert ext2 variable-length directory entries into the fixed-size struct
// dirent that the VFS hands to user space.
static int ext2_file_iterate(struct file *file, void *__either buf, loff_t len) {
    struct inode *ind = file_inode(file);
    assert(ind);
    if (!(ind->imode & IMODE_DIR))
        return -EINVAL;

    ilock(ind);

    int filled = 0;
    while ((loff_t)(filled + sizeof(struct dirent)) <= len) {
        if (file->pos >= ind->size)
            break;

        uint32 blkno;
        int ret = ext2_iaddr(ind, file->pos, &blkno);
        if (ret < 0) {
            iunlock(ind);
            return ret;
        }

        struct buf *bp = bread(0, blkno);
        uint32 boff    = file->pos % ext2_info.block_size;
        struct ext2_dir_entry *de =
            (struct ext2_dir_entry *)(bp->data + boff);

        if (de->rec_len == 0) {
            brelse(bp);
            break;
        }

        if (de->inode != 0) {
            struct dirent out;
            memset(&out, 0, sizeof(out));
            out.ino     = de->inode;
            uint32 cpy  = MIN(de->name_len, (uint32)(DIRENT_NAME_MAX - 1));
            memmove(out.name, de->name, cpy);
            out.name[cpy] = '\0';
            vfs_either_copy_out((uint8 *)buf + filled, &out, sizeof(out));
            filled += sizeof(struct dirent);
        }

        file->pos += de->rec_len;
        brelse(bp);
    }

    iunlock(ind);
    return filled;
}

// -------- FIFO ops --------

// Lazily allocate (and cache on the inode) the shared ring buffer.
// Caller must hold the inode's sleeplock.
static struct ext2_fifo *fifo_attach(struct inode *ind) {
    if (ind->private != NULL)
        return (struct ext2_fifo *)ind->private;

    void *pa = kallocpage();
    if (pa == NULL)
        return NULL;

    struct ext2_fifo *fi = (struct ext2_fifo *)PA_TO_KVA(pa);
    memset(fi, 0, sizeof(*fi));
    spinlock_init(&fi->lock, "ext2 fifo");
    ind->private = fi;
    return fi;
}

static int fifo_read(struct file *file, void *__either buf, loff_t len) {
    struct inode *ind = file_inode(file);
    if (!(ind->imode & IMODE_FIFO))
        return -EINVAL;

    ilock(ind);
    struct ext2_fifo *fi = fifo_attach(ind);
    iunlock(ind);
    if (fi == NULL)
        return -ENOSPC;

    struct proc *pr = curr_proc();
    int i;

    acquire(&fi->lock);
    while (fi->nread == fi->nwrite) {
        if (iskilled(pr)) {
            release(&fi->lock);
            return -1;
        }
        sleep(&fi->nread, &fi->lock);
    }
    for (i = 0; i < len; i++) {
        if (fi->nread == fi->nwrite)
            break;
        char ch = fi->data[fi->nread++ % FIFO_BUFSIZE];
        if (vfs_either_copy_out((uint8 *)buf + i, &ch, 1) < 0)
            break;
    }
    wakeup(&fi->nwrite);
    release(&fi->lock);
    return i;
}

static int fifo_write(struct file *file, void *__either buf, loff_t len) {
    struct inode *ind = file_inode(file);
    if (!(ind->imode & IMODE_FIFO))
        return -EINVAL;

    ilock(ind);
    struct ext2_fifo *fi = fifo_attach(ind);
    iunlock(ind);
    if (fi == NULL)
        return -ENOSPC;

    struct proc *pr = curr_proc();
    int i = 0;

    acquire(&fi->lock);
    while (i < len) {
        if (iskilled(pr)) {
            release(&fi->lock);
            return -1;
        }
        if (fi->nwrite == fi->nread + FIFO_BUFSIZE) {
            wakeup(&fi->nread);
            sleep(&fi->nwrite, &fi->lock);
        } else {
            char ch;
            if (vfs_either_copy_in((uint8 *)buf + i, &ch, 1) < 0)
                break;
            fi->data[fi->nwrite++ % FIFO_BUFSIZE] = ch;
            i++;
        }
    }
    wakeup(&fi->nread);
    release(&fi->lock);
    return i;
}
