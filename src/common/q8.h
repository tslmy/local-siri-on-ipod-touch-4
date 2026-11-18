// Shared int8 (Q8_0, group 32) matmul kernels and NEON vector math.
#ifndef Q8_H
#define Q8_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define HAVE_NEON 1
#endif
#define GS 32

// bits 8: int8 rows; bits 4: Q4_0 (byte i = elem i | elem i+16 << 4, +8 offset).
// s4: fp32 group scales in [out/4][in/32][4] order, so one NEON load gets 4 rows' scales.
typedef struct { const int8_t *q; const float *s4; int out, in, bits; } Q8;

// Point a Q8 at weights followed by scales in the file. packed=1: scales are
// already in s4 order (current converters) and are used in place from the mmap;
// packed=0: legacy row-major scales, repacked into a malloc'd copy.
// Read [off, off+len) of a file sequentially so its pages land in the unified buffer cache, which a
// MAP_SHARED mapping of the same file shares. On the iPod this pulls ~80 MB/s, while touching a
// cold mmap page by page manages ~6 MB/s.
#include <unistd.h>
static void prefetch_range(int fd, size_t off, size_t len) {
    static char buf[1 << 20];
    while (len > 0) {
        size_t n = len < sizeof buf ? len : sizeof buf;
        ssize_t r = pread(fd, buf, n, (off_t)off);
        if (r <= 0) break;
        off += (size_t)r; len -= (size_t)r;
    }
}

static Q8 q8_take_ex(const uint8_t **p, int out, int in, int bits, int packed) {
    Q8 w; w.out = out; w.in = in; w.bits = bits;
    int ng = in / GS;
    w.q = (const int8_t *)*p; *p += (size_t)out * in * bits / 8;
    const float *s = (const float *)*p; *p += (size_t)out * ng * 4;
    if (packed) { w.s4 = s; return w; }
    float *s4 = malloc((size_t)out * ng * 4);
    for (int o = 0; o < out; o++)
        for (int g = 0; g < ng; g++) s4[((size_t)(o / 4) * ng + g) * 4 + (o & 3)] = s[(size_t)o * ng + g];
    w.s4 = s4;
    return w;
}
static Q8 q8_take_bits(const uint8_t **p, int out, int in, int bits) { return q8_take_ex(p, out, in, bits, 0); }
static inline float q8_scale(const Q8 *w, int o, int g) { return w->s4[((size_t)(o / 4) * (w->in / GS) + g) * 4 + (o & 3)]; }
// Dequantize row o into out[in].
static void q8_row(const Q8 *w, int o, float *out) {
    int ng = w->in / GS;
    for (int g = 0; g < ng; g++) {
        float sc = q8_scale(w, o, g);
        if (w->bits == 8) {
            const int8_t *r = w->q + (size_t)o * w->in + g * GS;
            for (int i = 0; i < GS; i++) out[g * GS + i] = r[i] * sc;
        } else {
            const uint8_t *r = (const uint8_t *)w->q + (size_t)o * w->in / 2 + g * 16;
            for (int i = 0; i < 16; i++) { out[g * GS + i] = ((r[i] & 15) - 8) * sc; out[g * GS + i + 16] = ((r[i] >> 4) - 8) * sc; }
        }
    }
}
static Q8 q8_take(const uint8_t **p, int out, int in) { return q8_take_bits(p, out, in, 8); }

// Quantize n floats (n % 32 == 0) to int8 groups.
static void quantize(const float *x, int8_t *q, float *s, int n) {
    for (int g = 0; g < n / GS; g++) {
        const float *xg = x + g * GS;
        float amax = 0;
        for (int i = 0; i < GS; i++) { float a = fabsf(xg[i]); if (a > amax) amax = a; }
        float sc = amax / 127.f, inv = sc > 0 ? 1.f / sc : 0;
        s[g] = sc;
        for (int i = 0; i < GS; i++) q[g * GS + i] = (int8_t)lrintf(xg[i] * inv);
    }
}

