"""Compare weight-storage schemes for the chat model on the Mac before building any of them for the iPod.

For each scheme the weights are quantized and dequantized in place (simulation), then we measure perplexity on a
fixed text and greedy answers to a set of Siri-style questions. Usage:

    uv run convert/llm_quant_eval.py [--schemes q4e8,cq4,...]
"""

import argparse
import sys
from pathlib import Path

import numpy as np
import torch
from transformers import AutoModelForCausalLM, AutoTokenizer

sys.path.insert(0, str(Path(__file__).parent))
import cq  # noqa: E402

MODEL, REV = "HuggingFaceTB/SmolLM2-135M-Instruct", "12fd25f77366fa6b3b4b768ec3050bf629380bac"
SYSTEM = "You are Siri, a helpful voice assistant on an iPod. Answer in one or two short sentences."
QUESTIONS = [
    "What is the capital of France?",
    "What is the capital of Japan?",
    "How many ounces are in a pound?",
    "Who wrote Romeo and Juliet?",
    "How far away is the moon?",
    "What is the tallest mountain in the world?",
    "Tell me a joke.",
    "What is two plus two?",
    "Why is the sky blue?",
    "Tell me a story about a little dog who wanted to fly.",
]
TEXT = (
    "The Moon is Earth's only natural satellite. It orbits at an average distance of 384,400 km, about 30 times "
    "the diameter of Earth. Tidal forces between Earth and the Moon have synchronized the Moon's orbital period "
    "with its rotation period at 27.3 days, so the same side of the Moon always faces Earth. Paris is the capital "
    "and largest city of France. With an estimated population of over two million residents, it is the centre of "
    "the Ile-de-France region. William Shakespeare was an English playwright, poet and actor. He is widely "
    "regarded as the greatest writer in the English language and the world's pre-eminent dramatist. A pound is "
    "a unit of mass equal to sixteen ounces. The sky looks blue because air molecules scatter short wavelengths "
    "of sunlight more strongly than long ones, an effect called Rayleigh scattering."
)


