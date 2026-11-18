#!/bin/bash
# Extract the cross toolchain from Apple's Xcode 4.6.3 disk image (you download it yourself from
# https://developer.apple.com/download/all/?q=xcode%204.6.3 with a free Apple ID):
#   .toolchain/XcodeDefault.xctoolchain   clang 4.2 + ld64 that still target armv7 / iOS 6
#   .toolchain/iPhoneOS6.1.sdk            headers and stub libraries for iOS 6.1
#   scripts/toolchain.sh ~/Downloads/xcode4630916281a.dmg
source "$(dirname "$0")/lib.sh"
DMG=${1:?usage: toolchain.sh path/to/xcode_4.6.3.dmg}
if [ -x "$XCODE_TOOLCHAIN/usr/bin/clang" ] && [ -d "$IOS_SDK" ]; then echo "toolchain already in place"; exit 0; fi
arch -x86_64 /usr/bin/true 2>/dev/null || { echo "Rosetta 2 is needed: softwareupdate --install-rosetta" >&2; exit 1; }
MNT=$(mktemp -d)
hdiutil attach -readonly -nobrowse -mountpoint "$MNT" "$DMG" >/dev/null
trap 'hdiutil detach -quiet "$MNT"' EXIT
DEV=$MNT/Xcode.app/Contents/Developer
mkdir -p "$ROOT/.toolchain"
step "copying XcodeDefault.xctoolchain"
ditto "$DEV/Toolchains/XcodeDefault.xctoolchain" "$ROOT/.toolchain/XcodeDefault.xctoolchain"
step "copying iPhoneOS6.1.sdk"
ditto "$DEV/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS6.1.sdk" "$ROOT/.toolchain/iPhoneOS6.1.sdk"
"$ROOT/.toolchain/XcodeDefault.xctoolchain/usr/bin/clang" --version | head -1
