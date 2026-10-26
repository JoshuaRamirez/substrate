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
#include <ctype.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <limits.h>
#include <stdarg.h>
#include <sys/file.h>
#include <sys/time.h>
#include <dirent.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <dirent.h>
#endif

static substrate C;
static pid_t g_child = 0;
static volatile sig_atomic_t g_got_sig = 0, g_alarm = 0;
static void on_alarm(int s) { (void)s; g_alarm = 1; }

/* Forward what we are told to the thing we are supervising. The wrapper is a
 * conduit, not a policy: it does not decide that SIGTERM means stop. */
/* The child leads its own process group (see do_run), so forward to the
 * GROUP: a service's workers are part of the service. */
static void on_sig(int s) {
    g_got_sig = s;
    if (g_child > 0) kill(-g_child, s);
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
        uint64_t start = sub_proc_start((pid_t)pid);    /* the process's own start, not the row's claim (a re-join resets that) */
        if (start) since = start;
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

/* Is anything in this target still alive? For a group, kill(-pgid, 0)
 * fails with ESRCH only once every member is gone. */
static int target_alive(pid_t pid, int group) {
    return kill(group ? -pid : pid, 0) == 0;
}

static int do_stop(const char *who, int force) {
    sub_seg *s = need_seg();
    if (!s) return 1;

    pid_t want = 0;
    int hit = 0;
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
        break;
    }
    munmap(s, sizeof *s);
    if (!hit) {
        fprintf(stderr, "sub: no live peer named '%s'\n", who);
        return 2;
    }

    /* Signal the group only when the pid LEADS one. `sub run` children do. A
     * C peer that joined on its own (webd --join from a shell) shares its
     * shell's group, and signalling that group would take the shell with it. */
    int group = getpgid(want) == want;
    printf("sub: SIGTERM -> %s (%s %d)\n", who, group ? "group" : "pid", (int)want);
    if (kill(group ? -want : want, SIGTERM) < 0) { perror("sub: kill"); return 1; }

    /* Wait on the OS, not on a row: the row clears when dbd next reaps, which
     * is a housekeeping cadence, not a fact about the process. */
    for (int i = 0; i < 100 && target_alive(want, group); i++) usleep(100000);
    if (!target_alive(want, group)) { printf("sub: %s stopped\n", who); return 0; }

    /* Refusing to escalate unasked is the default: you said stop, not kill.
     * But a stop that can quietly not stop is a thin promise, so the failure
     * is loud and the escalation is one flag away. */
    if (!force) {
        fprintf(stderr, "sub: %s still running after 10s -- 'sub stop --force %s' "
                        "sends SIGKILL\n", who, who);
        return 4;
    }
    printf("sub: SIGKILL -> %s\n", who);
    kill(group ? -want : want, SIGKILL);
    for (int i = 0; i < 20 && target_alive(want, group); i++) usleep(100000);
    if (target_alive(want, group)) {
        fprintf(stderr, "sub: %s survived SIGKILL (stuck in the kernel?)\n", who);
        return 4;
    }
    printf("sub: %s killed\n", who);
    return 0;
}

/* ---- sub run ---- */

static int do_run(int argc, char **argv) {
    const char *name = NULL, *role = "service";
    int anyway = 0, i = 0, waitsec = 0;

    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--role") && i + 1 < argc) role = argv[++i];
        else if (!strcmp(argv[i], "--anyway")) anyway = 1;
        else if (!strcmp(argv[i], "--wait") && i + 1 < argc) waitsec = atoi(argv[++i]);
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
    /* launchd starts agents in no particular order, so a service can come up
     * before the registry does. Waiting is not the same as not caring: it
     * still refuses to run unregistered, it just gives the registry a window
     * to appear first. */
    int err = 0, joined = 0;
    sub_seg *probe = NULL;
    for (int t = 0; ; t++) {
        probe = sub_attach(&err);
        if (probe || t >= waitsec * 10) break;
        usleep(100000);
    }
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
        /* Its own process group, for two reasons. `sub stop` can then reach
         * everything the service forked, not just its first pid -- without
         * this, stopping `sh -c "worker & wait"` orphaned the worker to pid 1.
         * And a Ctrl-C at the terminal no longer hits the child directly AND
         * again via on_sig: the terminal signals the wrapper's group, which
         * the child is no longer in, so it is told once. */
        setpgid(0, 0);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT,  SIG_DFL);
        signal(SIGHUP,  SIG_DFL);
        execvp(argv[i], &argv[i]);
        fprintf(stderr, "sub: cannot exec %s: %s\n", argv[i], strerror(errno));
        _exit(127);
    }
    setpgid(kid, kid);    /* both sides, so neither races the other */
    g_child = kid;

    /* The row must name the service, not its babysitter. Everything downstream
     * -- reaping, `sub stop`, the dashboard -- reads this pid and acts on it. */
    if (joined) atomic_store(&C.slot->pid, (uint64_t)kid);

    /* Wait by polling, not by blocking: a registry that restarts (a crash,
     * an upgrade, the test suite) unlinks its segment and makes a new one,
     * and a wrapper that only ever blocked in waitpid would stay a row in a
     * table nobody can see any more. Once a second, revalidate -- re-join
     * the new owner if one appeared -- and put the service's pid back on
     * the row, since a fresh claim records the wrapper's. */
    int st = 0;
    for (;;) {
        pid_t r = waitpid(kid, &st, WNOHANG);
        if (r == kid) break;
        if (r < 0 && errno != EINTR) break;
        if (joined && sub_revalidate(&C) && C.slot &&
            atomic_load(&C.slot->pid) != (uint64_t)kid)
            atomic_store(&C.slot->pid, (uint64_t)kid);
        usleep(500000);
    }
    g_child = 0;
    if (joined) sub_close(&C);

    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return WEXITSTATUS(st);
}


/* ---- launchd: making it come back after a reboot (macOS only) ---- */

#if defined(__APPLE__)

#define SUB_LABEL_PFX "dev.substrate."

/* plist is XML. An unescaped & in someone's argument is a corrupt plist that
 * launchd refuses with a message about the file, not about the argument. */
static void xml_out(FILE *f, const char *t) {
    for (; *t; t++) switch (*t) {
        case '&':  fputs("&amp;",  f); break;
        case '<':  fputs("&lt;",   f); break;
        case '>':  fputs("&gt;",   f); break;
        case '"':  fputs("&quot;", f); break;
        case '\'': fputs("&apos;", f); break;
        default:   fputc(*t, f);
    }
}

static int self_path(char *out, size_t cap) {
    uint32_t n = (uint32_t)cap;
    if (_NSGetExecutablePath(out, &n) != 0) return -1;
    char real[PATH_MAX];
    if (realpath(out, real)) snprintf(out, cap, "%s", real);
    return 0;
}

/* launchd hands an agent a minimal PATH and no shell, so "node" means nothing
 * by the time the plist runs. Resolve it now, while a usable PATH exists. */
static int which_abs(const char *cmd, char *out, size_t cap) {
    if (strchr(cmd, '/')) {
        /* A path is kept as given, symlinks and all. Resolving
         * /opt/homebrew/opt/python@3.14/bin/python3.14 to its Cellar path
         * would pin a version the next upgrade removes. Only a relative
         * path is anchored, to the directory it was given in. */
        if (cmd[0] == '/') snprintf(out, cap, "%s", cmd);
        else {
            char cwd[PATH_MAX];
            if (!getcwd(cwd, sizeof cwd)) return -1;
            snprintf(out, cap, "%s/%s", cwd, cmd);
        }
        return access(out, X_OK);
    }
    const char *path = getenv("PATH");
    if (!path) return -1;
    char buf[PATH_MAX];
    while (*path) {
        size_t k = strcspn(path, ":");
        if (k && k < sizeof buf) {
            snprintf(buf, sizeof buf, "%.*s/%s", (int)k, path, cmd);
            if (access(buf, X_OK) == 0) { snprintf(out, cap, "%s", buf); return 0; }
        }
        path += k + (path[k] == ':');
    }
    return -1;
}

