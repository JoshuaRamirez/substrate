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
 *
 * It also carries the DATABASE PATH: where the owner persists the count. Any
 * peer may retarget it by writing here; the owner notices on its next tick.
 * This is the one shared value that is neither a single atomic nor a row with
 * a sole writer, so it is the one value that needs a protocol -- a seqlock.
 * The exception proves the rule the rest of this file is built on.
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
#include <sys/stat.h>
#include <sched.h>
#include <errno.h>

#define CNT_SHM_NAME  "/cnt.v7"      /* macOS caps shm names at 31 chars */
#define CNT_MAGIC     0x434E5437u    /* "CNT7" */
#define CNT_VERSION   7u             /* pin the FORMAT, not the binary */
#define CNT_MAX_PEERS 256            /* was 16, which silently capped contention
                                      * tests at 16 peers no matter how many
                                      * ring slots existed. The registry, not
                                      * the ring, was the scaling limit. */
#define CNT_NAMELEN   16
#define CNT_PATHLEN   256
#define CNT_RING_SLOTS 32
#define CNT_BLOCKS     64                 /* arena blocks, one bitmap word each side */
#define CNT_BLOCK_SIZE (1024 * 1024)      /* 64 MB of arena in total */
#define CNT_ARENA_SIZE ((size_t)CNT_BLOCKS * CNT_BLOCK_SIZE)
#define CNT_KEYS       256                /* blob store slots */
#define CNT_TOKLEN    33             /* 128 bits, hex, + NUL */

/* One row of the dashboard. Written only by the process that claimed it. */
typedef struct {
    _Atomic uint32_t state;              /* 0 = free, 1 = live */
    _Atomic uint64_t pid;
    _Atomic uint64_t ops;                /* bumps issued by this peer */
    _Atomic uint64_t since;              /* unix seconds at join */
    char             name[CNT_NAMELEN];
    char             role[CNT_NAMELEN];
} cnt_peer;

/* ---- the ring: one slot per in-flight request ----
 *
 * Everything else in this segment is fire-and-forget or shared-read, and needs
 * no protocol. A verb whose RESULT the caller must wait for is the first thing
 * that does. This is that protocol, and it is the smallest one that works:
 * a slot the client owns for the duration of one call.
 *
 *   free --(client CAS)--> request --(owner)--> done --(client)--> free
 *
 * No lock. The slot has exactly one writer at every point in its cycle: the
 * client owns it in `free` and `done`, the owner owns it in `request`. The
 * state word is the handoff, and it is a single atomic. Ownership is
 * partitioned in TIME rather than in space -- the same trick as the peer
 * table, turned sideways. */
typedef struct {
    _Atomic uint32_t state;      /* CNT_SLOT_* below */
    _Atomic uint32_t op;         /* CNT_OP_* */
    _Atomic uint64_t client;     /* pid, so the owner can free a dead caller's slot */
    _Atomic uint64_t arg;        /* reserve: how many ids. put/get: the key. */
    _Atomic uint64_t result;     /* reserve: base id. get: block index. */
    _Atomic uint64_t len;        /* put/get: payload length */
    _Atomic uint64_t block;      /* put: which arena block holds the payload */
    _Atomic uint32_t err;        /* 0 ok, nonzero refused */
} cnt_slot;

/* One entry of the blob store the owner keeps. */
typedef struct {
    _Atomic uint64_t key;        /* 0 = empty */
    _Atomic uint64_t block;
    _Atomic uint64_t len;
} cnt_blob;

#define CNT_SLOT_FREE    0u
#define CNT_SLOT_REQUEST 1u
#define CNT_SLOT_DONE    2u
#define CNT_SLOT_SERVING 3u   /* the owner has taken it; the client may NOT reclaim */
#define CNT_SLOT_CLAIMED 4u   /* the client holds it but has not filled it in yet */

/* Why SERVING exists.
 *
 * Without it, a client that timed out set its slot back to FREE -- possibly
 * while the owner was midway through serving that very slot. A new client
 * would claim the slot, and the owner's stale write would land on the new
 * request. The id allocated for the first caller reached nobody.
 *
 * Under 64 concurrent clients that leaked 3703 ids out of 128000. It is
 * invisible below the core count: with a spare core per client, nothing ever
 * times out, so the window never opens. Contention did not slow this design
 * down, it found a correctness bug in it.
 *
 * Now the handoff is a CAS both ways. The owner takes REQUEST -> SERVING, so
 * a timing-out client can only reclaim a slot nobody has picked up yet. If it
 * loses that race the work is already in flight, so it waits for the answer
 * rather than abandoning an id that has already been spent. */

