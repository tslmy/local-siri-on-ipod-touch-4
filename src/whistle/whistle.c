// Whistle (Cactus Compute, Apache-2.0) speech recognition in plain C + ARM NEON, for an armv7 Cortex-A8.
//
// It reads the published `whistle.cact` as is: mmap'd, with the 2-bit and 4-bit "CQ" weights used in place.
// A CQ group of 128 weights is w = norm * (codebook[idx] @ H), H the normalized 128-point Walsh-Hadamard
// matrix. Since H is symmetric, w . x = norm * (codebook[idx] . (H x)): the activation is rotated once per
// group (a fast Walsh-Hadamard transform), quantized to int8, and the weights never need dequantizing.
// The codebook levels are mapped to int8, so the inner loop is a NEON table lookup plus int8 multiplies.
//
// The model follows cactus/model/whistle.py; convert/whistle_ref.py is the NumPy reference for this file.
//
// Build:  cc -O3 -ffast-math whistle.c -DWHISTLE_MAIN -lm -o whistle
// Usage:  whistle whistle.cact in.wav...
#include "whistle.h"
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
#include "../common/cq.h"

#define TAG 0x05E12A84u
#define G 128          // CQ group size (all matrices here have in % 128 == 0)
#define MAXTOK 160     // decoded tokens per utterance
#define NL 4           // mHC lanes

static double prof[8];   // ms: 2 hada, 3 stem, 4 decoder logits, 5/6 decoder share of matmul/hada
static double now_ms(void) { struct timeval tv; gettimeofday(&tv, 0); return tv.tv_sec * 1e3 + tv.tv_usec / 1e3; }

static float h2f(uint16_t h) {
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, u;
    if (e == 0) {
        if (!m) u = s;
        else { e = 113; while (!(m & 1024)) { m <<= 1; e--; } m &= 1023; u = s | (e << 23) | (m << 13); }
    } else if (e == 31) u = s | 0x7f800000 | (m << 13);
    else u = s | ((e + 112) << 23) | (m << 13);
    float f; memcpy(&f, &u, 4); return f;
}

/* ---------- model ---------- */
typedef struct { float *d1, *d2, *b2, *d3, *d4, *w1a, *w1b, *w2a, *w2b, *w3a, *w3b, *cond_v, *cond_u; } Hada;
typedef struct { float *a_pre, *a_post, *a_res, *b_pre, *b_post, *b_res; CQ phi_pre, phi_post, phi_res; } MHC;
typedef struct {
    float *norm_in; CQ q, k, v; float *qt, *kt, *vt, *qn, *kn; CQ gate, out; float *post_norm, attn_gate, *pre_hada; Hada mlp;
    float *cross_norm; CQ cq, ck, cv, cgate, cout; float *cqn, *ckn, *post_cross_norm, cross_gate;
} Dec;
typedef struct {
    float *pre_hada0, *norm_in, *post_norm, *conv_norm, *conv_out_norm, *pre_hada; CQ q, k, v, gate, out;
    float *qn, *kn, attn_gate; CQ pw1, pw2; float *dw; Hada mlp0, mlp;
} Enc;
typedef struct { CQ tables, key, value; float *taps; } Engram;

struct whistle {
    int vocab, D, H, KV, L, QK, VD, taps, slots, sub, ntab, etaps, edil, norders, orders[4], nsites, sites[16];
    float rope;
    int text, EL, conv_k, stem_c, stages, mels, fft, win, hop;
    CQ emb; Dec *dec; MHC mhc, emhc; int *p1, *p2; Engram eng[16]; float *final_norm, pe_gate;
    Enc *enc; float *enc_final_norm, *stem_w, *stem_dw[4]; CQ stem_pw[4], stem_out; const float *mel; int mlo[128], mhi[128];
    char **piece; int *plen; uint8_t *ptype; int npieces;
    void *map; size_t map_len; int fd;
    double t_feat, t_enc, t_dec; int ntok;
};

static const uint8_t *B;   // file base during load
static const uint8_t *R;   // next directory record
static int nrec, irec;

typedef struct { int dtype, ndim, shape[4], group, bits; uint64_t off, nbytes; } Rec;
static Rec next_rec(void) {
    Rec r; uint32_t u[4]; uint16_t pad;
    if (irec >= nrec) { fprintf(stderr, "whistle: tensor directory ran out\n"); exit(1); }
    r.dtype = R[0]; r.ndim = R[1]; (void)pad;
    memcpy(u, R + 4, 16); for (int i = 0; i < 4; i++) r.shape[i] = (int)u[i];
    memcpy(&r.off, R + 20, 8); memcpy(&r.nbytes, R + 28, 8);
    memcpy(u, R + 36, 8); r.group = (int)u[0]; r.bits = (int)u[1];
    R += 44; irec++;
    return r;
}
static float *take_v(void) {   // FP16 / FP32 tensor -> malloc'd floats
    Rec r = next_rec();
    size_t n = 1; for (int i = 0; i < r.ndim; i++) n *= (size_t)r.shape[i];
    float *f = malloc(n * 4 + 16);
    if (r.dtype == 1) { const uint8_t *s = B + r.off; for (size_t i = 0; i < n; i++) { uint16_t h; memcpy(&h, s + 2 * i, 2); f[i] = h2f(h); } }
    else if (r.dtype == 2) memcpy(f, B + r.off, n * 4);
    else { fprintf(stderr, "whistle: expected a vector tensor at #%d (dtype %d)\n", irec - 1, r.dtype); exit(1); }
    return f;
}
static float take_s(void) { float *f = take_v(), v = f[0]; free(f); return v; }
static CQ take_q(void) {
    Rec r = next_rec();
    if (r.dtype != 3 || r.group != G || (r.bits != 2 && r.bits != 4) || r.shape[1] % G) {
        fprintf(stderr, "whistle: unsupported CQ tensor at #%d (bits %d group %d in %d)\n", irec - 1, r.bits, r.group, r.shape[1]); exit(1);
    }
    CQ w; w.out = r.shape[0]; w.in = r.shape[1]; w.bits = r.bits; w.g = G; w.q = B + r.off;
    int ng = w.in / G;
    const uint8_t *nb = w.q + (size_t)w.out * w.in * w.bits / 8;
    w.ns = malloc((size_t)w.out * ng * 4);
    float k = cq_kscale(w.bits, G);
    for (size_t i = 0; i < (size_t)w.out * ng; i++) { uint16_t h; memcpy(&h, nb + 2 * i, 2); w.ns[i] = h2f(h) * k; }
    return w;
}
static Hada take_hada(void) {
    Hada h;
    h.d1 = take_v(); h.d2 = take_v(); h.b2 = take_v(); h.d3 = take_v(); h.d4 = take_v();
    h.w1a = take_v(); h.w1b = take_v(); h.w2a = take_v(); h.w2b = take_v(); h.w3a = take_v(); h.w3b = take_v();
    h.cond_v = take_v(); h.cond_u = take_v();
    return h;
}
static MHC take_mhc(void) {
    MHC h;
    h.a_pre = take_v(); h.a_post = take_v(); h.a_res = take_v(); h.b_pre = take_v(); h.b_post = take_v(); h.b_res = take_v();
    h.phi_pre = take_q(); h.phi_post = take_q(); h.phi_res = take_q();
    return h;
}

