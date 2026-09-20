/* wedge -- takes the path seqlock odd and dies without releasing it.
 * Exists only so test.sh can prove the owner repairs what a corpse left behind. */
#include "counter.h"
#include <stdlib.h>

int main(void) {
    int err = 0;
    cnt_seg *s = cnt_attach(&err);
    if (!s) { fprintf(stderr, "wedge: no owner\n"); return 2; }
    uint32_t a = atomic_load(&s->path_seq);
    if (a & 1) return 3;
    atomic_store(&s->path_seq, a + 1);            /* odd: a write is "in flight" */
    atomic_store(&s->path_writer, (uint64_t)getpid());
    _exit(0);                                     /* and never finish it */
}
