/* top -- a native dashboard for the pipeline. Docker Desktop, minus Docker.
 *
 * It does not run `ps`. It does not poll a daemon. It does not speak a
 * protocol. It reads the peer table out of the same shared page every peer
 * writes its own row into, and it asks the OS whether each pid is still
 * alive, when it started and what it costs. That is the whole implementation.
 *
 * Immediate mode: the frame loop is the subscription.
 *
 * Two views. A fixed header (the segment, the db path, the headline: live
 * peers of registered) and, under it, a native scroll view whose document is
 * the table: one row per registered peer, then one greyed row per job that is
 * enabled at login (~/Library/LaunchAgents/dev.substrate.*.plist, SUB_AGENTS
 * in a test) but has no live row right now -- a timer between ticks, with its
 * next run; a service that is down. Columns size to their content.
 *
 * It also shows where the owner persists, and can move it: the panel writes a
 * path into the same page, and dbd acts on it. The dashboard does not talk to
 * the database. They share a variable.
 */
#import <Cocoa/Cocoa.h>
#include "substrate.h"

static sub_seg  *SEG   = NULL;     /* NULL when no owner is running */
static sub_peer *SELF  = NULL;
static int       ATTACH_ERR = 1;

#define ROW_H    30.0
#define HEAD_H   104.0
#define PAD      18.0
#define GAP      22.0
#define MAX_IDLE 512

static NSRect g_stop[SUB_MAX_PEERS];      /* hit boxes, rebuilt each frame */
static pid_t  g_stop_pid[SUB_MAX_PEERS];
static int    g_stop_slot[SUB_MAX_PEERS]; /* which row, so the click can re-check */
static int    g_stop_n = 0;
static NSRect g_move;                     /* the "move..." hit box */
static int    g_move_on = 0;
static NSString *g_note = nil;            /* last refusal, shown under the path */

/* Long paths lose their middle, not their name. */
static NSString *elide(NSString *s, NSUInteger cap) {
    if ([s length] <= cap) return s;
    NSUInteger head = cap / 3, tail = cap - head - 1;
    return [NSString stringWithFormat:@"%@...%@",
            [s substringToIndex:head], [s substringFromIndex:[s length] - tail]];
}

static time_t now_s(void) {                /* SUB_NOW pins the clock for a test */
    const char *e = getenv("SUB_NOW");
    return (e && *e) ? (time_t)strtoll(e, NULL, 10) : time(NULL);
}

static NSString *age_str(long s) {
    if (s < 0) s = 0;
    if (s < 60)    return [NSString stringWithFormat:@"%lds", s];
    if (s < 3600)  return [NSString stringWithFormat:@"%ldm %lds", s / 60, s % 60];
    if (s < 86400) return [NSString stringWithFormat:@"%ldh %ldm", s / 3600, (s % 3600) / 60];
    return [NSString stringWithFormat:@"%ldd %ldh", s / 86400, (s % 86400) / 3600];
}

/* UP is the process's own age (the OS says when it started); the row's claim
 * time is the fallback, for a pid the OS will not describe. */
static NSString *uptime_str(pid_t pid, uint64_t since) {
    uint64_t start = sub_proc_start(pid);
    if (!start) start = since;
    if (!start) return @"-";
    return age_str((long)(time(NULL) - (time_t)start));
}

static NSString *mem_str(uint64_t b) {
    if (!b) return @"-";
    if (b < 1024ull * 1024) return [NSString stringWithFormat:@"%llu KB", (unsigned long long)(b / 1024)];
    if (b < 1024ull * 1024 * 1024) return [NSString stringWithFormat:@"%.0f MB", b / 1048576.0];
    return [NSString stringWithFormat:@"%.1f GB", b / 1073741824.0];
}

static NSString *cpu_str(uint64_t ns) {
    if (!ns) return @"-";
    double s = ns / 1e9;
    if (s < 60) return [NSString stringWithFormat:@"%.1fs", s];
    return age_str((long)s);
}

/* ---- the jobs enabled at login, read from their plists ---- */

