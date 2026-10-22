#!/bin/bash
# Fetch build dependencies into deps/ (idempotent).
#
#  - MacKernelSDK (acidanthera): lets a modern Xcode build kexts for old
#    kernels; provides libkmod.a for 10.6+ x86_64 and the IOKit headers.
#  - IOAudioFamily headers from the Mac OS X 10.6 era (Apple open source,
#    APSL 2.0). We compile against the OLDEST IOAudioFamily we support so the
#    kext never references a symbol/vtable slot that 10.6 does not have;
#    newer IOAudioFamily releases stay binary compatible with it.
#
# Both are pinned for reproducible builds. Override with env vars if needed.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEPS="$ROOT/deps"

MKSDK_REPO="${MKSDK_REPO:-https://github.com/acidanthera/MacKernelSDK.git}"
MKSDK_REV="${MKSDK_REV:-3f750085caa17ec3a7880f11c11bf4f48cd6a164}"

IOAF_REPO="${IOAF_REPO:-https://github.com/apple-oss-distributions/IOAudioFamily.git}"
IOAF_TAG="${IOAF_TAG:-IOAudioFamily-183.4.11}"   # CFBundleVersion 1.8.3fc11 (Snow Leopard)

IOAF_HEADERS="IOAudioControl.h IOAudioControlUserClient.h IOAudioDebug.h IOAudioDefines.h
IOAudioDevice.h IOAudioEngine.h IOAudioEngineUserClient.h IOAudioLevelControl.h IOAudioPort.h
IOAudioSelectorControl.h IOAudioStream.h IOAudioToggleControl.h IOAudioTypes.h"

mkdir -p "$DEPS"

# --- MacKernelSDK ---------------------------------------------------------
if [ ! -d "$DEPS/MacKernelSDK/.git" ]; then
    echo ">> cloning MacKernelSDK"
    git clone --quiet "$MKSDK_REPO" "$DEPS/MacKernelSDK"
fi
git -C "$DEPS/MacKernelSDK" fetch --quiet origin "$MKSDK_REV" 2>/dev/null || true
git -c advice.detachedHead=false -C "$DEPS/MacKernelSDK" checkout --quiet "$MKSDK_REV"
echo ">> MacKernelSDK @ $(git -C "$DEPS/MacKernelSDK" rev-parse --short HEAD)"

# --- IOAudioFamily headers ------------------------------------------------
OVERLAY="$DEPS/ioaudio-overlay/IOKit/audio"
STAMP="$DEPS/ioaudio-overlay/.tag"
if [ ! -f "$STAMP" ] || [ "$(cat "$STAMP")" != "$IOAF_TAG" ]; then
    echo ">> fetching $IOAF_TAG headers"
    TMP="$(mktemp -d)"
    git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$IOAF_TAG" "$IOAF_REPO" "$TMP/IOAudioFamily"
    rm -rf "$DEPS/ioaudio-overlay"
    mkdir -p "$OVERLAY"
    for h in $IOAF_HEADERS; do
        cp "$TMP/IOAudioFamily/$h" "$OVERLAY/$h"
    done
    cp "$TMP/IOAudioFamily/APPLE_LICENSE" "$DEPS/ioaudio-overlay/APPLE_LICENSE"
    echo "$IOAF_TAG" > "$STAMP"
    rm -rf "$TMP"
fi
echo ">> IOAudioFamily headers: $(cat "$STAMP")"