static int run_cmd(char *const av[], int quiet) {
    pid_t k = fork();
    if (k < 0) return -1;
    if (k == 0) {
        if (quiet) {           /* "Boot-out failed: No such process" is the
                                * normal case for something not yet loaded. */
            int null = open("/dev/null", O_WRONLY);
            if (null >= 0) { dup2(null, 1); dup2(null, 2); close(null); }
        }
        execvp(av[0], av); _exit(127);
    }
    int st = 0;
    while (waitpid(k, &st, 0) < 0 && errno == EINTR) { }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void agent_path(const char *name, char *out, size_t cap) {
    snprintf(out, cap, "%s/Library/LaunchAgents/" SUB_LABEL_PFX "%s.plist",
             getenv("HOME"), name);
}

static void logs_dir(char *out, size_t cap) {
    snprintf(out, cap, "%s/Library/Logs/substrate", getenv("HOME"));
}

static int bootout(const char *label) {
    char tgt[256], lc[] = "launchctl", verb[] = "bootout";
    snprintf(tgt, sizeof tgt, "gui/%u/%s", getuid(), label);
    char *const av[] = { lc, verb, tgt, NULL };
    return run_cmd(av, 1);     /* absent is fine; bootstrap is what must work */
}

static int bootstrap(const char *plist) {
    char tgt[64], lc[] = "launchctl", verb[] = "bootstrap", pl[PATH_MAX];
    snprintf(tgt, sizeof tgt, "gui/%u", getuid());
    snprintf(pl, sizeof pl, "%s", plist);
    char *const av[] = { lc, verb, tgt, pl, NULL };
    return run_cmd(av, 0);
}

/* A schedule: how launchd starts the agent. KeepAlive is the service shape;
 * an interval or a list of clock times is a timer, run to completion. */
typedef struct { int every; int n; int hour[32], minute[32]; int nenv; const char *env[16]; int norestart; } sub_sched;

/* "02:30" or ":30" (hourly). -1 on anything else. */
static int parse_hhmm(const char *t, int *h, int *m) {
    const char *c = strchr(t, ':');
    if (!c || strlen(c + 1) != 2 || !isdigit((unsigned char)c[1]) || !isdigit((unsigned char)c[2])) return -1;
    *m = atoi(c + 1);
    if (*m > 59) return -1;
    if (c == t) { *h = -1; return 0; }
    if (c - t > 2) return -1;
    for (const char *q = t; q < c; q++) if (!isdigit((unsigned char)*q)) return -1;
    *h = atoi(t);
    return *h > 23 ? -1 : 0;
}

static int parse_at(const char *list, sub_sched *sc) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", list);
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
        if (sc->n >= 32) return -1;
        if (parse_hhmm(t, &sc->hour[sc->n], &sc->minute[sc->n]) < 0) return -1;
        sc->n++;
    }
    return sc->n ? 0 : -1;
}

