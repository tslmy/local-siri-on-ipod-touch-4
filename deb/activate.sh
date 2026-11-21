#!/bin/sh
# Turns Siri on when both halves are present: this package (engine) and Apple's Siri files from the
# user's own iPhone 4S firmware (added with `just apple-files` on a Mac). Safe to run repeatedly.
S=/var/root/siri
A=/System/Library/PrivateFrameworks/AssistantUI.framework/AssistantUI
if [ ! -f "$A" ]; then
  cat <<MSG
Local Siri engine installed, but Siri itself is not on this iPod yet.
Apple's Siri files are not part of this package. On a Mac, in a checkout of
github.com/tslmy/local-siri-on-ipod-touch-4, run:
    IPOD=root@<this iPod's address> just apple-files <Xcode 4.6.3 dmg>
That copies them over and finishes the setup.
MSG
  exit 0
fi
cd /var/mobile/Library/Preferences || exit 1
for f in com.apple.assistant.plist com.apple.assistant.support.plist com.apple.springboard.plist; do
  [ -f $f ] && [ ! -f $f.bak-localsiri ] && cp $f $f.bak-localsiri; done
P=$S/ace/plistset
su mobile -c "$P com.apple.assistant \"Server Override\" string http://guzzoni.apple.com:4443" >/dev/null 2>&1
su mobile -c "$P com.apple.assistant \"Authentication Disabled\" bool 1" >/dev/null 2>&1
su mobile -c "$P com.apple.assistant.support \"Assistant Enabled\" bool 1" >/dev/null 2>&1
su mobile -c "$P com.apple.springboard SBAssistantCapabilityOverride bool 1" >/dev/null 2>&1
# Siri accepts a Server Override only if its host ends in apple.com; point that host at this iPod so
# even Siri's built-in default server can only be reached on the device itself.
[ -f /etc/hosts.bak-localsiri ] || cp /etc/hosts /etc/hosts.bak-localsiri
grep -q "guzzoni.apple.com" /etc/hosts || printf "\n# local Siri: keep Siri on this device\n127.0.0.1\tguzzoni.apple.com\n" >> /etc/hosts
AC=/var/mobile/Library/Caches/libactivator.plist
if [ -f $AC ] && grep -q com.lmy.localsiri $AC; then
  [ -f $AC.bak-localsiri ] || cp $AC $AC.bak-localsiri
  grep -v libactivator.menu.hold.short $AC > $AC.tmp && mv $AC.tmp $AC && chown mobile:mobile $AC; fi
touch $S/.installed
for d in com.lmy.localsirid com.lmy.acesrv; do
  launchctl unload /Library/LaunchDaemons/$d.plist 2>/dev/null
  launchctl load /Library/LaunchDaemons/$d.plist; done
launchctl load /System/Library/LaunchDaemons/com.apple.assistantd.plist 2>/dev/null
launchctl load /System/Library/LaunchDaemons/com.apple.assistant_service.plist 2>/dev/null
launchctl stop com.apple.assistantd 2>/dev/null
echo "Local Siri is on. The models take ~20 s to load; then hold the Home button and ask a question."
killall SpringBoard
exit 0