// 4 consecutive weight rows (o..o+3) against one quantized activation row.
static inline void dot4_q8(float *out, const int8_t *w, const float *s4, int in, const int8_t *x, const float *xs, const float *bias) {
    int ng = in / GS;
#ifdef HAVE_NEON
    float32x4_t acc = vdupq_n_f32(0);
    for (int g = 0; g < ng; g++) {
        int8x16_t x0 = vld1q_s8(x + g * GS), x1 = vld1q_s8(x + g * GS + 16);
        int32x4_t s[4];
        for (int r = 0; r < 4; r++) {
            const int8_t *wr = w + (size_t)r * in + g * GS;
            int8x16_t w0 = vld1q_s8(wr), w1 = vld1q_s8(wr + 16);
            int16x8_t p = vmull_s8(vget_low_s8(w0), vget_low_s8(x0));
            p = vmlal_s8(p, vget_high_s8(w0), vget_high_s8(x0));
            int16x8_t q = vmull_s8(vget_low_s8(w1), vget_low_s8(x1));
            q = vmlal_s8(q, vget_high_s8(w1), vget_high_s8(x1));
            s[r] = vpadalq_s16(vpaddlq_s16(p), q);
        }
        int32x2_t a0 = vpadd_s32(vget_low_s32(s[0]), vget_high_s32(s[0]));
        int32x2_t a1 = vpadd_s32(vget_low_s32(s[1]), vget_high_s32(s[1]));
        int32x2_t a2 = vpadd_s32(vget_low_s32(s[2]), vget_high_s32(s[2]));
        int32x2_t a3 = vpadd_s32(vget_low_s32(s[3]), vget_high_s32(s[3]));
        int32x4_t sum = vcombine_s32(vpadd_s32(a0, a1), vpadd_s32(a2, a3));
        float32x4_t sc = vmulq_f32(vld1q_f32(s4 + g * 4), vld1q_dup_f32(xs + g));
        acc = vmlaq_f32(acc, vcvtq_f32_s32(sum), sc);
    }
    if (bias) acc = vaddq_f32(acc, vld1q_f32(bias));
    vst1q_f32(out, acc);
#else
    for (int r = 0; r < 4; r++) {
        float sum = 0;
        for (int g = 0; g < ng; g++) {
            int32_t si = 0;
            const int8_t *wr = w + (size_t)r * in + g * GS, *xg = x + g * GS;
            for (int i = 0; i < GS; i++) si += (int32_t)wr[i] * xg[i];
            sum += (float)si * s4[g * 4 + r] * xs[g];
        }
        out[r] = sum + (bias ? bias[r] : 0);
    }
#endif
}


// Q4_0 variant: rows are in/2 bytes.
static inline void dot4_q4(float *out, const uint8_t *w, const float *s4, int in, const int8_t *x, const float *xs, const float *bias) {
    int ng = in / GS, rb = in / 2;
#ifdef HAVE_NEON
    float32x4_t acc = vdupq_n_f32(0);
    const uint8x16_t m4 = vdupq_n_u8(0x0f); const int8x16_t off = vdupq_n_s8(8);
    for (int g = 0; g < ng; g++) {
        int8x16_t x0 = vld1q_s8(x + g * GS), x1 = vld1q_s8(x + g * GS + 16);
        int32x4_t s[4];
        for (int r = 0; r < 4; r++) {
            uint8x16_t b = vld1q_u8(w + (size_t)r * rb + g * 16);
            int8x16_t w0 = vsubq_s8(vreinterpretq_s8_u8(vandq_u8(b, m4)), off);
            int8x16_t w1 = vsubq_s8(vreinterpretq_s8_u8(vshrq_n_u8(b, 4)), off);
            int16x8_t p = vmull_s8(vget_low_s8(w0), vget_low_s8(x0));
            p = vmlal_s8(p, vget_high_s8(w0), vget_high_s8(x0));
            int16x8_t q = vmull_s8(vget_low_s8(w1), vget_low_s8(x1));
            q = vmlal_s8(q, vget_high_s8(w1), vget_high_s8(x1));
            s[r] = vpadalq_s16(vpaddlq_s16(p), q);
        }
        int32x2_t a0 = vpadd_s32(vget_low_s32(s[0]), vget_high_s32(s[0]));
        int32x2_t a1 = vpadd_s32(vget_low_s32(s[1]), vget_high_s32(s[1]));
        int32x2_t a2 = vpadd_s32(vget_low_s32(s[2]), vget_high_s32(s[2]));
        int32x2_t a3 = vpadd_s32(vget_low_s32(s[3]), vget_high_s32(s[3]));
        int32x4_t sum = vcombine_s32(vpadd_s32(a0, a1), vpadd_s32(a2, a3));
        float32x4_t sc = vmulq_f32(vld1q_f32(s4 + g * 4), vld1q_dup_f32(xs + g));
        acc = vmlaq_f32(acc, vcvtq_f32_s32(sum), sc);
    }
    if (bias) acc = vaddq_f32(acc, vld1q_f32(bias));
    vst1q_f32(out, acc);
#else
    for (int r = 0; r < 4; r++) {
        float sum = 0;
        for (int g = 0; g < ng; g++) {
            int32_t si = 0;
            const uint8_t *wr = w + (size_t)r * rb + g * 16; const int8_t *xg = x + g * GS;
            for (int i = 0; i < 16; i++) si += ((wr[i] & 15) - 8) * xg[i] + ((wr[i] >> 4) - 8) * xg[i + 16];
            sum += (float)si * s4[g * 4 + r] * xs[g];
        }
        out[r] = sum + (bias ? bias[r] : 0);
    }
#endif
}


