#!/bin/bash
# Download the models from Hugging Face (pinned revisions) and convert them to the formats the C engines read.
# Output in build/models (about 95 MB):
#   Whistle (Cactus Compute, speech recognition, Apache-2.0) -> whistle.cact, used as published (2/4-bit CQ)
#   SmolLM2-135M-Instruct (answers, Apache-2.0)               -> smollm2-135m-cq4.bin (4-bit CQ, GPTQ-rounded),
#                                                                smollm2-135m.tok
# LEGACY_MODELS=1 also builds the previous pair (Moonshine Tiny, SmolLM2 Q4/Q8, about 130 MB more), which
# localsirid falls back to when the new files are missing (or LOCALSIRI_ASR=moonshine).
source "$(dirname "$0")/lib.sh"
M=$BUILD/models
mkdir -p "$M"

WHISTLE_REV=88cb00eeb8928ddbcd410b491584f938ba030b07
WHISTLE_SHA=b6e02f048568ac5d01a2042556c658061e699acbc0aa2a1439f52f3d461dffeb
if [ ! -f "$M/whistle.cact" ]; then
  step "downloading Whistle"
  curl -fL --retry 3 -o "$M/whistle.cact.part" "https://huggingface.co/Cactus-Compute/whistle/resolve/$WHISTLE_REV/whistle.cact"
  echo "$WHISTLE_SHA  $M/whistle.cact.part" | shasum -a 256 -c - >/dev/null || { echo "whistle.cact: checksum mismatch" >&2; exit 1; }
  mv "$M/whistle.cact.part" "$M/whistle.cact"
fi
if [ ! -f "$M/smollm2-135m-cq4.bin" ]; then
  step "converting SmolLM2-135M-Instruct (4-bit CQ with GPTQ rounding, a minute or two)"
  uv run --project "$ROOT" python "$ROOT/convert/llm_convert.py" --cq 4 --gptq --out "$M/smollm2-135m-cq4"
  mv "$M/smollm2-135m-cq4.tok" "$M/smollm2-135m.tok"
fi
if [ -n "${LEGACY_MODELS:-}" ]; then
  if [ ! -f "$M/moonshine-tiny.bin" ]; then
    step "converting Moonshine Tiny"
    uv run --project "$ROOT" python "$ROOT/convert/moonshine_convert.py" "$M"
  fi
  if [ ! -f "$M/smollm2-135m-q4e8.bin" ]; then
    step "converting SmolLM2-135M-Instruct (Q4 layers, Q8 embedding)"
    uv run --project "$ROOT" python "$ROOT/convert/llm_convert.py" --wbits 4 --ebits 8 --out "$M/smollm2-135m-q4e8"
    rm -f "$M/smollm2-135m-q4e8.tok"
  fi
fi
ls -l "$M"