/* Write a plist, atomically: launchd may be reading the directory. */
static int write_plist(const char *name, char *const *args, int n, const char *wd, const sub_sched *sc) {
    char path[PATH_MAX], tmp[PATH_MAX], logs[PATH_MAX];
    agent_path(name, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    logs_dir(logs, sizeof logs);
    mkdir(logs, 0755);

    FILE *f = fopen(tmp, "w");
    if (!f) { fprintf(stderr, "sub: cannot write %s: %s\n", tmp, strerror(errno)); return -1; }
    fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
          "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
          "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
          "<plist version=\"1.0\">\n<dict>\n", f);
    fputs("  <key>Label</key><string>" SUB_LABEL_PFX, f); xml_out(f, name); fputs("</string>\n", f);
    fputs("  <key>ProgramArguments</key>\n  <array>\n", f);
    for (int i = 0; i < n; i++) { fputs("    <string>", f); xml_out(f, args[i]); fputs("</string>\n", f); }
    fputs("  </array>\n", f);
    if (sc && sc->norestart) fputs("  <key>RunAtLoad</key><true/>\n", f);   /* a login app: started once, not kept alive */
    else if (!sc || (!sc->every && !sc->n)) fputs("  <key>RunAtLoad</key><true/>\n  <key>KeepAlive</key><true/>\n", f);
    else if (sc->every) fprintf(f, "  <key>RunAtLoad</key><true/>\n  <key>StartInterval</key><integer>%d</integer>\n", sc->every);
    else {
        fputs("  <key>StartCalendarInterval</key>\n  <array>\n", f);
        for (int i = 0; i < sc->n; i++) {
            fputs("    <dict>", f);
            if (sc->hour[i] >= 0) fprintf(f, "<key>Hour</key><integer>%d</integer>", sc->hour[i]);
            fprintf(f, "<key>Minute</key><integer>%d</integer></dict>\n", sc->minute[i]);
        }
        fputs("  </array>\n", f);
    }
    if (wd) { fputs("  <key>WorkingDirectory</key><string>", f); xml_out(f, wd); fputs("</string>\n", f); }
    for (int i = 0; i < 2; i++) {
        fprintf(f, "  <key>Standard%sPath</key><string>", i ? "Error" : "Out");
        xml_out(f, logs); fputc('/', f); xml_out(f, name); fputs(".log</string>\n", f);
    }
    /* Give the agent the PATH this shell has. Without it, a service that
     * shells out finds a four-entry PATH and fails in a confusing place. */
    const char *pth = getenv("PATH");
    if (pth || (sc && sc->nenv)) {
        fputs("  <key>EnvironmentVariables</key><dict>\n", f);
        if (pth) { fputs("    <key>PATH</key><string>", f); xml_out(f, pth); fputs("</string>\n", f); }
        for (int i = 0; sc && i < sc->nenv; i++) {       /* K=V, validated at parse time */
            const char *eq = strchr(sc->env[i], '=');
            fputs("    <key>", f); fwrite(sc->env[i], 1, (size_t)(eq - sc->env[i]), f);
            fputs("</key><string>", f); xml_out(f, eq + 1); fputs("</string>\n", f);
        }
        fputs("  </dict>\n", f);
    }
    fputs("</dict>\n</plist>\n", f);
    if (fclose(f) != 0) { unlink(tmp); return -1; }
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

/* The registry is what every other agent needs, and launchd will not order
 * them -- so enabling anything ensures it, rather than leaving a step the
 * user finds out about only after a reboot. */
static int ensure_registry(void) {
    char plist[PATH_MAX];
    agent_path("registry", plist, sizeof plist);
    if (access(plist, F_OK) == 0) return 0;

    char me[PATH_MAX], dbd[PATH_MAX];
    if (self_path(me, sizeof me) < 0) return -1;
    snprintf(dbd, sizeof dbd, "%s-dbd", me);
    if (access(dbd, X_OK) != 0) {
        fprintf(stderr, "sub: cannot find sub-dbd next to %s\n", me);
        return -1;
    }
    char *const args[] = { dbd, NULL };
    if (write_plist("registry", args, 1, NULL, NULL) < 0) return -1;
    bootout(SUB_LABEL_PFX "registry");
    if (bootstrap(plist) != 0) {
        fprintf(stderr, "sub: launchctl could not load the registry agent\n");
        return -1;
    }
    printf("sub: registry agent enabled (" SUB_LABEL_PFX "registry)\n");
    return 0;
}

static int strstr_argv(char **argv, int argc, const char *flag) {
    for (int k = 0; k < argc; k++) { if (!strcmp(argv[k], "--")) break; if (!strcmp(argv[k], flag)) return 1; }
    return 0;
}

static int do_enable(int argc, char **argv) {
    const char *name = NULL, *role = "service", *dir = NULL, *plate = NULL, *at = NULL;
    int i = 0, waitsec = 60, want_registry = 1;
    sub_sched sc = {0};
    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) sc.every = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--at") && i + 1 < argc) at = argv[++i];
        else if (!strcmp(argv[i], "--env") && i + 1 < argc) {
            const char *kv = argv[++i], *eq = strchr(kv, '=');
            if (!eq || eq == kv) { fprintf(stderr, "sub: --env takes KEY=VALUE, got '%s'\n", kv); return 2; }
            if (sc.nenv >= 16) { fprintf(stderr, "sub: at most 16 --env settings\n"); return 2; }
            sc.env[sc.nenv++] = kv;
        }
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--role") && i + 1 < argc) role = argv[++i];
        else if (!strcmp(argv[i], "--dir")  && i + 1 < argc) dir  = argv[++i];
        else if (!strcmp(argv[i], "--nameplate") && i + 1 < argc) plate = argv[++i];
        else if (!strcmp(argv[i], "--wait") && i + 1 < argc) waitsec = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-registry")) want_registry = 0;
        else if (!strcmp(argv[i], "--no-restart")) sc.norestart = 1;
        else { fprintf(stderr, "sub enable: unknown option %s\n", argv[i]); return 2; }
    }
    if (i >= argc) {
        fprintf(stderr, "usage: sub enable [--name N] [--role R] [--dir D] [--nameplate FILE] [--env K=V]... [--every SECONDS | --at HH:MM[,HH:MM...] | --no-restart] -- cmd [args]\n");
        return 2;
    }
    if (!name) name = base_of(argv[i]);
    if (sc.every && at) { fprintf(stderr, "sub: --every and --at are two schedules; give one\n"); return 2; }
    if (sc.norestart && (sc.every || at)) { fprintf(stderr, "sub: --no-restart is for a login app; a timer already runs to completion\n"); return 2; }
    if (sc.every < 0 || (sc.every == 0 && argc > 0 && at == NULL && strstr_argv(argv, argc, "--every"))) {
        fprintf(stderr, "sub: --every takes a whole number of seconds, at least 1\n"); return 2;
    }
    if (at && parse_at(at, &sc) < 0) {
        fprintf(stderr, "sub: --at takes HH:MM (or :MM for hourly), comma-separated, got '%s'\n", at); return 2;
    }
    int timer = sc.every || sc.n;
    /* macOS Login Items names an agent after the file in ProgramArguments[0].
     * A nameplate is an executable that execs what follows it, so the agent
     * carries its own name there instead of "sub". */
    char plate_abs[PATH_MAX];
    if (plate) {
        if (which_abs(plate, plate_abs, sizeof plate_abs) != 0) {
            fprintf(stderr, "sub: nameplate '%s' is not an executable file\n", plate);
            return 2;
        }
    }
    if (strchr(name, '/')) { fprintf(stderr, "sub: a name cannot contain '/'\n"); return 2; }

    char me[PATH_MAX], cmd[PATH_MAX], cwd[PATH_MAX], ws[16];
    if (self_path(me, sizeof me) < 0) { fprintf(stderr, "sub: cannot find my own path\n"); return 1; }
    if (which_abs(argv[i], cmd, sizeof cmd) != 0) {
        fprintf(stderr, "sub: cannot find '%s' on PATH -- launchd will not either\n", argv[i]);
        return 2;
    }
    if (!dir) dir = getcwd(cwd, sizeof cwd);
    snprintf(ws, sizeof ws, "%d", waitsec);

    /* sub run is what launchd supervises: it registers the child, forwards
     * signals to its group, and exits with the child's exit code, which is
     * exactly the contract KeepAlive wants. */
    char v_run[] = "run", v_name[] = "--name", v_role[] = "--role",
         v_wait[] = "--wait", v_any[] = "--anyway", v_end[] = "--";
    /* A timer's tick must happen: with the registry away it waits briefly,
     * then runs unregistered. A lost tick is worse than an unlisted one. */
    if (timer) snprintf(ws, sizeof ws, "%d", waitsec > 5 ? 5 : waitsec);
    char nm[SUB_NAMELEN * 4], rl[SUB_NAMELEN * 4];
    snprintf(nm, sizeof nm, "%s", name);
    snprintf(rl, sizeof rl, "%s", role);
    char *args[64];
    int n = 0;
    if (plate) args[n++] = plate_abs;
    args[n++] = me;
    args[n++] = v_run;
    args[n++] = v_name; args[n++] = nm;
    args[n++] = v_role; args[n++] = rl;
    args[n++] = v_wait; args[n++] = ws;
    if (timer) args[n++] = v_any;
    args[n++] = v_end;
    args[n++] = cmd;
    for (int k = i + 1; k < argc && n < 63; k++) args[n++] = argv[k];
    args[n] = NULL;

    if (want_registry && ensure_registry() < 0) return 1;

    char plist[PATH_MAX], label[256];
    agent_path(name, plist, sizeof plist);
    snprintf(label, sizeof label, SUB_LABEL_PFX "%s", name);
    if (write_plist(name, args, n, dir, &sc) < 0) return 1;
    bootout(label);                       /* re-enabling is allowed */
    if (bootstrap(plist) != 0) {
        fprintf(stderr, "sub: launchctl could not load %s\n", plist);
        return 1;
    }
    if (sc.norestart) printf("sub: %s enabled -- starts at login, left alone when it exits\n", name);
    else if (sc.every) printf("sub: %s enabled -- every %ds, and at login\n", name, sc.every);
    else if (sc.n) printf("sub: %s enabled -- at %s\n", name, at);
    else printf("sub: %s enabled -- starts at login, restarts if it exits\n", name);
    printf("     plist %s\n", plist);
    printf("     log   %s/Library/Logs/substrate/%s.log\n", getenv("HOME"), name);
    return 0;
}

static int do_disable(const char *name) {
    char plist[PATH_MAX], label[256];
    agent_path(name, plist, sizeof plist);
    snprintf(label, sizeof label, SUB_LABEL_PFX "%s", name);
    if (access(plist, F_OK) != 0) {
        fprintf(stderr, "sub: %s is not enabled\n", name);
        return 2;
    }
    bootout(label);
    if (unlink(plist) != 0) { perror("sub: unlink"); return 1; }
    printf("sub: %s disabled and removed from login\n", name);
    return 0;
}

static int do_enabled(void) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/Library/LaunchAgents", getenv("HOME"));
    DIR *d = opendir(dir);
    if (!d) { perror("sub: opendir"); return 1; }
    struct dirent *e;
    int n = 0;
    size_t pfx = strlen(SUB_LABEL_PFX);
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, SUB_LABEL_PFX, pfx)) continue;
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".plist")) continue;
        char nm[128], label[256], tgt[320];
        snprintf(nm, sizeof nm, "%.*s", (int)(dot - e->d_name) - (int)pfx, e->d_name + pfx);
        snprintf(label, sizeof label, SUB_LABEL_PFX "%s", nm);
        snprintf(tgt, sizeof tgt, "gui/%u/%s", getuid(), label);
        char lc[] = "launchctl", verb[] = "print";
        char *const av[] = { lc, verb, tgt, NULL };
        int loaded = run_cmd(av, 1) == 0;
        if (!n++) printf("%-20s  %s\n", "NAME", "STATE");
        printf("%-20s  %s\n", nm, loaded ? "loaded" : "not loaded");
    }
    closedir(d);
    if (!n) printf("(nothing enabled at login)\n");
    return 0;
}

#else   /* not macOS */

static int do_enable(int argc, char **argv) {
    (void)argc; (void)argv;
    fprintf(stderr, "sub: enable needs launchd, which is macOS only.\n"
                    "     On Linux, wrap 'sub run' in a systemd user unit.\n");
    return 2;
}
static int do_disable(const char *name) {
    (void)name;
    fprintf(stderr, "sub: enable/disable need launchd, which is macOS only.\n");
    return 2;
}
static int do_enabled(void) {
    fprintf(stderr, "sub: enable/disable need launchd, which is macOS only.\n");
    return 2;
}

