[![Review Assignment Due Date](https://classroom.github.com/assets/deadline-readme-button-22041afd0340ce965d47ae6ef1cefeee28c7c493a6346c4f15d667ab976d596c.svg)](https://classroom.github.com/a/JGNFCPZR)
# SUSTechOS File System Project

This repository is based on SUSTechOS, a teaching operating system derived from [xv6-riscv](https://github.com/mit-pdos/xv6-riscv). The project focus is to implement an Ext2 file system, integrate it with the existing VFS and virtio-blk stack, and make it usable from user-space programs.

## What Is Included

The base system provides:

- S-mode boot through OpenSBI.
- QEMU single-core and SMP boot support.
- High-address kernel virtual memory.
- User process, syscall, VFS, pipe, simplefs, and virtio-blk infrastructure.
- User-space file-system score tests and benchmark programs.

The file-system project expects an Ext2 implementation to mount an Ext2 disk image and serve normal file-system operations through the VFS.

## Build

Build the user programs and kernel:

```sh
make build
```

Clean generated files:

```sh
make clean
```

## Run

Run with the default simplefs image:

```sh
make run
```

Run with an Ext2 image:

```sh
make run FS_TYPE=ext2
```

Run with SMP:

```sh
make runsmp
```

Run with a larger Ext2 image, useful for large-file tests:

```sh
make run FS_TYPE=ext2 FS_BLOCK_SIZE=1024 FS_BLOCKS=90000
```

`FS_TYPE=ext2` uses `mke2fs` and `debugfs`, so the host needs the usual
e2fsprogs tools installed.

## User-Space Tests

Build and boot the OS image. For Ext2 grading, pass `FS_TYPE=ext2` so the top-level `Makefile` creates an Ext2 `fs.img`:

```sh
make run FS_TYPE=ext2
```

After booting into `sh`, run:

```text
fs_score_basic
fs_score_dir
fs_score_path
fs_score_names
fs_score_errors
fs_score_dirents
fs_score_fd
fs_score_flags
fs_score_rw
fs_score_holes
fs_score_stress
fs_score_index
fs_score_index double
fs_score_index triple
fs_score_meta
fs_score_fifo
fs_bench_seq
fs_bench_rand
fs_bench_small
```

Reference success output:

```text
PASS fs_score_basic
PASS fs_score_dir
PASS fs_score_path
PASS fs_score_names
PASS fs_score_errors
PASS fs_score_dirents
PASS fs_score_fd
PASS fs_score_flags
PASS fs_score_rw
PASS fs_score_holes
PASS fs_score_stress
PASS fs_score_index single blocks=14
PASS fs_score_index double blocks=1280
PASS fs_score_index triple blocks=17920
PASS fs_score_meta
PASS fs_score_fifo
BENCH fs_bench_seq bytes=2097152 write_us=... read_us=...
BENCH fs_bench_rand blocks=256 ops=256 write_us=... read_us=...
BENCH fs_bench_small files=64 create_rw_us=... unlink_us=...
```

Any correctness failure prints `FAIL ...` with the source line and the syscall return value that caused the failure.

## Common Commands

```sh
make build
make run
make run FS_TYPE=ext2
make run FS_TYPE=ext2 FS_BLOCK_SIZE=1024 FS_BLOCKS=90000
make runsmp
make clean
make -C user clean
make -C user
```

## References

- SUSTechOS fs branch:
  <https://github.com/yuk1i/SUSTechOS/tree/fs>
- SUSTechOS docs:
  <https://yuk1i.github.io/os-next-docs/>