// out[t][o] = W[o] . X[t] (+ bias).  X rows are quantized rows at byte stride
// xstride (scale stride xstride/GS), which lets conv im2col alias its input.
static void matmul(float *out, const int8_t *xq, const float *xs, int T, int xstride, const Q8 *W, const float *bias) {
    int ng = W->in / GS;
    if (W->bits == 4) {
        for (int o = 0; o < W->out; o += 4) {
            const uint8_t *w = (const uint8_t *)W->q + (size_t)o * W->in / 2;
            const float *s4 = W->s4 + (size_t)(o / 4) * ng * 4;
            for (int t = 0; t < T; t++)
                dot4_q4(out + (size_t)t * W->out + o, w, s4, W->in, xq + (size_t)t * xstride, xs + (size_t)t * (xstride / GS), bias ? bias + o : NULL);
        }
        return;
    }
    for (int o = 0; o < W->out; o += 4) {
        const int8_t *w = W->q + (size_t)o * W->in;
        const float *s4 = W->s4 + (size_t)(o / 4) * ng * 4;
        for (int t = 0; t < T; t++)
            dot4_q8(out + (size_t)t * W->out + o, w, s4, W->in, xq + (size_t)t * xstride, xs + (size_t)t * (xstride / GS), bias ? bias + o : NULL);
    }
}