typedef struct {
    char name[SUB_NAMELEN], role[SUB_NAMELEN];
    int  every;                 /* StartInterval seconds, or 0 */
    int  ncal;                  /* StartCalendarInterval entries */
    int  hour[64], minute[64], weekday[64], day[64];   /* -1 = any */
    int  keepalive, runatload;
} sub_job;

static NSString *agents_dir(void) {
    const char *e = getenv("SUB_AGENTS");
    if (e && *e) return [NSString stringWithUTF8String:e];
    return [NSHomeDirectory() stringByAppendingPathComponent:@"Library/LaunchAgents"];
}

static int cal_int(NSDictionary *d, NSString *k) {
    id v = d[k];
    return [v respondsToSelector:@selector(intValue)] ? [v intValue] : -1;
}

/* One plist -> one job. The name is --name's value in the sub run line (else
 * the label's tail), the role --role's (else service). */
static int job_from_plist(NSString *path, sub_job *j) {
    NSDictionary *d = [NSDictionary dictionaryWithContentsOfFile:path];
    if (!d) return 0;
    memset(j, 0, sizeof *j);
    NSString *label = d[@"Label"] ?: [[path lastPathComponent] stringByDeletingPathExtension];
    NSString *tail = [label hasPrefix:@"dev.substrate."] ? [label substringFromIndex:14] : label;
    snprintf(j->name, SUB_NAMELEN, "%s", [tail UTF8String]);
    snprintf(j->role, SUB_NAMELEN, "service");
    NSArray *args = d[@"ProgramArguments"];
    for (NSUInteger i = 0; i + 1 < [args count]; i++) {
        if ([args[i] isEqualToString:@"--"]) break;
        if ([args[i] isEqualToString:@"--name"]) snprintf(j->name, SUB_NAMELEN, "%s", [args[i + 1] UTF8String]);
        if ([args[i] isEqualToString:@"--role"]) snprintf(j->role, SUB_NAMELEN, "%s", [args[i + 1] UTF8String]);
    }
    j->keepalive = [d[@"KeepAlive"] respondsToSelector:@selector(boolValue)] && [d[@"KeepAlive"] boolValue];
    j->runatload = [d[@"RunAtLoad"] respondsToSelector:@selector(boolValue)] && [d[@"RunAtLoad"] boolValue];
    j->every = cal_int(d, @"StartInterval") > 0 ? cal_int(d, @"StartInterval") : 0;
    id cal = d[@"StartCalendarInterval"];
    NSArray *items = [cal isKindOfClass:[NSArray class]] ? cal : cal ? @[cal] : @[];
    for (NSDictionary *e in items) {
        if (j->ncal >= 64 || ![e isKindOfClass:[NSDictionary class]]) break;
        j->hour[j->ncal] = cal_int(e, @"Hour"); j->minute[j->ncal] = cal_int(e, @"Minute");
        j->weekday[j->ncal] = cal_int(e, @"Weekday"); j->day[j->ncal] = cal_int(e, @"Day");
        j->ncal++;
    }
    return 1;
}

static int read_jobs(sub_job *out, int cap) {
    NSArray *names = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:agents_dir() error:nil];
    int n = 0;
    for (NSString *f in [names sortedArrayUsingSelector:@selector(compare:)]) {
        if (n >= cap) break;
        if (![f hasPrefix:@"dev.substrate."] || ![f hasSuffix:@".plist"]) continue;
        if ([f isEqualToString:@"dev.substrate.registry.plist"]) continue;   /* the owner's own agent; its row is dbd's */
        if (job_from_plist([agents_dir() stringByAppendingPathComponent:f], &out[n])) n++;
    }
    return n;
}

/* The next time a calendar schedule fires after `from`: launchd's semantics
 * for the keys the desk uses (Minute, Hour, Day, Weekday; a missing key means
 * every). Minute-only is hourly. 0 when nothing matches within 400 days. */
