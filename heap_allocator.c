/* Alright, next project. Last time it was marbles bouncing around
 * a window. This time there's no window at all, no SDL, nothing
 * on the screen. This one lives entirely in memory, literally.
 *
 * The goal: build my own malloc and free from scratch. The
 * functions every C program leans on without a second thought,
 * and that I've been leaning on too, in Tetris, in the marbles
 * game, everywhere. Time to see what's actually happening
 * underneath them.
 *
 * I'm basing the core design on the classic allocator from K&R
 * chapter 8, sunce that's exactly where I am in the book right now.
 * It's a free list threaded through the heap itself, no seperate
 * bookkeeping structure off to the side. Small, a bit clever, and it's
 * the direct ancestor of every allocator I'll meet later when I get
 * into kernel memory management.
 *
 * On top of that base I added calloc, realloc, some stats
 * tracking, and a little text visualizer so I can actually watch the free
 * list fragment and then coalesce back together instead
 * of just trusting that it works. 
 *
 * sbrk is old enough that glibc hides it behind a feature test
 * macro unless you ask nicely. This has to come before my includes,
 * or it's too late. */
#define _DEFAULT_SOURCE

/* What this needs. unistd for sbrk, the thing that actually
 * asks the OS for more heap. stddef guves us max_align_t for
 * the header's alignment trick. Everything else is standard. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>
#include <assert.h>

/* One knob to turn: how many "units" to request from the OS at
 * once when the free list runs dry. Asking for a small dribble
 * every time would mean constant sbrk calls, so we ask for a
 * chunk and bank the rest for next time. */
#define NALLOC 1024 /* units requested per sbrk call when we need more */

/* The header sits in front of every block, allocated or free.
 * And doubles as a free-list node when the block is free. The
 * union trick forces this to be aligned to whatever the
 * strictest type needs, so whatever the caller stuffs after it
 * is safely aligned too. */
typedef union header {
  struct {
    union header *ptr;
    size_t size;
  } s;
  max_align_t align;
} Header;

/* base is a permanet zero-size block that seeds the circular
 * list before anything has been freed yet. freep always points
 * somewhere inside that circle, it's where the next search
 * starts from. */
static Header base;
static Header *freep = NULL;

/* A few counters purely so the demo at the bottom has something
 * honest to report instead of me just asserting it works. */
static size_t stat_alloc_calls = 0;
static size_t stat_free_calls = 0;
static size_t stat_bytes_in_use = 0;
static size_t stat_morecore_calls = 0;

/* That's the whole shape of it: a curcular linked list of free
 * chunks, woven directly through the same momory that gets
 * handed out. No separate array or table tracking what's free.
 * The heap tracks itself. */

/* my_free is declared above morecore but defined below, C needs
 * the prototype since morecore calls it before it's seen in the
 * file. Small chicken-and-egg, easy fix. */
void my_free(void *ap);

/* This is the only function that evr actually asks the OS for
 * memory. EVerything else just carves up what this hands over. */
static Header *morecore(size_t nu) {
  char *cp;
  Header *up;

  /* Ask for at least NALLOC units, even if the caller needs
   * less. sbrk itself is cheap-ish but not free, and one call
   * for a big slab beats a hundred calls for tiny ones. */
  if (nu < NALLOC) nu = NALLOC;

  cp = sbrk((intptr_t)(nu * sizeof(Header)));
  if (cp == (char *)-1) return NULL; /* OS said no, out of memory */

  stat_morecore_calls++;

  up = (Header *)cp;
  up->s.size = nu;

  /* Hand the new slab to free() rather than duplicating its
   * insert-and-coalesce logic here. It'll get threaded into the
   * free list like any other block. */
  my_free((void *)(up + 1));
  return freep;
}

/* The allocator itself. Walk the circular free lest looking
 * for a block big enough. First one that fits wins, take it,
 * or if it's bigger than needed, slice off the front and leave
 * the remainder in the list for next time. */
