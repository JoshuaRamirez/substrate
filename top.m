/* top -- a native dashboard for the pipeline. Docker Desktop, minus Docker.
 *
 * It does not run `ps`. It does not poll a daemon. It does not speak a
 * protocol. It reads the peer table out of the same shared page every peer
 * writes its own row into, and it asks the OS whether each pid is still
 * alive. That is the whole implementation.
 *
 * Immediate mode: the frame loop is the subscription.
 *
 * It also shows where the owner persists, and can move it: the panel writes a
 * path into the same page, and dbd acts on it. The dashboard does not talk to
 * the database. They share a variable.
 */
#import <Cocoa/Cocoa.h>
#include "counter.h"

static cnt_seg  *SEG   = NULL;     /* NULL when no owner is running */
static cnt_peer *SELF  = NULL;
static int       ATTACH_ERR = 1;

#define ROW_H   34.0
#define TOP_Y   116.0
#define PAD     18.0

static NSRect g_stop[CNT_MAX_PEERS];      /* hit boxes, rebuilt each frame */
static pid_t  g_stop_pid[CNT_MAX_PEERS];
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

static NSString *uptime_str(uint64_t since) {
    if (!since) return @"-";
    long s = (long)(time(NULL) - (time_t)since);
    if (s < 0) s = 0;
    if (s < 60)   return [NSString stringWithFormat:@"%lds", s];
    if (s < 3600) return [NSString stringWithFormat:@"%ldm %lds", s / 60, s % 60];
    return [NSString stringWithFormat:@"%ldh %ldm", s / 3600, (s % 3600) / 60];
}

@interface TopView : NSView
@end

@implementation TopView

- (NSDictionary *)mono:(CGFloat)sz white:(CGFloat)w {
    return @{ NSFontAttributeName : [NSFont monospacedSystemFontOfSize:sz
                                                                weight:NSFontWeightRegular],
              NSForegroundColorAttributeName : [NSColor colorWithCalibratedWhite:w alpha:1.0] };
}

