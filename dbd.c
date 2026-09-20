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
#include "substrate.h"
#include <stdlib.h>

static char      DBFILE[SUB_PATHLEN] = "substrate.db";
static sub_seg  *g_seg  = NULL;
static sub_peer *g_self = NULL;
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
    char tmp[SUB_PATHLEN + 8];
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
        sub_path_write(g_seg, DBFILE);
        fflush(stdout);
        return;
    }
    persist(now);                                  /* flush the file we are leaving */
    snprintf(DBFILE, sizeof DBFILE, "%s", want);
    printf("dbd: database -> %s  (count=%llu)\n", DBFILE, (unsigned long long)now);
    fflush(stdout);
}

/* Two operations an installer needs, before dbd becomes a server.
 *
 * Uninstalling has to be able to release the segment. The shm name outlives
 * every process that ever mapped it, so deleting the binaries alone would
 * leave /sub.v8 behind until the machine reboots, and the next install would
 * inherit a stranger's page. */
static int do_stop(void) {
    int err = 0;
    sub_seg *s = sub_attach(&err);
    if (!s) { printf("dbd: no segment on %s\n", SUB_SHM_NAME); return 0; }
    pid_t who = (pid_t)atomic_load(&s->owner_pid);
    if (!sub_owner_alive(s)) {
        munmap(s, sizeof *s);
        printf("dbd: no live owner (last was pid %d)\n", (int)who);
        return 0;
    }
    if (kill(who, SIGTERM) != 0) {
        munmap(s, sizeof *s);
        perror("dbd: kill");
        return 1;
    }
    /* Note who else is on the page BEFORE the owner leaves and the table
     * goes with it. These processes do not die -- they detach and keep
     * running against private memory, which is the whole point of the design
     * but a surprise if an uninstall does not say so out loud. */
    struct { pid_t pid; char name[SUB_NAMELEN]; } others[SUB_MAX_PEERS];
    int n_others = 0;
    for (int i = 0; i < SUB_MAX_PEERS; i++) {
        sub_peer *p = &s->peers[i];
        if (!sub_peer_alive(p)) continue;
        pid_t pp = (pid_t)atomic_load(&p->pid);
        if (pp == who) continue;
        others[n_others].pid = pp;
        snprintf(others[n_others].name, sizeof others[n_others].name, "%s", p->name);
        n_others++;
    }

    printf("dbd: SIGTERM -> owner pid %d, waiting\n", (int)who);
    /* Let it leave on its own feet. Its own exit path is the only one that
     * persists the count and unlinks the name; killing it harder loses both. */
    int live = 1;
    for (int i = 0; i < 100 && live; i++) {        /* up to 10s */
        usleep(100000);
        live = sub_owner_alive(s);
    }
    munmap(s, sizeof *s);
    if (live) { fprintf(stderr, "dbd: owner pid %d did not exit\n", (int)who); return 1; }
    printf("dbd: owner stopped\n");
    if (n_others > 0) {
        printf("dbd: %d peer(s) are still running, now detached "
               "(each keeps counting in its own memory):\n", n_others);
        for (int i = 0; i < n_others; i++)
            printf("       pid %-7d %s\n", (int)others[i].pid, others[i].name);
    }
    return 0;
}

static int do_unlink(void) {
    int err = 0;
    sub_seg *s = sub_attach(&err);
    if (s) {
        int live = sub_owner_alive(s);
        pid_t who = (pid_t)atomic_load(&s->owner_pid);
        munmap(s, sizeof *s);
        /* Unlinking removes the NAME, not the MAPPING. Doing it under a live
         * owner is the split brain this project already paid for once: the
         * owner and its peers keep using a page nobody can look up, and the
         * next dbd creates a second world beside it. Refuse. */
        if (live) {
            fprintf(stderr, "dbd: refusing to unlink %s -- owner pid %d is alive "
                            "(--stop it first)\n", SUB_SHM_NAME, (int)who);
            return 3;
        }
    }
    if (shm_unlink(SUB_SHM_NAME) == 0)  printf("dbd: unlinked %s\n", SUB_SHM_NAME);
    else if (errno == ENOENT)           printf("dbd: %s already gone\n", SUB_SHM_NAME);
    else { perror("dbd: shm_unlink"); return 1; }
    return 0;
}

