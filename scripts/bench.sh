#!/bin/bash
# Benchmark the engines on the iPod outside Siri: speech recognition (Whistle, and Moonshine when its model is
# installed) on Mac-synthesized questions, then the chat model on two questions. Prints timings per stage.
source "$(dirname "$0")/lib.sh"
need_ipod
O=$BUILD/bench
rm -rf "$O" && mkdir -p "$O"
step "building the command-line engines"
cc_ios -O3 -ffast-math $NEON -DWHISTLE_MAIN "$ROOT/src/whistle/whistle.c" -lm -o "$O/whistle"
cc_ios -O3 -ffast-math $NEON -DMOONSHINE_MAIN "$ROOT/src/moonshine/moonshine.c" -lm -o "$O/moonshine"
cc_ios -O3 -ffast-math $NEON -DLLM_MAIN "$ROOT/src/llm/llm.c" -lm -o "$O/llm"
ldid -S "$O/whistle" "$O/moonshine" "$O/llm"
i=0
for q in "What is the capital of France?" "How many ounces are in a pound?" "Tell me a story about a little dog who wanted to fly."; do
  i=$((i + 1)); say -o "$O/q$i.wav" --data-format=LEI16@16000 --channels=1 "$q"
done
on_ipod 'mkdir -p /tmp/bench'
to_ipod "$O/whistle" "$O/moonshine" "$O/llm" "$O"/q*.wav /tmp/bench/
step "speech recognition"
on_ipod 'cd /tmp/bench; S=/var/root/siri
  echo "-- Whistle"; ./whistle $S/whistle.cact q*.wav 2>&1 | grep -v "^  prof"
  [ -f $S/moonshine-tiny.bin ] && { echo "-- Moonshine Tiny"; ./moonshine $S/moonshine-tiny.bin $S/moonshine-vocab.bin q*.wav 2>&1; }; true'
step "chat model"
on_ipod 'cd /tmp/bench; S=/var/root/siri; SYS="You are Siri, a helpful voice assistant on an iPod. Answer in one or two short sentences."
  for m in smollm2-135m-cq4 smollm2-135m-q4e8; do [ -f $S/$m.bin ] || continue; echo "-- $m"
    for q in "What is the capital of France?" "Who wrote Romeo and Juliet?"; do ./llm $S/$m.bin $S/smollm2-135m.tok "$q" 40 "$SYS" 2>&1 | grep -v "^load\|^time in"; done; done
  rm -rf /tmp/bench'
