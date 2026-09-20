/* layout -- print the binary contract of the shared segment.
 *
 * Four programs in three languages must agree on these numbers. Nothing else
 * is shared between them: no library, no runtime, no headers. This output IS
 * the interface, and it is what a sub.v8(5) man page would contain.
 */
#include "substrate.h"
#include <stddef.h>

static void f(const char *name, size_t off, size_t size) {
    printf("%-14s %6zu %6zu\n", name, off, size);
}

int main(void) {
    printf("shm        %s\nmagic      0x%08X\nversion    %u\nsize       %zu\n\n",
           SUB_SHM_NAME, SUB_MAGIC, SUB_VERSION, sizeof(sub_seg));
    printf("%-14s %6s %6s\n", "FIELD", "OFF", "SIZE");
    f("magic",       offsetof(sub_seg, magic),       sizeof(uint32_t));
    f("version",     offsetof(sub_seg, version),     sizeof(uint32_t));
    f("count",       offsetof(sub_seg, count),       sizeof(uint64_t));
    f("owner_pid",   offsetof(sub_seg, owner_pid),   sizeof(uint64_t));
    f("path_seq",    offsetof(sub_seg, path_seq),    sizeof(uint32_t));
    f("path_writer", offsetof(sub_seg, path_writer), sizeof(uint64_t));
    f("path",        offsetof(sub_seg, path),        SUB_PATHLEN);
    f("admin",       offsetof(sub_seg, admin),       SUB_TOKLEN);
    f("next_id",     offsetof(sub_seg, next_id),     sizeof(uint64_t));
    f("block_free",  offsetof(sub_seg, block_free),  sizeof(uint64_t));
    f("block_owner", offsetof(sub_seg, block_owner), sizeof(uint64_t) * SUB_BLOCKS);
    f("blobs",       offsetof(sub_seg, blobs),       sizeof(sub_blob) * SUB_KEYS);
    f("ring",        offsetof(sub_seg, ring),        sizeof(sub_slot) * SUB_RING_SLOTS);
    f("arena",       offsetof(sub_seg, arena),       SUB_ARENA_SIZE);
    f("peers",       offsetof(sub_seg, peers),       sizeof(sub_peer) * SUB_MAX_PEERS);
    printf("\narena          %zu blocks x %d bytes = %zu\n",
           (size_t)SUB_BLOCKS, SUB_BLOCK_SIZE, SUB_ARENA_SIZE);
    printf("\nslot           %6zu bytes  x%d\n", sizeof(sub_slot), SUB_RING_SLOTS);
    f("  .state",  offsetof(sub_slot, state),  sizeof(uint32_t));
    f("  .op",     offsetof(sub_slot, op),     sizeof(uint32_t));
    f("  .client", offsetof(sub_slot, client), sizeof(uint64_t));
    f("  .arg",    offsetof(sub_slot, arg),    sizeof(uint64_t));
    f("  .result", offsetof(sub_slot, result), sizeof(uint64_t));
    f("  .len",    offsetof(sub_slot, len),    sizeof(uint64_t));
    f("  .block",  offsetof(sub_slot, block),  sizeof(uint64_t));
    f("  .err",    offsetof(sub_slot, err),    sizeof(uint32_t));
    printf("\npeer           %6zu bytes  x%d\n", sizeof(sub_peer), SUB_MAX_PEERS);
    f("  .state", offsetof(sub_peer, state), sizeof(uint32_t));
    f("  .pid",   offsetof(sub_peer, pid),   sizeof(uint64_t));
    f("  .ops",   offsetof(sub_peer, ops),   sizeof(uint64_t));
    f("  .since", offsetof(sub_peer, since), sizeof(uint64_t));
    f("  .name",  offsetof(sub_peer, name),  SUB_NAMELEN);
    f("  .role",  offsetof(sub_peer, role),  SUB_NAMELEN);
    return 0;
}