static time_t next_calendar(const sub_job *j, time_t from) {
    time_t best = 0;
    for (int e = 0; e < j->ncal; e++) {
        int H = j->hour[e], M = j->minute[e] < 0 ? 0 : j->minute[e], WD = j->weekday[e], DY = j->day[e];
        for (int dd = 0; dd < 400 && (!best || 1); dd++) {
            time_t day = from + (time_t)dd * 86400;
            struct tm t; localtime_r(&day, &t);
            if (WD >= 0 && t.tm_wday != WD % 7) continue;
            if (DY >= 0 && t.tm_mday != DY) continue;
            int found = 0; time_t cand = 0;
            if (H >= 0) {
                t.tm_hour = H; t.tm_min = M; t.tm_sec = 0; cand = mktime(&t);
                found = cand > from;
            } else {                                     /* hourly at :M */
                for (int h = 0; h < 24 && !found; h++) {
                    t.tm_hour = h; t.tm_min = M; t.tm_sec = 0; cand = mktime(&t);
                    found = cand > from;
                }
            }
            if (found) { if (!best || cand < best) best = cand; break; }
        }
    }
    return best;
}

/* What the UP column says for a job with no live row. */
static NSString *idle_str(const sub_job *j, time_t from) {
    if (j->ncal) {
        time_t n = next_calendar(j, from);
        if (!n) return @"next: ?";
        struct tm t; localtime_r(&n, &t);
        struct tm f; localtime_r(&from, &f);
        char b[32];
        if (t.tm_yday == f.tm_yday && t.tm_year == f.tm_year) strftime(b, sizeof b, "next %H:%M", &t);
        else strftime(b, sizeof b, "next %a %H:%M", &t);
        return [NSString stringWithUTF8String:b];
    }
    if (j->every) {
        if (j->every % 3600 == 0) return [NSString stringWithFormat:@"every %dh", j->every / 3600];
        if (j->every % 60 == 0)   return [NSString stringWithFormat:@"every %dm", j->every / 60];
        return [NSString stringWithFormat:@"every %ds", j->every];
    }
    if (j->keepalive) return @"down";
    return @"at login";
}

/* ---- the header: fixed, above the table ---- */

@interface HeadView : NSView
@end

@implementation HeadView

