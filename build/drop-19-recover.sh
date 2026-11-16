#!/bin/bash
# Generation 19: roll the drop zone back to the look gen 17 ran.
#
# The generation-18 package carried the withdrawn Liquid Glass theme and the
# phone froze on "xnu mmu on". This writes the same package with
#   - libherosb.dylib back to the gen-17 build (sha256 bf913b0d...),
#   - an empty glass.off as a second guard,
# and nothing else changes: the kernel on BOOT (bt9) is not touched, and no
# other partition is written. Only USERDATA's drop zone is rewritten.
#
# Phone in TWRP, screen awake, cable plugged. Then reboot TWICE (the first
# boot installs the package, the second loads the library).
#
#   bash build/drop-19-recover.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# The binary must be the gen-17 one. Refuse to write otherwise: shipping the
# theme again would repeat the freeze.
want=bf913b0d923d056080b6f3e0411684b497af79a993a93d1ce1c26c82ac743071
got=$(sha256sum M/lib/libherosb.dylib | cut -d' ' -f1)
if [ "$got" != "$want" ]; then
   echo "M/lib/libherosb.dylib is $got" >&2
   echo "expected the gen-17 build $want" >&2
   echo "nothing written" >&2
   exit 1
fi
echo "libherosb.dylib: gen-17 build confirmed"

# A second guard, in case a later package ever ships without the revert.
: > build/glass.off

GEN=19 GLASS=0 bash build/drop-18.sh