int main(int argc, char **argv) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--stop"))   return do_stop();
        if (!strcmp(argv[i], "--unlink")) return do_unlink();
    }

    /* Where to persist: argv, then the environment, then the old default.
     * Resolved to absolute here, because the segment is read by processes
     * whose working directory is not ours. */
    const char *want = NULL;
    for (int i = 1; i < argc; i++) if (argv[i][0] != '-') { want = argv[i]; break; }
    if (!want) want = getenv("SUBSTRATE_DB");
    if (!want) want = "substrate.db";
    char abs[SUB_PATHLEN];
    sub_abspath(want, abs, sizeof abs);
    snprintf(DBFILE, sizeof DBFILE, "%s", abs);

    /* Clear any stale segment: on macOS an shm object can only be ftruncate'd
     * once in its life, so the owner must start from a fresh one. */
    /* Never displace a live owner. Unlinking removes the NAME, not the MAPPING:
     * a second dbd used to take the name while every existing peer kept using
     * the old page, and the two worlds diverged in silence. Check first. */
    {
        int err = 0;
        sub_seg *existing = sub_attach(&err);
        if (existing) {
            int live = sub_owner_alive(existing);
            pid_t who = (pid_t)atomic_load(&existing->owner_pid);
            munmap(existing, sizeof *existing);
            if (live) {
                fprintf(stderr, "dbd: refusing to start -- owner already running "
                                "on %s (pid %d)\n", SUB_SHM_NAME, who);
                return 3;
            }
        }
    }
    shm_unlink(SUB_SHM_NAME);

    int fd = shm_open(SUB_SHM_NAME, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { perror("shm_open"); return 1; }
    /* No fchmod here: macOS rejects it on an shm descriptor (EINVAL), and it
     * is not needed -- umask can only CLEAR permission bits, never add them,
     * so 0600 cannot be widened into something group- or world-readable. */
    if (ftruncate(fd, sizeof(sub_seg)) < 0) { perror("ftruncate"); return 1; }

    void *p = mmap(NULL, sizeof(sub_seg), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }

    g_seg = (sub_seg *)p;
    uint64_t start = load_persisted();
    atomic_store(&g_seg->count, start);
    atomic_store(&g_seg->path_seq, 0);
    sub_path_write(g_seg, DBFILE);       /* publish where the data actually goes */
    atomic_store(&g_seg->owner_pid, (uint64_t)getpid());
    atomic_store(&g_seg->path_writer, 0);
    atomic_store(&g_seg->next_id, 1);   /* id 0 means "failed" */
    atomic_store(&g_seg->block_free, ~0ull);   /* every arena block free */
    sub_mint_admin(g_seg);        /* before magic: no peer sees a tokenless segment */
    g_seg->version = SUB_VERSION;
    g_seg->magic   = SUB_MAGIC;   /* magic last: peers see a valid segment or none */

    g_self = sub_claim_slot(g_seg, "dbd", "owner");

    printf("dbd: owning %s  (format v%u, %zu bytes, %d slots)  restored count=%llu\n",
           SUB_SHM_NAME, SUB_VERSION, sizeof(sub_seg), SUB_MAX_PEERS,
           (unsigned long long)start);
    printf("dbd: database %s\n", DBFILE);
    printf("dbd: admin token minted (%s)\n",
           g_seg->admin[0] ? "ok" : "FAILED -- admin routes disabled");
    fflush(stdout);

    /* The ring changes what this process IS. Before the reserve verb, dbd was
     * a housekeeper that woke ten times a second. A verb whose answer a caller
     * blocks on turns it into a server: it must spin. Housekeeping moves onto
     * a slower cadence underneath. That cost is the honest price of a reply. */
    /* Spin hot, then park -- and measure both on a CLOCK.
     *
     * A ring's latency is its owner's polling interval, nothing else: sleeping
     * 500us between polls made a 3ns memory handoff cost 620us, worse than the
     * TCP round trip it exists to beat. So stay hot after work arrives.
     *
     * But "stay hot for 200000 iterations" is not a duration. Each iteration
     * scans 32 slots, so that was 1.7 SECONDS, not the 1ms the comment
     * claimed, and housekeeping fell 17x behind its 100ms budget. Same bug as
     * the client's spin-count timeout, made twice in one file: an iteration
     * count is not a unit of time on any machine you do not own. */
    uint64_t last = (uint64_t)-1;
    int64_t hot_until = 0;
    int64_t next_keep = sub_now_ns();
    while (!g_stop) {
        if (sub_serve_ring(g_seg)) {
            hot_until = sub_now_ns() + 1000000;        /* 1 real millisecond */
            continue;
        }
        int64_t now_ns = sub_now_ns();
        if (now_ns < hot_until) continue;              /* still hot */
        if (now_ns < next_keep) { usleep(200); continue; }
        next_keep = now_ns + 100000000;                /* housekeep at 10 Hz */
        if (sub_seqlock_repair(g_seg)) {
            printf("dbd: repaired a path seqlock left odd by a dead writer\n");
            fflush(stdout);
        }
        sub_reap_ring(g_seg);
        sub_reap_blocks(g_seg);
        int reaped = sub_reap(g_seg);
        if (reaped) { printf("dbd: reaped %d dead peer(s)\n", reaped); fflush(stdout); }

        char asked[SUB_PATHLEN];
        if (sub_path_read(g_seg, asked, sizeof asked) && strcmp(asked, DBFILE) != 0)
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
    sub_release_slot(g_self);
    g_seg->magic = 0;              /* revoke: late joiners must not attach */
    munmap(g_seg, sizeof(sub_seg));
    shm_unlink(SUB_SHM_NAME);
    printf("\ndbd: stopped, persisted count=%llu to %s\n",
           (unsigned long long)final, DBFILE);
    return 0;
}
