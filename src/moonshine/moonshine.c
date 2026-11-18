// Moonshine Tiny speech recognition in plain C (+ optional ARM NEON).
// Weights come from convert.py (Q8_0 matrices, fp32 vectors), mmap'd read-only
// so the kernel can drop them under memory pressure.
//
// Build:  cc -O3 -ffast-math moonshine.c -DMOONSHINE_MAIN -lm -o moonshine
// Usage:  moonshine moonshine-tiny.bin moonshine-vocab.bin in.wav
#include "moonshine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "../common/q8.h"


typedef struct {
    const float *ln1; Q8 q, k, v, o;
    const float *ln2; Q8 fc1; const float *b1; Q8 fc2; const float *b2;
} EncLayer;

typedef struct {
    const float *ln1; Q8 q, k, v, o;
    const float *ln2; Q8 cq, ck, cv, co;
    const float *ln3; Q8 fc1; const float *b1; Q8 fc2; const float *b2;
} DecLayer;

struct moonshine {
    int D, FF, nenc, ndec, nh, vocab, hd, nout;
    float *conv1p; const float *conv1, *gn_w, *gn_b; Q8 conv2; const float *conv2_b; Q8 conv3; const float *conv3_b;
    EncLayer *enc; const float *enc_ln;
    Q8 embed; DecLayer *dec; const float *dec_ln;
    char **vocab_str; int *vocab_len;
    void *map; size_t map_len; int fd;
    double t_enc, t_dec; int ntok;
};

static double now_ms(void) { struct timeval tv; gettimeofday(&tv, 0); return tv.tv_sec * 1e3 + tv.tv_usec / 1e3; }

/* ---------- loading ---------- */
static const uint8_t *P;
static const float *take_f(int n) { const float *r = (const float *)P; P += (size_t)n * 4; return r; }
static int packed;
static int wbits = 8;
static Q8 take_q(int out, int in) { return q8_take_ex(&P, out, in, wbits, packed); }

moonshine_t *moonshine_load(const char *model, const char *vocab) {
    int fd = open(model, O_RDONLY);
    if (fd < 0) { perror(model); return NULL; }
    struct stat st; fstat(fd, &st);
    void *map = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return NULL; }
    const int32_t *h = (const int32_t *)map;
    if (h[0] != 0x4d4f4f4e || h[1] < 1 || h[1] > 3) { fprintf(stderr, "bad model file\n"); return NULL; }
    moonshine_t *m = calloc(1, sizeof *m);
    m->map = map; m->map_len = st.st_size; m->fd = fd; packed = h[1] >= 2; wbits = h[1] == 3 ? 4 : 8;
    m->D = h[2]; m->FF = h[3]; m->nenc = h[4]; m->ndec = h[5]; m->nh = h[6]; m->vocab = h[7];
    m->hd = m->D / m->nh;
    int D = m->D, FF = m->FF;
    P = (const uint8_t *)map + 32;
    m->conv1 = take_f(D * 127);
    m->conv1p = calloc(D * 128, 4);
    for (int c = 0; c < D; c++) memcpy(m->conv1p + c * 128, m->conv1 + c * 127, 127 * 4); m->gn_w = take_f(D); m->gn_b = take_f(D);
    m->conv2 = take_q(2 * D, D * 7); m->conv2_b = take_f(2 * D);
    m->conv3 = take_q(D, 2 * D * 3); m->conv3_b = take_f(D);
    m->enc = calloc(m->nenc, sizeof(EncLayer));
    for (int l = 0; l < m->nenc; l++) {
        EncLayer *L = &m->enc[l];
        L->ln1 = take_f(D);
        L->q = take_q(D, D); L->k = take_q(D, D); L->v = take_q(D, D); L->o = take_q(D, D);
        L->ln2 = take_f(D);
        L->fc1 = take_q(FF, D); L->b1 = take_f(FF); L->fc2 = take_q(D, FF); L->b2 = take_f(D);
    }
    m->enc_ln = take_f(D);
    m->embed = take_q(m->vocab, D);
    m->nout = m->vocab;   // (limiting this like llm.c's LLM_VOCAB_LIMIT wrecks transcripts: ids aren't frequency-ordered)
    m->dec = calloc(m->ndec, sizeof(DecLayer));
    for (int l = 0; l < m->ndec; l++) {
        DecLayer *L = &m->dec[l];
        L->ln1 = take_f(D);
        L->q = take_q(D, D); L->k = take_q(D, D); L->v = take_q(D, D); L->o = take_q(D, D);
        L->ln2 = take_f(D);
        L->cq = take_q(D, D); L->ck = take_q(D, D); L->cv = take_q(D, D); L->co = take_q(D, D);
        L->ln3 = take_f(D);
        L->fc1 = take_q(2 * FF, D); L->b1 = take_f(2 * FF); L->fc2 = take_q(D, FF); L->b2 = take_f(D);
    }
    m->dec_ln = take_f(D);
    if ((size_t)(P - (const uint8_t *)map) != (size_t)st.st_size) {
        fprintf(stderr, "model size mismatch: used %ld of %lld\n", (long)(P - (const uint8_t *)map), (long long)st.st_size);
        return NULL;
    }
    FILE *vf = fopen(vocab, "rb");
    if (!vf) { perror(vocab); return NULL; }
    int nv; fread(&nv, 4, 1, vf);
    m->vocab_str = calloc(nv, sizeof(char *)); m->vocab_len = calloc(nv, sizeof(int));
    for (int i = 0; i < nv; i++) {
        uint16_t n; fread(&n, 2, 1, vf);
        m->vocab_str[i] = malloc(n + 1); fread(m->vocab_str[i], 1, n, vf); m->vocab_str[i][n] = 0;
        m->vocab_len[i] = n;
    }
    fclose(vf);
    return m;
}

