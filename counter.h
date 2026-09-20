/* counter.h -- the whole thesis, in one header.
 *
 * An executable is complete alone; joining a shared pipeline is a flag.
 *
 * The modes differ by POINTER VALUES, not by code paths:
 *
 *     alone   ->  c.cell = &c.own        c.ops = &c.own_ops
 *     joined  ->  c.cell = &seg->count   c.ops = &slot->ops
 *
 * counter_bump() and counter_read() are identical in both modes.
 * No branch on the hot path.
 *
 * The segment also carries a PEER TABLE: the registry the dashboard reads.
 * Each process owns exactly one slot and is its only writer, so the registry
 * needs no lock and no protocol either -- ownership is partitioned, not shared.
 */
#ifndef COUNTER_H
#define COUNTER_H

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/mman.h>
#include <errno.h>

#define CNT_SHM_NAME  "/cnt.v2"      /* macOS caps shm names at 31 chars */
#define CNT_MAGIC     0x434E5432u    /* "CNT2" */
#define CNT_VERSION   2u             /* pin the FORMAT, not the binary */
#define CNT_MAX_PEERS 16
#define CNT_NAMELEN   16

/* One row of the dashboard. Written only by the process that claimed it. */
typedef struct {
    _Atomic uint32_t state;              /* 0 = free, 1 = live */
    _Atomic uint64_t pid;
    _Atomic uint64_t ops;                /* bumps issued by this peer */
    _Atomic uint64_t since;              /* unix seconds at join */
    char             name[CNT_NAMELEN];
    char             role[CNT_NAMELEN];
} cnt_peer;

/* The entire shared "database". */
typedef struct {
    uint32_t          magic;
    uint32_t          version;
    _Atomic uint64_t  count;
    cnt_peer          peers[CNT_MAX_PEERS];
} cnt_seg;

typedef struct {
    _Atomic uint64_t *cell;      /* <- mode switch lives here */
    _Atomic uint64_t *ops;       /* <- and here. same trick, same pattern. */
    _Atomic uint64_t  own;       /* backing store when alone */
    _Atomic uint64_t  own_ops;
    cnt_seg          *seg;       /* non-NULL only when joined */
    cnt_peer         *slot;      /* this process's row, when joined */
    int               joined;
} counter;

/* ---- the call sites. identical in both modes. ---- */

static inline uint64_t counter_bump(counter *c) {
    atomic_fetch_add_explicit(c->ops, 1, memory_order_relaxed);
    return atomic_fetch_add_explicit(c->cell, 1, memory_order_relaxed) + 1;
}

static inline uint64_t counter_read(counter *c) {
    return atomic_load_explicit(c->cell, memory_order_relaxed);
}

/* ---- the registry ---- */

/* Liveness is not a heartbeat. It is the OS answering a question. */
static inline int cnt_peer_alive(const cnt_peer *p) {
    if (atomic_load(&p->state) != 1) return 0;
    pid_t pid = (pid_t)atomic_load(&p->pid);
    return pid > 0 && kill(pid, 0) == 0;
}

static inline cnt_peer *cnt_claim_slot(cnt_seg *s, const char *name, const char *role) {
    for (int i = 0; i < CNT_MAX_PEERS; i++) {
        cnt_peer *p = &s->peers[i];
        uint32_t expect = 0;
        if (atomic_compare_exchange_strong(&p->state, &expect, 1)) {
            atomic_store(&p->pid, (uint64_t)getpid());
            atomic_store(&p->ops, 0);
            atomic_store(&p->since, (uint64_t)time(NULL));
            snprintf(p->name, CNT_NAMELEN, "%s", name);
            snprintf(p->role, CNT_NAMELEN, "%s", role);
            return p;
        }
    }
    return NULL;
}

static inline void cnt_release_slot(cnt_peer *p) {
    if (!p) return;
    atomic_store(&p->pid, 0);
    atomic_store(&p->state, 0);
}

/* Reap slots whose process is gone. Called by the owner. */
static inline int cnt_reap(cnt_seg *s) {
    int n = 0;
    for (int i = 0; i < CNT_MAX_PEERS; i++) {
        cnt_peer *p = &s->peers[i];
        if (atomic_load(&p->state) == 1 && !cnt_peer_alive(p)) {
            cnt_release_slot(p);
            n++;
        }
    }
    return n;
}


/* Attach to the owner's segment without printing. Returns NULL on failure and
 * sets *err: 1 = no owner, 2 = bad magic, 3 = format mismatch. The dashboard
 * polls this, so it must stay silent. */
static inline cnt_seg *cnt_attach(int *err) {
    int fd = shm_open(CNT_SHM_NAME, O_RDWR, 0600);
    if (fd < 0) { *err = 1; return NULL; }
    void *p = mmap(NULL, sizeof(cnt_seg), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { *err = 1; return NULL; }
    cnt_seg *s = (cnt_seg *)p;
    if (s->magic != CNT_MAGIC) { munmap(p, sizeof *s); *err = 2; return NULL; }
    if (s->version != CNT_VERSION) { munmap(p, sizeof *s); *err = 3; return NULL; }
    *err = 0;
    return s;
}

/* ---- construction. the ONLY place the modes are distinguishable. ---- */

static inline void counter_open_alone(counter *c) {
    memset(c, 0, sizeof *c);
    atomic_store(&c->own, 0);
    atomic_store(&c->own_ops, 0);
    c->cell   = &c->own;
    c->ops    = &c->own_ops;
    c->joined = 0;
}

static inline int counter_open_joined(counter *c, const char *name, const char *role) {
    memset(c, 0, sizeof *c);
    int err = 0;
    cnt_seg *s = cnt_attach(&err);
    if (!s) {
        if (err == 1) fprintf(stderr, "join failed: no owner on %s -- start dbd first\n",
                              CNT_SHM_NAME);
        else if (err == 2) fprintf(stderr, "join refused: bad magic\n");
        else fprintf(stderr, "join refused: this binary speaks format v%u\n", CNT_VERSION);
        return -1;
    }

    cnt_peer *slot = cnt_claim_slot(s, name, role);
    if (!slot) {
        fprintf(stderr, "join refused: peer table full (%d slots)\n", CNT_MAX_PEERS);
        munmap(s, sizeof *s); return -1;
    }

    c->seg    = s;
    c->slot   = slot;
    c->cell   = &s->count;      /* <-- the entire difference between the modes */
    c->ops    = &slot->ops;
    c->joined = 1;
    return 0;
}

/* One flag in, a working counter out. This is the "SDK pipeline" surface. */
static inline int counter_open(counter *c, int join, const char *name, const char *role) {
    if (!join) { counter_open_alone(c); return 0; }
    return counter_open_joined(c, name, role);
}

static inline void counter_close(counter *c) {
    if (c->joined) cnt_release_slot(c->slot);
}

static inline const char *counter_mode(counter *c) {
    return c->joined ? "joined" : "alone";
}

#endif
