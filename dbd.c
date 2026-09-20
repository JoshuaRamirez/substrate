/* dbd -- the database. Owns the segment's creation, lifetime and persistence.
 *
 * It does exactly one thing: it is the single process that decides the shared
 * cell exists, what it starts at, and where it goes when the pipeline stops.
 * Peers mutate it with an atomic and need no protocol to do so -- which is the
 * point: when the boundary is memory, some operations need no protocol at all.
 */
#include "counter.h"
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>

static const char *DBFILE = "counter.db";
static cnt_seg *g_seg = NULL;
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

    printf("dbd: owning %s  (format v%u, %zu bytes)  restored count=%llu\n",
           CNT_SHM_NAME, CNT_VERSION, sizeof(cnt_seg), (unsigned long long)start);
    fflush(stdout);

    uint64_t last = (uint64_t)-1;
    while (!g_stop) {
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
    g_seg->magic = 0;              /* revoke: late joiners must not attach */
    munmap(g_seg, sizeof(cnt_seg));
    shm_unlink(CNT_SHM_NAME);
    printf("\ndbd: stopped, persisted count=%llu\n", (unsigned long long)final);
    return 0;
}