void moonshine_prefetch(moonshine_t *m) { prefetch_range(m->fd, 0, m->map_len); }

void moonshine_advise(moonshine_t *m, int need) {
    madvise(m->map, m->map_len, need ? MADV_WILLNEED : MADV_DONTNEED);
}

void moonshine_free(moonshine_t *m) {
    if (!m) return;
    munmap(m->map, m->map_len);
    for (int i = 0; i < m->vocab; i++) free(m->vocab_str[i]);
    free(m->vocab_str); free(m->vocab_len); free(m->enc); free(m->dec); free(m);
}

/* ---------- kernels ---------- */
static void layernorm(float *o, const float *x, const float *w, int n) {
    float mean = 0, var = 0;
    for (int i = 0; i < n; i++) mean += x[i];
    mean /= n;
    for (int i = 0; i < n; i++) { float d = x[i] - mean; var += d * d; }
    float inv = 1.f / sqrtf(var / n + 1e-5f);
    for (int i = 0; i < n; i++) o[i] = (x[i] - mean) * inv * w[i];
}

// Partial interleaved rotary: first 32 dims of each 36-dim head, pairs (2i,2i+1).
#define ROPE_MAXPOS 1024
static float rope_cs[ROPE_MAXPOS][32]; static int rope_ready, rope_rd;
static void rope(float *v, int pos, int nh, int hd) {
    int rd = (int)(hd * 0.9f); rd &= ~1;
    if (!rope_ready) {
        rope_rd = rd;
        for (int p = 0; p < ROPE_MAXPOS; p++)
            for (int i = 0; i < rd / 2 && i < 16; i++) {
                double f = p * pow(10000.0, -(double)(2 * i) / rd);
                rope_cs[p][2 * i] = (float)cos(f); rope_cs[p][2 * i + 1] = (float)sin(f);
            }
        rope_ready = 1;
    }
    const float *cs = rope_cs[pos < ROPE_MAXPOS ? pos : ROPE_MAXPOS - 1];
    for (int h = 0; h < nh; h++) {
        float *p = v + h * hd;
        for (int i = 0; i < rd / 2; i++) {
            float c = cs[2 * i], s = cs[2 * i + 1], a = p[2 * i], b = p[2 * i + 1];
            p[2 * i] = a * c - b * s; p[2 * i + 1] = b * c + a * s;
        }
    }
}

