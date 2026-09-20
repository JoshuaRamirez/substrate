/* look -- an instance that is complete on its own, data included.
 *
 * It needs no owner, no segment, no file and no network. Run it from an empty
 * directory on a machine with nothing else installed and it still answers.
 */
#include "emdb.h"
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

int main(int argc, char **argv) {
    emdb db;
    if (emdb_open(&db) < 0) { fprintf(stderr, "look: no embedded table\n"); return 2; }

    if (argc < 2 || !strcmp(argv[1], "--stat")) {
        printf("records = %zu\nbytes   = %zu\nfirst   = %s\nlast    = %s\npid     = %d\n",
               db.count, db.bytes,
               emdb_at(&db, 0) ? emdb_at(&db, 0)->key : "-",
               emdb_at(&db, db.count - 1) ? emdb_at(&db, db.count - 1)->key : "-",
               getpid());
        if (argc > 2 && !strcmp(argv[2], "--hold")) {
            /* Touch every page so the whole table is resident, then hold. If
             * each instance had its own copy, N of these would cost N x 40MB. */
            uint64_t sink = 0;
            for (size_t i = 0; i < db.count; i++) sink += db.rec[i].value;
            fprintf(stderr, "%llu\n", (unsigned long long)sink);
            pause();
        }
        return 0;
    }
    if (!strcmp(argv[1], "--bench")) {
        long iters = (argc > 2) ? atol(argv[2]) : 200000;
        struct timespec a, b;
        uint64_t v, sink = 0;

        /* Build the keys first. Formatting them is not the lookup. */
        char *keys = malloc((size_t)iters * EMDB_KEYLEN);
        if (!keys) return 1;
        for (long i = 0; i < iters; i++)
            snprintf(keys + i * EMDB_KEYLEN, EMDB_KEYLEN, "k%09ld",
                     (long)((uint64_t)i * 7919 % db.count));

        clock_gettime(CLOCK_MONOTONIC, &a);
        for (long i = 0; i < iters; i++)
            if (emdb_get(&db, keys + i * EMDB_KEYLEN, &v)) sink += v;
        clock_gettime(CLOCK_MONOTONIC, &b);
        free(keys);
        double ns = ((double)(b.tv_sec - a.tv_sec) * 1e9 + (double)(b.tv_nsec - a.tv_nsec))
                    / (double)iters;
        printf("embedded lookup        %10.1f ns/op   %10.0f op/s   (%ld iters, %zu records)\n",
               ns, 1e9 / ns, iters, db.count);
        return sink == 0;
    }
    uint64_t v = 0;
    if (!emdb_get(&db, argv[1], &v)) { printf("miss\n"); return 1; }
    printf("%llu\n", (unsigned long long)v);
    return 0;
}
