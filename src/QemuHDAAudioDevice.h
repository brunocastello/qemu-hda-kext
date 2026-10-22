/*
 * QemuHDAAudioDevice.h
 * IOAudioDevice for QEMU's emulated Intel HDA controller (intel-hda /
 * ich9-intel-hda) driving the hda-output codec. Owns the PCI device, the
 * register mapping, the DMA buffers and all controller/codec programming.
 * The engine (QemuHDAAudioEngine) only asks it to start/stop the stream and
 * to read the current DMA position.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef QEMU_HDA_AUDIO_DEVICE_H
#define QEMU_HDA_AUDIO_DEVICE_H

#include <IOKit/audio/IOAudioDevice.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

#ifndef QHDA_VERSION_STRING
#define QHDA_VERSION_STRING "0.0.0-dev"
#endif

class QemuHDAAudioEngine;

/* Stream geometry: 16-bit stereo at 44.1 kHz (default, QEMU's own default
 * audiodev rate, so QEMU does not resample) or 48 kHz. */
#define QHDA_DEFAULT_RATE       44100
#define QHDA_CHANNELS           2
#define QHDA_BYTES_PER_FRAME    4
#define QHDA_NUM_FRAMES         16384                                   /* ~370 ms @44.1k */
#define QHDA_BUFFER_BYTES       (QHDA_NUM_FRAMES * QHDA_BYTES_PER_FRAME)  /* 64 KiB */
#define QHDA_BDL_ENTRIES        4
#define QHDA_STREAM_TAG         1

class QemuHDAAudioDevice : public IOAudioDevice
{
    OSDeclareDefaultStructors(QemuHDAAudioDevice)

public:
    virtual bool initHardware(IOService *provider);
    virtual void stop(IOService *provider);
    virtual void free();

    /* Called by the engine. */
    static bool isSupportedRate(UInt32 rate) { return rate == 44100 || rate == 48000; }
    UInt32   getOutputRate() const { return fRate; }
    void     setOutputRate(UInt32 rate);         /* stream must be stopped */
    IOReturn startOutputStream();
    void     stopOutputStream();
    UInt32   readOutputPositionBytes();      /* LPIB, 0 .. QHDA_BUFFER_BYTES-1 */
    void    *getOutputBuffer() const { return fRingVirt; }
    UInt32   getDebugLevel() const { return fDebugLevel; }
    UInt32   getUIntProperty(const char *key, UInt32 defaultValue, UInt32 minValue, UInt32 maxValue);

private:
    /* MMIO helpers */
    UInt32 rd32(UInt32 off) { return *(volatile UInt32 *)(fRegs + off); }
    UInt16 rd16(UInt32 off) { return *(volatile UInt16 *)(fRegs + off); }
    void   wr32(UInt32 off, UInt32 v) { *(volatile UInt32 *)(fRegs + off) = v; }
    void   wr16(UInt32 off, UInt16 v) { *(volatile UInt16 *)(fRegs + off) = v; }

    bool   initController();
    bool   allocateDMABuffers();
    void   freeDMABuffers();
    bool   discoverCodec();
    bool   configureCodec();
    bool   resetOutputStream();
    bool   codecCommand(UInt32 nid, UInt32 verb12, UInt32 data8, UInt32 *response);
    bool   codecCommand4(UInt32 nid, UInt32 verb4, UInt32 data16, UInt32 *response);
    bool   sendImmediate(UInt32 command, UInt32 *response);
    UInt32 getParam(UInt32 nid, UInt32 param);

    bool   createEngine();

    IOPCIDevice              *fPCI;
    IOMemoryMap              *fRegMap;
    volatile UInt8           *fRegs;

    IOBufferMemoryDescriptor *fRingDesc;
    IOBufferMemoryDescriptor *fBDLDesc;
    void                     *fRingVirt;
    UInt64                    fRingPhys;
    void                     *fBDLVirt;
    UInt64                    fBDLPhys;

    UInt32                    fOutSDBase;        /* offset of output SD0 */
    UInt32                    fCodecAddr;
    UInt32                    fCodecVendor;
    UInt32                    fAFG;
    UInt32                    fDAC;
    UInt32                    fPin;
    UInt32                    fDACAmpSteps;
    UInt32                    fDebugLevel;
    UInt32                    fRate;
    UInt16                    fFormatWord;
    bool                      fStreamRunning;
};

#endif /* QEMU_HDA_AUDIO_DEVICE_H */
