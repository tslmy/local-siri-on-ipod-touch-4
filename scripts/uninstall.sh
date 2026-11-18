#!/bin/bash
# Remove everything install.sh added and restore the backed-up files.
source "$(dirname "$0")/lib.sh"
on_ipod 'for d in /System/Library/LaunchDaemons/com.apple.assistantd.plist /System/Library/LaunchDaemons/com.apple.assistant_service.plist \
           /Library/LaunchDaemons/com.lmy.acesrv.plist /Library/LaunchDaemons/com.lmy.localsirid.plist; do
    launchctl unload $d 2>/dev/null; done
  [ -f /var/root/siri/.installed ] && rm -rf /System/Library/PrivateFrameworks/AssistantServices.framework \
    /System/Library/PrivateFrameworks/AssistantUI.framework /System/Library/PrivateFrameworks/SAObjects.framework \
    /System/Library/SpringBoardPlugins/Assistant.uibundle /System/Library/Assistant /System/Library/PreferenceBundles/Assistant.bundle \
    /System/Library/UserEventPlugins/AssistantUEA.plugin /System/Library/LaunchDaemons/com.apple.assistantd.plist \
    /System/Library/LaunchDaemons/com.apple.assistant_service.plist "/Applications/Preferences.app/5.0~General Assistant.plist"
  rm -f /Library/MobileSubstrate/DynamicLibraries/SiriCapability.* /Library/MobileSubstrate/DynamicLibraries/SiriDebug.* \
    /Library/LaunchDaemons/com.lmy.acesrv.plist /Library/LaunchDaemons/com.lmy.localsirid.plist /var/root/siri/.installed
  for f in /var/mobile/Library/Preferences/com.apple.assistant.plist /var/mobile/Library/Preferences/com.apple.assistant.support.plist \
           /var/mobile/Library/Preferences/com.apple.springboard.plist /etc/hosts /var/mobile/Library/Caches/libactivator.plist; do
    [ -f $f.bak-localsiri ] && mv $f.bak-localsiri $f
    case $f in /var/mobile/*) [ -f $f ] && chown mobile:mobile $f && chmod 600 $f;; esac; done
  killall SpringBoard
  echo "removed; models and logs remain in /var/root/siri (delete that folder to free ~130 MB)"'