whistle_t *whistle_load(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return NULL; }
    struct stat st; fstat(fd, &st);
    void *map = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return NULL; }
    const uint32_t *h = map;
    if (h[0] != TAG || h[2] != 28) { fprintf(stderr, "%s: not a Whistle .cact (tag %08x)\n", path, h[0]); return NULL; }
    whistle_t *m = calloc(1, sizeof *m);
    m->map = map; m->map_len = st.st_size; m->fd = fd;
    m->vocab = h[5]; m->D = h[7]; m->H = h[8]; m->KV = h[9]; m->L = h[10]; m->QK = h[11]; m->VD = h[12];
    m->taps = h[19]; m->slots = h[20]; m->sub = h[21]; m->ntab = h[22]; m->etaps = h[23]; m->edil = h[24];
    m->norders = h[26]; for (int i = 0; i < 4; i++) m->orders[i] = h[27 + i];
    m->nsites = h[31]; for (int i = 0; i < 16; i++) m->sites[i] = h[32 + i];
    memcpy(&m->rope, h + 48, 4);
    if (m->D != 512 || h[14] != 512 || h[15] != NL || m->sub != G || m->taps > 4 || m->etaps > 8) {
        fprintf(stderr, "whistle: unexpected geometry (dim %d, hada %u, lanes %u)\n", m->D, h[14], h[15]); return NULL;
    }
    const float *cb = (const float *)((const uint8_t *)map + 196);
    float u[16];
    for (int i = 0; i < 4; i++) u[i] = cb[i] * sqrtf((float)G);
    cq_set_codebook(2, u);
    for (int i = 0; i < 16; i++) u[i] = cb[12 + i] * sqrtf((float)G);
    cq_set_codebook(4, u);
    B = map; R = (const uint8_t *)map + 196 + 28 * 4; nrec = h[1]; irec = 0;

    m->emb = take_q();
    m->dec = calloc(m->L, sizeof(Dec));
    for (int l = 0; l < m->L; l++) {
        Dec *d = &m->dec[l];
        d->norm_in = take_v(); d->q = take_q(); d->k = take_q(); d->v = take_q();
        if (m->taps) { d->qt = take_v(); d->kt = take_v(); d->vt = take_v(); }
        d->qn = take_v(); d->kn = take_v(); d->gate = take_q(); d->out = take_q(); d->post_norm = take_v();
        d->attn_gate = take_s(); d->pre_hada = take_v(); d->mlp = take_hada();
    }
    m->mhc = take_mhc();
    float *p1 = take_v(), *p2 = take_v();
    m->p1 = malloc(512 * sizeof(int)); m->p2 = malloc(512 * sizeof(int));
    for (int i = 0; i < 512; i++) { m->p1[i] = (int)p1[i]; m->p2[i] = (int)p2[i]; }
    free(p1); free(p2);
    for (int s = 0; s < m->nsites; s++) { Engram *e = &m->eng[s]; e->tables = take_q(); e->key = take_q(); e->value = take_q(); e->taps = take_v(); }
    m->final_norm = take_v();
    float *man = take_v();
    if (man[0] != 3246.f) { fprintf(stderr, "whistle: no audio manifest (probe heads?)\n"); return NULL; }
    m->text = (int)man[2]; m->EL = (int)man[3]; m->conv_k = (int)man[5]; m->stem_c = (int)man[6];
    m->stages = (int)man[7]; m->mels = (int)man[8]; m->fft = (int)man[10]; m->win = (int)man[11]; m->hop = (int)man[12];
    if ((int)man[4] != NL || (int)man[9] != 16000 || m->fft != 512 || m->stages > 4) { fprintf(stderr, "whistle: unexpected audio geometry\n"); return NULL; }
    free(man);
    for (int l = 0; l < m->L; l++) {
        Dec *d = &m->dec[l];
        d->cross_norm = take_v(); d->cq = take_q(); d->ck = take_q(); d->cv = take_q(); d->cgate = take_q(); d->cout = take_q();
        d->cqn = take_v(); d->ckn = take_v(); d->post_cross_norm = take_v(); d->cross_gate = take_s();
    }
    m->pe_gate = take_s();
    m->enc = calloc(m->EL, sizeof(Enc));
    for (int l = 0; l < m->EL; l++) {
        Enc *e = &m->enc[l];
        e->pre_hada0 = take_v(); e->norm_in = take_v(); e->post_norm = take_v(); e->conv_norm = take_v(); e->conv_out_norm = take_v(); e->pre_hada = take_v();
        e->q = take_q(); e->k = take_q(); e->v = take_q(); e->gate = take_q(); e->out = take_q();
        e->qn = take_v(); e->kn = take_v(); e->attn_gate = take_s(); e->pw1 = take_q(); e->pw2 = take_q(); e->dw = take_v();
        e->mlp0 = take_hada(); e->mlp = take_hada();
    }
    m->emhc = take_mhc();
    m->enc_final_norm = take_v();
    m->stem_w = take_v();
    for (int i = 1; i < m->stages; i++) { m->stem_dw[i] = take_v(); m->stem_pw[i] = take_q(); }
    m->stem_out = take_q();
    Rec mr = next_rec();
    m->mel = (const float *)(B + mr.off);
    for (int j = 0; j < m->mels && j < 128; j++) {   // each mel filter is nonzero on a short run of bins
        m->mlo[j] = 257; m->mhi[j] = 0;
        for (int k = 0; k < 257; k++) if (m->mel[k * m->mels + j] != 0) { if (k < m->mlo[j]) m->mlo[j] = k; m->mhi[j] = k + 1; }
        if (m->mhi[j] == 0) m->mlo[j] = 0;
    }
    Rec tr = next_rec();
    if (tr.dtype != 4) { fprintf(stderr, "whistle: tokenizer missing\n"); return NULL; }
    const uint8_t *tb = B + tr.off;
    memcpy(&m->npieces, tb, 4);
    m->piece = calloc(m->npieces, sizeof(char *)); m->plen = calloc(m->npieces, sizeof(int)); m->ptype = calloc(m->npieces, 1);
    const uint8_t *p = tb + 24;
    for (int i = 0; i < m->npieces; i++) {
        uint16_t ln; memcpy(&ln, p + 5, 2);
        m->ptype[i] = p[4]; m->plen[i] = ln;
        m->piece[i] = malloc(ln + 1); memcpy(m->piece[i], p + 7, ln); m->piece[i][ln] = 0;
        p += 7 + ln;
    }
    return m;
}

void whistle_prefetch(whistle_t *m) { prefetch_range(m->fd, 0, m->map_len); }
void whistle_advise(whistle_t *m, int need) { madvise(m->map, m->map_len, need ? MADV_WILLNEED : MADV_DONTNEED); }
void whistle_stats(whistle_t *m, double *feat_ms, double *enc_ms, double *dec_ms, int *ntok) {
    *feat_ms = m->t_feat; *enc_ms = m->t_enc; *dec_ms = m->t_dec; *ntok = m->ntok;
}

static void dbg(const char *name, const float *x, size_t n) {
    const char *d = getenv("WHISTLE_DUMP"); if (!d) return;
    char path[512]; snprintf(path, sizeof path, "%s.%s", d, name);
    FILE *f = fopen(path, "wb"); if (f) { fwrite(x, 4, n, f); fclose(f); }
}

