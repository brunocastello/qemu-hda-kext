/*
 * QemuHDARegs.h
 * Intel High Definition Audio register map and codec verbs used by
 * QemuHDAAudio. Only the subset needed to drive QEMU's intel-hda /
 * ich9-intel-hda controller with the hda-output (or hda-duplex) codec.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef QEMU_HDA_REGS_H
#define QEMU_HDA_REGS_H

/* ---- Global controller registers (offsets into BAR0) ------------------- */
#define HDA_REG_GCAP        0x00    /* 16 bit: [15:12]=OSS [11:8]=ISS [0]=64OK */
#define HDA_REG_VMIN        0x02
#define HDA_REG_VMAJ        0x03
#define HDA_REG_GCTL        0x08    /* 32 bit: [0]=CRST (1 = out of reset) */
#define HDA_REG_STATESTS    0x0E    /* 16 bit: codec presence bits, RW1C */
#define HDA_REG_INTCTL      0x20    /* 32 bit: [31]=GIE [30]=CIE [29:0]=SIE */
#define HDA_REG_INTSTS      0x24
#define HDA_REG_DPLBASE     0x70    /* DMA position buffer, [0]=enable */
#define HDA_REG_DPUBASE     0x74

/* Immediate command interface (used instead of CORB/RIRB) */
#define HDA_REG_IC          0x60    /* 32 bit: verb to send */
#define HDA_REG_IR          0x64    /* 32 bit: response */
#define HDA_REG_IRS         0x68    /* 16 bit: [1]=IRV (RW1C) [0]=ICB */
#define HDA_IRS_ICB         0x0001
#define HDA_IRS_IRV         0x0002

#define HDA_GCTL_CRST       0x00000001

/* ---- Stream descriptor registers --------------------------------------- */
#define HDA_SD_BASE         0x80
#define HDA_SD_SIZE         0x20

#define HDA_SD_CTL          0x00    /* 32 bit: CTL (24 bit) + STS (8 bit) */
#define HDA_SD_LPIB         0x04    /* 32 bit: link position in buffer (bytes) */
#define HDA_SD_CBL          0x08    /* 32 bit: cyclic buffer length (bytes) */
#define HDA_SD_LVI          0x0C    /* 16 bit: last valid BDL index */
#define HDA_SD_FIFOS        0x10
#define HDA_SD_FMT          0x12    /* 16 bit: stream format */
#define HDA_SD_BDPL         0x18    /* 32 bit: BDL base, low */
#define HDA_SD_BDPU         0x1C    /* 32 bit: BDL base, high */

#define HDA_SD_CTL_SRST     0x00000001  /* stream reset */
#define HDA_SD_CTL_RUN      0x00000002  /* DMA run */
#define HDA_SD_CTL_IOCE     0x00000004
#define HDA_SD_CTL_STRM_SHIFT 20        /* stream tag, bits 23:20 */
#define HDA_SD_STS_MASK     0x1C000000  /* BCIS | FIFOE | DESE, RW1C */

/* Buffer descriptor list entry (16 bytes, little endian) */
struct QemuHDABDLEntry {
    UInt32 addrLow;
    UInt32 addrHigh;
    UInt32 length;
    UInt32 flags;       /* bit 0 = IOC */
} __attribute__((packed));

/* ---- Stream format word -------------------------------------------------
 * [14]=BASE (0=48 kHz, 1=44.1 kHz) [13:11]=MULT [10:8]=DIV
 * [6:4]=BITS (1=16 bit) [3:0]=CHAN-1.
 */
#define HDA_FMT_48K_16BIT_STEREO   0x0011
#define HDA_FMT_44K1_16BIT_STEREO  0x4011

/* ---- Codec verbs ---------------------------------------------------------
 * A codec command is (CAd << 28) | (NID << 20) | payload, where payload is
 * either (12-bit verb << 8 | 8-bit data) or (4-bit verb << 16 | 16-bit data).
 */
#define HDA_VERB_GET_PARAMETER          0xF00   /* 12 bit */
#define HDA_VERB_SET_POWER_STATE        0x705   /* 12 bit */
#define HDA_VERB_SET_CHANNEL_STREAMID   0x706   /* 12 bit */
#define HDA_VERB_SET_PIN_WIDGET_CONTROL 0x707   /* 12 bit */
#define HDA_VERB_SET_EAPD               0x70C   /* 12 bit */
#define HDA_VERB_SET_STREAM_FORMAT      0x2     /* 4 bit  */
#define HDA_VERB_SET_AMP_GAIN_MUTE      0x3     /* 4 bit  */

/* GET_PARAMETER ids */
#define HDA_PAR_VENDOR_ID           0x00
#define HDA_PAR_REV_ID              0x02
#define HDA_PAR_NODE_COUNT          0x04
#define HDA_PAR_FUNCTION_TYPE       0x05
#define HDA_PAR_AUDIO_WIDGET_CAP    0x09
#define HDA_PAR_PCM                 0x0A
#define HDA_PAR_PIN_CAP             0x0C
#define HDA_PAR_AMP_IN_CAP          0x0D
#define HDA_PAR_AMP_OUT_CAP         0x12

#define HDA_FUNC_TYPE_AUDIO         0x01

#define HDA_WCAP_TYPE(cap)          (((cap) >> 20) & 0x0F)
#define HDA_WID_AUD_OUT             0x0
#define HDA_WID_AUD_IN              0x1
#define HDA_WID_PIN                 0x4
#define HDA_WCAP_IN_AMP             (1u << 1)
#define HDA_WCAP_OUT_AMP            (1u << 2)

#define HDA_PINCAP_OUT              (1u << 4)
#define HDA_PINCAP_IN               (1u << 5)
#define HDA_PINCTL_IN_EN            0x20
#define HDA_PINCTL_OUT_EN           0x40

#define HDA_AMPCAP_NUM_STEPS(cap)   (((cap) >> 8) & 0x7F)

/* SET_AMP_GAIN_MUTE payload bits */
#define HDA_AMP_SET_OUTPUT          0x8000
#define HDA_AMP_SET_INPUT           0x4000
#define HDA_AMP_SET_LEFT            0x2000
#define HDA_AMP_SET_RIGHT           0x1000
#define HDA_AMP_MUTE                0x0080

/* QEMU's codecs report vendor 0x1af4 (Red Hat / virtio) */
#define QEMU_HDA_VENDOR             0x1af4

#endif /* QEMU_HDA_REGS_H */
