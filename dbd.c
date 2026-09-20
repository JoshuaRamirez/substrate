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

static int persist_to(const char *file, uint64_t v) {
    FILE *f = fopen(file, "w");
    if (!f) return -1;
    fprintf(f, "%llu\n", (unsigned long long)v);
    fclose(f);
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
    shm_unlink(CNT_SHM_NAME);

    int fd = shm_open(CNT_SHM_NAME, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { perror("shm_open"); return 1; }
    if (ftruncate(fd, sizeof(cnt_seg)) < 0) { perror("ftruncate"); return 1; }

    void *p = mmap(NULL, sizeof(cnt_seg), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }

    g_seg = (cnt_seg *)p;
    uint64_t start = load_persisted();
    atomic_store(&g_seg->count, start);
    atomic_store(&g_seg->path_seq, 0);
    cnt_path_write(g_seg, DBFILE);       /* publish where the data actually goes */
    g_seg->version = CNT_VERSION;
    g_seg->magic   = CNT_MAGIC;   /* magic last: peers see a valid segment or none */

    g_self = cnt_claim_slot(g_seg, "dbd", "owner");

    printf("dbd: owning %s  (format v%u, %zu bytes, %d slots)  restored count=%llu\n",
           CNT_SHM_NAME, CNT_VERSION, sizeof(cnt_seg), CNT_MAX_PEERS,
           (unsigned long long)start);
    printf("dbd: database %s\n", DBFILE);
    fflush(stdout);

    uint64_t last = (uint64_t)-1;
    while (!g_stop) {
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
        usleep(100000);
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
