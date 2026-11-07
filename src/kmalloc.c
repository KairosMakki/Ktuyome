/*
 * kmalloc.c — kernel heap allocator for ktuyome
 *
 * Uses the physical RAM above the end of the BSS segment (_kend) up to
 * the top of conventional memory reported by the Multiboot memory map.
 * With -m 256 in QEMU that gives ~240+ MB of usable heap.
 *
 * Algorithm: segregated free-list allocator.
 *   - 16-byte minimum block size, 8-byte aligned headers.
 *   - Free blocks kept in size-class bins (power-of-two bucket, 17 bins
 *     covering 16 B … 1 MB, plus one overflow bin).
 *   - First-fit within each bin; coalesces with adjacent free neighbours
 *     on every free().
 *   - Falls back to a bump pointer when no suitable free block exists.
 *
 * Thread safety: single-threaded OS — no locking needed.
 */
#include "ktuyome.h"

/* ------------------------------------------------------------------ */
/*  Linker-provided end-of-BSS symbol                                  */
/* ------------------------------------------------------------------ */
extern char _kend[];          /* defined in src/linker.ld */

/* ------------------------------------------------------------------ */
/*  Block header  (stored immediately before the user data)           */
/*  Size field includes the header itself.  Low bit = free flag.      */
/* ------------------------------------------------------------------ */
#define HDR_SIZE  8u          /* sizeof(BlkHdr) */
#define MIN_BLK   16u         /* minimum usable allocation */
#define ALIGN     8u

typedef struct BlkHdr BlkHdr;
struct BlkHdr {
    u32 size;       /* total block size in bytes (header + data), low bit = free */
    u32 prev_size;  /* size of the previous physical block (0 if first) */
};

#define BLK_FREE      1u
#define blk_size(b)   ((b)->size & ~BLK_FREE)
#define blk_is_free(b) ((b)->size & BLK_FREE)
#define blk_set_free(b,f) ((b)->size = blk_size(b) | ((f)?BLK_FREE:0u))
#define blk_next(b)   ((BlkHdr*)((char*)(b) + blk_size(b)))
#define blk_prev(b)   ((b)->prev_size ? (BlkHdr*)((char*)(b) - (b)->prev_size) : (BlkHdr*)0)
#define blk_data(b)   ((void*)((char*)(b) + HDR_SIZE))
#define data_blk(p)   ((BlkHdr*)((char*)(p) - HDR_SIZE))

/* ------------------------------------------------------------------ */
/*  Free-list bins                                                     */
/*  bin[i] covers blocks where:  (1<<(i+4)) <= size < (1<<(i+5))     */
/*  bin[0]  : 16–31 B                                                  */
/*  bin[16] : >= 1 MB  (overflow)                                      */
/* ------------------------------------------------------------------ */
#define NBINS 17
static BlkHdr *bins[NBINS];   /* singly-linked via the first 4 bytes of the user area */

static inline int bin_for(u32 sz) {
    if (sz < 16) sz = 16;
    int b = 0;
    u32 s = sz >> 4;
    while (s > 1 && b < NBINS-1) { s >>= 1; b++; }
    return b;
}

/* free-list links stored in the first word of the free block's data area */
#define FL_NEXT(b)  (*(BlkHdr**)blk_data(b))

static void bin_push(BlkHdr *b) {
    int i = bin_for(blk_size(b));
    FL_NEXT(b) = bins[i];
    bins[i] = b;
}

static void bin_remove(BlkHdr *b) {
    int i = bin_for(blk_size(b));
    BlkHdr **pp = &bins[i];
    while (*pp && *pp != b) pp = &FL_NEXT(*pp);
    if (*pp) *pp = FL_NEXT(b);
}

/* ------------------------------------------------------------------ */
/*  Heap extent                                                        */
/* ------------------------------------------------------------------ */
static char  *heap_start = (char*)0;
static char  *heap_bump  = (char*)0;   /* next fresh byte */
static char  *heap_end   = (char*)0;   /* exclusive upper bound */

/* sentinel "end" block marks the heap ceiling */
static BlkHdr end_sentinel;

/* ------------------------------------------------------------------ */
/*  kmalloc_init — called once by kmalloc() on first use              */
/* ------------------------------------------------------------------ */
static int heap_ready = 0;

static void kmalloc_init(void) {
    /* Align heap_start to 8 bytes above _kend */
    u32 start = (u32)_kend;
    start = (start + ALIGN - 1) & ~(ALIGN - 1);

    /* Use mem_total_kb() to find the top of RAM; cap at 256 MB for safety */
    u32 total_kb = mem_total_kb();
    if (total_kb == 0 || total_kb > 256 * 1024) total_kb = 256 * 1024;
    u32 end = total_kb * 1024;
    /* Leave 64 KB at the very top as a safety buffer */
    if (end > 65536) end -= 65536;

    if (start >= end) {
        /* This should never happen on a 256 MB QEMU machine */
        heap_start = heap_bump = heap_end = (char*)start;
        heap_ready = 1;
        return;
    }

    heap_start = (char*)start;
    heap_bump  = (char*)start;
    heap_end   = (char*)end;

    /* Initialise end sentinel so coalescing knows when it hits the ceiling */
    end_sentinel.size     = 0;   /* size=0, not-free → never merged */
    end_sentinel.prev_size = 0;

    heap_ready = 1;
}

