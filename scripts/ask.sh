#!/bin/bash
# End-to-end test through the real Siri UI: open Siri on the iPod (via Activator), let this Mac speak
# the question out loud, then print what Siri heard and answered and save a screenshot.
# Put the iPod within earshot of the Mac and turn the Mac's volume up.
#   scripts/ask.sh "What is the capital of France?"
source "$(dirname "$0")/lib.sh"
Q=${1:-What is the capital of France?}
on_ipod 'command -v activator >/dev/null || { echo "ask needs Activator (to open Siri remotely)"; exit 1; }
  activator send libactivator.lockscreen.show; activator send libactivator.system.homebutton; sleep 1.5
  wc -l < /var/root/siri/ace/acesrv.log > /tmp/ask-start
  activator send libactivator.system.virtual-assistant; sleep 2.5'
say "$Q"
printf 'waiting for the answer'
for _ in $(seq 1 40); do
  sleep 2; printf .
  on_ipod 'tail -n +$(cat /tmp/ask-start) /var/root/siri/ace/acesrv.log | grep -q "RequestCompleted\|SpeechFailure"' && break
done
echo
on_ipod 'tail -n +$(cat /tmp/ask-start) /var/root/siri/ace/acesrv.log | grep -E "speech finish|heard \"" | sed "s/^\[acesrv [0-9.]*\] //"
  grep -E "q=" /var/root/siri/localsirid.log | tail -1 | sed "s/^\[localsirid\] //"
  sleep 3; /var/root/siri/screenshot /tmp/ask.png'
mkdir -p "$BUILD/screenshots"
SHOT=$BUILD/screenshots/ask-$(date +%H%M%S).png
from_ipod /tmp/ask.png "$SHOT" && echo "screenshot: $SHOT"
