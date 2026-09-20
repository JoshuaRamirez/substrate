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
#include <errno.h>

#define CNT_SHM_NAME  "/cnt.v5"      /* macOS caps shm names at 31 chars */
#define CNT_MAGIC     0x434E5435u    /* "CNT5" */
#define CNT_VERSION   5u             /* pin the FORMAT, not the binary */
#define CNT_MAX_PEERS 16
#define CNT_NAMELEN   16
#define CNT_PATHLEN   256
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
    cnt_peer          peers[CNT_MAX_PEERS];
} cnt_seg;

typedef struct {
    _Atomic uint64_t *cell;      /* <- mode switch lives here */
    _Atomic uint64_t *ops;       /* <- and here. same trick, same pattern. */
    _Atomic uint64_t  own;       /* backing store when alone */
    _Atomic uint64_t  own_ops;
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