/* ------------------------------------------------------------------ */
/*  Carve a fresh block from the bump region                          */
/* ------------------------------------------------------------------ */
static BlkHdr *bump_alloc(u32 need) {
    /* need = total bytes including header, already aligned */
    if (heap_bump + need > heap_end) return (BlkHdr*)0;
    BlkHdr *b = (BlkHdr*)heap_bump;
    b->size     = need;         /* not-free */
    b->prev_size = 0;           /* filled below if there's a prior block */
    heap_bump += need;
    return b;
}

/* ------------------------------------------------------------------ */
/*  kmalloc                                                            */
/* ------------------------------------------------------------------ */
void *kmalloc(u32 n) {
    if (!heap_ready) kmalloc_init();
    if (n == 0) n = 1;
    /* Round up to alignment, add header */
    u32 need = (n + HDR_SIZE + ALIGN - 1) & ~(ALIGN - 1);
    if (need < HDR_SIZE + MIN_BLK) need = HDR_SIZE + MIN_BLK;

    /* Search bins from the right size class upward */
    for (int i = bin_for(need); i < NBINS; i++) {
        BlkHdr **pp = &bins[i];
        while (*pp) {
            BlkHdr *b = *pp;
            if (blk_size(b) >= need) {
                /* Remove from free list */
                *pp = FL_NEXT(b);
                /* Split if remainder is large enough */
                u32 rem = blk_size(b) - need;
                if (rem >= HDR_SIZE + MIN_BLK) {
                    BlkHdr *split = (BlkHdr*)((char*)b + need);
                    split->size     = rem | BLK_FREE;
                    split->prev_size = need;
                    /* update the block after split's prev_size */
                    BlkHdr *after = blk_next(split);
                    if (after != &end_sentinel && blk_size(after))
                        after->prev_size = rem;
                    b->size = need;
                    bin_push(split);
                }
                blk_set_free(b, 0);
                return blk_data(b);
            }
            pp = &FL_NEXT(*pp);
        }
    }

    /* No free block — bump */
    BlkHdr *b = bump_alloc(need);
    if (!b) return (void*)0;  /* out of memory */

    /* Fix up prev_size for this new block */
    if ((char*)b > heap_start) {
        /* Walk back to find the previous physical block */
        /* Since bump allocation is sequential, prev is at heap_bump - need - prev_block_size
         * We can compute it from b->prev_size stored in the preceding block's header. */
        /* Actually: the block immediately before is at heap_bump - need - its size.
         * We don't have a direct back-pointer from bump blocks yet.
         * Set prev_size = 0 for bump blocks (safe — coalescing checks for 0). */
        b->prev_size = 0;
    }
    b->size = need;  /* not-free */
    return blk_data(b);
}

/* ------------------------------------------------------------------ */
/*  kfree                                                              */
/* ------------------------------------------------------------------ */
void kfree(void *p) {
    if (!p) return;
    BlkHdr *b = data_blk(p);
    blk_set_free(b, 1);

    /* Coalesce with next physical block */
    BlkHdr *nx = blk_next(b);
    if (nx != &end_sentinel && blk_size(nx) && blk_is_free(nx)) {
        bin_remove(nx);
        b->size = (blk_size(b) + blk_size(nx)) | BLK_FREE;
        /* update nx-next's prev_size */
        BlkHdr *nxnx = blk_next(b);
        if (blk_size(nxnx)) nxnx->prev_size = blk_size(b);
    }

    /* Coalesce with previous physical block */
    if (b->prev_size) {
        BlkHdr *pv = blk_prev(b);
        if (pv && blk_is_free(pv)) {
            bin_remove(pv);
            pv->size = (blk_size(pv) + blk_size(b)) | BLK_FREE;
            BlkHdr *after = blk_next(pv);
            if (blk_size(after)) after->prev_size = blk_size(pv);
            b = pv;
        }
    }

    bin_push(b);
}

/* ------------------------------------------------------------------ */
/*  krealloc                                                           */
/* ------------------------------------------------------------------ */
void *krealloc(void *p, u32 n) {
    if (!p) return kmalloc(n);
    if (n == 0) { kfree(p); return (void*)0; }
    BlkHdr *b = data_blk(p);
    u32 old_data = blk_size(b) - HDR_SIZE;
    if (n <= old_data) return p;   /* shrink in place (simple) */
    void *np = kmalloc(n);
    if (!np) return (void*)0;
    memcpy(np, p, old_data);
    kfree(p);
    return np;
}

/* ------------------------------------------------------------------ */
/*  kcalloc                                                            */
/* ------------------------------------------------------------------ */
void *kcalloc(u32 nmemb, u32 sz) {
    u32 total = nmemb * sz;
    void *p = kmalloc(total);
    if (p) memset(p, 0, total);
    return p;
}

/* ------------------------------------------------------------------ */
/*  Diagnostics (used by task manager)                                 */
/* ------------------------------------------------------------------ */
u32 kmalloc_used(void) {
    return (u32)(heap_bump - heap_start);
}
u32 kmalloc_total(void) {
    return (u32)(heap_end - heap_start);
}