/* What a slot is asking for. */
#define CNT_OP_RESERVE   0u
#define CNT_OP_PUT       1u    /* hand a block to the owner; it keeps it */
#define CNT_OP_GET       2u    /* ask where a blob is; read it in place */

/* The entire shared "database". */
typedef struct {
    uint32_t          magic;
    uint32_t          version;
    _Atomic uint64_t  count;
    _Atomic uint64_t  owner_pid;           /* the one process that owns this page */
    _Atomic uint32_t  path_seq;            /* odd = a path write is in flight */
    _Atomic uint64_t  path_writer;         /* pid holding path_seq odd, else 0 */
    char              path[CNT_PATHLEN];   /* absolute; the owner's database */
    char              admin[CNT_TOKLEN];   /* minted by the owner before `magic` */
    _Atomic uint64_t  next_id;             /* the id space the ring hands out */
    _Atomic uint64_t  block_free;          /* bitmap: 1 = free. 64 blocks, 64 bits. */
    _Atomic uint64_t  block_owner[CNT_BLOCKS];   /* pid holding each block, 0 = owner's */
    cnt_blob          blobs[CNT_KEYS];
    cnt_slot          ring[CNT_RING_SLOTS];
    cnt_peer          peers[CNT_MAX_PEERS];
    /* The arena goes LAST and is page-aligned, so the header stays small and a
     * peer that only wants the counter never touches 64 MB of anything. */
    _Alignas(16384) uint8_t arena[CNT_ARENA_SIZE];
} cnt_seg;

