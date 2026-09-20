/* dbd -- the database. Owns the segment's creation, lifetime, persistence,
 * and the peer registry (it reaps rows whose process is gone).
 *
 * Peers mutate the count with an atomic and need no protocol to do so.
 * When the boundary is memory, some operations need no protocol at all.
 *
 * Where it persists is itself shared state. The path lives in the segment, so
 * any peer can move the database while everything is running: write the path,
 * and the owner picks it up on its next tick. Save As, not Open -- the count
 * keeps counting; only its destination changes.
 */
#include "counter.h"
#include <stdlib.h>

static char      DBFILE[CNT_PATHLEN] = "counter.db";
static cnt_seg  *g_seg  = NULL;
static cnt_peer *g_self = NULL;
static volatile sig_atomic_t g_stop = 0;

static void on_signal(int _) { (void)_; g_stop = 1; }

static uint64_t load_persisted(void) {
    FILE *f = fopen(DBFILE, "r");
    if (!f) return 0;
    unsigned long long v = 0;
    if (fscanf(f, "%llu", &v) != 1) v = 0;
    fclose(f);
    return (uint64_t)v;
}

/* Write, flush, fsync, then rename. A crash mid-write used to leave a
 * truncated file that reads back as 0 -- silently losing the count it was
 * supposed to protect. rename(2) is atomic on the same filesystem. */
static int persist_to(const char *file, uint64_t v) {
    char tmp[CNT_PATHLEN + 8];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", file) >= sizeof tmp) return -1;
    FILE *f = fopen(tmp, "w");
    if (!f) return -1;
    if (fprintf(f, "%llu\n", (unsigned long long)v) < 0) { fclose(f); unlink(tmp); return -1; }
    if (fflush(f) != 0)             { fclose(f); unlink(tmp); return -1; }
    if (fsync(fileno(f)) != 0)      { fclose(f); unlink(tmp); return -1; }
    if (fclose(f) != 0)             { unlink(tmp); return -1; }
    if (rename(tmp, file) != 0)     { unlink(tmp); return -1; }
    return 0;
}

static void persist(uint64_t v) {
    if (persist_to(DBFILE, v) < 0) perror("persist");
}

/* A peer asked for a different file. Prove the new one is writable BEFORE
 * committing to it: a refused move leaves the database exactly where it was,
 * and the segment is corrected so the dashboard never shows a lie. */
static void retarget(const char *want) {
    uint64_t now = atomic_load(&g_seg->count);
    if (persist_to(want, now) < 0) {
        printf("dbd: refused %s (%s) -- staying on %s\n",
               want, strerror(errno), DBFILE);
        cnt_path_write(g_seg, DBFILE);
        fflush(stdout);
        return;
    }
    persist(now);                                  /* flush the file we are leaving */
    snprintf(DBFILE, sizeof DBFILE, "%s", want);
    printf("dbd: database -> %s  (count=%llu)\n", DBFILE, (unsigned long long)now);
    fflush(stdout);
}

