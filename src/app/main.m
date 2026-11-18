// LocalSiri: a Siri-style front end for localsirid (fully on-device).
// Opens listening immediately; ends on ~0.7 s of silence, a tap, or 10 s.
#import <UIKit/UIKit.h>
#import <AudioToolbox/AudioToolbox.h>
#import <AVFoundation/AVFoundation.h>
#import <QuartzCore/QuartzCore.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <objc/message.h>

#define RATE 16000
#define MAXSEC 10

/* ---------------- recorder ---------------- */
@interface Recorder : NSObject {
@public
    AudioQueueRef q; int16_t *buf; int n, cap, total; BOOL heard; int silentMs, voicedMs; float level, noise;
}
@property (nonatomic, copy) void (^onAutoStop)(void);
@property (nonatomic, copy) void (^onLevel)(float);
- (void)start; - (NSData *)stop;
@end

static void inputCB(void *ud, AudioQueueRef q, AudioQueueBufferRef b, const AudioTimeStamp *t, UInt32 np, const AudioStreamPacketDescription *d) {
    Recorder *r = (__bridge Recorder *)ud;
    int k = b->mAudioDataByteSize / 2; const int16_t *s = b->mAudioData;
    double e = 0; for (int i = 0; i < k; i++) e += (double)s[i] * s[i];
    float rms = sqrt(e / (k ? k : 1)) / 32768.f;
    r->total += k;
    if (r->total <= RATE / 4) { AudioQueueEnqueueBuffer(q, b, 0, NULL); return; }   // drop 250 ms (start-beep echo)
    if (r->n + k <= r->cap) { memcpy(r->buf + r->n, s, k * 2); r->n += k; }
    int ms = k * 1000 / RATE;
    if (r->n <= RATE / 4) r->noise = r->noise * 0.7f + rms * 0.3f;          // next 250 ms: noise floor
    float thr = fmaxf(0.01f, r->noise * 3.f);
    if (rms > thr) { r->voicedMs += ms; r->silentMs = 0; if (r->voicedMs > 150) r->heard = YES; }
    else r->silentMs += ms;
    r->level = rms;
    if (r.onLevel) { float lv = rms; dispatch_async(dispatch_get_main_queue(), ^{ r.onLevel(lv); }); }
    BOOL done = (r->heard && r->silentMs > 700) || r->n + k > r->cap || (!r->heard && r->n > RATE * 5);
    if (done && r.onAutoStop) { void (^cb)(void) = r.onAutoStop; r.onAutoStop = nil; dispatch_async(dispatch_get_main_queue(), cb); }
    else AudioQueueEnqueueBuffer(q, b, 0, NULL);
}

@implementation Recorder
- (void)start {
    cap = RATE * MAXSEC; free(buf); buf = malloc(cap * 2); n = 0; total = 0; heard = NO; silentMs = voicedMs = 0; noise = 0.01f;
    AudioStreamBasicDescription f = {0};
    f.mSampleRate = RATE; f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    f.mBytesPerPacket = 2; f.mFramesPerPacket = 1; f.mBytesPerFrame = 2; f.mChannelsPerFrame = 1; f.mBitsPerChannel = 16;
    AudioQueueNewInput(&f, inputCB, (__bridge void *)self, NULL, kCFRunLoopCommonModes, 0, &q);
    for (int i = 0; i < 3; i++) {
        AudioQueueBufferRef b; AudioQueueAllocateBuffer(q, RATE / 20 * 2, &b); AudioQueueEnqueueBuffer(q, b, 0, NULL);
    }
    AudioQueueStart(q, NULL);
}
- (NSData *)stop {
    if (q) { AudioQueueStop(q, true); AudioQueueDispose(q, true); q = NULL; }
    return [NSData dataWithBytes:buf length:n * 2];
}
@end