typedef struct {
    _Atomic uint64_t *cell;      /* <- mode switch lives here */
    _Atomic uint64_t *ops;       /* <- and here. same trick, same pattern. */
    _Atomic uint64_t  own;       /* backing store when alone */
    _Atomic uint64_t  own_ops;
    _Atomic uint64_t  own_next;  /* the id space, when alone */
    cnt_seg          *seg;       /* non-NULL only when joined AND attached */
    cnt_peer         *slot;      /* this process's row, when attached */
    int               joined;    /* was --join asked for? */
    int               detached;  /* asked to join, but the owner is gone */
    char              name[CNT_NAMELEN];
    char              role[CNT_NAMELEN];
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




/* ---- the owner, and surviving its departure ---- */

/* A segment is only as alive as the process that made it. When dbd exits it
 * zeroes `magic`; when it is killed, `magic` survives but its pid does not.
 * Check both, because shm_unlink removes the NAME, not the MAPPING -- a peer
 * holding an unlinked page keeps it alive and would otherwise never find out
 * the world had moved on. That was a real split-brain, not a hypothetical. */
static inline int cnt_owner_alive(cnt_seg *s) {
    if (!s || s->magic != CNT_MAGIC || s->version != CNT_VERSION) return 0;
    pid_t p = (pid_t)atomic_load(&s->owner_pid);
    return p > 0 && kill(p, 0) == 0;
}

/* Forward decls: revalidation needs both. */
static inline cnt_seg *cnt_attach(int *err);
static inline cnt_peer *cnt_claim_slot(cnt_seg *s, const char *name, const char *role);

/* Call this from a loop, a request handler, or a frame. Returns 1 if this
 * process is attached to a live owner, 0 if it is not.
 *
 * While detached, the counter keeps working against private memory so nothing
 * crashes -- but counter_mode() says "detached", so no UI ever shows a shared
 * number that is not shared. When an owner reappears, the private count is
 * discarded: the owner's value is the truth, not ours. */
static inline int counter_revalidate(counter *c) {
    if (!c->joined) return 0;                       /* alone on purpose */

    if (c->seg && cnt_owner_alive(c->seg)) return 1; /* the common case */

    if (c->seg) {                                    /* the owner went away */
        munmap(c->seg, sizeof(cnt_seg));
        c->seg  = NULL;
        c->slot = NULL;
        c->cell = &c->own;
        c->ops  = &c->own_ops;
        c->detached = 1;
    }

    int err = 0;
    cnt_seg *s = cnt_attach(&err);                   /* has a new owner appeared? */
    if (!s) return 0;
    if (!cnt_owner_alive(s)) { munmap(s, sizeof *s); return 0; }

    cnt_peer *slot = cnt_claim_slot(s, c->name, c->role);
    if (!slot) { munmap(s, sizeof *s); return 0; }

    c->seg      = s;
    c->slot     = slot;
    c->cell     = &s->count;
    c->ops      = &slot->ops;
    c->detached = 0;
    return 1;
}



/* ---- the arena: payloads that are never copied across the boundary ----
 *
 * The ring carries small fixed-size words. A payload cannot go in it, and
 * copying one through a socket is exactly the cost this whole design exists
 * to avoid. So payloads live in the segment, and what crosses the boundary is
 * a DESCRIPTOR: a block index and a length.
 *
 * A reader never copies. It is handed an offset into a page it already has
 * mapped, and it reads the bytes in place. That is the difference between
 * this and a socket, and it is the reason the gap widens with payload size
 * instead of staying constant.
 *
 * Allocation is one 64-bit bitmap and a CAS. 64 blocks of 1 MB. Each block
 * records the pid holding it, so the owner can reclaim what a dead peer left.
 */

/* Claim a free block. Returns its index, or -1 if the arena is full. */
static inline int cnt_block_alloc(cnt_seg *s) {
    for (int tries = 0; tries < 4096; tries++) {
        uint64_t m = atomic_load_explicit(&s->block_free, memory_order_relaxed);
        if (m == 0) return -1;                       /* arena full: real backpressure */
        int i = __builtin_ctzll(m);
        uint64_t want = m & ~(1ull << i);
        if (atomic_compare_exchange_weak_explicit(&s->block_free, &m, want,
                memory_order_acquire, memory_order_relaxed)) {
            atomic_store(&s->block_owner[i], (uint64_t)getpid());
            return i;
        }
    }
    return -1;
}

static inline void cnt_block_free(cnt_seg *s, int i) {
    if (i < 0 || i >= CNT_BLOCKS) return;
    atomic_store(&s->block_owner[i], 0);
    atomic_fetch_or_explicit(&s->block_free, 1ull << i, memory_order_release);
}

/* The bytes themselves. No copy happens here -- this is address arithmetic. */
static inline uint8_t *cnt_block_ptr(cnt_seg *s, int i) {
    return (i < 0 || i >= CNT_BLOCKS) ? NULL : s->arena + (size_t)i * CNT_BLOCK_SIZE;
}

/* Reclaim blocks whose holder is gone. The owner calls this; pid 0 means the
 * owner itself took the block, and those are freed with their blob entry. */
static inline int cnt_reap_blocks(cnt_seg *s) {
    int n = 0;
    for (int i = 0; i < CNT_BLOCKS; i++) {
        if (atomic_load(&s->block_free) & (1ull << i)) continue;
        pid_t h = (pid_t)atomic_load(&s->block_owner[i]);
        if (h > 0 && kill(h, 0) != 0) { cnt_block_free(s, i); n++; }
    }
    return n;
}

static inline int cnt_blocks_free(cnt_seg *s) {
    return __builtin_popcountll(atomic_load(&s->block_free));
}

/* ---- reserve: the verb whose answer the caller waits for ----
 *
 * THE TEST. Every other call here is a store the caller walks away from.
 * This one needs a value back, computed by the owner, and that is the case
 * the whole "joining is a flag" claim had never faced.
 *
 * The call site is the same in both modes -- but be exact about what that
 * costs. Alone, this is an add. Joined, it is a round trip: a slot claim, a
 * publish, a spin, a read. Same signature, same result, ~three orders of
 * magnitude apart. The abstraction holds; the performance does not pretend to.
 */


/* ---- waiting without starving the thing you are waiting for ----
 *
 * Spinning is right when the reply is nanoseconds away and there is a spare
 * core to spin on. Neither holds under load: with more clients than cores,
 * every spinning client is stealing CPU from the single owner that has to
 * answer all of them. Measured, 64 clients on 16 cores spinning flat out took
 * throughput from 2.3M/s to 16k/s and pushed tail latency into the 2-second
 * timeout -- requests DROPPED, not merely delayed.
 *
 * So: spin only briefly, then yield, then sleep. The yield is the important
 * step, because it is what lets the owner run.
 */
static inline void cnt_backoff(long attempt) {
    if (attempt < 64) {
#if defined(__aarch64__)
        __asm__ __volatile__("yield");
#elif defined(__x86_64__)
        __asm__ __volatile__("pause");
#endif
    } else if (attempt < 256) {
        sched_yield();                 /* let the owner have the core */
    } else {
        usleep(50);                    /* deeply contended: get out of the way */
    }
}

#define CNT_CALL_TIMEOUT_NS 2000000000ll   /* 2 seconds, measured on a clock */

static inline int64_t cnt_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000ll + t.tv_nsec;
}

