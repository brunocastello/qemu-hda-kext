#!/bin/bash
# Install QemuHDAAudio.kext inside the Mac OS X guest (10.6 - 10.9).
# Usage:  sudo ./install.sh [path/to/QemuHDAAudio.kext]
# Bash 3.2 compatible (the guest's /bin/bash).

KEXT_SRC="${1:-$(dirname "$0")/QemuHDAAudio.kext}"
DEST="/System/Library/Extensions"
NAME="QemuHDAAudio.kext"

if [ "$(id -u)" != "0" ]; then
    echo "Run with sudo: sudo $0 $*"; exit 1
fi
if [ ! -d "$KEXT_SRC" ]; then
    echo "Kext not found: $KEXT_SRC"; exit 1
fi
if [ "$(uname -m)" != "x86_64" ]; then
    echo "This kext is x86_64 only, but the kernel is $(uname -m)."
    echo "Boot the 64-bit kernel (boot-arg arch=x86_64) and try again."
    exit 1
fi

for other in VoodooHDA.kext AppleHDADisabler.kext; do
    if [ -d "$DEST/$other" ] || [ -d "/Library/Extensions/$other" ]; then
        echo "WARNING: $other is installed. Remove it first to avoid conflicts."
    fi
done
if kextstat | grep -qi voodoohda; then
    echo "WARNING: VoodooHDA is currently loaded; reboot after removing it."
fi

echo ">> unloading old copy (if any)"
kextunload -b org.qemuhda.driver.QemuHDAAudio >/dev/null 2>&1

echo ">> copying to $DEST"
rm -rf "$DEST/$NAME"
cp -R "$KEXT_SRC" "$DEST/$NAME" || exit 1
chown -R root:wheel "$DEST/$NAME"
find "$DEST/$NAME" -type d -exec chmod 755 {} \;
find "$DEST/$NAME" -type f -exec chmod 644 {} \;

echo ">> validating (kextutil -n -t)"
kextutil -n -t "$DEST/$NAME"
RC=$?

echo ">> refreshing kext caches"
touch "$DEST"
if kextcache -help 2>&1 | grep -q -- "-system-caches"; then
    kextcache -system-caches >/dev/null 2>&1
fi

if [ $RC -ne 0 ]; then
    echo "kextutil reported problems (see above). Not loading."
    exit 1
fi

echo ">> loading now"
kextload "$DEST/$NAME"
sleep 2
kextstat | grep -i qemuhda
echo
echo "Done. Select 'QEMU HDA Audio' in System Preferences > Sound > Output."
echo "Logs:  grep -i QemuHDAAudio /var/log/kernel.log | tail -40   (10.6/10.7)"
echo "       syslog | grep -i QemuHDAAudio | tail -40              (10.8/10.9)"
