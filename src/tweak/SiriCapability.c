// SiriCapability: Cydia Substrate tweak for running iOS 6 Siri on an iPod touch 4G.
//  1. MobileGestalt reports the "assistant" (Siri) capability (SpringBoard, assistantd, Preferences).
//  2. assistantd: Siri asks for Speex at sample rate 0 ("codec default"); the iPod's AudioCodecs
//     has no Speex encoder, so mediaserverd fails with -50. Record 16 kHz mono 16-bit PCM instead
//     (assistantd still labels the stream Speex_WB; acesrv tells them apart by packet size).
//  3. assistantd: disable ADAceConnection's response timeouts, which are sized for Apple's servers;
//     an on-device answer takes longer, and assistantd would drop the loopback connection.
//  4. Test hook: while /tmp/acesrv-test.wav exists, fake Siri's start/end-of-speech detection.
//  5. Debug: if /tmp/siri-debug exists, copy assistantd's own log (all levels) to /tmp/assistantd.log.
#include <CoreFoundation/CoreFoundation.h>
#include <objc/runtime.h>
#include <objc/message.h>
#include <mach-o/dyld.h>
#include <dlfcn.h>
#include <string.h>
#include <stdio.h>
#include <notify.h>
#include <dispatch/dispatch.h>
static FILE *dbg;   // debug log, see my_CPLog

// Cydia Substrate's MSHookFunction, looked up at run time (Substrate loads this tweak, so it is
// always there; nothing from the device is needed at build time).
static void MSHookFunction(void *symbol, void *replace, void **result) {
    static void (*hook)(void *, void *, void **);
    if (!hook) hook = (void (*)(void *, void *, void **))dlsym(RTLD_DEFAULT, "MSHookFunction");
    if (hook) hook(symbol, replace, result);
}

static const char *const kForceTrue[] = { "assistant", "assistant-eyes-free", NULL };

static Boolean (*orig_MGGetBoolAnswer)(CFStringRef);
static Boolean my_MGGetBoolAnswer(CFStringRef key) {
    char k[64];
    if (key && CFStringGetCString(key, k, sizeof k, kCFStringEncodingUTF8))
        for (int i = 0; kForceTrue[i]; i++) if (!strcmp(k, kForceTrue[i])) return 1;
    return orig_MGGetBoolAnswer(key);
}

static CFTypeRef (*orig_MGCopyAnswer)(CFStringRef);
static CFTypeRef my_MGCopyAnswer(CFStringRef key) {
    char k[64];
    if (key && CFStringGetCString(key, k, sizeof k, kCFStringEncodingUTF8))
        for (int i = 0; kForceTrue[i]; i++) if (!strcmp(k, kForceTrue[i])) return kCFBooleanTrue;
    return orig_MGCopyAnswer(key);
}

// Hook past a leading 4-byte Thumb-2 "b.w <next instruction>". On iOS 6, MGCopyAnswer starts
// with one (00 f0 00 b8). Substrate would copy that PC-relative branch into its trampoline
// unrelocated, and calling the original then crashes (SIGILL).
static void *hook_target(void *fn) {
    const unsigned char *p = (const unsigned char *)((unsigned long)fn & ~1UL);
    if (p[0] == 0x00 && p[1] == 0xf0 && p[2] == 0x00 && p[3] == 0xb8)
        return (void *)((unsigned long)fn + 4);
    return fn;
}

static BOOL (*orig_prepare)(id, SEL, CFDictionaryRef, void *);
static void hook_voice_controller(void);
static BOOL my_prepare(id self, SEL _cmd, CFDictionaryRef settings, void *error) {
    hook_voice_controller();
    CFMutableDictionaryRef m = settings ? CFDictionaryCreateMutableCopy(NULL, 0, settings)
        : CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int fmt = 'lpcm', one = 1, bits = 16, no = 0;
    CFNumberRef n;
#define SETI(k, v) n = CFNumberCreate(NULL, kCFNumberIntType, &(v)); CFDictionarySetValue(m, CFSTR(k), n); CFRelease(n)
    SETI("AVFormatIDKey", fmt); SETI("AVNumberOfChannelsKey", one); SETI("AVLinearPCMBitDepthKey", bits);
    SETI("AVLinearPCMIsBigEndianKey", no); SETI("AVLinearPCMIsFloatKey", no);
    // Siri passes rate 0 ("codec default") for Speex; PCM needs a real rate, and assistantd only maps
    // 8/11.025/16/22.05/32 kHz PCM to an ACE codec.
    double r = 16000; n = CFNumberCreate(NULL, kCFNumberDoubleType, &r); CFDictionarySetValue(m, CFSTR("AVSampleRateKey"), n); CFRelease(n);
    if (dbg) {
        CFStringRef d = CFCopyDescription(settings ? settings : (CFDictionaryRef)kCFNull), d2 = CFCopyDescription(m);
        char a[1024] = "", b2[1024] = "";
        CFStringGetCString(d, a, sizeof a, kCFStringEncodingUTF8); CFStringGetCString(d2, b2, sizeof b2, kCFStringEncodingUTF8);
        fprintf(dbg, "SiriCapability: recording as 16-bit PCM instead of Speex\n  in: %s\n  out: %s\n", a, b2); fflush(dbg);
        CFRelease(d); CFRelease(d2);
    }
    BOOL ok = orig_prepare(self, _cmd, m, error);
    if (dbg) { fprintf(dbg, "  prepare -> %d\n", ok); fflush(dbg); }
    CFRelease(m);
    return ok;
}