/* ---------- vector helpers ---------- */
static inline float sigm(float x) { return 1.f / (1.f + expf(-x)); }
static inline float silu1(float x) { return x / (1.f + expf(-x)); }

static float sumsq(const float *x, int n) {
    int i = 0; float ss = 0;
#ifdef HAVE_NEON
    float32x4_t a = vdupq_n_f32(0);
    for (; i + 4 <= n; i += 4) { float32x4_t v = vld1q_f32(x + i); a = vmlaq_f32(a, v, v); }
    ss = hsum_ps(a);
#endif
    for (; i < n; i++) ss += x[i] * x[i];
    return ss;
}
static void zcnorm(float *out, const float *x, const float *scale, int n) {
    float r = 1.f / sqrtf(sumsq(x, n) / n + 1e-6f);
    int i = 0;
#ifdef HAVE_NEON
    for (; i + 4 <= n; i += 4)
        vst1q_f32(out + i, vmulq_n_f32(vmulq_f32(vaddq_f32(vdupq_n_f32(1.f), vld1q_f32(scale + i)), vld1q_f32(x + i)), r));
#endif
    for (; i < n; i++) out[i] = (1.f + scale[i]) * x[i] * r;
}
static void rms_unit(float *out, const float *x, int n) {
    float r = 1.f / sqrtf(sumsq(x, n) / n + 1e-6f);
    int i = 0;
#ifdef HAVE_NEON
    for (; i + 4 <= n; i += 4) vst1q_f32(out + i, vmulq_n_f32(vld1q_f32(x + i), r));
#endif
    for (; i < n; i++) out[i] = x[i] * r;
}
static void silu_vec(float *x, int n) {
    int i = 0;
#ifdef HAVE_NEON
    for (; i + 4 <= n; i += 4) {
        float32x4_t v = vld1q_f32(x + i);
        vst1q_f32(x + i, vmulq_f32(v, recip_ps(vaddq_f32(vdupq_n_f32(1.f), exp_ps(vnegq_f32(v))))));
    }
#endif
    for (; i < n; i++) x[i] = silu1(x[i]);
}
// x[i] *= sigmoid(g[i])
static void gate_vec(float *x, const float *g, int n) {
    int i = 0;
#ifdef HAVE_NEON
    for (; i + 4 <= n; i += 4)
        vst1q_f32(x + i, vmulq_f32(vld1q_f32(x + i), recip_ps(vaddq_f32(vdupq_n_f32(1.f), exp_ps(vnegq_f32(vld1q_f32(g + i)))))));
#endif
    for (; i < n; i++) x[i] *= sigm(g[i]);
}
static void softmax(float *x, int n) {
    float mx = x[0]; for (int i = 1; i < n; i++) if (x[i] > mx) mx = x[i];
    int i = 0; float s = 0;
#ifdef HAVE_NEON
    float32x4_t acc = vdupq_n_f32(0), m4 = vdupq_n_f32(mx);
    for (; i + 4 <= n; i += 4) { float32x4_t e = exp_ps(vsubq_f32(vld1q_f32(x + i), m4)); vst1q_f32(x + i, e); acc = vaddq_f32(acc, e); }
    s = hsum_ps(acc);
#endif
    for (; i < n; i++) { x[i] = expf(x[i] - mx); s += x[i]; }
    float inv = 1.f / s; for (i = 0; i < n; i++) x[i] *= inv;
}
static inline float dot(const float *a, const float *b, int n) {
    int i = 0; float s = 0;
#ifdef HAVE_NEON
    float32x4_t acc = vdupq_n_f32(0);
    for (; i + 4 <= n; i += 4) acc = vmlaq_f32(acc, vld1q_f32(a + i), vld1q_f32(b + i));
    s = hsum_ps(acc);
#endif
    for (; i < n; i++) s += a[i] * b[i];
    return s;
}
// y[0..n) += a * x
static inline void axpy(float *y, float a, const float *x, int n) {
    int i = 0;
#ifdef HAVE_NEON
    for (; i + 4 <= n; i += 4) vst1q_f32(y + i, vmlaq_n_f32(vld1q_f32(y + i), vld1q_f32(x + i), a));
#endif
    for (; i < n; i++) y[i] += a * x[i];
}

// RoPE with a cached cos/sin table (positions < ROPE_MAX; head dim <= 64).
#define ROPE_MAX 1024
static float rope_c[ROPE_MAX][32], rope_s[ROPE_MAX][32]; static int rope_n, rope_hd;
static void rope_apply(float *x, int hd, int pos, float theta) {
    int h = hd / 2;
    if (rope_hd != hd) { rope_hd = hd; rope_n = 0; }
    while (rope_n <= pos && rope_n < ROPE_MAX) {
        for (int i = 0; i < h; i++) { double f = rope_n * pow(theta, -(double)(2 * i) / hd); rope_c[rope_n][i] = (float)cos(f); rope_s[rope_n][i] = (float)sin(f); }
        rope_n++;
    }
    const float *c = rope_c[pos], *s = rope_s[pos];
    for (int i = 0; i < h; i++) { float a = x[i], b = x[i + h]; x[i] = a * c[i] - b * s[i]; x[i + h] = b * c[i] + a * s[i]; }
}

// Kronecker-factored 512x512 transform: Z (16x32) -> A^T Z B. Rows accumulate in registers (a NEON
// store followed by a load of the same address stalls the Cortex-A8 pipeline).
static void kron(float *out, const float *z, const float *a, const float *b) {
    float y[16 * 32] __attribute__((aligned(16)));
#ifdef HAVE_NEON
    for (int i = 0; i < 16; i++) {
        float32x4_t c0 = vdupq_n_f32(0), c1 = c0, c2 = c0, c3 = c0, c4 = c0, c5 = c0, c6 = c0, c7 = c0;
        for (int j = 0; j < 32; j++) {
            const float *br = b + j * 32; float zj = z[i * 32 + j];
            c0 = vmlaq_n_f32(c0, vld1q_f32(br), zj); c1 = vmlaq_n_f32(c1, vld1q_f32(br + 4), zj);
            c2 = vmlaq_n_f32(c2, vld1q_f32(br + 8), zj); c3 = vmlaq_n_f32(c3, vld1q_f32(br + 12), zj);
            c4 = vmlaq_n_f32(c4, vld1q_f32(br + 16), zj); c5 = vmlaq_n_f32(c5, vld1q_f32(br + 20), zj);
            c6 = vmlaq_n_f32(c6, vld1q_f32(br + 24), zj); c7 = vmlaq_n_f32(c7, vld1q_f32(br + 28), zj);
        }
        float *yr = y + i * 32;
        vst1q_f32(yr, c0); vst1q_f32(yr + 4, c1); vst1q_f32(yr + 8, c2); vst1q_f32(yr + 12, c3);
        vst1q_f32(yr + 16, c4); vst1q_f32(yr + 20, c5); vst1q_f32(yr + 24, c6); vst1q_f32(yr + 28, c7);
    }
    for (int k = 0; k < 16; k++) {
        float32x4_t c0 = vdupq_n_f32(0), c1 = c0, c2 = c0, c3 = c0, c4 = c0, c5 = c0, c6 = c0, c7 = c0;
        for (int i = 0; i < 16; i++) {
            const float *yr = y + i * 32; float ak = a[i * 16 + k];
            c0 = vmlaq_n_f32(c0, vld1q_f32(yr), ak); c1 = vmlaq_n_f32(c1, vld1q_f32(yr + 4), ak);
            c2 = vmlaq_n_f32(c2, vld1q_f32(yr + 8), ak); c3 = vmlaq_n_f32(c3, vld1q_f32(yr + 12), ak);
            c4 = vmlaq_n_f32(c4, vld1q_f32(yr + 16), ak); c5 = vmlaq_n_f32(c5, vld1q_f32(yr + 20), ak);
            c6 = vmlaq_n_f32(c6, vld1q_f32(yr + 24), ak); c7 = vmlaq_n_f32(c7, vld1q_f32(yr + 28), ak);
        }
        float *o = out + k * 32;
        vst1q_f32(o, c0); vst1q_f32(o + 4, c1); vst1q_f32(o + 8, c2); vst1q_f32(o + 12, c3);
        vst1q_f32(o + 16, c4); vst1q_f32(o + 20, c5); vst1q_f32(o + 24, c6); vst1q_f32(o + 28, c7);
    }
#else
    memset(y, 0, sizeof y);
    for (int i = 0; i < 16; i++) for (int j = 0; j < 32; j++) axpy(y + i * 32, z[i * 32 + j], b + j * 32, 32);
    memset(out, 0, 512 * 4);
    for (int k = 0; k < 16; k++) for (int i = 0; i < 16; i++) axpy(out + k * 32, a[i * 16 + k], y + i * 32, 32);
#endif
}

