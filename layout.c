/* layout -- print the binary contract of the shared segment.
 *
 * Four programs in three languages must agree on these numbers. Nothing else
 * is shared between them: no library, no runtime, no headers. This output IS
 * the interface, and it is what a cnt.v6(5) man page would contain.
 */
#include "counter.h"
#include <stddef.h>

static void f(const char *name, size_t off, size_t size) {
    printf("%-14s %6zu %6zu\n", name, off, size);
}

int main(void) {
    printf("shm        %s\nmagic      0x%08X\nversion    %u\nsize       %zu\n\n",
           CNT_SHM_NAME, CNT_MAGIC, CNT_VERSION, sizeof(cnt_seg));
    printf("%-14s %6s %6s\n", "FIELD", "OFF", "SIZE");
    f("magic",       offsetof(cnt_seg, magic),       sizeof(uint32_t));
    f("version",     offsetof(cnt_seg, version),     sizeof(uint32_t));
    f("count",       offsetof(cnt_seg, count),       sizeof(uint64_t));
    f("owner_pid",   offsetof(cnt_seg, owner_pid),   sizeof(uint64_t));
    f("path_seq",    offsetof(cnt_seg, path_seq),    sizeof(uint32_t));
    f("path_writer", offsetof(cnt_seg, path_writer), sizeof(uint64_t));
    f("path",        offsetof(cnt_seg, path),        CNT_PATHLEN);
    f("admin",       offsetof(cnt_seg, admin),       CNT_TOKLEN);
    f("next_id",     offsetof(cnt_seg, next_id),     sizeof(uint64_t));
    f("block_free",  offsetof(cnt_seg, block_free),  sizeof(uint64_t));
    f("block_owner", offsetof(cnt_seg, block_owner), sizeof(uint64_t) * CNT_BLOCKS);
    f("blobs",       offsetof(cnt_seg, blobs),       sizeof(cnt_blob) * CNT_KEYS);
    f("ring",        offsetof(cnt_seg, ring),        sizeof(cnt_slot) * CNT_RING_SLOTS);
    f("arena",       offsetof(cnt_seg, arena),       CNT_ARENA_SIZE);
    f("peers",       offsetof(cnt_seg, peers),       sizeof(cnt_peer) * CNT_MAX_PEERS);
    printf("\narena          %zu blocks x %d bytes = %zu\n",
           (size_t)CNT_BLOCKS, CNT_BLOCK_SIZE, CNT_ARENA_SIZE);
    printf("\nslot           %6zu bytes  x%d\n", sizeof(cnt_slot), CNT_RING_SLOTS);
    f("  .state",  offsetof(cnt_slot, state),  sizeof(uint32_t));
    f("  .op",     offsetof(cnt_slot, op),     sizeof(uint32_t));
    f("  .client", offsetof(cnt_slot, client), sizeof(uint64_t));
    f("  .arg",    offsetof(cnt_slot, arg),    sizeof(uint64_t));
    f("  .result", offsetof(cnt_slot, result), sizeof(uint64_t));
    f("  .len",    offsetof(cnt_slot, len),    sizeof(uint64_t));
    f("  .block",  offsetof(cnt_slot, block),  sizeof(uint64_t));
    f("  .err",    offsetof(cnt_slot, err),    sizeof(uint32_t));
    printf("\npeer           %6zu bytes  x%d\n", sizeof(cnt_peer), CNT_MAX_PEERS);
    f("  .state", offsetof(cnt_peer, state), sizeof(uint32_t));
    f("  .pid",   offsetof(cnt_peer, pid),   sizeof(uint64_t));
    f("  .ops",   offsetof(cnt_peer, ops),   sizeof(uint64_t));
    f("  .since", offsetof(cnt_peer, since), sizeof(uint64_t));
    f("  .name",  offsetof(cnt_peer, name),  CNT_NAMELEN);
    f("  .role",  offsetof(cnt_peer, role),  CNT_NAMELEN);
    return 0;
}