def q4_0(w):  # what llm.c ships today: absmax per 32, 16 levels
    o, i = w.shape
    g = w.reshape(o, i // 32, 32)
    s = np.abs(g).max(2, keepdims=True) / 7.0
    s[s == 0] = 1
    return (np.clip(np.round(g / s), -8, 7) * s).reshape(o, i)


def q8_0(w):
    o, i = w.shape
    g = w.reshape(o, i // 32, 32)
    s = np.abs(g).max(2, keepdims=True) / 127.0
    s[s == 0] = 1
    return (np.clip(np.round(g / s), -127, 127) * s).reshape(o, i)


# scheme -> (embedding fn, attention fn, mlp fn); None keeps fp32. CQ norms are float32 per 64 (+0.5 bit).
def cqf(bits, g=64):
    return lambda w: cq.fake(w, bits, g)


SCHEMES = {
    "fp32": (None, None, None),
    "q4e8": (q8_0, q4_0, q4_0),
    "cq4": (cqf(4), cqf(4), cqf(4)),
    "cq3m": (cqf(4), cqf(4), cqf(3)),
    "cq3": (cqf(3), cqf(3), cqf(3)),
    "cq3e4": (cqf(4), cqf(3), cqf(3)),
    "cq2m": (cqf(4), cqf(4), cqf(2)),
    "cq4-emb-only": (cqf(4), None, None),
    "cq4-layers-only": (None, cqf(4), cqf(4)),
    "cq4-e8": (q8_0, cqf(4), cqf(4)),
    "cq4-attn-only": (cqf(4), cqf(4), None),
    "cq4-mlp-only": (cqf(4), None, cqf(4)),
    "cq4-g32": (cqf(4), cqf(4, 32), cqf(4, 32)),
    "q8attn-cq4mlp": (cqf(4), q8_0, cqf(4)),
}


def bits_of(name, n_emb, n_att, n_mlp):
    table = {
        "fp32": (32, 32, 32),
        "q4e8": (8 + 1, 4 + 1, 4 + 1),
        "cq4": (4.5, 4.5, 4.5),
        "cq3m": (4.5, 4.5, 3.5),
        "cq3": (3.5, 3.5, 3.5),
        "cq3e4": (4.5, 3.5, 3.5),
        "cq2m": (4.5, 4.5, 2.5),
        "cq4-emb-only": (4.5, 32, 32),
        "cq4-layers-only": (32, 4.5, 4.5),
        "cq4-e8": (9, 4.5, 4.5),
        "cq4-attn-only": (4.5, 4.5, 32),
        "cq4-mlp-only": (4.5, 32, 4.5),
        "cq4-g32": (4.5, 5, 5),
        "q8attn-cq4mlp": (4.5, 9, 4.5),
        "cq4-gptq": (4.5, 4.5, 4.5),
        "cq4-gptq-a8": (4.5, 4.5, 4.5),
        "fp32-a8": (32, 32, 32),
    }
    e, a, m = table[name]
    return (n_emb * e + n_att * a + n_mlp * m) / 8 / 1e6


ACT_Q = [False]


def add_act_quant(model, g=64):
    """What the C kernels do to a matmul's input: rotate each group of 64, int8 absmax per group."""
    h = torch.from_numpy(cq.hadamard(g))

    def pre(_m, inp):
        if not ACT_Q[0]:
            return None
        x = inp[0]
        sh = x.shape
        r = x.reshape(*sh[:-1], sh[-1] // g, g) @ h
        s = r.abs().amax(-1, keepdim=True).clamp_min(1e-12) / 127
        r = torch.round(r / s) * s
        return (((r @ h).reshape(sh)),)

    for m in model.modules():
        if isinstance(m, torch.nn.Linear):
            m.register_forward_pre_hook(pre)


def apply_gptq(model, base, tok, bits=4, g=64):
    import cq_gptq

    apply(model, base, "fp32")
    q = cq_gptq.quantize_model(model, tok, SYSTEM, bits, g)
    sd = model.state_dict()
    with torch.no_grad():
        for k, (idx, norms) in q.items():
            sd[k].copy_(torch.from_numpy(cq.dequantize(idx, norms, bits, g)))
        sd["model.embed_tokens.weight"].copy_(torch.from_numpy(cq.fake(base["model.embed_tokens.weight"], bits, g)))


def apply(model, base, scheme):
    emb, att, mlp = SCHEMES[scheme]
    sd = model.state_dict()
    with torch.no_grad():
        for k, v in base.items():
            fn = None
            if k == "model.embed_tokens.weight":
                fn = emb
            elif "self_attn" in k and k.endswith("proj.weight"):
                fn = att
            elif "mlp" in k and k.endswith("proj.weight"):
                fn = mlp
            sd[k].copy_(torch.from_numpy(fn(v) if fn else v))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--schemes", default=",".join(SCHEMES))
    args = ap.parse_args()
    tok = AutoTokenizer.from_pretrained(MODEL, revision=REV)
    model = AutoModelForCausalLM.from_pretrained(MODEL, revision=REV, torch_dtype=torch.float32).eval()
    base = {k: v.float().numpy().copy() for k, v in model.state_dict().items()}
    n_emb = base["model.embed_tokens.weight"].size
    n_att = sum(v.size for k, v in base.items() if "self_attn" in k and k.endswith("proj.weight"))
    n_mlp = sum(v.size for k, v in base.items() if "mlp" in k and k.endswith("proj.weight"))
    ids = tok(TEXT, return_tensors="pt").input_ids
    SCHEMES.setdefault("cq4-gptq", (None, None, None))
    SCHEMES.setdefault("cq4-gptq-a8", (None, None, None))
    SCHEMES.setdefault("fp32-a8", (None, None, None))
    if any(x.endswith("-a8") for x in args.schemes.split(",")):
        add_act_quant(model)
    for scheme in args.schemes.split(","):
        ACT_Q[0] = scheme.endswith("-a8")
        if scheme.startswith("cq4-gptq"):
            apply_gptq(model, base, tok)
        else:
            apply(model, base, scheme)
        with torch.no_grad():
            ppl = torch.exp(model(ids, labels=ids).loss).item()
        print(f"== {scheme}: ~{bits_of(scheme, n_emb, n_att, n_mlp):.0f} MB, perplexity {ppl:.2f}", flush=True)
        for q in QUESTIONS:
            msgs = [{"role": "system", "content": SYSTEM}, {"role": "user", "content": q}]
            p = tok.apply_chat_template(msgs, add_generation_prompt=True, return_tensors="pt", return_dict=True)[
                "input_ids"
            ]
            with torch.no_grad():
                out = model.generate(p, max_new_tokens=40, do_sample=False, repetition_penalty=1.1)
            ans = tok.decode(out[0, p.shape[1] :], skip_special_tokens=True).strip().replace("\n", " ")
            print(f"   {q} -> {ans}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