// out = HadamardMLP(x), 512 wide.
static void hada_mlp_(float *out, const float *x, const Hada *p, const int *p1, const int *p2) {
    float c8[8], cond[512], z[512], t[512];
#ifdef HAVE_NEON
    float32x4_t ca = vdupq_n_f32(0), cb = vdupq_n_f32(0);
    for (int i = 0; i < 512; i++) { ca = vmlaq_n_f32(ca, vld1q_f32(p->cond_v + i * 8), x[i]); cb = vmlaq_n_f32(cb, vld1q_f32(p->cond_v + i * 8 + 4), x[i]); }
    vst1q_f32(c8, ca); vst1q_f32(c8 + 4, cb);
#else
    for (int r = 0; r < 8; r++) c8[r] = 0;
    for (int i = 0; i < 512; i++) for (int r = 0; r < 8; r++) c8[r] += x[i] * p->cond_v[i * 8 + r];
#endif
    softmax(c8, 8);
    for (int n = 0; n < 512; n++) cond[n] = 1;
    for (int r = 0; r < 8; r++) axpy(cond, c8[r], p->cond_u + r * 512, 512);
    for (int i = 0; i < 512; i++) t[i] = p->d1[i] * x[i];
    kron(z, t, p->w1a, p->w1b);
    for (int i = 0; i < 512; i++) t[i] = p->d2[i] * cond[i] * z[p1[i]] + p->b2[i];
    silu_vec(t, 512);
    kron(z, t, p->w2a, p->w2b);
    for (int i = 0; i < 512; i++) t[i] = p->d3[i] * z[p2[i]];
    kron(z, t, p->w3a, p->w3b);
    for (int i = 0; i < 512; i++) out[i] = p->d4[i] * z[i];
}

static void hada_mlp(float *out, const float *x, const Hada *p, const int *p1, const int *p2) { double t = now_ms(); hada_mlp_(out, x, p, p1, p2); prof[2] += now_ms() - t; }

// 4x4 Sinkhorn on logits (in place -> doubly stochastic matrix). Linear space: scaling a row before the
// first row normalization changes nothing, so each row is shifted by its max; the clamp keeps every
// entry a normal float so no column can sum to zero.
static void sinkhorn4(float *a) {
    for (int r = 0; r < 4; r++) {
        float mx = a[r * 4]; for (int c = 1; c < 4; c++) if (a[r * 4 + c] > mx) mx = a[r * 4 + c];
        for (int c = 0; c < 4; c++) { float d = a[r * 4 + c] - mx; a[r * 4 + c] = expf(d < -60.f ? -60.f : d); }
    }
    for (int it = 0; it < 20; it++) {
        for (int r = 0; r < 4; r++) { float inv = 1.f / (a[r * 4] + a[r * 4 + 1] + a[r * 4 + 2] + a[r * 4 + 3]); for (int c = 0; c < 4; c++) a[r * 4 + c] *= inv; }
        for (int c = 0; c < 4; c++) { float inv = 1.f / (a[c] + a[4 + c] + a[8 + c] + a[12 + c]); for (int r = 0; r < 4; r++) a[r * 4 + c] *= inv; }
    }
}

/* Scratch for prepped activations. */
static int8_t *xq; static float *xs; static size_t xcap;
static void need_x(size_t n) {
    if (n <= xcap) return;
    free(xq); free(xs); xcap = n;
    xq = malloc(n); xs = malloc(n / G * sizeof(float) + 16);
}
// out[t][0..W.out) = W . x[t]   (x: T rows of W.in floats)
static void mm(float *out, const CQ *W, const float *x, int T) {
    need_x((size_t)T * W->in);
    for (int t = 0; t < T; t++) cq_prep(x + (size_t)t * W->in, W->in, W->bits, G, xq + (size_t)t * W->in, xs + (size_t)t * (W->in / G));
    cq_mm(out, W->out, W, 0, W->out, xq, xs, T);
}

// Prep T rows once for several matrices that share an input (all must have the same width and bits).
static void prep_rows(const float *x, int T, int in, int bits) {
    need_x((size_t)T * in);
    for (int t = 0; t < T; t++) cq_prep(x + (size_t)t * in, in, bits, G, xq + (size_t)t * in, xs + (size_t)t * (in / G));
}
static void mm_prepped(float *out, const CQ *W, int T) { cq_mm(out, W->out, W, 0, W->out, xq, xs, T); }

