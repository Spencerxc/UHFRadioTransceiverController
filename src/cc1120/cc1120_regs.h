#pragma once

// CC1120 register map, command strobes and status-byte fields.
// Addresses 0x00-0x2E are the normal register space. Extended registers are
// encoded as 0x2Fxx: the driver sends the 0x2F header byte followed by xx.

#include <cstdint>

namespace cc1120 {

// ---- SPI header bits --------------------------------------------------------
constexpr uint8_t kReadBit = 0x80;
constexpr uint8_t kBurstBit = 0x40;
constexpr uint8_t kExtAddr = 0x2F;
constexpr uint8_t kDirectMem = 0x3E;
constexpr uint8_t kFifo = 0x3F;

// ---- Configuration registers (normal space) ---------------------------------
constexpr uint16_t IOCFG3 = 0x0000;
constexpr uint16_t IOCFG2 = 0x0001;
constexpr uint16_t IOCFG1 = 0x0002;
constexpr uint16_t IOCFG0 = 0x0003;
constexpr uint16_t SYNC3 = 0x0004;
constexpr uint16_t SYNC2 = 0x0005;
constexpr uint16_t SYNC1 = 0x0006;
constexpr uint16_t SYNC0 = 0x0007;
constexpr uint16_t SYNC_CFG1 = 0x0008;
constexpr uint16_t SYNC_CFG0 = 0x0009;
constexpr uint16_t DEVIATION_M = 0x000A;
constexpr uint16_t MODCFG_DEV_E = 0x000B;
constexpr uint16_t DCFILT_CFG = 0x000C;
constexpr uint16_t PREAMBLE_CFG1 = 0x000D;
constexpr uint16_t PREAMBLE_CFG0 = 0x000E;
constexpr uint16_t FREQ_IF_CFG = 0x000F;
constexpr uint16_t IQIC = 0x0010;
constexpr uint16_t CHAN_BW = 0x0011;
constexpr uint16_t MDMCFG1 = 0x0012;
constexpr uint16_t MDMCFG0 = 0x0013;
constexpr uint16_t SYMBOL_RATE2 = 0x0014;
constexpr uint16_t SYMBOL_RATE1 = 0x0015;
constexpr uint16_t SYMBOL_RATE0 = 0x0016;
constexpr uint16_t AGC_REF = 0x0017;
constexpr uint16_t AGC_CS_THR = 0x0018;
constexpr uint16_t AGC_GAIN_ADJUST = 0x0019;
constexpr uint16_t AGC_CFG3 = 0x001A;
constexpr uint16_t AGC_CFG2 = 0x001B;
constexpr uint16_t AGC_CFG1 = 0x001C;
constexpr uint16_t AGC_CFG0 = 0x001D;
constexpr uint16_t FIFO_CFG = 0x001E;
constexpr uint16_t DEV_ADDR = 0x001F;
constexpr uint16_t SETTLING_CFG = 0x0020;
constexpr uint16_t FS_CFG = 0x0021;
constexpr uint16_t WOR_CFG1 = 0x0022;
constexpr uint16_t WOR_CFG0 = 0x0023;
constexpr uint16_t WOR_EVENT0_MSB = 0x0024;
constexpr uint16_t WOR_EVENT0_LSB = 0x0025;
constexpr uint16_t PKT_CFG2 = 0x0026;
constexpr uint16_t PKT_CFG1 = 0x0027;
constexpr uint16_t PKT_CFG0 = 0x0028;
constexpr uint16_t RFEND_CFG1 = 0x0029;
constexpr uint16_t RFEND_CFG0 = 0x002A;
constexpr uint16_t PA_CFG2 = 0x002B;
constexpr uint16_t PA_CFG1 = 0x002C;
constexpr uint16_t PA_CFG0 = 0x002D;
constexpr uint16_t PKT_LEN = 0x002E;

// ---- Extended configuration registers ---------------------------------------
constexpr uint16_t IF_MIX_CFG = 0x2F00;
constexpr uint16_t FREQOFF_CFG = 0x2F01;
constexpr uint16_t TOC_CFG = 0x2F02;
constexpr uint16_t MARC_SPARE = 0x2F03;
constexpr uint16_t ECG_CFG = 0x2F04;
constexpr uint16_t CFM_DATA_CFG = 0x2F05;
constexpr uint16_t EXT_CTRL = 0x2F06;
constexpr uint16_t RCCAL_FINE = 0x2F07;
constexpr uint16_t RCCAL_COARSE = 0x2F08;
constexpr uint16_t RCCAL_OFFSET = 0x2F09;
constexpr uint16_t FREQOFF1 = 0x2F0A;
constexpr uint16_t FREQOFF0 = 0x2F0B;
constexpr uint16_t FREQ2 = 0x2F0C;
constexpr uint16_t FREQ1 = 0x2F0D;
constexpr uint16_t FREQ0 = 0x2F0E;
constexpr uint16_t IF_ADC2 = 0x2F0F;
constexpr uint16_t IF_ADC1 = 0x2F10;
constexpr uint16_t IF_ADC0 = 0x2F11;
constexpr uint16_t FS_DIG1 = 0x2F12;
constexpr uint16_t FS_DIG0 = 0x2F13;
constexpr uint16_t FS_CAL3 = 0x2F14;
constexpr uint16_t FS_CAL2 = 0x2F15;
constexpr uint16_t FS_CAL1 = 0x2F16;
constexpr uint16_t FS_CAL0 = 0x2F17;
constexpr uint16_t FS_CHP = 0x2F18;
constexpr uint16_t FS_DIVTWO = 0x2F19;
constexpr uint16_t FS_DSM1 = 0x2F1A;
constexpr uint16_t FS_DSM0 = 0x2F1B;
constexpr uint16_t FS_DVC1 = 0x2F1C;
constexpr uint16_t FS_DVC0 = 0x2F1D;
constexpr uint16_t FS_LBI = 0x2F1E;
constexpr uint16_t FS_PFD = 0x2F1F;
constexpr uint16_t FS_PRE = 0x2F20;
constexpr uint16_t FS_REG_DIV_CML = 0x2F21;
constexpr uint16_t FS_SPARE = 0x2F22;
constexpr uint16_t FS_VCO4 = 0x2F23;
constexpr uint16_t FS_VCO3 = 0x2F24;
constexpr uint16_t FS_VCO2 = 0x2F25;
constexpr uint16_t FS_VCO1 = 0x2F26;
constexpr uint16_t FS_VCO0 = 0x2F27;
constexpr uint16_t GBIAS6 = 0x2F28;
constexpr uint16_t GBIAS5 = 0x2F29;
constexpr uint16_t GBIAS4 = 0x2F2A;
constexpr uint16_t GBIAS3 = 0x2F2B;
constexpr uint16_t GBIAS2 = 0x2F2C;
constexpr uint16_t GBIAS1 = 0x2F2D;
constexpr uint16_t GBIAS0 = 0x2F2E;
constexpr uint16_t IFAMP = 0x2F2F;
constexpr uint16_t LNA = 0x2F30;
constexpr uint16_t RXMIX = 0x2F31;
constexpr uint16_t XOSC5 = 0x2F32;
constexpr uint16_t XOSC4 = 0x2F33;
constexpr uint16_t XOSC3 = 0x2F34;
constexpr uint16_t XOSC2 = 0x2F35;
constexpr uint16_t XOSC1 = 0x2F36;
constexpr uint16_t XOSC0 = 0x2F37;
constexpr uint16_t ANALOG_SPARE = 0x2F38;
constexpr uint16_t PA_CFG3 = 0x2F39;

// ---- Extended status registers ----------------------------------------------
constexpr uint16_t WOR_TIME1 = 0x2F64;
constexpr uint16_t WOR_TIME0 = 0x2F65;
constexpr uint16_t RSSI1 = 0x2F71;
constexpr uint16_t RSSI0 = 0x2F72;
constexpr uint16_t MARCSTATE = 0x2F73;
constexpr uint16_t LQI_VAL = 0x2F74;
constexpr uint16_t PQT_SYNC_ERR = 0x2F75;
constexpr uint16_t DEM_STATUS = 0x2F76;
constexpr uint16_t FREQOFF_EST1 = 0x2F77;
constexpr uint16_t FREQOFF_EST0 = 0x2F78;
constexpr uint16_t RNDGEN = 0x2F80;
constexpr uint16_t GPIO_STATUS = 0x2F8C;
constexpr uint16_t FSCAL_CTRL = 0x2F8D;
constexpr uint16_t PARTNUMBER = 0x2F8F;
constexpr uint16_t PARTVERSION = 0x2F90;
constexpr uint16_t SERIAL_STATUS = 0x2F91;
constexpr uint16_t MODEM_STATUS1 = 0x2F92;
constexpr uint16_t MODEM_STATUS0 = 0x2F93;
constexpr uint16_t MARC_STATUS1 = 0x2F94;
constexpr uint16_t MARC_STATUS0 = 0x2F95;
constexpr uint16_t RXFIRST = 0x2FD2;
constexpr uint16_t TXFIRST = 0x2FD3;
constexpr uint16_t RXLAST = 0x2FD4;
constexpr uint16_t TXLAST = 0x2FD5;
constexpr uint16_t NUM_TXBYTES = 0x2FD6;
constexpr uint16_t NUM_RXBYTES = 0x2FD7;
constexpr uint16_t FIFO_NUM_TXBYTES = 0x2FD8;
constexpr uint16_t FIFO_NUM_RXBYTES = 0x2FD9;

// Last configuration register in each space (used by register dumps).
constexpr uint16_t kLastConfigReg = PKT_LEN;
constexpr uint16_t kLastExtConfigReg = PA_CFG3;

// ---- Command strobes --------------------------------------------------------
constexpr uint8_t SRES = 0x30;     // Reset chip
constexpr uint8_t SFSTXON = 0x31;  // Enable and calibrate synthesizer
constexpr uint8_t SXOFF = 0x32;    // Crystal oscillator off (on CSn high)
constexpr uint8_t SCAL = 0x33;     // Calibrate synthesizer and turn it off
constexpr uint8_t SRX = 0x34;      // Enable RX
constexpr uint8_t STX = 0x35;      // Enable TX
constexpr uint8_t SIDLE = 0x36;    // Exit RX/TX, go to IDLE
constexpr uint8_t SAFC = 0x37;     // Automatic frequency compensation
constexpr uint8_t SWOR = 0x38;     // Start eWOR sequence
constexpr uint8_t SPWD = 0x39;     // Enter SLEEP (on CSn high)
constexpr uint8_t SFRX = 0x3A;     // Flush RX FIFO
constexpr uint8_t SFTX = 0x3B;     // Flush TX FIFO
constexpr uint8_t SWORRST = 0x3C;  // Reset eWOR timer
constexpr uint8_t SNOP = 0x3D;     // No operation, returns status byte

// ---- Status byte ------------------------------------------------------------
constexpr uint8_t kStatusChipRdyN = 0x80;  // 1 = crystal not yet stable
constexpr uint8_t kStatusStateMask = 0x70;

enum class ChipState : uint8_t {
    Idle = 0x00,
    Rx = 0x10,
    Tx = 0x20,
    FstxOn = 0x30,
    Calibrate = 0x40,
    Settling = 0x50,
    RxFifoError = 0x60,
    TxFifoError = 0x70,
};

// MARCSTATE register, bits 4:0
constexpr uint8_t kMarcStateMask = 0x1F;

// FS_CFG.FSD_BANDSELECT (bits 3:0) values
constexpr uint8_t kBandSelectMask = 0x0F;
constexpr uint8_t kBand410_480 = 0x04;  // LO divider 8

constexpr uint8_t kPartNumberCc1120 = 0xEF;

}  // namespace cc1120
