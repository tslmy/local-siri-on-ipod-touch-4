# shellcheck shell=bash disable=SC2034
# Shared settings for the scripts. Source it; don't run it.
#
#   IPOD       ssh target of the iPod, e.g. root@192.168.1.23 (needed by deploy steps)
#   IPOD_KEY   optional private key for that ssh login
#   XCODE_TOOLCHAIN / IOS_SDK   override the toolchain and SDK that `just toolchain` extracts
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BUILD=$ROOT/build
XCODE_TOOLCHAIN=${XCODE_TOOLCHAIN:-$ROOT/.toolchain/XcodeDefault.xctoolchain}
IOS_SDK=${IOS_SDK:-$ROOT/.toolchain/iPhoneOS6.1.sdk}

# Cross compiler: Xcode 4.6's clang (an x86_64 binary; runs under Rosetta on Apple silicon)
# with the iOS 6.1 SDK. Newer Apple toolchains can no longer target armv7 / iOS 6.
cc_ios() {
  "$XCODE_TOOLCHAIN/usr/bin/clang" -arch armv7 -miphoneos-version-min=6.1 -isysroot "$IOS_SDK" "$@"
}
NEON="-mcpu=cortex-a8 -mfpu=neon"

SSH_OPTS=(-o LogLevel=ERROR -o ConnectTimeout=10)
if [ -n "${IPOD_KEY:-}" ]; then SSH_OPTS+=(-o IdentitiesOnly=yes -i "$IPOD_KEY"); fi
need_ipod() { [ -n "${IPOD:-}" ] || { echo "set IPOD=root@<your iPod's address> (see README)" >&2; exit 1; }; }
on_ipod() { need_ipod; ssh "${SSH_OPTS[@]}" "$IPOD" "$@"; }
# to_ipod <local files...> <remote path>   (copies onto the iPod)
to_ipod() { need_ipod; local dst=${*: -1}; scp -q "${SSH_OPTS[@]}" "${@:1:$#-1}" "$IPOD:$dst"; }
from_ipod() { need_ipod; scp -q "${SSH_OPTS[@]}" "$IPOD:$1" "$2"; }

step() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