/* ---------- mHC lane stack (shared by encoder and decoder) ---------- */
// One layer's lane mixing around a block. X: [T][NL][D]. block(ctx, l, u (T x D) in/out).
typedef void (*block_fn)(whistle_t *m, void *ctx, int l, float *u, int T);
static void lane_layer(whistle_t *m, const MHC *h, int l, float *X, int T, block_fn blk, void *ctx) {
    int D = m->D, nC = NL * D;
    float *nx = malloc((size_t)T * nC * 4), *u = malloc((size_t)T * D * 4), *y = malloc((size_t)T * D * 4);
    float *pp = malloc((size_t)T * 24 * 4);
    for (int t = 0; t < T; t++) rms_unit(nx + (size_t)t * nC, X + (size_t)t * nC, nC);
    need_x((size_t)T * nC);
    for (int t = 0; t < T; t++) cq_prep(nx + (size_t)t * nC, nC, h->phi_pre.bits, G, xq + (size_t)t * nC, xs + (size_t)t * (nC / G));
    cq_mm(pp, 24, &h->phi_pre, l * NL, l * NL + NL, xq, xs, T);
    cq_mm(pp + 4, 24, &h->phi_post, l * NL, l * NL + NL, xq, xs, T);
    cq_mm(pp + 8, 24, &h->phi_res, l * 16, l * 16 + 16, xq, xs, T);
    int lane = l % NL;
    for (int t = 0; t < T; t++) {
        const float *x = X + (size_t)t * nC; float *ut = u + (size_t)t * D;
        float hp[NL];
        for (int n = 0; n < NL; n++) hp[n] = sigm(h->a_pre[l] * pp[t * 24 + n] + h->b_pre[l * NL + n] + (n == lane ? 4.f : -4.f));
        memset(ut, 0, D * 4);
        for (int n = 0; n < NL; n++) axpy(ut, hp[n], x + n * D, D);
    }
    memcpy(y, u, (size_t)T * D * 4);
    blk(m, ctx, l, y, T);
    for (int t = 0; t < T; t++) {
        float *x = X + (size_t)t * nC; const float *ut = u + (size_t)t * D, *yt = y + (size_t)t * D;
        float hq[NL], hr[16], nxv[NL * 512], dl[512];
        for (int n = 0; n < NL; n++) hq[n] = 2.f * sigm(h->a_post[l] * pp[t * 24 + 4 + n] + h->b_post[l * NL + n] + (n == lane ? 0.f : -4.f));
        for (int k = 0; k < 16; k++) hr[k] = h->a_res[l] * pp[t * 24 + 8 + k] + h->b_res[l * 16 + k];
        sinkhorn4(hr);
        for (int c = 0; c < D; c++) dl[c] = yt[c] - ut[c];
        for (int i = 0; i < NL; i++) {
            float *o = nxv + i * D;
            memset(o, 0, D * 4);
            for (int j = 0; j < NL; j++) axpy(o, hr[i * 4 + j], x + j * D, D);
            axpy(o, hq[i], dl, D);
        }
        memcpy(x, nxv, (size_t)nC * 4);
    }
    free(nx); free(u); free(y); free(pp);
}
static void lanes_out(whistle_t *m, const float *X, int T, const float *norm, float *out) {
    int D = m->D;
    for (int t = 0; t < T; t++) {
        float a[512];
        for (int c = 0; c < D; c++) a[c] = 0.25f * (X[(t * NL) * D + c] + X[(t * NL + 1) * D + c] + X[(t * NL + 2) * D + c] + X[(t * NL + 3) * D + c]);
        zcnorm(out + (size_t)t * D, a, norm, D);
    }
}

/* ---------- audio front end ---------- */
static void fft512(float *re, float *im) {
    static float cw[256], sw[256]; static int init;
    if (!init) { for (int i = 0; i < 256; i++) { cw[i] = cosf(2 * (float)M_PI * i / 512); sw[i] = -sinf(2 * (float)M_PI * i / 512); } init = 1; }
    for (int i = 1, j = 0; i < 512; i++) {
        int bit = 256; for (; j & bit; bit >>= 1) j ^= bit; j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= 512; len <<= 1) {
        int step = 512 / len;
        for (int i = 0; i < 512; i += len)
            for (int k = 0; k < len / 2; k++) {
                float wr = cw[k * step], wi = sw[k * step];
                int a = i + k, b = a + len / 2;
                float tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr; im[b] = im[a] - ti; re[a] += tr; im[a] += ti;
            }
    }
}

// Normalized log-mel features: returns [n][mels], n = samples / hop.
static float *features(whistle_t *m, const float *wav, int ns, int *nout) {
    int win = m->win, hop = m->hop, M = m->mels, n = ns / hop;
    if (n < 1) n = 1;
    float *f = calloc((size_t)n * M, 4), *w = malloc(win * 4);
    for (int i = 0; i < win; i++) w[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / (win - 1));
    float re[512], im[512], pw[257];
    for (int t = 0; t < n; t++) {
        for (int i = 0; i < 512; i++) { re[i] = 0; im[i] = 0; }
        for (int i = 0; i < win; i++) { int s = t * hop + i - win / 2; re[i] = (s >= 0 && s < ns) ? wav[s] * w[i] : 0; }
        fft512(re, im);
        for (int k = 0; k < 257; k++) pw[k] = re[k] * re[k] + im[k] * im[k];
        for (int j = 0; j < M; j++) { float a = 0; for (int k = m->mlo[j]; k < m->mhi[j]; k++) a += pw[k] * m->mel[k * M + j]; f[t * M + j] = a; }
        for (int j = 0; j < M; j++) f[t * M + j] = logf(f[t * M + j] + 5.9604645e-8f);
    }
    for (int j = 0; j < M; j++) {
        double s = 0, s2 = 0;
        for (int t = 0; t < n; t++) s += f[t * M + j];
        float mean = (float)(s / n);
        for (int t = 0; t < n; t++) { float d = f[t * M + j] - mean; s2 += d * d; }
        float sd = sqrtf((float)(s2 / (n > 1 ? n - 1 : 1)));
        for (int t = 0; t < n; t++) f[t * M + j] = (f[t * M + j] - mean) / (sd + 1e-5f);
    }
    free(w);
    *nout = n;
    return f;
}

// 3x3 stride-2 pad-1 depthwise conv. in [H][W][C] (C==1: broadcast to w's channels) -> [ceil(H/2)][ceil(W/2)][C]
static float *dwconv_s2(const float *in, int Hh, int Ww, int Cin, const float *w, int C, int *ho, int *wo) {
    int Ho = (Hh + 1) / 2, Wo = (Ww + 1) / 2;
    float *out = calloc((size_t)Ho * Wo * C, 4);
    for (int y = 0; y < Ho; y++)
        for (int x = 0; x < Wo; x++) {
            float *o = out + ((size_t)y * Wo + x) * C;
            for (int c0 = 0; c0 < C; c0 += 16) {
#ifdef HAVE_NEON
                float32x4_t a0 = vdupq_n_f32(0), a1 = a0, a2 = a0, a3 = a0;
#else
                float acc[16] = { 0 };
#endif
                for (int dy = 0; dy < 3; dy++) {
                    int iy = 2 * y + dy - 1; if (iy < 0 || iy >= Hh) continue;
                    for (int dx = 0; dx < 3; dx++) {
                        int ix = 2 * x + dx - 1; if (ix < 0 || ix >= Ww) continue;
                        const float *wk = w + (dy * 3 + dx) * C + c0, *ip = in + ((size_t)iy * Ww + ix) * Cin;
#ifdef HAVE_NEON
                        if (Cin == 1) {
                            float v = ip[0];
                            a0 = vmlaq_n_f32(a0, vld1q_f32(wk), v); a1 = vmlaq_n_f32(a1, vld1q_f32(wk + 4), v);
                            a2 = vmlaq_n_f32(a2, vld1q_f32(wk + 8), v); a3 = vmlaq_n_f32(a3, vld1q_f32(wk + 12), v);
                        } else {
                            ip += c0;
                            a0 = vmlaq_f32(a0, vld1q_f32(wk), vld1q_f32(ip)); a1 = vmlaq_f32(a1, vld1q_f32(wk + 4), vld1q_f32(ip + 4));
                            a2 = vmlaq_f32(a2, vld1q_f32(wk + 8), vld1q_f32(ip + 8)); a3 = vmlaq_f32(a3, vld1q_f32(wk + 12), vld1q_f32(ip + 12));
                        }
#else
                        for (int c = 0; c < 16; c++) acc[c] += wk[c] * ip[Cin == 1 ? 0 : c0 + c];
#endif
                    }
                }
#ifdef HAVE_NEON
                vst1q_f32(o + c0, a0); vst1q_f32(o + c0 + 4, a1); vst1q_f32(o + c0 + 8, a2); vst1q_f32(o + c0 + 12, a3);
#else
                memcpy(o + c0, acc, sizeof acc);
#endif
            }
        }
    *ho = Ho; *wo = Wo;
    return out;
}

