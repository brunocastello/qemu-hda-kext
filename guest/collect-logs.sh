#!/bin/bash
# Gather everything needed to debug QemuHDAAudio into one text file on the
# Desktop, ready to copy to the host and paste into Claude Code.
# Usage:  sudo ./collect-logs.sh      (run while audio is playing, ideally)

OUT="$HOME/Desktop/qemuhda-report-$(date +%Y%m%d-%H%M%S).txt"
[ -n "$SUDO_USER" ] && OUT="/Users/$SUDO_USER/Desktop/$(basename "$OUT")"

{
    echo "=== sw_vers / uname"; sw_vers; uname -a
    echo; echo "=== kextstat (audio related)"
    kextstat | grep -iE "audio|hda|qemu|voodoo"
    echo; echo "=== kernel log (QemuHDAAudio)"
    if [ -f /var/log/kernel.log ]; then
        grep -i "QemuHDAAudio" /var/log/kernel.log | tail -200
    fi
    grep -i "QemuHDAAudio" /var/log/system.log 2>/dev/null | tail -200
    echo; echo "=== CoreAudio / QuickTime messages (system.log)"
    grep -iE "coreaudiod|HAL|AudioQueue|QuickTime|overload" /var/log/system.log 2>/dev/null | grep -v QemuHDAAudio | tail -60
    echo; echo "=== kextutil -n -t"
    for d in /System/Library/Extensions /Library/Extensions; do
        [ -d "$d/QemuHDAAudio.kext" ] && kextutil -n -t "$d/QemuHDAAudio.kext" 2>&1
    done
    echo; echo "=== ioreg: QemuHDAAudio device/engine"
    ioreg -l -w0 -r -c QemuHDAAudioDevice 2>/dev/null | head -400
    echo; echo "=== ioreg: engine state (sampled twice, 1 s apart)"
    ioreg -l -w0 | grep -E "IOAudioEngineState|IOAudioEngineNumActiveUserClients|IOAudioEngineSampleOffset" | head -20
    sleep 1
    ioreg -l -w0 | grep -E "IOAudioEngineState|IOAudioEngineNumActiveUserClients" | head -20
    echo; echo "=== PCI audio devices"
    ioreg -l -w0 | grep -iE '"(name|IOName)" = "pci8086,(2668|293e)"'
} > "$OUT" 2>&1

[ -n "$SUDO_USER" ] && chown "$SUDO_USER" "$OUT"
echo "Report written to $OUT"
