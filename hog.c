/* hog -- claims every ring slot and dies without completing any of them.
 * Exists so test.sh can prove the owner reclaims a dead caller's slots. */
#include "counter.h"
#include <stdlib.h>

int main(void) {
    int err = 0;
    cnt_seg *s = cnt_attach(&err);
    if (!s) { fprintf(stderr, "hog: no owner\n"); return 2; }
    for (int i = 0; i < CNT_RING_SLOTS; i++) {
        uint32_t expect = CNT_SLOT_FREE;
        if (atomic_compare_exchange_strong(&s->ring[i].state, &expect, CNT_SLOT_REQUEST)) {
            atomic_store(&s->ring[i].client, (uint64_t)getpid());
            atomic_store(&s->ring[i].arg, 0);      /* never served, never freed */
        }
    }
    _exit(0);
}