/* ---------- encoder ---------- */
typedef struct { int T, len; } EncCtx;

static void enc_block(whistle_t *m, void *vctx, int l, float *x, int T) {
    EncCtx *ctx = vctx; const Enc *e = &m->enc[l];
    int D = m->D, H = m->H, KV = m->KV, QK = m->QK, VD = m->VD, rep = H / KV;
    float *h = malloc((size_t)T * D * 4), *o = malloc((size_t)T * D * 4), *t2 = malloc((size_t)T * 2 * D * 4);
    // x += 0.5 * mlp0(norm(x))
    for (int t = 0; t < T; t++) { float tmp[512], r[512]; zcnorm(tmp, x + t * D, e->pre_hada0, D); hada_mlp(r, tmp, &e->mlp0, m->p1, m->p2); axpy(x + t * D, 0.5f, r, D); }
    // attention
    for (int t = 0; t < T; t++) zcnorm(h + t * D, x + t * D, e->norm_in, D);
    float *q = malloc((size_t)T * H * QK * 4), *k = malloc((size_t)T * KV * QK * 4), *v = malloc((size_t)T * KV * VD * 4), *g = malloc((size_t)T * D * 4);
    prep_rows(h, T, D, e->q.bits);
    mm_prepped(q, &e->q, T); mm_prepped(k, &e->k, T); mm_prepped(v, &e->v, T); mm_prepped(g, &e->gate, T);
    for (int t = 0; t < T; t++) {
        for (int hh = 0; hh < H; hh++) { float *p = q + (t * H + hh) * QK; zcnorm(p, p, e->qn, QK); rope_apply(p, QK, t, m->rope); }
        for (int hh = 0; hh < KV; hh++) { float *p = k + (t * KV + hh) * QK; zcnorm(p, p, e->kn, QK); rope_apply(p, QK, t, m->rope); }
    }
    float *sc = malloc((size_t)T * 4), isq = 1.f / sqrtf((float)QK);
    for (int t = 0; t < T; t++)
        for (int hh = 0; hh < H; hh++) {
            const float *qp = q + (t * H + hh) * QK; int kh = hh / rep;
            for (int s = 0; s < ctx->len; s++) sc[s] = dot(qp, k + (s * KV + kh) * QK, QK) * isq;
            softmax(sc, ctx->len);
            float *op = o + t * D + hh * VD;
            memset(op, 0, VD * 4);
            for (int s = 0; s < ctx->len; s++) axpy(op, sc[s], v + (s * KV + kh) * VD, VD);
        }
    gate_vec(o, g, T * D);
    mm(h, &e->out, o, T);
    float ag = sigm(e->attn_gate);
    for (int t = 0; t < T; t++) { zcnorm(h + t * D, h + t * D, e->post_norm, D); axpy(x + t * D, ag, h + t * D, D); }
    // conv module
    for (int t = 0; t < T; t++) zcnorm(h + t * D, x + t * D, e->conv_norm, D);
    mm(t2, &e->pw1, h, T);
    for (int t = 0; t < T; t++) {
        if (t < ctx->len) { memcpy(h + t * D, t2 + t * 2 * D, D * 4); gate_vec(h + t * D, t2 + t * 2 * D + D, D); }
        else memset(h + t * D, 0, D * 4);
    }
    int K = m->conv_k, half = K / 2;
    memset(o, 0, (size_t)T * D * 4);
    for (int t = 0; t < T; t++)
        for (int j = 0; j < K; j++) {
            int tt = t + j - half; if (tt < 0 || tt >= T) continue;
            float *op = o + t * D; const float *hp = h + tt * D, *wp = e->dw + j * D;
            int i = 0;
#ifdef HAVE_NEON
            for (; i + 4 <= D; i += 4) vst1q_f32(op + i, vmlaq_f32(vld1q_f32(op + i), vld1q_f32(wp + i), vld1q_f32(hp + i)));
#endif
            for (; i < D; i++) op[i] += wp[i] * hp[i];
        }
    for (int t = 0; t < T; t++) zcnorm(o + t * D, o + t * D, e->conv_out_norm, D);
    silu_vec(o, T * D);
    mm(h, &e->pw2, o, T);
    axpy(x, 1.f, h, T * D);
    for (int t = 0; t < T; t++) { float tmp[512], r[512]; zcnorm(tmp, x + t * D, e->pre_hada, D); hada_mlp(r, tmp, &e->mlp, m->p1, m->p2); axpy(x + t * D, 0.5f, r, D); }
    free(h); free(o); free(t2); free(q); free(k); free(v); free(g); free(sc);
}

// Returns encoder memory [T][D] (with the positional term added); *T, *len valid frames.
static float *encode(whistle_t *m, const float *wav, int ns, int *Tout, int *lenout) {
    double t0 = now_ms();
    int n, D = m->D, C = m->stem_c;
    float *f = features(m, wav, ns, &n);
    m->t_feat = now_ms() - t0;
    dbg("feat", f, (size_t)n * m->mels);
    int Hh, Ww;
    double ts = now_ms();
    float *a = dwconv_s2(f, n, m->mels, 1, m->stem_w, C, &Hh, &Ww);
    free(f);
    for (int i = 1; i < m->stages; i++) {
        silu_vec(a, Hh * Ww * C);
        int H2, W2;
        float *b = dwconv_s2(a, Hh, Ww, C, m->stem_dw[i], C, &H2, &W2);
        free(a);
        a = malloc((size_t)H2 * W2 * C * 4);
        mm(a, &m->stem_pw[i], b, H2 * W2);
        free(b); Hh = H2; Ww = W2;
    }
    silu_vec(a, Hh * Ww * C);
    int len = Hh, T = len + (len & 1);
    float *flat = calloc((size_t)T * C * Ww, 4);
    for (int t = 0; t < len; t++) for (int x = 0; x < Ww; x++) for (int c = 0; c < C; c++) flat[(size_t)t * C * Ww + c * Ww + x] = a[((size_t)t * Ww + x) * C + c];
    free(a);
    float *x0 = malloc((size_t)T * D * 4);
    mm(x0, &m->stem_out, flat, T);
    free(flat);
    prof[3] += now_ms() - ts;
    dbg("stem", x0, (size_t)T * D);
    float *X = malloc((size_t)T * NL * D * 4);
    for (int t = 0; t < T; t++) for (int n2 = 0; n2 < NL; n2++) memcpy(X + ((size_t)t * NL + n2) * D, x0 + (size_t)t * D, D * 4);
    EncCtx ctx = { T, len };
    for (int l = 0; l < m->EL; l++) lane_layer(m, &m->emhc, l, X, T, enc_block, &ctx);
    lanes_out(m, X, T, m->enc_final_norm, x0);
    free(X);
    float pg = sigm(m->pe_gate);
    for (int t = 0; t < T; t++)
        for (int i = 0; i < D / 2; i++) {
            float ang = (float)t * expf(-logf(10000.f) / (D / 2 - 1) * i);
            x0[t * D + i] += pg * sinf(ang); x0[t * D + D / 2 + i] += pg * cosf(ang);
        }
    dbg("enc", x0, (size_t)T * D);
    *Tout = T; *lenout = len;
    m->t_enc = now_ms() - t0;
    return x0;
}

