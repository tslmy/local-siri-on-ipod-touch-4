// Small llama-architecture LLM runner (GQA, RMSNorm, SwiGLU, tied embeddings)
// with Q8_0 weights mmap'd from convert_llama.py output and a GPT-2 style
// byte-level BPE tokenizer. Shares its NEON kernels with moonshine.c.
//
// Build: cc -O3 -ffast-math -DLLM_MAIN llm.c -lm -o llm
// Usage: llm smollm2-135m.bin smollm2-135m.tok "question" [max_tokens]
#include "llm.h"
#include "../common/q8.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>

typedef struct { const float *an; Q8 q, k, v, o; const float *fn; Q8 g, u, d; } Layer;

struct llm {
    int dim, hidden, nl, nh, nkv, vocab, hd, kvd, maxseq;
    float theta, eps;
    Q8 embed, head; Layer *L; const float *fnorm;
    void *map; size_t map_len; int fd;
    // tokenizer
    char **tok; int *toklen; int nmerge; int *mhash; int hsize; int (*merge)[3]; int byte_id[256];
    // state
    float *kc, *vc; int pos;
    float *x, *xb, *q, *k, *v, *att, *h1, *h2, *logits, *rope_c, *rope_s;
    int8_t *xq; float *xs; int maxT;
    double t_prefill, t_gen; int n_prefill, n_gen;
    double t_mark, t_layers, t_head;
};

static double now_ms(void) { struct timeval tv; gettimeofday(&tv, 0); return tv.tv_sec * 1e3 + tv.tv_usec / 1e3; }

static int hkey(int a, int b, int hs) { return (int)(((unsigned)a * 2654435761u ^ (unsigned)b * 40503u) & (hs - 1)); }

llm_t *llm_load(const char *model, const char *tokpath, int maxseq) {
    int fd = open(model, O_RDONLY);
    if (fd < 0) { perror(model); return NULL; }
    struct stat st; fstat(fd, &st);
    void *map = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { perror("mmap"); close(fd); return NULL; }
    const int32_t *h = map;
    if (h[0] != 0x4c4c4d51 || h[1] < 1 || h[1] > 3) { fprintf(stderr, "bad llm file\n"); return NULL; }
    llm_t *m = calloc(1, sizeof *m);
    m->map = map; m->map_len = st.st_size; m->fd = fd;
    m->dim = h[2]; m->hidden = h[3]; m->nl = h[4]; m->nh = h[5]; m->nkv = h[6]; m->vocab = h[7];
    m->theta = ((const float *)h)[8]; m->eps = ((const float *)h)[9];
    m->hd = m->dim / m->nh; m->kvd = m->hd * m->nkv; m->maxseq = maxseq;
    int wb = h[1] >= 2 ? h[10] : 8, eb = h[1] >= 2 ? h[11] : 8, pk = h[1] >= 3;
    const uint8_t *p = (const uint8_t *)map + (h[1] >= 2 ? 48 : 40);
    int D = m->dim, H = m->hidden;
    m->embed = q8_take_ex(&p, m->vocab, D, eb, pk);
    m->L = calloc(m->nl, sizeof(Layer));
    for (int l = 0; l < m->nl; l++) {
        Layer *L = &m->L[l];
        L->an = (const float *)p; p += D * 4;
        L->q = q8_take_ex(&p, D, D, wb, pk); L->k = q8_take_ex(&p, m->kvd, D, wb, pk); L->v = q8_take_ex(&p, m->kvd, D, wb, pk); L->o = q8_take_ex(&p, D, D, wb, pk);
        L->fn = (const float *)p; p += D * 4;
        L->g = q8_take_ex(&p, H, D, wb, pk); L->u = q8_take_ex(&p, H, D, wb, pk); L->d = q8_take_ex(&p, D, H, wb, pk);
    }
    m->fnorm = (const float *)p; p += D * 4;
    // Output classifier = the tied embedding, optionally limited to the first N token ids.
    // BPE ids follow merge order, so low ids are the common tokens; the input embedding
    // stays complete. Saves resident memory and per-token bandwidth.
    m->head = m->embed;
    { const char *e = getenv("LLM_VOCAB_LIMIT"); int lim = e ? atoi(e) : 0;
      if (lim >= 1024 && lim < m->vocab) m->head.out = lim & ~3; }
    if ((size_t)(p - (const uint8_t *)map) != (size_t)st.st_size) { fprintf(stderr, "llm size mismatch\n"); return NULL; }

    FILE *tf = fopen(tokpath, "rb");
    if (!tf) { perror(tokpath); return NULL; }
    int nv; fread(&nv, 4, 1, tf);
    m->tok = calloc(nv, sizeof(char *)); m->toklen = calloc(nv, sizeof(int));
    for (int i = 0; i < nv; i++) {
        uint16_t n; fread(&n, 2, 1, tf);
        m->tok[i] = malloc(n + 1); fread(m->tok[i], 1, n, tf); m->tok[i][n] = 0; m->toklen[i] = n;
    }
    fread(&m->nmerge, 4, 1, tf);
    m->merge = malloc(sizeof(int[3]) * m->nmerge);
    fread(m->merge, sizeof(int[3]), m->nmerge, tf);
    fread(m->byte_id, 4, 256, tf);
    fclose(tf);
    m->hsize = 1; while (m->hsize < m->nmerge * 2) m->hsize <<= 1;
    m->mhash = malloc(m->hsize * 4); memset(m->mhash, 0xff, m->hsize * 4);
    for (int r = 0; r < m->nmerge; r++) {
        int k = hkey(m->merge[r][0], m->merge[r][1], m->hsize);
        while (m->mhash[k] >= 0) k = (k + 1) & (m->hsize - 1);
        m->mhash[k] = r;
    }

    m->kc = calloc((size_t)m->nl * maxseq * m->kvd, 4);
    m->vc = calloc((size_t)m->nl * maxseq * m->kvd, 4);
    m->maxT = 32;
    int T = m->maxT;
    m->x = malloc(T * D * 4); m->xb = malloc(T * D * 4); m->q = malloc(T * D * 4);
    m->k = malloc(T * m->kvd * 4); m->v = malloc(T * m->kvd * 4);
    m->att = malloc(maxseq * 4); m->h1 = malloc((size_t)T * H * 4); m->h2 = malloc((size_t)T * H * 4);
    m->logits = malloc(m->vocab * 4);
    m->xq = malloc((size_t)T * H); m->xs = malloc((size_t)T * H / GS * 4);
    int half = m->hd / 2;
    m->rope_c = malloc((size_t)maxseq * half * 4); m->rope_s = malloc((size_t)maxseq * half * 4);
    for (int ps = 0; ps < maxseq; ps++)
        for (int i = 0; i < half; i++) {
            double f = ps * pow(m->theta, -2.0 * i / m->hd);
            m->rope_c[ps * half + i] = (float)cos(f); m->rope_s[ps * half + i] = (float)sin(f);
        }
    return m;
}

