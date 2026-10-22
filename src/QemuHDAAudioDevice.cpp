/*
 * QemuHDAAudioDevice.cpp
 *
 * SPDX-License-Identifier: MIT
 */

#include "QemuHDAAudioDevice.h"
#include "QemuHDAAudioEngine.h"
#include "QemuHDARegs.h"

#include <IOKit/IOLib.h>
#include <libkern/OSByteOrder.h>

#define super IOAudioDevice
OSDefineMetaClassAndStructors(QemuHDAAudioDevice, IOAudioDevice)

#define QLOG(lvl, fmt, args...) \
    do { if (fDebugLevel >= (lvl)) IOLog("QemuHDAAudio: " fmt "\n", ## args); } while (0)

/* Extra global registers not in QemuHDARegs.h (only used during init). */
#define HDA_REG_CORBCTL 0x4C
#define HDA_REG_RIRBCTL 0x5C

static const char *dirName(QemuHDADirection dir)
{
    return dir == kQemuHDAOutput ? "output" : "input";
}

/* ------------------------------------------------------------------------ */

UInt32 QemuHDAAudioDevice::getUIntProperty(const char *key, UInt32 defaultValue,
                                           UInt32 minValue, UInt32 maxValue)
{
    UInt32 value = defaultValue;
    OSNumber *num = OSDynamicCast(OSNumber, getProperty(key));
    if (num) {
        value = num->unsigned32BitValue();
    }
    if (value < minValue) value = minValue;
    if (value > maxValue) value = maxValue;
    return value;
}

bool QemuHDAAudioDevice::initHardware(IOService *provider)
{
    fDebugLevel = getUIntProperty("QemuHDADebug", 1, 0, 3);
    {
        UInt32 rate = getUIntProperty("QemuHDASampleRate", QHDA_DEFAULT_RATE, 0, 0xFFFFFFFF);
        if (!isSupportedRate(rate)) {
            rate = QHDA_DEFAULT_RATE;
        }
        setRate(kQemuHDAOutput, rate);
        setRate(kQemuHDAInput, rate);
    }
    fStream[kQemuHDAOutput].tag = QHDA_OUT_STREAM_TAG;
    fStream[kQemuHDAInput].tag  = QHDA_IN_STREAM_TAG;
    QLOG(1, "initHardware (build " QHDA_VERSION_STRING ")");

    if (!super::initHardware(provider)) {
        return false;
    }

    fPCI = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCI) {
        QLOG(0, "provider is not an IOPCIDevice");
        return false;
    }

    fPCI->setMemoryEnable(true);
    fPCI->setBusMasterEnable(true);

    fRegMap = fPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!fRegMap) {
        QLOG(0, "cannot map BAR0");
        return false;
    }
    fRegs = (volatile UInt8 *)fRegMap->getVirtualAddress();
    if (!fRegs) {
        QLOG(0, "BAR0 has no virtual address");
        return false;
    }

    if (!initController())    return false;
    if (!discoverCodec())     return false;
    if (!allocateDMABuffers(kQemuHDAOutput)) return false;
    if (fStream[kQemuHDAInput].present && !allocateDMABuffers(kQemuHDAInput)) {
        QLOG(0, "input disabled: cannot allocate its DMA buffers");
        fStream[kQemuHDAInput].present = false;
    }
    if (!configureCodec())    return false;
    if (!resetStream(kQemuHDAOutput)) return false;
    if (fStream[kQemuHDAInput].present && !resetStream(kQemuHDAInput)) {
        fStream[kQemuHDAInput].present = false;
    }

    setDeviceName("QEMU HDA Audio");
    setDeviceShortName("QEMUHDA");
    setManufacturerName("QEMU");
    setDeviceModelName("QemuHDAAudio");
    setDeviceTransportType(kIOAudioDeviceTransportTypePCI);

    if (!createEngine(kQemuHDAOutput)) {
        return false;
    }
    if (fStream[kQemuHDAInput].present && !createEngine(kQemuHDAInput)) {
        QLOG(0, "input disabled: cannot create its engine");
        fStream[kQemuHDAInput].present = false;
    }

    QLOG(1, "ready: codec cad=%u vendor=0x%08x afg=%u dac=%u pin=%u ampSteps=%u rate=%u; "
         "input: %s (adc=%u pin=%u ampSteps=%u)",
         (unsigned)fCodecAddr, (unsigned)fCodecVendor, (unsigned)fAFG,
         (unsigned)fStream[kQemuHDAOutput].converter, (unsigned)fStream[kQemuHDAOutput].pin,
         (unsigned)fStream[kQemuHDAOutput].ampSteps, (unsigned)fStream[kQemuHDAOutput].rate,
         fStream[kQemuHDAInput].present ? "yes" : "no",
         (unsigned)fStream[kQemuHDAInput].converter, (unsigned)fStream[kQemuHDAInput].pin,
         (unsigned)fStream[kQemuHDAInput].ampSteps);
    return true;
}