- (NSDictionary *)mono:(CGFloat)sz white:(CGFloat)w {
    return @{ NSFontAttributeName : [NSFont monospacedSystemFontOfSize:sz weight:NSFontWeightRegular],
              NSForegroundColorAttributeName : [NSColor colorWithCalibratedWhite:w alpha:1.0] };
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSRect b = self.bounds;
    CGFloat W = b.size.width, H = b.size.height;
    [[NSColor colorWithCalibratedWhite:0.09 alpha:1.0] setFill];
    NSRectFill(b);

    NSString *title = @"PIPELINE";
    [title drawAtPoint:NSMakePoint(PAD, H - 34)
        withAttributes:@{ NSFontAttributeName : [NSFont systemFontOfSize:13 weight:NSFontWeightBold],
                          NSForegroundColorAttributeName : [NSColor colorWithCalibratedWhite:0.62 alpha:1.0] }];

    NSString *sub = SEG ? [NSString stringWithFormat:@"%s  format v%u  %d slots  count %llu",
                                    sub_shm_name(), SUB_VERSION, SUB_MAX_PEERS,
                                    (unsigned long long)atomic_load(&SEG->count)]
                        : @"no owner attached";
    [sub drawAtPoint:NSMakePoint(PAD, H - 54) withAttributes:[self mono:10 white:0.40]];

    g_move_on = 0;
    if (!SEG) return;

    char db[SUB_PATHLEN];
    NSString *dbs = sub_path_read(SEG, db, sizeof db) && db[0] ? [NSString stringWithUTF8String:db] : @"(unknown)";
    NSString *line = [NSString stringWithFormat:@"db  %@", elide(dbs, 58)];
    [line drawAtPoint:NSMakePoint(PAD, H - 74) withAttributes:[self mono:10 white:0.52]];
    NSSize ls = [line sizeWithAttributes:[self mono:10 white:0.52]];
    g_move = NSMakeRect(PAD + ls.width + 12, H - 78, 58, 19);
    g_move_on = 1;
    [[NSColor colorWithCalibratedWhite:0.19 alpha:1.0] setFill];
    [[NSBezierPath bezierPathWithRoundedRect:g_move xRadius:5 yRadius:5] fill];
    NSDictionary *ma = @{ NSFontAttributeName : [NSFont systemFontOfSize:10 weight:NSFontWeightMedium],
                          NSForegroundColorAttributeName : [NSColor colorWithCalibratedWhite:0.72 alpha:1.0] };
    NSSize ms2 = [@"move..." sizeWithAttributes:ma];
    [@"move..." drawAtPoint:NSMakePoint(NSMidX(g_move) - ms2.width / 2, NSMidY(g_move) - ms2.height / 2) withAttributes:ma];
    if (g_note)
        [g_note drawAtPoint:NSMakePoint(PAD, H - 92)
             withAttributes:@{ NSFontAttributeName : [NSFont monospacedSystemFontOfSize:9 weight:NSFontWeightRegular],
                               NSForegroundColorAttributeName : [NSColor colorWithCalibratedRed:0.83 green:0.42 blue:0.42 alpha:1] }];

    /* the headline: live peers, of registered. The demo counter is in the sub line. */
    int live = 0, reg = 0;
    for (int i = 0; i < SUB_MAX_PEERS; i++) {
        sub_peer *p = &SEG->peers[i];
        if (atomic_load(&p->state) != 1) continue;
        reg++; if (sub_peer_alive(p)) live++;
    }
    NSString *cnt = [NSString stringWithFormat:@"%d", live];
    NSDictionary *big = @{ NSFontAttributeName : [NSFont monospacedDigitSystemFontOfSize:34 weight:NSFontWeightBold],
                           NSForegroundColorAttributeName : [NSColor whiteColor] };
    NSSize cs = [cnt sizeWithAttributes:big];
    [cnt drawAtPoint:NSMakePoint(W - PAD - cs.width, H - 52) withAttributes:big];
    NSString *cl = [NSString stringWithFormat:@"live peers · %d registered", reg];
    NSSize cls = [cl sizeWithAttributes:[self mono:10 white:0.40]];
    [cl drawAtPoint:NSMakePoint(W - PAD - cls.width, H - 66) withAttributes:[self mono:10 white:0.40]];
}

- (void)mouseDown:(NSEvent *)e {
    NSPoint pt = [self convertPoint:e.locationInWindow fromView:nil];
    if (g_move_on && NSPointInRect(pt, g_move)) [self moveDatabase];
}

/* The dashboard does not ask dbd to move. It writes the path and goes back to
 * drawing; dbd notices within 100ms. If dbd cannot open the file it writes the
 * old path back, and the row below reverts on its own. */
- (void)moveDatabase {
    if (!SEG) return;
    char cur[SUB_PATHLEN];
    NSSavePanel *sp = [NSSavePanel savePanel];
    [sp setTitle:@"Move the substrate database"];
    [sp setPrompt:@"Move"];
    [sp setNameFieldStringValue:@"substrate.db"];
    if (sub_path_read(SEG, cur, sizeof cur) && cur[0]) {
        NSString *c = [NSString stringWithUTF8String:cur];
        [sp setDirectoryURL:[NSURL fileURLWithPath:[c stringByDeletingLastPathComponent]]];
        [sp setNameFieldStringValue:[c lastPathComponent]];
    }
    if ([sp runModal] == NSModalResponseOK) {
        int rc = sub_path_write(SEG, [[[sp URL] path] UTF8String]);
        g_note = rc == 0 ? nil : [NSString stringWithFormat:@"refused: %s", sub_path_error(rc)];
    }
    [self setNeedsDisplay:YES];
}

@end

/* ---- the table: the scroll view's document, top-down ---- */

enum { C_NAME, C_ROLE, C_PID, C_UP, C_CPU, C_MEM, NCOL };
static const char *COLH[NCOL] = { "NAME", "ROLE", "PID", "UP", "CPU", "MEM" };