/* ---------- tokenizer ---------- */
static int merge_rank(llm_t *m, int a, int b) {
    int k = hkey(a, b, m->hsize);
    while (m->mhash[k] >= 0) {
        int r = m->mhash[k];
        if (m->merge[r][0] == a && m->merge[r][1] == b) return r;
        k = (k + 1) & (m->hsize - 1);
    }
    return -1;
}

static int bpe_word(llm_t *m, const unsigned char *s, int n, int *out, int max) {
    int buf[512]; if (n > 512) n = 512;
    for (int i = 0; i < n; i++) buf[i] = m->byte_id[s[i]];
    while (n > 1) {
        int best = -1, bi = -1;
        for (int i = 0; i + 1 < n; i++) {
            int r = merge_rank(m, buf[i], buf[i + 1]);
            if (r >= 0 && (best < 0 || r < best)) { best = r; bi = i; }
        }
        if (best < 0) break;
        buf[bi] = m->merge[best][2];
        memmove(buf + bi + 1, buf + bi + 2, (n - bi - 2) * 4);
        n--;
    }
    int c = 0;
    for (int i = 0; i < n && c < max; i++) out[c++] = buf[i];
    return c;
}

static int cls(unsigned char c) { // 0 space, 1 letter, 2 digit, 3 other
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') return 0;
    if ((c | 32) >= 'a' && (c | 32) <= 'z') return 1;
    if (c >= 0x80) return 1;
    if (c >= '0' && c <= '9') return 2;
    return 3;
}