/* ---------- decoder ---------- */
typedef struct {
    int pos, S, len;
    float *ck[16], *cv[16];                          // cross K [S][H*QK] (normed), V [S][H*VD]
    float *kc[16], *vc[16];                          // self K [MAXTOK][KV*QK] (tapped, normed, roped), V
    float *raw[16];                                  // last taps raw q|k|v projections, ring [taps][H*QK+KV*QK+KV*VD]
    float *ek[16], *ev[16];                          // engram key / tapped value for this token, per site
    float *evraw[16];                                // engram value history [MAXTOK][D]
    int toks[MAXTOK + 2];
} DecCtx;

static void dec_block(whistle_t *m, void *vctx, int l, float *x, int T) {
    (void)T;
    DecCtx *c = vctx; const Dec *d = &m->dec[l];
    int D = m->D, H = m->H, KV = m->KV, QK = m->QK, VD = m->VD, rep = H / KV, pos = c->pos;
    for (int s = 0; s < m->nsites; s++)
        if (m->sites[s] == l) {
            float a[512], b[512];
            rms_unit(a, x, D); rms_unit(b, c->ek[s], D);
            axpy(x, sigm(dot(a, b, D) / sqrtf((float)D)), c->ev[s], D);
        }
    float h[512], o[512], g[512], hn[512];
    int nq = H * QK, nk = KV * QK, nv = KV * VD, rw = nq + nk + nv;
    zcnorm(hn, x, d->norm_in, D);
    need_x(D); cq_prep(hn, D, d->q.bits, G, xq, xs);
    float *cur = c->raw[l] + (pos % m->taps) * rw;
    cq_mm(cur, 0, &d->q, 0, nq, xq, xs, 1);
    cq_mm(cur + nq, 0, &d->k, 0, nk, xq, xs, 1);
    cq_mm(cur + nq + nk, 0, &d->v, 0, nv, xq, xs, 1);
    cq_mm(g, 0, &d->gate, 0, D, xq, xs, 1);
    float q[512], kk[256], vv[256];
    for (int i = 0; i < rw; i++) {
        float s = 0;
        const float *tp = i < nq ? d->qt + i : i < nq + nk ? d->kt + (i - nq) : d->vt + (i - nq - nk);
        int stride = i < nq ? nq : i < nq + nk ? nk : nv;
        for (int j = 0; j < m->taps && j <= pos; j++) s += tp[j * stride] * c->raw[l][((pos - j) % m->taps) * rw + i];
        if (i < nq) q[i] = s; else if (i < nq + nk) kk[i - nq] = s; else vv[i - nq - nk] = s;
    }
    for (int hh = 0; hh < H; hh++) { zcnorm(q + hh * QK, q + hh * QK, d->qn, QK); rope_apply(q + hh * QK, QK, pos, m->rope); }
    for (int hh = 0; hh < KV; hh++) { zcnorm(kk + hh * QK, kk + hh * QK, d->kn, QK); rope_apply(kk + hh * QK, QK, pos, m->rope); }
    memcpy(c->kc[l] + pos * nk, kk, nk * 4); memcpy(c->vc[l] + pos * nv, vv, nv * 4);
    float sc[MAXTOK + 2], isq = 1.f / sqrtf((float)QK);
    for (int hh = 0; hh < H; hh++) {
        int kh = hh / rep;
        for (int s = 0; s <= pos; s++) sc[s] = dot(q + hh * QK, c->kc[l] + s * nk + kh * QK, QK) * isq;
        softmax(sc, pos + 1);
        memset(o + hh * VD, 0, VD * 4);
        for (int s = 0; s <= pos; s++) axpy(o + hh * VD, sc[s], c->vc[l] + s * nv + kh * VD, VD);
    }
    gate_vec(o, g, H * VD);
    mm(h, &d->out, o, 1);
    zcnorm(h, h, d->post_norm, D);
    axpy(x, sigm(d->attn_gate), h, D);
    // cross attention
    zcnorm(hn, x, d->cross_norm, D);
    need_x(D); cq_prep(hn, D, d->cq.bits, G, xq, xs);
    cq_mm(q, 0, &d->cq, 0, H * QK, xq, xs, 1);
    cq_mm(g, 0, &d->cgate, 0, D, xq, xs, 1);
    float *scs = malloc(c->S * 4);
    for (int hh = 0; hh < H; hh++) {
        zcnorm(q + hh * QK, q + hh * QK, d->cqn, QK);
        for (int s = 0; s < c->len; s++) scs[s] = dot(q + hh * QK, c->ck[l] + s * H * QK + hh * QK, QK) * isq;
        softmax(scs, c->len);
        memset(o + hh * VD, 0, VD * 4);
        for (int s = 0; s < c->len; s++) axpy(o + hh * VD, scs[s], c->cv[l] + s * H * VD + hh * VD, VD);
    }
    free(scs);
    gate_vec(o, g, H * VD);
    mm(h, &d->cout, o, 1);
    zcnorm(h, h, d->post_cross_norm, D);
    axpy(x, sigm(d->cross_gate), h, D);
    zcnorm(hn, x, d->pre_hada, D);
    hada_mlp(h, hn, &d->mlp, m->p1, m->p2);
    axpy(x, 1.f, h, D);
}

static void engram_step(whistle_t *m, DecCtx *c) {
    int D = m->D, pos = c->pos, heads = m->ntab / m->norders;
    float e[2048];
    for (int s = 0; s < m->nsites; s++) {
        const Engram *g = &m->eng[s];
        for (int oi = 0; oi < m->norders; oi++)
            for (int hh = 0; hh < heads; hh++) {
                int tb = oi * heads + hh, order = m->orders[oi];
                float *row = e + tb * m->sub;
                if (pos < order - 1) { memset(row, 0, m->sub * 4); continue; }
                uint32_t acc = 0x9E3779B9u * (uint32_t)(oi * heads + hh + 1);
                for (int j = 0; j < order; j++) acc = (acc ^ (uint32_t)(pos - j >= 0 ? c->toks[pos - j] : 0)) * 0x01000193u;
                acc ^= acc >> 15;
                cq_row(&g->tables, tb * m->slots + (int)(acc % (uint32_t)m->slots), row);
            }
        mm(c->ek[s], &g->key, e, 1);
        mm(c->evraw[s] + pos * D, &g->value, e, 1);
        for (int i = 0; i < D; i++) {
            float v = 0;
            for (int j = 0; j < m->etaps; j++) { int p = pos - j * m->edil; if (p >= 0) v += g->taps[j * D + i] * c->evraw[s][p * D + i]; }
            c->ev[s][i] = v;
        }
    }
}