/* What one call returns. */
typedef struct {
    uint64_t result;      /* reserve: base id.  get: block index. */
    uint64_t len;         /* get: payload length */
    uint32_t err;         /* 0 ok */
    int      full;        /* 1 = no slot was free: this is backpressure, not failure */
} cnt_reply;

/* ---- backpressure ----
 *
 * The ring has 32 slots. More than 32 callers in flight is not an error, it is
 * a full queue, and the two are worth telling apart: a full queue means WAIT,
 * a failure means GIVE UP. Returning 0 for both -- which is what this did
 * before -- silently turns an overload into data loss.
 *
 * So the slot claim spins with a bounded backoff until a slot frees or the
 * deadline passes, and the caller is told which happened. No request is
 * dropped while any slot is cycling; throughput degrades, correctness does
 * not.
 */
static inline cnt_reply cnt_call(cnt_seg *s, uint32_t op, uint64_t arg,
                                 uint64_t len, uint64_t block, int64_t timeout_ns) {
    cnt_reply r = {0, 0, 0, 0};
    int64_t deadline = cnt_now_ns() + timeout_ns;

    cnt_slot *mine = NULL;
    for (long attempt = 0; !mine; attempt++) {
        for (int i = 0; i < CNT_RING_SLOTS && !mine; i++) {
            uint32_t expect = CNT_SLOT_FREE;
            /* CLAIMED, not REQUEST: the slot is ours but its fields are still
             * the PREVIOUS caller's. Publishing REQUEST here would let the
             * owner read stale args and spend ids on them -- 784 leaked out of
             * 128000 before this line said CLAIMED. Publish the state LAST,
             * the same rule dbd uses for `magic`. */
            if (atomic_compare_exchange_strong(&s->ring[i].state, &expect, CNT_SLOT_CLAIMED))
                mine = &s->ring[i];
        }
        if (mine) break;
        if (cnt_now_ns() > deadline || !cnt_owner_alive(s)) { r.full = 1; return r; }
        cnt_backoff(attempt);
    }

    atomic_store(&mine->client, (uint64_t)getpid());
    atomic_store(&mine->op, op);
    atomic_store(&mine->arg, arg);
    atomic_store(&mine->len, len);
    atomic_store(&mine->block, block);
    atomic_store(&mine->err, 0);
    atomic_store(&mine->result, 0);
    atomic_store_explicit(&mine->state, CNT_SLOT_REQUEST, memory_order_release);  /* now it is real */

    for (long spin = 0; ; spin++) {
        if (atomic_load_explicit(&mine->state, memory_order_acquire) == CNT_SLOT_DONE) {
            r.err    = atomic_load(&mine->err);
            r.result = atomic_load(&mine->result);
            r.len    = atomic_load(&mine->len);
            atomic_store(&mine->client, 0);
            atomic_store_explicit(&mine->state, CNT_SLOT_FREE, memory_order_release);
            return r;
        }
        cnt_backoff(spin);
        /* Wall time, not a spin count: a spin count silently means 4ms on a
         * fast machine and 40ms on a slow one. */
        if ((spin & 0x3FF) == 0x3FF) {
            if (!cnt_owner_alive(s)) break;
            if (cnt_now_ns() > deadline) {
                /* Give up ONLY if the owner has not taken the slot. If this
                 * CAS fails the request is in flight and its id is already
                 * spent, so abandoning it would lose an id forever. */
                uint32_t mineNow = CNT_SLOT_REQUEST;
                if (atomic_compare_exchange_strong(&mine->state, &mineNow, CNT_SLOT_FREE)) {
                    atomic_store(&mine->client, 0);
                    r.err = 0xFFFF;
                    return r;
                }
                deadline = cnt_now_ns() + CNT_CALL_TIMEOUT_NS;   /* in flight: wait it out */
            }
        }
    }
    /* Owner died mid-call. The slot is unreclaimable by us; the owner's
     * successor reaps it by pid. */
    r.err = 0xFFFF;
    return r;
}

