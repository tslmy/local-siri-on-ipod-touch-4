"""GPTQ-style rounding for CQ (see cq.py): choose each weight's codebook index so the layer's output error on
real activations stays small, instead of rounding every weight to its nearest level independently.

Works in the rotated domain: with R the block-diagonal Walsh-Hadamard rotation, W x = (W R)(R x), so GPTQ runs on
W R with the Hessian of R x. A group's norm is taken from the (error-compensated) weights when GPTQ reaches it.
Calibration: chat-formatted questions (disjoint from the evaluation set) answered by the full-precision model.
"""

import numpy as np
import torch

import cq

CALIB_QUESTIONS = [
    "What is the largest ocean on Earth?", "Who painted the Mona Lisa?", "How many legs does a spider have?",
    "What is the boiling point of water?", "Name a primary color.", "What language is spoken in Brazil?",
    "How many days are in a week?", "What is the chemical symbol for gold?", "Who was the first president of "
    "the United States?", "What do bees make?", "How many continents are there?", "What is the square root of "
    "nine?", "What is the freezing point of water in Fahrenheit?", "Which planet is known as the red planet?",
    "What is the longest river in Africa?", "Set an alarm for seven in the morning.", "What's the weather like "
    "today?", "Tell me something interesting.", "How do I make a cup of tea?", "What time is it in London?",
    "Who discovered penicillin?", "What is photosynthesis?", "How many minutes are in an hour?",
    "Give me a fun fact about cats.", "What is the speed of light?", "Who invented the telephone?",
    "What is the capital of Italy?", "What is the capital of Canada?", "How many centimeters are in a meter?",
    "Why do leaves change color in autumn?", "What is a haiku?", "Recommend a good book.",
    "How many strings does a guitar have?", "What is the opposite of hot?", "Tell me a riddle.",
    "Spell the word necessary.", "What is ten times ten?", "Where do penguins live?",
    "What is the smallest planet?", "How do airplanes fly?",
]


def calibration(model, tok, system, max_new=40):
    seqs = []
    for q in CALIB_QUESTIONS:
        msgs = [{"role": "system", "content": system}, {"role": "user", "content": q}]
        p = tok.apply_chat_template(msgs, add_generation_prompt=True, return_tensors="pt", return_dict=True)["input_ids"]
        with torch.no_grad():
            out = model.generate(p, max_new_tokens=max_new, do_sample=False, repetition_penalty=1.1)
        seqs.append(out)
    return seqs


def collect_hessians(model, seqs, names):
    """Sum of x x^T over all calibration tokens for the input of each named nn.Linear."""
    hs, hooks = {}, []
    mods = dict(model.named_modules())
    for n in names:
        lin = mods[n]
        hs[n] = torch.zeros(lin.in_features, lin.in_features, dtype=torch.float64)

        def hook(_m, inp, _out, n=n):
            x = inp[0].reshape(-1, inp[0].shape[-1]).double()
            hs[n] += x.T @ x

        hooks.append(lin.register_forward_hook(hook))
    with torch.no_grad():
        for s in seqs:
            model(s)
    for h in hooks:
        h.remove()
    return hs


def rotation(n, g):
    h = torch.from_numpy(cq.hadamard(g)).double()
    return torch.block_diag(*([h] * (n // g)))


def gptq_cq(w, hess, bits, g, damp=0.01):
    """w [out, in] float32 numpy, hess [in, in] (of the unrotated input) -> (idx, norms) like cq.quantize."""
    out, n = w.shape
    R = rotation(n, g)
    W = torch.from_numpy(w).double() @ R          # rotated weights; columns are rotated coordinates
    Hr = R @ hess @ R                             # R symmetric
    dead = torch.diag(Hr) == 0
    Hr[dead, dead] = 1
    Hr += damp * torch.mean(torch.diag(Hr)) * torch.eye(n, dtype=torch.float64)
    Hinv = torch.linalg.cholesky(torch.cholesky_inverse(torch.linalg.cholesky(Hr)), upper=True)
    cb = torch.from_numpy(cq.lloyd_max_gaussian(bits) / np.sqrt(g)).double()
    bnd = (cb[:-1] + cb[1:]) / 2
    idx = torch.zeros(out, n, dtype=torch.uint8)
    norms = torch.zeros(out, n // g, dtype=torch.float64)
    for i1 in range(0, n, g):                     # one block per CQ group (lazy-batch GPTQ)
        i2 = i1 + g
        W1, Hinv1 = W[:, i1:i2].clone(), Hinv[i1:i2, i1:i2]
        Err1 = torch.zeros_like(W1)
        nrm = W1.norm(dim=1).clamp_min(1e-12)
        norms[:, i1 // g] = nrm
        for i in range(g):
            col = W1[:, i]
            k = torch.bucketize(col / nrm, bnd)
            q = cb[k] * nrm
            idx[:, i1 + i] = k.to(torch.uint8)
            err = (col - q) / Hinv1[i, i]
            W1[:, i:] -= err[:, None] * Hinv1[i, i:][None, :]
            Err1[:, i] = err
        W[:, i2:] -= Err1 @ Hinv[i1:i2, i2:]
    return idx.numpy(), norms.float().numpy()


def quantize_model(model, tok, system, bits=4, g=64):
    """{state_dict name: (idx, norms)} for every attention and MLP projection; the embedding stays plain CQ."""
    names = [n for n, m in model.named_modules() if isinstance(m, torch.nn.Linear) and ("self_attn" in n or "mlp" in n)]
    seqs = calibration(model, tok, system)
    hs = collect_hessians(model, seqs, names)
    out = {}
    mods = dict(model.named_modules())
    for n in names:
        w = mods[n].weight.detach().float().numpy()
        out[n + ".weight"] = gptq_cq(w, hs[n], bits, g)
    return out