// GPT-2 pre-tokenizer (ASCII approximation of the \p{L}/\p{N} regex) + BPE.
int llm_encode(llm_t *m, const char *text, int *out, int max) {
    const unsigned char *s = (const unsigned char *)text;
    int n = (int)strlen(text), i = 0, c = 0;
    while (i < n && c < max) {
        int j = i;
        if (s[i] == '\'' && i + 1 < n) {
            const char *cs[] = {"'s", "'t", "'re", "'ve", "'m", "'ll", "'d"};
            int hit = 0;
            for (int k = 0; k < 7; k++) { int l = (int)strlen(cs[k]); if (i + l <= n && !memcmp(s + i, cs[k], l)) { j = i + l; hit = 1; break; } }
            if (hit) { c += bpe_word(m, s + i, j - i, out + c, max - c); i = j; continue; }
        }
        if (cls(s[i]) == 0) {
            while (j < n && cls(s[j]) == 0) j++;
            if (j < n && j - i > 1) j--;               // last space joins the next word
            else if (j < n && j - i == 1 && s[i] == ' ') { j = i; goto word; }
            c += bpe_word(m, s + i, j - i, out + c, max - c); i = j; continue;
        }
    word:
        j = i;
        if (s[j] == ' ') j++;
        if (j < n) {
            int k = cls(s[j]);
            while (j < n && cls(s[j]) == k && !(k == 3 && s[j] == '\'' && 0)) j++;
        }
        c += bpe_word(m, s + i, j - i, out + c, max - c); i = j;
    }
    return c;
}

const char *llm_token_str(llm_t *m, int id, int *len) { *len = m->toklen[id]; return m->tok[id]; }

/* ---------- forward ---------- */
static void rmsnorm(float *o, const float *x, const float *w, int n, float eps) {
    float ss = 0;
    for (int i = 0; i < n; i++) ss += x[i] * x[i];
    float inv = 1.f / sqrtf(ss / n + eps);
    for (int i = 0; i < n; i++) o[i] = x[i] * inv * w[i];
}

static void rope_neox(llm_t *m, float *v, int nheads, int pos) {
    int half = m->hd / 2; const float *c = m->rope_c + pos * half, *s = m->rope_s + pos * half;
    for (int h = 0; h < nheads; h++) {
        float *p = v + h * m->hd;
        for (int i = 0; i < half; i++) {
            float a = p[i], b = p[i + half];
            p[i] = a * c[i] - b * s[i]; p[i + half] = b * c[i] + a * s[i];
        }
    }
}

static void attend_gqa(llm_t *m, float *out, const float *q, const float *K, const float *V, int n) {
    int hd = m->hd, kvd = m->kvd, rep = m->nh / m->nkv;
    float scale = 1.f / sqrtf((float)hd), *att = m->att;
    for (int h = 0; h < m->nh; h++) {
        const float *qh = q + h * hd; int kh = h / rep;
        float *oh = out + h * hd;
        float mx = -1e30f;
        for (int t = 0; t < n; t++) {
            const float *k = K + (size_t)t * kvd + kh * hd;
#ifdef HAVE_NEON
            float32x4_t a = vdupq_n_f32(0);
            for (int i = 0; i < hd; i += 4) a = vmlaq_f32(a, vld1q_f32(qh + i), vld1q_f32(k + i));
            float32x2_t s2 = vadd_f32(vget_low_f32(a), vget_high_f32(a));
            vst1_lane_f32(att + t, vpadd_f32(s2, s2), 0);
#else
            float s = 0; for (int i = 0; i < hd; i++) s += qh[i] * k[i]; att[t] = s;
#endif
        }
        for (int t = 0; t < n; t++) if (att[t] > mx) mx = att[t];
        float sum = 0;
        int t = 0;
#ifdef HAVE_NEON
        float32x4_t mxv = vdupq_n_f32(mx), sc = vdupq_n_f32(scale), s4 = vdupq_n_f32(0);
        for (; t + 4 <= n; t += 4) { float32x4_t e = exp_ps(vmulq_f32(vsubq_f32(vld1q_f32(att + t), mxv), sc)); vst1q_f32(att + t, e); s4 = vaddq_f32(s4, e); }
        sum = hsum_ps(s4);
#endif
        for (; t < n; t++) { att[t] = expf((att[t] - mx) * scale); sum += att[t]; }
        memset(oh, 0, hd * 4);
        float inv = 1.f / sum;
        for (t = 0; t < n; t++) {
            const float *v = V + (size_t)t * kvd + kh * hd;
#ifdef HAVE_NEON
            float32x4_t w = vdupq_n_f32(att[t] * inv);
            for (int i = 0; i < hd; i += 4) vst1q_f32(oh + i, vmlaq_f32(vld1q_f32(oh + i), w, vld1q_f32(v + i)));
#else
            float w = att[t] * inv; for (int i = 0; i < hd; i++) oh[i] += w * v[i];
#endif
        }
    }
}