/* ---- the verbs ---- */

/* Returns the base of a reserved range of `n` ids, or 0 on failure.
 * Same call site in both modes: alone it is an add, joined it is a round trip. */
static inline uint64_t counter_reserve(counter *c, uint64_t n) {
    if (n == 0) return 0;
    if (!c->seg) return atomic_fetch_add_explicit(&c->own_next, n, memory_order_relaxed);
    cnt_reply r = cnt_call(c->seg, CNT_OP_RESERVE, n, 0, 0, CNT_CALL_TIMEOUT_NS);
    return r.err ? 0 : r.result;
}

/* Store a payload under `key`. ONE copy: caller's buffer -> arena. After that
 * the bytes never move again, however many readers there are. */
static inline int counter_put(counter *c, uint64_t key, const void *data, uint64_t len) {
    if (!c->seg || len == 0 || len > CNT_BLOCK_SIZE) return -1;
    int b = cnt_block_alloc(c->seg);
    if (b < 0) return -2;                              /* arena full */
    memcpy(cnt_block_ptr(c->seg, b), data, (size_t)len);
    cnt_reply r = cnt_call(c->seg, CNT_OP_PUT, key, len, (uint64_t)b, CNT_CALL_TIMEOUT_NS);
    if (r.err || r.full) { cnt_block_free(c->seg, b); return -3; }
    return 0;
}

/* Hand the owner a block the caller has already filled. This is the ingest
 * path that avoids the second copy: a server can read a socket STRAIGHT INTO
 * the arena and then publish, instead of landing it in a buffer first. */
static inline int counter_publish(counter *c, uint64_t key, int block, uint64_t len) {
    if (!c->seg || block < 0 || len == 0 || len > CNT_BLOCK_SIZE) return -1;
    cnt_reply r = cnt_call(c->seg, CNT_OP_PUT, key, len, (uint64_t)block, CNT_CALL_TIMEOUT_NS);
    if (r.err || r.full) { cnt_block_free(c->seg, block); return -3; }
    return 0;
}

/* Locate a payload. Returns a pointer INTO THE SHARED PAGE -- zero copies.
 * The caller reads the bytes where they already are. */
static inline const uint8_t *counter_get(counter *c, uint64_t key, uint64_t *len_out) {
    if (!c->seg) return NULL;
    cnt_reply r = cnt_call(c->seg, CNT_OP_GET, key, 0, 0, CNT_CALL_TIMEOUT_NS);
    if (r.err || r.full) return NULL;
    if (len_out) *len_out = r.len;
    return cnt_block_ptr(c->seg, (int)r.result);
}

/* The owner side. Serves every pending slot; returns how many it served.
 * Call it in a tight loop -- which is the real cost of a reply-carrying verb:
 * the owner stops being a housekeeper on a tick and becomes a server. */
