"""NumPy reference for Cactus Compute's Whistle speech model, reading the published `whistle.cact` directly.

It mirrors cactus/model/whistle.py and architecture.py (Apache-2.0) one-for-one, with plain float math and
the deployed 2/4-bit weights, so the C port in src/whistle/ has something exact to compare against.

    uv run convert/whistle_ref.py whistle.cact clip.wav [--dump dir]
"""

import argparse
import math
import struct
import sys
import wave

import numpy as np

TAG = 0x05E12A84
HDR = "<48If"
REC = "<BBHIIIIQQII"
FP16, FP32, CQ, RAW = 1, 2, 3, 4
LANGS = ("en", "de", "fr", "es", "it", "nl", "pl")


def walsh(n):
    h = np.array([[1.0]], np.float32)
    while h.shape[0] < n:
        h = np.block([[h, h], [h, -h]])
    return h / np.sqrt(n)


def unpack_lsb(packed, bits, in_pad):
    out = packed.shape[0]
    chunks = packed.reshape(out, in_pad // 8, bits).astype(np.uint64)
    word = np.zeros(chunks.shape[:-1], np.uint64)
    for b in range(bits):
        word |= chunks[..., b] << np.uint64(8 * b)
    idx = np.empty((out, in_pad // 8, 8), np.uint8)
    for i in range(8):
        idx[..., i] = (word >> np.uint64(i * bits)) & np.uint64((1 << bits) - 1)
    return idx.reshape(out, in_pad)


class Cact:
    """Tensors of a .cact archive, in file order. CQ tensors are dequantized on first use."""

    def __init__(self, path):
        self.raw = raw = open(path, "rb").read()
        hdr = struct.unpack_from(HDR, raw, 0)
        assert hdr[0] == TAG, "not a .cact archive"
        self.hdr = hdr
        n, cb_n = hdr[1], hdr[2]
        off = struct.calcsize(HDR)
        cb = np.frombuffer(raw[off:off + cb_n * 4], np.float32)
        self.cb = {2: cb[0:4], 3: cb[4:12], 4: cb[12:28]}
        off += cb_n * 4
        self.recs = []
        for _ in range(n):
            self.recs.append(struct.unpack_from(REC, raw, off))
            off += struct.calcsize(REC)
        self.pos = 0
        self.cache = {}

    def get(self, i):
        if i in self.cache:
            return self.cache[i]
        dtype, ndim, _, *rest = self.recs[i]
        shape = tuple(rest[:ndim])
        offset, nbytes, group, bits = rest[4:8]
        blob = self.raw[offset:offset + nbytes]
        if dtype == FP16:
            t = np.frombuffer(blob, np.float16).reshape(shape).astype(np.float32)
        elif dtype == FP32:
            t = np.frombuffer(blob, np.float32).reshape(shape)
        elif dtype == CQ:
            out, ind = shape
            in_pad = -(-ind // group) * group
            npk = out * in_pad * bits // 8
            idx = unpack_lsb(np.frombuffer(blob[:npk], np.uint8).reshape(out, -1), bits, in_pad)
            norms = np.frombuffer(blob[npk:], np.float16).reshape(out, in_pad // group).astype(np.float32)
            unit = self.cb[bits][idx].reshape(out, in_pad // group, group)
            t = ((unit * norms[..., None]) @ walsh(group)).reshape(out, in_pad)[:, :ind]
        else:
            t = blob
        self.cache[i] = t
        return t

    def take(self):
        t = self.get(self.pos)
        self.pos += 1
        return t


def load(path):
    c = Cact(path)
    h = c.hdr
    L, taps = h[10], h[19]
    m = {"vocab": h[5], "dim": h[7], "heads": h[8], "kv": h[9], "layers": L, "qk": h[11], "v": h[12],
         "lanes": h[15], "taps": taps, "slots": h[20], "sub": h[21], "tables": h[22], "etaps": h[23],
         "edil": h[24], "orders": list(h[27:27 + h[26]]), "sites": list(h[32:32 + h[31]]), "rope": h[48]}
    m["embedding"] = c.take()
    hada = ("d1", "d2", "b2", "d3", "d4", "w1a", "w1b", "w2a", "w2b", "w3a", "w3b", "cond_v", "cond_u")
    dec = []
    for _ in range(L):
        d = {"norm_in": c.take(), "q": c.take(), "k": c.take(), "v": c.take()}
        if taps:
            d.update(q_taps=c.take(), k_taps=c.take(), v_taps=c.take())
        d.update(q_norm=c.take(), k_norm=c.take(), gate=c.take(), out=c.take(), post_norm=c.take(),
                 attn_gate=c.take(), pre_hada=c.take())
        d["mlp"] = {k: c.take() for k in hada}
        dec.append(d)
    m["dec"] = dec
    m["mhc"] = take_mhc(c, L, m["lanes"])
    m["p1"] = c.take().astype(np.int64)
    m["p2"] = c.take().astype(np.int64)
    m["engrams"] = [{"tables": c.take(), "key": c.take(), "value": c.take(), "taps": c.take()} for _ in m["sites"]]
    m["final_norm"] = c.take()
    man = c.take()
    assert man[0] == 3246.0, "no audio manifest (heads present?)"
    a = dict(zip(("ver", "text", "enc_layers", "enc_lanes", "conv_kernel", "stem", "stem_stages", "mels", "rate",
                  "fft", "win", "hop"), [int(v) for v in man[1:]]))
    m["audio"] = a
    for d in dec:
        d["cross_norm"] = c.take()
        d["cq"], d["ck"], d["cv"], d["cgate"], d["cout"] = (c.take() for _ in range(5))
        d["cq_norm"], d["ck_norm"], d["post_cross_norm"], d["cross_gate"] = (c.take() for _ in range(4))
    m["pe_gate"] = c.take()
    enc = []
    for _ in range(a["enc_layers"]):
        e = {k: c.take() for k in ("pre_hada_norm_0", "norm_in", "post_norm", "conv_norm", "conv_out_norm",
                                    "pre_hada")}
        e["q"], e["k"], e["v"], e["gate"], e["out"] = (c.take() for _ in range(5))
        e.update(q_norm=c.take(), k_norm=c.take(), attn_gate=c.take(), pw1=c.take(), pw2=c.take(), dw=c.take())
        e["mlp0"] = {k: c.take() for k in hada}
        e["mlp"] = {k: c.take() for k in hada}
        enc.append(e)
    m["enc"] = enc
    m["enc_mhc"] = take_mhc(c, a["enc_layers"], a["enc_lanes"])
    m["enc_final_norm"] = c.take()
    m["stem_w"] = c.take()
    m["stem_dw"], m["stem_pw"] = [], []
    for _ in range(1, a["stem_stages"]):
        m["stem_dw"].append(c.take())
        m["stem_pw"].append(c.take())
    m["stem_out"] = c.take()
    m["mel"] = c.take()
    m["tokenizer"] = c.take()
    assert c.pos == len(c.recs), (c.pos, len(c.recs))
    return m


def take_mhc(c, L, n):
    h = {k: c.take() for k in ("a_pre", "a_post", "a_res", "b_pre", "b_post", "b_res")}
    for k in ("phi_pre", "phi_post", "phi_res"):
        h[k] = c.take()  # (L*cols, n*C), [out, in]
    h["phi_pre"] = h["phi_pre"].reshape(L, n, -1)
    h["phi_post"] = h["phi_post"].reshape(L, n, -1)
    h["phi_res"] = h["phi_res"].reshape(L, n * n, -1)
    return h


def tokenizer_pieces(blob):
    n = struct.unpack_from("<I", blob, 0)[0]
    off = struct.calcsize("<IIIIIBBH")
    pieces, types = [], []
    for _ in range(n):
        _, t, ln = struct.unpack_from("<fBH", blob, off)
        off += 7
        pieces.append(blob[off:off + ln].decode("utf-8"))
        types.append(t)
        off += ln
    return pieces, types


def detok(blob, ids):
    pieces, types = tokenizer_pieces(blob)
    buf = bytearray()
    for i in ids:
        if types[i] == 4:
            buf.append(int(pieces[i][3:5], 16))
        elif types[i] in (1, 2):
            continue
        else:
            buf += pieces[i].encode()
    return buf.decode("utf-8", "replace").replace("▁", " ").lstrip(" ")


# ---- layers ----------------------------------------------------------------------------------------------------

def sigmoid(x):
    return 1 / (1 + np.exp(-x))


def silu(x):
    return x * sigmoid(x)


def zcnorm(x, scale, eps=1e-6):
    return (1 + scale) * x / np.sqrt(np.mean(x * x, -1, keepdims=True) + eps)


def rms_unit(x, eps=1e-6):
    return x / np.sqrt(np.mean(x * x, -1, keepdims=True) + eps)


def softmax(x, axis=-1):
    x = x - x.max(axis, keepdims=True)
    e = np.exp(x)
    return e / e.sum(axis, keepdims=True)


def hadamard_mlp(x, p, p1, p2):
    n = p["d1"].shape[0]
    cond = 1 + softmax(x @ p["cond_v"]) @ p["cond_u"]

    def kron(z, a, b):
        z = z.reshape(*z.shape[:-1], a.shape[0], b.shape[0])
        return np.einsum("...ij,ik,jl->...kl", z, a, b).reshape(*z.shape[:-2], n)

    z = kron(p["d1"] * x, p["w1a"], p["w1b"])[..., p1]
    z = kron(silu(p["d2"] * cond * z + p["b2"]), p["w2a"], p["w2b"])[..., p2]
    z = kron(p["d3"] * z, p["w3a"], p["w3b"])
    return p["d4"] * z


def rope_tables(hd, T, theta):
    f = 1.0 / theta ** (np.arange(0, hd, 2, dtype=np.float32) / hd)
    a = np.outer(np.arange(T, dtype=np.float32), f)
    return np.cos(a), np.sin(a)


def apply_rope(x, cos, sin):  # x (..., T, hd)
    h = x.shape[-1] // 2
    x1, x2 = x[..., :h], x[..., h:]
    return np.concatenate([x1 * cos - x2 * sin, x2 * cos + x1 * sin], -1)


def shift(z, j):
    if j == 0:
        return z
    out = np.zeros_like(z)
    out[j:] = z[:-j]
    return out


def attention(x, p, m, rope, kmask=None, causal=False):
    """Self-attention as MultiHeadAttention: x (T, D)."""
    H, KV, qk, vd = m["heads"], m["kv"], m["qk"], m["v"]
    T = x.shape[0]
    q, k, v = x @ p["q"].T, x @ p["k"].T, x @ p["v"].T
    if "q_taps" in p:
        q = sum(p["q_taps"][j] * shift(q, j) for j in range(len(p["q_taps"])))
        k = sum(p["k_taps"][j] * shift(k, j) for j in range(len(p["k_taps"])))
        v = sum(p["v_taps"][j] * shift(v, j) for j in range(len(p["v_taps"])))
    q = zcnorm(q.reshape(T, H, qk).transpose(1, 0, 2), p["q_norm"])
    k = zcnorm(k.reshape(T, KV, qk).transpose(1, 0, 2), p["k_norm"])
    v = v.reshape(T, KV, vd).transpose(1, 0, 2)
    q, k = apply_rope(q, *rope), apply_rope(k, *rope)
    rep = H // KV
    k, v = np.repeat(k, rep, 0), np.repeat(v, rep, 0)
    s = q @ k.transpose(0, 2, 1) / math.sqrt(qk)
    allowed = np.ones((T, T), bool)
    if causal:
        allowed &= np.tril(np.ones((T, T), bool))
    if kmask is not None:
        allowed &= kmask[None, :]
    s = np.where(allowed[None], s, -1e30)
    o = (softmax(s) @ v).transpose(1, 0, 2).reshape(T, H * vd)
    o = o * sigmoid(x @ p["gate"].T)
    return o @ p["out"].T


def cross_attention(x, mem, mmask, d, m):
    H, qk, vd = m["heads"], m["qk"], m["v"]
    T, S = x.shape[0], mem.shape[0]
    q = zcnorm((x @ d["cq"].T).reshape(T, H, qk).transpose(1, 0, 2), d["cq_norm"])
    k = zcnorm((mem @ d["ck"].T).reshape(S, H, qk).transpose(1, 0, 2), d["ck_norm"])
    v = (mem @ d["cv"].T).reshape(S, H, vd).transpose(1, 0, 2)
    s = q @ k.transpose(0, 2, 1) / math.sqrt(qk)
    s = np.where(mmask[None, None, :], s, -1e30)
    o = (softmax(s) @ v).transpose(1, 0, 2).reshape(T, H * vd)
    o = o * sigmoid(x @ d["cgate"].T)
    return o @ d["cout"].T


def sinkhorn(lg, iters=20):
    for _ in range(iters):
        lg = lg - np.log(np.exp(lg - lg.max(-1, keepdims=True)).sum(-1, keepdims=True)) - lg.max(-1, keepdims=True)
        lg = lg - np.log(np.exp(lg - lg.max(-2, keepdims=True)).sum(-2, keepdims=True)) - lg.max(-2, keepdims=True)
    return np.exp(lg)


def lane_stack(x, hc, L, n, block, final_norm):
    """mHC residual stream: x (T, D) -> broadcast to n lanes, L blocks, mean over lanes, final norm."""
    T, C = x.shape
    X = np.broadcast_to(x[:, None, :], (T, n, C)).copy()
    for i in range(L):
        lane = np.eye(n, dtype=np.float32)[i % n]
        pre_off, post_off = 8 * lane - 4, -4 * (1 - lane)
        nx = rms_unit(X.reshape(T, n * C))
        hpre = sigmoid(hc["a_pre"][i] * (nx @ hc["phi_pre"][i].T) + hc["b_pre"][i] + pre_off)
        u = np.einsum("tn,tnc->tc", hpre, X)
        y = block(i, u) - u
        hpost = 2 * sigmoid(hc["a_post"][i] * (nx @ hc["phi_post"][i].T) + hc["b_post"][i] + post_off)
        hres = sinkhorn(hc["a_res"][i] * (nx @ hc["phi_res"][i].T).reshape(T, n, n) + hc["b_res"][i])
        X = np.einsum("tij,tjc->tic", hres, X) + hpost[..., None] * y[:, None, :]
    return zcnorm(X.mean(1), final_norm)


# ---- audio front end and encoder ---------------------------------------------------------------------------------

def features(m, wav):
    a = m["audio"]
    win, hop, fft = a["win"], a["hop"], a["fft"]
    w = np.hanning(win).astype(np.float32)
    x = np.pad(wav.astype(np.float32), (win // 2, win // 2))
    nfr = (len(x) - win) // hop + 1
    frames = np.stack([x[i * hop:i * hop + win] * w for i in range(nfr)])
    spec = np.fft.rfft(frames, n=fft)
    power = np.abs(spec) ** 2
    feat = np.log(power @ m["mel"] + 2.0 ** -24)
    n = len(wav) // hop
    feat = feat[:n] if n <= len(feat) else feat
    mean = feat.mean(0, keepdims=True)
    std = np.sqrt(((feat - mean) ** 2).sum(0, keepdims=True) / max(n - 1, 1))
    return (feat - mean) / (std + 1e-5)


def dwconv2d_s2(x, w):
    """x (H, W, C), w (9, C) [3x3 kernel, row-major], stride 2, pad 1 -> (ceil(H/2), ceil(W/2), C)."""
    Hh, Ww, C = x.shape
    xp = np.pad(x, ((1, 2), (1, 2), (0, 0)))
    Ho, Wo = (Hh + 1) // 2, (Ww + 1) // 2
    out = np.zeros((Ho, Wo, w.shape[1]), np.float32)
    for dy in range(3):
        for dx in range(3):
            out += w[dy * 3 + dx] * xp[dy:dy + 2 * Ho:2, dx:dx + 2 * Wo:2]
    return out


def encode(m, wav):
    a = m["audio"]
    x = features(m, wav)  # (T, 80)
    n = x.shape[0]
    h = dwconv2d_s2(x[..., None], m["stem_w"])  # in 1 channel -> 128 via broadcast
    n = (n + 1) // 2
    h[n:] = 0
    for i in range(a["stem_stages"] - 1):
        h = dwconv2d_s2(silu(h), m["stem_dw"][i])
        n = (n + 1) // 2
        h[n:] = 0
        h = h @ m["stem_pw"][i].T
    h = silu(h).transpose(0, 2, 1).reshape(h.shape[0], -1)  # (T', C*F)
    x = h @ m["stem_out"].T
    if x.shape[0] % 2:
        x = np.pad(x, ((0, 1), (0, 0)))
    T = x.shape[0]
    seen = np.arange(T) < n
    rope = rope_tables(m["qk"], T, m["rope"])

    def block(i, x):
        e = m["enc"][i]
        x = x + 0.5 * hadamard_mlp(zcnorm(x, e["pre_hada_norm_0"]), e["mlp0"], m["p1"], m["p2"])
        h = attention(zcnorm(x, e["norm_in"]), e, m, rope, kmask=seen)
        x = x + sigmoid(e["attn_gate"][0]) * zcnorm(h, e["post_norm"])
        h = zcnorm(x, e["conv_norm"]) @ e["pw1"].T
        D = h.shape[1] // 2
        h = h[:, :D] * sigmoid(h[:, D:]) * seen[:, None]
        K = e["dw"].shape[0]
        hp = np.pad(h, ((K // 2, K // 2), (0, 0)))
        h = sum(e["dw"][k] * hp[k:k + T] for k in range(K))
        h = silu(zcnorm(h, e["conv_out_norm"]))
        x = x + h @ e["pw2"].T
        return x + 0.5 * hadamard_mlp(zcnorm(x, e["pre_hada"]), e["mlp"], m["p1"], m["p2"])

    out = lane_stack(x, m["enc_mhc"], a["enc_layers"], a["enc_lanes"], block, m["enc_final_norm"])
    return out, seen


# ---- decoder (full recompute per step; clarity over speed) -------------------------------------------------------

SEED, PRIME = 0x9E3779B9, 0x01000193


def engram_indices(tokens, orders, heads, slots):
    u = np.asarray(tokens, np.uint64)
    idx = []
    for oi, order in enumerate(orders):
        for h in range(heads):
            seed = (SEED * (oi * heads + h + 1)) & 0xFFFFFFFF
            acc = np.full_like(u, seed)
            for j in range(order):
                sh = np.zeros_like(u)
                sh[j:] = u[:len(u) - j]
                acc = ((acc ^ sh) * PRIME) & 0xFFFFFFFF
            acc = acc ^ (acc >> 15)
            idx.append(acc % slots)
    return np.stack(idx, -1).astype(np.int64)  # (T, tables)


def decode_logits(m, tokens, mem, mmask):
    D = m["dim"]
    T = len(tokens)
    x = m["embedding"][tokens] * math.sqrt(D)
    rope = rope_tables(m["qk"], T, m["rope"])
    orders, tables = m["orders"], m["tables"]
    heads = tables // len(orders)
    idx = engram_indices(tokens, orders, heads, m["slots"])
    kv = []
    for e in m["engrams"]:
        rows = []
        for t in range(tables):
            f = e["tables"][t * m["slots"] + idx[:, t]]
            o = orders[t // heads]
            f = f * (np.arange(T) >= o - 1)[:, None]
            rows.append(f)
        emb = np.concatenate(rows, -1)
        k, v = emb @ e["key"].T, emb @ e["value"].T
        v = sum(e["taps"][j] * shift(v, j * m["edil"]) for j in range(m["etaps"]))
        kv.append((k, v))

    def block(i, x):
        d = m["dec"][i]
        if i in m["sites"]:
            ek, ev = kv[m["sites"].index(i)]
            alpha = sigmoid((rms_unit(x) * rms_unit(ek)).sum(-1) / math.sqrt(D))
            x = x + alpha[:, None] * ev
        h = attention(zcnorm(x, d["norm_in"]), d, m, rope, causal=True)
        x = x + sigmoid(d["attn_gate"][0]) * zcnorm(h, d["post_norm"])
        h = cross_attention(zcnorm(x, d["cross_norm"]), mem, mmask, d, m)
        x = x + sigmoid(d["cross_gate"][0]) * zcnorm(h, d["post_cross_norm"])
        return x + hadamard_mlp(zcnorm(x, d["pre_hada"]), d["mlp"], m["p1"], m["p2"])

    x = lane_stack(x, m["mhc"], m["layers"], m["lanes"], block, m["final_norm"])
    return x[-1] @ m["embedding"].T


def sinusoids(D, T):
    ang = np.arange(T)[:, None] * np.exp(-np.log(10000.0) / (D // 2 - 1) * np.arange(D // 2))
    return np.concatenate([np.sin(ang), np.cos(ang)], -1).astype(np.float32)


def transcribe(m, wav, lang="en", max_tokens=100):
    enc, seen = encode(m, wav)
    mem = enc + sigmoid(m["pe_gate"][0]) * sinusoids(m["dim"], enc.shape[0])
    text = m["audio"]["text"]
    toks = [2, text + LANGS.index(lang)]
    for _ in range(max_tokens):
        lg = decode_logits(m, toks, mem, seen)
        lg[text:] = -np.inf
        nxt = int(lg.argmax())
        if nxt == 1:
            break
        toks.append(nxt)
    return toks[2:], enc


def read_wav(path):
    with wave.open(path) as w:
        assert w.getframerate() == 16000 and w.getnchannels() == 1 and w.getsampwidth() == 2
        return np.frombuffer(w.readframes(w.getnframes()), np.int16).astype(np.float32) / 32768.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cact")
    ap.add_argument("wavs", nargs="+")
    ap.add_argument("--dump", help="write encoder output + token ids per wav here (for the C port's tests)")
    args = ap.parse_args()
    m = load(args.cact)
    for path in args.wavs:
        ids, enc = transcribe(m, read_wav(path))
        print(f"{path}\t{detok(m['tokenizer'], ids)}\t{ids}")
        if args.dump:
            np.save(f"{args.dump}/{path.rsplit('/', 1)[-1]}.enc.npy", enc)
    sys.stdout.flush()


if __name__ == "__main__":
    main()