void QemuHDAAudioDevice::stop(IOService *provider)
{
    QLOG(1, "stop");
    if (fRegs) {
        for (int d = 0; d < kQemuHDANumDirections; d++) {
            if (fStream[d].running) {
                stopStream((QemuHDADirection)d);
            }
        }
    }
    fEngine[kQemuHDAOutput] = NULL;
    fEngine[kQemuHDAInput]  = NULL;
    super::stop(provider);
}

void QemuHDAAudioDevice::free()
{
    freeDMABuffers();
    if (fRegMap) {
        fRegMap->release();
        fRegMap = NULL;
        fRegs = NULL;
    }
    super::free();
}

/* ---- Power management ----------------------------------------------------
 * IOAudioFamily pauses running engines before calling us for sleep and
 * resumes them after we return from wake. On S3 resume QEMU resets every
 * device, so the controller, the codec and the stream descriptors must be
 * programmed again; the DMA buffers live in RAM and survive.
 */

IOReturn QemuHDAAudioDevice::performPowerStateChange(IOAudioDevicePowerState oldPowerState,
                                                     IOAudioDevicePowerState newPowerState,
                                                     UInt32 *microsecondsUntilComplete)
{
    if (newPowerState == kIOAudioDeviceSleep && oldPowerState != kIOAudioDeviceSleep) {
        QLOG(1, "going to sleep");
        for (int d = 0; d < kQemuHDANumDirections; d++) {
            if (fEngine[d]) {
                fEngine[d]->suspendForSleep();      /* stops a DMA left running while idle */
            }
            if (fRegs && fStream[d].running) {
                stopStream((QemuHDADirection)d);
            }
        }
    } else if (oldPowerState == kIOAudioDeviceSleep && newPowerState != kIOAudioDeviceSleep) {
        QLOG(1, "waking up");
        if (!reinitAfterWake()) {
            QLOG(0, "re-initialisation after wake failed; audio may stay silent");
        }
    }
    return kIOReturnSuccess;
}

bool QemuHDAAudioDevice::reinitAfterWake()
{
    if (!fRegs || !fPCI) {
        return false;
    }
    fPCI->setMemoryEnable(true);
    fPCI->setBusMasterEnable(true);

    if (!initController()) {
        return false;
    }
    /* Same codec, same nodes: only check it is still answering. */
    UInt32 vendor = getParam(0, HDA_PAR_VENDOR_ID);
    if (vendor != fCodecVendor) {
        QLOG(0, "codec at cad %u answered 0x%08x after wake (expected 0x%08x)",
             (unsigned)fCodecAddr, (unsigned)vendor, (unsigned)fCodecVendor);
        return false;
    }
    if (!configureCodec()) {
        return false;
    }
    for (int d = 0; d < kQemuHDANumDirections; d++) {
        fStream[d].running = false;
        if (fStream[d].present && !resetStream((QemuHDADirection)d)) {
            return false;
        }
    }
    return true;
}

/* ---- Controller --------------------------------------------------------- */

