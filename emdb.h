/* emdb.h -- the database that lives INSIDE the executable.
 *
 * The other half of the original idea: an instance that carries everything it
 * needs, including its data, and loads it all into memory with no file to
 * find, no server to reach, and no parse step.
 *
 * The table is linked into the binary's own read-only __TEXT segment
 * (-sectcreate). Three consequences, and they are the whole point:
 *
 *   1. dyld maps it when the process starts. "Loading the database" is not an
 *      operation. There is no open(), no read(), no deserialise. The records
 *      are already addressable memory the instant main() runs.
 *
 *   2. __TEXT is read-only and file-backed, so N running copies of the same
 *      binary share ONE physical copy of the table, deduplicated by the OS
 *      page cache for free. No coordination, no server, no shared segment.
 *      This is why "every instance loads the whole database" is affordable.
 *
 *   3. Only the pages actually touched are ever faulted in. A binary search
 *      over 40 MB reads about twenty pages.
 *
 * The on-disk form IS the in-memory form: fixed-width records, sorted by key.
 * Lookup is a binary search over the mapped bytes. Nothing is copied.
 */
#ifndef EMDB_H
#define EMDB_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>

#define EMDB_KEYLEN 32

typedef struct {              /* 40 bytes, fixed width, sorted by key */
    char     key[EMDB_KEYLEN];
    uint64_t value;
} emdb_rec;

typedef struct {
    const emdb_rec *rec;
    size_t          count;
    size_t          bytes;
} emdb;

/* No I/O happens here. The section is already mapped; this just finds it. */
static inline int emdb_open(emdb *db) {
    unsigned long sz = 0;
    const uint8_t *p = getsectiondata(&_mh_execute_header, "__TEXT", "__emdb", &sz);
    if (!p || sz < sizeof(emdb_rec)) { db->rec = NULL; db->count = 0; db->bytes = 0; return -1; }
    db->rec   = (const emdb_rec *)(const void *)p;
    db->count = sz / sizeof(emdb_rec);
    db->bytes = sz;
    return 0;
}

static inline int emdb_get(const emdb *db, const char *key, uint64_t *out) {
    if (!db->rec) return 0;
    size_t lo = 0, hi = db->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strncmp(key, db->rec[mid].key, EMDB_KEYLEN);
        if (c == 0) { if (out) *out = db->rec[mid].value; return 1; }
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return 0;
}

static inline const emdb_rec *emdb_at(const emdb *db, size_t i) {
    return (db->rec && i < db->count) ? &db->rec[i] : NULL;
}

#endif