typedef struct {
    NSString *cell[NCOL];
    int alive, me, idle, slot;
    pid_t pid;
} row_t;

@interface TopView : NSView
- (void)attachIfNeeded;
- (int)rows:(row_t *)rows cap:(int)cap;
@end

@implementation TopView

- (BOOL)isFlipped { return YES; }       /* row 0 at the top, the document grows downward */

- (NSDictionary *)mono:(CGFloat)sz white:(CGFloat)w {
    return @{ NSFontAttributeName : [NSFont monospacedSystemFontOfSize:sz weight:NSFontWeightRegular],
              NSForegroundColorAttributeName : [NSColor colorWithCalibratedWhite:w alpha:1.0] };
}

- (void)attachIfNeeded {
    if (SEG && !sub_owner_alive(SEG)) {     /* the owner left; stop trusting the page */
        munmap(SEG, sizeof(sub_seg));
        SEG = NULL; SELF = NULL; g_stop_n = 0;
    }
    if (SEG) return;
    SEG = sub_attach(&ATTACH_ERR);
    if (SEG) SELF = sub_claim_slot(SEG, "top", "dash");
}

/* The rows: every registered peer, then every enabled job with no live row of
 * its name. Returns how many; the document's height follows. */
- (int)rows:(row_t *)rows cap:(int)cap {
    int n = 0;
    char live_names[SUB_MAX_PEERS][SUB_NAMELEN]; int nlive = 0;
    for (int i = 0; i < SUB_MAX_PEERS && n < cap; i++) {
        sub_peer *p = &SEG->peers[i];
        if (atomic_load(&p->state) != 1) continue;
        row_t *r = &rows[n++];
        r->pid = (pid_t)atomic_load(&p->pid);
        r->alive = sub_peer_alive(p); r->me = (SELF && p == SELF); r->idle = 0; r->slot = i;
        uint64_t rss, cpu; sub_proc_cost(r->pid, &rss, &cpu);
        r->cell[C_NAME] = r->me ? [NSString stringWithFormat:@"%s *", p->name] : [NSString stringWithUTF8String:p->name];
        r->cell[C_ROLE] = [NSString stringWithUTF8String:p->role];
        r->cell[C_PID]  = [NSString stringWithFormat:@"%d", r->pid];
        r->cell[C_UP]   = uptime_str(r->pid, atomic_load(&p->since));
        r->cell[C_CPU]  = cpu_str(cpu);
        r->cell[C_MEM]  = mem_str(rss);
        if (r->alive && nlive < SUB_MAX_PEERS) snprintf(live_names[nlive++], SUB_NAMELEN, "%s", p->name);
    }
    static sub_job jobs[MAX_IDLE];
    int nj = read_jobs(jobs, MAX_IDLE);
    time_t from = now_s();
    for (int k = 0; k < nj && n < cap; k++) {
        int seen = 0;
        for (int l = 0; l < nlive; l++) if (!strcmp(live_names[l], jobs[k].name)) { seen = 1; break; }
        if (seen) continue;
        row_t *r = &rows[n++];
        r->pid = 0; r->alive = 0; r->me = 0; r->idle = 1; r->slot = -1;
        r->cell[C_NAME] = [NSString stringWithUTF8String:jobs[k].name];
        r->cell[C_ROLE] = [NSString stringWithUTF8String:jobs[k].role];
        r->cell[C_PID]  = @"-";
        r->cell[C_UP]   = idle_str(&jobs[k], from);
        r->cell[C_CPU]  = @"-";
        r->cell[C_MEM]  = @"-";
    }
    return n;
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSRect b = self.bounds;
    CGFloat W = b.size.width;
    [[NSColor colorWithCalibratedWhite:0.09 alpha:1.0] setFill];
    NSRectFill(b);

    [self attachIfNeeded];
    g_stop_n = 0;

    if (!SEG) {
        NSString *m = @"start the owner:   ./bin/dbd";
        NSDictionary *a = [self mono:13 white:0.45];
        NSSize ms = [m sizeWithAttributes:a];
        [m drawAtPoint:NSMakePoint((W - ms.width) / 2, 60) withAttributes:a];
        [self fitRows:0];
        return;
    }

    static row_t rows[SUB_MAX_PEERS + MAX_IDLE];
    int n = [self rows:rows cap:SUB_MAX_PEERS + MAX_IDLE];
    [self fitRows:n];

    /* columns size to their content: the widest cell or header in each */
    NSDictionary *cell = [self mono:12 white:0.55], *hd = [self mono:10 white:0.38];
    CGFloat colw[NCOL], x[NCOL], cx = PAD + 22;
    for (int c = 0; c < NCOL; c++) {
        colw[c] = [[NSString stringWithUTF8String:COLH[c]] sizeWithAttributes:hd].width;
        for (int i = 0; i < n; i++) {
            CGFloat w = [rows[i].cell[c] sizeWithAttributes:cell].width;
            if (w > colw[c]) colw[c] = w;
        }
        x[c] = cx; cx += colw[c] + GAP;
    }

    CGFloat y = 6;
    for (int c = 0; c < NCOL; c++)
        [[NSString stringWithUTF8String:COLH[c]] drawAtPoint:NSMakePoint(x[c], y) withAttributes:hd];
    [[NSColor colorWithCalibratedWhite:0.20 alpha:1.0] setFill];
    NSRectFill(NSMakeRect(PAD, y + 18, W - 2 * PAD, 1));

    for (int i = 0; i < n; i++) {
        row_t *r = &rows[i];
        CGFloat ry = y + 26 + i * ROW_H;
        if (i % 2 == 0) {
            [[NSColor colorWithCalibratedWhite:0.12 alpha:1.0] setFill];
            NSRectFill(NSMakeRect(PAD, ry - 6, W - 2 * PAD, ROW_H - 2));
        }
        NSColor *dot = r->alive ? [NSColor colorWithCalibratedRed:0.20 green:0.80 blue:0.40 alpha:1]
                     : r->idle  ? [NSColor colorWithCalibratedWhite:0.22 alpha:1]
                                : [NSColor colorWithCalibratedWhite:0.35 alpha:1];
        [dot setFill];
        [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(PAD + 2, ry + 4, 8, 8)] fill];
        CGFloat wh = r->alive ? 0.92 : r->idle ? 0.40 : 0.38;
        CGFloat wd = r->idle ? 0.34 : 0.55;
        for (int c = 0; c < NCOL; c++)
            [r->cell[c] drawAtPoint:NSMakePoint(x[c], ry) withAttributes:[self mono:12 white:(c == C_NAME ? wh : wd)]];

        if (r->alive && !r->me) {
            NSRect sb = NSMakeRect(W - PAD - 62, ry - 2, 62, 22);
            [[NSColor colorWithCalibratedRed:0.42 green:0.15 blue:0.17 alpha:1.0] setFill];
            [[NSBezierPath bezierPathWithRoundedRect:sb xRadius:5 yRadius:5] fill];
            NSDictionary *sa = @{ NSFontAttributeName : [NSFont systemFontOfSize:11 weight:NSFontWeightMedium],
                                  NSForegroundColorAttributeName : [NSColor colorWithCalibratedRed:1.0 green:0.72 blue:0.72 alpha:1.0] };
            NSSize ss = [@"stop" sizeWithAttributes:sa];
            [@"stop" drawAtPoint:NSMakePoint(NSMidX(sb) - ss.width / 2, NSMidY(sb) - ss.height / 2) withAttributes:sa];
            g_stop[g_stop_n] = sb; g_stop_pid[g_stop_n] = r->pid; g_stop_slot[g_stop_n] = r->slot; g_stop_n++;
        }
    }
    if (n == 0)
        [@"no peers" drawAtPoint:NSMakePoint(PAD + 22, y + 30) withAttributes:[self mono:12 white:0.35]];

    NSString *ft = @"rows are shm slots.  liveness is kill(pid,0).  UP, CPU, MEM are the OS's for that pid.  grey = enabled at login, not running now.  * = this window";
    [ft drawAtPoint:NSMakePoint(PAD, y + 26 + n * ROW_H + 8) withAttributes:[self mono:9 white:0.28]];
}

