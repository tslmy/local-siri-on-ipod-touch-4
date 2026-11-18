#!/bin/bash
# pre-commit guard: refuse Mach-O binaries, disk images and model files. Apple's code (firmware,
# frameworks, the decached AssistantUI) and the models must never end up in this repository.
bad=0
for f in "$@"; do
  case "$f" in *.dmg|*.ipsw|*.bin|*.tok|*.dylib|*.a) echo "refusing to commit $f"; bad=1; continue;; esac
  if [ -f "$f" ] && head -c 4 "$f" | xxd -p | grep -qE '^(feedface|cefaedfe|feedfacf|cffaedfe|cafebabe)$'; then
    echo "refusing to commit Mach-O binary $f"; bad=1
  fi
done
exit $bad