static inline int cnt_serve_ring(cnt_seg *s) {
    int served = 0;
    for (int i = 0; i < CNT_RING_SLOTS; i++) {
        cnt_slot *q = &s->ring[i];
        uint32_t want = CNT_SLOT_REQUEST;
        if (!atomic_compare_exchange_strong_explicit(&q->state, &want, CNT_SLOT_SERVING,
                memory_order_acquire, memory_order_relaxed))
            continue;                       /* not ours, or already taken */

        pid_t who = (pid_t)atomic_load(&q->client);
        if (who > 0 && kill(who, 0) != 0) {        /* caller died mid-call */
            atomic_store(&q->client, 0);
            atomic_store_explicit(&q->state, CNT_SLOT_FREE, memory_order_release);
            continue;
        }

        uint32_t op  = atomic_load(&q->op);
        uint64_t arg = atomic_load(&q->arg);
        atomic_store(&q->err, 0);

        if (op == CNT_OP_RESERVE) {
            if (arg == 0 || arg > (1ull << 32)) { atomic_store(&q->err, 1); }
            else atomic_store(&q->result,
                     atomic_fetch_add_explicit(&s->next_id, arg, memory_order_relaxed));

        } else if (op == CNT_OP_PUT) {
            uint64_t len = atomic_load(&q->len);
            int      blk = (int)atomic_load(&q->block);
            if (arg == 0 || blk < 0 || blk >= CNT_BLOCKS || len > CNT_BLOCK_SIZE) {
                atomic_store(&q->err, 2);
            } else {
                int slot = -1, reuse = -1;
                for (int k = 0; k < CNT_KEYS; k++) {
                    uint64_t kk = atomic_load(&s->blobs[k].key);
                    if (kk == arg) { reuse = k; break; }
                    if (kk == 0 && slot < 0) slot = k;
                }
                if (reuse >= 0) {           /* replace: free what it pointed at */
                    cnt_block_free(s, (int)atomic_load(&s->blobs[reuse].block));
                    slot = reuse;
                }
                if (slot < 0) { atomic_store(&q->err, 3); }      /* blob table full */
                else {
                    atomic_store(&s->block_owner[blk], 0);       /* the owner holds it now */
                    atomic_store(&s->blobs[slot].block, (uint64_t)blk);
                    atomic_store(&s->blobs[slot].len, len);
                    atomic_store(&s->blobs[slot].key, arg);
                    atomic_store(&q->result, (uint64_t)blk);
                }
            }

        } else if (op == CNT_OP_GET) {
            int found = 0;
            for (int k = 0; k < CNT_KEYS; k++) {
                if (atomic_load(&s->blobs[k].key) == arg) {
                    atomic_store(&q->result, atomic_load(&s->blobs[k].block));
                    atomic_store(&q->len,    atomic_load(&s->blobs[k].len));
                    found = 1;
                    break;
                }
            }
            if (!found) atomic_store(&q->err, 4);

        } else {
            atomic_store(&q->err, 5);
        }

        atomic_store_explicit(&q->state, CNT_SLOT_DONE, memory_order_release);
        served++;
    }
    return served;
}

/* Free slots whose caller is gone, so a crashed client cannot leak the ring. */
static inline int cnt_reap_ring(cnt_seg *s) {
    int n = 0;
    for (int i = 0; i < CNT_RING_SLOTS; i++) {
        cnt_slot *q = &s->ring[i];
        uint32_t st = atomic_load(&q->state);
        if (st == CNT_SLOT_FREE) continue;
        pid_t who = (pid_t)atomic_load(&q->client);
        if (who > 0 && kill(who, 0) == 0) continue;
        atomic_store(&q->client, 0);
        atomic_store_explicit(&q->state, CNT_SLOT_FREE, memory_order_release);
        n++;
    }
    return n;
}

/* ---- the admin credential ---- */

/* The trust boundary of this system is the shm file mode, not a password.
 * Anything that can map the segment can already write every value in it, so
 * peers are inside by construction -- `top` never asks for a token.
 *
 * What is OUTSIDE is webd's callers: a browser tab can reach port 8080 but
 * cannot map shm. So the owner mints a random token into the segment, and
 * BEING ABLE TO READ IT is the credential. webd demands it from callers and
 * reads it from the segment to check. No secret ever leaves the substrate
 * except into the page webd serves, which the same-origin policy protects.
 */

static inline void cnt_mint_admin(cnt_seg *s) {
    static const char HEX[] = "0123456789abcdef";
    unsigned char raw[16];
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(raw, 1, sizeof raw, f) != sizeof raw) {
        /* Never fall back to something guessable: refuse instead. */
        if (f) fclose(f);
        s->admin[0] = 0;
        return;
    }
    fclose(f);
    for (int i = 0; i < 16; i++) {
        s->admin[i * 2]     = HEX[raw[i] >> 4];
        s->admin[i * 2 + 1] = HEX[raw[i] & 15];
    }
    s->admin[32] = 0;
}

