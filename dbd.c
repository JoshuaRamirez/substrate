/* dbd -- the database. Owns the segment's creation, lifetime, persistence,
 * and the peer registry (it reaps rows whose process is gone).
 *
 * Peers mutate the count with an atomic and need no protocol to do so.
 * When the boundary is memory, some operations need no protocol at all.
 */
#include "counter.h"
#include <stdlib.h>

static const char *DBFILE = "counter.db";
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

static void persist(uint64_t v) {
    FILE *f = fopen(DBFILE, "w");
    if (!f) { perror("persist"); return; }
    fprintf(f, "%llu\n", (unsigned long long)v);
    fclose(f);
}

int main(void) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

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
    g_seg->version = CNT_VERSION;
    g_seg->magic   = CNT_MAGIC;   /* magic last: peers see a valid segment or none */

    g_self = cnt_claim_slot(g_seg, "dbd", "owner");

    printf("dbd: owning %s  (format v%u, %zu bytes, %d slots)  restored count=%llu\n",
           CNT_SHM_NAME, CNT_VERSION, sizeof(cnt_seg), CNT_MAX_PEERS,
           (unsigned long long)start);
    fflush(stdout);

    uint64_t last = (uint64_t)-1;
    while (!g_stop) {
        int reaped = cnt_reap(g_seg);
        if (reaped) { printf("dbd: reaped %d dead peer(s)\n", reaped); fflush(stdout); }

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
    printf("\ndbd: stopped, persisted count=%llu\n", (unsigned long long)final);
    return 0;
}