#endif

/* ---- sub change: the system-change lane ----
 *
 * A change to the running system (a service restarted, a launchd job loaded,
 * a setting written) goes through one lane, one at a time, in the order the
 * changes asked. It is a queue, not a gate: nothing is refused and nothing
 * waits for approval. A change only waits its turn.
 *
 * The lane is a directory. Taking a ticket is a short critical section under
 * flock(lane.lock): bump the number in `seq`, create queue/<ticket>.q and lock
 * that file for the life of the process. The holder is the lowest ticket whose
 * file is still locked. A process that dies, however it dies, drops its lock
 * with it -- the kernel does that, not a heartbeat -- so a waiter that can lock
 * an entry knows its owner is gone, clears it, and logs it if it was running.
 *
 * A stuck change does not hold the lane forever: past its timeout its own
 * process group is sent SIGTERM, then SIGKILL, it is logged timed-out, and the
 * next change runs. Each change is one line of changes.jsonl. */

#define LANE_POLL_US   20000
#define LANE_TIMEOUT   300

typedef struct { char *p; size_t n, cap; } sbuf;

static void sb_put(sbuf *b, const char *s, size_t k) {
    if (b->n + k + 1 > b->cap) {
        size_t c = b->cap ? b->cap : 256;
        while (c < b->n + k + 1) c *= 2;
        char *q = realloc(b->p, c);
        if (!q) return;
        b->p = q; b->cap = c;
    }
    memcpy(b->p + b->n, s, k); b->n += k; b->p[b->n] = 0;
}
static void sb_s(sbuf *b, const char *s) { sb_put(b, s, strlen(s)); }
__attribute__((format(printf, 2, 3)))
static void sb_f(sbuf *b, const char *fmt, ...) {
    char t[512]; va_list ap; va_start(ap, fmt);
    int k = vsnprintf(t, sizeof t, fmt, ap); va_end(ap);
    if (k > 0) sb_put(b, t, (size_t)k < sizeof t ? (size_t)k : sizeof t - 1);
}
static void sb_json(sbuf *b, const char *s) {
    sb_s(b, "\"");
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { char e[2] = { '\\', (char)c }; sb_put(b, e, 2); }
        else if (c == '\n') sb_s(b, "\\n");
        else if (c == '\t') sb_s(b, "\\t");
        else if (c == '\r') sb_s(b, "\\r");
        else if (c < 0x20)  sb_f(b, "\\u%04x", c);
        else sb_put(b, (const char *)&c, 1);
    }
    sb_s(b, "\"");
}

static double now_s(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + tv.tv_usec / 1e6;
}

/* Local time, milliseconds and offset: 2026-10-20T15:08:00.123-07:00 */
static void iso_at(double t, char *out, size_t cap) {
    time_t s = (time_t)t; struct tm tm; localtime_r(&s, &tm);
    char base[32], z[8];
    strftime(base, sizeof base, "%Y-%m-%dT%H:%M:%S", &tm);
    strftime(z, sizeof z, "%z", &tm);
    int ms = (int)((t - (double)s) * 1000); if (ms > 999) ms = 999; if (ms < 0) ms = 0;
    snprintf(out, cap, "%s.%03d%.3s:%.2s", base, ms, z, z + 3);
}

static void lane_root(char *out, size_t cap) {
    const char *e = getenv("SUB_LANE"), *h = getenv("HOME");
    if (e && *e) snprintf(out, cap, "%s", e);
    else snprintf(out, cap, "%s/Library/Application Support/substrate/changes", h ? h : "/tmp");
}

static int mkdir_p(const char *path) {
    char t[PATH_MAX]; snprintf(t, sizeof t, "%s", path);
    for (char *p = t + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(t, 0755); *p = '/'; }
    return (mkdir(t, 0755) == 0 || errno == EEXIST) ? 0 : -1;
}

static int on_path(const char *cmd) {
    const char *path = getenv("PATH");
    char buf[PATH_MAX];
    while (path && *path) {
        size_t k = strcspn(path, ":");
        if (k && k < sizeof buf - strlen(cmd) - 2) {
            snprintf(buf, sizeof buf, "%.*s/%s", (int)k, path, cmd);
            if (access(buf, X_OK) == 0) return 1;
        }
        path += k + (path[k] == ':');
    }
    return 0;
}

/* Who asked: the desk's name for this agent ($DESK_WHOAMI, else desk-whoami,
 * which reads desk_identity), else the herdr pane, else the login. */
static void who_am_i(char *out, size_t cap) {
    const char *w = getenv("DESK_WHOAMI");
    if (w && *w) { snprintf(out, cap, "%s", w); return; }
    if (on_path("desk-whoami")) {
        FILE *p = popen("desk-whoami 2>/dev/null", "r");
        if (p) {
            char line[256] = "";
            if (!fgets(line, sizeof line, p)) line[0] = 0;
            int st = pclose(p);
            line[strcspn(line, "\t\n")] = 0;
            if (st == 0 && line[0] && strcmp(line, "unknown")) { snprintf(out, cap, "%s", line); return; }
        }
    }
    const char *pane = getenv("HERDR_PANE_ID");
    if (pane && *pane) { snprintf(out, cap, "herdr pane %s", pane); return; }
    const char *u = getenv("USER");
    snprintf(out, cap, "%s", u && *u ? u : "unknown");
}

typedef struct {
    unsigned long long ticket, nested_in;
    const char *what, *why, *undo;
    char who[256];
    char **cmd; int ncmd;
    int timeout;
    double queued, started, ended;
    pid_t pid, pgid; uint64_t child_start;
    const char *status; int exit_code;
} lane_change;

static void change_json(sbuf *b, const lane_change *c) {
    char t[48];
    sb_f(b, "{\"ticket\":%llu,\"what\":", c->ticket); sb_json(b, c->what);
    sb_s(b, ",\"who\":");  sb_json(b, c->who);
    sb_s(b, ",\"why\":");  sb_json(b, c->why ? c->why : "");
    sb_s(b, ",\"undo\":"); sb_json(b, c->undo ? c->undo : "");
    sb_s(b, ",\"cmd\":[");
    for (int i = 0; i < c->ncmd; i++) { if (i) sb_s(b, ","); sb_json(b, c->cmd[i]); }
    sb_s(b, "]");
    iso_at(c->queued, t, sizeof t); sb_s(b, ",\"queued\":"); sb_json(b, t); sb_f(b, ",\"queued_s\":%.3f", c->queued);
    if (c->started > 0) { iso_at(c->started, t, sizeof t); sb_s(b, ",\"started\":"); sb_json(b, t); sb_f(b, ",\"started_s\":%.3f", c->started); }
    if (c->ended > 0) {
        iso_at(c->ended, t, sizeof t); sb_s(b, ",\"ended\":"); sb_json(b, t); sb_f(b, ",\"ended_s\":%.3f", c->ended);
        sb_s(b, ",\"status\":"); sb_json(b, c->status);
        sb_f(b, ",\"exit\":%d", c->exit_code);
    }
    sb_f(b, ",\"timeout\":%d,\"pid\":%d", c->timeout, (int)c->pid);
    if (c->pgid > 0) sb_f(b, ",\"pgid\":%d,\"child_start\":%llu", (int)c->pgid, (unsigned long long)c->child_start);
    if (c->nested_in) sb_f(b, ",\"nested_in\":%llu", c->nested_in);
    sb_s(b, "}");
}

static void lane_log(const char *root, const char *line, size_t n) {
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s/changes.jsonl", root);
    int fd = open(p, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) { fprintf(stderr, "sub: cannot log the change to %s: %s\n", p, strerror(errno)); return; }
    if (write(fd, line, n) != (ssize_t)n) fprintf(stderr, "sub: short write to %s\n", p);
    close(fd);
}

