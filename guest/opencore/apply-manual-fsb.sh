#!/bin/bash
# Convert OpenCore PlatformInfo (MacPro5,1) to manual mode with a fixed
# FSBFrequency of 250050000 Hz, so the 10.6 kernel computes a TSC of
# 1.0002 GHz (QEMU TCG's real TSC is 1 GHz; the x86_64 10.6 kernel panics at
# <= 1.0000678 GHz). Works with OpenCore 0.6.6 and 0.6.8.
#
# Usage:  sudo ./apply-manual-fsb.sh [/Volumes/EFI-LEGACY]
# Bash 3.2 / Python 2.6 compatible (Mac OS X 10.6).

VOL="${1:-/Volumes/EFI-LEGACY}"
HERE="$(cd "$(dirname "$0")" && pwd)"
CFG="$VOL/EFI/OC/config.plist"
PB=/usr/libexec/PlistBuddy

if [ "$(id -u)" != "0" ]; then
    echo "Run with sudo: sudo $0 $*"; exit 1
fi
if [ ! -f "$CFG" ]; then
    echo "Not found: $CFG"; exit 1
fi

echo ">> running OpenCore: $(nvram 4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102:opencore-version 2>/dev/null | awk '{print $2}')"
BK="$CFG.before-manual-fsb-$(date +%Y%m%d-%H%M%S)"
cp -p "$CFG" "$BK" || { echo "Backup failed. Nothing changed."; exit 1; }
echo ">> backup: $BK"

if ! /usr/bin/python "$HERE/oc-manual-fsb.py" "$CFG"; then
    exit 1
fi
if ! plutil -lint "$CFG"; then
    echo "Resulting plist is invalid; restoring backup."
    cp -p "$BK" "$CFG"
    exit 1
fi

echo ">> result"
for k in Automatic UpdateDataHub UpdateNVRAM UpdateSMBIOS DataHub:FSBFrequency \
         DataHub:SystemProductName DataHub:BoardProduct SMBIOS:SystemSerialNumber PlatformNVRAM:BID; do
    printf "   %-28s %s\n" "$k" "$($PB -c "Print :PlatformInfo:$k" "$CFG" 2>&1)"
done

echo
echo "Done. Reboot, then check:  sysctl machdep.tsc.frequency   (expect 1000200000)"
echo "To undo:  sudo cp -p \"$BK\" \"$CFG\""
