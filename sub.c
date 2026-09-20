/* sub -- one command for the things you run on this machine.
 *
 * The peer table was built to show which of THIS project's processes were on
 * the page. It is, without changing a byte, a process registry: 256 rows of
 * {pid, name, role, since, ops}, reaped by the owner the moment a pid stops
 * answering kill(pid,0). Liveness is not a heartbeat anyone has to remember to
 * send -- it is the OS answering a question. That is the whole reuse.
 *
 * The one thing missing was that nothing else on this machine is written in C,
 * so nothing else can include this header and claim a row. A wrapper closes
 * that: `sub run` claims the row on the child's behalf and writes the CHILD's
 * pid into it, so the row tracks the service, not the supervisor. Kill the
 * wrapper and the row survives, because the thing it names is still running.
 * Kill the service and the row is reaped, because the thing it names is not.
 *
 *   sub run --name api -- node server.js
 *   sub ps
 *   sub stop api
 *
 * No format change. This reads and writes only fields v8 already had, which
 * is why pypeer.py and swiftpeer.swift keep working without being told.
 */
#include "substrate.h"
#include <sys/wait.h>
#include <stdlib.h>

static substrate C;
static pid_t g_child = 0;

/* Forward what we are told to the thing we are supervising. The wrapper is a
 * conduit, not a policy: it does not decide that SIGTERM means stop. */
static void on_sig(int s) {
    if (g_child > 0) kill(g_child, s);
}

static void fmt_age(uint64_t s, char *out, size_t cap) {
    unsigned long long v = (unsigned long long)s;
    if      (v < 60)    snprintf(out, cap, "%llus", v);
    else if (v < 3600)  snprintf(out, cap, "%llum%02llus", v / 60, v % 60);
    else if (v < 86400) snprintf(out, cap, "%lluh%02llum", v / 3600, (v % 3600) / 60);
    else                snprintf(out, cap, "%llud%02lluh", v / 86400, (v % 86400) / 3600);
}