/* An entry is rewritten in place, never renamed over: the lock lives on the
 * inode, and a new inode would read as unlocked, that is, as dead. */
static void entry_write(int fd, const lane_change *c) {
    if (fd < 0) return;
    sbuf b = {0}; change_json(&b, c); sb_s(&b, "\n");
    if (b.p) { if (ftruncate(fd, 0) == 0 && pwrite(fd, b.p, b.n, 0) < 0) { /* display only */ } }
    free(b.p);
}

static char *read_fd(int fd) {
    sbuf b = {0}; char t[4096]; ssize_t k; off_t off = 0;
    while ((k = pread(fd, t, sizeof t, off)) > 0) { sb_put(&b, t, (size_t)k); off += k; }
    if (!b.p) sb_s(&b, "");
    return b.p;
}

/* A string field of one JSON line we wrote ourselves. Inside a value every
 * quote is escaped, so "key":" can only match the key itself. */
static int jstr(const char *line, const char *key, char *out, size_t cap) {
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\":\"", key);
    const char *p = strstr(line, pat);
    out[0] = 0;
    if (!p) return 0;
    p += strlen(pat);
    size_t k = 0;
    while (*p && *p != '"' && k + 1 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'u') { out[k++] = '?'; p += (strlen(p) >= 5) ? 5 : strlen(p); continue; }
            out[k++] = (*p == 'n' || *p == 't' || *p == 'r') ? ' ' : *p;
            p++; continue;
        }
        out[k++] = *p++;
    }
    out[k] = 0;
    return 1;
}
static double jnum(const char *line, const char *key, double dflt) {
    char pat[64]; snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(line, pat);
    if (!p) return dflt;
    p += strlen(pat);
    if (*p == 'n' || *p == '"') return dflt;
    return strtod(p, NULL);
}

static int cmp_ull(const void *a, const void *b) {
    unsigned long long x = *(const unsigned long long *)a, y = *(const unsigned long long *)b;
    return x < y ? -1 : x > y;
}

/* The tickets in the queue, ascending. Caller frees. */
static int queue_tickets(const char *root, unsigned long long **out) {
    char q[PATH_MAX]; snprintf(q, sizeof q, "%s/queue", root);
    *out = NULL;
    DIR *d = opendir(q);
    if (!d) return 0;
    int n = 0, cap = 0; struct dirent *e;
    while ((e = readdir(d))) {
        size_t L = strlen(e->d_name);
        if (L < 3 || strcmp(e->d_name + L - 2, ".q") || !isdigit((unsigned char)e->d_name[0])) continue;
        if (n == cap) { cap = cap ? cap * 2 : 16; unsigned long long *t = realloc(*out, cap * sizeof **out); if (!t) break; *out = t; }
        (*out)[n++] = strtoull(e->d_name, NULL, 10);
    }
    closedir(d);
    if (n) qsort(*out, (size_t)n, sizeof **out, cmp_ull);
    return n;
}

static void entry_path(const char *root, unsigned long long t, char *out, size_t cap) {
    snprintf(out, cap, "%s/queue/%012llu.q", root, t);
}

/* Is this entry's owner alive? Its lock answers: a lock we can take is one
 * nobody holds. Returns 1 alive, 0 dead (with *fdp left open AND locked, so
 * the caller can bury it), -1 gone. */
static int entry_alive(const char *path, int *fdp) {
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
        /* Opened before its owner unlinked it and locked after the owner
         * closed it: that is a change that ended cleanly, not a dead one.
         * Only the file still at this path counts. */
        struct stat a, b;
        if (fstat(fd, &a) != 0 || stat(path, &b) != 0 || a.st_ino != b.st_ino || a.st_dev != b.st_dev) {
            close(fd);
            return -1;
        }
        if (fdp) *fdp = fd; else close(fd);
        return 0;
    }
    close(fd);
    return 1;
}

/* A dead owner's entry: what it was running is stopped (its own group, and
 * only if that group is still the one it started), the change is logged
 * holder-died, and the entry goes. Called under the lane lock. */
static void lane_bury(const char *root, int fd, const char *path) {
    char *body = read_fd(fd);
    if (strstr(body, "\"started\":")) {
        pid_t pg = (pid_t)jnum(body, "pgid", 0);
        uint64_t st = (uint64_t)jnum(body, "child_start", 0);
        /* Ours if the group still exists and its leader is the process the
         * change started, or is gone (a pid is not reused while it names a
         * live group, so a leaderless group of that number is still ours). */
        uint64_t now_start = sub_proc_start(pg);
        if (pg > 1 && st && kill(-pg, 0) == 0 && (now_start == st || (now_start == 0 && kill(pg, 0) != 0))) {
            kill(-pg, SIGTERM);
            for (int i = 0; i < 50 && kill(-pg, 0) == 0; i++) usleep(10000);
            kill(-pg, SIGKILL);
        }
        size_t L = strlen(body);
        while (L && (body[L - 1] == '\n' || body[L - 1] == ' ')) L--;
        if (L && body[L - 1] == '}') {
            sbuf b = {0}; char t[48]; double now = now_s();
            sb_put(&b, body, L - 1);
            iso_at(now, t, sizeof t);
            sb_s(&b, ",\"ended\":"); sb_json(&b, t);
            sb_f(&b, ",\"ended_s\":%.3f,\"status\":\"holder-died\",\"exit\":null}\n", now);
            lane_log(root, b.p, b.n);
            free(b.p);
        }
    }
    free(body);
    unlink(path);
    close(fd);
}

/* Under the lane lock: how many live changes are ahead of ticket `mine`.
 * Dead ones met on the way are buried. Stops at the first live one. */
static int lane_ahead(const char *root, unsigned long long mine, unsigned long long *first) {
    unsigned long long *t = NULL;
    int n = queue_tickets(root, &t), live = 0;
    for (int i = 0; i < n && t[i] < mine; i++) {
        char p[PATH_MAX]; entry_path(root, t[i], p, sizeof p);
        int fd = -1, a = entry_alive(p, &fd);
        if (a == 0) { lane_bury(root, fd, p); continue; }
        if (a == 1) { live = 1; if (first) *first = t[i]; break; }
    }
    free(t);
    return live;
}

