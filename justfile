# Local Siri on an iPod touch 4G (iOS 6.1.6): the native Siri UI, answered entirely on the device.
#
#   just setup ~/Downloads/xcode4630916281a.dmg     # once: toolchain, firmware, AssistantUI, models, build
#   IPOD=root@192.168.1.23 just install             # copy to the iPod and respring
#
# IPOD (and optionally IPOD_KEY, a private key file) select the iPod for the SSH steps.

set shell := ["bash", "-c"]
set dotenv-load := true

# List the recipes
default:
    @just --list --unsorted

# Everything that happens on the Mac: toolchain, firmware, AssistantUI, models, build
setup xcode_dmg: (toolchain xcode_dmg) firmware assistantui models build

# Check the Mac's prerequisites (and the iPod, if IPOD is set)
doctor:
    bash scripts/doctor.sh

# Extract Xcode 4.6.3's armv7 compiler and the iOS 6.1 SDK from Apple's disk image
toolchain xcode_dmg:
    bash scripts/toolchain.sh "{{xcode_dmg}}"

# Download and decrypt the iPhone 4S iOS 6.1.3 root filesystem (~925 MB) and mount it
firmware:
    bash scripts/firmware.sh

# Extract AssistantUI from the 4S shared cache and make it loadable on the iPod
assistantui: firmware
    bash scripts/assistantui.sh

# Download Moonshine Tiny and SmolLM2-135M-Instruct and convert them (~130 MB)
models:
    bash scripts/models.sh

# Cross-compile everything that runs on the iPod
build:
    bash scripts/build.sh

# Install or update everything on the iPod over SSH, then respring
install:
    bash scripts/install.sh

# Remove everything `install` added and restore the backups
uninstall:
    bash scripts/uninstall.sh

# Ask the on-device brain directly (no Siri UI) with audio synthesized on the Mac
smoke question="What is the capital of France?":
    bash scripts/smoke.sh "{{question}}"

# Ask through the real Siri UI: opens Siri, the Mac speaks, prints the answer, saves a screenshot
ask question="What is the capital of France?":
    bash scripts/ask.sh "{{question}}"

# Show the last lines of the on-device logs
logs:
    bash -c 'source scripts/lib.sh; on_ipod "tail -n 15 /var/root/siri/localsirid.log; echo; tail -n 15 /var/root/siri/ace/acesrv.log"'

# Take a screenshot of the iPod into build/screenshots/
screenshot:
    bash -c 'source scripts/lib.sh; mkdir -p build/screenshots; f=build/screenshots/$(date +%H%M%S).png; on_ipod /var/root/siri/screenshot /tmp/shot.png; from_ipod /tmp/shot.png $f; echo $f'

# Install the debug tweak: `sirinotify mic` presses Siri's mic button, Siri errors go to the syslog
debug-on:
    bash -c 'source scripts/lib.sh; to_ipod build/ipod/SiriDebug.dylib src/debug/SiriDebug.plist /Library/MobileSubstrate/DynamicLibraries/; on_ipod "touch /tmp/siri-debug; killall SpringBoard; launchctl stop com.apple.assistantd"'

# Remove the debug tweak
debug-off:
    bash -c 'source scripts/lib.sh; on_ipod "rm -f /Library/MobileSubstrate/DynamicLibraries/SiriDebug.* /tmp/siri-debug; killall SpringBoard"'

# Also install LocalSiri.app, the stand-alone fallback app (open it with localsiri:// or an Activator gesture)
install-app:
    bash -c 'source scripts/lib.sh; tar -C build/ipod -czf build/app.tgz LocalSiri.app; to_ipod build/app.tgz /var/root/; on_ipod "tar -C /Applications -xzf /var/root/app.tgz --no-same-owner && rm /var/root/app.tgz && su mobile -c uicache"'

# Delete build outputs (keeps the toolchain, the firmware download and the models)
clean:
    rm -rf build/ipod build/stage build/decache build/port build/siri-files.tgz
