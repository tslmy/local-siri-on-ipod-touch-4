#!/bin/bash
# Download only the root filesystem of the iPhone 4S iOS 6.1.3 restore image from Apple, decrypt it
# with its published key (The Apple Wiki, "Keys:BrightonMaps 10B329 (iPhone4,1)") and mount it
# read-only at build/fw/mnt. About 925 MB to download and 1.8 GB on disk.
# Apple's files stay on your machine (build/ is git-ignored); nothing from them is in this repo.
source "$(dirname "$0")/lib.sh"
IPSW_URL="https://secure-appldnld.apple.com/iOS6.1/091-2611.20130319.Fr54r/iPhone4,1_6.1.3_10B329_Restore.ipsw"
ROOTFS=048-2613-005.dmg
KEY=d4685a3c01a0b6f762350191d98b8964d4c7af349aa84bacef828be9683514ccf44a8426
FW=$BUILD/fw
mkdir -p "$FW"
if [ ! -f "$FW/$ROOTFS" ]; then
  step "downloading $ROOTFS (only that member of the IPSW)"
  uv run --project "$ROOT" python -c "
from remotezip import RemoteZip
RemoteZip('$IPSW_URL').extract('$ROOTFS', '$FW')"
fi
if [ ! -f "$FW/rootfs.dmg" ]; then
  step "decrypting"
  uv run --project "$ROOT" python "$ROOT/port/vfdecrypt.py" "$FW/$ROOTFS" "$FW/rootfs.dmg.part" "$KEY"
  mv "$FW/rootfs.dmg.part" "$FW/rootfs.dmg"
fi
mkdir -p "$FW/mnt"
mount | grep -q " on $FW/mnt " || hdiutil attach -readonly -nobrowse -mountpoint "$FW/mnt" "$FW/rootfs.dmg" 2>/dev/null >/dev/null
test -f "$FW/mnt/System/Library/Caches/com.apple.dyld/dyld_shared_cache_armv7"
echo "iPhone 4S root filesystem mounted at $FW/mnt"
