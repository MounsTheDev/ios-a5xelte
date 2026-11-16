#!/bin/bash
# Reflash BOOT (sda5) from the reference bt9 image and read it back.
# Only used if build/diag-readonly.sh says BOOT differs. Writes sda5 and
# nothing else; never touches sda3 (EFS), sda6 (RECOVERY) or sda18 (USERDATA).
set -euo pipefail
cd "$(dirname "$0")/.."

ADB=${ADB:-adb}
IMG=build/SAFE-xnu-boot-bt9.img
WANT=ff4ec978254765e51fb16b07e9cd57bca157371156fc25711324a761c565fd38

got=$(sha256sum "$IMG" | cut -d' ' -f1)
[ "$got" = "$WANT" ] || { echo "local $IMG is $got, expected $WANT" >&2; exit 1; }

cap=$("$ADB" shell cat /sys/class/power_supply/battery/capacity | tr -d '\r\n ')
[ "${cap:-0}" -ge 15 ] || { echo "battery ${cap}%: refuse to flash" >&2; exit 1; }

SZ=$(stat -c %s "$IMG")
echo "pushing $SZ bytes"
"$ADB" push "$IMG" /tmp/bt9.img
"$ADB" shell "dd if=/tmp/bt9.img of=/dev/block/sda5 bs=1048576 conv=fsync"
echo "reading back the first $SZ bytes"
back=$("$ADB" shell "head -c $SZ /dev/block/sda5 | sha256sum" | cut -d' ' -f1)
echo "want: $WANT"
echo "got : $back"
[ "$back" = "$WANT" ] && echo "BOOT: rewritten and verified" || { echo "READBACK MISMATCH" >&2; exit 1; }
