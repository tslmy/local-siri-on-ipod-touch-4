// SiriDebug: test-only SpringBoard tweak so Siri can be driven and inspected over SSH.
//   sirinotify mic    -> presses Siri's mic button
//   sirinotify hook   -> only installs the logging hooks (needs Siri opened once)
//   (on first use it also logs Siri's error callbacks, with arguments and caller, to the syslog)
// Not installed by 4-install.sh; copy SiriDebug.dylib/.plist to /Library/MobileSubstrate/DynamicLibraries/.
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <notify.h>
#include <dispatch/dispatch.h>

static void log_calls(Class c, NSArray *sels) {
    for (NSString *name in sels) {
        SEL sel = NSSelectorFromString(name);
        Method m = class_getInstanceMethod(c, sel);
        if (!m) continue;
        IMP orig = method_getImplementation(m);
        int nargs = method_getNumberOfArguments(m) - 2;
        id block = ^(__unsafe_unretained id self, void *a, void *b, void *c2) {
            NSMutableString *args = [NSMutableString string];
            void *v[3] = { a, b, c2 };
            for (int i = 0; i < nargs && i < 3; i++) {
                if ((uintptr_t)v[i] < 0x10000) { [args appendFormat:@" | %lu", (unsigned long)(uintptr_t)v[i]]; continue; }   // BOOL/int
                id o = (__bridge id)v[i];
                [args appendFormat:@" | %@", o];
                for (NSError *e = o; [e isKindOfClass:[NSError class]]; e = e.userInfo[NSUnderlyingErrorKey])
                    [args appendFormat:@"\n    -> %@ %ld %@", e.domain, (long)e.code, e.userInfo];
            }
            NSLog(@"SiriDebug: -[%@ %@]%@\n%@", [self class], name, args,
                  [[[NSThread callStackSymbols] subarrayWithRange:NSMakeRange(1, 5)] componentsJoinedByString:@"\n"]);
            ((void (*)(id, SEL, void *, void *, void *))orig)(self, sel, a, b, c2);
        };
        method_setImplementation(m, imp_implementationWithBlock(block));
    }
}

static id plugin_with_hooks(void) {
    static BOOL hooked;
    id host = [NSClassFromString(@"SBAssistantController") performSelector:@selector(sharedInstance)];
    id plugin = [host valueForKey:@"pluginController"];
    if (plugin && !hooked) {
        hooked = YES;
        log_calls([plugin class], @[@"_handleError:", @"_displayNotReadyError", @"_listenFailed",
            @"assistantConnection:speechRecordingDidFail:", @"assistantConnection:speechRecognitionDidFail:",
            @"assistantConnection:requestFailedWithError:requestClass:", @"_sendFailedCommandForRefIdentifier:",
            @"speechSynthesizerDidStartSpeaking:", @"speechSynthesizer:didFinishSpeaking:withError:"]);
    }
    return plugin;
}

__attribute__((constructor)) static void init(void) {
    int tok, tok2;
    notify_register_dispatch("com.lmy.siridebug.hook", &tok2, dispatch_get_main_queue(), ^(int t) { plugin_with_hooks(); });
    notify_register_dispatch("com.lmy.siridebug.mic", &tok, dispatch_get_main_queue(), ^(int t) {
        id plugin = plugin_with_hooks();
        NSLog(@"SiriDebug: mic -> %@", plugin);
        if ([plugin respondsToSelector:@selector(_micButtonHit:)])
            [plugin performSelector:@selector(_micButtonHit:) withObject:nil];
    });
}
