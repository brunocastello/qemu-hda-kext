# CLAUDE.md — QemuHDAAudio.kext

Context for Claude Code to continue this project. **Talk to the user in English.** Code, comments and commit messages in English.

## Goal

Audio driver (IOAudioFamily kext) for QEMU's emulated Intel HDA controller (`intel-hda` / `ich9-intel-hda` with the `hda-output`, `hda-duplex` or `hda-micro` codec), for **Mac OS X 10.6.8, 10.7, 10.8 and 10.9** guests running in **UTM under x86_64 emulation (TCG)** on an **M4 Pro Mac** (host macOS 27). Goal: clean audio that tolerates the high, irregular latency of an emulated guest.

**Status (2026-10-22): v0.3.0 released, repo public.** Verified on 10.6.8: playback (alerts, `say`, QuickTime, iTunes), recording via UTM's SPICE backend, sleep/wake. 10.7–10.9 untested.

## How we got here

- **VoodooHDA 2.8.8** (10.6): loads and creates an engine, but no sound on any backend/controller variant. Uninstalled.
- **usb-audio** (Apple's USB Audio driver): plays, but "hammering"/stuttering. No QEMU overruns; bigger buffers didn't help. Blamed on guest latency at the time — in hindsight largely the slow guest clock (see below).
- Decision: our own HDA driver with a **large DMA ring** and **large sample offset**, no interrupts.

## User environment

- Host: M4 Pro Mac, macOS 27, UTM with QEMU **10.0.12**.
- VM "Mac OS X 1068": `q35`, `-cpu Penryn-v1,+ssse3,+sse4.1,+sse4.2`, 2 cores, `-accel tcg,thread=multi`, OVMF + OpenCore **0.6.6 DEBUG** on the `EFI-LEGACY` disk (`AS_SL.qcow2`, USB storage, boot index 0), SMBIOS MacPro5,1.
- Guest kernel: **x86_64**. The kext is x86_64 only.
- UTM sound: **Intel HD Audio Controller (ich9)** chosen in the GUI, backend Default/SPICE. No custom QEMU audio arguments any more.
- The user has no Xcode; **everything is built by GitHub Actions** and the zip is copied into the guest.
- UTM is sandboxed: files written by QEMU (e.g. `-audiodev wav`) don't land where expected; don't go down that path.

## Architecture

```
src/
  QemuHDAAudioDevice.{h,cpp}  IOAudioDevice: PCI/BAR0, controller reset, codec discovery,
                              per-direction DMA buffers (64 KiB ring + BDL), stream start/stop,
                              LPIB reads, sleep/wake (performPowerStateChange).
  QemuHDAAudioEngine.{h,cpp}  IOAudioEngine (one per direction): one 16-bit stereo stream (44.1/48 kHz),
                              software volume/mute, clip float→SInt16 (output) / convert SInt16→float
                              (input), timer polling LPIB; output DMA keeps running between sounds.
  QemuHDARegs.h               HDA registers and codec verbs used.
  QemuHDAGainTable.h          precomputed volume curve (0.75 dB/step) — no pow() in the kernel.
  kmod_info.c                 equivalent of the <product>_info.c Xcode generates for kexts.
Resources/Info.plist.in       personality (IOPCIMatch 0x26688086 0x293e8086, IOProbeScore 6000) + settings.
guest/                        install.sh / uninstall.sh / collect-logs.sh (bash 3.2, run in the guest).
guest/opencore/               apply-manual-fsb.sh + oc-manual-fsb.py (Python 2.6): OpenCore clock fix.
scripts/fetch-deps.sh         MacKernelSDK (pinned) + IOAudioFamily-183.4.11 headers (10.6 era).
scripts/macho-compat.py       strips load commands the 10.6 kxld rejects.
```

Key decisions:

- **No interrupts.** A work-loop `IOTimerEventSource` (default 5 ms) reads LPIB. On a buffer wrap the timestamp is **back-dated** (`now − LPIB/rate`), so poll latency doesn't become timestamp jitter. A poll delayed by more than a whole ring estimates the wraps from elapsed time.
- **Output DMA runs continuously** (silence) after the engine stops; the poll timer erases the ring behind the play head. On start, loop 0 is anchored to when the DMA was last at frame 0. This fixed short sounds losing their start/end (restarting the DMA and QEMU's host stream each time made "frame 0 = now" wrong until the first wrap). `QemuHDAIdleStopSec` can stop it after silence (default 0 = never).
- **Input is a separate engine** with its own clock (QEMU paces input and output independently). Input on SD0 (tag 2), output on SD4 (tag 1). The input DMA **stops** with its engine, otherwise UTM holds the host microphone open.
- **Sleep/wake:** IOAudioFamily pauses running engines, then `performPowerStateChange` stops every DMA (including the idle output DMA). On wake it re-initialises controller, codec and stream descriptors, because QEMU resets all devices on S3 resume; the family then restarts engines through `performAudioEngineStart`.
- **Immediate command interface** (IC/IR/IRS) instead of CORB/RIRB. QEMU leaves `ICB` stuck if a verb goes to a missing codec; `sendImmediate()` clears it.
- **Software volume** (in clip/convert). DAC and ADC amps at maximum (amps only exist with `mixer=on`).
- QEMU codec vendor `0x1af4` (`hda-output` 0x1af40012, `hda-duplex` 0x1af40022 with mixer); nodes are discovered generically. Only the first QEMU codec is used.
- **Rate: 44.1 kHz default**, 48 kHz optional (`QemuHDASampleRate` or Audio MIDI Setup); `performFormatChange` stops the DMA, changes FMT + codec format, restarts.
- QEMU side (from `hw/audio/intel-hda.c` / `hda-codec.c`): input streams SD0–3, output SD4–7 (`GCAP=0x4401`), stream tags looked up per direction, data pulled on a 1 ms timer at the stream rate, LPIB advances in ~1 ms steps.

## The guest clock (essential, not a driver issue)

- Under TCG on an ARM64 host, QEMU's `cpu_get_host_ticks()` falls back to `get_clock()` (ns) → **the guest TSC runs at exactly 1 GHz**. `IA32_PERF_STATUS` is fixed at `1000 | 4<<40` (bus ratio 4). TCG exposes no CPUID 0x40000010 timing leaf.
- The 10.6 kernel computes TSC = `FSBFrequency` (from OpenCore's DataHub) × ratio from PERF_STATUS bits 40–44.
- **OpenCore 0.6.6/0.6.7 Automatic:** reads the ratio from bits 8–12 (= 3), measures the TSC correctly (~1 GHz) and publishes FSB = 333.33 MHz → kernel TSC 1.333 GHz → **guest clock at 0.75×**. Symptoms: audio stops after ~2 s (CoreAudio sees the device 33% fast and clients send silence), ~7 ring wraps per 2 s in the stats instead of ~5.4. Fixed in OC 0.6.8.
- **OpenCore ≥ 0.6.8 Automatic:** FSB 250 MHz → TSC 1.000 GHz → **kernel panic** `commpage no match for last`: xnu-1504 sets `kSlow` when tscFreq ≤ `SLOW_TSC_THRESHOLD` (1000067800) and x86_64 only has `nanotime_64`, which requires !kSlow.
- **Fix:** `PlatformInfo/Automatic = false` with `DataHub/FSBFrequency = 250050000` → kernel TSC 1000200000 Hz (0.02% error). OpenCore ignores `DataHub/FSBFrequency` in Automatic mode, so manual DataHub/PlatformNVRAM/SMBIOS sections are needed; `guest/opencore/oc-manual-fsb.py` reproduces OC 0.6.6's Automatic output for MacPro5,1 (OpenCorePkg `MP51.yaml`) plus the fixed FSB. Applied to the user's VM; `machdep.tsc.frequency = 1000200000`.
- Decision: **stay on OpenCore 0.6.6**; the manual FSB is needed with any version, and 0.6.8 only adds config-schema risk.
- Recovery tricks that worked: OC 0.6.x uses the directory OpenCore.efi was loaded from as its root (boot a backup via OVMF *Boot From File*); the EFI qcow2 can be edited on the host with `qemu-img convert` → raw → `hdiutil attach -imagekey diskimage-class=CRawDiskImage` (VM fully stopped).

## UTM and QEMU audio backends

- From UTM's `Configuration/UTMQemuConfiguration+Arguments.swift`: picking an `*hda*` card in the GUI generates `-device ich9-intel-hda -device hda-duplex,audiodev=audio0` (backend Default or SPICE, `-audiodev spice,id=audio0`) or `-device hda-output,audiodev=audio1` (backend CoreAudio, `-audiodev coreaudio,id=audio1`). The driver handles both.
- QEMU 10.0's **CoreAudio backend is output only** (`audio/coreaudio.m`: only `init_out`, `max_voices_in = 0`), so the microphone needs SPICE; UTM's SPICE client asks macOS for microphone permission when the guest records.
- UTM only enables SPICE audio (`UTMSpiceIOOptionsHasAudio`) when the VM has a sound card configured; "Sound = None" plus custom args on `audio0` is silent.
- SPICE runs at 48 kHz when the client supports Opus; with the guest at 44.1 kHz QEMU resamples (sounds fine).

## Build

Requires macOS with Xcode (CI uses `macos-15`, Xcode 16.4). No `.xcodeproj` on purpose.

**RULE: never build anything on the user's Mac.** No `make`, `make deps`, `make objects`, Xcode or local dependency downloads. All builds happen **only on GitHub Actions** (public repo `brunocastello/qemu-hda-kext`): commit + push, follow with `gh run watch`, read failures with `gh run view --log-failed`, download with `gh run download`. Pushing a `v*` tag attaches the zip to a GitHub Release. The commands below are what the workflow runs.

```sh
make deps     # clones deps/ (MacKernelSDK @ 3f750085…, IOAudioFamily-183.4.11)
make          # build/QemuHDAAudio.kext
make check    # load commands, segments, undefined symbols (fails on __cxa_atexit etc.)
make zip      # build/QemuHDAAudio-<VERSION>.zip (kext + guest scripts + opencore/)
make objects  # compile only
```

Version in `VERSION`. Sensitive flags (don't remove without a reason):

- `-isystem deps/ioaudio-overlay` **before** MacKernelSDK: compiling against the **oldest** supported IOAudioFamily (1.8.3) guarantees the kext only references symbols/vtable slots that exist on 10.6; newer IOAudioFamily versions stay binary compatible (kxld patches reserved slots).
- `-target x86_64-apple-macos10.6` + `-mkernel -fapple-kext -fno-use-cxa-atexit -fno-rtti -fno-exceptions -fno-stack-protector`.
- `-mllvm -disable-atexit-based-global-dtor-lowering` (conditional): recent LLVM turns static destructors (e.g. the `MetaClass` from `OSDefineMetaClassAndStructors`) into `__cxa_atexit` calls, which no kernel exports. Without it the kext won't load; `make check` fails if it comes back.
- `scripts/macho-compat.py`: the 10.6 kxld (xnu-1504) rejects `LC_VERSION_MIN_MACOSX`, `LC_FUNCTION_STARTS`, `LC_DATA_IN_CODE`, `LC_SOURCE_VERSION`, `LC_BUILD_VERSION`. The script strips those (metadata only) and **fails** on any unknown load command (e.g. chained fixups) instead of removing something functional.
- `OSBundleLibraries` with low versions (`IOAudioFamily 1.1fc9`, KPIs `10.0`) so it loads on 10.6–10.9.

The build is clean (no warnings). The `refs/tags/IOAudioFamily-183.4.11 ... is not a commit!` message in `make deps` is just an annotated tag in a shallow clone. If linking ever breaks: check the "Build" step log; try `-Xlinker -ld_classic` or pin another Xcode in the workflow; `libclang_rt.cc_kext.a` is optional (find it with `xcrun clang -print-file-name=lib/darwin/libclang_rt.cc_kext.a`); investigate unknown load commands before adding them to `STRIP`.

## Testing in the guest

```sh
unzip QemuHDAAudio-x.y.z.zip && cd QemuHDAAudio-x.y.z
sudo ./install.sh            # copies to /System/Library/Extensions, kextutil -t, kextload
# System Preferences → Sound → Output/Input → "QEMU HDA Audio"
afplay /System/Library/Sounds/Submarine.aiff
say -v Alex "Hello from Alex"          # short sounds catch start/end losses
sudo ./collect-logs.sh       # writes ~/Desktop/qemuhda-report-*.txt to paste here
```

Recording: QuickTime Player → File → New Audio Recording. Sleep: `sudo pmset schedule wake "$(date -v+2M '+%m/%d/%y %H:%M:%S')"; sudo pmset sleepnow`.

Logs: 10.6/10.7 in `/var/log/kernel.log`; 10.8/10.9 in `/var/log/system.log`. Prefix `QemuHDAAudio:`. A loaded kext often can't be unloaded ("classes have instances"); reboot after installing.

### Settings (installed kext's Info.plist)

| Key | Default | Effect |
|---|---|---|
| `QemuHDASampleRate` | 44100 | initial rate (44100 or 48000) |
| `QemuHDADebug` | 1 | 0 errors; 1 startup; 2 stream events + stats every 2 s (wraps, late polls, max poll gap, io calls/silent calls, max io = HAL buffer size, `lead` = distance between DMA and CoreAudio's write/read: ~sample offset when healthy, ~ring size when CoreAudio is late); 3 every verb |
| `QemuHDASampleOffset` | 4096 | frames CoreAudio writes ahead of the DMA (~93 ms at 44.1 kHz). Range 64–8192 |
| `QemuHDASampleLatency` | 32 | reported latency (A/V sync only) |
| `QemuHDAPollIntervalMs` | 5 | LPIB polling interval |
| `QemuHDAIdleStopSec` | 0 | seconds of silence before the output DMA stops; 0 = keep it running |
| `QemuHDAInput` | 1 | publish the codec's input (hda-duplex/hda-micro) as "Line In"; 0 = output only |
| `QemuHDAAllowAnyCodec` | 0 | accept a non-QEMU codec (testing only) |

After editing: `sudo touch /System/Library/Extensions` and reboot.

## What to check first if something breaks

1. **Doesn't load** → `kextutil -n -t` shows why: missing symbol (see `make check`), bad load command, dependency.
2. **Loads but not in Sound** → `ioreg -l -w0 -r -c QemuHDAAudioDevice`; `QemuHDADebug=2` logs GCAP, STATESTS, codec vendor, widgets.
3. **Silence** → with `QemuHDADebug=2`, check "output stream started" and that `wraps` grows. If wraps stay 0, the DMA isn't running (Bus Master, BDL, stream tag on the codec).
4. **Stops after ~2 s / ~7 wraps per 2 s** → guest clock, see above (`sysctl machdep.tsc.frequency`).
5. **Stutters** → raise `QemuHDASampleOffset` (8192); the stats' max poll gap measures guest latency.
6. **No microphone** → backend must be Default/SPICE, not CoreAudio; UTM needs microphone permission on the host.

## Next steps

- Test 10.7, 10.8, 10.9 (same binary; check `machdep.tsc.frequency` and whether the commpage TSC threshold also applies).
- Optional: use two codecs (output on CoreAudio, `hda-micro` on SPICE at another `cad`) — the driver currently uses only the first QEMU codec.

## Conventions

- Don't reformat whole files; keep changes small and focused.
- Don't build locally (see the Build rule); validate changes through GitHub Actions.
- Scripts in `guest/` must run on 10.6's bash 3.2 (no complex `[[ =~ ]]`, no `mapfile`, no `${var,,}`) and Python 2.6.