/* ---------------- views ---------------- */
// Dark woven "linen" backdrop drawn procedurally.
@interface LinenView : UIView @end
@implementation LinenView
- (void)drawRect:(CGRect)r {
    CGContextRef c = UIGraphicsGetCurrentContext();
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGFloat comps[] = {0.20, 0.21, 0.24, 1, 0.09, 0.09, 0.11, 1};
    CGGradientRef g = CGGradientCreateWithColorComponents(cs, comps, NULL, 2);
    CGContextDrawRadialGradient(c, g, CGPointMake(r.size.width / 2, r.size.height * 0.35), 0,
                                CGPointMake(r.size.width / 2, r.size.height * 0.35), r.size.height * 0.8, kCGGradientDrawsAfterEndLocation);
    CGGradientRelease(g); CGColorSpaceRelease(cs);
    srand(7);
    for (int y = 0; y < r.size.height; y += 2) {
        CGContextSetRGBStrokeColor(c, 1, 1, 1, 0.012 + (rand() % 100) / 4000.0);
        CGContextMoveToPoint(c, 0, y); CGContextAddLineToPoint(c, r.size.width, y + (rand() % 3 - 1)); CGContextStrokePath(c);
    }
    for (int x = 0; x < r.size.width; x += 3) {
        CGContextSetRGBStrokeColor(c, 0, 0, 0, 0.03 + (rand() % 100) / 3000.0);
        CGContextMoveToPoint(c, x, 0); CGContextAddLineToPoint(c, x + (rand() % 3 - 1), r.size.height); CGContextStrokePath(c);
    }
}
@end

// Round brushed-metal microphone button with a glow ring.
@interface MicButton : UIControl { CALayer *glow; }
@property (nonatomic) float level; @property (nonatomic) BOOL listening, thinking;
@end
@implementation MicButton
- (id)initWithFrame:(CGRect)f {
    if ((self = [super initWithFrame:f])) {
        self.backgroundColor = [UIColor clearColor];
        glow = [CALayer layer];
        glow.frame = CGRectInset(self.bounds, -6, -6); glow.cornerRadius = glow.frame.size.width / 2;
        glow.borderWidth = 4; glow.borderColor = [UIColor colorWithRed:0.62 green:0.45 blue:1 alpha:1].CGColor;
        glow.shadowColor = glow.borderColor; glow.shadowRadius = 10; glow.shadowOpacity = 1; glow.shadowOffset = CGSizeZero;
        glow.opacity = 0;
        [self.layer addSublayer:glow];
    }
    return self;
}
- (void)setLevel:(float)l { _level = l; if (_listening) glow.opacity = fminf(1, 0.35f + l * 12); }
- (void)setListening:(BOOL)b { _listening = b; glow.opacity = b ? 0.4 : 0; [glow removeAllAnimations]; }
- (void)setThinking:(BOOL)b {
    _thinking = b;
    if (b) {
        glow.opacity = 1;
        CABasicAnimation *a = [CABasicAnimation animationWithKeyPath:@"opacity"];
        a.fromValue = @0.2; a.toValue = @1; a.duration = 0.6; a.autoreverses = YES; a.repeatCount = HUGE_VALF;
        [glow addAnimation:a forKey:@"pulse"];
    } else { [glow removeAllAnimations]; glow.opacity = 0; }
}
- (void)drawRect:(CGRect)r {
    CGContextRef c = UIGraphicsGetCurrentContext();
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGFloat m[] = {0.93, 0.93, 0.95, 1, 0.62, 0.63, 0.66, 1};
    CGGradientRef g = CGGradientCreateWithColorComponents(cs, m, NULL, 2);
    CGContextSaveGState(c);
    CGContextAddEllipseInRect(c, CGRectInset(r, 2, 2)); CGContextClip(c);
    CGContextDrawLinearGradient(c, g, CGPointMake(0, 0), CGPointMake(0, r.size.height), 0);
    CGContextRestoreGState(c);
    CGGradientRelease(g); CGColorSpaceRelease(cs);
    CGContextSetRGBStrokeColor(c, 0.3, 0.3, 0.33, 1); CGContextSetLineWidth(c, 1.5);
    CGContextStrokeEllipseInRect(c, CGRectInset(r, 2, 2));
    // microphone glyph
    CGFloat cx = r.size.width / 2, cy = r.size.height / 2;
    CGContextSetRGBFillColor(c, 0.22, 0.22, 0.25, 1);
    UIBezierPath *cap = [UIBezierPath bezierPathWithRoundedRect:CGRectMake(cx - 8, cy - 20, 16, 26) cornerRadius:8];
    [cap fill];
    CGContextSetRGBStrokeColor(c, 0.22, 0.22, 0.25, 1); CGContextSetLineWidth(c, 3);
    CGContextAddArc(c, cx, cy - 4, 13, 0, M_PI, 0); CGContextStrokePath(c);
    CGContextMoveToPoint(c, cx, cy + 9); CGContextAddLineToPoint(c, cx, cy + 17); CGContextStrokePath(c);
    CGContextMoveToPoint(c, cx - 8, cy + 17); CGContextAddLineToPoint(c, cx + 8, cy + 17); CGContextStrokePath(c);
}
@end