/* The document is as tall as its rows (plus the footer), never shorter than
 * the clip view, so the scroll view scrolls exactly when there is more. */
- (void)fitRows:(int)n {
    CGFloat want = 6 + 26 + n * ROW_H + 40;
    NSScrollView *sv = [self enclosingScrollView];
    CGFloat minh = sv ? sv.contentSize.height : 0, w = sv ? sv.contentSize.width : self.bounds.size.width;
    if (want < minh) want = minh;
    if (fabs(self.frame.size.height - want) > 0.5 || fabs(self.frame.size.width - w) > 0.5)
        [self setFrameSize:NSMakeSize(w, want)];
}

- (void)mouseDown:(NSEvent *)e {
    NSPoint pt = [self convertPoint:e.locationInWindow fromView:nil];
    for (int i = 0; i < g_stop_n; i++) {
        if (NSPointInRect(pt, g_stop[i])) {
            /* The row could have been reaped between the frame that drew this
             * button and this click, and the OS recycles pids. Signal only if
             * the slot STILL holds the pid we drew. */
            sub_peer *p = SEG && g_stop_slot[i] >= 0 ? &SEG->peers[g_stop_slot[i]] : NULL;
            if (!p || atomic_load(&p->state) != 1 || (pid_t)atomic_load(&p->pid) != g_stop_pid[i]) {
                [self setNeedsDisplay:YES];
                return;
            }
            kill(g_stop_pid[i], SIGTERM);
            [self setNeedsDisplay:YES];
            return;
        }
    }
}