// Debug aid (only if /tmp/siri-debug exists): copy assistantd's AppSupport _CPLog output, at
// every level, to /tmp/assistantd.log.
#include <stdarg.h>
#include <unistd.h>
// assistantd: if (MaxAccepted >= L && (CustomMaxConsoleLevel(ctx) >= L || CustomMaxLogFileLevel(ctx) >= L))
//                _CPLog(L, ctx, facility, format, ...)
static void (*orig_CPLog)(int, CFStringRef, CFStringRef, CFStringRef, ...);
static void my_CPLog(int level, CFStringRef context, CFStringRef facility, CFStringRef format, ...) {
    va_list ap; va_start(ap, format);
    CFStringRef s = CFStringCreateWithFormatAndArguments(NULL, NULL, format, ap);
    va_end(ap);
    char buf[4096], c[64] = "";
    if (context && CFGetTypeID(context) == CFStringGetTypeID()) CFStringGetCString(context, c, sizeof c, kCFStringEncodingUTF8);
    if (s && CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) { fprintf(dbg, "[%d %s] %s\n", level, c, buf); fflush(dbg); }
    if (s) CFRelease(s);
}
static int my_max_level(CFStringRef ctx) { return 7; }

// Rebind a symbol pointer in every loaded image (writes __DATA only: Substrate can't patch code in
// assistantd, which "is not CS_VALID").
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
static void rebind(const struct mach_header *mh, intptr_t slide, const char *name, void *repl, void **orig) {
    const struct load_command *lc = (const void *)(mh + 1);
    const struct segment_command *linkedit = NULL; const struct symtab_command *st = NULL; const struct dysymtab_command *dy = NULL;
    for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const char *)lc + lc->cmdsize)) {
        if (lc->cmd == LC_SEGMENT && !strcmp(((const struct segment_command *)lc)->segname, "__LINKEDIT")) linkedit = (const void *)lc;
        else if (lc->cmd == LC_SYMTAB) st = (const void *)lc;
        else if (lc->cmd == LC_DYSYMTAB) dy = (const void *)lc;
    }
    if (!linkedit || !st || !dy) return;
    uintptr_t base = slide + linkedit->vmaddr - linkedit->fileoff;
    const struct nlist *syms = (const void *)(base + st->symoff);
    const char *strs = (const char *)(base + st->stroff);
    const uint32_t *ind = (const void *)(base + dy->indirectsymoff);
    lc = (const void *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const char *)lc + lc->cmdsize)) {
        if (lc->cmd != LC_SEGMENT) continue;
        const struct segment_command *sg = (const void *)lc;
        const struct section *sec = (const void *)(sg + 1);
        for (uint32_t k = 0; k < sg->nsects; k++, sec++) {
            uint32_t t = sec->flags & SECTION_TYPE;
            if (t != S_LAZY_SYMBOL_POINTERS && t != S_NON_LAZY_SYMBOL_POINTERS) continue;
            void **slots = (void **)(slide + sec->addr);
            for (uint32_t j = 0; j < sec->size / 4; j++) {
                uint32_t si = ind[sec->reserved1 + j];
                if (si & (INDIRECT_SYMBOL_LOCAL | INDIRECT_SYMBOL_ABS)) continue;
                if (!strcmp(strs + syms[si].n_un.n_strx, name)) { if (orig && !*orig) *orig = dlsym(RTLD_DEFAULT, name + 1); slots[j] = repl; }
            }
        }
    }
}

// Test hook (only while /tmp/acesrv-test.wav exists): nobody is talking to the iPod during
// unattended tests, so Siri's on-device endpointer never hears a startpoint and fails with 207.
// Report a startpoint on the first recorded buffer and an endpoint 2 s later. acesrv then
// answers from the test WAV instead of the (silent) audio it was sent.
#include <sys/time.h>
static void (*orig_buffer)(id, SEL, id, id);
static double t_first, t_last;
static int fired;
static double now_s(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec + tv.tv_usec / 1e6; }
static void my_buffer(id self, SEL _cmd, id vc, id buf) {
    orig_buffer(self, _cmd, vc, buf);
    if (access("/tmp/acesrv-test.wav", F_OK) != 0) return;
    double t = now_s();
    if (t - t_last > 1.0) t_first = 0;   // a new recording started
    t_last = t;
    if (!t_first) {
        t_first = now_s(); fired = 0;
        ((void (*)(id, SEL, id))objc_msgSend)(self, sel_registerName("voiceControllerDidDetectStartpoint:"), vc);
        if (dbg) { fprintf(dbg, "test hook: startpoint\n"); fflush(dbg); }
    } else if (!fired && now_s() - t_first > 2.0) {
        fired = 1; t_first = 0;
        ((void (*)(id, SEL, id))objc_msgSend)(self, sel_registerName("voiceControllerDidDetectEndpoint:"), vc);
        if (dbg) { fprintf(dbg, "test hook: endpoint\n"); fflush(dbg); }
    }
}