bool QemuHDAAudioDevice::initController()
{
    UInt16 gcap = rd16(HDA_REG_GCAP);
    UInt32 iss  = (gcap >> 8) & 0x0F;
    UInt32 oss  = (gcap >> 12) & 0x0F;

    QLOG(2, "GCAP=0x%04x ISS=%u OSS=%u VMAJ=%u VMIN=%u", gcap, (unsigned)iss,
         (unsigned)oss, *(volatile UInt8 *)(fRegs + HDA_REG_VMAJ),
         *(volatile UInt8 *)(fRegs + HDA_REG_VMIN));

    if (oss == 0) {
        QLOG(0, "controller reports no output streams (GCAP=0x%04x)", gcap);
        return false;
    }
    /* Input stream descriptors come first, then the output ones. */
    fStream[kQemuHDAOutput].sdBase = HDA_SD_BASE + iss * HDA_SD_SIZE;
    fStream[kQemuHDAInput].sdBase  = HDA_SD_BASE;
    if (iss == 0) {
        fStream[kQemuHDAInput].present = false;
    }

    UInt32 gctl = rd32(HDA_REG_GCTL);
    if (!(gctl & HDA_GCTL_CRST)) {
        QLOG(2, "controller in reset, bringing it up");
        wr32(HDA_REG_GCTL, gctl | HDA_GCTL_CRST);
        for (int i = 0; i < 100; i++) {
            if (rd32(HDA_REG_GCTL) & HDA_GCTL_CRST) break;
            IOSleep(1);
        }
        if (!(rd32(HDA_REG_GCTL) & HDA_GCTL_CRST)) {
            QLOG(0, "controller did not leave reset");
            return false;
        }
        IOSleep(2);     /* codecs need >= 521 us to request an address */
        fPCI->setBusMasterEnable(true);
    }

    /* We use neither interrupts, CORB/RIRB nor the DMA position buffer.
     * Make sure a previous driver did not leave any of them running. */
    wr32(HDA_REG_INTCTL, 0);
    *(volatile UInt8 *)(fRegs + HDA_REG_CORBCTL) = 0;
    *(volatile UInt8 *)(fRegs + HDA_REG_RIRBCTL) = 0;
    wr32(HDA_REG_DPLBASE, 0);
    wr32(HDA_REG_DPUBASE, 0);
    return true;
}

bool QemuHDAAudioDevice::sendImmediate(UInt32 command, UInt32 *response)
{
    int i;

    for (i = 0; i < 1000 && (rd16(HDA_REG_IRS) & HDA_IRS_ICB); i++) {
        IODelay(10);
    }
    if (rd16(HDA_REG_IRS) & HDA_IRS_ICB) {
        /* QEMU leaves ICB set forever when a verb addressed a codec that
         * does not exist; clearing it by hand recovers the interface. */
        QLOG(2, "immediate command interface stuck busy, clearing");
        wr16(HDA_REG_IRS, 0);
    }

    wr32(HDA_REG_IC, command);
    wr16(HDA_REG_IRS, HDA_IRS_IRV | HDA_IRS_ICB);   /* clear IRV, start */

    for (i = 0; i < 1000; i++) {
        UInt16 irs = rd16(HDA_REG_IRS);
        if ((irs & HDA_IRS_IRV) && !(irs & HDA_IRS_ICB)) {
            UInt32 r = rd32(HDA_REG_IR);
            wr16(HDA_REG_IRS, HDA_IRS_IRV);             /* ack */
            if (response) *response = r;
            QLOG(3, "verb 0x%08x -> 0x%08x", (unsigned)command, (unsigned)r);
            return true;
        }
        IODelay(10);
    }
    /* No response: typically a verb sent to a codec address that is not
     * present while probing. Release the interface for the next command. */
    wr16(HDA_REG_IRS, 0);
    QLOG(2, "codec verb 0x%08x timed out", (unsigned)command);
    return false;
}

bool QemuHDAAudioDevice::codecCommand(UInt32 nid, UInt32 verb12, UInt32 data8, UInt32 *response)
{
    UInt32 cmd = (fCodecAddr << 28) | ((nid & 0x7F) << 20) |
                 ((verb12 & 0xFFF) << 8) | (data8 & 0xFF);
    return sendImmediate(cmd, response);
}

bool QemuHDAAudioDevice::codecCommand4(UInt32 nid, UInt32 verb4, UInt32 data16, UInt32 *response)
{
    UInt32 cmd = (fCodecAddr << 28) | ((nid & 0x7F) << 20) |
                 ((verb4 & 0xF) << 16) | (data16 & 0xFFFF);
    return sendImmediate(cmd, response);
}