// Run T (<= maxT) tokens at positions pos..pos+T-1; logits for the last one.
static void forward_chunk(llm_t *m, const int *toks, int T, int want_logits) {
    m->t_mark = now_ms();
    int D = m->dim, H = m->hidden, kvd = m->kvd;
    for (int t = 0; t < T; t++) {
        q8_row(&m->embed, toks[t], m->x + t * D);
    }
    for (int l = 0; l < m->nl; l++) {
        Layer *L = &m->L[l];
        float *Kc = m->kc + (size_t)l * m->maxseq * kvd, *Vc = m->vc + (size_t)l * m->maxseq * kvd;
        for (int t = 0; t < T; t++) rmsnorm(m->xb + t * D, m->x + t * D, L->an, D, m->eps);
        quantize(m->xb, m->xq, m->xs, T * D);
        matmul(m->q, m->xq, m->xs, T, D, &L->q, NULL);
        matmul(m->k, m->xq, m->xs, T, D, &L->k, NULL);
        matmul(m->v, m->xq, m->xs, T, D, &L->v, NULL);
        for (int t = 0; t < T; t++) {
            rope_neox(m, m->q + t * D, m->nh, m->pos + t);
            rope_neox(m, m->k + t * kvd, m->nkv, m->pos + t);
            memcpy(Kc + (size_t)(m->pos + t) * kvd, m->k + t * kvd, kvd * 4);
            memcpy(Vc + (size_t)(m->pos + t) * kvd, m->v + t * kvd, kvd * 4);
        }
        for (int t = 0; t < T; t++) attend_gqa(m, m->xb + t * D, m->q + t * D, Kc, Vc, m->pos + t + 1);
        quantize(m->xb, m->xq, m->xs, T * D);
        matmul(m->q, m->xq, m->xs, T, D, &L->o, NULL);
        for (int i = 0; i < T * D; i++) m->x[i] += m->q[i];
        for (int t = 0; t < T; t++) rmsnorm(m->xb + t * D, m->x + t * D, L->fn, D, m->eps);
        quantize(m->xb, m->xq, m->xs, T * D);
        matmul(m->h1, m->xq, m->xs, T, D, &L->g, NULL);
        matmul(m->h2, m->xq, m->xs, T, D, &L->u, NULL);
        swiglu_vec(m->h2, m->h1, (size_t)T * H);
        quantize(m->h2, m->xq, m->xs, T * H);
        matmul(m->q, m->xq, m->xs, T, H, &L->d, NULL);
        for (int i = 0; i < T * D; i++) m->x[i] += m->q[i];
    }
    m->pos += T;
    double tl0 = now_ms(); m->t_layers += tl0 - m->t_mark;
    if (want_logits) {
        rmsnorm(m->xb, m->x + (T - 1) * D, m->fnorm, D, m->eps);
        quantize(m->xb, m->xq, m->xs, D);
        matmul(m->logits, m->xq, m->xs, 1, D, &m->head, NULL);
        m->t_head += now_ms() - tl0;
    }
}

void llm_advise(llm_t *m, int need) { madvise(m->map, m->map_len, need ? MADV_WILLNEED : MADV_DONTNEED); }
// Pull everything a forward pass reads into the page cache, sequentially: the transformer layers
// and the classifier's slice of the embedding (rows beyond LLM_VOCAB_LIMIT are only looked up).
void llm_prefetch(llm_t *m) {
    const uint8_t *base = m->map;
    size_t qrow = (size_t)m->embed.in * m->embed.bits / 8, ng = m->embed.in / GS;
    size_t q0 = (const uint8_t *)m->embed.q - base, s0 = (const uint8_t *)m->embed.s4 - base;
    prefetch_range(m->fd, 0, q0);                                         // header
    prefetch_range(m->fd, q0, (size_t)m->head.out * qrow);                // classifier rows
    prefetch_range(m->fd, s0, (size_t)m->head.out * ng * 4);              // their scales
    size_t layers = s0 + (size_t)m->vocab * ng * 4;                       // end of the embedding
    prefetch_range(m->fd, layers, m->map_len - layers);
}
void llm_set_pos(llm_t *m, int pos) { m->pos = pos; }
int llm_get_pos(llm_t *m) { return m->pos; }

