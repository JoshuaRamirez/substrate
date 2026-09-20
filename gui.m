/* gui -- a native driver. A real Cocoa window. No browser engine, no webview,
 * no HTML, no toolkit to install.
 *
 * It is immediate-mode: drawRect: reads the shared cell every single frame.
 * There is no cache, no subscription, no invalidation, no push channel.
 * The frame loop IS the subscription. That is what the shared-memory model
 * looks like when you draw it.
 */
#import <Cocoa/Cocoa.h>
#include "counter.h"

static counter C;
static NSRect BTN;

@interface CounterView : NSView
@end

@implementation CounterView

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    NSRect b = self.bounds;

    [[NSColor colorWithCalibratedWhite:0.10 alpha:1.0] setFill];
    NSRectFill(b);

    /* ---- every frame, straight off the shared page ---- */
    uint64_t v = counter_read(&C);

    NSDictionary *big = @{
        NSFontAttributeName : [NSFont monospacedDigitSystemFontOfSize:76
                                                               weight:NSFontWeightBold],
        NSForegroundColorAttributeName : [NSColor whiteColor]
    };
    NSString *num = [NSString stringWithFormat:@"%llu", (unsigned long long)v];
    NSSize ns = [num sizeWithAttributes:big];
    [num drawAtPoint:NSMakePoint((b.size.width - ns.width) / 2, 138)
      withAttributes:big];

    NSDictionary *small = @{
        NSFontAttributeName : [NSFont monospacedSystemFontOfSize:11
                                                          weight:NSFontWeightRegular],
        NSForegroundColorAttributeName : [NSColor colorWithCalibratedWhite:0.55 alpha:1.0]
    };
    NSString *mode = [NSString stringWithFormat:@"mode = %s", counter_mode(&C)];
    NSSize ms = [mode sizeWithAttributes:small];
    [mode drawAtPoint:NSMakePoint((b.size.width - ms.width) / 2, 108)
       withAttributes:small];

    BTN = NSMakeRect((b.size.width - 160) / 2, 40, 160, 44);
    [[NSColor colorWithCalibratedRed:0.20 green:0.52 blue:0.92 alpha:1.0] setFill];
    [[NSBezierPath bezierPathWithRoundedRect:BTN xRadius:9 yRadius:9] fill];

    NSDictionary *lbl = @{
        NSFontAttributeName : [NSFont systemFontOfSize:15 weight:NSFontWeightSemibold],
        NSForegroundColorAttributeName : [NSColor whiteColor]
    };
    NSString *bt = @"bump";
    NSSize bs = [bt sizeWithAttributes:lbl];
    [bt drawAtPoint:NSMakePoint(NSMidX(BTN) - bs.width / 2,
                                NSMidY(BTN) - bs.height / 2)
     withAttributes:lbl];
}

- (void)mouseDown:(NSEvent *)e {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    if (NSPointInRect(p, BTN)) {
        counter_bump(&C);              /* identical call site */
        [self setNeedsDisplay:YES];
    }
}

- (void)tick:(NSTimer *)t { (void)t; [self setNeedsDisplay:YES]; }

@end

int main(int argc, const char **argv) {
    int join = 0, selftest = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--join"))     join = 1;
        if (!strcmp(argv[i], "--selftest")) selftest = 1;
    }

    if (selftest) {                    /* same data path, no window */
        if (counter_open(&C, join, "gui", "ui") < 0) return 2;
        uint64_t v = counter_bump(&C);
        printf("gui selftest: mode=%s count=%llu\n", counter_mode(&C),
               (unsigned long long)v);
        counter_close(&C);
        return 0;
    }

    if (counter_open(&C, join, "gui", "ui") < 0) return 2;   /* <- the only mode-aware line */

    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        NSRect frame = NSMakeRect(0, 0, 340, 260);
        NSWindow *w = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:(NSWindowStyleMaskTitled |
                                 NSWindowStyleMaskClosable |
                                 NSWindowStyleMaskMiniaturizable)
                        backing:NSBackingStoreBuffered
                          defer:NO];
        [w setTitle:[NSString stringWithFormat:@"counter (%s)", counter_mode(&C)]];
        [w center];

        CounterView *v = [[CounterView alloc] initWithFrame:frame];
        [w setContentView:v];
        [w makeKeyAndOrderFront:nil];

        [NSTimer scheduledTimerWithTimeInterval:1.0 / 30.0
                                         target:v
                                       selector:@selector(tick:)
                                       userInfo:nil
                                        repeats:YES];

        [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];
    }
    return 0;
}
