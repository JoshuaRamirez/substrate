/* blobbench -- what a payload costs when it is never copied.
 *
 * The ring carries words. A payload cannot go in it, so it goes in the arena
 * and what crosses the boundary is a DESCRIPTOR: a block index and a length.
 *
 * The comparison is deliberately like-for-like: move N bytes from a producer
 * to a consumer and have the consumer actually READ every byte (a checksum,
 * so nothing can be optimised away).
 *
 *   arena    producer copies in once. Consumer is handed an offset into a
 *            page it already has mapped and reads in place. ZERO copies on
 *            the read path, however many readers there are.
 *   socket   producer copies user->kernel, consumer copies kernel->user.
 *            Two copies per transfer, every transfer, per consumer.
 *
 * Two consumers, because the first measurement was misleading on its own:
 *
 *   FULL   the consumer reads every byte. At 1 MB both sides converge, because
 *          both are then bound by memory bandwidth on the read, not by the
 *          transfer. Zero-copy buys nothing if you were going to touch it all
 *          anyway. That is the honest ceiling and it is worth stating.
 *
 *   SPARSE the consumer touches one byte per 4 KB page -- an index probe, a
 *          header, a field. This is where the structural difference lives:
 *          with the arena you pay for what you READ, with a socket you pay for
 *          what was SENT. The socket has to move all N bytes either way.
 */
#include "substrate.h"
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>

static uint64_t sum_bytes(const uint8_t *p, size_t n) {
    uint64_t s = 0;
    for (size_t i = 0; i < n; i++) s += p[i];
    return s;
}

/* One byte per page: what a reader that only wants part of a blob pays. */
static uint64_t probe_bytes(const uint8_t *p, size_t n) {
    uint64_t s = 0;
    for (size_t i = 0; i < n; i += 4096) s += p[i];
    return s;
}

static double now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static void row(const char *what, size_t n, double ns, long it) {
    double mbps = ((double)n / (1024.0 * 1024.0)) / (ns / 1e9);
    printf("  %-26s %9zu B  %10.0f ns  %9.0f MB/s  (%ld)\n", what, n, ns, mbps, it);
    fflush(stdout);
}

/* A child that writes n bytes whenever poked. */
static pid_t spawn_sender(int fd, size_t n, const uint8_t *buf) {
    pid_t p = fork();
    if (p != 0) return p;
    unsigned char go;
    while (read(fd, &go, 1) == 1) {
        size_t off = 0;
        while (off < n) {
            ssize_t w = write(fd, buf + off, n - off);
            if (w <= 0) _exit(0);
            off += (size_t)w;
        }
    }
    _exit(0);
}

static double socket_recv(int fd, uint8_t *dst, size_t n, long iters, uint64_t *chk) {
    unsigned char go = 1;
    double t0 = now_ns();
    for (long i = 0; i < iters; i++) {
        if (write(fd, &go, 1) != 1) break;
        size_t off = 0;
        while (off < n) {
            ssize_t r = read(fd, dst + off, n - off);
            if (r <= 0) break;
            off += (size_t)r;
        }
        *chk += sum_bytes(dst, n);
    }
    return (now_ns() - t0) / (double)iters;
}

int main(int argc, char **argv) {
    int join = 0;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--join")) join = 1;

    substrate C;
    if (sub_open(&C, join, "blobbench", "bench") < 0) return 2;
    if (!C.seg) { fprintf(stderr, "blobbench: needs --join and a running dbd\n"); return 2; }

    size_t sizes[] = { 4096, 65536, 1048576 };
    for (unsigned si = 0; si < sizeof sizes / sizeof *sizes; si++) {
        size_t n = sizes[si];
        long iters = n >= 1048576 ? 2000 : 20000;

        uint8_t *src = malloc(n), *dst = malloc(n);
        if (!src || !dst) return 1;
        for (size_t i = 0; i < n; i++) src[i] = (uint8_t)(i * 31 + si);
        uint64_t want = sum_bytes(src, n), chk = 0;

        if (sub_put(&C, 1000 + si, src, n) != 0) {
            fprintf(stderr, "blobbench: put failed (arena full?)\n");
            return 3;
        }

        /* --- arena: descriptor across, bytes read in place --- */
        double t0 = now_ns();
        for (long i = 0; i < iters; i++) {
            uint64_t len = 0;
            const uint8_t *p = sub_get(&C, 1000 + si, &len);
            if (!p || len != n) { fprintf(stderr, "get failed\n"); return 4; }
            chk += sum_bytes(p, (size_t)len);          /* no copy: read where it lives */
        }
        double arena_ns = (now_ns() - t0) / (double)iters;
        if (chk != want * (uint64_t)iters) { fprintf(stderr, "arena checksum wrong\n"); return 5; }
        row("arena (zero-copy read)", n, arena_ns, iters);

        /* --- AF_UNIX: two copies per transfer --- */
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
            int big = (int)n * 2;
            setsockopt(sv[0], SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
            setsockopt(sv[1], SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
            pid_t kid = spawn_sender(sv[1], n, src);
            close(sv[1]);
            chk = 0;
            double sock_ns = socket_recv(sv[0], dst, n, iters, &chk);
            row("AF_UNIX (two copies)", n, sock_ns, iters);
            printf("  %-26s %8.1fx  (reader touches every byte)\n",
                   "  -> arena advantage", sock_ns / arena_ns);

            /* --- and now a reader that only probes --- */
            uint64_t pw = probe_bytes(src, n), pc = 0;
            double t1 = now_ns();
            for (long i = 0; i < iters; i++) {
                uint64_t len = 0;
                const uint8_t *p = sub_get(&C, 1000 + si, &len);
                if (!p) return 6;
                pc += probe_bytes(p, (size_t)len);
            }
            double arena_probe = (now_ns() - t1) / (double)iters;
            if (pc != pw * (uint64_t)iters) { fprintf(stderr, "probe checksum wrong\n"); return 7; }
            row("arena (sparse probe)", n, arena_probe, iters);

            pc = 0;
            unsigned char go = 1;
            double t2 = now_ns();
            for (long i = 0; i < iters; i++) {
                if (write(sv[0], &go, 1) != 1) break;
                size_t off = 0;
                while (off < n) {
                    ssize_t r = read(sv[0], dst + off, n - off);
                    if (r <= 0) break;
                    off += (size_t)r;
                }
                pc += probe_bytes(dst, n);     /* still had to receive all of it */
            }
            double sock_probe = (now_ns() - t2) / (double)iters;
            row("AF_UNIX (sparse probe)", n, sock_probe, iters);
            printf("  %-26s %8.1fx  (reader probes one byte per page)\n",
                   "  -> arena advantage", sock_probe / arena_probe);

            close(sv[0]);
            kill(kid, SIGKILL); waitpid(kid, NULL, 0);
        }
        free(src); free(dst);
        printf("\n");
    }
    sub_close(&C);
    return 0;
}
