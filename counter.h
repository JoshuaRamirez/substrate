/* counter.h -- the whole thesis, in one header.
 *
 * An executable is complete alone; joining a shared pipeline is a flag.
 *
 * The two modes differ by ONE POINTER VALUE:
 *
 *     alone   ->  c.cell = &c.own     (private memory in this process)
 *     joined  ->  c.cell = shared     (a page shared by every peer)
 *
 * counter_bump() and counter_read() are byte-identical in both modes.
 * There is not even a branch on the hot path. That is the claim.
 */
#ifndef COUNTER_H
#define COUNTER_H

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>

#define CNT_SHM_NAME "/cnt.v1"      /* macOS caps shm names at 31 chars */
#define CNT_MAGIC    0x434E5431u    /* "CNT1" */
#define CNT_VERSION  1u             /* pin the FORMAT, not the binary */

/* The entire shared "database". */
typedef struct {
    uint32_t          magic;
    uint32_t          version;
    _Atomic uint64_t  count;
} cnt_seg;

typedef struct {
    _Atomic uint64_t *cell;   /* <- the mode switch lives here, and nowhere else */
    _Atomic uint64_t  own;    /* backing store when alone */
    cnt_seg          *seg;    /* non-NULL only when joined */
    int               joined;
} counter;

/* ---- the call sites. identical in both modes. ---- */

static inline uint64_t counter_bump(counter *c) {
    return atomic_fetch_add_explicit(c->cell, 1, memory_order_relaxed) + 1;
}

static inline uint64_t counter_read(counter *c) {
    return atomic_load_explicit(c->cell, memory_order_relaxed);
}

/* ---- construction. the ONLY place the two modes are distinguishable. ---- */

/* Alone: complete, self-contained, no peers, no segment, no failure mode. */
static inline void counter_open_alone(counter *c) {
    memset(c, 0, sizeof *c);
    atomic_store(&c->own, 0);
    c->cell   = &c->own;
    c->joined = 0;
}

/* Joined: point at the owner's page. Returns 0 on success, -1 if no owner. */
static inline int counter_open_joined(counter *c) {
    memset(c, 0, sizeof *c);
    int fd = shm_open(CNT_SHM_NAME, O_RDWR, 0600);
    if (fd < 0) {
        fprintf(stderr, "join failed: no owner on %s (%s) -- start dbd first\n",
                CNT_SHM_NAME, strerror(errno));
        return -1;
    }
    void *p = mmap(NULL, sizeof(cnt_seg), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { perror("mmap"); return -1; }

    cnt_seg *s = (cnt_seg *)p;
    if (s->magic != CNT_MAGIC) {
        fprintf(stderr, "join refused: bad magic 0x%08x\n", s->magic);
        munmap(p, sizeof *s); return -1;
    }
    if (s->version != CNT_VERSION) {
        fprintf(stderr, "join refused: segment format v%u, this binary speaks v%u\n",
                s->version, CNT_VERSION);
        munmap(p, sizeof *s); return -1;
    }
    c->seg    = s;
    c->cell   = &s->count;   /* <-- the entire difference between the two modes */
    c->joined = 1;
    return 0;
}

/* One flag in, a working counter out. This is the "SDK pipeline" surface. */
static inline int counter_open(counter *c, int join) {
    if (!join) { counter_open_alone(c); return 0; }
    return counter_open_joined(c);
}

static inline const char *counter_mode(counter *c) {
    return c->joined ? "joined" : "alone";
}

#endif
