#!/bin/bash
# Add Apple's Siri files (from YOUR copy of the iPhone 4S iOS 6.1.3 firmware) to an iPod that already
# has the com.lmy.local-siri package, then switch Siri on. The files stay on your machines: never share them.
#   IPOD=root@192.168.1.23 just apple-files ~/Downloads/xcode4630916281a.dmg
source "$(dirname "$0")/lib.sh"
need_ipod
bash "$ROOT/scripts/toolchain.sh" "$1"
bash "$ROOT/scripts/firmware.sh"
bash "$ROOT/scripts/assistantui.sh"
FW=$BUILD/fw/mnt
step "checking the iPod"
on_ipod '[ "$(sysctl -n hw.machine)" = iPod4,1 ] || { echo "this is not an iPod touch 4G"; exit 1; }
  [ -x /var/root/siri/activate.sh ] || { echo "install the com.lmy.local-siri package from Cydia first"; exit 1; }'
step "staging and copying Siri files"
STAGE=$BUILD/stage
rm -rf "$STAGE" && mkdir -p "$STAGE"
grep -v '^#' "$ROOT/port/siri-files.txt" | grep . | while IFS= read -r p; do
  mkdir -p "$STAGE/$(dirname "$p")"; cp -Rp "$FW/$p" "$STAGE/$p"
done
cp "$BUILD/port/AssistantUI" "$STAGE/System/Library/PrivateFrameworks/AssistantUI.framework/AssistantUI"
COPYFILE_DISABLE=1 tar --no-xattrs -C "$STAGE" -czf "$BUILD/siri-files.tgz" .
to_ipod "$BUILD/siri-files.tgz" /var/root/
on_ipod 'set -e
  tar -C / -xzf /var/root/siri-files.tgz --no-same-owner --keep-directory-symlink --no-overwrite-dir
  rm /var/root/siri-files.tgz
  chown -R root:wheel /System/Library/PrivateFrameworks/Assistant*.framework /System/Library/PrivateFrameworks/SAObjects.framework \
    /System/Library/SpringBoardPlugins/Assistant.uibundle /System/Library/Assistant /System/Library/PreferenceBundles/Assistant.bundle \
    /System/Library/UserEventPlugins/AssistantUEA.plugin /System/Library/LaunchDaemons/com.apple.assistant*.plist
  /var/root/siri/activate.sh'