// One query vector q (all heads) against K,V rows [0,n) with row stride D.
static void attend(float *out, const float *q, const float *K, const float *V, int n, int D, int nh, int hd, float *att) {
    float scale = 1.f / sqrtf((float)hd);
    for (int h = 0; h < nh; h++) {
        const float *qh = q + h * hd;
        float *oh = out + h * hd;
#ifdef HAVE_NEON
        if (hd == 36) {
            float32x4_t q0 = vld1q_f32(qh), q1 = vld1q_f32(qh + 4), q2 = vld1q_f32(qh + 8), q3 = vld1q_f32(qh + 12),
                        q4 = vld1q_f32(qh + 16), q5 = vld1q_f32(qh + 20), q6 = vld1q_f32(qh + 24), q7 = vld1q_f32(qh + 28),
                        q8 = vld1q_f32(qh + 32);
            for (int t = 0; t < n; t++) {
                const float *k = K + (size_t)t * D + h * hd;
                float32x4_t a = vmulq_f32(q0, vld1q_f32(k));
                a = vmlaq_f32(a, q1, vld1q_f32(k + 4)); a = vmlaq_f32(a, q2, vld1q_f32(k + 8));
                a = vmlaq_f32(a, q3, vld1q_f32(k + 12)); a = vmlaq_f32(a, q4, vld1q_f32(k + 16));
                a = vmlaq_f32(a, q5, vld1q_f32(k + 20)); a = vmlaq_f32(a, q6, vld1q_f32(k + 24));
                a = vmlaq_f32(a, q7, vld1q_f32(k + 28)); a = vmlaq_f32(a, q8, vld1q_f32(k + 32));
                float32x2_t s2 = vadd_f32(vget_low_f32(a), vget_high_f32(a));
                vst1_lane_f32(att + t, vpadd_f32(s2, s2), 0);
            }
            // max
            float32x4_t mx4 = vdupq_n_f32(-1e30f); int t = 0;
            for (; t + 4 <= n; t += 4) mx4 = vmaxq_f32(mx4, vld1q_f32(att + t));
            float32x2_t m2 = vpmax_f32(vget_low_f32(mx4), vget_high_f32(mx4)); m2 = vpmax_f32(m2, m2);
            for (; t < n; t++) m2 = vmax_f32(m2, vld1_dup_f32(att + t));
            float32x4_t mxv = vcombine_f32(m2, m2), sc = vdupq_n_f32(scale), sum4 = vdupq_n_f32(0);
            for (t = 0; t + 4 <= n; t += 4) {
                float32x4_t e = exp_ps(vmulq_f32(vsubq_f32(vld1q_f32(att + t), mxv), sc));
                vst1q_f32(att + t, e); sum4 = vaddq_f32(sum4, e);
            }
            for (; t < n; t++) {
                float32x4_t e = exp_ps(vmulq_f32(vsubq_f32(vld1q_dup_f32(att + t), mxv), sc));
                vst1q_lane_f32(att + t, e, 0); sum4 = vaddq_f32(sum4, vsetq_lane_f32(0, vsetq_lane_f32(0, vsetq_lane_f32(0, e, 1), 2), 3));
            }
            float32x2_t s2 = vadd_f32(vget_low_f32(sum4), vget_high_f32(sum4)); s2 = vpadd_f32(s2, s2);
            float32x4_t o0 = vdupq_n_f32(0), o1 = o0, o2 = o0, o3 = o0, o4 = o0, o5 = o0, o6 = o0, o7 = o0, o8 = o0;
            for (t = 0; t < n; t++) {
                const float *v = V + (size_t)t * D + h * hd;
                float32x4_t w = vld1q_dup_f32(att + t);
                o0 = vmlaq_f32(o0, w, vld1q_f32(v)); o1 = vmlaq_f32(o1, w, vld1q_f32(v + 4)); o2 = vmlaq_f32(o2, w, vld1q_f32(v + 8));
                o3 = vmlaq_f32(o3, w, vld1q_f32(v + 12)); o4 = vmlaq_f32(o4, w, vld1q_f32(v + 16)); o5 = vmlaq_f32(o5, w, vld1q_f32(v + 20));
                o6 = vmlaq_f32(o6, w, vld1q_f32(v + 24)); o7 = vmlaq_f32(o7, w, vld1q_f32(v + 28)); o8 = vmlaq_f32(o8, w, vld1q_f32(v + 32));
            }
            float32x4_t inv = recip_ps(vcombine_f32(s2, s2));
            vst1q_f32(oh, vmulq_f32(o0, inv)); vst1q_f32(oh + 4, vmulq_f32(o1, inv)); vst1q_f32(oh + 8, vmulq_f32(o2, inv));
            vst1q_f32(oh + 12, vmulq_f32(o3, inv)); vst1q_f32(oh + 16, vmulq_f32(o4, inv)); vst1q_f32(oh + 20, vmulq_f32(o5, inv));
            vst1q_f32(oh + 24, vmulq_f32(o6, inv)); vst1q_f32(oh + 28, vmulq_f32(o7, inv)); vst1q_f32(oh + 32, vmulq_f32(o8, inv));
            continue;
        }
#endif
        float mx = -1e30f;
        for (int t = 0; t < n; t++) {
            const float *kh = K + (size_t)t * D + h * hd;
            float s = 0;
            for (int i = 0; i < hd; i++) s += qh[i] * kh[i];
            s *= scale; att[t] = s; if (s > mx) mx = s;
        }
        float sum = 0;
        for (int t = 0; t < n; t++) { att[t] = expf(att[t] - mx); sum += att[t]; }
        memset(oh, 0, hd * sizeof(float));
        for (int t = 0; t < n; t++) {
            const float *vh = V + (size_t)t * D + h * hd;
            float a = att[t] / sum;
            for (int i = 0; i < hd; i++) oh[i] += a * vh[i];
        }
    }
}

