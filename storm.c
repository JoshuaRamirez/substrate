/* storm -- contention at scale, and whether backpressure is real.
 *
 * Forks N client processes onto a 32-slot ring and makes them all reserve at
 * once. Two questions, and only one of them is about speed:
 *
 *   CORRECTNESS: with N clients racing, is every id handed out exactly once?
 *   The union of everything every client received must be precisely
 *   1..total with no duplicate and no gap. A lock-free ring that is subtly
 *   wrong shows up here and nowhere else.
 *
 *   BACKPRESSURE: with more clients than slots, does an overload WAIT or does
 *   it LOSE? A full ring is a full queue, not a failure, and the difference
 *   is whether a request comes back late or never comes back at all.
 */
#include "counter.h"
#include <stdlib.h>
#include <sys/wait.h>

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv) {
    int  clients = (argc > 1) ? atoi(argv[1]) : 64;
    long per     = (argc > 2) ? atol(argv[2]) : 2000;
    if (clients < 1) clients = 1;
    if (per < 1) per = 1;

    printf("storm: %d client processes x %ld reserves, on %d ring slots\n",
           clients, per, CNT_RING_SLOTS);
    fflush(stdout);

    int64_t t0 = cnt_now_ns();
    for (int i = 0; i < clients; i++) {
        if (fork() == 0) {
            counter C;
            if (counter_open(&C, 1, "storm", "load") < 0) _exit(9);   /* parent counts these */

            uint64_t *got = malloc((size_t)per * sizeof(uint64_t));
            int64_t  *lat = malloc((size_t)per * sizeof(int64_t));
            if (!got || !lat) _exit(8);

            long lost = 0;
            for (long k = 0; k < per; k++) {
                int64_t a = cnt_now_ns();
                uint64_t id = counter_reserve(&C, 1);
                lat[k] = cnt_now_ns() - a;
                got[k] = id;
                if (id == 0) lost++;             /* dropped: backpressure failed */
            }
            qsort(lat, (size_t)per, sizeof(int64_t), cmp_u64);

            char f[64];
            snprintf(f, sizeof f, "/tmp/storm.%d.bin", i);
            FILE *fp = fopen(f, "wb");
            if (fp) { fwrite(got, sizeof(uint64_t), (size_t)per, fp); fclose(fp); }

            snprintf(f, sizeof f, "/tmp/storm.%d.txt", i);
            fp = fopen(f, "w");
            if (fp) {
                fprintf(fp, "%ld %lld %lld %lld\n", lost,
                        (long long)lat[per / 2],
                        (long long)lat[(per * 99) / 100],
                        (long long)lat[per - 1]);
                fclose(fp);
            }
            counter_close(&C);
            _exit(0);
        }
    }
    int joined = 0, failed = 0;
    for (int i = 0; i < clients; i++) {
        int st = 0;
        wait(&st);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0) joined++; else failed++;
    }
    double secs = (double)(cnt_now_ns() - t0) / 1e9;

    /* ---- did every id come out exactly once? ---- */
    size_t total = (size_t)clients * (size_t)per;
    uint64_t *all = malloc(total * sizeof(uint64_t));
    if (!all) return 1;
    size_t n = 0;
    long lost = 0;
    long long p50 = 0, p99 = 0, pmax = 0;

    for (int i = 0; i < clients; i++) {
        char f[64];
        snprintf(f, sizeof f, "/tmp/storm.%d.bin", i);
        FILE *fp = fopen(f, "rb");
        if (fp) { n += fread(all + n, sizeof(uint64_t), (size_t)per, fp); fclose(fp); }
        unlink(f);

        snprintf(f, sizeof f, "/tmp/storm.%d.txt", i);
        fp = fopen(f, "r");
        if (fp) {
            long l; long long a, b, c;
            if (fscanf(fp, "%ld %lld %lld %lld", &l, &a, &b, &c) == 4) {
                lost += l;
                p50 += a;
                if (b > p99) p99 = b;
                if (c > pmax) pmax = c;
            }
            fclose(fp);
        }
        unlink(f);
    }

    qsort(all, n, sizeof(uint64_t), cmp_u64);
    long dupes = 0, gaps = 0, zeros = 0;
    for (size_t i = 0; i < n; i++) {
        if (all[i] == 0) { zeros++; continue; }
        if (i && all[i] == all[i - 1]) dupes++;
        if (i && all[i] > all[i - 1] + 1) gaps++;
    }

    printf("  clients that joined %d of %d\n", joined, clients);
    printf("  clients that failed %d\n", failed);
    printf("  ids handed out      %zu\n", n - (size_t)zeros);
    printf("  duplicates          %ld\n", dupes);
    printf("  gaps in the range   %ld\n", gaps);
    printf("  dropped requests    %ld\n", lost);
    printf("  wall time           %.2f s\n", secs);
    printf("  throughput          %.0f reserves/s\n", (double)total / secs);
    printf("  latency p50 (avg)   %lld ns\n", p50 / clients);
    printf("  latency p99 (worst) %lld ns\n", p99);
    printf("  latency max         %lld ns\n", pmax);
    free(all);
    /* A client that could not even join is not a pass. */
    return (dupes || gaps || lost || zeros || failed || n != total) ? 1 : 0;
}
