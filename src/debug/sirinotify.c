// sirinotify <name>: posts com.lmy.siridebug.<name> (the iPod has no notifyutil)
#include <notify.h>
#include <stdio.h>
int main(int argc, char **argv) {
    char n[128]; snprintf(n, sizeof n, "com.lmy.siridebug.%s", argc > 1 ? argv[1] : "mic");
    return notify_post(n);
}
