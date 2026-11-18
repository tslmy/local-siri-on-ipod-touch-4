# Convert a HF llama-architecture model (default SmolLM2-135M-Instruct) into
# the Q8_0 format read by llm.c, plus a byte-level BPE tokenizer file.
#   uv run convert/llm_convert.py --wbits 4 --ebits 8 --out build/models/smollm2-135m-q4e8
import struct
import json
import numpy as np
from transformers import AutoModelForCausalLM, AutoTokenizer

import argparse

ap = argparse.ArgumentParser()
ap.add_argument("--model", default="HuggingFaceTB/SmolLM2-135M-Instruct")
ap.add_argument("--revision", default="12fd25f77366fa6b3b4b768ec3050bf629380bac")  # pinned HF commit
ap.add_argument("--out", default="smollm2-135m")
ap.add_argument("--wbits", type=int, default=8, choices=[4, 8])  # layer matrices
ap.add_argument("--ebits", type=int, default=8, choices=[4, 8])  # tied embedding / classifier
args = ap.parse_args()
name, prefix = args.model, args.out
m = AutoModelForCausalLM.from_pretrained(name, revision=args.revision)
c = m.config
sd = {k: v.float().numpy() for k, v in m.state_dict().items()}
GS = 32
assert c.tie_word_embeddings
theta = c.rope_parameters["rope_theta"] if getattr(c, "rope_parameters", None) else c.rope_theta

f = open(prefix + ".bin", "wb")
f.write(
    struct.pack(
        "<8i2f2i",
        0x4C4C4D51,
        3,
        c.hidden_size,
        c.intermediate_size,
        c.num_hidden_layers,
        c.num_attention_heads,
        c.num_key_value_heads,
        c.vocab_size,
        float(theta),
        float(c.rms_norm_eps),
        args.wbits,
        args.ebits,
    )
)


def fp32(n):
    f.write(np.ascontiguousarray(sd[n], dtype="<f4").tobytes())


def q8(n, bits=None):
    bits = bits or args.wbits
    a = sd[n].astype(np.float32)
    o, i = a.shape
    g = a.reshape(o, i // GS, GS)
    if bits == 8:
        s = np.abs(g).max(axis=2) / 127.0
        s[s == 0] = 1.0
        q = np.clip(np.round(g / s[..., None]), -127, 127).astype(np.int8)
        f.write(q.tobytes())
    else:  # Q4_0: byte j of a group = (q[j]+8) | (q[j+16]+8) << 4
        s = np.abs(g).max(axis=2) / 7.0
        s[s == 0] = 1.0
        q = (np.clip(np.round(g / s[..., None]), -8, 7) + 8).astype(np.uint8)
        f.write((q[..., :16] | (q[..., 16:] << 4)).astype(np.uint8).tobytes())
    # scales in [out/4][in/32][4] order (what the NEON kernel loads)
    f.write(np.ascontiguousarray(s.reshape(o // 4, 4, -1).transpose(0, 2, 1)).astype("<f4").tobytes())


q8("model.embed_tokens.weight", args.ebits)
for l in range(c.num_hidden_layers):
    p = f"model.layers.{l}."
    fp32(p + "input_layernorm.weight")
    for n in "qkvo":
        q8(p + f"self_attn.{n}_proj.weight")
    fp32(p + "post_attention_layernorm.weight")
    q8(p + "mlp.gate_proj.weight")
    q8(p + "mlp.up_proj.weight")
    q8(p + "mlp.down_proj.weight")
fp32("model.norm.weight")
f.close()

# ---- tokenizer: vocab bytes, ranked merges, byte -> initial id ----
tok = AutoTokenizer.from_pretrained(name, revision=args.revision)
j = json.loads(tok.backend_tokenizer.to_str())
vocab = j["model"]["vocab"]
for a in j["added_tokens"]:
    vocab[a["content"]] = a["id"]
bs = list(range(ord("!"), ord("~") + 1)) + list(range(ord("¡"), ord("¬") + 1)) + list(range(ord("®"), ord("ÿ") + 1))
cs = bs[:]
n = 0
for b in range(256):
    if b not in bs:
        bs.append(b)
        cs.append(256 + n)
        n += 1
u2b = {chr(cc): b for b, cc in zip(bs, cs, strict=False)}
inv = [b""] * c.vocab_size
special = {a["id"] for a in j["added_tokens"]}
for s, i in vocab.items():
    if i >= c.vocab_size:
        continue
    inv[i] = s.encode() if i in special else bytes(u2b[ch] for ch in s)
merges = j["model"]["merges"]
with open(prefix + ".tok", "wb") as t:
    t.write(struct.pack("<i", c.vocab_size))
    for b in inv:
        t.write(struct.pack("<H", len(b)) + b)
    t.write(struct.pack("<i", len(merges)))
    for mg in merges:
        a, b = mg if isinstance(mg, list) else mg.split(" ")
        t.write(struct.pack("<3i", vocab[a], vocab[b], vocab[a + b]))
    byte_id = [vocab.get(chr(cs[bs.index(b)]), 0) for b in range(256)]
    t.write(struct.pack("<256i", *byte_id))
print("ok", prefix)
