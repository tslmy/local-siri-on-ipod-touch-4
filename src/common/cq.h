// CQ ("codebook quantization") matrix kernels, shared by the Whistle speech model and the chat model.
//
// A group of g weights (g = 64 or 128) is stored as an L2 norm plus one 2- or 4-bit index per weight into a
// Lloyd-Max codebook for a unit Gaussian, after a Walsh-Hadamard rotation: w = norm * (cb[idx] / sqrt(g)) @ H.
// H is symmetric, so w . x = norm * (cb[idx] / sqrt(g)) . (H x): the activation is rotated once per group
// (a fast Walsh-Hadamard transform) and quantized to int8, and the inner loop is a NEON table lookup from
// index to int8 level plus int8 multiplies. Weights are never dequantized for a matmul.
//
// Indices are an LSB-first bitstream per row. Activations are prepped in the order the kernel unpacks:
// 2-bit: in each 64-run, slot 16j+m holds element 4m+j; 4-bit: in each 32-run, slot 16j+m holds element 2m+j.
#ifndef CQ_H
#define CQ_H
#include "q8.h"
#include <sys/time.h>

typedef struct { const uint8_t *q; float *ns; int out, in, bits, g; } CQ;   // ns: norm * level scale, [out][in/g]

static int8_t cq_lv2[8], cq_lv4[16];      // codebook levels as int8 (lv2 padded for vtbl1)
static float cq_u2[4], cq_u4[16];         // unit-Gaussian Lloyd-Max levels
static double cq_ms_mm, cq_ms_prep;       // time spent, for profiling

static double cq_now(void) { struct timeval tv; gettimeofday(&tv, 0); return tv.tv_sec * 1e3 + tv.tv_usec / 1e3; }

// Set the unit codebook for 2 or 4 bits (sorted Lloyd-Max levels for N(0,1)).
static void cq_set_codebook(int bits, const float *u) {
    int n = 1 << bits; float amax = 0, *dst = bits == 2 ? cq_u2 : cq_u4; int8_t *lv = bits == 2 ? cq_lv2 : cq_lv4;
    for (int i = 0; i < n; i++) { dst[i] = u[i]; if (fabsf(u[i]) > amax) amax = fabsf(u[i]); }
    for (int i = 0; i < n; i++) lv[i] = (int8_t)lrintf(u[i] / amax * 127.f);
}
// Multiply a stored group norm by this to get the kernel's per-group scale.
static float cq_kscale(int bits, int g) {
    const float *u = bits == 2 ? cq_u2 : cq_u4; int n = 1 << bits; float amax = 0;
    for (int i = 0; i < n; i++) if (fabsf(u[i]) > amax) amax = fabsf(u[i]);
    return amax / sqrtf((float)g) / 127.f;
}

// In-place unnormalized fast Walsh-Hadamard transform of g (64 or 128) floats.
static void cq_fwht(float *x, int g) {
#ifdef HAVE_NEON
    const uint32x4_t m02 = vreinterpretq_u32_u64(vdupq_n_u64(0x00000000ffffffffull));   // lanes 0,2
    const uint32x4_t m01 = vcombine_u32(vdup_n_u32(~0u), vdup_n_u32(0));                // lanes 0,1
    float32x4_t v[32];
    int nv = g / 4;
    for (int i = 0; i < nv; i++) {
        float32x4_t a = vld1q_f32(x + 4 * i), p = vrev64q_f32(a);
        a = vbslq_f32(m02, vaddq_f32(a, p), vsubq_f32(p, a));                          // h = 1
        float32x4_t q = vcombine_f32(vget_high_f32(a), vget_low_f32(a));
        v[i] = vbslq_f32(m01, vaddq_f32(a, q), vsubq_f32(q, a));                       // h = 2
    }
    for (int h = 1; h < nv; h <<= 1)                                                   // h = 4 ..
        for (int i = 0; i < nv; i += h << 1)
            for (int j = i; j < i + h; j++) { float32x4_t a = v[j], c = v[j + h]; v[j] = vaddq_f32(a, c); v[j + h] = vsubq_f32(a, c); }
    for (int i = 0; i < nv; i++) vst1q_f32(x + 4 * i, v[i]);
#else
    for (int h = 1; h < g; h <<= 1)
        for (int i = 0; i < g; i += h << 1)
            for (int j = i; j < i + h; j++) { float a = x[j], b = x[j + h]; x[j] = a + b; x[j + h] = a - b; }
#endif
}