void *my_malloc(size_t nbytes) {
  Header *p, *prevp;
  size_t nunits;

  if (nbytes == 0) return NULL;

  /* Round up to whole header-sized units, plus one unit for
   * our own header. This is the size in Header units, not bytes. */
  nunits = (nbytes + sizeof(Header) - 1) / sizeof(Header) + 1;

  if ((prevp = freep) == NULL) {
    /* First call ever, the list doesn't exist yet. Make
     * base point to itself, a circle of one, size zero. */
    base.s.ptr = freep = prevp = &base;
    base.s.size = 0;
  }

  for (p = prevp->s.ptr; ; prevp = p, p = p->s.ptr) {
    if (p->s.size >= nunits) {
      if(p->s.size == nunits) {
        /* Exact fit, cut this block out of the list entirely */
        prevp->s.ptr = p->s.ptr;
      } else {
        /* Bigger than needed, shrink it and carve the tail off
         * for the caller. The part still in the list keeps its spot,
         * only its recorded size shrinks. */
        p->s.size -=nunits;
        p += p->s.size;
        p->s.size = nunits;
      }
      freep = prevp;
      stat_alloc_calls++;
      stat_bytes_in_use += nunits * sizeof(Header);
      return (void *)(p + 1);
    }
    if (p == freep) {
      /* We've gone all the way around the circle with nothing
       * big enough. Time to ask the OS for more. */
      if ((p = morecore(nunits)) == NULL)
        return NULL;
    }
  }
}

/* Freeing a block means finding where it belongs in the circle
 * by address, splicing it back in, and merging with whichever
 * neighbor(s) happen to sit right against it in memory. That
 * mergy step is the entire fragmentation fix, without it the
 * heap would just get chewed into smaller and smaller crumbs
 * over time. */
void my_free(void *ap) {
  Header *bp, *p;

  if (ap == NULL) return;

  bp = (Header *)ap - 1;

  /* Find the free block just before where bp belongs, address
   * wise. The loop handles the wraparound case where bp sits
   * before the lowest free block or after the highest one. */
  for (p = freep; !(bp > p && bp < p->s.ptr); p = p->s.ptr)
    if (p >= p->s.ptr && (bp > p || bp < p->s.ptr))
      break;

  if (bp + bp->s.size == p->s.ptr) {
    /* bp butts right up against the block after it, merge them
     * into one bigger block instead of leaving two adjacent
     * free chunks sitting there separately. */
    bp->s.size += p->s.ptr->s.size;
    bp->s.ptr = p->s.ptr->s.ptr;
  } else {
    bp->s.ptr = p->s.ptr;
  }

  if (p + p->s.size == bp) {
    /* Same idea, the other direction: the block before bp
     * butts up against it. absorb bp into that one. */
    p->s.size += bp->s.size;
    p->s.ptr = bp->s.ptr;
  } else {
    p->s.ptr = bp;
  }

  freep = p;
  stat_free_calls++;
  stat_bytes_in_use -= bp->s.size * sizeof(Header);
}

/* malloc and free are the two load-bearing pieces. calloc and
 * realloc are just convenieces built on top, no new ideas. */

/* Same as malloc but zeroed, mainly useful so a fresh struct
 * doesn't come back full of whatever garbage sbrk last handed
 * to some other program. */
void *my_calloc(size_t nmemb, size_t size) {
  size_t total = nmemb * size;
  if (nmemb != 0 && total / nmemb != size) return NULL;
  void *p = my_malloc(total);
  if (p) memset(p, 0, total);
  return p;
}

/* A deliberately simple realloc: allocate fresh, copy the
 * smaller of the old and new sizes across, free the old block.
 * A "real" realloc would try to grow in place first if the
 * block behind it happens to be free, but that's a good next
 * upgrade, not a day-one requirement. */
void *my_realloc(void *ap, size_t nbytes) {
  if (ap == NULL) return my_malloc(nbytes);
  if (nbytes == 0) { my_free(ap); return NULL; }

  Header *bp = (Header *)ap - 1;
  size_t old_bytes = (bp->s.size - 1) * sizeof(Header);

  void *newp = my_malloc(nbytes);
  if (newp == NULL) return NULL;

  size_t copy_bytes = old_bytes < nbytes ? old_bytes : nbytes;
  memcpy(newp, ap, copy_bytes);
  my_free(ap);
  return newp;
}

/* This next bit is my versionof the marble game's digit
 * renderer, a small side, tool that isn't the point of the
 * project but akes it possible to actually see what's going
 * on. Instead of segments and rectangles, this walks the free
 * list and prints each chunk's size as a bar of asterisks, so
 * fragmentation is something I can watch happen instead of
 * something I have to take on faith. */