- (void)attachIfNeeded {
    if (SEG) return;
    SEG = cnt_attach(&ATTACH_ERR);
    if (SEG) SELF = cnt_claim_slot(SEG, "top", "dash");
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSRect b = self.bounds;
    CGFloat W = b.size.width, H = b.size.height;

    [[NSColor colorWithCalibratedWhite:0.09 alpha:1.0] setFill];
    NSRectFill(b);

    [self attachIfNeeded];

    /* ---- header ---- */
    NSString *title = @"PIPELINE";
    [title drawAtPoint:NSMakePoint(PAD, H - 34)
        withAttributes:@{ NSFontAttributeName : [NSFont systemFontOfSize:13
                                                                 weight:NSFontWeightBold],
                          NSForegroundColorAttributeName :
                              [NSColor colorWithCalibratedWhite:0.62 alpha:1.0] }];

    NSString *sub = SEG ? [NSString stringWithFormat:@"%s  format v%u  %d slots",
                                    CNT_SHM_NAME, CNT_VERSION, CNT_MAX_PEERS]
                        : @"no owner attached";
    [sub drawAtPoint:NSMakePoint(PAD, H - 54) withAttributes:[self mono:10 white:0.40]];

    /* ---- where the data actually goes ---- */
    g_move_on = 0;
    if (SEG) {
        char db[CNT_PATHLEN];
        NSString *dbs = cnt_path_read(SEG, db, sizeof db) && db[0]
                      ? [NSString stringWithUTF8String:db] : @"(unknown)";
        NSString *line = [NSString stringWithFormat:@"db  %@", elide(dbs, 58)];
        [line drawAtPoint:NSMakePoint(PAD, H - 74) withAttributes:[self mono:10 white:0.52]];

        NSSize ls = [line sizeWithAttributes:[self mono:10 white:0.52]];
        g_move = NSMakeRect(PAD + ls.width + 12, H - 78, 58, 19);
        g_move_on = 1;
        [[NSColor colorWithCalibratedWhite:0.19 alpha:1.0] setFill];
        [[NSBezierPath bezierPathWithRoundedRect:g_move xRadius:5 yRadius:5] fill];
        NSDictionary *ma = @{
            NSFontAttributeName : [NSFont systemFontOfSize:10 weight:NSFontWeightMedium],
            NSForegroundColorAttributeName :
                [NSColor colorWithCalibratedWhite:0.72 alpha:1.0] };
        NSSize ms2 = [@"move..." sizeWithAttributes:ma];
        [@"move..." drawAtPoint:NSMakePoint(NSMidX(g_move) - ms2.width / 2,
                                            NSMidY(g_move) - ms2.height / 2)
                 withAttributes:ma];

        if (g_note)
            [g_note drawAtPoint:NSMakePoint(PAD, H - 90)
                 withAttributes:@{ NSFontAttributeName :
                       [NSFont monospacedSystemFontOfSize:9 weight:NSFontWeightRegular],
                   NSForegroundColorAttributeName :
                       [NSColor colorWithCalibratedRed:0.83 green:0.42 blue:0.42 alpha:1] }];
    }

    if (SEG) {
        NSString *cnt = [NSString stringWithFormat:@"%llu",
                         (unsigned long long)atomic_load(&SEG->count)];
        NSDictionary *big = @{
            NSFontAttributeName : [NSFont monospacedDigitSystemFontOfSize:34
                                                                   weight:NSFontWeightBold],
            NSForegroundColorAttributeName : [NSColor whiteColor] };
        NSSize cs = [cnt sizeWithAttributes:big];
        [cnt drawAtPoint:NSMakePoint(W - PAD - cs.width, H - 52) withAttributes:big];
        NSString *cl = @"count";
        NSSize ls = [cl sizeWithAttributes:[self mono:10 white:0.40]];
        [cl drawAtPoint:NSMakePoint(W - PAD - ls.width, H - 66)
         withAttributes:[self mono:10 white:0.40]];
    }

    /* ---- empty state ---- */
    if (!SEG) {
        NSString *m = @"start the owner:   ./bin/dbd";
        NSDictionary *a = [self mono:13 white:0.45];
        NSSize ms = [m sizeWithAttributes:a];
        [m drawAtPoint:NSMakePoint((W - ms.width) / 2, H / 2 - 10) withAttributes:a];
        g_stop_n = 0;
        return;
    }

    /* ---- column headers ---- */
    CGFloat y = H - TOP_Y;
    NSDictionary *hd = [self mono:10 white:0.38];
    [@"NAME"  drawAtPoint:NSMakePoint(PAD + 22,  y) withAttributes:hd];
    [@"ROLE"  drawAtPoint:NSMakePoint(PAD + 128, y) withAttributes:hd];
    [@"PID"   drawAtPoint:NSMakePoint(PAD + 226, y) withAttributes:hd];
    [@"UP"    drawAtPoint:NSMakePoint(PAD + 310, y) withAttributes:hd];
    [@"OPS"   drawAtPoint:NSMakePoint(PAD + 404, y) withAttributes:hd];

    [[NSColor colorWithCalibratedWhite:0.20 alpha:1.0] setFill];
    NSRectFill(NSMakeRect(PAD, y - 8, W - 2 * PAD, 1));

    /* ---- rows ---- */
    g_stop_n = 0;
    int shown = 0;
    for (int i = 0; i < CNT_MAX_PEERS; i++) {
        cnt_peer *p = &SEG->peers[i];
        if (atomic_load(&p->state) != 1) continue;

        int alive = cnt_peer_alive(p);
        CGFloat ry = y - 24 - shown * ROW_H;
        if (ry < 30) break;

        if (shown % 2 == 0) {
            [[NSColor colorWithCalibratedWhite:0.12 alpha:1.0] setFill];
            NSRectFill(NSMakeRect(PAD, ry - 9, W - 2 * PAD, ROW_H - 4));
        }

        NSColor *dot = alive ? [NSColor colorWithCalibratedRed:0.20 green:0.80 blue:0.40 alpha:1]
                             : [NSColor colorWithCalibratedWhite:0.35 alpha:1];
        [dot setFill];
        [[NSBezierPath bezierPathWithOvalInRect:NSMakeRect(PAD + 2, ry + 3, 8, 8)] fill];

        pid_t pid  = (pid_t)atomic_load(&p->pid);
        BOOL   me  = (SELF && p == SELF);
        CGFloat wh = alive ? 0.92 : 0.38;

        NSString *nm = me ? [NSString stringWithFormat:@"%s *", p->name]
                          : [NSString stringWithUTF8String:p->name];
        [nm drawAtPoint:NSMakePoint(PAD + 22, ry) withAttributes:[self mono:12 white:wh]];
        [[NSString stringWithUTF8String:p->role]
            drawAtPoint:NSMakePoint(PAD + 128, ry) withAttributes:[self mono:12 white:0.55]];
        [[NSString stringWithFormat:@"%d", pid]
            drawAtPoint:NSMakePoint(PAD + 226, ry) withAttributes:[self mono:12 white:0.55]];
        [uptime_str(atomic_load(&p->since))
            drawAtPoint:NSMakePoint(PAD + 310, ry) withAttributes:[self mono:12 white:0.55]];
        [[NSString stringWithFormat:@"%llu", (unsigned long long)atomic_load(&p->ops)]
            drawAtPoint:NSMakePoint(PAD + 404, ry) withAttributes:[self mono:12 white:wh]];

        if (alive && !me) {
            NSRect sb = NSMakeRect(W - PAD - 62, ry - 4, 62, 22);
            [[NSColor colorWithCalibratedRed:0.42 green:0.15 blue:0.17 alpha:1.0] setFill];
            [[NSBezierPath bezierPathWithRoundedRect:sb xRadius:5 yRadius:5] fill];
            NSDictionary *sa = @{
                NSFontAttributeName : [NSFont systemFontOfSize:11 weight:NSFontWeightMedium],
                NSForegroundColorAttributeName :
                    [NSColor colorWithCalibratedRed:1.0 green:0.72 blue:0.72 alpha:1.0] };
            NSSize ss = [@"stop" sizeWithAttributes:sa];
            [@"stop" drawAtPoint:NSMakePoint(NSMidX(sb) - ss.width / 2,
                                             NSMidY(sb) - ss.height / 2)
                  withAttributes:sa];
            g_stop[g_stop_n]     = sb;
            g_stop_pid[g_stop_n] = pid;
            g_stop_n++;
        }
        shown++;
    }

    if (shown == 0) {
        NSString *m = @"no peers";
        NSDictionary *a = [self mono:12 white:0.35];
        [m drawAtPoint:NSMakePoint(PAD + 22, y - 34) withAttributes:a];
    }

    NSString *ft = @"rows are shm slots.  liveness is kill(pid,0).  * = this window";
    [ft drawAtPoint:NSMakePoint(PAD, 12) withAttributes:[self mono:9 white:0.28]];
}

