#!/bin/bash
# run_fsuae.sh ADF KICKSTART OUT_PREFIX SECONDS [inject-script]
# Boots the ADF in FS-UAE (A1200, PAL, chip RAM only, turbo floppy) headless under Xvfb, using ../../platform-a1200/test/run.sh.
# inject-script lines: `shot NAME` (screenshot), `sleep N`, `key <xdotool args>`.
here=$(cd "$(dirname "$0")" && pwd)
t=$here/../../platform-a1200/test
cp "$here/pal_turbo.fs-uae" "$t/pal_turbo.fs-uae"
(cd "$t" && KICK="$2" ADF="$1" bash run.sh pal_turbo "$3" "${4:-10}" "$5")
rm -f "$t/pal_turbo.fs-uae"