/* ---------- encoder ---------- */
// Returns malloc'd [T][D] encoder states.
static double prof[8];
#define PROF(i) do { double _n = now_ms(); prof[i] += _n - _pt; _pt = _n; } while (0)
static float *encode(moonshine_t *m, const float *audio, int n, int *Tout) {
    double _pt = now_ms();
    int D = m->D, FF = m->FF;
    if (n < 127 + 64 * 20) return NULL;
    int L1 = (n - 127) / 64 + 1, L2 = (L1 - 7) / 3 + 1, T = (L2 - 3) / 2 + 1;
    float *h1 = malloc((size_t)L1 * D * 4);
    float *ap = calloc((size_t)n + 128, 4); memcpy(ap, audio, (size_t)n * 4);
    for (int t = 0; t < L1; t++) {
        const float *x = ap + (size_t)t * 64;
        float *o = h1 + (size_t)t * D;
        for (int c = 0; c < D; c++) {
            const float *w = m->conv1p + c * 128;
#ifdef HAVE_NEON
            float32x4_t a0 = vdupq_n_f32(0), a1 = a0;
            for (int k = 0; k < 128; k += 8) {
                a0 = vmlaq_f32(a0, vld1q_f32(w + k), vld1q_f32(x + k));
                a1 = vmlaq_f32(a1, vld1q_f32(w + k + 4), vld1q_f32(x + k + 4));
            }
            float32x4_t a = vaddq_f32(a0, a1);
            float32x2_t s2 = vadd_f32(vget_low_f32(a), vget_high_f32(a));
            vst1_lane_f32(o + c, vpadd_f32(s2, s2), 0);
#else
            float s = 0;
            for (int k = 0; k < 128; k++) s += w[k] * x[k];
            o[c] = s;
#endif
        }
    }
    free(ap);
    tanh_vec(h1, (size_t)L1 * D);
    PROF(0);
    double mean = 0, var = 0; size_t N = (size_t)L1 * D;
    for (size_t i = 0; i < N; i++) mean += h1[i];
    mean /= N;
    for (size_t i = 0; i < N; i++) { double d = h1[i] - mean; var += d * d; }
    float inv = (float)(1.0 / sqrt(var / N + 1e-5));
    for (int t = 0; t < L1; t++)
        for (int c = 0; c < D; c++) { float *p = &h1[(size_t)t * D + c]; *p = (*p - (float)mean) * inv * m->gn_w[c] + m->gn_b[c]; }

    int maxq = (L1 * D > L2 * 2 * D ? L1 * D : L2 * 2 * D);
    if (T * FF > maxq) maxq = T * FF;
    int8_t *xq = malloc(maxq); float *xs = malloc(maxq / GS * 4);
    quantize(h1, xq, xs, L1 * D);
    PROF(1);
    float *h2 = malloc((size_t)L2 * 2 * D * 4);
    matmul(h2, xq, xs, L2, 3 * D, &m->conv2, m->conv2_b);       // im2col row t = h1[3t..3t+6]
    PROF(2);
    gelu_vec(h2, (size_t)L2 * 2 * D);
    PROF(3);
    free(h1);
    quantize(h2, xq, xs, L2 * 2 * D);
    float *x = malloc((size_t)T * D * 4);
    matmul(x, xq, xs, T, 2 * 2 * D, &m->conv3, m->conv3_b);
    gelu_vec(x, (size_t)T * D);
    free(h2);
    PROF(2);

    float *hn = malloc((size_t)T * D * 4), *q = malloc((size_t)T * D * 4), *k = malloc((size_t)T * D * 4),
          *v = malloc((size_t)T * D * 4), *ff = malloc((size_t)T * FF * 4), *att = malloc(T * 4);
    for (int l = 0; l < m->nenc; l++) {
        EncLayer *L = &m->enc[l];
        for (int t = 0; t < T; t++) layernorm(hn + t * D, x + t * D, L->ln1, D);
        quantize(hn, xq, xs, T * D);
        matmul(q, xq, xs, T, D, &L->q, NULL);
        matmul(k, xq, xs, T, D, &L->k, NULL);
        matmul(v, xq, xs, T, D, &L->v, NULL);
        PROF(4);
        for (int t = 0; t < T; t++) { rope(q + t * D, t, m->nh, m->hd); rope(k + t * D, t, m->nh, m->hd); }
        for (int t = 0; t < T; t++) attend(hn + t * D, q + t * D, k, v, T, D, m->nh, m->hd, att);
        PROF(5);
        quantize(hn, xq, xs, T * D);
        matmul(q, xq, xs, T, D, &L->o, NULL);
        for (size_t i = 0; i < (size_t)T * D; i++) x[i] += q[i];
        for (int t = 0; t < T; t++) layernorm(hn + t * D, x + t * D, L->ln2, D);
        quantize(hn, xq, xs, T * D);
        matmul(ff, xq, xs, T, D, &L->fc1, L->b1);
        PROF(4);
        gelu_vec(ff, (size_t)T * FF);
        PROF(3);
        quantize(ff, xq, xs, T * FF);
        matmul(q, xq, xs, T, FF, &L->fc2, L->b2);
        for (size_t i = 0; i < (size_t)T * D; i++) x[i] += q[i];
    }
    PROF(4);
    for (int t = 0; t < T; t++) layernorm(x + t * D, x + t * D, m->enc_ln, D);
    if (getenv("MS_PROF")) fprintf(stderr, "conv1 %.0f gn %.0f conv23 %.0f gelu %.0f layer-matmul %.0f attn %.0f\n", prof[0], prof[1], prof[2], prof[3], prof[4], prof[5]);
    memset(prof, 0, sizeof prof);
    free(hn); free(q); free(k); free(v); free(ff); free(att); free(xq); free(xs);
    *Tout = T;
    return x;
}