int main(int argc, char **argv) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* Where to persist: argv, then the environment, then the old default.
     * Resolved to absolute here, because the segment is read by processes
     * whose working directory is not ours. */
    const char *want = NULL;
    for (int i = 1; i < argc; i++) if (argv[i][0] != '-') { want = argv[i]; break; }
    if (!want) want = getenv("COUNTER_DB");
    if (!want) want = "counter.db";
    char abs[CNT_PATHLEN];
    cnt_abspath(want, abs, sizeof abs);
    snprintf(DBFILE, sizeof DBFILE, "%s", abs);

    /* Clear any stale segment: on macOS an shm object can only be ftruncate'd
     * once in its life, so the owner must start from a fresh one. */
    /* Never displace a live owner. Unlinking removes the NAME, not the MAPPING:
     * a second dbd used to take the name while every existing peer kept using
     * the old page, and the two worlds diverged in silence. Check first. */
    {
        int err = 0;
        cnt_seg *existing = cnt_attach(&err);
        if (existing) {
            int live = cnt_owner_alive(existing);
            pid_t who = (pid_t)atomic_load(&existing->owner_pid);
            munmap(existing, sizeof *existing);
            if (live) {
                fprintf(stderr, "dbd: refusing to start -- owner already running "
                                "on %s (pid %d)\n", CNT_SHM_NAME, who);
                return 3;
            }
        }
    }
    shm_unlink(CNT_SHM_NAME);

    int fd = shm_open(CNT_SHM_NAME, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { perror("shm_open"); return 1; }
    /* No fchmod here: macOS rejects it on an shm descriptor (EINVAL), and it
     * is not needed -- umask can only CLEAR permission bits, never add them,
     * so 0600 cannot be widened into something group- or world-readable. */
    if (ftruncate(fd, sizeof(cnt_seg)) < 0) { perror("ftruncate"); return 1; }

    void *p = mmap(NULL, sizeof(cnt_seg), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }

    g_seg = (cnt_seg *)p;
    uint64_t start = load_persisted();
    atomic_store(&g_seg->count, start);
    atomic_store(&g_seg->path_seq, 0);
    cnt_path_write(g_seg, DBFILE);       /* publish where the data actually goes */
    atomic_store(&g_seg->owner_pid, (uint64_t)getpid());
    atomic_store(&g_seg->path_writer, 0);
    atomic_store(&g_seg->next_id, 1);   /* id 0 means "failed" */
    cnt_mint_admin(g_seg);        /* before magic: no peer sees a tokenless segment */
    g_seg->version = CNT_VERSION;
    g_seg->magic   = CNT_MAGIC;   /* magic last: peers see a valid segment or none */

    g_self = cnt_claim_slot(g_seg, "dbd", "owner");

    printf("dbd: owning %s  (format v%u, %zu bytes, %d slots)  restored count=%llu\n",
           CNT_SHM_NAME, CNT_VERSION, sizeof(cnt_seg), CNT_MAX_PEERS,
           (unsigned long long)start);
    printf("dbd: database %s\n", DBFILE);
    printf("dbd: admin token minted (%s)\n",
           g_seg->admin[0] ? "ok" : "FAILED -- admin routes disabled");
    fflush(stdout);

    /* The ring changes what this process IS. Before the reserve verb, dbd was
     * a housekeeper that woke ten times a second. A verb whose answer a caller
     * blocks on turns it into a server: it must spin. Housekeeping moves onto
     * a slower cadence underneath. That cost is the honest price of a reply. */
    uint64_t last = (uint64_t)-1;
    int tick = 0;
    long idle = 0;
    while (!g_stop) {
        /* Spin hot, then park. A ring's latency is its owner's polling
         * interval, nothing else: sleeping 500us between polls made a
         * 3-nanosecond memory handoff cost 620 MICROseconds -- worse than the
         * TCP round trip it was supposed to beat. So stay hot while work is
         * arriving, and only back off once the ring has been quiet a while. */
        if (cnt_serve_ring(g_seg)) { idle = 0; continue; }
        if (idle < 200000) { idle++; continue; }       /* ~1ms of hot spinning */
        if (++tick < 100) { usleep(1000); continue; }  /* then park, 100ms cycle */
        tick = 0;
        if (cnt_seqlock_repair(g_seg)) {
            printf("dbd: repaired a path seqlock left odd by a dead writer\n");
            fflush(stdout);
        }
        cnt_reap_ring(g_seg);
        int reaped = cnt_reap(g_seg);
        if (reaped) { printf("dbd: reaped %d dead peer(s)\n", reaped); fflush(stdout); }

        char asked[CNT_PATHLEN];
        if (cnt_path_read(g_seg, asked, sizeof asked) && strcmp(asked, DBFILE) != 0)
            retarget(asked);

        uint64_t now = atomic_load(&g_seg->count);
        if (now != last) {
            printf("dbd: count=%llu\n", (unsigned long long)now);
            fflush(stdout);
            persist(now);
            last = now;
        }
    }

    uint64_t final = atomic_load(&g_seg->count);
    persist(final);
    cnt_release_slot(g_self);
    g_seg->magic = 0;              /* revoke: late joiners must not attach */
    munmap(g_seg, sizeof(cnt_seg));
    shm_unlink(CNT_SHM_NAME);
    printf("\ndbd: stopped, persisted count=%llu to %s\n",
           (unsigned long long)final, DBFILE);
    return 0;
}
