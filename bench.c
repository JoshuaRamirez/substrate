/* bench -- the numbers the first design promised and never produced.
 *
 * Four things, same machine, same moment:
 *   bump alone       a store into private memory
 *   bump joined      a store into a page shared with other processes
 *   reserve alone    an add
 *   reserve joined   a ring round trip: publish, spin, read
 *   AF_UNIX rtt      the cheapest socket round trip the OS offers
 *   TCP loopback rtt what "just use HTTP on localhost" actually costs
 *
 * The first four are the claim. The last two are what the claim is against.
 */
#include "substrate.h"
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

static double now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static void row(const char *what, double ns, long iters) {
    printf("  %-22s %10.1f ns/op   %10.0f op/s   (%ld iters)\n",
           what, ns, 1e9 / ns, iters);
    fflush(stdout);
}

/* An echo child on an already-connected socketpair-style fd. */
static pid_t spawn_echo(int fd) {
    pid_t p = fork();
    if (p != 0) return p;
    unsigned char b;
    while (read(fd, &b, 1) == 1) { if (write(fd, &b, 1) != 1) break; }
    _exit(0);
}

static double rtt_over(int fd, long iters) {
    unsigned char b = 1, r;
    double t0 = now_ns();
    for (long i = 0; i < iters; i++) {
        if (write(fd, &b, 1) != 1) break;
        if (read(fd, &r, 1) != 1) break;
    }
    return (now_ns() - t0) / (double)iters;
}

int main(int argc, char **argv) {
    int join = 0;
    long N = 2000000, R = 20000, S = 20000;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--join")) join = 1;

    substrate C;
    if (sub_open(&C, join, "bench", "bench") < 0) return 2;
    printf("bench: mode=%s\n", sub_mode(&C));

    /* --- the shared-memory side --- */
    double t0 = now_ns();
    for (long i = 0; i < N; i++) sub_bump(&C);
    row(join ? "bump joined" : "bump alone", (now_ns() - t0) / (double)N, N);

    t0 = now_ns();
    for (long i = 0; i < R; i++) if (!sub_reserve(&C, 1)) { R = i ? i : 1; break; }
    row(join ? "reserve joined (ring)" : "reserve alone", (now_ns() - t0) / (double)R, R);

    /* --- what it is being compared against --- */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
        pid_t kid = spawn_echo(sv[1]);
        close(sv[1]);
        row("AF_UNIX round trip", rtt_over(sv[0], S), S);
        close(sv[0]);
        kill(kid, SIGKILL); waitpid(kid, NULL, 0);
    }

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    socklen_t al = sizeof a;
    if (srv >= 0 && bind(srv, (struct sockaddr *)&a, sizeof a) == 0 &&
        listen(srv, 1) == 0 && getsockname(srv, (struct sockaddr *)&a, &al) == 0) {
        int cli = socket(AF_INET, SOCK_STREAM, 0);
        int yes = 1; setsockopt(cli, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
        if (connect(cli, (struct sockaddr *)&a, sizeof a) == 0) {
            int acc = accept(srv, NULL, NULL);
            setsockopt(acc, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof yes);
            pid_t kid = spawn_echo(acc);
            close(acc);
            row("TCP loopback round trip", rtt_over(cli, S), S);
            kill(kid, SIGKILL); waitpid(kid, NULL, 0);
        }
        close(cli); close(srv);
    }

    sub_close(&C);
    return 0;
}
