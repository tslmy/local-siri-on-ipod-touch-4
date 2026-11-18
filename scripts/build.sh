#!/bin/bash
# Cross-compile everything that runs on the iPod into build/ipod, then fake-sign it with ldid.
source "$(dirname "$0")/lib.sh"
[ -x "$XCODE_TOOLCHAIN/usr/bin/clang" ] && [ -d "$IOS_SDK" ] || { echo "run 'just toolchain <xcode.dmg>' first" >&2; exit 1; }
S=$ROOT/src
O=$BUILD/ipod
rm -rf "$O"   # ldid asserts when re-signing a binary it signed before; always start clean
mkdir -p "$O" "$BUILD/third_party"

# Speex 1.2.1 (BSD), compiled into acesrv to decode Siri's audio when it really is Speex.
SPEEX=$BUILD/third_party/speex-1.2.1
if [ ! -d "$SPEEX" ]; then
  step "fetching Speex 1.2.1"
  curl -sSL -o "$BUILD/third_party/speex.tgz" https://downloads.xiph.org/releases/speex/speex-1.2.1.tar.gz
  echo "4b44d4f2b38a370a2d98a78329fefc56a0cf93d1c1be70029217baae6628feea  $BUILD/third_party/speex.tgz" | shasum -a 256 -c - >/dev/null
  tar -C "$BUILD/third_party" -xzf "$BUILD/third_party/speex.tgz"
fi
SPX=""
for f in bits cb_search exc_10_16_table exc_10_32_table exc_20_32_table exc_5_256_table exc_5_64_table \
         exc_8_128_table filters gain_table_lbr gain_table hexc_10_32_table hexc_table high_lsp_tables lpc \
         lsp_tables_nb lsp ltp modes_wb modes nb_celp quant_lsp sb_celp speex_callbacks speex_header speex \
         stereo vbr vq window; do SPX="$SPX $SPEEX/libspeex/$f.c"; done

step "localsirid + sirictl (speech recognition and LLM daemon)"
cc_ios -O3 -ffast-math $NEON "$S/localsirid/localsirid.c" "$S/moonshine/moonshine.c" "$S/whistle/whistle.c" "$S/llm/llm.c" -lm -lpthread -o "$O/localsirid"
cc_ios -O3 -ffast-math $NEON "$S/localsirid/sirictl.c" "$S/moonshine/moonshine.c" -lm -o "$O/sirictl"

step "acesrv (on-device ACE server)"
# shellcheck disable=SC2086
cc_ios -O2 $NEON -DHAVE_CONFIG_H -I"$S/acesrv/speexcfg" -I"$SPEEX/include" -Wno-everything \
  "$S/acesrv/acesrv.c" $SPX -lz -framework CoreFoundation -o "$O/acesrv"

step "SiriCapability tweak"
cc_ios -O2 -dynamiclib -framework CoreFoundation -lobjc \
  -install_name /Library/MobileSubstrate/DynamicLibraries/SiriCapability.dylib \
  "$S/tweak/SiriCapability.c" -o "$O/SiriCapability.dylib"

step "tools"
cc_ios -O2 -framework CoreFoundation "$S/tools/plistset.c" -o "$O/plistset"
cc_ios -O2 -fobjc-arc -framework UIKit -framework Foundation -framework CoreGraphics "$S/tools/screenshot.m" -o "$O/screenshot"
for t in asllog memstat psrss; do cc_ios -O2 "$S/tools/$t.c" -o "$O/$t"; done
cc_ios -dynamiclib -fobjc-arc -O2 -framework Foundation "$S/debug/SiriDebug.m" -o "$O/SiriDebug.dylib"
cc_ios -O2 "$S/debug/sirinotify.c" -o "$O/sirinotify"

step "LocalSiri.app (optional stand-alone app)"
APP=$O/LocalSiri.app
rm -rf "$APP" && mkdir -p "$APP"
cc_ios -O2 -fobjc-arc -framework UIKit -framework Foundation -framework CoreGraphics -framework QuartzCore \
  -framework AudioToolbox -framework AVFoundation "$S/app/main.m" -o "$APP/LocalSiri"
cp "$S/app/Info.plist" "$S/app/Icon.png" "$S/app/Icon@2x.png" "$APP/"

step "signing"
for f in localsirid sirictl acesrv SiriCapability.dylib plistset screenshot asllog memstat psrss SiriDebug.dylib sirinotify LocalSiri.app/LocalSiri; do
  ldid -S "$O/$f"
done
ls -l "$O"
