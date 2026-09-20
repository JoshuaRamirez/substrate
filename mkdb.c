/* mkdb -- generate the table that gets linked into the binaries.
 * Fixed-width, sorted by key, so the file IS the memory layout. */
#include "emdb.h"
#include <stdlib.h>

int main(int argc, char **argv) {
    long n = (argc > 1) ? atol(argv[1]) : 1000000;
    const char *out = (argc > 2) ? argv[2] : "db.blob";
    if (n < 1) n = 1;

    FILE *f = fopen(out, "wb");
    if (!f) { perror("open"); return 1; }

    emdb_rec r;
    for (long i = 0; i < n; i++) {
        memset(&r, 0, sizeof r);
        snprintf(r.key, EMDB_KEYLEN, "k%09ld", i);    /* zero-padded: sorted by construction */
        r.value = (uint64_t)i * 2654435761u;          /* something to check against */
        if (fwrite(&r, sizeof r, 1, f) != 1) { perror("write"); fclose(f); return 1; }
    }
    fclose(f);
    printf("mkdb: %ld records, %zu bytes -> %s\n", n, (size_t)n * sizeof(emdb_rec), out);
    return 0;
}
