#!/bin/bash
# Check prerequisites on the Mac and, if IPOD is set, on the iPod.
source "$(dirname "$0")/lib.sh"
set +e
ok=1
check() { if eval "$2" >/dev/null 2>&1; then printf '  ok    %s\n' "$1"; else printf '  MISSING  %s  (%s)\n' "$1" "$3"; ok=0; fi; }
echo "Mac:"
check "macOS"                    '[ "$(uname)" = Darwin ]'                       "needs macOS (hdiutil, Rosetta)"
check "Rosetta 2"                'arch -x86_64 /usr/bin/true'                    "softwareupdate --install-rosetta"
check "Homebrew"                 'command -v brew'                               "https://brew.sh"
check "just"                     'command -v just'                               "brew install just"
check "uv"                       'command -v uv'                                 "brew install uv"
check "ldid"                     'command -v ldid'                               "brew install ldid"
check "boost"                    '[ -d "$(brew --prefix)/include/boost" ]'      "brew install boost"
check "git, curl"                'command -v git && command -v curl'            "Xcode command line tools"
check "Xcode 4.6.3 toolchain"    '[ -x "$XCODE_TOOLCHAIN/usr/bin/clang" ]'       "just toolchain <xcode_4.6.3.dmg>"
check "iOS 6.1 SDK"              '[ -d "$IOS_SDK/System" ]'                      "just toolchain <xcode_4.6.3.dmg>"
if [ -n "${IPOD:-}" ]; then
  echo "iPod ($IPOD):"
  check "SSH login"              'on_ipod true'                                  "OpenSSH from Cydia, and your key in /var/root/.ssh/authorized_keys"
  check "iPod touch 4G"          '[ "$(on_ipod sysctl -n hw.machine)" = iPod4,1 ]' "this project targets iPod4,1"
  check "Cydia Substrate"        'on_ipod test -f /Library/MobileSubstrate/MobileSubstrate.dylib' "install Cydia Substrate (MobileSubstrate) from Cydia"
  check "Activator (for just ask)" 'on_ipod command -v activator'               "optional: install Activator from BigBoss"
fi
[ $ok = 1 ] && echo "all good" || exit 1
