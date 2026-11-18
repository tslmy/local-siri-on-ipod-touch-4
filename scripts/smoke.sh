#!/bin/bash
# Quick check of the on-device brain without Siri: synthesize a question on the Mac, send the audio
# to localsirid on the iPod and print the transcript, the answer and the timings.
source "$(dirname "$0")/lib.sh"
Q=${1:-What is the capital of France?}
mkdir -p "$BUILD"
say -o "$BUILD/smoke.wav" --data-format=LEI16@16000 --channels=1 "$Q"
to_ipod "$BUILD/smoke.wav" /tmp/smoke.wav
on_ipod '/var/root/siri/sirictl /tmp/smoke.wav >/dev/null; grep -E "heard|q=" /var/root/siri/localsirid.log | tail -1'