// Rotate + int8-quantize n activations (n % g == 0) for a CQ matmul of the given bits.
static void cq_prep(const float *x, int n, int bits, int g, int8_t *xq, float *xs) {
    double t0 = cq_now();
    float r[128] __attribute__((aligned(16)));
    int8_t t[128] __attribute__((aligned(16)));
    const float rs = 1.f / sqrtf((float)g);
    for (int gi = 0; gi < n / g; gi++) {
        memcpy(r, x + gi * g, g * 4);
        cq_fwht(r, g);
        int8_t *o = xq + gi * g;
#ifdef HAVE_NEON
        float32x4_t mx = vdupq_n_f32(0);
        for (int i = 0; i < g; i += 4) mx = vmaxq_f32(mx, vabsq_f32(vld1q_f32(r + i)));
        float32x2_t m2 = vpmax_f32(vget_low_f32(mx), vget_high_f32(mx)); m2 = vpmax_f32(m2, m2);
        float amax = vget_lane_f32(m2, 0), inv = amax > 0 ? 127.f / amax : 0;
        xs[gi] = amax / 127.f * rs;
        const float32x4_t half = vdupq_n_f32(0.5f);
        const uint32x4_t sign = vdupq_n_u32(0x80000000u);
        for (int i = 0; i < g; i += 16) {
            int16x4_t h[4];
            for (int k = 0; k < 4; k++) {
                float32x4_t v = vmulq_n_f32(vld1q_f32(r + i + 4 * k), inv);
                float32x4_t rh = vreinterpretq_f32_u32(vorrq_u32(vandq_u32(vreinterpretq_u32_f32(v), sign), vreinterpretq_u32_f32(half)));
                h[k] = vmovn_s32(vcvtq_s32_f32(vaddq_f32(v, rh)));
            }
            vst1q_s8(t + i, vcombine_s8(vmovn_s16(vcombine_s16(h[0], h[1])), vmovn_s16(vcombine_s16(h[2], h[3]))));
        }
        if (bits == 2) {
            for (int c = 0; c < g; c += 64) {
                int8x16x4_t d = vld4q_s8(t + c);
                vst1q_s8(o + c, d.val[0]); vst1q_s8(o + c + 16, d.val[1]); vst1q_s8(o + c + 32, d.val[2]); vst1q_s8(o + c + 48, d.val[3]);
            }
        } else {
            for (int c = 0; c < g; c += 32) {
                int8x16x2_t d = vld2q_s8(t + c);
                vst1q_s8(o + c, d.val[0]); vst1q_s8(o + c + 16, d.val[1]);
            }
        }
#else
        float amax = 0;
        for (int i = 0; i < g; i++) { float a = fabsf(r[i]); if (a > amax) amax = a; }
        float inv = amax > 0 ? 127.f / amax : 0;
        xs[gi] = amax / 127.f * rs;
        for (int i = 0; i < g; i++) { float v = r[i] * inv; t[i] = (int8_t)(int)(v + (v < 0 ? -0.5f : 0.5f)); }
        if (bits == 2) {
            for (int c = 0; c < g; c += 64)
                for (int j = 0; j < 4; j++) for (int m = 0; m < 16; m++) o[c + 16 * j + m] = t[c + 4 * m + j];
        } else {
            for (int c = 0; c < g; c += 32)
                for (int j = 0; j < 2; j++) for (int m = 0; m < 16; m++) o[c + 16 * j + m] = t[c + 2 * m + j];
        }
#endif
    }
    cq_ms_prep += cq_now() - t0;
}