/* ---------- vector math (NEON fast paths; avoid slow iOS 6 libm erff/expf) ---------- */
#ifdef HAVE_NEON
static inline float32x4_t recip_ps(float32x4_t d) {
    float32x4_t r = vrecpeq_f32(d);
    r = vmulq_f32(vrecpsq_f32(d, r), r);
    return vmulq_f32(vrecpsq_f32(d, r), r);
}
// Cephes-style exp, rel. error ~1e-7 over [-87, 88].
static inline float32x4_t exp_ps(float32x4_t x) {
    x = vminq_f32(x, vdupq_n_f32(88.3762626647949f));
    x = vmaxq_f32(x, vdupq_n_f32(-87.3365447505531f));
    float32x4_t fx = vmlaq_f32(vdupq_n_f32(0.5f), x, vdupq_n_f32(1.44269504088896341f));
    float32x4_t tmp = vcvtq_f32_s32(vcvtq_s32_f32(fx));
    uint32x4_t mask = vcgtq_f32(tmp, fx);
    fx = vsubq_f32(tmp, vreinterpretq_f32_u32(vandq_u32(mask, vreinterpretq_u32_f32(vdupq_n_f32(1.f)))));
    x = vmlsq_f32(x, fx, vdupq_n_f32(0.693359375f));
    x = vmlsq_f32(x, fx, vdupq_n_f32(-2.12194440e-4f));
    float32x4_t z = vmulq_f32(x, x);
    float32x4_t y = vdupq_n_f32(1.9875691500E-4f);
    y = vmlaq_f32(vdupq_n_f32(1.3981999507E-3f), y, x);
    y = vmlaq_f32(vdupq_n_f32(8.3334519073E-3f), y, x);
    y = vmlaq_f32(vdupq_n_f32(4.1665795894E-2f), y, x);
    y = vmlaq_f32(vdupq_n_f32(1.6666665459E-1f), y, x);
    y = vmlaq_f32(vdupq_n_f32(5.0000001201E-1f), y, x);
    y = vmlaq_f32(vaddq_f32(x, vdupq_n_f32(1.f)), y, z);
    int32x4_t e = vshlq_n_s32(vaddq_s32(vcvtq_s32_f32(fx), vdupq_n_s32(127)), 23);
    return vmulq_f32(y, vreinterpretq_f32_s32(e));
}
// Abramowitz-Stegun 7.1.26 erf, abs. error < 1.5e-7.
static inline float32x4_t erf_ps(float32x4_t x) {
    float32x4_t a = vabsq_f32(x);
    float32x4_t t = recip_ps(vmlaq_f32(vdupq_n_f32(1.f), a, vdupq_n_f32(0.3275911f)));
    float32x4_t y = vdupq_n_f32(1.061405429f);
    y = vmlaq_f32(vdupq_n_f32(-1.453152027f), y, t);
    y = vmlaq_f32(vdupq_n_f32(1.421413741f), y, t);
    y = vmlaq_f32(vdupq_n_f32(-0.284496736f), y, t);
    y = vmlaq_f32(vdupq_n_f32(0.254829592f), y, t);
    y = vmulq_f32(y, t);
    float32x4_t r = vmlsq_f32(vdupq_n_f32(1.f), y, exp_ps(vnegq_f32(vmulq_f32(a, a))));
    return vbslq_f32(vcltq_f32(x, vdupq_n_f32(0)), vnegq_f32(r), r);
}
static inline float32x4_t tanh_ps(float32x4_t x) {
    x = vminq_f32(vmaxq_f32(x, vdupq_n_f32(-9.f)), vdupq_n_f32(9.f));
    float32x4_t e = exp_ps(vaddq_f32(x, x));
    return vmlsq_f32(vdupq_n_f32(1.f), vdupq_n_f32(2.f), recip_ps(vaddq_f32(e, vdupq_n_f32(1.f))));
}
static inline float hsum_ps(float32x4_t v) {
    float32x2_t t = vadd_f32(vget_low_f32(v), vget_high_f32(v));
    return vget_lane_f32(vpadd_f32(t, t), 0);
}
#endif

static void gelu_vec(float *x, size_t n) {
    size_t i = 0;
#ifdef HAVE_NEON
    for (; i + 4 <= n; i += 4) {
        float32x4_t v = vld1q_f32(x + i);
        float32x4_t e = erf_ps(vmulq_n_f32(v, 0.70710678f));
        vst1q_f32(x + i, vmulq_f32(vmulq_n_f32(v, 0.5f), vaddq_f32(e, vdupq_n_f32(1.f))));
    }
#endif
    for (; i < n; i++) x[i] = 0.5f * x[i] * (1.f + erff(x[i] * 0.70710678f));
}
static void tanh_vec(float *x, size_t n) {
    size_t i = 0;
#ifdef HAVE_NEON
    for (; i + 4 <= n; i += 4) vst1q_f32(x + i, tanh_ps(vld1q_f32(x + i)));
#endif
    for (; i < n; i++) x[i] = tanhf(x[i]);
}
// x[i] = x[i] * silu(g[i])
static void swiglu_vec(float *x, const float *g, size_t n) {
    size_t i = 0;
#ifdef HAVE_NEON
    for (; i + 4 <= n; i += 4) {
        float32x4_t gv = vld1q_f32(g + i);
        float32x4_t sg = vmulq_f32(gv, recip_ps(vaddq_f32(vdupq_n_f32(1.f), exp_ps(vnegq_f32(gv)))));
        vst1q_f32(x + i, vmulq_f32(vld1q_f32(x + i), sg));
    }
#endif
    for (; i < n; i++) x[i] = x[i] * g[i] / (1.f + expf(-g[i]));
}

#endif