static unsigned long long next_ticket(const char *root) {
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s/seq", root);
    int fd = open(p, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return (unsigned long long)(now_s() * 1000);
    char buf[32] = ""; ssize_t k = pread(fd, buf, sizeof buf - 1, 0);
    unsigned long long n = k > 0 ? strtoull(buf, NULL, 10) : 0;
    n++;
    int w = snprintf(buf, sizeof buf, "%llu\n", n);
    if (ftruncate(fd, 0) != 0 || pwrite(fd, buf, (size_t)w, 0) != w) { /* the entry's O_EXCL still keeps tickets unique */ }
    close(fd);
    return n;
}

/* Run the change: its own process group, a deadline, then the log line. */
static int lane_run(lane_change *c, const char *root, int efd, const char *epath) {
    c->started = now_s();
    char tk[32]; snprintf(tk, sizeof tk, "%llu", c->nested_in ? c->nested_in : c->ticket);
    /* The child execs only once its process group is on record in the entry:
     * it waits on a pipe for the go. If this process dies first, the pipe
     * reads EOF and the child leaves without running anything, so there is
     * never a change running that a waiter could not find and stop. */
    int gate[2];
    if (pipe(gate) != 0) { perror("sub: pipe"); c->exit_code = 1; c->status = "failed"; goto done; }
    fcntl(gate[0], F_SETFD, FD_CLOEXEC); fcntl(gate[1], F_SETFD, FD_CLOEXEC);
    pid_t kid = fork();
    if (kid < 0) { perror("sub: fork"); close(gate[0]); close(gate[1]); c->exit_code = 1; c->status = "failed"; goto done; }
    if (kid == 0) {
        setpgid(0, 0);
        signal(SIGTERM, SIG_DFL); signal(SIGINT, SIG_DFL); signal(SIGHUP, SIG_DFL);
        close(gate[1]);
        char go = 0; ssize_t k;
        while ((k = read(gate[0], &go, 1)) < 0 && errno == EINTR) { }
        if (k != 1) _exit(125);
        close(gate[0]);
        /* a change that makes a change runs inside this one, not behind it */
        setenv("SUB_CHANGE_TICKET", tk, 1);
        setenv("SUB_CHANGE_LANE", root, 1);
        execvp(c->cmd[0], c->cmd);
        fprintf(stderr, "sub: cannot exec %s: %s\n", c->cmd[0], strerror(errno));
        _exit(127);
    }
    setpgid(kid, kid);
    g_child = kid;
    c->pgid = kid; c->child_start = sub_proc_start(kid);
    entry_write(efd, c);
    signal(SIGTERM, on_sig); signal(SIGINT, on_sig); signal(SIGHUP, on_sig);
    close(gate[0]);
    if (write(gate[1], "g", 1) != 1) { /* the child is gone already; waitpid says how */ }
    close(gate[1]);

    /* Block in waitpid; the deadline is a timer that interrupts it (and
     * keeps interrupting every second, so a tick that lands just before
     * the wait cannot be lost). No polling: a free lane costs a fork. */
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm; sigemptyset(&sa.sa_mask);          /* no SA_RESTART */
    sigaction(SIGALRM, &sa, NULL);
    struct itimerval it = { { 1, 0 }, { c->timeout, 0 } };
    g_alarm = 0;
    setitimer(ITIMER_REAL, &it, NULL);
    double deadline = c->started + c->timeout;
    int st = 0, timed = 0, reaped = 0;
    for (;;) {
        pid_t r = waitpid(kid, &st, 0);
        if (r == kid) { reaped = 1; break; }
        if (r < 0 && errno != EINTR) break;
        if (g_alarm && now_s() >= deadline) {
            struct itimerval off = { { 0, 0 }, { 0, 0 } };
            setitimer(ITIMER_REAL, &off, NULL);
            timed = 1;
            fprintf(stderr, "sub: change #%llu (%s) passed its %ds -- stopping its process group\n",
                    c->ticket, c->what, c->timeout);
            kill(-kid, SIGTERM);
            for (int i = 0; i < 40; i++) {
                if (waitpid(kid, &st, WNOHANG) == kid) { reaped = 1; break; }
                usleep(50000);
            }
            kill(-kid, SIGKILL);      /* whatever of the group is left, leader or not */
            if (!reaped) while (waitpid(kid, &st, 0) < 0 && errno == EINTR) { }
            reaped = 1;
            break;
        }
    }
    { struct itimerval off = { { 0, 0 }, { 0, 0 } }; setitimer(ITIMER_REAL, &off, NULL); }
    g_child = 0;
    if (timed) { c->exit_code = 124; c->status = "timed-out"; }
    else {
        c->exit_code = !reaped ? 1 : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : WEXITSTATUS(st);
        c->status = g_got_sig ? "interrupted" : c->exit_code == 0 ? "ok" : "failed";
    }
done:
    c->ended = now_s();
    {
        sbuf b = {0}; change_json(&b, c); sb_s(&b, "\n");
        if (b.p) lane_log(root, b.p, b.n);
        free(b.p);
    }
    /* unlink before close: closed first, a waiter could lock it and bury a
     * change that ended cleanly */
    if (epath) unlink(epath);
    if (efd >= 0) close(efd);
    return c->exit_code;
}

/* Queue the change, wait its turn, run it. Never refuses: a lane that
 * cannot be made still runs the change, unqueued, and says so. */
static int lane_change_run(lane_change *c) {
    char root[PATH_MAX], q[PATH_MAX];
    lane_root(root, sizeof root);
    snprintf(q, sizeof q, "%s/queue", root);
    if (c->timeout <= 0) c->timeout = LANE_TIMEOUT;
    c->pid = getpid();
    c->queued = now_s();
    who_am_i(c->who, sizeof c->who);

    int lk = -1;
    char lp[PATH_MAX]; snprintf(lp, sizeof lp, "%s/lane.lock", root);
    if (mkdir_p(q) == 0) lk = open(lp, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lk < 0) {
        fprintf(stderr, "sub: no lane at %s (%s) -- running the change unqueued\n", root, strerror(errno));
        return lane_run(c, root, -1, NULL);
    }

    while (flock(lk, LOCK_EX) < 0 && errno == EINTR) { }
    c->ticket = next_ticket(root);

    /* Already inside a running change (a change whose command makes a
     * change)? Then this one is part of it: run now, logged as nested. */
    const char *outer = getenv("SUB_CHANGE_TICKET"), *olane = getenv("SUB_CHANGE_LANE");
    if (outer && olane && !strcmp(olane, root)) {
        char op[PATH_MAX]; entry_path(root, strtoull(outer, NULL, 10), op, sizeof op);
        if (entry_alive(op, NULL) == 1) {
            c->nested_in = strtoull(outer, NULL, 10);
            flock(lk, LOCK_UN); close(lk);
            return lane_run(c, root, -1, NULL);
        }
    }

    char ep[PATH_MAX]; int efd = -1;
    for (int tries = 0; tries < 1000; tries++) {
        entry_path(root, c->ticket, ep, sizeof ep);
        efd = open(ep, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (efd >= 0 || errno != EEXIST) break;
        c->ticket++;                      /* seq was reset under a stale entry */
    }
    if (efd < 0) {
        flock(lk, LOCK_UN); close(lk);
        fprintf(stderr, "sub: cannot queue at %s (%s) -- running the change unqueued\n", ep, strerror(errno));
        return lane_run(c, root, -1, NULL);
    }
    flock(efd, LOCK_EX);
    entry_write(efd, c);
    flock(lk, LOCK_UN);

    int told = 0;
    for (;;) {
        unsigned long long first = 0;
        while (flock(lk, LOCK_EX) < 0 && errno == EINTR) { }
        int ahead = lane_ahead(root, c->ticket, &first);
        flock(lk, LOCK_UN);
        if (!ahead) break;
        if (!told++) fprintf(stderr, "sub: change #%llu (%s) waits its turn behind #%llu\n", c->ticket, c->what, first);
        usleep(LANE_POLL_US);
    }
    close(lk);
    return lane_run(c, root, efd, ep);
}

static int do_change(int argc, char **argv) {
    lane_change c = {0};
    int i = 0;
    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        else if (!strcmp(argv[i], "--undo") && i + 1 < argc) c.undo = argv[++i];
        else if (!strcmp(argv[i], "--why") && i + 1 < argc) c.why = argv[++i];
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) c.timeout = atoi(argv[++i]);
        else if (argv[i][0] != '-' && !c.what) c.what = argv[i];
        else { fprintf(stderr, "sub change: unknown option %s\n", argv[i]); return 2; }
    }
    if (!c.what || !*c.what || i >= argc) {
        fprintf(stderr, "usage: sub change \"<what>\" [--undo \"<cmd>\"] [--why \"<why>\"] [--timeout SECONDS] -- cmd [args]\n");
        return 2;
    }
    c.cmd = &argv[i]; c.ncmd = argc - i;
    return lane_change_run(&c);
}

static int my_exe(char *out, size_t cap, const char *argv0) {
#if defined(__APPLE__)
    uint32_t n = (uint32_t)cap;
    if (_NSGetExecutablePath(out, &n) == 0) return 0;
#else
    ssize_t k = readlink("/proc/self/exe", out, cap - 1);
    if (k > 0) { out[k] = 0; return 0; }
#endif
    snprintf(out, cap, "%s", argv0);
    return 0;
}