@end

/* --selftest: the same data path, no window. The first line is what the test
 * suite has always read; the idle rows follow, one per line, so the schedule
 * reading is testable with SUB_AGENTS and SUB_NOW. */
static int selftest(void) {
    SEG = sub_attach(&ATTACH_ERR);
    if (!SEG) { printf("top selftest: no owner (err=%d)\n", ATTACH_ERR); return 2; }
    int live = 0, reg = 0;
    for (int k = 0; k < SUB_MAX_PEERS; k++) {
        if (atomic_load(&SEG->peers[k].state) != 1) continue;
        reg++; if (sub_peer_alive(&SEG->peers[k])) live++;
    }
    char db[SUB_PATHLEN];
    if (!sub_path_read(SEG, db, sizeof db)) snprintf(db, sizeof db, "(unknown)");
    printf("top selftest: peers=%d count=%llu\n", live, (unsigned long long)atomic_load(&SEG->count));
    printf("top selftest: db=%s\n", db);
    printf("top selftest: registered=%d\n", reg);
    static sub_job jobs[MAX_IDLE];
    int nj = read_jobs(jobs, MAX_IDLE);
    time_t from = now_s();
    for (int k = 0; k < nj; k++) {
        int seen = 0;
        for (int i = 0; i < SUB_MAX_PEERS && !seen; i++)
            if (sub_peer_alive(&SEG->peers[i]) && !strcmp(SEG->peers[i].name, jobs[k].name)) seen = 1;
        if (!seen) printf("top selftest: idle %s %s %s\n", jobs[k].name, jobs[k].role, [idle_str(&jobs[k], from) UTF8String]);
    }
    return 0;
}

/* --render PNG: the table drawn offscreen, no window, the same data path as
 * the frame loop. It prints the document's height before the draw, after it,
 * and what its rows need. AppKit's rule for a view is that a draw never
 * changes geometry: a document whose frame is set from inside drawRect: is
 * resized while its layer is being displayed, which AppKit may refuse with an
 * exception it then crashes on (+[NSApplication _crashOnException:]). So the
 * frame must already fit the rows before the draw, and be the same after. */