// Run one token through the decoder; returns argmax over the text vocabulary when want_logits.
static int dec_step(whistle_t *m, DecCtx *c, int tok, int want) {
    int D = m->D;
    c->toks[c->pos] = tok;
    float x[512], X[NL * 512];
    cq_row(&m->emb, tok, x);
    float sd = sqrtf((float)D);
    for (int i = 0; i < D; i++) x[i] *= sd;
    engram_step(m, c);
    for (int n = 0; n < NL; n++) memcpy(X + n * D, x, D * 4);
    for (int l = 0; l < m->L; l++) lane_layer(m, &m->mhc, l, X, 1, dec_block, c);
    int best = -1;
    if (want) {
        lanes_out(m, X, 1, m->final_norm, x);
        double tl = now_ms();
        need_x(D); cq_prep(x, D, m->emb.bits, G, xq, xs);
        float bv = -1e30f;
        for (int o = 0; o < m->text; o++) { float v = cq_dot(&m->emb, o, xq, xs); if (v > bv) { bv = v; best = o; } }
        prof[4] += now_ms() - tl;
    }
    c->pos++;
    return best;
}

int whistle_transcribe(whistle_t *m, const float *audio, int n, char *out, int outlen) {
    int T, len, D = m->D, H = m->H, QK = m->QK, VD = m->VD, KV = m->KV;
    float *mem = encode(m, audio, n, &T, &len);
    double t0 = now_ms();
    double p0 = cq_ms_mm, p2 = prof[2];
    DecCtx c; memset(&c, 0, sizeof c);
    c.S = T; c.len = len;
    int rw = H * QK + KV * QK + KV * VD;
    for (int l = 0; l < m->L; l++) {
        const Dec *d = &m->dec[l];
        c.ck[l] = malloc((size_t)T * H * QK * 4); c.cv[l] = malloc((size_t)T * H * VD * 4);
        mm(c.ck[l], &d->ck, mem, T); mm(c.cv[l], &d->cv, mem, T);
        for (int s = 0; s < T * H; s++) zcnorm(c.ck[l] + s * QK, c.ck[l] + s * QK, d->ckn, QK);
        c.kc[l] = malloc((size_t)(MAXTOK + 2) * KV * QK * 4); c.vc[l] = malloc((size_t)(MAXTOK + 2) * KV * VD * 4);
        c.raw[l] = calloc((size_t)(m->taps ? m->taps : 1) * rw, 4);
    }
    for (int s = 0; s < m->nsites; s++) { c.ek[s] = malloc(D * 4); c.ev[s] = malloc(D * 4); c.evraw[s] = malloc((size_t)(MAXTOK + 2) * D * 4); }
    free(mem);
    // [bos, <|en|>] then greedy
    int maxtok = 8 + len / 2;   // ~12 tokens per second of audio is plenty
    if (maxtok > MAXTOK) maxtok = MAXTOK;
    dec_step(m, &c, 2, 0);
    int tok = dec_step(m, &c, m->text + 0, 1), ntok = 0, o = 0;
    while (tok != 1 && ntok < maxtok) {
        if (m->ptype[tok] == 4) { if (o < outlen - 1) out[o++] = (char)strtol(m->piece[tok] + 3, NULL, 16); }
        else if (m->ptype[tok] != 1 && m->ptype[tok] != 2)
            for (int i = 0; i < m->plen[tok]; i++) {
                const unsigned char *p = (const unsigned char *)m->piece[tok] + i;
                if (p[0] == 0xe2 && p[1] == 0x96 && p[2] == 0x81) { if (o > 0 && o < outlen - 1) out[o++] = ' '; i += 2; }
                else if (o < outlen - 1) out[o++] = (char)p[0];
            }
        ntok++;
        tok = dec_step(m, &c, tok, 1);
    }
    out[o] = 0;
    for (int l = 0; l < m->L; l++) { free(c.ck[l]); free(c.cv[l]); free(c.kc[l]); free(c.vc[l]); free(c.raw[l]); }
    for (int s = 0; s < m->nsites; s++) { free(c.ek[s]); free(c.ev[s]); free(c.evraw[s]); }
    m->t_dec = now_ms() - t0; m->ntok = ntok;
    prof[5] = cq_ms_mm - p0; prof[6] = prof[2] - p2;
    return o;
}

#ifdef WHISTLE_MAIN
static float *read_wav16(const char *path, int *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(sz); fread(b, 1, sz, f); fclose(f);
    long p = 12; const uint8_t *data = NULL; uint32_t dlen = 0;
    while (p + 8 <= sz) { uint32_t cl; memcpy(&cl, b + p + 4, 4); if (!memcmp(b + p, "data", 4)) { data = b + p + 8; dlen = cl; break; } p += 8 + cl + (cl & 1); }
    if (!data) { fprintf(stderr, "%s: no data chunk\n", path); return NULL; }
    *n = dlen / 2;
    float *a = malloc(*n * 4);
    for (int i = 0; i < *n; i++) { int16_t s; memcpy(&s, data + 2 * i, 2); a[i] = s / 32768.f; }
    free(b);
    return a;
}
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s whistle.cact in.wav...\n", argv[0]); return 1; }
    double t0 = now_ms();
    whistle_t *m = whistle_load(argv[1]);
    if (!m) return 1;
    fprintf(stderr, "load %.0f ms\n", now_ms() - t0);
    if (getenv("WHISTLE_KBENCH")) {   // kernel micro-benchmark: decoder layer 0 out_proj (512x512, 2-bit) and logits (4-bit)
        float x[64 * 512], y[64 * 512];
        for (int i = 0; i < 64 * 512; i++) x[i] = sinf(i * 0.37f);
        const CQ *W = &m->dec[0].out, *E = &m->emb;
        for (int T = 1; T <= 64; T *= 4) {
            prep_rows(x, T, 512, W->bits);
            double t1 = now_ms(); int reps = 64 / T;
            for (int r = 0; r < reps; r++) mm_prepped(y, W, T);
            double ms = now_ms() - t1;
            fprintf(stderr, "2-bit 512x512 T=%d: %.3f ms per row-vector, %.0f MMAC/s\n", T, ms / 64, 64.0 * 512 * 512 / ms / 1e3);
        }
        prep_rows(x, 1, 512, E->bits);
        double t1 = now_ms();
        for (int r = 0; r < 4; r++) for (int o = 0; o < 8192; o++) y[o & 1023] = cq_dot(E, o, xq, xs);
        double ms = (now_ms() - t1) / 4;
        fprintf(stderr, "4-bit 8192x512 T=1: %.2f ms, %.0f MMAC/s\n", ms, 8192.0 * 512 / ms / 1e3);
        return 0;
    }
    for (int i = 2; i < argc; i++) {
        int n; float *a = read_wav16(argv[i], &n);
        if (!a) continue;
        char text[1024];
        t0 = now_ms();
        whistle_transcribe(m, a, n, text, sizeof text);
        double ms = now_ms() - t0, fe, en, de; int nt;
        whistle_stats(m, &fe, &en, &de, &nt);
        printf("%s\t%s\n", argv[i], text);
        fprintf(stderr, "  prof: cq_mm %.0f (dec %.0f) prep %.0f hada %.0f (dec %.0f) stem %.0f logits %.0f\n", cq_ms_mm, prof[5], cq_ms_prep, prof[2], prof[6], prof[3], prof[4]); memset(prof, 0, sizeof prof); cq_ms_mm = cq_ms_prep = 0;
        fprintf(stderr, "  audio %.2fs: total %.0f ms (features %.0f, encoder %.0f, decoder %.0f for %d tokens) RTF %.2f\n",
                n / 16000.0, ms, fe, en - fe, de, nt, ms / (n / 16.0));
        free(a);
    }
    return 0;
}
#endif