/* Inside a running change already? Then a verb that changes the system is
 * part of that change, and runs as it is. */
static int inside_change(void) {
    const char *outer = getenv("SUB_CHANGE_TICKET"), *olane = getenv("SUB_CHANGE_LANE");
    char root[PATH_MAX]; lane_root(root, sizeof root);
    if (!outer || !olane || strcmp(olane, root)) return 0;
    char op[PATH_MAX]; entry_path(root, strtoull(outer, NULL, 10), op, sizeof op);
    return entry_alive(op, NULL) == 1;
}

/* sub enable / disable / stop change the running system, so they go through
 * the lane: this process re-runs itself as the change's command. */
static int through_lane(int argc, char **argv, const char *what, const char *undo) {
    char me[PATH_MAX]; my_exe(me, sizeof me, argv[0]);
    char **cmd = calloc((size_t)argc + 1, sizeof *cmd);
    if (!cmd) return 1;
    cmd[0] = me;
    for (int k = 1; k < argc; k++) cmd[k] = argv[k];
    lane_change c = {0};
    c.what = what; c.undo = undo; c.cmd = cmd; c.ncmd = argc;
    int rc = lane_change_run(&c);
    free(cmd);
    return rc;
}

/* sub restart NAME: kickstart -k the agent sub enable made (or a full
 * launchd label), through the lane. */
static int do_restart(int argc, char **argv) {
    int timeout = 0; const char *name = NULL;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--timeout") && i + 1 < argc) timeout = atoi(argv[++i]);
        else if (argv[i][0] != '-' && !name) name = argv[i];
        else { fprintf(stderr, "sub restart: unknown option %s\n", argv[i]); return 2; }
    }
    if (!name || !*name || strchr(name, '/')) { fprintf(stderr, "usage: sub restart [--timeout SECONDS] NAME|LABEL\n"); return 2; }
    char label[256], plist[PATH_MAX], tgt[320], what[300];
    if (strchr(name, '.')) snprintf(label, sizeof label, "%s", name);
    else {
        snprintf(label, sizeof label, "dev.substrate.%s", name);
        const char *h = getenv("HOME");
        snprintf(plist, sizeof plist, "%s/Library/LaunchAgents/%s.plist", h ? h : "", label);
        if (access(plist, F_OK) != 0) {
            fprintf(stderr, "sub: %s is not enabled (no %s) -- restart restarts what 'sub enable' set up\n", name, plist);
            return 2;
        }
    }
    snprintf(tgt, sizeof tgt, "gui/%u/%s", getuid(), label);
    snprintf(what, sizeof what, "restart %s", name);
    const char *lc = getenv("SUB_LAUNCHCTL");
    char lcb[PATH_MAX]; snprintf(lcb, sizeof lcb, "%s", lc && *lc ? lc : "launchctl");
    char v1[] = "kickstart", v2[] = "-k";
    char *cmd[] = { lcb, v1, v2, tgt, NULL };
    lane_change c = {0};
    c.what = what; c.cmd = cmd; c.ncmd = 4; c.timeout = timeout;
    return lane_change_run(&c);
}

/* sub changes [--last N] [--json] | --running */
static int do_changes(int argc, char **argv) {
    int last = 20, json = 0, running = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--last") && i + 1 < argc) last = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--json")) json = 1;
        else if (!strcmp(argv[i], "--running")) running = 1;
        else { fprintf(stderr, "usage: sub changes [--last N] [--json] | --running\n"); return 2; }
    }
    char root[PATH_MAX]; lane_root(root, sizeof root);
    double now = now_s();
    if (running) {
        unsigned long long *t = NULL;
        int n = queue_tickets(root, &t), shown = 0;
        for (int i = 0; i < n; i++) {
            char p[PATH_MAX]; entry_path(root, t[i], p, sizeof p);
            int fd = open(p, O_RDONLY | O_CLOEXEC);
            if (fd < 0) continue;
            if (flock(fd, LOCK_EX | LOCK_NB) == 0) { close(fd); continue; }   /* nobody holds it: gone */
            char *body = read_fd(fd); close(fd);
            char what[256], who[128];
            jstr(body, "what", what, sizeof what); jstr(body, "who", who, sizeof who);
            double st = jnum(body, "started_s", 0), qd = jnum(body, "queued_s", now);
            if (st > 0) printf("running  #%-6llu %7.1fs  %-24.24s  %s\n", t[i], now - st, who, what);
            else        printf("waiting  #%-6llu %7.1fs  %-24.24s  %s\n", t[i], now - qd, who, what);
            free(body); shown++;
        }
        free(t);
        if (!shown) printf("(no change running)\n");
        return 0;
    }
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s/changes.jsonl", root);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { printf("(no changes logged in %s)\n", p); return 0; }
    char *all = read_fd(fd); close(fd);
    /* the start of the last `last` lines */
    size_t L = strlen(all); int seen = 0; size_t from = 0;
    if (last > 0) {
        for (size_t k = L; k > 0; k--) {
            if (all[k - 1] == '\n' && k != L && ++seen == last) { from = k; break; }
        }
    }
    for (char *line = all + from, *nl; line && *line; line = nl ? nl + 1 : NULL) {
        nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (!*line) continue;
        if (json) { printf("%s\n", line); continue; }
        char what[256], who[128], status[24], when[48];
        jstr(line, "what", what, sizeof what); jstr(line, "who", who, sizeof who);
        jstr(line, "status", status, sizeof status);
        if (!jstr(line, "started", when, sizeof when)) jstr(line, "queued", when, sizeof when);
        if (strlen(when) >= 19) { when[19] = 0; if (when[10] == 'T') when[10] = ' '; }
        double st = jnum(line, "started_s", 0), en = jnum(line, "ended_s", 0);
        char ex[16]; double e = jnum(line, "exit", -1e9);
        if (e < -1e8) snprintf(ex, sizeof ex, "-"); else snprintf(ex, sizeof ex, "%d", (int)e);
        printf("#%-6llu %s  %-11s %4s %7.1fs  %-24.24s  %s\n",
               (unsigned long long)jnum(line, "ticket", 0), when, status, ex,
               st > 0 && en >= st ? en - st : 0.0, who, what);
    }
    free(all);
    return 0;
}

/* ---- the desk host (cut 4 of "the desk is one app", 2026-10-26) ----
 * While the desk's manifest exists (DESK_HOST_MANIFEST, else
 * ~/Obsidian/Desk-Core/Meta/Machine/desk.yaml), enable, disable and
 * restart are the host's: a row in the manifest and a line to Desk.app,
 * through `desk-host`. `sub ps` is unchanged (the registry's shared
 * memory; the registry runs as the app's first child). The way back for
 * one module is `--launchd` (or SUB_LAUNCHD=1): the launchd agent as
 * before. */
static int host_manifest_present(void) {
    const char *e = getenv("SUB_LAUNCHD");
    if (e && *e && strcmp(e, "0")) return 0;
    const char *m = getenv("DESK_HOST_MANIFEST");
    char buf[PATH_MAX];
    if (!m || !*m) {
        const char *h = getenv("HOME");
        snprintf(buf, sizeof buf, "%s/Obsidian/Desk-Core/Meta/Machine/desk.yaml", h ? h : "");
        m = buf;
    }
    return access(m, F_OK) == 0;
}
/* The flag is stripped and SUB_LAUNCHD=1 set in the environment, so the
 * re-exec through the change lane (through_lane runs `sub enable …` again
 * inside the change) keeps the choice. */