// One weight row against one prepped activation row.
static float cq_dot(const CQ *W, int o, const int8_t *xq, const float *xs) {
    int g = W->g, ng = W->in / g;
    const uint8_t *w = W->q + (size_t)o * W->in * W->bits / 8;
    const float *ns = W->ns + (size_t)o * ng;
#ifdef HAVE_NEON
    __builtin_prefetch(ns + 16);
    float32x4_t acc = vdupq_n_f32(0);
    if (W->bits == 2) {
        const int8x8_t lut = vld1_s8(cq_lv2);
        const uint8x16_t m3 = vdupq_n_u8(3);
        for (int gi = 0; gi < ng; gi++) {
            int32x4_t s = vdupq_n_s32(0);
            __builtin_prefetch(w + 192);
            for (int c = 0; c < g; c += 64) {
                uint8x16_t b = vld1q_u8(w); w += 16;
                const int8_t *x = xq + gi * g + c;
                uint8x16_t ix[4] = { vandq_u8(b, m3), vandq_u8(vshrq_n_u8(b, 2), m3), vandq_u8(vshrq_n_u8(b, 4), m3), vshrq_n_u8(b, 6) };
                for (int j = 0; j < 4; j++) {
                    int16x8_t p = vmull_s8(vtbl1_s8(lut, vreinterpret_s8_u8(vget_low_u8(ix[j]))), vld1_s8(x + 16 * j));
                    p = vmlal_s8(p, vtbl1_s8(lut, vreinterpret_s8_u8(vget_high_u8(ix[j]))), vld1_s8(x + 16 * j + 8));
                    s = vpadalq_s16(s, p);
                }
            }
            acc = vmlaq_n_f32(acc, vcvtq_f32_s32(s), ns[gi] * xs[gi]);
        }
    } else {
        int8x8x2_t lut; lut.val[0] = vld1_s8(cq_lv4); lut.val[1] = vld1_s8(cq_lv4 + 8);
        const uint8x16_t m15 = vdupq_n_u8(15);
        for (int gi = 0; gi < ng; gi++) {
            int32x4_t s = vdupq_n_s32(0);
            __builtin_prefetch(w + 192);
            for (int c = 0; c < g; c += 32) {
                uint8x16_t b = vld1q_u8(w); w += 16;
                const int8_t *x = xq + gi * g + c;
                uint8x16_t ix[2] = { vandq_u8(b, m15), vshrq_n_u8(b, 4) };
                for (int j = 0; j < 2; j++) {
                    int16x8_t p = vmull_s8(vtbl2_s8(lut, vreinterpret_s8_u8(vget_low_u8(ix[j]))), vld1_s8(x + 16 * j));
                    p = vmlal_s8(p, vtbl2_s8(lut, vreinterpret_s8_u8(vget_high_u8(ix[j]))), vld1_s8(x + 16 * j + 8));
                    s = vpadalq_s16(s, p);
                }
            }
            acc = vmlaq_n_f32(acc, vcvtq_f32_s32(s), ns[gi] * xs[gi]);
        }
    }
    return hsum_ps(acc);
#else
    float sum = 0;
    for (int gi = 0; gi < ng; gi++) {
        int32_t si = 0;
        const int8_t *x = xq + gi * g;
        if (W->bits == 2) {
            for (int c = 0; c < g; c += 64, w += 16)
                for (int m = 0; m < 16; m++)
                    for (int j = 0; j < 4; j++) si += cq_lv2[(w[m] >> (2 * j)) & 3] * x[c + 16 * j + m];
        } else {
            for (int c = 0; c < g; c += 32, w += 16)
                for (int m = 0; m < 16; m++)
                    si += cq_lv4[w[m] & 15] * x[c + m] + cq_lv4[w[m] >> 4] * x[c + 16 + m];
        }
        sum += (float)si * ns[gi] * xs[gi];
    }
    return sum;
#endif
}

