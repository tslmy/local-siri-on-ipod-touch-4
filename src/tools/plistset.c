// plistset: set or show a preference through CFPreferences (the iPod has no plutil/defaults).
//   plistset <domain> <key> bool|int|string <value>      e.g. plistset com.apple.assistant "Authentication Disabled" bool 1
//   plistset <domain> <key>                              (print current value)
// Run as the user who owns the preferences (mobile): su mobile -c "plistset ..."
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s domain key [bool|int|string value]\n", argv[0]); return 1; }
    CFStringRef dom = CFStringCreateWithCString(NULL, argv[1], kCFStringEncodingUTF8);
    CFStringRef key = CFStringCreateWithCString(NULL, argv[2], kCFStringEncodingUTF8);
    if (argc >= 5) {
        CFPropertyListRef v = NULL;
        if (!strcmp(argv[3], "bool")) v = atoi(argv[4]) ? kCFBooleanTrue : kCFBooleanFalse;
        else if (!strcmp(argv[3], "int")) { int i = atoi(argv[4]); v = CFNumberCreate(NULL, kCFNumberIntType, &i); }
        else v = CFStringCreateWithCString(NULL, argv[4], kCFStringEncodingUTF8);
        CFPreferencesSetAppValue(key, v, dom);
        if (!CFPreferencesAppSynchronize(dom)) { fprintf(stderr, "synchronize failed\n"); return 1; }
    } else if (argc == 4 && !strcmp(argv[3], "delete")) {
        CFPreferencesSetAppValue(key, NULL, dom); CFPreferencesAppSynchronize(dom);
    }
    CFPropertyListRef cur = CFPreferencesCopyAppValue(key, dom);
    if (cur) { CFShow(cur); CFRelease(cur); } else printf("(unset)\n");
    return 0;
}