/* ---------------- controller ---------------- */
@interface VC : UIViewController {
    UIScrollView *scroll; MicButton *mic; UILabel *stats; Recorder *rec; CGFloat y; BOOL busy;
    UILabel *answerLabel; NSMutableString *answer; id tts;
}
- (void)listen;
@end

@implementation VC
- (void)loadView {
    LinenView *v = [[LinenView alloc] initWithFrame:[UIScreen mainScreen].bounds];
    v.contentMode = UIViewContentModeRedraw;
    self.view = v;
    CGRect b = v.bounds;
    scroll = [[UIScrollView alloc] initWithFrame:CGRectMake(0, 0, b.size.width, b.size.height - 110)];
    [v addSubview:scroll];
    UIView *tray = [[UIView alloc] initWithFrame:CGRectMake(0, b.size.height - 110, b.size.width, 110)];
    CAGradientLayer *gl = [CAGradientLayer layer]; gl.frame = tray.bounds;
    gl.colors = @[(id)[UIColor colorWithWhite:0.18 alpha:1].CGColor, (id)[UIColor colorWithWhite:0.05 alpha:1].CGColor];
    [tray.layer addSublayer:gl];
    CALayer *line = [CALayer layer]; line.frame = CGRectMake(0, 0, b.size.width, 1); line.backgroundColor = [UIColor colorWithWhite:0.35 alpha:1].CGColor;
    [tray.layer addSublayer:line];
    [v addSubview:tray];
    mic = [[MicButton alloc] initWithFrame:CGRectMake(b.size.width / 2 - 36, 14, 72, 72)];
    [mic addTarget:self action:@selector(micTapped) forControlEvents:UIControlEventTouchUpInside];
    [tray addSubview:mic];
    stats = [[UILabel alloc] initWithFrame:CGRectMake(6, 88, b.size.width - 12, 18)];
    stats.backgroundColor = [UIColor clearColor]; stats.textColor = [UIColor colorWithWhite:0.55 alpha:1];
    stats.font = [UIFont systemFontOfSize:10]; stats.textAlignment = NSTextAlignmentCenter;
    stats.text = @"on-device: Moonshine Tiny + SmolLM2-135M";
    [tray addSubview:stats];
    y = 24;
    rec = [Recorder new];
    __weak VC *ws = self;
    rec.onLevel = ^(float l) { VC *s = ws; if (s) s->mic.level = l; };
    dlopen("/System/Library/PrivateFrameworks/VoiceServices.framework/VoiceServices", RTLD_LAZY);
    Class ts = NSClassFromString(@"VSSpeechSynthesizer");
    if (ts) tts = [ts new];
}