// ADAceConnection arms a response timer after every send, sized for Apple's servers. An on-device
// answer takes longer than that (speech recognition alone is 1-6 s), so assistantd would drop the
// loopback connection and resend the request. Disable it; the server is local.
static void no_timeout(id self, SEL _cmd) {
    if (dbg) { fprintf(dbg, "SiriCapability: suppressed -[ADAceConnection %s]\n", sel_getName(_cmd)); fflush(dbg); }
}

static void hook_voice_controller(void) {
    static int timeout_done;
    if (!timeout_done) {
        Class ac = (Class)objc_getClass("ADAceConnection");
        const char *names[] = { "_scheduleTimeout", "_timeout", "_safeTimeoutFired", NULL };
        for (int i = 0; ac && names[i]; i++) {
            Method tm = class_getInstanceMethod(ac, sel_registerName(names[i]));
            if (tm) { method_setImplementation(tm, (IMP)no_timeout); timeout_done = 1; }
        }
        if (timeout_done && dbg) { fprintf(dbg, "SiriCapability: ACE response timeouts disabled\n"); fflush(dbg); }
    }
    if (!orig_buffer) {
        Class sm = (Class)objc_getClass("ADSpeechManager");
        Method bm = sm ? class_getInstanceMethod(sm, sel_registerName("voiceControllerRecordBufferAvailable:buffer:")) : NULL;
        if (bm) orig_buffer = (void *)method_setImplementation(bm, (IMP)my_buffer);
    }
    if (orig_prepare) return;
    Class c = (Class)objc_getClass("AVVoiceController");
    Method mt = c ? class_getInstanceMethod(c, sel_registerName("prepareRecordWithSettings:error:")) : NULL;
    if (mt) orig_prepare = (void *)method_setImplementation(mt, (IMP)my_prepare);
    if (mt && dbg) { fprintf(dbg, "SiriCapability: hooked AVVoiceController\n"); fflush(dbg); }
}
static void image_added(const struct mach_header *mh, intptr_t slide) {
    hook_voice_controller();
    if (dbg) {
        rebind(mh, slide, "__CPLog", (void *)my_CPLog, (void **)&orig_CPLog);
        rebind(mh, slide, "__CPLoggingCustomMaxConsoleLevel", (void *)my_max_level, NULL);
        rebind(mh, slide, "__CPLoggingCustomMaxLogFileLevel", (void *)my_max_level, NULL);
    }
}

__attribute__((constructor)) static void init(void) {
    const char *exe = _dyld_get_image_name(0);
    if (exe && strstr(exe, "/assistantd")) {
        if (access("/tmp/siri-debug", F_OK) == 0 && (dbg = fopen("/tmp/assistantd.log", "a"))) {
            int *lvl = dlsym(RTLD_DEFAULT, "__CPLoggingMaxAcceptedLevel");
            if (lvl) {   // AppSupport lowers it again later; keep it at 7
                *lvl = 7;
                dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(0, 0));
                dispatch_source_set_timer(t, DISPATCH_TIME_NOW, 200 * NSEC_PER_MSEC, 50 * NSEC_PER_MSEC);
                dispatch_source_set_event_handler(t, ^{ *lvl = 7; });
                dispatch_resume(t);
            }
            fprintf(dbg, "--- assistantd %d (log level var %p)\n", getpid(), lvl); fflush(dbg);
        }
        _dyld_register_func_for_add_image(image_added);   // also runs for already-loaded images
        // assistantd's own classes (ADAceConnection) aren't registered with the runtime yet when
        // tweaks load; try again once the main run loop is up.
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{ hook_voice_controller(); });
    }
    void *mg = dlopen("/usr/lib/libMobileGestalt.dylib", RTLD_LAZY);
    if (!mg) return;
    void *b = dlsym(mg, "MGGetBoolAnswer"), *c = dlsym(mg, "MGCopyAnswer");   // MGGetBoolAnswer: iOS 7+
    if (b) MSHookFunction(hook_target(b), (void *)my_MGGetBoolAnswer, (void **)&orig_MGGetBoolAnswer);
    if (c) MSHookFunction(hook_target(c), (void *)my_MGCopyAnswer, (void **)&orig_MGCopyAnswer);
}