#ifdef HAVE_NEON
// One weight row against 4 prepped activation rows: the weights are unpacked once per chunk.
static void cq_dot4(const CQ *W, int o, const int8_t *xq, const float *xs, float *out, int ostride) {
    int g = W->g, ng = W->in / g, in = W->in;
    const uint8_t *w = W->q + (size_t)o * in * W->bits / 8;
    const float *ns = W->ns + (size_t)o * ng;
    float32x4_t acc[4] = { vdupq_n_f32(0), vdupq_n_f32(0), vdupq_n_f32(0), vdupq_n_f32(0) };
    if (W->bits == 2) {
        const int8x8_t lut = vld1_s8(cq_lv2);
        const uint8x16_t m3 = vdupq_n_u8(3);
        for (int gi = 0; gi < ng; gi++) {
            int32x4_t s[4] = { vdupq_n_s32(0), vdupq_n_s32(0), vdupq_n_s32(0), vdupq_n_s32(0) };
            __builtin_prefetch(w + 192);
            for (int c = 0; c < g; c += 64) {
                uint8x16_t b = vld1q_u8(w); w += 16;
                uint8x16_t ix[4] = { vandq_u8(b, m3), vandq_u8(vshrq_n_u8(b, 2), m3), vandq_u8(vshrq_n_u8(b, 4), m3), vshrq_n_u8(b, 6) };
                for (int j = 0; j < 4; j++) {
                    int8x8_t wl = vtbl1_s8(lut, vreinterpret_s8_u8(vget_low_u8(ix[j]))), wh = vtbl1_s8(lut, vreinterpret_s8_u8(vget_high_u8(ix[j])));
                    for (int t = 0; t < 4; t++) {
                        const int8_t *x = xq + (size_t)t * in + gi * g + c + 16 * j;
                        s[t] = vpadalq_s16(s[t], vmlal_s8(vmull_s8(wl, vld1_s8(x)), wh, vld1_s8(x + 8)));
                    }
                }
            }
            for (int t = 0; t < 4; t++) acc[t] = vmlaq_n_f32(acc[t], vcvtq_f32_s32(s[t]), ns[gi] * xs[t * ng + gi]);
        }
    } else {
        int8x8x2_t lut; lut.val[0] = vld1_s8(cq_lv4); lut.val[1] = vld1_s8(cq_lv4 + 8);
        const uint8x16_t m15 = vdupq_n_u8(15);
        for (int gi = 0; gi < ng; gi++) {
            int32x4_t s[4] = { vdupq_n_s32(0), vdupq_n_s32(0), vdupq_n_s32(0), vdupq_n_s32(0) };
            __builtin_prefetch(w + 192);
            for (int c = 0; c < g; c += 32) {
                uint8x16_t b = vld1q_u8(w); w += 16;
                uint8x16_t ix[2] = { vandq_u8(b, m15), vshrq_n_u8(b, 4) };
                for (int j = 0; j < 2; j++) {
                    int8x8_t wl = vtbl2_s8(lut, vreinterpret_s8_u8(vget_low_u8(ix[j]))), wh = vtbl2_s8(lut, vreinterpret_s8_u8(vget_high_u8(ix[j])));
                    for (int t = 0; t < 4; t++) {
                        const int8_t *x = xq + (size_t)t * in + gi * g + c + 16 * j;
                        s[t] = vpadalq_s16(s[t], vmlal_s8(vmull_s8(wl, vld1_s8(x)), wh, vld1_s8(x + 8)));
                    }
                }
            }
            for (int t = 0; t < 4; t++) acc[t] = vmlaq_n_f32(acc[t], vcvtq_f32_s32(s[t]), ns[gi] * xs[t * ng + gi]);
        }
    }
    for (int t = 0; t < 4; t++) out[(size_t)t * ostride] = hsum_ps(acc[t]);
}
#endif

// out[t*ostride + o - r0] = W[o] . X[t] for rows [r0, r1). X is T prepped rows of W->in.
static void cq_mm(float *out, int ostride, const CQ *W, int r0, int r1, const int8_t *xq, const float *xs, int T) {
    double t1 = cq_now();
    int ng = W->in / W->g, t0 = 0;
#ifdef HAVE_NEON
    for (; t0 + 4 <= T; t0 += 4)
        for (int o = r0; o < r1; o++)
            cq_dot4(W, o, xq + (size_t)t0 * W->in, xs + (size_t)t0 * ng, out + (size_t)t0 * ostride + o - r0, ostride);
#endif
    for (int o = r0; o < r1; o++)
        for (int t = t0; t < T; t++)
            out[(size_t)t * ostride + o - r0] = cq_dot(W, o, xq + (size_t)t * W->in, xs + (size_t)t * ng);
    cq_ms_mm += cq_now() - t1;
}

// Dequantize row o of W into out[W->in].
static void cq_row(const CQ *W, int o, float *out) {
    int g = W->g, ng = W->in / g;
    const uint8_t *w = W->q + (size_t)o * W->in * W->bits / 8;
    const float *u = W->bits == 2 ? cq_u2 : cq_u4;
    float back = 1.f / cq_kscale(W->bits, g) / sqrtf((float)g);   // ns -> norm, and the codebook's 1/sqrt(g)
    for (int gi = 0; gi < ng; gi++) {
        float n = W->ns[(size_t)o * ng + gi] * back / sqrtf((float)g), *r = out + gi * g;
        for (int i = 0; i < g; i++) {
            int bit = (gi * g + i) * W->bits, idx = (w[bit >> 3] >> (bit & 7)) & ((1 << W->bits) - 1);
            r[i] = u[idx] * n;
        }
        cq_fwht(r, g);
    }
}
#endif