static const char *base_of(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static sub_seg *need_seg(void) {
    int err = 0;
    sub_seg *s = sub_attach(&err);
    if (s) return s;
    if (err == 3) fprintf(stderr, "sub: segment is not format v%u\n", SUB_VERSION);
    else if (err == 2) fprintf(stderr, "sub: segment has bad magic\n");
    else fprintf(stderr, "sub: nothing registered -- start the registry first:\n"
                         "       sub-dbd &\n");
    return NULL;
}

/* ---- sub ps ---- */

static int do_ps(void) {
    sub_seg *s = need_seg();
    if (!s) return 1;

    uint64_t now = (uint64_t)time(NULL);
    printf("%-8s  %-15s  %-15s  %-9s  %s\n", "PID", "NAME", "ROLE", "UP", "OPS");

    int live = 0, stale = 0;
    for (int i = 0; i < SUB_MAX_PEERS; i++) {
        sub_peer *p = &s->peers[i];
        if (atomic_load(&p->state) != 1) continue;
        uint64_t pid   = atomic_load(&p->pid);
        uint64_t since = atomic_load(&p->since);
        int alive = sub_peer_alive(p);
        char age[16];
        fmt_age(now > since ? now - since : 0, age, sizeof age);
        printf("%-8llu  %-15.*s  %-15.*s  %-9s  %llu%s\n",
               (unsigned long long)pid,
               SUB_NAMELEN, p->name, SUB_NAMELEN, p->role, age,
               (unsigned long long)atomic_load(&p->ops),
               alive ? "" : "   (gone, not yet reaped)");
        alive ? live++ : stale++;
    }
    if (!live && !stale) printf("(nothing registered)\n");
    else printf("\n%d registered", live + stale);
    if (stale) printf(", %d awaiting reap", stale);
    if (live || stale) printf("\n");

    munmap(s, sizeof *s);
    return 0;
}

/* ---- sub stop ---- */

static int do_stop(const char *who) {
    sub_seg *s = need_seg();
    if (!s) return 1;

    pid_t want = 0;
    int hit = 0, rc = 0;
    for (int i = 0; i < SUB_MAX_PEERS; i++) {
        sub_peer *p = &s->peers[i];
        if (!sub_peer_alive(p)) continue;
        char pidbuf[24];
        snprintf(pidbuf, sizeof pidbuf, "%llu", (unsigned long long)atomic_load(&p->pid));
        if (strncmp(p->name, who, SUB_NAMELEN) && strcmp(pidbuf, who)) continue;

        /* The registry's own owner is not just another row: stopping it takes
         * the table with it, and there is a command that does that properly. */
        if (!strncmp(p->role, "owner", SUB_NAMELEN)) {
            fprintf(stderr, "sub: %s owns the registry -- use 'sub-dbd --stop'\n", who);
            munmap(s, sizeof *s);
            return 3;
        }
        want = (pid_t)atomic_load(&p->pid);
        hit = 1;
        printf("sub: SIGTERM -> %s (pid %d)\n", who, (int)want);
        if (kill(want, SIGTERM) < 0) { perror("sub: kill"); rc = 1; }
        break;
    }
    if (!hit) {
        fprintf(stderr, "sub: no live peer named '%s'\n", who);
        munmap(s, sizeof *s);
        return 2;
    }

    /* Wait on the OS, not on a row: the row clears when dbd next reaps, which
     * is a housekeeping cadence, not a fact about the process. */
    for (int i = 0; i < 100 && kill(want, 0) == 0; i++) usleep(100000);
    if (kill(want, 0) == 0) {
        fprintf(stderr, "sub: pid %d still running after 10s\n", (int)want);
        rc = 4;
    } else {
        printf("sub: %s stopped\n", who);
    }
    munmap(s, sizeof *s);
    return rc;
}

/* ---- sub run ---- */

static int do_run(int argc, char **argv) {
    const char *name = NULL, *role = "service";
    int anyway = 0, i = 0;

    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--role") && i + 1 < argc) role = argv[++i];
        else if (!strcmp(argv[i], "--anyway")) anyway = 1;
        else { fprintf(stderr, "sub run: unknown option %s\n", argv[i]); return 2; }
    }
    if (i >= argc) {
        fprintf(stderr, "usage: sub run [--name N] [--role R] [--anyway] -- cmd [args]\n");
        return 2;
    }
    if (!name) name = base_of(argv[i]);
    if (strlen(name) >= SUB_NAMELEN)
        fprintf(stderr, "sub: name truncated to %d chars\n", SUB_NAMELEN - 1);

    /* Refusing beats registering nothing. A service you believe is managed and
     * is not is worse than one that would not start -- --anyway is the door,
     * but you have to open it on purpose.
     *
     * Probe with sub_attach first: it is the one attach that is contractually
     * silent (the dashboard polls it), so the failure here is reported once,
     * in this tool's words, instead of twice in two voices. */
    int err = 0, joined = 0;
    sub_seg *probe = sub_attach(&err);
    if (probe) {
        munmap(probe, sizeof *probe);
        joined = sub_open(&C, 1, name, role) == 0;
    }
    if (!joined) {
        if (!anyway) {
            fprintf(stderr, "sub: not started. Run the registry (sub-dbd &), "
                            "or pass --anyway to run unregistered.\n");
            return 3;
        }
        sub_open(&C, 0, name, role);      /* alone: a valid struct, no row */
        fprintf(stderr, "sub: no registry -- running %s unregistered\n", name);
    }

    signal(SIGTERM, on_sig);
    signal(SIGINT,  on_sig);
    signal(SIGHUP,  on_sig);

    pid_t kid = fork();
    if (kid < 0) { perror("sub: fork"); if (joined) sub_close(&C); return 1; }
    if (kid == 0) {
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT,  SIG_DFL);
        signal(SIGHUP,  SIG_DFL);
        execvp(argv[i], &argv[i]);
        fprintf(stderr, "sub: cannot exec %s: %s\n", argv[i], strerror(errno));
        _exit(127);
    }
    g_child = kid;

    /* The row must name the service, not its babysitter. Everything downstream
     * -- reaping, `sub stop`, the dashboard -- reads this pid and acts on it. */
    if (joined) atomic_store(&C.slot->pid, (uint64_t)kid);

    int st = 0;
    while (waitpid(kid, &st, 0) < 0 && errno == EINTR) { /* a forwarded signal */ }
    g_child = 0;
    if (joined) sub_close(&C);

    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return WEXITSTATUS(st);
}

static int usage(void) {
    printf(
      "sub -- register and manage the things you run on this machine\n"
      "\n"
      "  sub run [--name N] [--role R] [--anyway] -- cmd [args...]\n"
      "        Run cmd and register it. --name defaults to the command's\n"
      "        basename, --role to \"service\" (both capped at %d chars).\n"
      "        Without a registry running, refuses; --anyway runs anyway.\n"
      "        Exits with the child's exit code.\n"
      "  sub ps\n"
      "        List every registered process. Registers nothing itself.\n"
      "  sub stop NAME|PID\n"
      "        SIGTERM one registered process and wait up to 10s for it.\n"
      "\n"
      "The registry is the segment %s (format v%u), owned by sub-dbd.\n"
      "Start it with 'sub-dbd &' and stop it with 'sub-dbd --stop'.\n",
      SUB_NAMELEN - 1, SUB_SHM_NAME, SUB_VERSION);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    if (!strcmp(argv[1], "ps"))   return do_ps();
    if (!strcmp(argv[1], "run"))  return do_run(argc - 2, argv + 2);
    if (!strcmp(argv[1], "stop")) {
        if (argc < 3) { fprintf(stderr, "usage: sub stop NAME|PID\n"); return 2; }
        return do_stop(argv[2]);
    }
    if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h") || !strcmp(argv[1], "help"))
        return usage();
    fprintf(stderr, "sub: unknown verb '%s' (try: sub --help)\n", argv[1]);
    return 2;
}
