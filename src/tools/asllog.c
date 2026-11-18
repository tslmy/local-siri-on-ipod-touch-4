// asllog: print recent system log (ASL) messages; the iPod has no syslog file.
//   asllog [seconds=60] [substring]
#include <asl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 60;
    const char *needle = argc > 2 ? argv[2] : NULL;
    char since[32];
    snprintf(since, sizeof since, "%ld", (long)time(NULL) - secs);
    aslmsg q = asl_new(ASL_TYPE_QUERY);
    asl_set_query(q, ASL_KEY_TIME, since, ASL_QUERY_OP_GREATER_EQUAL | ASL_QUERY_OP_NUMERIC);
    aslresponse r = asl_search(NULL, q);
    aslmsg m;
    while ((m = aslresponse_next(r))) {
        const char *s = asl_get(m, ASL_KEY_SENDER), *t = asl_get(m, ASL_KEY_MSG), *tm = asl_get(m, ASL_KEY_TIME);
        if (!t) continue;
        if (needle && !strcasestr(t, needle) && !(s && strcasestr(s, needle))) continue;
        time_t tt = tm ? atol(tm) : 0; char buf[16]; strftime(buf, sizeof buf, "%H:%M:%S", localtime(&tt));
        printf("%s %s: %s\n", buf, s ? s : "?", t);
    }
    aslresponse_free(r);
    return 0;
}
