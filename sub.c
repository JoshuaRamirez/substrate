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
#include <limits.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/stat.h>
#include <dirent.h>
#endif

static substrate C;
static pid_t g_child = 0;

/* Forward what we are told to the thing we are supervising. The wrapper is a
 * conduit, not a policy: it does not decide that SIGTERM means stop. */
/* The child leads its own process group (see do_run), so forward to the
 * GROUP: a service's workers are part of the service. */
static void on_sig(int s) {
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

    int st = 0;
    while (waitpid(kid, &st, 0) < 0 && errno == EINTR) { /* a forwarded signal */ }
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
        if (!realpath(cmd, out)) return -1;
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

/* Write a plist, atomically: launchd may be reading the directory. */
static int write_plist(const char *name, char *const *args, int n, const char *wd) {
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
    fputs("  <key>RunAtLoad</key><true/>\n  <key>KeepAlive</key><true/>\n", f);
    if (wd) { fputs("  <key>WorkingDirectory</key><string>", f); xml_out(f, wd); fputs("</string>\n", f); }
    for (int i = 0; i < 2; i++) {
        fprintf(f, "  <key>Standard%sPath</key><string>", i ? "Error" : "Out");
        xml_out(f, logs); fputc('/', f); xml_out(f, name); fputs(".log</string>\n", f);
    }
    /* Give the agent the PATH this shell has. Without it, a service that
     * shells out finds a four-entry PATH and fails in a confusing place. */
    const char *pth = getenv("PATH");
    if (pth) {
        fputs("  <key>EnvironmentVariables</key><dict><key>PATH</key><string>", f);
        xml_out(f, pth); fputs("</string></dict>\n", f);
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
    if (write_plist("registry", args, 1, NULL) < 0) return -1;
    bootout(SUB_LABEL_PFX "registry");
    if (bootstrap(plist) != 0) {
        fprintf(stderr, "sub: launchctl could not load the registry agent\n");
        return -1;
    }
    printf("sub: registry agent enabled (" SUB_LABEL_PFX "registry)\n");
    return 0;
}

static int do_enable(int argc, char **argv) {
    const char *name = NULL, *role = "service", *dir = NULL;
    int i = 0, waitsec = 60, want_registry = 1;
    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--role") && i + 1 < argc) role = argv[++i];
        else if (!strcmp(argv[i], "--dir")  && i + 1 < argc) dir  = argv[++i];
        else if (!strcmp(argv[i], "--wait") && i + 1 < argc) waitsec = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--no-registry")) want_registry = 0;
        else { fprintf(stderr, "sub enable: unknown option %s\n", argv[i]); return 2; }
    }
    if (i >= argc) {
        fprintf(stderr, "usage: sub enable [--name N] [--role R] [--dir D] -- cmd [args]\n");
        return 2;
    }
    if (!name) name = base_of(argv[i]);
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
         v_wait[] = "--wait", v_end[] = "--";
    char nm[SUB_NAMELEN * 4], rl[SUB_NAMELEN * 4];
    snprintf(nm, sizeof nm, "%s", name);
    snprintf(rl, sizeof rl, "%s", role);
    char *args[64];
    int n = 0;
    args[n++] = me;
    args[n++] = v_run;
    args[n++] = v_name; args[n++] = nm;
    args[n++] = v_role; args[n++] = rl;
    args[n++] = v_wait; args[n++] = ws;
    args[n++] = v_end;
    args[n++] = cmd;
    for (int k = i + 1; k < argc && n < 63; k++) args[n++] = argv[k];
    args[n] = NULL;

    if (want_registry && ensure_registry() < 0) return 1;

    char plist[PATH_MAX], label[256];
    agent_path(name, plist, sizeof plist);
    snprintf(label, sizeof label, SUB_LABEL_PFX "%s", name);
    if (write_plist(name, args, n, dir) < 0) return 1;
    bootout(label);                       /* re-enabling is allowed */
    if (bootstrap(plist) != 0) {
        fprintf(stderr, "sub: launchctl could not load %s\n", plist);
        return 1;
    }
    printf("sub: %s enabled -- starts at login, restarts if it exits\n", name);
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
      "  sub enable [--name N] [--role R] [--dir D] -- cmd [args...]\n"
      "        Same as run, but at login and restarted if it exits, via a\n"
      "        launchd agent. Also enables the registry itself, unless you\n"
      "        pass --no-registry because you start it some other way.\n"
      "        macOS only.\n"
      "  sub disable NAME\n"
      "        Stop it starting at login and remove its agent.\n"
      "  sub enabled\n"
      "        List what is set to start at login.\n"
      "  sub stop [--force] NAME|PID\n"
      "        SIGTERM it and everything it forked, wait up to 10s.\n"
      "        --force: SIGKILL whatever is left after that.\n"
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
    if (!strcmp(argv[1], "enable"))  return do_enable(argc - 2, argv + 2);
    if (!strcmp(argv[1], "enabled")) return do_enabled();
    if (!strcmp(argv[1], "disable")) {
        if (argc < 3) { fprintf(stderr, "usage: sub disable NAME\n"); return 2; }
        return do_disable(argv[2]);
    }
    if (!strcmp(argv[1], "stop")) {
        int force = argc > 2 && !strcmp(argv[2], "--force");
        if (argc < 3 + force) {
            fprintf(stderr, "usage: sub stop [--force] NAME|PID\n");
            return 2;
        }
        return do_stop(argv[2 + force], force);
    }
    if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h") || !strcmp(argv[1], "help"))
        return usage();
    fprintf(stderr, "sub: unknown verb '%s' (try: sub --help)\n", argv[1]);
    return 2;
}
