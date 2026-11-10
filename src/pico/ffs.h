// An Amiga Fast File System volume (DOS\1) written from scratch (ffs.c): a bare
// partition image, as ide.c mounts one (its virtual RDB in front).  Plain C, no
// allocation: the caller gives an arena for the tree and a write callback.
//
//   ffs_begin -> ffs_add ... (every file and directory with its size)
//   -> ffs_layout (the image's size) -> ffs_write_meta (boot, root, bitmap,
//   directories, file headers) -> per file ffs_file_begin / ffs_file_data ... /
//   ffs_file_end, its data in a contiguous run of blocks.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Write `len` bytes at byte `off` of the image; 0 = done.
typedef int (*ffs_write_fn)(void *ctx, uint32_t off, const void *src, uint32_t len);

typedef struct ffs ffs_t;

#define FFS_NAME_MAX 30      // AmigaDOS's limit; longer names are cut

// The tree lives in `arena` (64 bytes a node plus the comments).
ffs_t *ffs_begin(void *arena, uint32_t size, const char *volume, uint32_t unix_time);
// A file (dir false) or a directory at `path` ('/'-separated, from the root);
// its parent directories are made as needed.  Returns its id, or -1: out of
// arena, -2: the path is already there (the first one wins; for a directory
// that is no error: its id is returned).
int ffs_add(ffs_t *f, const char *path, bool dir, uint32_t size, uint8_t protect,
            const char *comment, uint32_t unix_time);
// The node at `path` (case-insensitive, as AmigaDOS), or -1.
int ffs_find(ffs_t *f, const char *path);
uint32_t ffs_file_size(ffs_t *f, int id);
int ffs_count(ffs_t *f);

// Place every block, with at least `free_blocks` left free; the image's size in
// 512-byte blocks (a multiple of 32: ide.c's virtual cylinders), 0 = too large
// (more than ~49 MB: the root's 25 bitmap blocks).
uint32_t ffs_layout(ffs_t *f, uint32_t free_blocks);
// Blocks 0-1 (DOS\1), the root, the bitmap, every directory and file header.
int ffs_write_meta(ffs_t *f, ffs_write_fn wr, void *ctx);
// A file's data, in order; ffs_file_end pads the last block and fails when
// fewer bytes came than ffs_add said.  `buf`: a staging buffer (a multiple of
// 512 bytes, 4 KB or more for fewer, larger writes).
int ffs_file_begin(ffs_t *f, int id, ffs_write_fn wr, void *ctx, uint8_t *buf, uint32_t buf_size);
int ffs_file_data(ffs_t *f, const void *src, uint32_t len);
int ffs_file_end(ffs_t *f);

#ifdef __cplusplus
}
#endif