/* ---------- decoder ---------- */
int moonshine_transcribe(moonshine_t *m, const float *audio, int n, char *out, int outlen) {
    int D = m->D, FF = m->FF, nh = m->nh, hd = m->hd, T;
    double t0 = now_ms();
    float *enc = encode(m, audio, n, &T);
    if (!enc) { out[0] = 0; return 0; }
    double t1 = now_ms();
    int maxtok = (int)(n / 16000.0 * 6.5) + 4;
    if (maxtok > 192) maxtok = 192;

    int8_t *xq = malloc(T * D > 2 * FF ? T * D : 2 * FF); float *xs = malloc((T * D > 2 * FF ? T * D : 2 * FF) / GS * 4);
    // cross-attention K/V for every layer
    float *CK = malloc((size_t)m->ndec * T * D * 4), *CV = malloc((size_t)m->ndec * T * D * 4);
    quantize(enc, xq, xs, T * D);
    for (int l = 0; l < m->ndec; l++) {
        matmul(CK + (size_t)l * T * D, xq, xs, T, D, &m->dec[l].ck, NULL);
        matmul(CV + (size_t)l * T * D, xq, xs, T, D, &m->dec[l].cv, NULL);
    }
    free(enc);
    float *SK = malloc((size_t)m->ndec * maxtok * D * 4), *SV = malloc((size_t)m->ndec * maxtok * D * 4);
    float *x = malloc(D * 4), *hn = malloc(D * 4), *q = malloc(D * 4), *a = malloc(D * 4),
          *ff = malloc(2 * FF * 4), *logits = malloc(m->vocab * 4);
    float *att = malloc((T > maxtok ? T : maxtok) * 4);
    int tok = 1, pos = 0, len = 0;
    out[0] = 0;
    for (; pos < maxtok; pos++) {
        // embedding (dequantize row)
        q8_row(&m->embed, tok, x);
        for (int l = 0; l < m->ndec; l++) {
            DecLayer *L = &m->dec[l];
            float *Kc = SK + (size_t)l * maxtok * D, *Vc = SV + (size_t)l * maxtok * D;
            layernorm(hn, x, L->ln1, D);
            quantize(hn, xq, xs, D);
            matmul(q, xq, xs, 1, D, &L->q, NULL);
            matmul(Kc + pos * D, xq, xs, 1, D, &L->k, NULL);
            matmul(Vc + pos * D, xq, xs, 1, D, &L->v, NULL);
            rope(q, pos, nh, hd); rope(Kc + pos * D, pos, nh, hd);
            attend(a, q, Kc, Vc, pos + 1, D, nh, hd, att);
            quantize(a, xq, xs, D);
            matmul(q, xq, xs, 1, D, &L->o, NULL);
            for (int i = 0; i < D; i++) x[i] += q[i];

            layernorm(hn, x, L->ln2, D);
            quantize(hn, xq, xs, D);
            matmul(q, xq, xs, 1, D, &L->cq, NULL);
            attend(a, q, CK + (size_t)l * T * D, CV + (size_t)l * T * D, T, D, nh, hd, att);
            quantize(a, xq, xs, D);
            matmul(q, xq, xs, 1, D, &L->co, NULL);
            for (int i = 0; i < D; i++) x[i] += q[i];

            layernorm(hn, x, L->ln3, D);
            quantize(hn, xq, xs, D);
            matmul(ff, xq, xs, 1, D, &L->fc1, L->b1);
            swiglu_vec(ff, ff + FF, FF);
            quantize(ff, xq, xs, FF);
            matmul(q, xq, xs, 1, FF, &L->fc2, L->b2);
            for (int i = 0; i < D; i++) x[i] += q[i];
        }
        layernorm(hn, x, m->dec_ln, D);
        quantize(hn, xq, xs, D);
        matmul(logits, xq, xs, 1, D, &m->embed, NULL);
        int best = 0;
        for (int i = 1; i < m->nout; i++) if (logits[i] > logits[best]) best = i;
        if (best == 2) break;
        tok = best;
        const char *s = m->vocab_str[tok]; int sl = m->vocab_len[tok];
        if (len == 0) while (sl > 0 && *s == ' ') { s++; sl--; }
        if (len + sl < outlen - 1) { memcpy(out + len, s, sl); len += sl; out[len] = 0; }
    }
    double t2 = now_ms();
    m->t_enc = t1 - t0; m->t_dec = t2 - t1; m->ntok = pos;
    free(xq); free(xs); free(CK); free(CV); free(SK); free(SV);
    free(x); free(hn); free(q); free(a); free(ff); free(logits); free(att);
    return len;
}