- (UILabel *)addText:(NSString *)t user:(BOOL)user {
    CGFloat w = self.view.bounds.size.width;
    UILabel *l = [[UILabel alloc] init];
    l.numberOfLines = 0; l.backgroundColor = [UIColor clearColor]; l.text = t;
    if (user) {
        l.font = [UIFont fontWithName:@"Georgia-Italic" size:17] ?: [UIFont italicSystemFontOfSize:17];
        l.textColor = [UIColor colorWithWhite:0.2 alpha:1];
        CGSize s = [t sizeWithFont:l.font constrainedToSize:CGSizeMake(w - 90, 999)];
        UIView *bubble = [[UIView alloc] initWithFrame:CGRectMake(w - s.width - 44, y, s.width + 30, s.height + 20)];
        bubble.backgroundColor = [UIColor colorWithRed:0.97 green:0.96 blue:0.92 alpha:1];
        bubble.layer.cornerRadius = 12; bubble.layer.shadowOpacity = 0.6; bubble.layer.shadowOffset = CGSizeMake(0, 2);
        bubble.layer.borderColor = [UIColor colorWithWhite:0.75 alpha:1].CGColor; bubble.layer.borderWidth = 1;
        l.frame = CGRectMake(15, 10, s.width, s.height);
        [bubble addSubview:l]; [scroll addSubview:bubble];
        y += bubble.frame.size.height + 16;
    } else {
        l.font = [UIFont boldSystemFontOfSize:18]; l.textColor = [UIColor whiteColor];
        l.shadowColor = [UIColor blackColor]; l.shadowOffset = CGSizeMake(0, -1);
        CGSize s = [t sizeWithFont:l.font constrainedToSize:CGSizeMake(w - 36, 9999)];
        l.frame = CGRectMake(18, y, w - 36, MAX(24, s.height));
        [scroll addSubview:l];
        y += l.frame.size.height + 6;
    }
    [self relayout];
    return l;
}
- (void)relayout {
    if (answerLabel) {
        CGSize s = [answerLabel.text sizeWithFont:answerLabel.font constrainedToSize:CGSizeMake(answerLabel.frame.size.width, 9999)];
        CGRect f = answerLabel.frame; CGFloat old = f.size.height; f.size.height = s.height; answerLabel.frame = f;
        y += s.height - old;
    }
    scroll.contentSize = CGSizeMake(scroll.bounds.size.width, y + 20);
    if (y + 20 > scroll.bounds.size.height) [scroll setContentOffset:CGPointMake(0, y + 20 - scroll.bounds.size.height) animated:YES];
}

// Writes the current screen to /tmp/localsiri.png (for checking the UI over SSH).
- (void)snapshot {
    UIGraphicsBeginImageContextWithOptions(self.view.bounds.size, YES, 0);
    [self.view.layer renderInContext:UIGraphicsGetCurrentContext()];
    UIImage *img = UIGraphicsGetImageFromCurrentImageContext();
    UIGraphicsEndImageContext();
    [UIImagePNGRepresentation(img) writeToFile:@"/tmp/localsiri.png" atomically:YES];
}
- (void)viewDidAppear:(BOOL)a { [super viewDidAppear:a]; [self greet]; }
- (void)greet { [self addText:@"What can I help you with?" user:NO]; [self listen]; }

- (void)micTapped {
    if (busy) return;
    if (mic.listening) [self finishListening]; else [self listen];
}
- (void)listen {
    if (busy || mic.listening) return;
    [[AVAudioSession sharedInstance] setCategory:AVAudioSessionCategoryPlayAndRecord error:nil];
    [[AVAudioSession sharedInstance] setActive:YES error:nil];
    AudioServicesPlaySystemSound(1113);
    __weak VC *ws = self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        VC *s = ws; if (!s) return;
        s->mic.listening = YES;
        s->rec.onAutoStop = ^{ [ws finishListening]; };
        [s->rec start];
    });
}
- (void)finishListening {
    if (!mic.listening) return;
    mic.listening = NO; rec.onAutoStop = nil;
    NSData *pcm = [rec stop];
    // Debug hook: if this WAV exists (16 kHz mono s16), use it instead of the mic.
    NSData *wav = [NSData dataWithContentsOfFile:@"/tmp/localsiri-test.wav"];
    if (wav.length > 44) {
        pcm = [wav subdataWithRange:NSMakeRange(44, wav.length - 44)];
        [[NSFileManager defaultManager] removeItemAtPath:@"/tmp/localsiri-test.wav" error:nil];
    }
    AudioServicesPlaySystemSound(1114);
    busy = YES; mic.thinking = YES;
    [NSThread detachNewThreadSelector:@selector(ask:) toTarget:self withObject:pcm];
}

