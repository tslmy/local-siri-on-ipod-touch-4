#!/bin/bash
# Extract AssistantUI.framework/AssistantUI, the one Siri binary the iPod's own dyld shared cache
# lacks, from the iPhone 4S cache mounted by firmware.sh, and make it loadable on the iPod.
#  1. kennytm's dyld_decache (GPLv3, https://github.com/kennytm/Miscellaneous), built from source
#     at a pinned commit, with a one-line fix: that commit moved segment collection into
#     prepare_for_save(), which the decacher never calls, so every dump came out empty.
#  2. port/fixdylib.py: rebuild what the cache builder stripped (see that file).
# Output: build/port/AssistantUI (Apple's code: keep it to yourself).
source "$(dirname "$0")/lib.sh"
CACHE=$BUILD/fw/mnt/System/Library/Caches/com.apple.dyld/dyld_shared_cache_armv7
[ -f "$CACHE" ] || { echo "run 'just firmware' first" >&2; exit 1; }
SRC=$BUILD/kennytm
OUT=$BUILD/port
mkdir -p "$OUT"
if [ ! -x "$BUILD/dyld_decache" ]; then
  step "building dyld_decache"
  [ -d "$SRC" ] || git clone -q https://github.com/kennytm/Miscellaneous "$SRC"
  git -C "$SRC" checkout -q 82a812fe8d6b50ee9d471b88be40a9a69b91660d
  grep -q 'this->prepare_for_save();' "$SRC/dyld_decache.cpp" || sed -i '' \
    's|memset(&_new_linkedit_offsets, 0, sizeof(_new_linkedit_offsets));|& this->prepare_for_save();|' "$SRC/dyld_decache.cpp"
  BREW=$(brew --prefix)
  clang++ -std=c++11 -Wno-c++11-narrowing -w -O2 -I"$BREW/include" -I"$SRC" \
    "$SRC/dyld_decache.cpp" "$SRC/DataFile.cpp" -L"$BREW/lib" -lboost_filesystem -o "$BUILD/dyld_decache"
fi
step "extracting AssistantUI"
rm -rf "$BUILD/decache"
"$BUILD/dyld_decache" -o "$BUILD/decache" -f AssistantUI "$CACHE" | tail -1
uv run --project "$ROOT" python "$ROOT/port/fixdylib.py" "$CACHE" \
  "$BUILD/decache/System/Library/PrivateFrameworks/AssistantUI.framework/AssistantUI" "$OUT/AssistantUI"
otool -hv "$OUT/AssistantUI" | tail -1