/* The dashboard does not ask dbd to move. It writes the path and goes back to
 * drawing; dbd notices within 100ms. If dbd cannot open the file it writes the
 * old path back, and the row below reverts on its own. */
- (void)moveDatabase {
    if (!SEG) return;
    char cur[CNT_PATHLEN];
    NSSavePanel *sp = [NSSavePanel savePanel];
    [sp setTitle:@"Move the counter database"];
    [sp setPrompt:@"Move"];
    [sp setNameFieldStringValue:@"counter.db"];
    if (cnt_path_read(SEG, cur, sizeof cur) && cur[0]) {
        NSString *c = [NSString stringWithUTF8String:cur];
        [sp setDirectoryURL:[NSURL fileURLWithPath:[c stringByDeletingLastPathComponent]]];
        [sp setNameFieldStringValue:[c lastPathComponent]];
    }
    if ([sp runModal] == NSModalResponseOK) {
        int rc = cnt_path_write(SEG, [[[sp URL] path] UTF8String]);
        g_note = rc == 0 ? nil
               : [NSString stringWithFormat:@"refused: %s", cnt_path_error(rc)];
    }
    [self setNeedsDisplay:YES];
}

- (void)mouseDown:(NSEvent *)e {
    NSPoint pt = [self convertPoint:e.locationInWindow fromView:nil];
    if (g_move_on && NSPointInRect(pt, g_move)) { [self moveDatabase]; return; }
    for (int i = 0; i < g_stop_n; i++) {
        if (NSPointInRect(pt, g_stop[i])) {
            kill(g_stop_pid[i], SIGTERM);
            [self setNeedsDisplay:YES];
            return;
        }
    }
}

- (void)tick:(NSTimer *)t { (void)t; [self setNeedsDisplay:YES]; }

@end

int main(int argc, const char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest")) {          /* same data path, no window */
            SEG = cnt_attach(&ATTACH_ERR);
            if (!SEG) { printf("top selftest: no owner (err=%d)\n", ATTACH_ERR); return 2; }
            int live = 0;
            for (int k = 0; k < CNT_MAX_PEERS; k++)
                if (cnt_peer_alive(&SEG->peers[k])) live++;
            char db[CNT_PATHLEN];
            if (!cnt_path_read(SEG, db, sizeof db)) snprintf(db, sizeof db, "(unknown)");
            printf("top selftest: peers=%d count=%llu\n", live,
                   (unsigned long long)atomic_load(&SEG->count));
            printf("top selftest: db=%s\n", db);
            return 0;
        }
    }

    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        NSRect frame = NSMakeRect(0, 0, 640, 420);
        NSWindow *w = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:(NSWindowStyleMaskTitled |
                                 NSWindowStyleMaskClosable |
                                 NSWindowStyleMaskMiniaturizable |
                                 NSWindowStyleMaskResizable)
                        backing:NSBackingStoreBuffered
                          defer:NO];
        [w setTitle:@"pipeline top"];
        [w center];

        TopView *v = [[TopView alloc] initWithFrame:frame];
        [v setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        [w setContentView:v];
        [w makeKeyAndOrderFront:nil];

        [NSTimer scheduledTimerWithTimeInterval:0.2
                                         target:v
                                       selector:@selector(tick:)
                                       userInfo:nil
                                        repeats:YES];

        [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];
    }
    return 0;
}