UInt32 QemuHDAAudioDevice::getParam(UInt32 nid, UInt32 param)
{
    UInt32 r = 0;
    if (!codecCommand(nid, HDA_VERB_GET_PARAMETER, param, &r)) {
        return 0;
    }
    return r;
}

/* ---- Codec -------------------------------------------------------------- */

bool QemuHDAAudioDevice::discoverCodec()
{
    UInt16 statests = rd16(HDA_REG_STATESTS);
    UInt32 candidates = statests & 0x7FFF;
    bool   allowAny = getUIntProperty("QemuHDAAllowAnyCodec", 0, 0, 1) != 0;
    StreamState *out = &fStream[kQemuHDAOutput];
    StreamState *in  = &fStream[kQemuHDAInput];

    QLOG(2, "STATESTS=0x%04x", statests);
    if (candidates == 0) {
        /* A previous driver may have acknowledged (cleared) the bits. */
        candidates = 0x000F;
    }

    fCodecVendor = 0;
    for (UInt32 cad = 0; cad < 15; cad++) {
        if (!(candidates & (1u << cad))) continue;
        fCodecAddr = cad;
        UInt32 vendor = getParam(0, HDA_PAR_VENDOR_ID);
        QLOG(2, "cad %u: vendor 0x%08x", (unsigned)cad, (unsigned)vendor);
        if (vendor == 0 || vendor == 0xFFFFFFFF) continue;
        if ((vendor >> 16) == QEMU_HDA_VENDOR || allowAny) {
            fCodecVendor = vendor;
            break;
        }
    }
    if (!fCodecVendor) {
        QLOG(0, "no QEMU HDA codec found (set QemuHDAAllowAnyCodec=1 to override)");
        return false;
    }

    /* Find the audio function group. */
    UInt32 nc    = getParam(0, HDA_PAR_NODE_COUNT);
    UInt32 start = (nc >> 16) & 0xFF;
    UInt32 count = nc & 0xFF;
    fAFG = 0;
    for (UInt32 nid = start; nid < start + count; nid++) {
        if ((getParam(nid, HDA_PAR_FUNCTION_TYPE) & 0xFF) == HDA_FUNC_TYPE_AUDIO) {
            fAFG = nid;
            break;
        }
    }
    if (!fAFG) {
        QLOG(0, "no audio function group (root node count 0x%08x)", (unsigned)nc);
        return false;
    }

    /* Find the first DAC, ADC, output-capable pin and input-capable pin. */
    nc    = getParam(fAFG, HDA_PAR_NODE_COUNT);
    start = (nc >> 16) & 0xFF;
    count = nc & 0xFF;
    out->converter = out->pin = out->ampSteps = 0;
    in->converter  = in->pin  = in->ampSteps  = 0;
    for (UInt32 nid = start; nid < start + count; nid++) {
        UInt32 cap  = getParam(nid, HDA_PAR_AUDIO_WIDGET_CAP);
        UInt32 type = HDA_WCAP_TYPE(cap);
        QLOG(2, "widget %u: caps 0x%08x type %u", (unsigned)nid, (unsigned)cap, (unsigned)type);
        if (type == HDA_WID_AUD_OUT && !out->converter) {
            out->converter = nid;
            if (cap & HDA_WCAP_OUT_AMP) {
                out->ampSteps = HDA_AMPCAP_NUM_STEPS(getParam(nid, HDA_PAR_AMP_OUT_CAP));
            }
        } else if (type == HDA_WID_AUD_IN && !in->converter) {
            in->converter = nid;
            if (cap & HDA_WCAP_IN_AMP) {
                in->ampSteps = HDA_AMPCAP_NUM_STEPS(getParam(nid, HDA_PAR_AMP_IN_CAP));
            }
        } else if (type == HDA_WID_PIN) {
            UInt32 pincap = getParam(nid, HDA_PAR_PIN_CAP);
            if ((pincap & HDA_PINCAP_OUT) && !out->pin) {
                out->pin = nid;
            } else if ((pincap & HDA_PINCAP_IN) && !in->pin) {
                in->pin = nid;
            }
        }
    }
    if (!out->converter || !out->pin) {
        QLOG(0, "codec has no usable output path (dac=%u pin=%u)",
             (unsigned)out->converter, (unsigned)out->pin);
        return false;
    }
    out->present = true;

    /* Input is optional: hda-duplex (line-in) and hda-micro (microphone)
     * have it, hda-output does not. The controller must also have input
     * stream descriptors (initController() clears 'present' otherwise). */
    in->present = (in->converter != 0 && in->pin != 0 &&
                   ((rd16(HDA_REG_GCAP) >> 8) & 0x0F) != 0 &&
                   getUIntProperty("QemuHDAInput", 1, 0, 1) != 0);
    return true;
}