static int render(const char *png) {
    NSRect fr = NSMakeRect(0, 0, 760, 456);
    TopView *v = [[TopView alloc] initWithFrame:fr];
    [v attachIfNeeded];
    static row_t tmp[SUB_MAX_PEERS + MAX_IDLE];
    int n = SEG ? [v rows:tmp cap:SUB_MAX_PEERS + MAX_IDLE] : 0;
    CGFloat want = 6 + 26 + n * ROW_H + 40;
    CGFloat before = v.frame.size.height;
    NSRect b = v.bounds;
    NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:NULL pixelsWide:(NSInteger)b.size.width pixelsHigh:(NSInteger)b.size.height
        bitsPerSample:8 samplesPerPixel:4 hasAlpha:YES isPlanar:NO colorSpaceName:NSCalibratedRGBColorSpace bytesPerRow:0 bitsPerPixel:0];
    NSGraphicsContext *ctx = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
    [v displayRectIgnoringOpacity:b inContext:ctx];
    CGFloat after = v.frame.size.height;
    if (png && *png) [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToFile:[NSString stringWithUTF8String:png] atomically:YES];
    printf("top render: rows=%d want=%.0f before=%.0f after=%.0f ok=%d\n", n, want, before, after, before == want && after == want);
    return 0;
}

int main(int argc, const char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--token")) {
            /* Not a leak: anything that can run this can already map the
             * segment and write every value in it. This just spells the
             * credential out for a shell, which cannot map shm from a pipe. */
            SEG = sub_attach(&ATTACH_ERR);
            if (!SEG) { fprintf(stderr, "no owner\n"); return 2; }
            if (!SEG->admin[0]) { fprintf(stderr, "owner minted no token\n"); return 3; }
            printf("%s\n", SEG->admin);
            return 0;
        }
        if (!strcmp(argv[i], "--selftest")) return selftest();
        if (!strcmp(argv[i], "--render")) return render(i + 1 < argc ? argv[i + 1] : "");
        if (!strcmp(argv[i], "--next-run") && i + 1 < argc) {   /* one plist's next fire, for a test: SUB_NOW pins the clock */
            sub_job j;
            if (!job_from_plist([NSString stringWithUTF8String:argv[i + 1]], &j)) { fprintf(stderr, "cannot read %s\n", argv[i + 1]); return 2; }
            printf("%s %s %s\n", j.name, j.role, [idle_str(&j, now_s()) UTF8String]);
            return 0;
        }
    }

    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        NSRect frame = NSMakeRect(0, 0, 760, 560);
        NSWindow *w = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                 NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                        backing:NSBackingStoreBuffered
                          defer:NO];
        [w setTitle:@"pipeline top"];
        [w setMinSize:NSMakeSize(520, 240)];
        [w center];

        NSView *root = [[NSView alloc] initWithFrame:frame];
        [root setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];

        HeadView *hv = [[HeadView alloc] initWithFrame:NSMakeRect(0, frame.size.height - HEAD_H, frame.size.width, HEAD_H)];
        [hv setAutoresizingMask:NSViewWidthSizable | NSViewMinYMargin];
        [root addSubview:hv];

        NSScrollView *sv = [[NSScrollView alloc] initWithFrame:NSMakeRect(0, 0, frame.size.width, frame.size.height - HEAD_H)];
        [sv setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [sv setHasVerticalScroller:YES];
        [sv setHasHorizontalScroller:NO];
        [sv setAutohidesScrollers:YES];
        [sv setBorderType:NSNoBorder];
        [sv setDrawsBackground:YES];
        [sv setBackgroundColor:[NSColor colorWithCalibratedWhite:0.09 alpha:1.0]];
        [sv setScrollerKnobStyle:NSScrollerKnobStyleLight];
        TopView *v = [[TopView alloc] initWithFrame:NSMakeRect(0, 0, frame.size.width, frame.size.height - HEAD_H)];
        [v setAutoresizingMask:NSViewWidthSizable];
        [sv setDocumentView:v];
        [root addSubview:sv];
        [w setContentView:root];
        [w makeKeyAndOrderFront:nil];

        [NSTimer scheduledTimerWithTimeInterval:0.2 repeats:YES block:^(NSTimer *t) {
            (void)t; [hv setNeedsDisplay:YES]; [v setNeedsDisplay:YES];
        }];

        [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];
    }
    return 0;
}
