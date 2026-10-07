#pragma once

// Default radio configuration: 434.000 MHz, 1.2 kbps 2-FSK narrowband.
//
// Based on TI's CC112x EasyLink example settings (SmartRF Studio
// "1.2 kbps, 2-FSK, 4 kHz deviation, 25 kHz RX BW"), moved from the
// 868 MHz band to 434 MHz by setting FS_CFG band select to LO divider 8
// (FREQ word 0x6C8000 with a 32 MHz XOSC).
//
// TODO: regenerate this table with SmartRF Studio 7 for your exact board,
// crystal and data rate ("Register export" -> template {addr, value}).

#include <cstddef>

#include "cc1120/cc1120.h"

namespace cc1120 {

inline constexpr RegSetting kPreset434MHz[] = {
    {IOCFG3, 0xB0},        // GPIO3: high impedance
    {IOCFG2, 0x06},        // GPIO2: PKT_SYNC_RXTX
    {IOCFG1, 0xB0},        // GPIO1 (shared with SO): high impedance when CSn high
    {IOCFG0, 0x40},        // GPIO0: RXFIFO_THR, inverted
    {SYNC_CFG1, 0x0B},
    {DCFILT_CFG, 0x1C},
    {IQIC, 0xC6},
    {CHAN_BW, 0x08},
    {MDMCFG0, 0x05},
    {AGC_REF, 0x20},
    {AGC_CS_THR, 0x19},
    {AGC_CFG1, 0xA9},
    {AGC_CFG0, 0xCF},
    {FIFO_CFG, 0x00},
    {SETTLING_CFG, 0x03},
    {FS_CFG, 0x14},        // FS lock detector on, 410-480 MHz band (LO div 8)
    {PKT_CFG0, 0x20},      // Variable packet length (first byte = length)
    {PKT_LEN, 0xFF},
    {IF_MIX_CFG, 0x00},
    {FREQOFF_CFG, 0x22},
    {FREQ2, 0x6C},         // 434.000 MHz
    {FREQ1, 0x80},
    {FREQ0, 0x00},
    {FS_DIG1, 0x00},
    {FS_DIG0, 0x5F},
    {FS_CAL0, 0x0E},
    {FS_DIVTWO, 0x03},
    {FS_DSM0, 0x33},
    {FS_DVC0, 0x17},
    {FS_PFD, 0x50},
    {FS_PRE, 0x6E},
    {FS_REG_DIV_CML, 0x14},
    {FS_SPARE, 0xAC},
    {XOSC5, 0x0E},
    {XOSC3, 0xC7},
    {XOSC1, 0x07},
};

inline constexpr size_t kPreset434MHzCount = sizeof(kPreset434MHz) / sizeof(kPreset434MHz[0]);

}  // namespace cc1120
