# Convert UsefulSensors/moonshine-tiny (HF transformers) into the flat binary
# format read by moonshine.c.  Large matrices are stored Q8_0 (int8, one fp32
# scale per 32 inputs); small vectors and conv1 stay fp32.
#
#   .venv/bin/python convert.py [out_dir]
#
# Writes moonshine-tiny.bin and moonshine-vocab.bin.
import struct
import os
import numpy as np
from transformers import MoonshineForConditionalGeneration, AutoTokenizer

import argparse

ap = argparse.ArgumentParser()
ap.add_argument("out", nargs="?", default=".")
ap.add_argument("--bits", type=int, default=8, choices=[4, 8])
ap.add_argument("--name", default="moonshine-tiny")
ap.add_argument("--revision", default="390624ed33d594443aa4aa221f5b9f283b545b5a")  # pinned HF commit
args = ap.parse_args()
out = args.out
NAME = "UsefulSensors/moonshine-tiny"
m = MoonshineForConditionalGeneration.from_pretrained(NAME, revision=args.revision)
c = m.config
sd = {k: v.float().numpy() for k, v in m.state_dict().items()}
GS = 32

f = open(os.path.join(out, args.name + ".bin"), "wb")
D, FF = c.hidden_size, c.intermediate_size
f.write(
    struct.pack(
        "<8i",
        0x4D4F4F4E,
        2 if args.bits == 8 else 3,
        D,
        FF,
        c.encoder_num_hidden_layers,
        c.decoder_num_hidden_layers,
        c.encoder_num_attention_heads,
        c.vocab_size,
    )
)


def fp32(name, a=None):
    a = sd[name] if a is None else a
    f.write(np.ascontiguousarray(a, dtype="<f4").tobytes())


def q8(name, a=None):
    a = sd[name] if a is None else a
    a = a.reshape(a.shape[0], -1).astype(np.float32)  # (out, in)
    o, i = a.shape
    assert i % GS == 0, (name, a.shape)
    g = a.reshape(o, i // GS, GS)
    if args.bits == 8:
        s = np.abs(g).max(axis=2) / 127.0
        s[s == 0] = 1.0
        q = np.clip(np.round(g / s[..., None]), -127, 127).astype(np.int8)
        f.write(q.tobytes())
    else:
        s = np.abs(g).max(axis=2) / 7.0
        s[s == 0] = 1.0
        q = (np.clip(np.round(g / s[..., None]), -8, 7) + 8).astype(np.uint8)
        f.write((q[..., :16] | (q[..., 16:] << 4)).astype(np.uint8).tobytes())
    # scales in [out/4][in/32][4] order (what the NEON kernel loads)
    f.write(np.ascontiguousarray(s.reshape(o // 4, 4, -1).transpose(0, 2, 1)).astype("<f4").tobytes())


def conv_w(name):
    # (out, in, k) -> (out, k*in) so the input row of im2col is [t][c] (time-major)
    w = sd[name]
    return w.transpose(0, 2, 1).reshape(w.shape[0], -1)


e = "model.encoder."
fp32(e + "conv1.weight")  # (288,1,127)
fp32(e + "groupnorm.weight")
fp32(e + "groupnorm.bias")
q8(None, conv_w(e + "conv2.weight"))
fp32(e + "conv2.bias")
q8(None, conv_w(e + "conv3.weight"))
fp32(e + "conv3.bias")
for l in range(c.encoder_num_hidden_layers):
    p = f"{e}layers.{l}."
    fp32(p + "input_layernorm.weight")
    for n in "qkvo":
        q8(p + f"self_attn.{n}_proj.weight")
    fp32(p + "post_attention_layernorm.weight")
    q8(p + "mlp.fc1.weight")
    fp32(p + "mlp.fc1.bias")
    q8(p + "mlp.fc2.weight")
    fp32(p + "mlp.fc2.bias")
fp32(e + "layer_norm.weight")

d = "model.decoder."
q8(d + "embed_tokens.weight")
for l in range(c.decoder_num_hidden_layers):
    p = f"{d}layers.{l}."
    fp32(p + "input_layernorm.weight")
    for n in "qkvo":
        q8(p + f"self_attn.{n}_proj.weight")
    fp32(p + "post_attention_layernorm.weight")
    for n in "qkvo":
        q8(p + f"encoder_attn.{n}_proj.weight")
    fp32(p + "final_layernorm.weight")
    q8(p + "mlp.fc1.weight")
    fp32(p + "mlp.fc1.bias")
    q8(p + "mlp.fc2.weight")
    fp32(p + "mlp.fc2.bias")
fp32(d + "norm.weight")
f.close()

# vocab: per token, u16 length + bytes ('▁' -> ' ', <0xNN> -> byte)
tok = AutoTokenizer.from_pretrained(NAME, revision=args.revision)
with open(os.path.join(out, "moonshine-vocab.bin"), "wb") as v:
    v.write(struct.pack("<i", c.vocab_size))
    for i, t in enumerate(tok.convert_ids_to_tokens(list(range(c.vocab_size)))):
        if i < 3 or t.startswith("<<"):
            b = b""
        elif len(t) == 6 and t.startswith("<0x") and t.endswith(">"):
            b = bytes([int(t[3:5], 16)])
        else:
            b = t.replace("▁", " ").encode("utf-8")
        v.write(struct.pack("<H", len(b)) + b)
print("ok")