bool QemuHDAAudioDevice::configureCodec()
{
    StreamState *out = &fStream[kQemuHDAOutput];
    StreamState *in  = &fStream[kQemuHDAInput];
    bool ok = true;

    ok &= codecCommand(fAFG, HDA_VERB_SET_POWER_STATE, 0, NULL);   /* D0 */
    ok &= codecCommand(out->converter, HDA_VERB_SET_POWER_STATE, 0, NULL);
    ok &= codecCommand(out->pin, HDA_VERB_SET_POWER_STATE, 0, NULL);
    ok &= codecCommand(out->pin, HDA_VERB_SET_PIN_WIDGET_CONTROL, HDA_PINCTL_OUT_EN, NULL);

    if (out->ampSteps) {
        /* Hardware amp at its maximum (0 dB in QEMU's mixer); the volume
         * control is applied in software, in clipOutputSamples(). */
        ok &= codecCommand4(out->converter, HDA_VERB_SET_AMP_GAIN_MUTE,
                            HDA_AMP_SET_OUTPUT | HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT |
                            (out->ampSteps & 0x7F), NULL);
    }

    ok &= codecCommand(out->converter, HDA_VERB_SET_CHANNEL_STREAMID, (out->tag << 4) | 0, NULL);
    ok &= codecCommand4(out->converter, HDA_VERB_SET_STREAM_FORMAT, out->formatWord, NULL);

    if (!ok) {
        QLOG(0, "codec configuration failed");
        return false;
    }

    if (in->present) {
        bool inOk = true;
        inOk &= codecCommand(in->converter, HDA_VERB_SET_POWER_STATE, 0, NULL);
        inOk &= codecCommand(in->pin, HDA_VERB_SET_POWER_STATE, 0, NULL);
        inOk &= codecCommand(in->pin, HDA_VERB_SET_PIN_WIDGET_CONTROL, HDA_PINCTL_IN_EN, NULL);
        if (in->ampSteps) {
            /* Same idea as the output: ADC amp at maximum, unmuted; the
             * input level control is applied in convertInputSamples(). */
            inOk &= codecCommand4(in->converter, HDA_VERB_SET_AMP_GAIN_MUTE,
                                  HDA_AMP_SET_INPUT | HDA_AMP_SET_LEFT | HDA_AMP_SET_RIGHT |
                                  (in->ampSteps & 0x7F), NULL);
        }
        inOk &= codecCommand(in->converter, HDA_VERB_SET_CHANNEL_STREAMID, (in->tag << 4) | 0, NULL);
        inOk &= codecCommand4(in->converter, HDA_VERB_SET_STREAM_FORMAT, in->formatWord, NULL);
        if (!inOk) {
            QLOG(0, "input path configuration failed; input disabled");
            in->present = false;
        }
    }
    return true;
}

/* ---- DMA buffers -------------------------------------------------------- */

