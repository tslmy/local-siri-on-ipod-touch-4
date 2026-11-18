// localsirid: on-device assistant backend for the iPod touch 4G.
// Listens on 127.0.0.1:7788. One request per connection:
//   "AUD1" u32le n  int16le[n] (16 kHz mono)   -> speech recognition, then LLM
//   "TXT1" u32le n  utf8[n]                    -> LLM only
//   "WRM1" u32le 0                              -> load the speech model now (sent when the user starts talking)
// Replies with \n-terminated, tab-separated lines:
//   HEARD\t<text>          (AUD1 only)
//   TOK\t<piece>           (streamed; '\n' in a piece is sent as "\\n")
//   DONE\t<asr_ms>\t<prefill_ms>\t<gen_ms>\t<ntok>
//   ERR\t<message>
// Both models are mmap'd; before each stage the other model's pages are
// dropped (MADV_DONTNEED) so they take turns in the iPod's 256 MB.
#include "../moonshine/moonshine.h"
#include "../llm/llm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <math.h>

#define PORT 7788
#define MAXSEQ 192
#define MAXNEW 60

static const char *SYSTEM_PROMPT =
    "You are Siri, a helpful voice assistant on an iPod. Answer in one or two short sentences.";

static moonshine_t *asr; static llm_t *lm; static int prefix_len, evict = 1, kill_voiced = 0;

static double now_ms(void) { struct timeval tv; gettimeofday(&tv, 0); return tv.tv_sec * 1e3 + tv.tv_usec / 1e3; }

static int readn(int fd, void *buf, size_t n) {
    uint8_t *p = buf;
    while (n) { ssize_t r = read(fd, p, n); if (r <= 0) return -1; p += r; n -= r; }
    return 0;
}
static void sendline(int fd, const char *tag, const char *s, int len) {
    char buf[1024]; int o = snprintf(buf, sizeof buf, "%s\t", tag);
    for (int i = 0; i < len && o < (int)sizeof buf - 4; i++) {
        if (s[i] == '\n') { buf[o++] = '\\'; buf[o++] = 'n'; }
        else if (s[i] == '\t') buf[o++] = ' ';
        else buf[o++] = s[i];
    }
    buf[o++] = '\n';
    write(fd, buf, o);
}

typedef struct { int fd; int n, sentences; } Gen;
// Stream each token; stop after two sentences (or one long one) like Siri would.
static int tok_cb(void *ud, const char *piece, int len) {
    Gen *g = ud; sendline(g->fd, "TOK", piece, len); g->n++;
    while (len > 0 && (piece[len - 1] == '"' || piece[len - 1] == ')')) len--;
    if (len > 0 && (piece[len - 1] == '.' || piece[len - 1] == '!' || piece[len - 1] == '?')) {
        g->sentences++;
        if (g->sentences >= 2 || g->n >= 28) return 1;
    }
    return 0;
}

static void answer(int fd, const char *question, double asr_ms) {
    int ids[MAXSEQ];
    llm_set_pos(lm, prefix_len);
    int n = llm_chat_user(lm, question, ids, MAXSEQ - prefix_len - MAXNEW);
    if (llm_feed(lm, ids, n) < 0) { sendline(fd, "ERR", "question too long", 17); return; }
    Gen g = {fd, 0, 0};
    llm_generate(lm, MAXNEW, 1.1f, tok_cb, &g);
    double pm, gm; int pn, gn;
    llm_stats(lm, &pm, &pn, &gm, &gn);
    char buf[128];
    int l = snprintf(buf, sizeof buf, "DONE\t%.0f\t%.0f\t%.0f\t%d\n", asr_ms, pm, gm, gn);
    write(fd, buf, l);
    fprintf(stderr, "[localsirid] q=\"%s\" asr %.0f ms, prompt %d tok %.0f ms, gen %d tok %.0f ms (%.2f tok/s)\n",
            question, asr_ms, pn, pm, gn, gm, gn ? gn * 1000.0 / gm : 0);
}

