# QemuHDAAudio.kext

Audio driver for **Mac OS X 10.6–10.9** guests on **QEMU/UTM**. It drives QEMU's emulated Intel HDA controller (`intel-hda` / `ich9-intel-hda`) with the `hda-output` codec.

It targets guests running under **x86 emulation (TCG)**, for example on an Apple Silicon Mac, where VoodooHDA stays silent and `usb-audio` stutters. To ride out the emulated guest's irregular latency it uses:

- a large DMA ring (~370 ms at 44.1 kHz) that keeps running between sounds;
- a large, adjustable sample offset;
- timestamps derived from the DMA position (LPIB), with no interrupts.

Output (and input, with `hda-duplex` or `hda-micro`) is 16-bit stereo at **44.1 kHz** (default) or 48 kHz. Sleep/wake is supported. x86_64 only: the guest must run the 64-bit kernel (`uname -m` → `x86_64`).

Tested: Mac OS X 10.6.8 on UTM (QEMU 10.0, TCG) on an M4 Pro Mac — system sounds, `say`, QuickTime and iTunes play cleanly. 10.7–10.9 are untested.

## Required: fix the guest clock first (OpenCore)

**Without this, audio stops after ~2 seconds** (and most other timing in the guest is off too).

Under TCG on an ARM64 host, QEMU's emulated TSC ticks at exactly **1 GHz**. The guest kernel computes the TSC frequency from the `FSBFrequency` OpenCore hands it × the bus ratio QEMU reports (4):

- **OpenCore 0.6.6/0.6.7 in Automatic mode** reads the bus ratio from the wrong bits (3 instead of 4), publishes FSB = 1 GHz ÷ 3, and the kernel ends up with 1.333 GHz → **the guest clock runs at 0.75× real time**. CoreAudio then sees the audio device running 33% fast and apps fall silent after a couple of seconds.
- **OpenCore ≥ 0.6.8 in Automatic mode** publishes the correct 250 MHz → TSC 1.000 GHz, but the **10.6 x86_64 kernel panics** at boot (`commpage no match for last`): it requires a TSC above 1,000,067,800 Hz (`SLOW_TSC_THRESHOLD`).

The fix, for any OpenCore version: switch `PlatformInfo` to manual mode with **`DataHub → FSBFrequency = 250050000`**. The kernel then computes 1,000,200,000 Hz: no panic, and the clock is off by only 0.02%.

Check your guest:

```sh
sysctl machdep.tsc.frequency    # 1333321860-ish = broken (0.75x clock); 1000200000 = fixed
```

The zip includes `opencore/apply-manual-fsb.sh`, which converts an OpenCore config with `Automatic = true` and `SystemProductName = MacPro5,1` to manual mode. It reproduces exactly what Automatic generates for MacPro5,1 (OpenCore's model database), keeps your serial/MLB/ROM/UUID from `Generic`, adds the fixed FSB, and backs up `config.plist` first. For other models, fill `DataHub`/`PlatformNVRAM`/`SMBIOS` by hand (see OpenCore's `SampleCustom.plist`) with the same `FSBFrequency`.

**Back up your EFI disk on the host before touching OpenCore.**

```sh
# in the guest, with the OpenCore EFI partition mounted as /Volumes/EFI-LEGACY
cd QemuHDAAudio-<version>/opencore
sudo ./apply-manual-fsb.sh /Volumes/EFI-LEGACY
# reboot, then: sysctl machdep.tsc.frequency  ->  1000200000
```

## Get the kext

GitHub Actions builds it on every push.

1. Open the repository's **Actions** tab, open the latest `build` run and download the `QemuHDAAudio-…` artifact.
2. The artifact is a `.zip` holding `QemuHDAAudio-<version>.zip` with the kext and scripts.
3. On `v*` tags (e.g. `v0.2.0`) the zip is also attached to a Release.

## Configure the VM in UTM

Set *Sound* to **None**. Under *QEMU → Arguments*, add one argument per line:

```
-audiodev
coreaudio,id=snd0
-device
ich9-intel-hda,id=hda0
-device
hda-output,bus=hda0.0,audiodev=snd0
```

`intel-hda` (ICH6) also works instead of `ich9-intel-hda`. Uninstall VoodooHDA first.

### Audio input

Use `hda-duplex` (line out + line in) or `hda-micro` (speaker + microphone) instead of `hda-output`; the same `-audiodev` serves both directions:

```
-device
hda-duplex,bus=hda0.0,audiodev=snd0
```

The guest then shows **QEMU HDA Audio** under *System Preferences → Sound → Input* ("Line In"). On the host, UTM needs microphone access (macOS asks the first time; otherwise *System Settings → Privacy & Security → Microphone*). The input stream only runs while a guest app records, so the host microphone is not held open otherwise. Set `QemuHDAInput` to 0 to hide the input.

### Sample rate

QEMU opens the host's CoreAudio output at **44100 Hz** by default, and the kext defaults to 44100 Hz too, so nothing is resampled.

For 48 kHz:

1. Use `coreaudio,id=snd0,out.frequency=48000` for `-audiodev`.
2. In the guest's *Audio MIDI Setup*, set QEMU HDA Audio to 48000 Hz.

Both rates must match to avoid resampling.

## Install in the guest

Copy the zip into the guest and run:

```sh
unzip QemuHDAAudio-<version>.zip
cd QemuHDAAudio-<version>
sudo ./install.sh
```

Then:

1. In *System Preferences → Sound → Output*, choose **QEMU HDA Audio**.
2. Test with `afplay /System/Library/Sounds/Submarine.aiff`.

To remove it, run `sudo ./uninstall.sh` and reboot.

## Troubleshooting

```sh
sudo ./collect-logs.sh
```

This writes a report to the Desktop with logs, driver state and the IORegistry entries.

Settings live in the installed kext's `Info.plist`:

```
/System/Library/Extensions/QemuHDAAudio.kext/Contents/Info.plist
```

| Key | Default | Effect |
|---|---|---|
| `QemuHDADebug` | 1 | 0 errors only; 1 startup; 2 stream events + stats every 2 s; 3 every codec verb |
| `QemuHDASampleOffset` | 4096 | frames CoreAudio writes ahead of the DMA (~93 ms). Raise to 8192 if audio still stutters |
| `QemuHDASampleRate` | 44100 | initial rate (44100 or 48000); match the `-audiodev` frequency |
| `QemuHDAIdleStopSec` | 0 | seconds of silence before the DMA stops; 0 = keep it running (avoids losing the start of short sounds) |
| `QemuHDAInput` | 1 | publish the codec's input (hda-duplex / hda-micro); 0 = output only |
| `QemuHDAPollIntervalMs` | 5 | LPIB polling interval |
| `QemuHDASampleLatency` | 32 | reported latency (A/V sync only) |

After editing: `sudo touch /System/Library/Extensions` and reboot.

If sound stops after ~2 s, check the guest clock first (see above). With `QemuHDADebug` = 2, the stats line should show ~5.4 `wraps` per 2 s at 44.1 kHz; ~7 means the guest clock runs at 0.75×.

## License

This project's code: MIT (see `LICENSE`). The build downloads Apple's IOAudioFamily headers and acidanthera's MacKernelSDK, both APSL 2.0. The final binary includes MacKernelSDK's `libkmod.a`.