bool QemuHDAAudioDevice::allocateDMABuffers(QemuHDADirection dir)
{
    /* 32-bit, page aligned, physically contiguous. */
    const mach_vm_address_t mask = 0x00000000FFFFF000ULL;
    StreamState *st = &fStream[dir];
    IOByteCount segLen = 0;

    st->ringDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        QHDA_BUFFER_BYTES, mask);
    st->bdlDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
        PAGE_SIZE, mask);
    if (!st->ringDesc || !st->bdlDesc) {
        QLOG(0, "cannot allocate %s DMA buffers", dirName(dir));
        return false;
    }
    if (st->ringDesc->prepare() != kIOReturnSuccess || st->bdlDesc->prepare() != kIOReturnSuccess) {
        QLOG(0, "cannot wire %s DMA buffers", dirName(dir));
        return false;
    }

    st->ringVirt = st->ringDesc->getBytesNoCopy();
    st->bdlVirt  = st->bdlDesc->getBytesNoCopy();
    bzero(st->ringVirt, QHDA_BUFFER_BYTES);
    bzero(st->bdlVirt, PAGE_SIZE);

    st->ringPhys = st->ringDesc->getPhysicalSegment(0, &segLen, kIOMemoryMapperNone);
    if (!st->ringPhys || segLen < QHDA_BUFFER_BYTES) {
        QLOG(0, "%s ring buffer not contiguous (phys 0x%llx len %llu)", dirName(dir),
             (unsigned long long)st->ringPhys, (unsigned long long)segLen);
        return false;
    }
    st->bdlPhys = st->bdlDesc->getPhysicalSegment(0, &segLen, kIOMemoryMapperNone);
    if (!st->bdlPhys) {
        QLOG(0, "%s BDL has no physical address", dirName(dir));
        return false;
    }

    QemuHDABDLEntry *bdl = (QemuHDABDLEntry *)st->bdlVirt;
    const UInt32 chunk = QHDA_BUFFER_BYTES / QHDA_BDL_ENTRIES;
    for (UInt32 i = 0; i < QHDA_BDL_ENTRIES; i++) {
        UInt64 a = st->ringPhys + (UInt64)i * chunk;
        bdl[i].addrLow  = OSSwapHostToLittleInt32((UInt32)(a & 0xFFFFFFFFULL));
        bdl[i].addrHigh = OSSwapHostToLittleInt32((UInt32)(a >> 32));
        bdl[i].length   = OSSwapHostToLittleInt32(chunk);
        bdl[i].flags    = 0;    /* no IOC: we poll, interrupts stay off */
    }

    QLOG(2, "%s ring phys 0x%llx (%u bytes), BDL phys 0x%llx", dirName(dir),
         (unsigned long long)st->ringPhys, (unsigned)QHDA_BUFFER_BYTES,
         (unsigned long long)st->bdlPhys);
    return true;
}

void QemuHDAAudioDevice::freeDMABuffers()
{
    for (int d = 0; d < kQemuHDANumDirections; d++) {
        StreamState *st = &fStream[d];
        if (st->ringDesc) {
            st->ringDesc->complete();
            st->ringDesc->release();
            st->ringDesc = NULL;
            st->ringVirt = NULL;
        }
        if (st->bdlDesc) {
            st->bdlDesc->complete();
            st->bdlDesc->release();
            st->bdlDesc = NULL;
            st->bdlVirt = NULL;
        }
    }
}

/* ---- Streams ------------------------------------------------------------ */

bool QemuHDAAudioDevice::resetStream(QemuHDADirection dir)
{
    const UInt32 sd = fStream[dir].sdBase;
    int i;

    /* Stop DMA first. */
    wr32(sd + HDA_SD_CTL, rd32(sd + HDA_SD_CTL) & ~(UInt32)(HDA_SD_CTL_RUN | HDA_SD_STS_MASK));
    for (i = 0; i < 100 && (rd32(sd + HDA_SD_CTL) & HDA_SD_CTL_RUN); i++) {
        IODelay(10);
    }

    wr32(sd + HDA_SD_CTL, HDA_SD_CTL_SRST);
    for (i = 0; i < 100 && !(rd32(sd + HDA_SD_CTL) & HDA_SD_CTL_SRST); i++) {
        IODelay(10);
    }
    wr32(sd + HDA_SD_CTL, 0);
    for (i = 0; i < 100 && (rd32(sd + HDA_SD_CTL) & HDA_SD_CTL_SRST); i++) {
        IODelay(10);
    }
    if (rd32(sd + HDA_SD_CTL) & HDA_SD_CTL_SRST) {
        QLOG(0, "%s stream stuck in reset", dirName(dir));
        return false;
    }
    return true;
}

