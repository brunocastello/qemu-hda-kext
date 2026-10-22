# QemuHDAAudio

> [!CAUTION]
> ### 🤖 AI-Generated Code Ahead
> **This entire repository was crafted with the help of AI.**
>
> If you are allergic to neural networks, synthesized functions, or autocomplete on steroids, **this repository is not for you**.
> You are kindly invited to close this page and write your code line-by-line in `ed` or `vim`. Everyone else, enjoy!

A sound driver for **Mac OS X 10.6–10.9** virtual machines running in **UTM / QEMU**, including on Apple Silicon Macs where the guest is emulated.

It drives QEMU's Intel HD Audio device, so the guest gets working speakers and microphone instead of the silence of VoodooHDA or the stuttering of USB audio.

- Playback and recording, 16-bit stereo, 44.1 or 48 kHz
- Built to survive the slow, uneven timing of an emulated Mac
- Sleep and wake
- 64-bit kernel only

Tested on Mac OS X 10.6.8 in UTM (QEMU 10) on an M4 Pro Mac. 10.7–10.9 should work but are untested.

## Set up the VM in UTM

1. In the VM's settings, set **Sound** to **Intel HD Audio Controller (ich9)**. The ich6 one (`intel-hda`) works too. No custom QEMU arguments are needed.
2. In *UTM → Settings → Sound*, pick the backend:
   - **Default** or **SPICE with GStreamer**: playback **and** microphone. macOS asks for microphone permission the first time the guest records.
   - **CoreAudio (Output Only)**: playback only.

## Fix the guest clock first

If sound plays for about two seconds and then goes silent, the VM's clock is wrong. This is an OpenCore setting, not a driver problem.

Check it in the guest:

```sh
sysctl machdep.tsc.frequency
```

- About **1333000000** → broken: the guest clock runs at 75% speed.
- **1000200000** → fixed.

The fix is to set OpenCore's `PlatformInfo → Automatic` to `false` and `DataHub → FSBFrequency` to **`250050000`**. The zip includes a script that does this for `MacPro5,1` configs and backs up `config.plist` first:

```sh
cd QemuHDAAudio-<version>/opencore
sudo ./apply-manual-fsb.sh /Volumes/EFI-LEGACY    # your mounted OpenCore EFI partition
```

Back up your EFI disk before you run it, then reboot.

<details>
<summary>Why this happens</summary>

When QEMU emulates an x86 Mac on an ARM64 host, the virtual CPU's timestamp counter runs at exactly 1 GHz. OpenCore 0.6.6/0.6.7 misreads the CPU's bus ratio and reports a speed that makes the guest's clock run at 0.75×. OpenCore 0.6.8 and later report the true 1 GHz, but the 10.6 64-bit kernel refuses anything at or below 1.0000678 GHz and panics at boot. A fixed bus frequency of 250.05 MHz gives the kernel 1.0002 GHz: no panic, and only 0.02% clock error.
</details>

## Install

Download `QemuHDAAudio-<version>.zip` from the [Releases](../../releases) page, copy it into the guest, and run:

```sh
unzip QemuHDAAudio-<version>.zip
cd QemuHDAAudio-<version>
sudo ./install.sh
```

Then choose **QEMU HDA Audio** in *System Preferences → Sound* (Output and Input). Uninstall VoodooHDA first if you have it. To remove the driver, run `sudo ./uninstall.sh` and reboot.

## Troubleshooting

Run `sudo ./collect-logs.sh`. It saves a report to the Desktop.

The settings live in `/System/Library/Extensions/QemuHDAAudio.kext/Contents/Info.plist`. After changing them, run `sudo touch /System/Library/Extensions` and reboot.

| Setting | Default | What it does |
|---|---|---|
| `QemuHDASampleOffset` | 4096 | How far ahead the system writes audio. Raise to 8192 if sound still stutters. |
| `QemuHDASampleRate` | 44100 | Starting rate, 44100 or 48000. Also changeable in Audio MIDI Setup. |
| `QemuHDAInput` | 1 | Set to 0 to hide the input device. |
| `QemuHDADebug` | 1 | Log detail, 0–3. Use 2 when reporting a problem. |

## Building

GitHub Actions builds the driver on every push and attaches the zip to a release for `v*` tags. To build locally you need a Mac with Xcode: `make deps && make zip`.

## License

MIT. The build also uses Apple's IOAudioFamily headers and acidanthera's MacKernelSDK (both APSL 2.0).