int llm_feed(llm_t *m, const int *toks, int n) {
    if (m->pos + n > m->maxseq) return -1;
    double t0 = now_ms();
    for (int i = 0; i < n; i += m->maxT) {
        int T = n - i < m->maxT ? n - i : m->maxT;
        forward_chunk(m, toks + i, T, i + T == n);
    }
    m->t_prefill += now_ms() - t0; m->n_prefill += n;
    return 0;
}

int llm_generate(llm_t *m, int maxnew, float rep_penalty, llm_token_cb cb, void *ud) {
    int hist[512], nh = 0;
    double t0 = now_ms();
    for (int i = 0; i < maxnew && m->pos < m->maxseq; i++) {
        float *lg = m->logits;
        for (int j = 0; j < nh; j++) { float *p = &lg[hist[j]]; *p = *p > 0 ? *p / rep_penalty : *p * rep_penalty; }
        int best = 0;
        for (int j = 1; j < m->head.out; j++) if (lg[j] > lg[best]) best = j;
        if (best == 0 || best == 2) break;                  // <|endoftext|>, <|im_end|>
        if (nh < 512) hist[nh++] = best;
        if (cb && cb(ud, m->tok[best], m->toklen[best])) break;
        forward_chunk(m, &best, 1, 1);
        m->n_gen++;
    }
    m->t_gen += now_ms() - t0;
    return nh;
}

void llm_stats(llm_t *m, double *pre_ms, int *npre, double *gen_ms, int *ngen) {
    *pre_ms = m->t_prefill; *npre = m->n_prefill; *gen_ms = m->t_gen; *ngen = m->n_gen;
    m->t_prefill = m->t_gen = 0; m->n_prefill = m->n_gen = 0;
}

// ChatML prompt pieces for SmolLM2-Instruct. The prefix (cached once) ends with the
// user turn's header, so a question costs only its own tokens + 5.
int llm_chat_prefix(llm_t *m, const char *system, int *out, int max) {
    int c = 0;
    out[c++] = 1;
    c += llm_encode(m, "system\n", out + c, max - c);
    c += llm_encode(m, system, out + c, max - c);
    out[c++] = 2;
    c += llm_encode(m, "\n", out + c, max - c);
    out[c++] = 1;
    c += llm_encode(m, "user\n", out + c, max - c);
    return c;
}
int llm_chat_user(llm_t *m, const char *user, int *out, int max) {
    int c = llm_encode(m, user, out, max - 8);
    out[c++] = 2;
    c += llm_encode(m, "\n", out + c, max - c);
    out[c++] = 1;
    c += llm_encode(m, "assistant\n", out + c, max - c);
    return c;
}

#ifdef LLM_MAIN
static int print_cb(void *ud, const char *s, int n) { fwrite(s, 1, n, stdout); fflush(stdout); return 0; }
int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s model.bin model.tok \"question\" [max_tokens] [system]\n", argv[0]); return 1; }
    int maxnew = argc > 4 ? atoi(argv[4]) : 64;
    const char *sys = argc > 5 ? argv[5] : "You are Siri, a helpful voice assistant. Answer in one or two short sentences.";
    double t0 = now_ms();
    llm_t *m = llm_load(argv[1], argv[2], 512);
    if (!m) return 1;
    fprintf(stderr, "load %.0f ms\n", now_ms() - t0);
    if (getenv("LLM_TOKTEST")) {
        int ids[256], n = llm_encode(m, argv[3], ids, 256);
        for (int i = 0; i < n; i++) printf("%d ", ids[i]);
        printf("\n"); return 0;
    }
    int ids[512], n = llm_chat_prefix(m, sys, ids, 512);
    llm_feed(m, ids, n);
    double pm; int pn; double gm; int gn;
    llm_stats(m, &pm, &pn, &gm, &gn);
    fprintf(stderr, "system prefix: %d tok in %.0f ms (%.1f tok/s)\n", pn, pm, pn * 1000.0 / pm);
    n = llm_chat_user(m, argv[3], ids, 512);
    llm_feed(m, ids, n);
    llm_generate(m, maxnew, 1.1f, print_cb, NULL);
    printf("\n");
    llm_stats(m, &pm, &pn, &gm, &gn);
    fprintf(stderr, "time in layers %.0f ms, classifier %.0f ms\n", m->t_layers, m->t_head);
    fprintf(stderr, "prompt %d tok in %.0f ms (%.1f tok/s) | gen %d tok in %.0f ms (%.2f tok/s)\n",
            pn, pm, pn * 1000.0 / pm, gn, gm, gn ? gn * 1000.0 / gm : 0);
    return 0;
}
#endif
