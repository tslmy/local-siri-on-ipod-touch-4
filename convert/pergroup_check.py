"""Is each rotated group of SmolLM2 bell-shaped? Per-group kurtosis percentiles, and the error of the shared Gaussian codebook against
each group's own (in-sample) best 16 levels. Run: uv run --no-project --with numpy python convert/pergroup_check.py"""

import sys, os, json, struct, glob
import numpy as np
sys.path.insert(0, 'convert')
from cq import hadamard, lloyd_max_gaussian
PATH = glob.glob(os.path.expanduser("~/.cache/huggingface/hub/models--HuggingFaceTB--SmolLM2-135M-Instruct/snapshots/*/model.safetensors"))[0]
def load(path):
    with open(path,'rb') as f:
        n=struct.unpack('<Q',f.read(8))[0]; head=json.loads(f.read(n)); base=8+n
        for k,v in head.items():
            if k=='__metadata__' or len(v['shape'])!=2: continue
            a,b=v['data_offsets']; f.seek(base+a); raw=np.frombuffer(f.read(b-a),dtype=np.uint16)
            yield k,(raw.astype(np.uint32)<<16).view(np.float32).reshape(v['shape'])
rng=np.random.default_rng(0)
G=64; H=hadamard(G).astype(np.float64).T
chunks=[]
for name,W in load(PATH):
    if 'embed' in name: kind='embedding'
    elif 'self_attn' in name: kind='attention'
    elif 'mlp' in name: kind='mlp'
    else: continue
    U=W.astype(np.float64).reshape(-1,G)
    idx=rng.choice(len(U),size=min(len(U),600),replace=False)
    chunks.append((kind,U[idx]))
res={}
for kind in ('attention','mlp','embedding'):
    U=np.concatenate([c for k,c in chunks if k==kind])
    U=U/np.sqrt((U**2).mean(1,keepdims=True))   # unit rms
    R=U@H
    out={}
    for label,X in (('unrotated',U),('rotated',R)):
        cb=lloyd_max_gaussian(4).astype(np.float64)       # unit-sigma levels, shared
        q=cb[np.abs(X[...,None]-cb).argmin(-1)]
        shared=np.sqrt(((X-q)**2).sum()/ (X**2).sum())
        # per-group Lloyd (16 levels), initialised from the Gaussian levels, 20 iterations
        C=np.tile(cb,(len(X),1))
        for _ in range(20):
            idx=np.abs(X[...,None]-C[:,None,:]).argmin(-1)
            for k in range(16):
                m=(idx==k); cnt=m.sum(1); s=(X*m).sum(1)
                C[:,k]=np.where(cnt>0,s/np.maximum(cnt,1),C[:,k])
        idx=np.abs(X[...,None]-C[:,None,:]).argmin(-1)
        qg=np.take_along_axis(C,idx,1)
        own=np.sqrt(((X-qg)**2).sum()/(X**2).sum())
        # per-group kurtosis
        k4=(X**4).mean(1)
        out[label]=dict(shared=round(shared*100,2),own=round(own*100,2),kurt_med=round(float(np.median(k4)),2),kurt_p10=round(float(np.percentile(k4,10)),2),kurt_p90=round(float(np.percentile(k4,90)),2))
    res[kind]=out
print(json.dumps(res,indent=1))
