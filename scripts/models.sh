#!/bin/bash
# Download the two models from Hugging Face (pinned revisions) and convert them to the formats the
# C engines read. Output in build/models (about 130 MB).
#   Moonshine Tiny (speech recognition, MIT)        -> moonshine-tiny.bin (Q8), moonshine-vocab.bin
#   SmolLM2-135M-Instruct (answers, Apache-2.0)     -> smollm2-135m-q4e8.bin (Q4 layers, Q8 embedding),
#                                                      smollm2-135m.tok
source "$(dirname "$0")/lib.sh"
M=$BUILD/models
mkdir -p "$M"
if [ ! -f "$M/moonshine-tiny.bin" ]; then
  step "converting Moonshine Tiny"
  uv run --project "$ROOT" python "$ROOT/convert/moonshine_convert.py" "$M"
fi
if [ ! -f "$M/smollm2-135m-q4e8.bin" ]; then
  step "converting SmolLM2-135M-Instruct"
  uv run --project "$ROOT" python "$ROOT/convert/llm_convert.py" --wbits 4 --ebits 8 --out "$M/smollm2-135m-q4e8"
  mv "$M/smollm2-135m-q4e8.tok" "$M/smollm2-135m.tok"
fi
ls -l "$M"
