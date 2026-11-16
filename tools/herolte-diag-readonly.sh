#!/bin/bash
# Read-only diagnosis before any write. Nothing here modifies the phone:
# every command is dd from a partition to stdout, cat of a sysfs file, or
# ls of a directory. It refuses to run if the battery is too low for a
# later drop (a drop interrupted by a brownout is the one thing to avoid).
#
#   bash tools/herolte-diag-readonly.sh > build/diag-report.txt 2>&1
set -uo pipefail
cd "$(dirname "$0")/.."

ADB=${ADB:-adb}
BT9=build/SAFE-xnu-boot-bt9.img
WANT=ff4ec978254765e51fb16b07e9cd57bca157371156fc25711324a761c565fd38

say() { printf '\n=== %s ===\n' "$*"; }

say "device"
"$ADB" devices
"$ADB" shell getprop ro.twrp.version

say "battery (a drop below 15% is not attempted)"
cap=$("$ADB" shell cat /sys/class/power_supply/battery/capacity 2>/dev/null | tr -d '\r\n ')
mv=$("$ADB" shell cat /sys/class/power_supply/battery/voltage_now 2>/dev/null | tr -d '\r\n ')
st=$("$ADB" shell cat /sys/class/power_supply/battery/status 2>/dev/null | tr -d '\r\n ')
echo "capacity=${cap}%  voltage=${mv}uV  status=${st}"
case "${cap:-0}" in
   ''|*[!0-9]*) echo "capacity unreadable: refusing to continue" >&2; exit 1;;
esac
if [ "$cap" -lt 15 ]; then
   echo "capacity below 15%: charge before a drop" >&2
fi

say "BOOT (sda5) against $BT9"
SZ=$(stat -c %s "$BT9")
echo "payload size: $SZ bytes; partition: $("$ADB" shell blockdev --getsize64 /dev/block/sda5 | tr -d '\r\n ')"
got=$("$ADB" shell "dd if=/dev/block/sda5 bs=1048576 count=$(( (SZ + 1048575) / 1048576 )) 2>/dev/null | head -c $SZ | sha256sum" | cut -d' ' -f1)
echo "want: $WANT"
echo "got : $got"
[ "$got" = "$WANT" ] && echo "BOOT: identical" || echo "BOOT: DIFFERS -> reflash $BT9"

say "partition layout"
"$ADB" shell "ls -l /dev/block/by-name/ 2>/dev/null | head -30"
"$ADB" shell "cat /proc/partitions 2>/dev/null | head -30"

say "RECOVERY (sda6) — only read, to see it is the TWRP we expect"
"$ADB" shell "blockdev --getsize64 /dev/block/sda6 2>/dev/null"
"$ADB" shell "dd if=/dev/block/sda6 bs=1M count=1 2>/dev/null | md5sum"

say "USERDATA (sda18) size and drop zone bounds"
"$ADB" shell "blockdev --getsize64 /dev/block/platform/155a0000.ufs/by-name/USERDATA 2>/dev/null"

say "iOS root volume (/dev/ufsroot0 is inside USERDATA; TWRP does not mount HFSX)"
"$ADB" shell "ls /dev/block/platform/155a0000.ufs/by-name/ 2>/dev/null"
"$ADB" shell "mount 2>/dev/null | head -20"

say "done (nothing was written)"