static void print_free_list(const char *label) {
  printf("\n-- %s --\n", label);
  if (freep == NULL) {
    printf(" (nothing freed yet)\n");
    return;
  }

  Header *p = freep->s.ptr;
  size_t chunk = 0;
  size_t total_free_bytes = 0;

  do {
    if (p->s.size > 0) {
      size_t bytes = p->s.size * sizeof(Header);
      total_free_bytes += bytes;
      printf(" chunk %-2zu %6zu bytes ", ++chunk, bytes);
      size_t stars = bytes / 32 + 1;
      if (stars > 40) stars = 40;
      for (size_t i = 0; i < stars; i++) putchar('*');
      putchar('\n');
    }
    p = p->s.ptr;
  } while (p != freep->s.ptr && chunk < 200);

  printf(" %zu free chunk(s), %zu bytes free total\n", chunk, total_free_bytes);
}

static void print_stats(void) {
  printf("\n-- allocator stats --\n");
  printf(" my_malloc calls   : %zu\n", stat_alloc_calls);
  printf(" my_free calls     : %zu\n", stat_free_calls);
  printf(" bytes in use      : %zu\n", stat_bytes_in_use);
  printf(" sbrk (morecore)   : %zu call(s)\n", stat_morecore_calls);
}

/* And now the actual point: prove it works, and more
 * importantly, prove the coalescing works, by deliberately
 * making a mess and then watching free() clean it up. */
int main(void) {
  printf("Building my own malloc/free, K&R style.\n");

  /* Step 1: allocate a handful of differently sized blocks, the
   * way a real program would, some small structs, a couple of
   * bigger buffers. */
  enum { N = 8 };
  void *blocks[N];
  size_t sizes[N] = { 24, 128, 16, 512, 64, 8, 256, 40 };

  for (int i = 0; i < N; i++) {
    blocks[i] = my_malloc(sizes[i]);
    printf("allocated block %d: %zu bytes at %p\n", i, sizes[i], blocks[i]);
    assert(blocks[i] != NULL);
    memset(blocks[i], 0xAB, sizes[i]);
  }

  /* Step 2: free every other block. This is the fragmentation
   * move, it leaves a checkerboard of free and allocated chunks
   * instead of one clean region, which is exactly the situation
   * a naive allocator handles badly. */
  for (int i = 0; i < N; i += 2) {
    my_free(blocks[i]);
    blocks[i] = NULL;
  }
  print_free_list("free list after checkerboard freeing (fragmented)");

  /* Step 3: now free the rest. Because each freed block sits
   * right next to an already free neighbor in memory, my_free's
   * merge logic should collapse this checkerboard back down
   * into far fewer, bigger chunks. */
  for (int i = 1; i < N; i += 2) {
    my_free(blocks[i]);
    blocks[i] = NULL;
  }
  print_free_list("free list after freeing the rest (coalesced)");

  /* Step 4: calloc, and prove it's actually zeroed, not just
   * conveniently already zero because it's fresh from the OS. */
  int *nums = my_calloc(10, sizeof(int));
  assert(nums != NULL);
  int all_zero = 1;
  for (int i = 0; i < 10; i++) if (nums[i] != 0) all_zero = 0;
  printf("\ncalloc'd 10 ints, all zero: %s\n", all_zero ? "yes" : "no");

  /* Step 5: realloc, grow it and check the original values
   * survived the move. */
  for (int i = 0; i < 10; i++) nums[i] = i * i;
  int *bigger = my_realloc(nums, 20 * sizeof(int));
  assert(bigger != NULL);
  int preserved = 1;
  for (int i = 0; i < 10; i++) if (bigger[i] != i * i) preserved = 0;
  printf("realloc grew 10 ints to 20, old values preserved: %s\n", preserved ? "yes" : "no");
  my_free(bigger);

  print_stats();

  printf("\nDone. Next step from here would be an explicit free list\n"
      "instead of scanning through allocated blocks too, or a\n"
      "best-fit search instead of first-fit. Both are on the list\n"
      "for when I get further into OSTEP.\n");

  return 0;
}