/* Constant-time compare: a token check should not leak its prefix by timing. */
static inline int cnt_admin_ok(cnt_seg *s, const char *presented) {
    if (!s || !presented || !s->admin[0]) return 0;
    size_t n = strlen(s->admin);
    if (strlen(presented) != n) return 0;
    unsigned char d = 0;
    for (size_t i = 0; i < n; i++) d |= (unsigned char)(s->admin[i] ^ presented[i]);
    return d == 0;
}

/* ---- the database path: the one value that needs a protocol ---- */

/* Every other shared value here is a single atomic, or a row whose only writer
 * is the process that claimed it. The path is neither: it is 256 bytes and any
 * peer may write it. So it gets a seqlock -- the same "publish last" shape dbd
 * uses for `magic`, generalised. Writers take the sequence odd, copy, put it
 * back even. Readers copy, then check the sequence did not move under them. */

static inline int cnt_path_read(cnt_seg *s, char *out, size_t cap) {
    char tmp[CNT_PATHLEN];
    for (int tries = 0; tries < 128; tries++) {
        uint32_t a = atomic_load_explicit(&s->path_seq, memory_order_acquire);
        if (a & 1) continue;                       /* a write is in flight */
        memcpy(tmp, s->path, CNT_PATHLEN);
        tmp[CNT_PATHLEN - 1] = 0;                  /* a torn copy is still bounded */
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(&s->path_seq, memory_order_relaxed) == a) {
            snprintf(out, cap, "%s", tmp);
            return 1;
        }
    }
    if (cap) out[0] = 0;
    return 0;
}

/* Absolute paths only: peers have different working directories, so a relative
 * path names a different file in each one. Returns 0, or -1 relative, -2 too
 * long, -3 contended. */
static inline int cnt_path_write(cnt_seg *s, const char *path) {
    if (!path || path[0] != '/') return -1;
    size_t n = strlen(path);
    if (n >= CNT_PATHLEN) return -2;
    for (int tries = 0; tries < 4096; tries++) {
        uint32_t a = atomic_load_explicit(&s->path_seq, memory_order_relaxed);
        if (a & 1) continue;
        if (!atomic_compare_exchange_weak_explicit(&s->path_seq, &a, a + 1,
                memory_order_acquire, memory_order_relaxed)) continue;
        atomic_store(&s->path_writer, (uint64_t)getpid());
        memset(s->path, 0, CNT_PATHLEN);
        memcpy(s->path, path, n);
        atomic_store(&s->path_writer, 0);
        atomic_store_explicit(&s->path_seq, a + 2, memory_order_release);
        return 0;
    }
    return -3;
}


/* A writer killed between taking the sequence odd and putting it back even
 * would wedge the path forever: every reader spins out, every writer spins
 * out. Only the owner repairs it, and only once the writer is provably dead.
 * Returns 1 if it repaired something. */
static inline int cnt_seqlock_repair(cnt_seg *s) {
    uint32_t a = atomic_load(&s->path_seq);
    if (!(a & 1)) return 0;
    pid_t w = (pid_t)atomic_load(&s->path_writer);
    if (w > 0 && kill(w, 0) == 0) return 0;          /* still alive: mid-write */
    s->path[CNT_PATHLEN - 1] = 0;                    /* bound any torn copy */
    atomic_store(&s->path_writer, 0);
    atomic_store_explicit(&s->path_seq, a + 1, memory_order_release);
    return 1;
}

static inline const char *cnt_path_error(int rc) {
    switch (rc) {
        case 0:  return "ok";
        case -1: return "path must be absolute";
        case -2: return "path too long";
        default: return "path is being written by another peer";
    }
}

/* Resolve against this process's cwd, so what lands in the segment is absolute. */
static inline void cnt_abspath(const char *in, char *out, size_t cap) {
    if (!in || !*in) { if (cap) out[0] = 0; return; }
    if (in[0] == '/') { snprintf(out, cap, "%s", in); return; }
    char cwd[1024];
    if (getcwd(cwd, sizeof cwd)) snprintf(out, cap, "%s/%s", cwd, in);
    else                         snprintf(out, cap, "%s", in);
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
    atomic_store(&c->own_next, 1);   /* id 0 means "failed" in both modes */
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
    snprintf(c->name, sizeof c->name, "%s", name);
    snprintf(c->role, sizeof c->role, "%s", role);
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
    if (!c->joined)  return "alone";
    return c->detached ? "detached" : "joined";
}

#endif
