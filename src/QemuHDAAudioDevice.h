/*
 * QemuHDAAudioDevice.h
 * IOAudioDevice for QEMU's emulated Intel HDA controller (intel-hda /
 * ich9-intel-hda) driving the hda-output, hda-duplex or hda-micro codec.
 * Owns the PCI device, the register mapping, the DMA buffers and all
 * controller/codec programming. The engines (QemuHDAAudioEngine, one per
 * direction) only ask it to start/stop their stream and to read the
 * current DMA position.
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

/* Stream tags are looked up per direction (QEMU: output streams SD4-7,
 * input streams SD0-3); distinct values anyway, for readability in traces. */
#define QHDA_OUT_STREAM_TAG     1
#define QHDA_IN_STREAM_TAG      2

enum QemuHDADirection {
    kQemuHDAOutput = 0,
    kQemuHDAInput  = 1,
    kQemuHDANumDirections
};

class QemuHDAAudioDevice : public IOAudioDevice
{
    OSDeclareDefaultStructors(QemuHDAAudioDevice)

public:
    virtual bool initHardware(IOService *provider);
    virtual void stop(IOService *provider);
    virtual void free();

    virtual IOReturn performPowerStateChange(IOAudioDevicePowerState oldPowerState,
                                             IOAudioDevicePowerState newPowerState,
                                             UInt32 *microsecondsUntilComplete);

    /* Called by the engines. */
    static bool isSupportedRate(UInt32 rate) { return rate == 44100 || rate == 48000; }
    bool     hasStream(QemuHDADirection dir) const { return fStream[dir].present; }
    UInt32   getRate(QemuHDADirection dir) const { return fStream[dir].rate; }
    void     setRate(QemuHDADirection dir, UInt32 rate);     /* stream must be stopped */
    IOReturn startStream(QemuHDADirection dir);
    void     stopStream(QemuHDADirection dir);
    UInt32   readPositionBytes(QemuHDADirection dir);         /* LPIB, 0 .. QHDA_BUFFER_BYTES-1 */
    void    *getBuffer(QemuHDADirection dir) const { return fStream[dir].ringVirt; }
    UInt32   getDebugLevel() const { return fDebugLevel; }
    UInt32   getUIntProperty(const char *key, UInt32 defaultValue, UInt32 minValue, UInt32 maxValue);

private:
    struct StreamState {
        bool                      present;      /* codec path + controller stream exist */
        bool                      running;
        UInt32                    sdBase;       /* offset of the stream descriptor */
        UInt32                    tag;
        UInt32                    converter;    /* DAC or ADC nid */
        UInt32                    pin;
        UInt32                    ampSteps;     /* converter amp (0 = none) */
        UInt32                    rate;
        UInt16                    formatWord;
        IOBufferMemoryDescriptor *ringDesc;
        IOBufferMemoryDescriptor *bdlDesc;
        void                     *ringVirt;
        UInt64                    ringPhys;
        void                     *bdlVirt;
        UInt64                    bdlPhys;
    };

    /* MMIO helpers */
    UInt32 rd32(UInt32 off) { return *(volatile UInt32 *)(fRegs + off); }
    UInt16 rd16(UInt32 off) { return *(volatile UInt16 *)(fRegs + off); }
    void   wr32(UInt32 off, UInt32 v) { *(volatile UInt32 *)(fRegs + off) = v; }
    void   wr16(UInt32 off, UInt16 v) { *(volatile UInt16 *)(fRegs + off) = v; }

    bool   initController();
    bool   allocateDMABuffers(QemuHDADirection dir);
    void   freeDMABuffers();
    bool   discoverCodec();
    bool   configureCodec();
    bool   resetStream(QemuHDADirection dir);
    bool   reinitAfterWake();
    bool   codecCommand(UInt32 nid, UInt32 verb12, UInt32 data8, UInt32 *response);
    bool   codecCommand4(UInt32 nid, UInt32 verb4, UInt32 data16, UInt32 *response);
    bool   sendImmediate(UInt32 command, UInt32 *response);
    UInt32 getParam(UInt32 nid, UInt32 param);

    bool   createEngine(QemuHDADirection dir);

    IOPCIDevice              *fPCI;
    IOMemoryMap              *fRegMap;
    volatile UInt8           *fRegs;

    StreamState               fStream[kQemuHDANumDirections];
    QemuHDAAudioEngine       *fEngine[kQemuHDANumDirections];  /* not retained; owned by IOAudioDevice */

    UInt32                    fCodecAddr;
    UInt32                    fCodecVendor;
    UInt32                    fAFG;
    UInt32                    fDebugLevel;
};

#endif /* QEMU_HDA_AUDIO_DEVICE_H */