- (void)ask:(NSData *)pcm {
    @autoreleasepool {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET; a.sin_port = htons(7788); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(s, (struct sockaddr *)&a, sizeof a)) {
            close(s);
            [self performSelectorOnMainThread:@selector(onLine:) withObject:@"ERR\tThe assistant service isn't running." waitUntilDone:NO];
            [self performSelectorOnMainThread:@selector(onLine:) withObject:@"DONE\t0\t0\t0\t0" waitUntilDone:NO];
            return;
        }
        uint32_t n = (uint32_t)pcm.length / 2;
        write(s, "AUD1", 4); write(s, &n, 4); write(s, pcm.bytes, pcm.length);
        NSMutableData *acc = [NSMutableData data]; char buf[1024]; ssize_t r;
        while ((r = read(s, buf, sizeof buf)) > 0) {
            [acc appendBytes:buf length:r];
            for (;;) {
                const char *p = acc.bytes; const char *nl = memchr(p, '\n', acc.length);
                if (!nl) break;
                NSString *line = [[NSString alloc] initWithBytes:p length:nl - p encoding:NSUTF8StringEncoding] ?: @"";
                [self performSelectorOnMainThread:@selector(onLine:) withObject:line waitUntilDone:NO];
                [acc replaceBytesInRange:NSMakeRange(0, nl - p + 1) withBytes:NULL length:0];
            }
        }
        close(s);
    }
}

- (void)speak:(NSString *)t {
    if (!tts || !t.length) return;
    SEL s1 = NSSelectorFromString(@"startSpeakingString:withLanguageCode:");
    SEL s2 = NSSelectorFromString(@"startSpeakingString:toURL:withLanguageCode:");
    SEL s0 = NSSelectorFromString(@"startSpeakingString:");
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
    if ([tts respondsToSelector:s2]) ((id (*)(id, SEL, id, id, id))objc_msgSend)(tts, s2, t, nil, @"en-US");
    else if ([tts respondsToSelector:s1]) [tts performSelector:s1 withObject:t withObject:@"en-US"];
    else if ([tts respondsToSelector:s0]) [tts performSelector:s0 withObject:t];
#pragma clang diagnostic pop
}

- (void)onLine:(NSString *)line {
    NSArray *f = [line componentsSeparatedByString:@"\t"];
    NSString *tag = f.count ? f[0] : @"";
    NSString *rest = f.count > 1 ? [line substringFromIndex:tag.length + 1] : @"";
    if ([tag isEqualToString:@"HEARD"]) {
        [self addText:rest.length ? [NSString stringWithFormat:@"“%@”", rest] : @"(I didn't catch that)" user:YES];
        answer = [NSMutableString string];
        answerLabel = [self addText:@"" user:NO];
    } else if ([tag isEqualToString:@"TOK"]) {
        [answer appendString:[rest stringByReplacingOccurrencesOfString:@"\\n" withString:@"\n"]];
        answerLabel.text = [answer stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
        [self relayout];
    } else if ([tag isEqualToString:@"ERR"]) {
        answerLabel = nil; [self addText:rest user:NO];
    } else if ([tag isEqualToString:@"DONE"]) {
        busy = NO; mic.thinking = NO;
        if (f.count >= 5)
            stats.text = [NSString stringWithFormat:@"speech %.1fs · prompt %.1fs · answer %@ tok in %.1fs (on device)",
                          [f[1] doubleValue] / 1000, [f[2] doubleValue] / 1000, f[4], [f[3] doubleValue] / 1000];
        NSString *t = answerLabel.text; answerLabel = nil;
        y += 14; [self relayout];
        [self speak:t];
        [self performSelector:@selector(snapshot) withObject:nil afterDelay:0.5];
    }
}
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property (strong, nonatomic) UIWindow *window;
@end
@implementation AppDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)o {
    self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
    self.window.rootViewController = [VC new];
    [self.window makeKeyAndVisible];
    return YES;
}
- (void)applicationWillEnterForeground:(UIApplication *)app {
    VC *vc = (VC *)self.window.rootViewController;
    [vc listen];
}
@end

int main(int argc, char *argv[]) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class])); }
}