IOReturn QemuHDAAudioDevice::startStream(QemuHDADirection dir)
{
    StreamState *st = &fStream[dir];
    const UInt32 sd = st->sdBase;

    if (!fRegs || !st->present) return kIOReturnNotReady;
    if (st->running) return kIOReturnSuccess;

    if (!resetStream(dir)) {
        return kIOReturnIOError;
    }

    wr32(sd + HDA_SD_BDPL, (UInt32)(st->bdlPhys & 0xFFFFFFFFULL));
    wr32(sd + HDA_SD_BDPU, (UInt32)(st->bdlPhys >> 32));
    wr32(sd + HDA_SD_CBL, QHDA_BUFFER_BYTES);
    wr16(sd + HDA_SD_LVI, QHDA_BDL_ENTRIES - 1);
    wr16(sd + HDA_SD_FMT, st->formatWord);

    /* Re-assert the codec side every time: cheap, and robust against a
     * codec reset while we were stopped. */
    codecCommand(st->converter, HDA_VERB_SET_CHANNEL_STREAMID, (st->tag << 4) | 0, NULL);
    codecCommand4(st->converter, HDA_VERB_SET_STREAM_FORMAT, st->formatWord, NULL);

    UInt32 ctl = st->tag << HDA_SD_CTL_STRM_SHIFT;
    wr32(sd + HDA_SD_CTL, ctl);
    wr32(sd + HDA_SD_CTL, ctl | HDA_SD_CTL_RUN);

    st->running = true;
    QLOG(2, "%s stream started at %u Hz (SD @0x%x, FMT=0x%04x, CTL=0x%08x)", dirName(dir),
         (unsigned)st->rate, (unsigned)sd, (unsigned)st->formatWord, (unsigned)rd32(sd + HDA_SD_CTL));
    return kIOReturnSuccess;
}

void QemuHDAAudioDevice::setRate(QemuHDADirection dir, UInt32 rate)
{
    /* Applied to the stream descriptor and the codec on the next
     * startStream(); the HDA spec requires the stream to be stopped
     * while its format changes. */
    StreamState *st = &fStream[dir];
    st->rate       = rate;
    st->formatWord = (rate == 44100) ? HDA_FMT_44K1_16BIT_STEREO : HDA_FMT_48K_16BIT_STEREO;
    QLOG(2, "%s rate set to %u Hz (FMT=0x%04x)", dirName(dir), (unsigned)rate,
         (unsigned)st->formatWord);
}

void QemuHDAAudioDevice::stopStream(QemuHDADirection dir)
{
    const UInt32 sd = fStream[dir].sdBase;

    if (!fRegs) return;
    wr32(sd + HDA_SD_CTL, rd32(sd + HDA_SD_CTL) & ~(UInt32)(HDA_SD_CTL_RUN | HDA_SD_STS_MASK));
    fStream[dir].running = false;
    QLOG(2, "%s stream stopped", dirName(dir));
}

UInt32 QemuHDAAudioDevice::readPositionBytes(QemuHDADirection dir)
{
    if (!fRegs) return 0;
    UInt32 pos = rd32(fStream[dir].sdBase + HDA_SD_LPIB);
    if (pos >= QHDA_BUFFER_BYTES) {
        pos %= QHDA_BUFFER_BYTES;
    }
    return pos & ~(UInt32)(QHDA_BYTES_PER_FRAME - 1);
}

/* ---- Engines ------------------------------------------------------------ */

bool QemuHDAAudioDevice::createEngine(QemuHDADirection dir)
{
    QemuHDAAudioEngine *engine = new QemuHDAAudioEngine;
    if (!engine) {
        return false;
    }
    if (!engine->init(this, dir)) {
        engine->release();
        return false;
    }
    IOReturn r = activateAudioEngine(engine);
    if (r != kIOReturnSuccess) {
        engine->release();
        QLOG(0, "activateAudioEngine (%s) failed: 0x%x", dirName(dir), r);
        return false;
    }
    fEngine[dir] = engine;      /* the device's engine list holds the reference */
    engine->release();
    return true;
}
