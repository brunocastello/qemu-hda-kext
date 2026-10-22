/*
 * QemuHDAAudioEngine.cpp
 *
 * SPDX-License-Identifier: MIT
 */

#include "QemuHDAAudioEngine.h"
#include "QemuHDAAudioDevice.h"
#include "QemuHDAGainTable.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/audio/IOAudioStream.h>
#include <IOKit/audio/IOAudioLevelControl.h>
#include <IOKit/audio/IOAudioToggleControl.h>
#include <IOKit/audio/IOAudioDefines.h>
#include <kern/clock.h>

#define super IOAudioEngine
OSDefineMetaClassAndStructors(QemuHDAAudioEngine, IOAudioEngine)

#define QLOG(lvl, fmt, args...) \
    do { if (fDevice && fDevice->getDebugLevel() >= (lvl)) \
         IOLog("QemuHDAAudio: " fmt "\n", ## args); } while (0)

#define QHDA_STATS_PERIOD_NS (2ULL * 1000000000ULL)

static inline UInt64 absToNs(UInt64 abs)
{
    UInt64 ns;
    absolutetime_to_nanoseconds(abs, &ns);
    return ns;
}

static inline UInt64 nsToAbs(UInt64 ns)
{
    UInt64 abs;
    nanoseconds_to_absolutetime(ns, &abs);
    return abs;
}

/* ------------------------------------------------------------------------ */

bool QemuHDAAudioEngine::init(QemuHDAAudioDevice *device)
{
    if (!device || !super::init(NULL)) {
        return false;
    }
    fDevice      = device;
    fVolume      = QHDA_VOLUME_MAX;
    fMuted       = false;
    fRunning     = false;
    fDMAActive   = false;
    fIdleErasing = false;
    return true;
}

void QemuHDAAudioEngine::free()
{
    if (fTimer) {
        fTimer->release();
        fTimer = NULL;
    }
    super::free();
}

bool QemuHDAAudioEngine::initHardware(IOService *provider)
{
    IOAudioSampleRate rate;
    IOWorkLoop *wl;

    if (!super::initHardware(provider)) {
        return false;
    }

    UInt32 sampleOffset  = fDevice->getUIntProperty("QemuHDASampleOffset", 4096, 64, QHDA_NUM_FRAMES / 2);
    UInt32 sampleLatency = fDevice->getUIntProperty("QemuHDASampleLatency", 32, 0, 8192);
    fPollMs              = fDevice->getUIntProperty("QemuHDAPollIntervalMs", 5, 1, 50);
    fIdleStopSec         = fDevice->getUIntProperty("QemuHDAIdleStopSec", 0, 0, 3600);

    rate.whole = fDevice->getOutputRate();
    rate.fraction = 0;
    setSampleRate(&rate);
    setNumSampleFramesPerBuffer(QHDA_NUM_FRAMES);
    setSampleOffset(sampleOffset);
    setOutputSampleLatency(sampleLatency);
    setDescription("QEMU HDA Line Out");

    /* Output stream: the DMA ring itself is the sample buffer. */
    fOutStream = new IOAudioStream;
    if (!fOutStream || !fOutStream->initWithAudioEngine(this, kIOAudioStreamDirectionOutput, 1, "Line Out")) {
        QLOG(0, "cannot create output stream");
        if (fOutStream) { fOutStream->release(); fOutStream = NULL; }
        return false;
    }

    IOAudioStreamFormat format = {
        QHDA_CHANNELS,                                  /* fNumChannels */
        kIOAudioStreamSampleFormatLinearPCM,            /* fSampleFormat */
        kIOAudioStreamNumericRepresentationSignedInt,   /* fNumericRepresentation */
        16,                                             /* fBitDepth */
        16,                                             /* fBitWidth */
        kIOAudioStreamAlignmentHighByte,                /* fAlignment (unused) */
        kIOAudioStreamByteOrderLittleEndian,            /* fByteOrder */
        true,                                           /* fIsMixable */
        0                                               /* fDriverTag */
    };

    fOutStream->setSampleBuffer(fDevice->getOutputBuffer(), QHDA_BUFFER_BYTES);
    {
        /* Both rates QEMU's codec handles natively. 44.1 kHz matches QEMU's
         * default -audiodev frequency, so no resampling happens in QEMU. */
        IOAudioSampleRate r44 = { 44100, 0 };
        IOAudioSampleRate r48 = { 48000, 0 };
        fOutStream->addAvailableFormat(&format, &r44, &r44);
        fOutStream->addAvailableFormat(&format, &r48, &r48);
    }
    fOutStream->setFormat(&format);
    fOutStream->setTerminalType(EXTERNAL_LINE_CONNECTOR);
    addAudioStream(fOutStream);
    fOutStream->release();          /* the engine keeps its own reference */

    if (!createControls()) {
        return false;
    }

    wl = getWorkLoop();
    if (!wl) {
        return false;
    }
    fTimer = IOTimerEventSource::timerEventSource(this, QemuHDAAudioEngine::timerFired);
    if (!fTimer || wl->addEventSource(fTimer) != kIOReturnSuccess) {
        QLOG(0, "cannot create poll timer");
        return false;
    }

    QLOG(1, "engine ready: %u Hz, %u frames ring, sample offset %u, latency %u, poll %u ms",
         (unsigned)rate.whole, (unsigned)QHDA_NUM_FRAMES, (unsigned)sampleOffset,
         (unsigned)sampleLatency, (unsigned)fPollMs);
    return true;
}

bool QemuHDAAudioEngine::createControls()
{
    IOAudioControl *control;

    control = IOAudioLevelControl::createVolumeControl(
        QHDA_VOLUME_MAX,            /* initial */
        0,                          /* min */
        QHDA_VOLUME_MAX,            /* max */
        -(48 << 16),                /* min dB (IOFixed), 0.75 dB per step */
        0,                          /* max dB */
        kIOAudioControlChannelIDAll,
        kIOAudioControlChannelNameAll,
        0,
        kIOAudioControlUsageOutput);
    if (!control) {
        return false;
    }
    control->setValueChangeHandler((IOAudioControl::IntValueChangeHandler)volumeChangeHandler, this);
    addDefaultAudioControl(control);
    control->release();

    control = IOAudioToggleControl::createMuteControl(
        false,
        kIOAudioControlChannelIDAll,
        kIOAudioControlChannelNameAll,
        0,
        kIOAudioControlUsageOutput);
    if (!control) {
        return false;
    }
    control->setValueChangeHandler((IOAudioControl::IntValueChangeHandler)muteChangeHandler, this);
    addDefaultAudioControl(control);
    control->release();

    return true;
}

void QemuHDAAudioEngine::stop(IOService *provider)
{
    if (fDMAActive) {
        stopDMA();
    }
    if (fTimer) {
        fTimer->cancelTimeout();
        IOWorkLoop *wl = getWorkLoop();
        if (wl) {
            wl->removeEventSource(fTimer);
        }
        fTimer->release();
        fTimer = NULL;
    }
    super::stop(provider);
}

/* ---- Start / stop ------------------------------------------------------- */

IOReturn QemuHDAAudioEngine::startDMA()
{
    /* A fresh DMA start begins at frame 0 of a silent ring. */
    if (fOutStream) {
        void *buf = fOutStream->getSampleBuffer();
        if (buf) bzero(buf, fOutStream->getSampleBufferSize());
    }

    IOReturn r = fDevice->startOutputStream();
    if (r != kIOReturnSuccess) {
        QLOG(0, "cannot start output stream: 0x%x", r);
        return r;
    }
    fDMAActive   = true;
    fIdleErasing = false;
    if (fTimer) {
        fTimer->setTimeoutMS(fPollMs);
    }
    return kIOReturnSuccess;
}

void QemuHDAAudioEngine::stopDMA()
{
    if (fTimer) {
        fTimer->cancelTimeout();
    }
    fDevice->stopOutputStream();
    fDMAActive   = false;
    fIdleErasing = false;
}

IOReturn QemuHDAAudioEngine::performAudioEngineStart()
{
    bool resumed = fDMAActive;

    if (!fDMAActive) {
        IOReturn r = startDMA();
        if (r != kIOReturnSuccess) {
            return r;
        }
    }
    fIdleErasing = false;

    /* The mix buffer still holds what the previous session mixed ahead of
     * the play head; clients add into it, so it must start out silent. The
     * ring itself is already silent behind the DMA (idle erase), and the
     * part ahead of it is the tail of the previous sound, still playing. */
    if (fOutStream) {
        void *buf = fOutStream->getMixBuffer();
        if (buf) bzero(buf, fOutStream->getMixBufferSize());
    }

    resetPositionTracking();
    fRunning = true;
    QLOG(2, "engine started (%s DMA)", resumed ? "running" : "new");
    return kIOReturnSuccess;
}

IOReturn QemuHDAAudioEngine::performAudioEngineStop()
{
    fRunning = false;

    /* Keep the DMA running: what CoreAudio wrote ahead of the play head
     * (about the sample offset) still gets played, and the next start
     * finds a steady stream. Erase behind the DMA from here on. */
    fIdleErasePos    = fDevice->readOutputPositionBytes();
    fIdleErasedBytes = 0;
    fIdleErasing     = true;
    clock_get_uptime(&fIdleSince);

    if (fDevice->getDebugLevel() >= 2) {
        logStats("engine stopped", fIdleErasePos);
    }
    return kIOReturnSuccess;
}

void QemuHDAAudioEngine::eraseSamples(UInt32 fromBytes, UInt32 toBytes)
{
    UInt8 *ring = fOutStream ? (UInt8 *)fOutStream->getSampleBuffer() : NULL;
    if (!ring) {
        return;
    }
    if (toBytes >= fromBytes) {
        bzero(ring + fromBytes, toBytes - fromBytes);
    } else {
        bzero(ring + fromBytes, QHDA_BUFFER_BYTES - fromBytes);
        bzero(ring, toBytes);
    }
}

void QemuHDAAudioEngine::idlePoll()
{
    UInt32 pos = fDevice->readOutputPositionBytes();

    if (fIdleErasing) {
        UInt32 advanced = (pos + QHDA_BUFFER_BYTES - fIdleErasePos) % QHDA_BUFFER_BYTES;
        eraseSamples(fIdleErasePos, pos);
        fIdleErasePos     = pos;
        fIdleErasedBytes += advanced;
        if (fIdleErasedBytes >= QHDA_BUFFER_BYTES) {
            /* The DMA went once around the ring since the stop: all of it,
             * including the tail that was ahead of the play head, has been
             * played and erased. */
            fIdleErasing = false;
        }
    }

    if (fIdleStopSec) {
        UInt64 now;
        clock_get_uptime(&now);
        if (absToNs(now - fIdleSince) >= (UInt64)fIdleStopSec * 1000000000ULL) {
            stopDMA();      /* startDMA() clears the ring before restarting */
            QLOG(2, "output stream idle for %u s, DMA stopped", (unsigned)fIdleStopSec);
        }
    }
}

UInt32 QemuHDAAudioEngine::getCurrentSampleFrame()
{
    return fDevice->readOutputPositionBytes() / QHDA_BYTES_PER_FRAME;
}

IOReturn QemuHDAAudioEngine::performFormatChange(IOAudioStream *audioStream,
                                                 const IOAudioStreamFormat *newFormat,
                                                 const IOAudioSampleRate *newSampleRate)
{
    /* Only the sample rate can change (one sample format is published). */
    if (!newSampleRate || newSampleRate->whole == fDevice->getOutputRate()) {
        return kIOReturnSuccess;
    }
    if (!QemuHDAAudioDevice::isSupportedRate(newSampleRate->whole)) {
        return kIOReturnUnsupported;
    }

    QLOG(1, "sample rate change %u -> %u Hz", (unsigned)fDevice->getOutputRate(),
         (unsigned)newSampleRate->whole);

    /* The HDA stream format may only change while the stream is stopped.
     * An idle (silent) DMA is simply left stopped until the next start. */
    if (fDMAActive) {
        stopDMA();
    }
    fDevice->setOutputRate(newSampleRate->whole);
    if (fRunning) {
        IOReturn r = startDMA();
        if (r != kIOReturnSuccess) {
            QLOG(0, "cannot restart output stream after rate change: 0x%x", r);
            fRunning = false;
            return r;
        }
        resetPositionTracking();
    }
    return kIOReturnSuccess;
}

void QemuHDAAudioEngine::resetPositionTracking()
{
    UInt64 now;
    UInt32 pos = fDevice->readOutputPositionBytes();
    const UInt64 bytesPerSec = (UInt64)fDevice->getOutputRate() * QHDA_BYTES_PER_FRAME;

    clock_get_uptime(&now);
    fLastPosBytes = pos;
    fLastPollTime = now;
    fStatsStart   = now;
    fMaxPollGapNs = 0;
    fWraps        = 0;
    fLatePolls    = 0;
    resetClipStats();

    /* Loop 0 began when the DMA was last at frame 0: 'pos' bytes ago. On a
     * fresh start pos is ~0; on a resumed (already running) DMA this
     * anchors CoreAudio to the real, steady position right away. */
    AbsoluteTime ts = now - nsToAbs((UInt64)pos * 1000000000ULL / bytesPerSec);
    takeTimeStamp(false, &ts);
}

/* ---- Position polling / timestamps -------------------------------------- */

void QemuHDAAudioEngine::timerFired(OSObject *owner, IOTimerEventSource *sender)
{
    QemuHDAAudioEngine *self = OSDynamicCast(QemuHDAAudioEngine, owner);
    if (!self || !self->fDMAActive) {
        return;
    }
    if (self->fRunning) {
        self->pollPosition();
    } else {
        self->idlePoll();
    }
    if (self->fDMAActive && sender) {
        sender->setTimeoutMS(self->fPollMs);
    }
}

void QemuHDAAudioEngine::pollPosition()
{
    UInt64 now;
    clock_get_uptime(&now);

    UInt64 gapNs = absToNs(now - fLastPollTime);
    fLastPollTime = now;
    if (gapNs > fMaxPollGapNs) {
        fMaxPollGapNs = gapNs;
    }
    if (gapNs > (UInt64)fPollMs * 3ULL * 1000000ULL) {
        fLatePolls++;
    }

    UInt32 pos = fDevice->readOutputPositionBytes();
    const UInt64 bytesPerSec = (UInt64)fDevice->getOutputRate() * QHDA_BYTES_PER_FRAME;
    const UInt64 ringNs      = (UInt64)QHDA_NUM_FRAMES * 1000000000ULL / fDevice->getOutputRate();

    /*
     * How many times did the DMA engine wrap since the last poll?
     * Normally 0 or 1 (detected by the position going backwards). If the
     * poll was delayed by more than a whole ring, estimate the count from
     * the elapsed time.
     */
    UInt32 wraps = 0;
    if (gapNs >= ringNs) {
        UInt64 estBytes = gapNs * bytesPerSec / 1000000000ULL;
        UInt64 travelled = (UInt64)fLastPosBytes + estBytes;
        if (travelled > pos) {
            wraps = (UInt32)((travelled - pos + QHDA_BUFFER_BYTES / 2) / QHDA_BUFFER_BYTES);
        }
        QLOG(1, "poll delayed %llu ms (> ring), assuming %u wrap(s)",
             (unsigned long long)(gapNs / 1000000ULL), (unsigned)wraps);
    }
    if (pos < fLastPosBytes && wraps == 0) {
        wraps = 1;
    }

    if (wraps) {
        /*
         * The most recent wrap happened 'pos' bytes ago. Back-date the
         * timestamp accordingly, so that poll latency does not show up as
         * timestamp jitter. Older wraps (if any) were one ring earlier each.
         */
        UInt64 sinceWrapNs = (UInt64)pos * 1000000000ULL / bytesPerSec;
        for (UInt32 k = wraps; k > 0; k--) {
            UInt64 backNs = sinceWrapNs + (UInt64)(k - 1) * ringNs;
            AbsoluteTime ts = now - nsToAbs(backNs);
            takeTimeStamp(true, &ts);
            fWraps++;
        }
    }
    fLastPosBytes = pos;

    if (fDevice->getDebugLevel() >= 2 && absToNs(now - fStatsStart) >= QHDA_STATS_PERIOD_NS) {
        logStats("stats", pos);
        fStatsStart   = now;
        fMaxPollGapNs = 0;
        fLatePolls    = 0;
        resetClipStats();
    }
}

void QemuHDAAudioEngine::resetClipStats()
{
    fClipCalls     = 0;
    fClipSilent    = 0;
    fClipFrames    = 0;
    fClipMaxFrames = 0;
    fLeadMin       = 0xFFFFFFFF;
    fLeadMax       = 0;
}

void QemuHDAAudioEngine::logStats(const char *what, UInt32 pos)
{
    IOLog("QemuHDAAudio: %s: wraps %u, late polls %u, max poll gap %llu us, pos %u | "
          "clips %u (silent %u), frames %llu, max clip %u, lead %u..%u\n",
          what, (unsigned)fWraps, (unsigned)fLatePolls,
          (unsigned long long)(fMaxPollGapNs / 1000), (unsigned)pos,
          (unsigned)fClipCalls, (unsigned)fClipSilent, (unsigned long long)fClipFrames,
          (unsigned)fClipMaxFrames, (unsigned)(fClipCalls ? fLeadMin : 0), (unsigned)fLeadMax);
}

/* ---- Controls ----------------------------------------------------------- */

IOReturn QemuHDAAudioEngine::volumeChangeHandler(OSObject *target, IOAudioControl *control,
                                                 SInt32 oldValue, SInt32 newValue)
{
    QemuHDAAudioEngine *self = OSDynamicCast(QemuHDAAudioEngine, target);
    if (!self) return kIOReturnBadArgument;
    if (newValue < 0) newValue = 0;
    if (newValue > QHDA_VOLUME_MAX) newValue = QHDA_VOLUME_MAX;
    self->fVolume = newValue;
    return kIOReturnSuccess;
}

IOReturn QemuHDAAudioEngine::muteChangeHandler(OSObject *target, IOAudioControl *control,
                                               SInt32 oldValue, SInt32 newValue)
{
    QemuHDAAudioEngine *self = OSDynamicCast(QemuHDAAudioEngine, target);
    if (!self) return kIOReturnBadArgument;
    self->fMuted = (newValue != 0);
    return kIOReturnSuccess;
}

/* ---- Clip (float mix buffer -> SInt16 DMA ring) ------------------------- */

IOReturn QemuHDAAudioEngine::clipOutputSamples(const void *mixBuf, void *sampleBuf,
                                               UInt32 firstSampleFrame, UInt32 numSampleFrames,
                                               const IOAudioStreamFormat *streamFormat,
                                               IOAudioStream *audioStream)
{
    const UInt32 ch    = streamFormat->fNumChannels;
    const float *in    = (const float *)mixBuf + (firstSampleFrame * ch);
    SInt16      *out   = (SInt16 *)sampleBuf + (firstSampleFrame * ch);
    const UInt32 count = numSampleFrames * ch;

    SInt32 vol = fVolume;
    if (vol < 0) vol = 0;
    if (vol > QHDA_VOLUME_MAX) vol = QHDA_VOLUME_MAX;
    const float gain = fMuted ? 0.0f : kQemuHDAGainTable[vol];
    bool silent = true;

    if (fDevice->getDebugLevel() >= 2) {
        UInt32 dmaFrame = fDevice->readOutputPositionBytes() / QHDA_BYTES_PER_FRAME;
        UInt32 lead = (firstSampleFrame + QHDA_NUM_FRAMES - dmaFrame) % QHDA_NUM_FRAMES;
        fClipCalls++;
        fClipFrames += numSampleFrames;
        if (numSampleFrames > fClipMaxFrames) fClipMaxFrames = numSampleFrames;
        if (lead < fLeadMin) fLeadMin = lead;
        if (lead > fLeadMax) fLeadMax = lead;
    }

    for (UInt32 i = 0; i < count; i++) {
        if (in[i] != 0.0f) silent = false;
        float s = in[i] * gain;
        if (s > 1.0f) {
            s = 1.0f;
        } else if (s < -1.0f) {
            s = -1.0f;
        }
        out[i] = (SInt16)(s * 32767.0f);
    }
    if (silent && fDevice->getDebugLevel() >= 2) {
        fClipSilent++;
    }
    return kIOReturnSuccess;
}
