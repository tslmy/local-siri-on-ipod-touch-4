// Test client for localsirid:  sirictl file.wav   |   sirictl -t "question"
#include "../moonshine/moonshine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s file.wav | -t \"question\"\n", argv[0]); return 1; }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(7788); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof a)) { perror("connect"); return 1; }
    struct timeval t0; gettimeofday(&t0, 0);
    if (!strcmp(argv[1], "-t") && argc > 2) {
        uint32_t n = strlen(argv[2]);
        write(s, "TXT1", 4); write(s, &n, 4); write(s, argv[2], n);
    } else {
        int n; float *f = moonshine_read_wav(argv[1], &n);
        if (!f) return 1;
        int16_t *p = malloc(n * 2);
        for (int i = 0; i < n; i++) { float v = f[i] * 32767.f; p[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }
        uint32_t un = n;
        write(s, "AUD1", 4); write(s, &un, 4); write(s, p, n * 2);
    }
    char buf[4096]; ssize_t r;
    while ((r = read(s, buf, sizeof buf)) > 0) {
        struct timeval t; gettimeofday(&t, 0);
        double ms = (t.tv_sec - t0.tv_sec) * 1e3 + (t.tv_usec - t0.tv_usec) / 1e3;
        fprintf(stderr, "[%6.0f ms] ", ms);
        fwrite(buf, 1, r, stdout); fflush(stdout);
    }
    return 0;
}