void moonshine_stats(moonshine_t *m, double *enc_ms, double *dec_ms, int *ntok) {
    *enc_ms = m->t_enc; *dec_ms = m->t_dec; *ntok = m->ntok;
}

/* ---------- WAV ---------- */
float *moonshine_read_wav(const char *path, int *nout) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(sz); fread(b, 1, sz, f); fclose(f);
    if (sz < 44 || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) { fprintf(stderr, "not a WAV\n"); free(b); return NULL; }
    int ch = 1, rate = 16000, bits = 16, fmt = 1; const uint8_t *data = NULL; uint32_t dlen = 0;
    for (long p = 12; p + 8 <= sz;) {
        uint32_t cl = b[p + 4] | b[p + 5] << 8 | b[p + 6] << 16 | (uint32_t)b[p + 7] << 24;
        if (!memcmp(b + p, "fmt ", 4)) {
            fmt = b[p + 8] | b[p + 9] << 8; ch = b[p + 10] | b[p + 11] << 8;
            rate = b[p + 12] | b[p + 13] << 8 | b[p + 14] << 16 | b[p + 15] << 24; bits = b[p + 22] | b[p + 23] << 8;
        } else if (!memcmp(b + p, "data", 4)) { data = b + p + 8; dlen = cl; if (p + 8 + dlen > (uint32_t)sz) dlen = sz - p - 8; break; }
        p += 8 + cl + (cl & 1);
    }
    if (!data || (fmt != 1 && fmt != 3 && fmt != 0xFFFE)) { fprintf(stderr, "unsupported WAV\n"); free(b); return NULL; }
    int bps = bits / 8, nfr = dlen / (bps * ch);
    float *mono = malloc(nfr * 4);
    for (int i = 0; i < nfr; i++) {
        float s = 0;
        for (int c = 0; c < ch; c++) {
            const uint8_t *p = data + (size_t)(i * ch + c) * bps;
            if (bits == 16) s += (int16_t)(p[0] | p[1] << 8) / 32768.f;
            else if (bits == 32 && fmt == 3) { float v; memcpy(&v, p, 4); s += v; }
            else if (bits == 32) s += (int32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24) / 2147483648.f;
            else if (bits == 8) s += (p[0] - 128) / 128.f;
        }
        mono[i] = s / ch;
    }
    free(b);
    if (rate == 16000) { *nout = nfr; return mono; }
    int n = (int)((double)nfr * 16000 / rate);
    float *r = malloc(n * 4);
    for (int i = 0; i < n; i++) {
        double src = (double)i * rate / 16000; int j = (int)src; double fr = src - j;
        r[i] = j + 1 < nfr ? (float)(mono[j] * (1 - fr) + mono[j + 1] * fr) : mono[nfr - 1];
    }
    free(mono); *nout = n; return r;
}

#ifdef MOONSHINE_MAIN
int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s model.bin vocab.bin in.wav...\n", argv[0]); return 1; }
    double t0 = now_ms();
    moonshine_t *m = moonshine_load(argv[1], argv[2]);
    if (!m) return 1;
    fprintf(stderr, "load %.0f ms\n", now_ms() - t0);
    for (int i = 3; i < argc; i++) {
        int n; float *a = moonshine_read_wav(argv[i], &n);
        if (!a) continue;
        char text[2048];
        moonshine_transcribe(m, a, n, text, sizeof text);
        double e, d; int nt; moonshine_stats(m, &e, &d, &nt);
        printf("%s\n", text);
        fprintf(stderr, "audio %.2f s | encoder %.0f ms | decoder %.0f ms (%d tok, %.0f ms/tok) | RTF %.2f\n",
                n / 16000.0, e, d, nt, d / (nt ? nt : 1), (e + d) / 1000.0 / (n / 16000.0));
        free(a);
    }
    moonshine_free(m);
    return 0;
}
#endif
