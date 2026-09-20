/* peer -- the headless twin of gui and top.
 *
 * gui and top are Cocoa, so they exist only on macOS. But everything the test
 * suite asks of them is UI-free: join the segment, bump the cell, count the
 * live rows, read the admin token. That is all substrate, not window.
 *
 * So on a system without Cocoa the same checks run against this instead, and
 * the printed lines are byte-identical on purpose. If the two ever drift, the
 * suite stops testing the same contract on the two platforms, silently -- and
 * this project has already been bitten once by a test that passed while
 * measuring something other than what it claimed.
 */
#include "substrate.h"
#include <stdlib.h>

static substrate C;

int main(int argc, char **argv) {
    int join = 0, selftest = 0, toptest = 0, token = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--join"))     join     = 1;
        if (!strcmp(argv[i], "--selftest")) selftest = 1;
        if (!strcmp(argv[i], "--toptest"))  toptest  = 1;
        if (!strcmp(argv[i], "--token"))    token    = 1;
    }

    /* the top side: read the registry, read the token */
    if (toptest || token) {
        int err = 0;
        sub_seg *seg = sub_attach(&err);
        if (!seg) {
            if (token) { fprintf(stderr, "no owner (err=%d)\n", err); return 2; }
            printf("top selftest: no owner (err=%d)\n", err);
            return 2;
        }
        if (token) {
            if (!seg->admin[0]) { fprintf(stderr, "owner minted no token\n"); return 3; }
            printf("%s\n", seg->admin);
            return 0;
        }
        int live = 0;
        for (int k = 0; k < SUB_MAX_PEERS; k++)
            if (sub_peer_alive(&seg->peers[k])) live++;
        char db[SUB_PATHLEN];
        if (!sub_path_read(seg, db, sizeof db)) snprintf(db, sizeof db, "(unknown)");
        printf("top selftest: peers=%d count=%llu\n", live,
               (unsigned long long)atomic_load(&seg->count));
        printf("top selftest: db=%s\n", db);
        return 0;
    }

    /* the gui side: join (or not), bump once, say which mode you ended up in */
    if (!selftest) {
        fprintf(stderr, "usage: peer --selftest [--join] | --toptest | --token\n");
        return 2;
    }
    if (sub_open(&C, join, "gui", "ui") < 0) return 2;
    uint64_t v = sub_bump(&C);
    printf("gui selftest: mode=%s count=%llu\n", sub_mode(&C),
           (unsigned long long)v);
    sub_close(&C);
    return 0;
}
