"""CQ ("codebook quantization", the scheme Cactus Compute ships Whistle in), for any weight matrix.

Each group of G input weights is rotated by the normalized Walsh-Hadamard matrix H, split into its L2 norm and
a unit vector, and the unit vector's coordinates are snapped to a Lloyd-Max codebook for a Gaussian (the rotation
makes them close to Gaussian). Reconstruction: w = norm * (codebook[idx] @ H). The C kernels never reconstruct:
they rotate the activation instead (src/common/cq.h).

Storage per matrix [out, in]: packed indices, LSB-first bitstream per row (out * in * bits / 8 bytes), then
float32 norms [out][in / G]. The codebooks are reproducible from this file.
"""

import functools

import numpy as np


@functools.lru_cache(maxsize=None)
def lloyd_max_gaussian(bits, iters=200, samples=400000, seed=0):
    """Same procedure (and therefore same levels) as cactus/model/quantize.py."""
    levels = 1 << bits
    x = np.sort(np.random.RandomState(seed).randn(samples))
    c = x[((np.arange(levels) + 0.5) / levels * samples).astype(int)].astype(np.float64)
    for _ in range(iters):
        bnd = (c[:-1] + c[1:]) / 2.0
        idx = np.searchsorted(bnd, x)
        for k in range(levels):
            m = idx == k
            if m.any():
                c[k] = x[m].mean()
    return np.sort(c).astype(np.float32)


@functools.lru_cache(maxsize=None)
def hadamard(g):
    h = np.array([[1.0]], np.float32)
    while h.shape[0] < g:
        h = np.block([[h, h], [h, -h]])
    return (h / np.sqrt(g)).astype(np.float32)


def quantize(w, bits, group):
    """w [out, in] float -> (idx uint8 [out, in], norms float32 [out, in/group])."""
    out, n = w.shape
    assert n % group == 0
    cb = lloyd_max_gaussian(bits) / np.sqrt(group)
    g = w.reshape(out, n // group, group).astype(np.float32) @ hadamard(group)
    norm = np.sqrt((g * g).sum(-1, keepdims=True))
    unit = g / np.maximum(norm, 1e-12)
    bnd = (cb[:-1] + cb[1:]) / 2
    idx = np.searchsorted(bnd, unit).astype(np.uint8)
    return idx.reshape(out, n), norm[..., 0]


def dequantize(idx, norms, bits, group):
    out, n = idx.shape
    cb = lloyd_max_gaussian(bits) / np.sqrt(group)
    unit = cb[idx].reshape(out, n // group, group)
    return ((unit * norms[..., None]) @ hadamard(group)).reshape(out, n)


def pack(idx, bits):
    """LSB-first bitstream per row (index k at bits [k*bits, (k+1)*bits))."""
    out, n = idx.shape
    assert (n * bits) % 8 == 0
    b = ((idx[..., None] >> np.arange(bits, dtype=np.uint8)) & 1).reshape(out, n * bits)
    return np.packbits(b, axis=1, bitorder="little")


def fake(w, bits, group):
    return dequantize(*quantize(w, bits, group), bits, group)
