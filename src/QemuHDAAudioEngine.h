/*
 * QemuHDAAudioEngine.h
 * IOAudioEngine with a single 16-bit stereo output stream.
 *
 * Rates: 44.1 kHz (default) and 48 kHz, selectable in Audio MIDI Setup.
 *
 * Design goal: survive the large, irregular scheduling latency of a guest
 * running under QEMU TCG (x86 emulated on Apple Silicon). Therefore:
 *   - a large DMA ring (QHDA_NUM_FRAMES: ~370 ms at 44.1 kHz, ~341 ms at 48 kHz),
 *   - a large, configurable sample offset so CoreAudio writes far ahead of
 *     the DMA read position,
 *   - no interrupts: a work-loop timer polls LPIB and back-dates each
 *     buffer-wrap timestamp using the position it observes, so a late poll
 *     does not translate into a late (jittery) timestamp.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef QEMU_HDA_AUDIO_ENGINE_H
#define QEMU_HDA_AUDIO_ENGINE_H

#include <IOKit/audio/IOAudioEngine.h>
#include <IOKit/IOTimerEventSource.h>

class QemuHDAAudioDevice;

class QemuHDAAudioEngine : public IOAudioEngine
{
    OSDeclareDefaultStructors(QemuHDAAudioEngine)

public:
    virtual bool init(QemuHDAAudioDevice *device);
    virtual void free();

    virtual bool initHardware(IOService *provider);
    virtual void stop(IOService *provider);

    virtual IOReturn performAudioEngineStart();
    virtual IOReturn performAudioEngineStop();
    virtual UInt32   getCurrentSampleFrame();

    virtual IOReturn performFormatChange(IOAudioStream *audioStream,
                                         const IOAudioStreamFormat *newFormat,
                                         const IOAudioSampleRate *newSampleRate);

    virtual IOReturn clipOutputSamples(const void *mixBuf, void *sampleBuf,
                                       UInt32 firstSampleFrame, UInt32 numSampleFrames,
                                       const IOAudioStreamFormat *streamFormat,
                                       IOAudioStream *audioStream);

    /* Control handlers (static trampolines). */
    static IOReturn volumeChangeHandler(OSObject *target, IOAudioControl *control,
                                        SInt32 oldValue, SInt32 newValue);
    static IOReturn muteChangeHandler(OSObject *target, IOAudioControl *control,
                                      SInt32 oldValue, SInt32 newValue);

private:
    static void timerFired(OSObject *owner, IOTimerEventSource *sender);
    void   pollPosition();
    void   resetPositionTracking();
    void   resetClipStats();
    void   logStats(const char *what, UInt32 pos);
    bool   createControls();

    QemuHDAAudioDevice  *fDevice;
    IOTimerEventSource  *fTimer;
    IOAudioStream       *fOutStream;

    UInt32               fPollMs;
    UInt32               fLastPosBytes;
    bool                 fRunning;

    volatile SInt32      fVolume;       /* 0 .. QHDA_VOLUME_MAX */
    volatile bool        fMuted;

    /* statistics (debug level >= 2) */
    UInt64               fLastPollTime; /* abs time */
    UInt64               fMaxPollGapNs;
    UInt64               fStatsStart;   /* abs time */
    UInt32               fWraps;
    UInt32               fLatePolls;

    /* clip statistics (debug level >= 2), reset every stats period.
     * "lead" = frames between the DMA read position and the first frame
     * CoreAudio clips, modulo the ring: ~sample offset when healthy, close
     * to the ring size when CoreAudio writes behind the DMA (too late). */
    UInt32               fClipCalls;
    UInt32               fClipSilent;   /* calls whose mix data was all zero */
    UInt64               fClipFrames;
    UInt32               fClipMaxFrames;
    UInt32               fLeadMin;
    UInt32               fLeadMax;
};

#define QHDA_VOLUME_MAX 64   /* 0.75 dB per step, 0 = -inf, 1 = -47.25 dB */

#endif /* QEMU_HDA_AUDIO_ENGINE_H */
