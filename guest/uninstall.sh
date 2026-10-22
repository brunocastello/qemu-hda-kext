#!/bin/bash
# Remove QemuHDAAudio.kext from the Mac OS X guest.
# Usage:  sudo ./uninstall.sh

NAME="QemuHDAAudio.kext"

if [ "$(id -u)" != "0" ]; then
    echo "Run with sudo: sudo $0"; exit 1
fi

kextunload -b org.qemuhda.driver.QemuHDAAudio >/dev/null 2>&1
for dir in /System/Library/Extensions /Library/Extensions; do
    if [ -d "$dir/$NAME" ]; then
        echo ">> removing $dir/$NAME"
        rm -rf "$dir/$NAME"
    fi
done
touch /System/Library/Extensions
if kextcache -help 2>&1 | grep -q -- "-system-caches"; then
    kextcache -system-caches >/dev/null 2>&1
fi
echo "Done. Reboot to finish."