// Keep the last request's audio as a WAV for debugging (/var/root/siri/last.wav).
static void save_wav(const char *path, const int16_t *pcm, uint32_t n) {
    FILE *f = fopen(path, "wb"); if (!f) return;
    uint32_t dl = n * 2, rl = 36 + dl, sr = 16000, br = 32000, fl = 16; uint16_t pcmf = 1, ch = 1, ba = 2, bits = 16;
    fwrite("RIFF", 1, 4, f); fwrite(&rl, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fl, 4, 1, f);
    fwrite(&pcmf, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f); fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&dl, 4, 1, f); fwrite(pcm, 2, n, f); fclose(f);
}
static int has_letters(const char *s) { for (; *s; s++) if ((*s | 32) >= 'a' && (*s | 32) <= 'z') return 1; return 0; }

static void handle(int fd) {
    char magic[4]; uint32_t n;
    if (readn(fd, magic, 4) || readn(fd, &n, 4)) return;
    if (!memcmp(magic, "WRM1", 4)) {
        double t0 = now_ms();
        moonshine_prefetch(asr);
        fprintf(stderr, "[localsirid] warm: speech model in %.0f ms\n", now_ms() - t0);
        write(fd, "OK\n", 3);
    } else if (!memcmp(magic, "TXT1", 4) && n < 4096) {
        char *q = malloc(n + 1);
        if (readn(fd, q, n) == 0) { q[n] = 0; answer(fd, q, 0); }
        free(q);
    } else if (!memcmp(magic, "AUD1", 4) && n < 16000 * 30) {
        int16_t *pcm = malloc(n * 2); float *a = malloc(n * 4);
        if (readn(fd, pcm, n * 2) == 0) {
            save_wav("/var/root/siri/last.wav", pcm, n);
            // loudest 100 ms window; below ~-40 dBFS there is no speech worth decoding
            double peak = 0, best = 0;
            for (uint32_t i = 0; i < n; i++) { a[i] = pcm[i] / 32768.f; if (fabs(a[i]) > peak) peak = fabs(a[i]); }
            for (uint32_t w = 0; w + 1600 <= n; w += 800) {
                double e = 0; for (uint32_t i = w; i < w + 1600; i++) e += a[i] * a[i];
                if (e / 1600 > best) best = e / 1600;
            }
            best = sqrt(best);
            fprintf(stderr, "[localsirid] audio %.2f s, peak %.3f, loudest-100ms rms %.4f\n", n / 16000.0, peak, best);
            if (best < 0.02) {
                sendline(fd, "HEARD", "", 0); write(fd, "DONE\t0\t0\t0\t0\n", 13);
                free(pcm); free(a); return;
            }
            // Trim silence: Siri's endpointer often keeps recording for seconds after you stop, and
            // speech recognition time grows with audio length. Siri's end tone lands in the last
            // ~100 ms (a loud click), so drop that, then keep the span of 100 ms windows louder than
            // 8x the noise floor (20th percentile), padded by 300 ms.
            if (n > 3200) n -= 1600;
            {
                int nw = n >= 1600 ? (int)((n - 1600) / 800 + 1) : 0;
                float *r = malloc((nw + 1) * sizeof *r), *srt = malloc((nw + 1) * sizeof *srt);
                for (int k = 0; k < nw; k++) {
                    double e = 0; for (uint32_t i2 = k * 800; i2 < k * 800 + 1600; i2++) e += a[i2] * a[i2];
                    r[k] = srt[k] = sqrt(e / 1600);
                }
                for (int x = 1; x < nw; x++) { float v = srt[x]; int y = x - 1; while (y >= 0 && srt[y] > v) { srt[y + 1] = srt[y]; y--; } srt[y + 1] = v; }
                double floor_ = nw ? srt[nw / 5] : 0, thr = floor_ * 8 > 0.004 ? floor_ * 8 : 0.004;
                long first = -1, last = -1;
                for (int k = 0; k < nw; k++) if (r[k] > thr) { if (first < 0) first = k * 800L; last = k * 800L + 1600; }
                if (first >= 0) {
                    long s0 = first - 4800 > 0 ? first - 4800 : 0, s1 = last + 4800 < (long)n ? last + 4800 : (long)n;
                    if (s0 > 0 || s1 < (long)n) {
                        fprintf(stderr, "[localsirid] trimmed %.2f s -> %.2f s (floor %.4f)\n", n / 16000.0, (s1 - s0) / 16000.0, floor_);
                        memmove(a, a + s0, (s1 - s0) * sizeof *a); n = (uint32_t)(s1 - s0);
                    }
                }
                free(r); free(srt);
            }
            double t0 = now_ms();
            // Optionally free the TTS daemon's RAM (voiced, ~18 MB). Off by default: with the native
            // Siri UI it kills Siri mid-sentence, and prefetching made it unnecessary.
            if (kill_voiced) system("/usr/bin/killall voiced 2>/dev/null");
            if (evict) llm_advise(lm, 0);
            moonshine_prefetch(asr);   // usually already warm (WRM1 at speech start); cheap if so
            char text[1024];
            moonshine_transcribe(asr, a, n, text, sizeof text);
            double asr_ms = now_ms() - t0;
            sendline(fd, "HEARD", text, (int)strlen(text));   // Siri shows the words now
            if (evict) moonshine_advise(asr, 0);
            double tp = now_ms();
            llm_prefetch(lm);          // sequential read: ~1-3 s, versus ~10 s of page faults under pressure
            fprintf(stderr, "[localsirid] llm prefetch %.0f ms\n", now_ms() - tp);
            fprintf(stderr, "[localsirid] heard %.2f s audio in %.0f ms: \"%s\"\n", n / 16000.0, asr_ms, text);
            if (has_letters(text)) answer(fd, text, asr_ms);
            else write(fd, "DONE\t0\t0\t0\t0\n", 13);
        }
        free(pcm); free(a);
    } else sendline(fd, "ERR", "bad request", 11);
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "/var/root/siri";
    const char *llm_name = argc > 2 ? argv[2] : "smollm2-135m-q4e8";
    char p1[512], p2[512];
    signal(SIGPIPE, SIG_IGN);
    if (getenv("LOCALSIRI_EVICT")) evict = atoi(getenv("LOCALSIRI_EVICT"));
    if (getenv("LOCALSIRI_KILL_VOICED")) kill_voiced = atoi(getenv("LOCALSIRI_KILL_VOICED"));
    setenv("LLM_VOCAB_LIMIT", "24576", 0);   // classifier over the 24k most common tokens (see llm.c)
    snprintf(p1, sizeof p1, "%s/moonshine-tiny.bin", dir); snprintf(p2, sizeof p2, "%s/moonshine-vocab.bin", dir);
    asr = moonshine_load(p1, p2);
    snprintf(p1, sizeof p1, "%s/%s.bin", dir, llm_name); snprintf(p2, sizeof p2, "%s/smollm2-135m.tok", dir);
    lm = llm_load(p1, p2, MAXSEQ);
    if (!asr || !lm) { fprintf(stderr, "model load failed\n"); return 1; }
    int ids[MAXSEQ];
    double t0 = now_ms();
    prefix_len = llm_chat_prefix(lm, SYSTEM_PROMPT, ids, MAXSEQ);
    llm_feed(lm, ids, prefix_len);
    double pm, gm; int pn, gn; llm_stats(lm, &pm, &pn, &gm, &gn);
    fprintf(stderr, "[localsirid] system prompt cached: %d tok in %.0f ms\n", prefix_len, now_ms() - t0);
    moonshine_advise(asr, 0);

    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET; a.sin_port = htons(PORT); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 4)) { perror("bind"); return 1; }
    fprintf(stderr, "[localsirid] listening on 127.0.0.1:%d\n", PORT);
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) continue;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        handle(c);
        close(c);
    }
}
