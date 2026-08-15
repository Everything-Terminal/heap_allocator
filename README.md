# 🧱 heap_allocator

A custom `malloc()` / `free()` implementation from scratch in C, built K&R
chapter 8 style, a free list threaded directly through the heap itself,
with no separate bookkeeping structure off to the side.

No `<stdlib.h>` allocator used anywhere, just raw `sbrk()` calls and a
circular free list.

## ✨ Features

- `my_malloc()` / `my_free()`, first-fit allocation over a circular free list
- Automatic coalescing of adjacent free blocks on `free()`
- `my_calloc()`, overflow-safe size multiplication, zeroed memory
- `my_realloc()`, allocate-copy-free, with old-value preservation on grow
- Built-in stats tracking (alloc/free calls, bytes in use, `sbrk` calls)
- ASCII free-list visualizer, watch fragmentation and coalescing happen live
- A deliberate "checkerboard" stress test: allocate 8 blocks, free every
  other one, then free the rest and watch it collapse back into a single
  chunk

## 🚀 Build & Run

```bash
gcc -std=c11 -Wall -Wextra -o heap_allocator heap_allocator.c
./heap_allocator
```

## 📝 Design Notes

The header (`Header`) sits in front of every block, allocated or free, and
doubles as a free-list node when the block is free. It's a `union` with
`max_align_t` so whatever the caller stuffs after it is safely aligned.

`morecore()` is the only function that ever calls `sbrk`. When the free
list runs dry, it asks the OS for a batch of `NALLOC` units at once rather
than dribbling out one `sbrk` call per allocation, then hands the new slab
to the free-list splicing logic like any other block.

`my_free()` finds where the freed block belongs in the circle by address,
splices it back in, and merges with whichever neighbor(s) sit right against
it in memory. That merge step is the entire fragmentation fix, without it
the heap would just get chewed into smaller and smaller crumbs over time.

## 📚 Reference

Built following the classic allocator design from Kernighan & Ritchie,
_The C Programming Language_

## 🔗 Related

- [marbles_game](https://github.com/Everything-Terminal/marbles_game), a
  physics-based marble pinball game in C and SDL2
