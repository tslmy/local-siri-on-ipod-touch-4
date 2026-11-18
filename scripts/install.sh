#!/bin/bash
# Install (or update) local Siri on the iPod over SSH. Safe to re-run.
#   - Siri's files from your iPhone 4S firmware copy + the extracted AssistantUI
#   - localsirid (speech recognition + LLM) with its models, acesrv (ACE server), the tweak
#   - preferences: Siri enabled, Server Override http://guzzoni.apple.com:4443, no authentication
#   - /etc/hosts: guzzoni.apple.com -> 127.0.0.1, so Siri can only ever reach this iPod
# Every file it edits is backed up once as <file>.bak-localsiri. `just uninstall` reverses it.
source "$(dirname "$0")/lib.sh"
FW=$BUILD/fw/mnt
for f in "$BUILD/port/AssistantUI" "$BUILD/ipod/acesrv" "$BUILD/models/smollm2-135m-q4e8.bin"; do
  [ -e "$f" ] || { echo "missing $f: run 'just setup' first" >&2; exit 1; }
done
[ -d "$FW/System" ] || bash "$ROOT/scripts/firmware.sh" >/dev/null

step "checking the iPod"
on_ipod 'set -e
  v=$(sysctl -n kern.osversion 2>/dev/null || true); m=$(sysctl -n hw.machine)
  echo "model $m, build $v"
  [ "$m" = iPod4,1 ] || { echo "this is not an iPod touch 4G"; exit 1; }
  [ -f /Library/MobileSubstrate/MobileSubstrate.dylib ] || { echo "Cydia Substrate (MobileSubstrate) is not installed"; exit 1; }
  if [ -e /System/Library/PrivateFrameworks/AssistantUI.framework ] && [ ! -f /var/root/siri/.installed ]; then
    echo "Siri files already exist and were not installed by this project; refusing to overwrite"; exit 1; fi'

step "staging Siri files from the iPhone 4S firmware"
STAGE=$BUILD/stage
rm -rf "$STAGE" && mkdir -p "$STAGE"
grep -v '^#' "$ROOT/port/siri-files.txt" | grep . | while IFS= read -r p; do
  mkdir -p "$STAGE/$(dirname "$p")"; cp -Rp "$FW/$p" "$STAGE/$p"
done
cp "$BUILD/port/AssistantUI" "$STAGE/System/Library/PrivateFrameworks/AssistantUI.framework/AssistantUI"
COPYFILE_DISABLE=1 tar --no-xattrs -C "$STAGE" -czf "$BUILD/siri-files.tgz" .

step "copying (models are skipped when unchanged)"
on_ipod 'mkdir -p /var/root/siri/ace; for d in com.lmy.localsirid com.lmy.acesrv; do
  launchctl unload /Library/LaunchDaemons/$d.plist 2>/dev/null; done; true'
for m in moonshine-tiny.bin moonshine-vocab.bin smollm2-135m-q4e8.bin smollm2-135m.tok; do
  local_size=$(wc -c < "$BUILD/models/$m" | tr -d ' ')
  remote_size=$(on_ipod "wc -c < /var/root/siri/$m 2>/dev/null || echo 0" | tr -d ' ')
  [ "$local_size" = "$remote_size" ] || to_ipod "$BUILD/models/$m" /var/root/siri/
done
I=$BUILD/ipod
to_ipod "$I/localsirid" "$I/sirictl" "$I/screenshot" "$I/asllog" "$I/memstat" "$I/psrss" "$ROOT/src/localsirid/run.sh" /var/root/siri/
to_ipod "$I/acesrv" "$I/plistset" "$I/sirinotify" "$ROOT/src/acesrv/run.sh" /var/root/siri/ace/
to_ipod "$I/SiriCapability.dylib" "$ROOT/src/tweak/SiriCapability.plist" /Library/MobileSubstrate/DynamicLibraries/
to_ipod "$ROOT/src/localsirid/com.lmy.localsirid.plist" "$ROOT/src/acesrv/com.lmy.acesrv.plist" /Library/LaunchDaemons/
to_ipod "$BUILD/siri-files.tgz" /var/root/

step "installing Siri files and configuring"
on_ipod 'set -e
  tar -C / -xzf /var/root/siri-files.tgz --no-same-owner && rm /var/root/siri-files.tgz
  chown -R root:wheel /System/Library/PrivateFrameworks/Assistant*.framework /System/Library/PrivateFrameworks/SAObjects.framework \
    /System/Library/SpringBoardPlugins/Assistant.uibundle /System/Library/Assistant /System/Library/PreferenceBundles/Assistant.bundle \
    /System/Library/UserEventPlugins/AssistantUEA.plugin /System/Library/LaunchDaemons/com.apple.assistant*.plist
  chmod 755 /var/root/siri/run.sh /var/root/siri/ace/run.sh
  touch /var/root/siri/.installed
  # preferences (backed up once)
  cd /var/mobile/Library/Preferences
  for f in com.apple.assistant.plist com.apple.assistant.support.plist com.apple.springboard.plist; do
    [ -f $f ] && [ ! -f $f.bak-localsiri ] && cp $f $f.bak-localsiri; done
  P=/var/root/siri/ace/plistset
  su mobile -c "$P com.apple.assistant \"Server Override\" string http://guzzoni.apple.com:4443" >/dev/null 2>&1
  su mobile -c "$P com.apple.assistant \"Authentication Disabled\" bool 1" >/dev/null 2>&1
  su mobile -c "$P com.apple.assistant.support \"Assistant Enabled\" bool 1" >/dev/null 2>&1
  su mobile -c "$P com.apple.springboard SBAssistantCapabilityOverride bool 1" >/dev/null 2>&1
  # Siri accepts a Server Override only if its host ends in apple.com. Point that host at this iPod;
  # then even Siri'"'"'s built-in default server can only be reached on the device itself.
  [ -f /etc/hosts.bak-localsiri ] || cp /etc/hosts /etc/hosts.bak-localsiri
  grep -q "guzzoni.apple.com" /etc/hosts || printf "\n# local Siri: keep Siri on this device\n127.0.0.1\tguzzoni.apple.com\n" >> /etc/hosts
  # if Activator had the Home-button hold mapped to the LocalSiri app, give it back to Siri
  A=/var/mobile/Library/Caches/libactivator.plist
  if [ -f $A ] && grep -q com.lmy.localsiri $A; then
    [ -f $A.bak-localsiri ] || cp $A $A.bak-localsiri
    grep -v libactivator.menu.hold.short $A > $A.tmp && mv $A.tmp $A && chown mobile:mobile $A; fi
  launchctl load /Library/LaunchDaemons/com.lmy.localsirid.plist
  launchctl load /Library/LaunchDaemons/com.lmy.acesrv.plist
  launchctl load /System/Library/LaunchDaemons/com.apple.assistantd.plist 2>/dev/null || true
  launchctl load /System/Library/LaunchDaemons/com.apple.assistant_service.plist 2>/dev/null || true
  launchctl stop com.apple.assistantd 2>/dev/null || true
  killall SpringBoard'
echo "Installed. The models take ~20 s to load; then hold the Home button and ask a question."