static int strip_launchd_flag(int argc, char **argv) {
    int found = 0, w = 0;
    for (int r = 0; r < argc; r++) {
        if (!found && !strcmp(argv[r], "--launchd")) { found = 1; continue; }
        argv[w++] = argv[r];
    }
    argv[w] = NULL;
    if (found) setenv("SUB_LAUNCHD", "1", 1);
    return found;
}
static int to_desk_host(const char *verb, int argc, char **argv) {
    enum { MAXV = 256 };
    char *nv[MAXV + 3];
    char v0[] = "desk-host", v1[32];
    if (argc > MAXV) argc = MAXV;
    snprintf(v1, sizeof v1, "%s", verb);
    nv[0] = v0; nv[1] = v1;
    for (int i = 0; i < argc; i++) nv[2 + i] = argv[i];
    nv[2 + argc] = NULL;
    execvp("desk-host", nv);
    fprintf(stderr, "sub: desk-host is not on the PATH (the manifest exists, so %s is the host's); "
                    "pass --launchd for the launchd agent as before\n", verb);
    return 127;
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
      "  sub enable [--name N] [--role R] [--dir D] [--nameplate FILE]\n"
      "             [--every SECONDS | --at HH:MM[,HH:MM...]] -- cmd [args...]\n"
      "        Same as run, but at login and restarted if it exits, via a\n"
      "        launchd agent. Also enables the registry itself, unless you\n"
      "        pass --no-registry because you start it some other way.\n"
      "        --nameplate: an executable that execs what follows it, put\n"
      "        first so Login Items shows its name instead of \"sub\".\n"
      "        --every / --at: a timer instead of a service -- run on that\n"
      "        schedule to completion (--at :MM means hourly), listed while\n"
      "        it runs, and run even if the registry is away.\n"
      "        --env KEY=VALUE (repeatable): a setting the service reads.\n"
      "        --no-restart: a login app -- started at login, not started\n"
      "        again when it exits (you quit it on purpose).\n"
      "        macOS only.\n"
      "        While the desk's manifest exists (Desk-Core Meta/Machine/\n"
      "        desk.yaml), enable, disable and restart are the desk host's:\n"
      "        a row in the manifest and a line to Desk.app (desk-host).\n"
      "        --launchd (or SUB_LAUNCHD=1): the launchd agent as before.\n"
      "  sub disable [--launchd] NAME\n"
      "        Stop it starting at login and remove its agent.\n"
      "  sub enabled\n"
      "        List what is set to start at login.\n"
      "  sub stop [--force] NAME|PID\n"
      "        SIGTERM it and everything it forked, wait up to 10s.\n"
      "        --force: SIGKILL whatever is left after that.\n"
      "  sub restart [--timeout S] NAME|LABEL\n"
      "        launchctl kickstart -k the agent 'sub enable' made (or a full\n"
      "        launchd label), as a change through the lane.\n"
      "  sub change \"<what>\" [--undo \"<cmd>\"] [--why \"<why>\"] [--timeout S] -- cmd [args...]\n"
      "        A change to the running system: queued, run one at a time in\n"
      "        arrival order, logged. A queue, not a gate -- nothing is refused.\n"
      "        Past --timeout (default %ds) its process group is killed, it is\n"
      "        logged timed-out (exit 124), and the next change runs. enable,\n"
      "        disable, stop and restart go through the same lane.\n"
      "  sub net VERB ...  |  sub keygen\n"
      "        This machine's substrate, reachable from the machines it trusts:\n"
      "        sub-net serve, ps, run, stop, events, listen, connect (sub-net --help).\n"
      "  sub changes [--last N] [--json] | --running\n"
      "        The change log, or the change running now and the queue behind it.\n"
      "        Lane: $SUB_LANE, else ~/Library/Application Support/substrate/changes.\n"
      "\n"
      "The registry is the segment %s (format v%u), owned by sub-dbd.\n"
      "Start it with 'sub-dbd &' and stop it with 'sub-dbd --stop'.\n",
      SUB_NAMELEN - 1, LANE_TIMEOUT, sub_shm_name(), SUB_VERSION);
    return 0;
}

/* sub net VERB ... and sub keygen: the network adapter is its own binary
 * (it needs threads; sub does not), found beside this one. */
static int do_net(char **argv) {
    char me[PATH_MAX], tool[PATH_MAX + 16];
    my_exe(me, sizeof me, "sub");
    char *slash = strrchr(me, '/');
    if (slash) *slash = 0;
    const char *names[] = { "sub-net", "net" };
    for (int k = 0; k < 2; k++) {
        snprintf(tool, sizeof tool, "%s/%s", slash ? me : ".", names[k]);
        if (access(tool, X_OK) == 0) { argv[0] = tool; execv(tool, argv); }
    }
    fprintf(stderr, "sub: no sub-net beside %s\n", slash ? me : "this sub");
    return 127;
}

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    if (!strcmp(argv[1], "net") && argc > 2) return do_net(argv + 1);
    if (!strcmp(argv[1], "keygen")) return do_net(argv);
    if (!strcmp(argv[1], "ps"))   return do_ps();
    if (!strcmp(argv[1], "run"))  return do_run(argc - 2, argv + 2);
    if (!strcmp(argv[1], "change"))  return do_change(argc - 2, argv + 2);
    if (!strcmp(argv[1], "changes")) return do_changes(argc - 2, argv + 2);
    if (!strcmp(argv[1], "restart")) {
        int launchd_flag = strip_launchd_flag(argc - 2, argv + 2);
        if (launchd_flag) argc--;
        if (!launchd_flag && host_manifest_present()) return to_desk_host("restart", argc - 2, argv + 2);
        return do_restart(argc - 2, argv + 2);
    }
    if (!strcmp(argv[1], "enable")) {
        int launchd_flag = strip_launchd_flag(argc - 2, argv + 2);
        if (launchd_flag) argc--;
        if (!launchd_flag && host_manifest_present()) return to_desk_host("enable", argc - 2, argv + 2);
        if (!inside_change() && argc > 2) {
            /* what: "enable NAME", NAME as do_enable would choose it */
            const char *nm = NULL;
            for (int k = 2; k < argc; k++) {
                if (!strcmp(argv[k], "--")) { if (!nm && k + 1 < argc) nm = base_of(argv[k + 1]); break; }
                if (!strcmp(argv[k], "--name") && k + 1 < argc) { nm = argv[k + 1]; break; }
            }
            char what[200], undo[200];
            snprintf(what, sizeof what, "enable %s", nm ? nm : "?");
            snprintf(undo, sizeof undo, "sub disable %s", nm ? nm : "?");
            return through_lane(argc, argv, what, undo);
        }
        return do_enable(argc - 2, argv + 2);
    }
    if (!strcmp(argv[1], "enabled")) return do_enabled();
    if (!strcmp(argv[1], "disable")) {
        int launchd_flag = strip_launchd_flag(argc - 2, argv + 2);
        if (launchd_flag) argc--;
        if (argc < 3) { fprintf(stderr, "usage: sub disable [--launchd] NAME\n"); return 2; }
        if (!launchd_flag && host_manifest_present()) return to_desk_host("disable", argc - 2, argv + 2);
        if (!inside_change()) {
            char what[200]; snprintf(what, sizeof what, "disable %s", argv[2]);
            return through_lane(argc, argv, what, NULL);
        }
        return do_disable(argv[2]);
    }
    if (!strcmp(argv[1], "stop")) {
        int force = argc > 2 && !strcmp(argv[2], "--force");
        if (argc < 3 + force) {
            fprintf(stderr, "usage: sub stop [--force] NAME|PID\n");
            return 2;
        }
        if (!inside_change()) {
            char what[200]; snprintf(what, sizeof what, "stop %s", argv[2 + force]);
            return through_lane(argc, argv, what, NULL);
        }
        return do_stop(argv[2 + force], force);
    }
    if (!strcmp(argv[1], "--version")) { printf("sub %s (format v%u)\n", SUB_RELEASE, SUB_VERSION); return 0; }
    if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h") || !strcmp(argv[1], "help"))
        return usage();
    fprintf(stderr, "sub: unknown verb '%s' (try: sub --help)\n", argv[1]);
    return 2;
}
