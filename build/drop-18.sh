#!/bin/bash
# Drop zone generation 18: libherosb with the Liquid Glass look.
#
# Run this with the phone in TWRP, screen awake and the cable plugged in
# (adb loses a sleeping TWRP). It writes the package to USERDATA and verifies
# it; it never touches BOOT, EFS or RECOVERY.
#
#   bash build/drop-18.sh              (adb must be on PATH)
#
# After it prints "-> verified", reboot TWICE:
#   1. the first boot installs the package;
#   2. the second boot loads the new libherosb (the shim is loaded before the
#      installer runs, so one boot is not enough).
set -euo pipefail

cd "$(dirname "$0")/.."

ADB=${ADB:-adb}
GEN=${GEN:-18}

# Every path is given as DEST=SRC and checked before anything is written.
items=(
   autorun=M/hero-run
   test=M/eagl_ca_test
   gl.env=drop/gl.env
   test.env=drop/test.env
   disabled.list=drop/disabled.list
   wallpaper.off=drop/wallpaper.off
   gestures.off=drop/gestures.off
   lib/libdrm.2.dylib=M/lib/libdrm.2.dylib
   lib/libOSMesa.8.dylib=M/lib/libOSMesa.8.dylib
   # The HakiOS EAGL bridge is installed under the shim's name: the kernel
   # inserts /private/var/herolte/lib/libheroeagl.dylib into backboardd.
   lib/libheroeagl.dylib=M/lib/libhakiosgl.dylib
   lib/libherosb.dylib=M/lib/libherosb.dylib
   lib/libheroapp.dylib=M/lib/libheroapp.dylib
   audio/HakiosAudio=M/audio/HakiosAudio
   audio/Info.plist=M/audio/Info.plist
   wifi/fw.bin=drop/wifi/fw.bin
   wifi/clm.blob=drop/wifi/clm.blob
   wifi/nvram.txt=drop/wifi/nvram.txt
   wifi/network.txt=drop/wifi/network.txt
   bt/bcm4359.hcd=drop/bt/bcm4359.hcd
)

# Any app whose .ipa is present goes in too; with none, the package carries
# no apps and the ones already installed stay where they are. Only arm64
# slices can run here: herolte-drop.py refuses a 32-bit or arm64e-only
# binary, so a rejected .ipa stops the whole drop. ipa/Clock.ipa is the
# donor IPSW's MobileTimer.app and its executable is CPU_TYPE_ARM (armv7),
# which this phone cannot run: it is skipped rather than shipped.
for ipa in ipa/*.ipa; do
   [ -e "$ipa" ] || continue
   if [ "$(basename "$ipa")" = Clock.ipa ]; then
      echo "skip: $ipa (armv7 executable, cannot run on arm64)"
      continue
   fi
   items+=("app=$ipa")
done

missing=0
for item in "${items[@]}"; do
   src=${item#*=}
   if [ ! -f "$src" ]; then
      echo "missing: $src" >&2
      missing=1
   fi
done
[ "$missing" = 0 ] || { echo "nothing written" >&2; exit 1; }

echo "device:"
"$ADB" devices

echo "generation $GEN, ${#items[@]} entries"
python3 tools/herolte-drop.py "$ADB" --generation "$GEN" "${items[@]}"
