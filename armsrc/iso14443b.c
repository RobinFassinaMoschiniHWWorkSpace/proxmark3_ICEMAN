//-----------------------------------------------------------------------------
// Copyright (C) Jonathan Westhues, Nov 2006
// Copyright (C) Gerhard de Koning Gans - May 2008
// Copyright (C) Proxmark3 contributors. See AUTHORS.md for details.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// See LICENSE.txt for the text of the license.
//-----------------------------------------------------------------------------
// Routines to support ISO 14443B. This includes both the reader software and
// the `fake tag' modes.
//-----------------------------------------------------------------------------
#include "iso14443b.h"

#include "proxmark3_arm.h"
#include "common.h"  // access to global variable: g_dbglevel
#include "util.h"
#include "string.h"
#include "crc16.h"
#include "protocols.h"
#include "appmain.h"
#include "BigBuf.h"
#include "cmd.h"
#include "fpgaloader.h"
#include "commonutil.h"
#include "dbprint.h"
#include "ticks.h"
#include "iso14b.h"       // defines for ETU conversions
#include "iclass.h"       // picopass buffer defines

/*
* Current timing issues with ISO14443-b implementation
* Proxmark3
* Carrier Frequency 13.56MHz
*    1 / 13 560 000 = 73.74 nano seconds  ( 0.07374 µs )

* SSP_CLK runs at 13.56MHz / 4 = 3,39MHz
*    1 / 3 390 000 = 294.98 nano seconds  ( 0.2949 µs )
*
* 1 ETU = 9.4395 µs = 32 SSP_CLK = 128 FC
* 1 SSP_CLK = 4 FC
* 1 µs  3 SSP_CLK about 14 FC
* PROBLEM 1.
* ----------
* one way of calculating time, that relates both to PM3 ssp_clk 3.39MHz, ISO freq of 13.56Mhz and ETUs
* convert from µS -> our SSP_CLK units which is used in the DEFINES..
* convert from ms -> our SSP_CLK units...
* convert from ETU -> our SSP_CLK units...
* All ETU -> µS -> ms should be diveded by for to match Proxmark3 FPGA SSP_CLK :)
*
*
* PROBLEM 2.
* ----------
* all DEFINES is in SSP_CLK ticks
* all delays is in SSP_CLK ticks
*/

#ifndef RECEIVE_MASK
# define RECEIVE_MASK  (DMA_BUFFER_SIZE - 1)
#endif

// SSP_CLK runs at 13,56MHz / 32 = 423.75kHz when simulating a tag
// All values should be multiples of 2 (?)
#define DELAY_READER_TO_ARM               8
#define DELAY_ARM_TO_READER               0

// SSP_CLK runs at 13.56MHz / 4 = 3,39MHz when acting as reader.
// All values should be multiples of 16
#define DELAY_ARM_TO_TAG                 16
#define DELAY_TAG_TO_ARM                 32

// SSP_CLK runs at 13.56MHz / 4 = 3,39MHz when sniffing.
// All values should be multiples of 16
#define DELAY_TAG_TO_ARM_SNIFF           32
#define DELAY_READER_TO_ARM_SNIFF        32

/* ISO14443-4
*
* Frame Waiting Time Integer (FWI)
* can be between 0 and 14.
* FWI_default = 4
* FWI_max = 14
*
* Frame Waiting Time (FWT) formula
* --------------------------------
* FWT = (256 x 16 / fc) * 2 to the power for FWI
*
* sample:
*                          ------- 2 to the power of FWI(4)
* FWT = (256 x 16 / fc) * (2*2*2*2) == 4.833 ms

* FTW(default) == FWT(4) == 4.822ms
*
* FWI_max == 2^14 = 16384
* FWT(max) = (256 x 16 / fc) * 16384 == 4949
*
* Which gives a maximum Frame Waiting time of FWT(max) == 4949 ms
*   FWT(max) in ETU  4949000 / 9.4395 µS = 524286 ETU
*
* Simple calc to convert to ETU or µS
* -----------------------------------
*       uint32_t fwt_time_etu = (32 << fwt);
*       uint32_t fwt_time_us = (302 << fwt);
*
*/

#ifndef MAX_14B_TIMEOUT
// FWT(max) = 4949 ms or 4.95 seconds.
// SSP_CLK = 4949000 * 3.39 = 16777120
# define MAX_14B_TIMEOUT (16777120U)
#endif

// Activation frame waiting time
// 512 ETU?
// 65536/fc == 4833 µS or 4.833ms
// SSP_CLK =  4833 µS * 3.39 = 16384
#ifndef FWT_TIMEOUT_14B
# define FWT_TIMEOUT_14B (16384)
#endif

// ETU 14 * 9.4395 µS = 132 µS == 0.132ms
// TR2,  counting from start of PICC EOF 14 ETU.
#define DELAY_ISO14443B_PICC_TO_PCD_READER  HF14_ETU_TO_SSP(14)
#define DELAY_ISO14443B_PCD_TO_PICC_READER  HF14_ETU_TO_SSP(15)

/* Guard Time (per 14443-2) in ETU
*
* Transition time. TR0 - guard time
*   TR0 - 8 ETU's minimum.
*   TR0 - 32 ETU's maximum for ATQB only
*   TR0 - FWT for all other commands
*       32,64,128,256,512, ... , 262144, 524288 ETU
*   TR0 = FWT(1), FWT(2), FWT(3) .. FWT(14)
*
*
*  TR0
*/
#ifndef ISO14B_TR0
# define ISO14B_TR0  HF14_ETU_TO_SSP(16)
#endif

#ifndef ISO14B_TR0_MAX
# define ISO14B_TR0_MAX HF14_ETU_TO_SSP(32)
// *   TR0 - 32 ETU's maximum for ATQB only
// *   TR0 - FWT for all other commands

// TR0 max is 159 µS or 32 samples from FPGA
// 16 ETU * 9.4395 µS == 151 µS
// 16 * 8 = 128 sub carrier cycles,
// 128 / 4 = 32 I/Q pairs.
// since 1 I/Q pair after 4 subcarrier cycles at 848kHz subcarrier
#endif

// 8 ETU = 75 µS == 256 SSP_CLK
#ifndef ISO14B_TR0_MIN
# define ISO14B_TR0_MIN HF14_ETU_TO_SSP(8)
#endif

// Synchronization time (per 14443-2) in ETU
// 16 ETU = 151 µS == 512 SSP_CLK
#ifndef ISO14B_TR1_MIN
# define ISO14B_TR1_MIN HF14_ETU_TO_SSP(16)
#endif
// Synchronization time (per 14443-2) in ETU
// 25 ETU == 236 µS == 800 SSP_CLK
#ifndef ISO14B_TR1_MAX
# define ISO14B_TR1 HF14_ETU_TO_SSP(25)
#endif

// Frame Delay Time PICC to PCD  (per 14443-3 Amendment 1) in ETU
// 14 ETU == 132 µS == 448 SSP_CLK
#ifndef ISO14B_TR2
# define ISO14B_TR2 HF14_ETU_TO_SSP(14)
#endif

#ifndef ISO14B_BLOCK_SIZE
# define ISO14B_BLOCK_SIZE  4
#endif

// 4sample
#define SEND4STUFFBIT(x) tosend_stuffbit(!(x));tosend_stuffbit(!(x));tosend_stuffbit(!(x));tosend_stuffbit(!(x));

static void iso14b_set_timeout(uint32_t timeout_etu);
static void iso14b_set_maxframesize(uint16_t size);
static void iso14b_set_fwt(uint8_t fwt);

// the block number for the ISO14443-4 PCB  (used with APDUs)
static uint8_t s_iso14b_pcb_blocknum = 0;
static uint8_t s_iso14b_fwt = 9;
static uint32_t s_iso14b_timeout = MAX_14B_TIMEOUT;

static bool s_field_on = false;

/*
* ISO 14443-B communications
* --------------------------
* Reader to card | ASK  - Amplitude Shift Keying Modulation (PCD to PICC for Type B) (NRZ-L encodig)
* Card to reader | BPSK - Binary Phase Shift Keying Modulation, (PICC to PCD for Type B)
*
* It uses half duplex with a 106 kbit per second data rate in each direction.
* Data transmitted by the card is load modulated with a 847.5 kHz subcarrier.
*
* fc - carrier frequency 13.56 MHz
* TR0 - Guard Time per 14443-2
* TR1 - Synchronization Time per 14443-2
* TR2 - PICC to PCD Frame Delay Time (per 14443-3 Amendment 1)
*
* Elementary Time Unit (ETU)
* --------------------------
* ETU is used to denote 1 bit period i.e. how long one bit transfer takes.
*
* - 128 Carrier cycles / 13.56MHz = 8 Subcarrier units / 848kHz = 1/106kHz = 9.4395 µS
* - 16 Carrier cycles = 1 Subcarrier unit  = 1.17 µS
*
* Definition
* ----------
* 1 ETU =  128 / ( D x fc )
* where
*    D = divisor.  Which initial is 1
*   fc = carrier frequency
* gives
*   1 ETU = 128 / fc
*   1 ETU = 128 / 13 560 000 = 9.4395 µS
*   1 ETU = 9.4395 µS
*
* (note: It seems we are using the subcarrier as base for our time calculations rather than the field clock)
*
* - 1 ETU = 1/106 KHz
* - 1 ETU = 8 subcarrier units ( 8 / 848kHz )
* - 1 ETU = 1 bit period
*
*
* Card sends data at 848kHz subcarrier
* subcar |duration| FC division| I/Q pairs
* -------+--------+------------+--------
* 106kHz | 9.44µS | FC/128     | 16
* 212kHz | 4.72µS | FC/64      | 8
* 424kHz | 2.36µS | FC/32      | 4
* 848kHz | 1.18µS | FC/16      | 2
* -------+--------+------------+--------
*
*
* One Character consists of 1 start, 1 stop, 8 databit with a total length of 10bits.
* - 1 Character = 10 ETU = 1 startbit, 8 databits, 1 stopbit
* - startbit is a 0
* - stopbit is a 1
*
* Start of frame (SOF) is
* - [10-11] ETU of ZEROS, unmodulated time
* - [2-3] ETU of ONES,
*
* End of frame (EOF) is
* - [10-11] ETU of ZEROS, unmodulated time
*
* Reader data transmission
* ------------------------
*   - no modulation ONES
*   - SOF
*   - Command, data and CRC_B (1 Character)
*   - EOF
*   - no modulation ONES
*
* Card data transmission
* ----------------------
*   - TR1
*   - SOF
*   - data  (1 Character)
*   - CRC_B
*   - EOF
*
* Transfer times
* --------------
* let calc how long it takes the reader to send a message
*  SOF 10 ETU + 4 data bytes + 2 crc bytes + EOF 2 ETU
*  10 + (4+2 * 10) + 2 = 72 ETU
*  72 * 9.4395 = 680 µS  or  0.68 ms
*
*
* -> TO VERIFY THIS BELOW <-
* --------------------------
* The mode FPGA_MAJOR_MODE_HF_SIMULATOR | FPGA_HF_SIMULATOR_MODULATE_BPSK which we use to simulate tag
* works like this:
* Simulation per definition is "inverted" effect on the reader antenna.
* - A 1-bit input to the FPGA becomes 8 pulses at 847.5kHz (1.18µS / pulse) == 9.44us
* - A 0-bit input to the FPGA becomes an unmodulated time of 1.18µS  or does it become 8 nonpulses for 9.44us
*
*
* FPGA implementation
* -------------------
* Piwi implemented a I/Q sampling mode for the FPGA, where...
*
* FPGA doesn't seem to work with ETU.  It seems to work with pulse / duration instead.
*
* This means that we are using a bit rate of 106 kbit/s, or fc/128.
* Oversample by 4, which ought to make things practical for the ARM
* (fc/32, 423.8 kbits/s, ~52 kbytes/s)
*
* We are sampling the signal at FC/32,  we are reporting over SSP to PM3 each
*
* Current I/Q pair sampling
* -------------------------
* Let us report a correlation every 64 samples. I.e.
*  1 I/Q pair after 4 subcarrier cycles for the 848kHz subcarrier,
*  1 I/Q pair after 2 subcarrier cycles for the 424kHz subcarrier,
*/


/*
* Formula to calculate FWT (in ETUs) by timeout (in ms):
*
* 1 tick is about 1.5µS
* 1000 ms/s
*
* FWT = 13560000 * 1000 / (8*16) * timeout
* FWT = 13560000 * 1000 / 128 * timeout
*
* sample: 3sec == 3000ms
*
*  13560000 * 1000 / 128 * 3000 == 13560000000 / 384000 ==
*  13560000 / 384  = 35312 FWT
*
*  35312 * 9.4395 ==
*
* @param timeout is in frame wait time, fwt, measured in ETUs
*
*  However we need to compensate for SSP_CLK ...
*/



//=============================================================================
// An ISO 14443 Type B tag. We listen for commands from the reader, using
// a UART kind of thing that's implemented in software. When we get a
// frame (i.e., a group of bytes between SOF and EOF), we check the CRC.
// If it's good, then we can do something appropriate with it, and send
// a response.
//=============================================================================

//-----------------------------------------------------------------------------
// Code up a string of octets at layer 2 (including CRC, we don't generate
// that here) so that they can be transmitted to the reader. Doesn't transmit
// them yet, just leaves them ready to send in ToSend[].
//-----------------------------------------------------------------------------
static void CodeIso14443bAsTag(const uint8_t *cmd, int len) {

    tosend_reset();

    // Transmit a burst of ones, as the initial thing that lets the
    // reader get phase sync.
    // This loop is TR1, per specification
    // TR1 minimum must be > 80/fs
    // TR1 maximum 200/fs
    // 80/fs < TR1 < 200/fs
    // 10 ETU < TR1 < 24 ETU

    int i;
    // Send TR1.
    // 10-11 ETU * 4times samples ONES
    for (i = 0; i < 10; i++) {
        SEND4STUFFBIT(1);
    }

    // Send SOF.
    // 10-11 ETU * 4times samples ZEROS
    for (i = 0; i < 10; i++) {
        SEND4STUFFBIT(0);
    }

    // 2-3 ETU * 4times samples ONES
    for (i = 0; i < 2; i++) {
        SEND4STUFFBIT(1);
    }

    // data
    for (i = 0; i < len; i++) {

        // Start bit
        SEND4STUFFBIT(0);

        // Data bits
        uint8_t b = cmd[i];
        for (int j = 0; j < 8; j++) {
            SEND4STUFFBIT(b & 1);
            b >>= 1;
        }

        // Stop bit
        SEND4STUFFBIT(1);

        // Extra Guard bit
        // For PICC it ranges 0-18us (1etu = 9us)
        //SEND4STUFFBIT(1);
    }

    // Send EOF.
    // 10-11 ETU * 4 sample rate = ZEROS
    for (i = 0; i < 10; i++) {
        SEND4STUFFBIT(0);
    }

    // why this? push out transfers between arm and fpga?
    //for (i = 0; i < 2; i++) {
    //    SEND4STUFFBIT(1);
    //}

    tosend_t *ts = get_tosend();
    // Convert from last byte pos to length
    ts->max++;
}

//-----------------------------------------------------------------------------
// The software UART that receives commands from the reader, and its state
// variables.
//-----------------------------------------------------------------------------
static struct {
    enum {
        STATE_14B_UNSYNCD,
        STATE_14B_GOT_FALLING_EDGE_OF_SOF,
        STATE_14B_AWAITING_START_BIT,
        STATE_14B_RECEIVING_DATA
    }       state;
    uint16_t shiftReg;
    int      bitCnt;
    int      byteCnt;
    int      byteCntMax;
    int      posCnt;
    uint8_t  *output;
} Uart;

static void Uart14bReset(void) {
    Uart.state = STATE_14B_UNSYNCD;
    Uart.shiftReg = 0;
    Uart.bitCnt = 0;
    Uart.byteCnt = 0;
    Uart.byteCntMax = MAX_FRAME_SIZE;
    Uart.posCnt = 0;
}

static void Uart14bInit(uint8_t *data) {
    Uart.output = data;
    Uart14bReset();
}

// param timeout accepts ETU
static void iso14b_set_timeout(uint32_t timeout_etu) {

    uint32_t ssp = HF14_ETU_TO_SSP(timeout_etu);

    if (ssp > MAX_14B_TIMEOUT) {
        ssp = MAX_14B_TIMEOUT;
    }

    s_iso14b_timeout = ssp;
    if (g_dbglevel >= DBG_DEBUG) {
        Dbprintf("ISO14443B Timeout set to %ld fwt", s_iso14b_timeout);
    }
}

// keep track of FWT,  also updates the timeout
static void iso14b_set_fwt(uint8_t fwt) {
    s_iso14b_fwt = fwt;
    if (g_dbglevel >= DBG_DEBUG) {
        Dbprintf("ISO14443B FWT Timeout set to %ld fwt", s_iso14b_fwt);
    }
    iso14b_set_timeout(32 << fwt);
}

static void iso14b_set_maxframesize(uint16_t size) {
    if (size > 256) {
        size = MAX_FRAME_SIZE;
    }

    Uart.byteCntMax = size;
    if (g_dbglevel >= DBG_DEBUG) {
        Dbprintf("ISO14443B Max frame size set to %d bytes", Uart.byteCntMax);
    }
}

//-----------------------------------------------------------------------------
// The software Demod that receives commands from the tag, and its state variables.
//-----------------------------------------------------------------------------

#define NOISE_THRESHOLD          80                   // don't try to correlate noise
#define MAX_PREVIOUS_AMPLITUDE   (-1 - NOISE_THRESHOLD)

static struct {
    enum {
        DEMOD_UNSYNCD,
        DEMOD_PHASE_REF_TRAINING,
        WAIT_FOR_RISING_EDGE_OF_SOF,
        DEMOD_AWAITING_START_BIT,
        DEMOD_RECEIVING_DATA
    }       state;
    uint16_t bitCount;
    int      posCount;
    int      thisBit;
    uint16_t shiftReg;
    uint16_t max_len;
    uint8_t  *output;
    uint16_t len;
    int      sumI;
    int      sumQ;
} Demod;

// Clear out the state of the "UART" that receives from the tag.
static void Demod14bReset(void) {
    Demod.state = DEMOD_UNSYNCD;
    Demod.bitCount = 0;
    Demod.posCount = 0;
    Demod.thisBit = 0;
    Demod.shiftReg = 0;
    Demod.len = 0;
    Demod.sumI = 0;
    Demod.sumQ = 0;
}

static void Demod14bInit(uint8_t *data, uint16_t max_len) {
    Demod.output = data;
    Demod.max_len = max_len;
    Demod14bReset();
}

/* Receive & handle a bit coming from the reader.
 *
 * This function is called 4 times per bit (every 2 subcarrier cycles).
 * Subcarrier frequency fs is 848kHz, 1/fs = 1,18us, i.e. function is called every 2,36us
 *
 * LED handling:
 * LED A -> ON once we have received the SOF and are expecting the rest.
 * LED A -> OFF once we have received EOF or are in error state or unsynced
 *
 * Returns: true if we received a EOF
 *          false if we are still waiting for some more
 */
static RAMFUNC int Handle14443bSampleFromReader(uint8_t bit) {
    switch (Uart.state) {
        case STATE_14B_UNSYNCD:
            if (bit == false) {
                // we went low, so this could be the beginning of an SOF
                Uart.state = STATE_14B_GOT_FALLING_EDGE_OF_SOF;
                Uart.posCnt = 0;
                Uart.bitCnt = 0;
            }
            break;

        case STATE_14B_GOT_FALLING_EDGE_OF_SOF:
            Uart.posCnt++;

            if (Uart.posCnt == 2) { // sample every 4 1/fs in the middle of a bit

                if (bit) {
                    if (Uart.bitCnt > 9) {
                        // we've seen enough consecutive
                        // zeros that it's a valid SOF
                        Uart.posCnt = 0;
                        Uart.byteCnt = 0;
                        Uart.state = STATE_14B_AWAITING_START_BIT;
                        LED_A_ON(); // Indicate we got a valid SOF
                    } else {
                        // didn't stay down long enough before going high, error
                        Uart.state = STATE_14B_UNSYNCD;
                    }
                } else {
                    // do nothing, keep waiting
                }
                Uart.bitCnt++;
            }

            if (Uart.posCnt >= 4) {
                Uart.posCnt = 0;
            }

            if (Uart.bitCnt > 12) {
                // Give up if we see too many zeros without a one, too.
                LED_A_OFF();
                Uart.state = STATE_14B_UNSYNCD;
            }
            break;

        case STATE_14B_AWAITING_START_BIT:
            Uart.posCnt++;

            if (bit) {

                // max 57us between characters = 49 1/fs,
                // max 3 etus after low phase of SOF = 24 1/fs
                if (Uart.posCnt > 50 / 2) {
                    // stayed high for too long between characters, error
                    Uart.state = STATE_14B_UNSYNCD;
                }

            } else {
                // falling edge, this starts the data byte
                Uart.posCnt = 0;
                Uart.bitCnt = 0;
                Uart.shiftReg = 0;
                Uart.state = STATE_14B_RECEIVING_DATA;
            }
            break;

        case STATE_14B_RECEIVING_DATA:

            Uart.posCnt++;

            if (Uart.posCnt == 2) {
                // time to sample a bit
                Uart.shiftReg >>= 1;
                if (bit) {
                    Uart.shiftReg |= 0x200;
                }
                Uart.bitCnt++;
            }

            if (Uart.posCnt >= 4) {
                Uart.posCnt = 0;
            }

            if (Uart.bitCnt == 10) {
                if ((Uart.shiftReg & 0x200) && !(Uart.shiftReg & 0x001)) {
                    // this is a data byte, with correct
                    // start and stop bits
                    Uart.output[Uart.byteCnt] = (Uart.shiftReg >> 1) & 0xFF;
                    Uart.byteCnt++;

                    if (Uart.byteCnt >= Uart.byteCntMax) {
                        // Buffer overflowed, give up
                        LED_A_OFF();
                        Uart.state = STATE_14B_UNSYNCD;
                    } else {
                        // so get the next byte now
                        Uart.posCnt = 0;
                        Uart.state = STATE_14B_AWAITING_START_BIT;
                    }
                } else if (Uart.shiftReg == 0x000) {
                    // this is an EOF byte
                    LED_A_OFF(); // Finished receiving
                    Uart.state = STATE_14B_UNSYNCD;
                    if (Uart.byteCnt != 0)
                        return true;

                } else {
                    // this is an error
                    LED_A_OFF();
                    Uart.state = STATE_14B_UNSYNCD;
                }
            }
            break;

        default:
            LED_A_OFF();
            Uart.state = STATE_14B_UNSYNCD;
            break;
    }
    return false;
}

//-----------------------------------------------------------------------------
// Receive a command (from the reader to us, where we are the simulated tag),
// and store it in the given buffer, up to the given maximum length. Keeps
// spinning, waiting for a well-framed command, until either we get one
// (returns true) or someone presses the pushbutton on the board (false).
//
// Assume that we're called with the SSC (to the FPGA) and ADC path set
// correctly.
//-----------------------------------------------------------------------------
static bool GetIso14443bCommandFromReader(uint8_t *received, uint16_t *len) {
    // Set FPGA mode to "simulated ISO 14443B tag", no modulation (listen
    // only, since we are receiving, not transmitting).
    // Signal field is off with the appropriate LED
    LED_D_OFF();
    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_SIMULATOR | FPGA_HF_SIMULATOR_NO_MODULATION);

    // Now run a `software UART' on the stream of incoming samples.
    Uart14bInit(received);

    while (BUTTON_PRESS() == false) {
        WDT_HIT();

        if (AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_RXRDY)) {
            uint8_t b = (uint8_t)AT91C_BASE_SSC->SSC_RHR;
            for (uint8_t mask = 0x80; mask != 0x00; mask >>= 1) {
                if (Handle14443bSampleFromReader(b & mask)) {
                    *len = Uart.byteCnt;
                    return true;
                }
            }
        }
    }
    return false;
}

static void TransmitFor14443b_AsTag(const uint8_t *response, uint16_t len) {

    // Signal field is off with the appropriate LED
    LED_D_OFF();

    // TR0: min - 1024 cycles = 75.52 us - max 4096 cycles = 302.08 us
    SpinDelayUs(76);

    // Modulate BPSK
    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_SIMULATOR | FPGA_HF_SIMULATOR_MODULATE_BPSK);

    // Transmit the response.
    for (uint16_t i = 0; i < len;) {

        // Put byte into tx holding register as soon as it is ready
        if (AT91C_BASE_SSC->SSC_SR & AT91C_SSC_TXRDY) {
            AT91C_BASE_SSC->SSC_THR = response[i++];

            // Start-up SSC once first byte is in SSC_THR
            if (i == 1) {
                FpgaSetupSsc(FPGA_MAJOR_MODE_HF_SIMULATOR);
            }
        }
    }
}
//-----------------------------------------------------------------------------
// Main loop of simulated tag: receive commands from reader, decide what
// response to send, and send it.
//-----------------------------------------------------------------------------
void SimulateIso14443bTag(const uint8_t *pupi) {

    /*
        // the only commands we understand is WUPB, AFI=0, Select All, N=1:
        static const uint8_t cmdWUPB[] = { ISO14443B_REQB, 0x00, 0x08, 0x39, 0x73 };
        // ... and REQB, AFI=0, Normal Request, N=1:
        static const uint8_t cmdREQB[] = { ISO14443B_REQB, 0x00, 0x00, 0x71, 0xFF };
        // ... and HLTB
        static const uint8_t cmdHLTB[] = { 0x50, 0xff, 0xff, 0xff, 0xff };
        // ... and ATTRIB
        static const uint8_t cmdATTRIB[] = { ISO14443B_ATTRIB, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    */
    LED_A_ON();

    // setup device.
    FpgaDownloadAndGo(FPGA_BITSTREAM_HF);

    // connect Demodulated Signal to ADC:
    SetAdcMuxFor(GPIO_MUXSEL_HIPKD);

    // Set up the synchronous serial port
    FpgaSetupSsc(FPGA_MAJOR_MODE_HF_SIMULATOR);

    // allocate command receive buffer
    BigBuf_free_keep_EM();
    BigBuf_Clear_keep_EM();

    clear_trace();
    set_tracing(true);

    // ... if not PUPI/UID is supplied we always respond with ATQB, PUPI = 820de174, Application Data = 0x20381922,
    // supports only 106kBit/s in both directions, max frame size = 32Bytes,
    // supports ISO14443-4, FWI=8 (77ms), NAD supported, CID not supported:
    uint8_t respATQB[] = {
        0x50,
        0x82, 0x0d, 0xe1, 0x74,
        0x20, 0x38, 0x19,
        0x22, 0x00, 0x21, 0x85,
        0x5e, 0xd7
    };

    // ...PUPI/UID supplied from user. Adjust ATQB response accordingly
    if (memcmp("\x00\x00\x00\x00", pupi, 4) != 0) {
        memcpy(respATQB + 1, pupi, 4);
        AddCrc14B(respATQB, 12);
    }

    // response to HLTB and ATTRIB
    static const uint8_t respOK[] = {0x00, 0x78, 0xF0};

    uint16_t len, cmdsReceived = 0;
    int cardSTATE = SIM_POWER_OFF;
    int vHf = 0; // in mV

    const tosend_t *ts = get_tosend();

    uint8_t *receivedCmd = BigBuf_calloc(MAX_FRAME_SIZE);

    // prepare "ATQB" tag answer (encoded):
    CodeIso14443bAsTag(respATQB, sizeof(respATQB));
    uint8_t *encodedATQB = BigBuf_malloc(ts->max);
    uint16_t encodedATQBLen = ts->max;
    memcpy(encodedATQB, ts->buf, ts->max);


    // prepare "OK" tag answer (encoded):
    CodeIso14443bAsTag(respOK, sizeof(respOK));
    uint8_t *encodedOK = BigBuf_malloc(ts->max);
    uint16_t encodedOKLen = ts->max;
    memcpy(encodedOK, ts->buf, ts->max);

    // Simulation loop
    while (BUTTON_PRESS() == false) {
        WDT_HIT();

        //iceman: limit with 2000 times..
        if (data_available()) {
            break;
        }

        // find reader field
        vHf = (MAX_ADC_HF_VOLTAGE * SumAdc(ADC_CHAN_HF, 32)) >> 15;
        if (vHf > MF_MINFIELDV) {
            if (cardSTATE == SIM_POWER_OFF) {
                cardSTATE = SIM_IDLE;
                LED_A_ON();
            }
        } else {
            cardSTATE = SIM_POWER_OFF;
            LED_A_OFF();
        }

        if (cardSTATE == SIM_POWER_OFF) {
            continue;
        }

        // Get reader command
        if (GetIso14443bCommandFromReader(receivedCmd, &len) == false) {
            Dbprintf("button pressed, received %d commands", cmdsReceived);
            break;
        }

        LogTrace(receivedCmd, len, 0, 0, NULL, true);

        if ((len == 5) && (receivedCmd[0] == ISO14443B_REQB) && (receivedCmd[2] & 0x08)) {
            // WUPB
            switch (cardSTATE) {
                case SIM_IDLE:
                case SIM_READY:
                case SIM_HALT: {
                    TransmitFor14443b_AsTag(encodedATQB, encodedATQBLen);
                    LogTrace(respATQB, sizeof(respATQB), 0, 0, NULL, false);
                    cardSTATE = SIM_READY;
                    break;
                }
                case SIM_ACTIVE:
                default: {
                    TransmitFor14443b_AsTag(encodedATQB, encodedATQBLen);
                    LogTrace(respATQB, sizeof(respATQB), 0, 0, NULL, false);
                    break;
                }
            }
        } else if ((len == 5) && (receivedCmd[0] == ISO14443B_REQB) && !(receivedCmd[2] & 0x08)) {
            // REQB
            switch (cardSTATE) {
                case SIM_IDLE:
                case SIM_READY: {
                    TransmitFor14443b_AsTag(encodedATQB, encodedATQBLen);
                    LogTrace(respATQB, sizeof(respATQB), 0, 0, NULL, false);
                    cardSTATE = SIM_READY;
                    break;
                }
                case SIM_ACTIVE: {
                    TransmitFor14443b_AsTag(encodedATQB, encodedATQBLen);
                    LogTrace(respATQB, sizeof(respATQB), 0, 0, NULL, false);
                    break;
                }
                case SIM_HALT:
                default: {
                    break;
                }
            }
        } else if ((len == 7) && (receivedCmd[0] == ISO14443B_HALT)) {
            // HLTB
            switch (cardSTATE) {
                case SIM_READY: {
                    TransmitFor14443b_AsTag(encodedOK, encodedOKLen);
                    LogTrace(respOK, sizeof(respOK), 0, 0, NULL, false);
                    cardSTATE = SIM_HALT;
                    break;
                }
                case SIM_IDLE:
                case SIM_ACTIVE: {
                    TransmitFor14443b_AsTag(encodedOK, encodedOKLen);
                    LogTrace(respOK, sizeof(respOK), 0, 0, NULL, false);
                    break;
                }
                case SIM_HALT:
                default: {
                    break;
                }
            }
        } else if (len == 11 && receivedCmd[0] == ISO14443B_ATTRIB) {
            // ATTRIB
            switch (cardSTATE) {
                case SIM_READY: {
                    TransmitFor14443b_AsTag(encodedOK, encodedOKLen);
                    LogTrace(respOK, sizeof(respOK), 0, 0, NULL, false);
                    cardSTATE = SIM_ACTIVE;
                    break;
                }
                case SIM_IDLE:
                case SIM_ACTIVE: {
                    TransmitFor14443b_AsTag(encodedOK, encodedOKLen);
                    LogTrace(respOK, sizeof(respOK), 0, 0, NULL, false);
                    break;
                }
                case SIM_HALT:
                default: {
                    break;
                }
            }
        }

        ++cmdsReceived;
    }

    switch_off();
    if (g_dbglevel >= DBG_DEBUG) {
        Dbprintf("Emulator stopped. Trace length: %d ", BigBuf_get_traceLen());
    }
}

/*
void Simulate_iso14443b_srx_tag(uint8_t *uid) {

    LED_A_ON();
    / SRI512

    > initiate  06 00       ISO14443B_INITIATE
    < xx crc crc
    > select 0e xx          ISO14443B_SELECT
    < xx nn nn

    > readblock 08 blck_no  ISO14443B_READ_BLK
    < d0 d1 d2 d3 2byte crc

    > get uid               ISO14443B_GET_UID
    < 81  93  99  20  92  11  02  (8byte UID in MSB  D002 199220 999381)

#define ISO14443B_REQB         0x05
#define ISO14443B_ATTRIB       0x1D
#define ISO14443B_HALT         0x50
#define ISO14443B_INITIATE     0x06
#define ISO14443B_SELECT       0x0E
#define ISO14443B_GET_UID      0x0B
#define ISO14443B_READ_BLK     0x08
#define ISO14443B_WRITE_BLK    0x09
#define ISO14443B_RESET        0x0C
#define ISO14443B_COMPLETION   0x0F
#define ISO14443B_AUTHENTICATE 0x0A
#define ISO14443B_PING         0xBA
#define ISO14443B_PONG         0xAB


    static const uint8_t resp_init_srx[] = { 0x73, 0x64, 0xb1 };
    uint8_t resp_select_srx[] = { 0x73, 0x64, 0xb1 };

    // a default uid, or user supplied
    uint8_t resp_getuid_srx[10] = {
        0x81, 0x93, 0x99, 0x20, 0x92, 0x11, 0x02, 0xD0, 0x00, 0x00
    };

    // ...UID supplied from user. Adjust ATQB response accordingly
    if (memcmp("\x00\x00\x00\x00\x00\x00\x00\x00", uid, 8) != 0) {
        memcpy(resp_getuid_srx, uid, 8);
        AddCrc14B(resp_getuid_srx, 8);
    }

    // response to HLTB and ATTRIB
    static const uint8_t respOK[] = {0x00, 0x78, 0xF0};

    // setup device.
    FpgaDownloadAndGo(FPGA_BITSTREAM_HF);

    // connect Demodulated Signal to ADC:
    SetAdcMuxFor(GPIO_MUXSEL_HIPKD);

    // Set up the synchronous serial port
    FpgaSetupSsc(FPGA_MAJOR_MODE_HF_SIMULATOR);

    // allocate command receive buffer
    BigBuf_free();
    BigBuf_Clear_ext(false);
    set_tracing(true);

    uint16_t len, cmdsReceived = 0;
    int cardSTATE = SIM_NOFIELD;
    int vHf = 0; // in mV

    tosend_t *ts = get_tosend();

    uint8_t *receivedCmd = BigBuf_malloc(MAX_FRAME_SIZE);

    // prepare "ATQB" tag answer (encoded):
    CodeIso14443bAsTag(respATQB, sizeof(respATQB));
    uint8_t *encodedATQB = BigBuf_malloc(ts->max);
    uint16_t encodedATQBLen = ts->max;
    memcpy(encodedATQB, ts->buf, ts->max);


    // prepare "OK" tag answer (encoded):
    CodeIso14443bAsTag(respOK, sizeof(respOK));
    uint8_t *encodedOK = BigBuf_malloc(ts->max);
    uint16_t encodedOKLen = ts->max;
    memcpy(encodedOK, ts->buf, ts->max);

    // Simulation loop
    while (BUTTON_PRESS() == false) {
        WDT_HIT();

        //iceman: limit with 2000 times..
        if (data_available()) {
            break;
        }

        // find reader field
        if (cardSTATE == SIM_NOFIELD) {

            vHf = (MAX_ADC_HF_VOLTAGE * SumAdc(ADC_CHAN_HF, 32)) >> 15;
            if (vHf > MF_MINFIELDV) {
                cardSTATE = SIM_IDLE;
                LED_A_ON();
            }
        }
        if (cardSTATE == SIM_NOFIELD) continue;

        // Get reader command
        if (GetIso14443bCommandFromReader(receivedCmd, &len) == false) {
            Dbprintf("button pressed, received %d commands", cmdsReceived);
            break;
        }

        // ISO14443-B protocol states:
        // REQ or WUP request in ANY state
        // WUP in HALTED state
        if (len == 5) {
            if ((receivedCmd[0] == ISO14443B_REQB && (receivedCmd[2] & 0x8) == 0x8 && cardSTATE == SIM_HALTED) ||
                    receivedCmd[0] == ISO14443B_REQB) {
                LogTrace(receivedCmd, len, 0, 0, NULL, true);
                cardSTATE = SIM_SELECTING;
            }
        }

        /
        * How should this flow go?
        *  REQB or WUPB
        *   send response  ( waiting for Attrib)
        *  ATTRIB
        *   send response  ( waiting for commands 7816)
        *  HALT
            send halt response ( waiting for wupb )
        /

        switch (cardSTATE) {
            //case SIM_NOFIELD:
            case SIM_HALTED:
            case SIM_IDLE: {
                LogTrace(receivedCmd, len, 0, 0, NULL, true);
                break;
            }
            case SIM_SELECTING: {
                TransmitFor14443b_AsTag(encodedATQB, encodedATQBLen);
                LogTrace(respATQB, sizeof(respATQB), 0, 0, NULL, false);
                cardSTATE = SIM_WORK;
                break;
            }
            case SIM_HALTING: {
                TransmitFor14443b_AsTag(encodedOK, encodedOKLen);
                LogTrace(respOK, sizeof(respOK), 0, 0, NULL, false);
                cardSTATE = SIM_HALTED;
                break;
            }
            case SIM_ACKNOWLEDGE: {
                TransmitFor14443b_AsTag(encodedOK, encodedOKLen);
                LogTrace(respOK, sizeof(respOK), 0, 0, NULL, false);
                cardSTATE = SIM_IDLE;
                break;
            }
            case SIM_WORK: {
                if (len == 7 && receivedCmd[0] == ISO14443B_HALT) {
                    cardSTATE = SIM_HALTED;
                } else if (len == 11 && receivedCmd[0] == ISO14443B_ATTRIB) {
                    cardSTATE = SIM_ACKNOWLEDGE;
                } else {
                    // Todo:
                    // - SLOT MARKER
                    // - ISO7816
                    // - emulate with a memory dump
                    if (g_dbglevel >= DBG_DEBUG)
                        Dbprintf("new cmd from reader: len=%d, cmdsRecvd=%d", len, cmdsReceived);

                    // CRC Check
                    if (len >= 3) { // if crc exists

                        if (check_crc(CRC_14443_B, receivedCmd, len) == false) {
                            if (g_dbglevel >= DBG_DEBUG) {
                                DbpString("CRC fail");
                            }
                        }
                    } else {
                        if (g_dbglevel >= DBG_DEBUG) {
                            DbpString("CRC ok");
                        }
                    }
                    cardSTATE = SIM_IDLE;
                }
                break;
            }
            default:
                break;
        }

        ++cmdsReceived;
    }

    if (g_dbglevel >= DBG_DEBUG)
        Dbprintf("Emulator stopped. Trace length: %d ", BigBuf_get_traceLen());

    switch_off(); //simulate
}
*/

//=============================================================================
// An ISO 14443 Type B reader. We take layer two commands, code them
// appropriately, and then send them to the tag. We then listen for the
// tag's response, which we leave in the buffer to be demodulated on the
// PC side.
//=============================================================================
// We support both 14b framing and 14b' framing.
// 14b framing looks like:
// xxxxxxxx1111111111111111-000000000011-0........1-0........1-0........1-1-0........1-0........1-1000000000011xxxxxx
//         TR1              SOF 10*0+2*1 start-stop  ^^^^^^^^byte         ^ occasional stuff bit   EOF 10*0+N*1
// 14b' framing looks like:
// xxxxxxxxxxxxxxxx111111111111111111111-0........1-0........1-0........1-1-0........1-0........1-000000000000xxxxxxx
//                 SOF?                  start-stop  ^^^^^^^^byte         ^ occasional stuff bit  EOF

/*
 * Handles reception of a bit from the tag
 *
 * This function is called 2 times per bit (every 4 subcarrier cycles).
 * Subcarrier frequency fs is 848kHz, 1/fs = 1,18us, i.e. function is called every 4,72us
 *
 * LED handling:
 * LED C -> ON once we have received the SOF and are expecting the rest.
 * LED C -> OFF once we have received EOF or are unsynced
 *
 * Returns: true if we received a EOF
 *          false if we are still waiting for some more
 *
 */
static RAMFUNC int Handle14443bSamplesFromTag(int ci, int cq) {

    int v = 0;

// The soft decision on the bit uses an estimate of just the
// quadrant of the reference angle, not the exact angle.
#define MAKE_SOFT_DECISION() { \
        if(Demod.sumI > 0) { \
            v = ci; \
        } else { \
            v = -ci; \
        } \
        if(Demod.sumQ > 0) { \
            v += cq; \
        } else { \
            v -= cq; \
        } \
    }

#define SUBCARRIER_DETECT_THRESHOLD  8
// Subcarrier amplitude v = sqrt(ci^2 + cq^2), approximated here by max(abs(ci),abs(cq)) + 1/2*min(abs(ci),abs(cq)))
#define AMPLITUDE(ci,cq) (MAX(ABS(ci),ABS(cq)) + (MIN(ABS(ci),ABS(cq))/2))

    switch (Demod.state) {

        case DEMOD_UNSYNCD: {
            if (AMPLITUDE(ci, cq) > SUBCARRIER_DETECT_THRESHOLD) {  // subcarrier detected
                Demod.state = DEMOD_PHASE_REF_TRAINING;
                Demod.sumI = ci;
                Demod.sumQ = cq;
                Demod.posCount = 1;
            }
            break;
        }
        case DEMOD_PHASE_REF_TRAINING: {
            // While we get a constant signal
            if (AMPLITUDE(ci, cq) > SUBCARRIER_DETECT_THRESHOLD) {
                if (((ABS(Demod.sumI) > ABS(Demod.sumQ)) && (((ci > 0) && (Demod.sumI > 0)) || ((ci < 0) && (Demod.sumI < 0)))) ||  // signal closer to horizontal, polarity check based on on I
                        ((ABS(Demod.sumI) <= ABS(Demod.sumQ)) && (((cq > 0) && (Demod.sumQ > 0)) || ((cq < 0) && (Demod.sumQ < 0))))) { // signal closer to vertical, polarity check based on on Q

                    if (Demod.posCount < 10) {  // refine signal approximation during first 10 samples
                        Demod.sumI += ci;
                        Demod.sumQ += cq;
                    }
                    Demod.posCount += 1;
                } else {
                    // transition
                    if (Demod.posCount < 10) {
                        // subcarrier lost
                        Demod.state = DEMOD_UNSYNCD;
                        break;
                    } else {
                        // at this point it can be start of 14b' data or start of 14b SOF
                        MAKE_SOFT_DECISION();
                        Demod.posCount = 1;             // this was the first half
                        Demod.thisBit = v;
                        Demod.shiftReg = 0;
                        Demod.state = DEMOD_RECEIVING_DATA;
                    }
                }
            } else {
                // subcarrier lost
                Demod.state = DEMOD_UNSYNCD;
            }
            break;
        }
        case DEMOD_AWAITING_START_BIT: {
            Demod.posCount++;
            MAKE_SOFT_DECISION();
            if (v > 0) {
                if (Demod.posCount > 3 * 2) {       // max 19us between characters = 16 1/fs, max 3 etu after low phase of SOF = 24 1/fs
                    LED_C_OFF();
                    if (Demod.bitCount == 0 && Demod.len == 0) { // received SOF only, this is valid for iClass/Picopass
                        return true;
                    } else {
                        Demod.state = DEMOD_UNSYNCD;
                    }
                }
            } else {                            // start bit detected
                Demod.posCount = 1;             // this was the first half
                Demod.thisBit = v;
                Demod.shiftReg = 0;
                Demod.state = DEMOD_RECEIVING_DATA;
            }
            break;
        }
        case WAIT_FOR_RISING_EDGE_OF_SOF: {

            Demod.posCount++;
            MAKE_SOFT_DECISION();
            if (v > 0) {
                if (Demod.posCount < 9 * 2) { // low phase of SOF too short (< 9 etu). Note: spec is >= 10, but FPGA tends to "smear" edges
                    Demod.state = DEMOD_UNSYNCD;
                } else {
                    LED_C_ON(); // Got SOF
                    Demod.posCount = 0;
                    Demod.bitCount = 0;
                    Demod.len = 0;
                    Demod.state = DEMOD_AWAITING_START_BIT;
                }
            } else {
                if (Demod.posCount > 12 * 2) { // low phase of SOF too long (> 12 etu)
                    Demod.state = DEMOD_UNSYNCD;
                    LED_C_OFF();
                }
            }
            break;
        }
        case DEMOD_RECEIVING_DATA: {

            MAKE_SOFT_DECISION();

            if (Demod.posCount == 0) {          // first half of bit
                Demod.thisBit = v;
                Demod.posCount = 1;
            } else {                            // second half of bit
                Demod.thisBit += v;

                Demod.shiftReg >>= 1;
                if (Demod.thisBit > 0) {    // logic '1'
                    Demod.shiftReg |= 0x200;
                }

                Demod.bitCount++;
                if (Demod.bitCount == 10) {

                    uint16_t s = Demod.shiftReg;

                    if ((s & 0x200) && !(s & 0x001)) { // stop bit == '1', start bit == '0'
                        Demod.output[Demod.len] = (s >> 1);
                        Demod.len++;
                        Demod.bitCount = 0;
                        Demod.state = DEMOD_AWAITING_START_BIT;
                    } else {
                        if (s == 0x000) {
                            if (Demod.len > 0) {
                                LED_C_OFF();
                                // This is EOF (start, stop and all data bits == '0'
                                return true;
                            } else {
                                // Zeroes but no data acquired yet?
                                // => Still in SOF of 14b, wait for raising edge
                                Demod.posCount = 10 * 2;
                                Demod.bitCount = 0;
                                Demod.len = 0;
                                Demod.state = WAIT_FOR_RISING_EDGE_OF_SOF;
                                break;
                            }
                        }
                        if (AMPLITUDE(ci, cq) < SUBCARRIER_DETECT_THRESHOLD) {
                            LED_C_OFF();
                            // subcarrier lost
                            Demod.state = DEMOD_UNSYNCD;
                            if (Demod.len > 0) { // no EOF but no signal anymore and we got data, e.g. ASK CTx
                                return true;
                            }
                        }
                        // we have still signal but no proper byte or EOF? this shouldn't happen
                        //Demod.posCount = 10 * 2;
                        Demod.bitCount = 0;
                        Demod.len = 0;
                        Demod.state = WAIT_FOR_RISING_EDGE_OF_SOF;
                        break;
                    }
                }
                Demod.posCount = 0;
            }
            break;
        }
        default: {
            Demod.state = DEMOD_UNSYNCD;
            LED_C_OFF();
            break;
        }
    }
    return false;
}

/*
 *  Demodulate the samples we received from the tag, also log to tracebuffer
 */
static int Get14443bAnswerFromTag(uint8_t *response, uint16_t max_len, uint32_t timeout, uint32_t *eof_time, uint16_t *retlen) {

    // Set up the demodulator for tag -> reader responses.
    Demod14bInit(response, max_len);

    // The DMA buffer, used to stream samples from the FPGA
    dmabuf16_t *dma = get_dma16();
    if (dma == NULL) {
        if (g_dbglevel >= DBG_DEBUG) Dbprintf("Failed to allocate memory");
        return PM3_EMALLOC;
    }

    if (FpgaSetupSscDma((uint8_t *) dma->buf, DMA_BUFFER_SIZE) == false) {
        if (g_dbglevel > DBG_ERROR) Dbprintf("FpgaSetupSscDma failed. Exiting");
        return PM3_EMALLOC;
    }

    uint32_t dma_start_time = 0;
    uint16_t *upTo = dma->buf;
    int samples = 0, ret = 0;

    // Put FPGA in the appropriate mode
    LED_D_ON();
    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_READER | FPGA_HF_READER_SUBCARRIER_848_KHZ | FPGA_HF_READER_MODE_RECEIVE_IQ);

    for (;;) {

        volatile uint16_t behindBy = ((uint16_t *)AT91C_BASE_PDC_SSC->PDC_RPR - upTo) & (DMA_BUFFER_SIZE - 1);
        if (behindBy == 0) {
            continue;
        }

        samples++;

        if (samples == 1) {
            // DMA has transferred the very first data
            dma_start_time = GetCountSspClk() & 0xfffffff0;
        }

        volatile int8_t ci = *upTo >> 8;
        volatile int8_t cq = *upTo;
        upTo++;

        // we have read all of the DMA buffer content.
        if (upTo >= dma->buf + DMA_BUFFER_SIZE) {

            // start reading the circular buffer from the beginning again
            upTo = dma->buf;

            // DMA Counter Register had reached 0, already rotated.
            if (AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_ENDRX)) {

                // primary buffer was stopped
                if (AT91C_BASE_PDC_SSC->PDC_RCR == 0) {
                    AT91C_BASE_PDC_SSC->PDC_RPR = (uint32_t) dma->buf;
                    AT91C_BASE_PDC_SSC->PDC_RCR = DMA_BUFFER_SIZE;
                }
                // secondary buffer sets as primary, secondary buffer was stopped
                if (AT91C_BASE_PDC_SSC->PDC_RNCR == 0) {
                    AT91C_BASE_PDC_SSC->PDC_RNPR = (uint32_t) dma->buf;
                    AT91C_BASE_PDC_SSC->PDC_RNCR = DMA_BUFFER_SIZE;
                }

                WDT_HIT();
                if (BUTTON_PRESS()) {
                    DbpString("stopped");
                    break;
                }
            }
        }

        if (Handle14443bSamplesFromTag(ci, cq)) {

            *eof_time = GetCountSspClkDelta(dma_start_time) - DELAY_TAG_TO_ARM;  // end of EOF

            if (Demod.len > Demod.max_len) {
                ret = PM3_EOVFLOW;
            }
            break;
        }

        if (((GetCountSspClkDelta(dma_start_time)) > timeout) && Demod.state < DEMOD_PHASE_REF_TRAINING) {
            ret = PM3_ETIMEOUT;
            break;
        }
    }

    FpgaDisableSscDma();
    if (ret < 0) {
        return ret;
    }

    if (Demod.len) {

        // 3 * 10  + 20 + 10 == 60,    60 << 5  = 1920

        // We are counting backwards here from EOF
        // tranfers time (in ETU) for
        //                         bytes            SOF      EOF
        uint32_t deduct = (Demod.len * (8 + 2)) + (10 + 2) + 10;
        uint32_t sof_time = *eof_time - HF14_ETU_TO_SSP(deduct);
        LogTrace(Demod.output, Demod.len, sof_time, *eof_time, NULL, false);
    }

    if (retlen) {
        *retlen = Demod.len;
    }
    return PM3_SUCCESS;
}

//-----------------------------------------------------------------------------
// Transmit the command (to the tag) that was placed in ToSend[].
//-----------------------------------------------------------------------------
// param start_time in SSP_CLK
static void TransmitFor14443b_AsReader(uint32_t *start_time) {

    tosend_t *ts = get_tosend();

#ifdef RDV4
    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_READER | FPGA_HF_READER_MODE_SEND_SHALLOW_MOD_RDV4);
#else
    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_READER | FPGA_HF_READER_MODE_SEND_SHALLOW_MOD);
#endif

    // TR2 minimum 14 ETUs
    if (*start_time < ISO14B_TR0) {
//        *start_time = DELAY_ARM_TO_TAG;
        *start_time = ISO14B_TR0;
    }

//    *start_time = (*start_time - DELAY_ARM_TO_TAG) & 0xfffffff0;
    *start_time = (*start_time & 0xfffffff0);

    if (GetCountSspClk() > *start_time) { // we may miss the intended time
        *start_time = (GetCountSspClk() + 32) & 0xfffffff0; // next possible time
    }

    // wait
    while (GetCountSspClk() < *start_time);

    LED_B_ON();
    for (int c = 0; c < ts->max; c++) {
        volatile uint8_t data = ts->buf[c];

        for (uint8_t i = 0; i < 8; i++) {
            volatile uint16_t send_word = (data & 0x80) ? 0x0000 : 0xFFFF;

            while (!(AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_TXRDY))) ;
            AT91C_BASE_SSC->SSC_THR = send_word;

            while (!(AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_TXRDY))) ;
            AT91C_BASE_SSC->SSC_THR = send_word;

            data <<= 1;
        }
        WDT_HIT();
    }

    // transmit remaining bits. we need one-sample granularity now

    volatile uint8_t data = ts->buf[ts->max], last_bits = ts->bit;

    for (uint8_t i = 0; i < last_bits; i++) {
        volatile uint16_t send_word = (data & 0x80) ? 0x0000 : 0xFFFF;

        while (!(AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_TXRDY))) ;
        AT91C_BASE_SSC->SSC_THR = send_word;

        while (!(AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_TXRDY))) ;
        AT91C_BASE_SSC->SSC_THR = send_word;

        data <<= 1;
    }
    WDT_HIT();


    LED_B_OFF();

//    *start_time += DELAY_ARM_TO_TAG;

    // wait for last transfer to complete
    while (!(AT91C_BASE_SSC->SSC_SR & AT91C_SSC_TXEMPTY)) {};
}

//-----------------------------------------------------------------------------
// Code a layer 2 command (string of octets, including CRC) into ToSend[],
// so that it is ready to transmit to the tag using TransmitFor14443b().
//-----------------------------------------------------------------------------
static void CodeIso14443bAsReader(const uint8_t *cmd, int len, bool framing) {
    /*
    *   QUESTION:  how long is a 1 or 0 in pulses in the xcorr_848 mode?
    *              1 "stuffbit" = 1ETU (9us)
    *
    *   TR2  -  After the PICC response, the PCD is required to wait the Frame Delay Time (TR2)
                before transmission of the next command. The minimum frame delay time required for
                all commands is 14 ETUs
    *
    */
    int i;
    tosend_reset();

    // add framing enable flag.
    // xerox chips use unframed commands during anticollision
    if (framing) {
        // Send SOF
        // 10-11 ETUs of ZERO
        for (i = 0; i < 10; i++) {
            tosend_stuffbit(0);
        }
        // 2-3 ETUs of ONE
        tosend_stuffbit(1);
        tosend_stuffbit(1);
    }

    // Sending cmd, LSB
    // from here we add BITS
    for (i = 0; i < len; i++) {
        // Start bit
        tosend_stuffbit(0);

        // Data bits
        volatile uint8_t b = cmd[i];
        tosend_stuffbit(b & 1);
        tosend_stuffbit((b >> 1) & 1);
        tosend_stuffbit((b >> 2) & 1);
        tosend_stuffbit((b >> 3) & 1);
        tosend_stuffbit((b >> 4) & 1);
        tosend_stuffbit((b >> 5) & 1);
        tosend_stuffbit((b >> 6) & 1);
        tosend_stuffbit((b >> 7) & 1);

        // Stop bit
        tosend_stuffbit(1);
        // EGT extra guard time  1 ETU = 9us
        // For PCD it ranges 0-57us === 0 - 6 ETU
        // FOR PICC it ranges 0-19us == 0 - 2 ETU
    }

    if (framing) {
        // Send EOF
        // 10-11 ETUs of ZERO
        for (i = 0; i < 10; i++) {
            tosend_stuffbit(0);
        }
    }

    // we can't use padding now
    /*
        int pad = (10 + 2 + (len * 10) + 10) & 0x7;
        for (i = 0; i < 16 - pad; ++i)
            tosend_stuffbit(1);
    */
}

/*
*  Convenience function to encode, transmit and trace iso 14443b comms
*/
void CodeAndTransmit14443bAsReader(const uint8_t *cmd, int len, uint32_t *start_time, uint32_t *eof_time, bool framing) {
    const tosend_t *ts = get_tosend();
    CodeIso14443bAsReader(cmd, len, framing);
    TransmitFor14443b_AsReader(start_time);
    if (g_trigger) LED_A_ON();

// eof_time in ssp clocks, but bits was added here!
//    *eof_time = *start_time + (10 * ts->max) + 10 + 2 + 10;

    *eof_time = *start_time + HF14_ETU_TO_SSP(8 * ts->max);

    LogTrace(cmd, len, *start_time, *eof_time, NULL, true);
}

/* Sends an APDU to the tag
 * TODO: check CRC and preamble
 */
int iso14443b_apdu(uint8_t const *msg, size_t msg_len, bool send_chaining, void *rxdata, uint16_t rxmaxlen, uint8_t *response_byte, uint16_t *responselen) {

    uint8_t real_cmd[msg_len + 4];

    if (msg_len) {
        // ISO 14443 APDU frame: PCB [CID] [NAD] APDU CRC PCB=0x02
        real_cmd[0] = 0x02; // bnr, nad, cid, chn=0; i-block(0x00)

        if (send_chaining) {
            real_cmd[0] |= 0x10;
        }

        // put block number into the PCB
        real_cmd[0] |= s_iso14b_pcb_blocknum;
        memcpy(real_cmd + 1, msg, msg_len);
    } else {
        // R-block. ACK
        real_cmd[0] = 0xA2; // r-block + ACK
        real_cmd[0] |= s_iso14b_pcb_blocknum;
    }

    AddCrc14B(real_cmd, msg_len + 1);

    // send
    uint32_t start_time = 0;
    uint32_t eof_time = 0;
    CodeAndTransmit14443bAsReader(real_cmd, msg_len + 3, &start_time, &eof_time, true);

    eof_time += DELAY_ISO14443B_PCD_TO_PICC_READER;

    // Activation frame waiting time
    // 65536/fc == 4833 µS
    // SSP_CLK =  4833 µS * 3.39 = 16384

    uint16_t len = 0;
    int status = Get14443bAnswerFromTag(rxdata, rxmaxlen, s_iso14b_timeout, &eof_time, &len);
    if (status != PM3_SUCCESS) {
        return status;
    }

    uint8_t *data_bytes = (uint8_t *) rxdata;

    if (len) {
        // S-Block WTX
        while (len && ((data_bytes[0] & 0xF2) == 0xF2)) {

            uint32_t save_iso14b_timeout_spp = s_iso14b_timeout;

            // 2 high bits mandatory set to 0b
            // byte1 - WTXM [1..59].
            uint8_t wtxm = data_bytes[1] & 0x3F;

            // command FWT = FWT * WTXM
            uint32_t fwt_temp = (s_iso14b_fwt * wtxm);

            // temporarily increase timeout
            iso14b_set_timeout((32 << fwt_temp));

            // Transmit WTX back
            data_bytes[1] = wtxm;

            // now need to fix CRC.
            AddCrc14B(data_bytes, len - 2);

            // transmit S-Block
            CodeAndTransmit14443bAsReader(data_bytes, len, &start_time, &eof_time, true);

            // retrieve the result again (with increased timeout)
            eof_time += DELAY_ISO14443B_PCD_TO_PICC_READER;

            if (Get14443bAnswerFromTag(rxdata, rxmaxlen, s_iso14b_timeout, &eof_time, &len) != PM3_SUCCESS) {
                return PM3_ECARDEXCHANGE;
            }

            data_bytes = rxdata;

            // restore timeout
            s_iso14b_timeout = save_iso14b_timeout_spp;
        }

        // if we received an I- or R(ACK)-Block with a block number equal to the
        // current block number, toggle the current block number

        if ((len >= 3) &&                                                             // PCB + CRC = 3 bytes
                (((data_bytes[0] & 0xC0) == 0) || (data_bytes[0] & 0xD0) == 0x80)  &&  // I-Block   OR   R-Block with ACK bit set to 0
                ((data_bytes[0] & 0x01) == s_iso14b_pcb_blocknum)) {                     // equal block numbers

            s_iso14b_pcb_blocknum ^= 1;

        }

        // if we received I-block with chaining we need to send ACK and receive another block of data
        if (response_byte) {
            *response_byte = data_bytes[0];
        }

        // crc check
        if (len >= 3 && (check_crc(CRC_14443_B, data_bytes, len) == false)) {
            return PM3_ECRC;
        }
    }

    // cut frame byte
    if (len) {
        len -= 1;

        for (int i = 0; i < len; i++) {
            data_bytes[i] = data_bytes[i + 1];
        }
    }

    if (responselen) {
        *responselen = len;
    }
    return PM3_SUCCESS;
}

/**
* ASK CTS initialise.
*/
static int iso14443b_select_cts_card(iso14b_cts_card_select_t *card) {
    // INITIATE command: wake up the tag using the INITIATE
    uint8_t cmdINIT[] = {ASK_REQT, 0xF9, 0xE0};
    uint8_t cmdMSBUID[] = {ASK_SELECT, 0xFF, 0xFF, 0x00, 0x00};
    uint8_t cmdLSBUID[] = {0xC4, 0x00, 0x00};

// iceman: todo static crc
    AddCrc14B(cmdMSBUID, 3);
    AddCrc14B(cmdLSBUID, 1);

    uint8_t r[8] = { 0x00 };

    uint32_t start_time = 0;
    uint32_t eof_time = 0;
    CodeAndTransmit14443bAsReader(cmdINIT, sizeof(cmdINIT), &start_time, &eof_time, true);

    eof_time += DELAY_ISO14443B_PCD_TO_PICC_READER;
    uint16_t retlen = 0;
    if (Get14443bAnswerFromTag(r, sizeof(r), s_iso14b_timeout, &eof_time, &retlen) != PM3_SUCCESS) {
        return PM3_ECARDEXCHANGE;
    }

    if (retlen != 4) {
        return PM3_ELENGTH;
    }
    if (check_crc(CRC_14443_B, r, retlen) == false) {
        return PM3_ECRC;
    }

    if (card) {
        // pc. fc  Product code, Facility code
        card->pc = r[0];
        card->fc = r[1];
    }

    start_time = eof_time + ISO14B_TR2;
    CodeAndTransmit14443bAsReader(cmdMSBUID, sizeof(cmdMSBUID), &start_time, &eof_time, true);

    eof_time += DELAY_ISO14443B_PCD_TO_PICC_READER;
    if (Get14443bAnswerFromTag(r, sizeof(r), s_iso14b_timeout, &eof_time, &retlen) != PM3_SUCCESS) {
        return PM3_ECARDEXCHANGE;
    }

    if (retlen != 4) {
        return PM3_ELENGTH;
    }
    if (check_crc(CRC_14443_B, r, retlen) == false) {
        return PM3_ECRC;
    }

    if (card) {
        memcpy(card->uid, r, 2);
    }

    start_time = eof_time + ISO14B_TR2;
    CodeAndTransmit14443bAsReader(cmdLSBUID, sizeof(cmdLSBUID), &start_time, &eof_time, true);

    eof_time += DELAY_ISO14443B_PCD_TO_PICC_READER;
    if (Get14443bAnswerFromTag(r, sizeof(r), s_iso14b_timeout, &eof_time, &retlen) != PM3_SUCCESS) {
        return PM3_ECARDEXCHANGE;
    }

    if (retlen != 4) {
        return PM3_ELENGTH;
    }
    if (check_crc(CRC_14443_B, r, retlen) == false) {
        return PM3_ECRC;
    }

    if (card) {
        memcpy(card->uid + 2, r, 2);
    }

    return PM3_SUCCESS;
}
/**
* SRx Initialise.
*/
int iso14443b_select_srx_card(iso14b_card_select_t *card) {
    // INITIATE command: wake up the tag using the INITIATE
    static const uint8_t init_srx[] = { ISO14443B_INITIATE, 0x00, 0x97, 0x5b };
    uint8_t r_init[3] = { 0x00 };
    uint8_t r_select[3] = { 0x00 };
    uint8_t r_papid[10] = { 0x00 };

    uint32_t start_time = 0;
    uint32_t eof_time = 0;
    CodeAndTransmit14443bAsReader(init_srx, sizeof(init_srx), &start_time, &eof_time, true);

    eof_time += DELAY_ISO14443B_PCD_TO_PICC_READER;
    uint16_t retlen = 0;
    if (Get14443bAnswerFromTag(r_init, sizeof(r_init), s_iso14b_timeout, &eof_time, &retlen) != PM3_SUCCESS) {
        return PM3_ECARDEXCHANGE;
    }

    // Randomly generated Chip ID
    if (card) {
        card->chipid = Demod.output[0];
    }

    // SELECT command (with space for CRC)
    uint8_t select_srx[] = { ISO14443B_SELECT, 0x00, 0x00, 0x00};
    select_srx[1] = r_init[0];

    AddCrc14B(select_srx, 2);

    start_time = eof_time + ISO14B_TR2;
    CodeAndTransmit14443bAsReader(select_srx, sizeof(select_srx), &start_time, &eof_time, true);

    eof_time += DELAY_ISO14443B_PCD_TO_PICC_READER;
    if (Get14443bAnswerFromTag(r_select, sizeof(r_select), s_iso14b_timeout, &eof_time, &retlen) != PM3_SUCCESS) {
        return PM3_ECARDEXCHANGE;
    }

    if (retlen != 3) {
        return PM3_ELENGTH;
    }
    if (check_crc(CRC_14443_B, r_select, retlen) == false) {
        return PM3_ECRC;
    }

    // Check response from the tag: should be the same UID as the command we just sent:
    if (select_srx[1] != r_select[0]) {
        return PM3_EWRONGANSWER;
    }

    // First get the tag's UID:
    select_srx[0] = ISO14443B_GET_UID;
    select_srx[1] = 0xAB;
    select_srx[2] = 0x4E;

    start_time = eof_time + ISO14B_TR2;
    CodeAndTransmit14443bAsReader(select_srx, 3, &start_time, &eof_time, true); // Only first three bytes for this one

    eof_time += DELAY_ISO14443B_PCD_TO_PICC_READER;
    if (Get14443bAnswerFromTag(r_papid, sizeof(r_papid), s_iso14b_timeout, &eof_time, & retlen) != PM3_SUCCESS) {
        return PM3_ECARDEXCHANGE;
    }

    if (retlen != 10) {
        return PM3_ELENGTH;
    }

    if (check_crc(CRC_14443_B, r_papid, retlen) == false) {
        return PM3_ECRC;
    }

    if (card) {
        card->uidlen = 8;
        memcpy(card->uid, r_papid, 8);
    }
    return PM3_SUCCESS;
}

// Xerox tag connect function: wup, anticoll, attrib, password
// the original chips require all commands in this sequence

// 0: OK, 1: select fail, 2: attrib fail, 3: crc fail, 4: password fail
static int iso14443b_select_xrx_card(iso14b_card_select_t *card) {
//                                          AFI
    static const uint8_t x_wup1[] = { 0x0D, 0x37, 0x21, 0x92, 0xf2 };
    static const uint8_t x_wup2[] = { 0x5D, 0x37, 0x21, 0x71, 0x71 };
    uint8_t slot_mark[1] = { 0x00 };
    uint8_t x_atqb[24] = { 0x00 };     // ATQB len = 18

    uint32_t start_time = 0;
    uint32_t eof_time = 0;

    // wait for carrier
    iso14b_set_timeout(24);

    // wup1
    CodeAndTransmit14443bAsReader(x_wup1, sizeof(x_wup1), &start_time, &eof_time, true);

    start_time = eof_time + US_TO_SSP(9000);    // 9ms before next cmd ( 30510 )

    // wup2
    CodeAndTransmit14443bAsReader(x_wup2, sizeof(x_wup2), &start_time, &eof_time, true);

    uint64_t uid = 0;
    uint16_t retlen = 0;

    for (uint8_t uid_pos = 0; uid_pos < 64; uid_pos += 2) {

        uint8_t slot;
        for (slot = 0; slot < 4; slot++) {

            // next slot after 24 ETU  (786)
            start_time = eof_time + HF14_ETU_TO_SSP(30);
            Get14443bAnswerFromTag(x_atqb, sizeof(x_atqb), s_iso14b_timeout, &eof_time, &retlen);
            if (retlen > 0) {
                Dbprintf("unexpected data %d", retlen);
                return PM3_ECARDEXCHANGE;
            }

            // tx unframed slot-marker
            if (Demod.posCount) {   // no rx, but subcarrier burst detected
                uid |= (uint64_t)slot << uid_pos;

                slot_mark[0] = 0xB1 + (slot << 1);  // ack slot
                CodeAndTransmit14443bAsReader(slot_mark, sizeof(slot_mark), &start_time, &eof_time, false);
                break;
            } else {        // no subcarrier burst
                slot_mark[0] = 0xA1 + (slot << 1);  // nak slot
                CodeAndTransmit14443bAsReader(slot_mark, sizeof(slot_mark), &start_time, &eof_time, false);
            }
        }

        if (4 == slot) {
            if (g_dbglevel >= DBG_DEBUG) {
                DbpString("no answer to anticollision");
            }
            return PM3_ESOFT;
        }
    }

    if (Get14443bAnswerFromTag(x_atqb, sizeof(x_atqb), s_iso14b_timeout, &eof_time, &retlen) != PM3_SUCCESS) {
        return PM3_ECARDEXCHANGE;
    }

    if (g_dbglevel >= DBG_DEBUG) {
        Dbprintf("anticollision uid %llx", uid);
    }

    // ATQB too short?
    if (retlen < 18) {
        return PM3_ELENGTH;
    }

    // VALIDATE CRC
    if (check_crc(CRC_14443_B, x_atqb, 18) == false) {      // use fixed len because unstable EOF catch
        return PM3_ECRC;
    }

    if (x_atqb[0] != 0x50) {
        return PM3_EWRONGANSWER;
    }

    if (card) {
        card->uidlen = 8;
        memcpy(card->uid, x_atqb + 1, 8);
        memcpy(card->atqb, x_atqb + 9, 7);
    }

    // send ATTRIB command

    uint8_t txbuf[18];

    txbuf[1]  = 0x1d;
    memcpy(txbuf + 2, &uid, 8);
    txbuf[10] = 0;
    txbuf[11] = 0xF;
    txbuf[12] = 1;
    txbuf[13] = 0xF;

    AddCrc14B(txbuf + 1, 13);

    start_time = eof_time + ISO14B_TR2;
    CodeAndTransmit14443bAsReader(txbuf + 1, 15, &start_time, &eof_time, true);

    eof_time += DELAY_ISO14443B_PCD_TO_PICC_READER;
    if (Get14443bAnswerFromTag(x_atqb, sizeof(x_atqb), s_iso14b_timeout, &eof_time, &retlen) != PM3_SUCCESS) {
        return PM3_ECARDEXCHANGE;
    }

    if (retlen < 3) {
        return PM3_ELENGTH;
    }

    if (check_crc(CRC_14443_B, x_atqb, 3) == false) {
        return PM3_ECRC;
    }

    if (x_atqb[0] != 0) {
        return PM3_EWRONGANSWER;
    }

    // apply PASSWORD command

    txbuf[0]  = 0x02;
    txbuf[1]  = 0x38;
    // uid from previous command used
    txbuf[10] = 0x03;
    txbuf[11] = 0x4e;
    txbuf[12] = 0x4b;
    txbuf[13] = 0x53;
    txbuf[14] = 0x4F;

    AddCrc14B(txbuf, 15);

    start_time = eof_time + ISO14B_TR2;
    CodeAndTransmit14443bAsReader(txbuf, 17, &start_time, &eof_time, true);

    eof_time += DELAY_ISO14443B_PCD_TO_PICC_READER;
    if (Get14443bAnswerFromTag(x_atqb, sizeof(x_atqb), s_iso14b_timeout, &eof_time, &retlen) != PM3_SUCCESS) {
        return PM3_ECARDEXCHANGE;
    }

    if (retlen < 4) {
        return PM3_ELENGTH;
    }

    if (check_crc(CRC_14443_B, x_atqb, 4) == false) {
        return PM3_ECRC;
    }

    if (x_atqb[0] != 0x02 || x_atqb[1] != 0x00) {
        return PM3_EWRONGANSWER;
    }

    return PM3_SUCCESS;
}

/* Perform the ISO 14443 B Card Selection procedure
 * Currently does NOT do any collision handling.
 * It expects 0-1 cards in the device's range.
 * TODO: Support multiple cards (perform anticollision)
 * TODO: Verify CRC checksums
 */
int iso14443b_select_card(iso14b_card_select_t *card) {
    // WUPB command (including CRC)
    // Note: WUPB wakes up all tags, REQB doesn't wake up tags in HALT state
    // WUTB or REQB  is denoted in the third byte, lower nibble.  0 vs 8
    //static const uint8_t wupb[] = { ISO14443B_REQB, 0x00, 0x08, 0x39, 0x73 };
    static const uint8_t wupb[] = { ISO14443B_REQB, 0x00, 0x00, 0x71, 0xff };

    // ATTRIB command (with space for CRC)
    uint8_t attrib[11] = { ISO14443B_ATTRIB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00};
    uint8_t r_pupid[14] = { 0x00 };
    uint8_t r_attrib[3] = { 0x00 };

    // first, wake up the tag
    uint32_t start_time = 0;
    uint32_t eof_time = 0;
    CodeAndTransmit14443bAsReader(wupb, sizeof(wupb), &start_time, &eof_time, true);

    eof_time += DELAY_ISO14443B_PCD_TO_PICC_READER;
    uint16_t retlen = 0;
    if (Get14443bAnswerFromTag(r_pupid, sizeof(r_pupid), s_iso14b_timeout, &eof_time, &retlen) != PM3_SUCCESS) {
        return PM3_ECARDEXCHANGE;
    }

    // ATQB too short?
    if (retlen < 14) {
        return PM3_ELENGTH;
    }

    // VALIDATE CRC
    if (check_crc(CRC_14443_B, r_pupid, retlen) == false) {
        return PM3_ECRC;
    }

    if (card) {
        card->uidlen = 4;
        memcpy(card->uid, r_pupid + 1, 4);
        memcpy(card->atqb, r_pupid + 5, 7);
    }

    // copy the PUPI to ATTRIB  ( PUPI == UID )
    memcpy(attrib + 1, r_pupid + 1, 4);

    // copy the protocol info from ATQB (Protocol Info -> Protocol_Type) into ATTRIB (Param 3)
    attrib[7] = r_pupid[10] & 0x0F;
    AddCrc14B(attrib, 9);
    start_time = eof_time + ISO14B_TR2;
    CodeAndTransmit14443bAsReader(attrib, sizeof(attrib), &start_time, &eof_time, true);

    eof_time += DELAY_ISO14443B_PCD_TO_PICC_READER;
    if (Get14443bAnswerFromTag(r_attrib, sizeof(r_attrib), s_iso14b_timeout, &eof_time, &retlen) != PM3_SUCCESS) {
        return PM3_ECARDEXCHANGE;
    }

    // Answer to ATTRIB too short?
    if (retlen < 3) {
        return PM3_ELENGTH;
    }

    // VALIDATE CRC
    if (check_crc(CRC_14443_B, r_attrib, retlen) == false) {
        return PM3_ECRC;
    }

    if (card) {

        // CID
        card->cid = r_attrib[0];

        // MAX FRAME
        uint16_t maxFrame = card->atqb[5] >> 4;
        if (maxFrame < 5)       maxFrame = 8 * maxFrame + 16;
        else if (maxFrame == 5) maxFrame = 64;
        else if (maxFrame == 6) maxFrame = 96;
        else if (maxFrame == 7) maxFrame = 128;
        else if (maxFrame == 8) maxFrame = 256;
        else maxFrame = 257;
        iso14b_set_maxframesize(maxFrame);

        // FWT
        uint8_t fwt = card->atqb[6] >> 4;
        if (fwt < 15) {
            iso14b_set_fwt(fwt);
        }
    }
    // reset PCB block number
    s_iso14b_pcb_blocknum = 0;
    return PM3_SUCCESS;
}

static int iso14443b_select_picopass_card(picopass_hdr_t *hdr) {

    static uint8_t act_all[] = { ICLASS_CMD_ACTALL };
    static uint8_t identify[] = { ICLASS_CMD_READ_OR_IDENTIFY };
    static uint8_t read_conf[] = { ICLASS_CMD_READ_OR_IDENTIFY, 0x01, 0xfa, 0x22 };

    // ICLASS_CMD_SELECT  0x81 tells  ISO14443b/BPSK coding/106 kbits/s
    // ICLASS_CMD_SELECT  0x41 tells  ISO14443b/BPSK coding/423 kbits/s
    uint8_t select[] = { 0x80 | ICLASS_CMD_SELECT, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint8_t read_aia[] = { ICLASS_CMD_READ_OR_IDENTIFY, 0x05, 0xde, 0x64};
    uint8_t read_check_cc[] = { 0x80 | ICLASS_CMD_READCHECK, 0x02 };
    uint8_t resp[ICLASS_BUFFER_SIZE] = {0};

    uint32_t start_time = 0;
    uint32_t eof_time = 0;
    uint16_t retlen = 0;

    // first, wake up the tag  0x0A
    CodeAndTransmit14443bAsReader(act_all, sizeof(act_all), &start_time, &eof_time, true);
    eof_time += DELAY_ISO14443B_PCD_TO_PICC_READER;

    // 0x0C
    // start_time = eof_time + ISO14B_TR2;
    start_time = eof_time + US_TO_SSP(330);    // 330ms before next cmd
    CodeAndTransmit14443bAsReader(identify, sizeof(identify), &start_time, &eof_time, true);
    eof_time += DELAY_ISO14443B_PCD_TO_PICC_READER;

    // expect a 10-byte response here, 8 byte anticollision-CSN and 2 byte CRC
    if (Get14443bAnswerFromTag(resp, sizeof(resp), s_iso14b_timeout, &eof_time, &retlen) != PM3_SUCCESS) {
        return PM3_ECARDEXCHANGE;
    }
    if (retlen != 10) {
        return PM3_ELENGTH;
    }

    // copy the Anti-collision CSN to our select-packet
    memcpy(&select[1], resp, 8);

    // select the card
    start_time = eof_time + ISO14B_TR2;
    CodeAndTransmit14443bAsReader(select, sizeof(select), &start_time, &eof_time, true);
    eof_time += DELAY_ISO14443B_PCD_TO_PICC_READER;

    // expect a 10-byte response here, 8 byte CSN and 2 byte CRC
    if (Get14443bAnswerFromTag(resp, sizeof(resp), s_iso14b_timeout, &eof_time, &retlen) != PM3_SUCCESS) {
        return PM3_ECARDEXCHANGE;
    }
    if (retlen != 10) {
        return PM3_ELENGTH;
    }

    // save CSN
    memcpy(hdr->csn, resp, sizeof(hdr->csn));

    // card selected, now read config (block1) (only 8 bytes no CRC)
    start_time = eof_time + ISO14B_TR2;
    CodeAndTransmit14443bAsReader(read_conf, sizeof(read_conf), &start_time, &eof_time, true);
    eof_time += DELAY_ISO14443B_PCD_TO_PICC_READER;

    // expect a 10-byte response here, 8 bytes CONFIGURATION and 2 byte CRC)
    if (Get14443bAnswerFromTag(resp, sizeof(resp), s_iso14b_timeout, &eof_time, &retlen) != PM3_SUCCESS) {
        return PM3_ECARDEXCHANGE;
    }
    if (retlen != 10) {
        return PM3_ELENGTH;
    }

    // save CONF
    memcpy((uint8_t *)&hdr->conf, resp, sizeof(hdr->conf));

    uint8_t pagemap = get_pagemap(hdr);
    if (pagemap != PICOPASS_NON_SECURE_PAGEMODE) {

        // read App Issuer Area block 5
        start_time = eof_time + ISO14B_TR2;
        CodeAndTransmit14443bAsReader(read_aia, sizeof(read_aia), &start_time, &eof_time, true);
        eof_time += DELAY_ISO14443B_PCD_TO_PICC_READER;

        // expect AIA, a 10-byte response here
        if (Get14443bAnswerFromTag(resp, sizeof(resp), s_iso14b_timeout, &eof_time, &retlen) != PM3_SUCCESS) {
            return PM3_ECARDEXCHANGE;
        }
        if (retlen != 10) {
            return PM3_ELENGTH;
        }

        memcpy(hdr->app_issuer_area, resp, sizeof(hdr->app_issuer_area));

        // card selected, now read e-purse (cc) (block2) (only 8 bytes no CRC)
        start_time = eof_time + ISO14B_TR2;
        CodeAndTransmit14443bAsReader(read_check_cc, sizeof(read_check_cc), &start_time, &eof_time, true);
        eof_time += DELAY_ISO14443B_PCD_TO_PICC_READER;

        // expect EPURSE, a 8 byte response here
        if (Get14443bAnswerFromTag(resp, sizeof(resp), s_iso14b_timeout, &eof_time, &retlen) != PM3_SUCCESS) {
            return PM3_ECARDEXCHANGE;
        }
        if (retlen != 8) {
            return PM3_ELENGTH;
        }

        memcpy(hdr->epurse, resp, sizeof(hdr->epurse));

    }  else {

        // on NON_SECURE_PAGEMODE cards, AIA is on block2..

        // read App Issuer Area block 2
        read_aia[1] = 0x02;
        read_aia[2] = 0x61;
        read_aia[3] = 0x10;

        start_time = eof_time + ISO14B_TR2;
        CodeAndTransmit14443bAsReader(read_aia, sizeof(read_aia), &start_time, &eof_time, true);
        eof_time += DELAY_ISO14443B_PCD_TO_PICC_READER;

        // expect AIA, a 10-byte response here
        if (Get14443bAnswerFromTag(resp, sizeof(resp), s_iso14b_timeout, &eof_time, &retlen) != PM3_SUCCESS) {
            return PM3_ECARDEXCHANGE;
        }
        if (retlen != 10) {
            return PM3_ELENGTH;
        }

        memcpy(hdr->epurse, resp, sizeof(hdr->epurse));
    }
    return PM3_SUCCESS;
}

// Set up ISO 14443 Type B communication (similar to iso14443a_setup)
// field is setup for "Sending as Reader"
void iso14443b_setup(void) {

    switch_off(); // disconnect raw
    SpinDelay(20);

    FpgaDownloadAndGo(FPGA_BITSTREAM_HF);

    // allocate command receive buffer
    BigBuf_free();

    // Initialize Demod and Uart structs
    Demod14bInit(BigBuf_calloc(MAX_FRAME_SIZE), MAX_FRAME_SIZE);
    Uart14bInit(BigBuf_calloc(MAX_FRAME_SIZE));

    // connect Demodulated Signal to ADC:
    SetAdcMuxFor(GPIO_MUXSEL_HIPKD);

    // Set up the synchronous serial port
    FpgaSetupSsc(FPGA_MAJOR_MODE_HF_READER);

    // Signal field is on with the appropriate LED
#ifdef RDV4
    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_READER | FPGA_HF_READER_MODE_SEND_SHALLOW_MOD_RDV4);
#else
    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_READER | FPGA_HF_READER_MODE_SEND_SHALLOW_MOD);
#endif

    SpinDelay(100);

    // Start the timer
    StartCountSspClk();

    // reset timeout
    iso14b_set_fwt(8);

    s_field_on = true;

    LED_D_ON();
}

//-----------------------------------------------------------------------------
// Read a SRI512 ISO 14443B tag.
//
// SRI512 tags are just simple memory tags, here we're looking at making a dump
// of the contents of the memory. No anticollision algorithm is done, we assume
// we have a single tag in the field.
//
// I tried to be systematic and check every answer of the tag, every CRC, etc...
//-----------------------------------------------------------------------------
int read_14b_srx_block(uint8_t blocknr, uint8_t *block) {

    uint8_t cmd[] = {ISO14443B_READ_BLK, blocknr, 0x00, 0x00};
    AddCrc14B(cmd, 2);

    uint8_t r_block[6] = {0};

    uint32_t start_time = 0;
    uint32_t eof_time = 0;
    CodeAndTransmit14443bAsReader(cmd, sizeof(cmd), &start_time, &eof_time, true);

    eof_time += DELAY_ISO14443B_PCD_TO_PICC_READER;
    uint16_t retlen = 0;
    if (Get14443bAnswerFromTag(r_block, sizeof(r_block), s_iso14b_timeout, &eof_time, &retlen) != PM3_SUCCESS) {
        return PM3_ECARDEXCHANGE;
    }

    // Check if we got an answer from the tag
    if (retlen != 6) {
        return PM3_EWRONGANSWER;
    }
    // The check the CRC of the answer
    if (check_crc(CRC_14443_B, r_block, retlen) == false) {
        return PM3_ECRC;
    }

    if (block) {
        memcpy(block, r_block, ISO14B_BLOCK_SIZE);
    }

    return PM3_SUCCESS;
}

void read_14b_st_block(uint8_t blocknr) {
    iso14443b_setup();

    set_tracing(true);

    uint8_t *data = BigBuf_calloc(ISO14B_BLOCK_SIZE);
    iso14b_card_select_t *card = (iso14b_card_select_t *) BigBuf_calloc(sizeof(iso14b_card_select_t));

    int res = iso14443b_select_srx_card(card);
    if (res != PM3_SUCCESS) {
        reply_ng(CMD_HF_SRI_READ, res, NULL, 0);
        goto out;
    }

    res = read_14b_srx_block(blocknr, data);
    reply_ng(CMD_HF_SRI_READ, res, data, ISO14B_BLOCK_SIZE);

out:
    set_tracing(false);
    BigBuf_free_keep_EM();
    switch_off();
}

//=============================================================================
// Finally, the `sniffer' combines elements from both the reader and
// simulated tag, to show both sides of the conversation.
//=============================================================================

//-----------------------------------------------------------------------------
// Record the sequence of commands sent by the reader to the tag, with
// triggering so that we start recording at the point that the tag is moved
// near the reader.
//-----------------------------------------------------------------------------
/*
 * Memory usage for this function, (within BigBuf)
 * Last Received command (reader->tag) - MAX_FRAME_SIZE
 * Last Received command (tag->reader) - MAX_FRAME_SIZE
 * DMA Buffer - ISO14443B_DMA_BUFFER_SIZE
 * Demodulated samples received - all the rest
 */
void SniffIso14443b(void) {

    LEDsoff();
    LED_A_ON();

    FpgaDownloadAndGo(FPGA_BITSTREAM_HF);

    if (g_dbglevel >= DBG_INFO) {
        DbpString("Press " _GREEN_("pm3 button") " to abort sniffing");
    }

    BigBuf_free();
    clear_trace();
    set_tracing(true);

    // Initialize Demod and Uart structs
    uint8_t dm_buf[MAX_FRAME_SIZE] = {0};
    Demod14bInit(dm_buf, sizeof(dm_buf));

    uint8_t ua_buf[MAX_FRAME_SIZE] = {0};
    Uart14bInit(ua_buf);

    //Demod14bInit(BigBuf_malloc(MAX_FRAME_SIZE), MAX_FRAME_SIZE);
    //Uart14bInit(BigBuf_malloc(MAX_FRAME_SIZE));

    // Set FPGA in the appropriate mode
    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_READER | FPGA_HF_READER_SUBCARRIER_848_KHZ | FPGA_HF_READER_MODE_SNIFF_IQ);
//    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_READER | FPGA_HF_READER_SUBCARRIER_848_KHZ | FPGA_HF_READER_MODE_SNIFF_AMPLITUDE);

    // connect Demodulated Signal to ADC:
    SetAdcMuxFor(GPIO_MUXSEL_HIPKD);
    FpgaSetupSsc(FPGA_MAJOR_MODE_HF_READER);

    StartCountSspClk();

    // The DMA buffer, used to stream samples from the FPGA
    dmabuf16_t *dma = get_dma16();

    // Setup and start DMA.
    if (!FpgaSetupSscDma((uint8_t *) dma->buf, DMA_BUFFER_SIZE)) {
        if (g_dbglevel > DBG_ERROR) DbpString("FpgaSetupSscDma failed. Exiting");
        switch_off();
        return;
    }

    // We won't start recording the frames that we acquire until we trigger;
    // a good trigger condition to get started is probably when we see a
    // response from the tag.
    bool tag_is_active = false;
    bool reader_is_active = false;
    bool expect_tag_answer = false;
    int dma_start_time = 0;

    // Count of samples received so far, so that we can include timing
    int samples = 0;

    uint16_t *upTo = dma->buf;

    for (;;) {

        volatile int behind_by = ((uint16_t *)AT91C_BASE_PDC_SSC->PDC_RPR - upTo) & (DMA_BUFFER_SIZE - 1);
        if (behind_by < 1) continue;

        samples++;
        if (samples == 1) {
            // DMA has transferred the very first data
            dma_start_time = GetCountSspClk() & 0xfffffff0;
        }

        volatile int8_t ci = *upTo >> 8;
        volatile int8_t cq = *upTo;
        upTo++;

        // we have read all of the DMA buffer content.
        if (upTo >= dma->buf + DMA_BUFFER_SIZE) {

            // start reading the circular buffer from the beginning again
            upTo = dma->buf;

            // DMA Counter Register had reached 0, already rotated.
            if (AT91C_BASE_SSC->SSC_SR & (AT91C_SSC_ENDRX)) {

                // primary buffer was stopped
                if (AT91C_BASE_PDC_SSC->PDC_RCR == 0) {
                    AT91C_BASE_PDC_SSC->PDC_RPR = (uint32_t) dma->buf;
                    AT91C_BASE_PDC_SSC->PDC_RCR = DMA_BUFFER_SIZE;
                }
                // secondary buffer sets as primary, secondary buffer was stopped
                if (AT91C_BASE_PDC_SSC->PDC_RNCR == 0) {
                    AT91C_BASE_PDC_SSC->PDC_RNPR = (uint32_t) dma->buf;
                    AT91C_BASE_PDC_SSC->PDC_RNCR = DMA_BUFFER_SIZE;
                }

                WDT_HIT();
                if (BUTTON_PRESS()) {
                    DbpString("Sniff stopped");
                    break;
                }
            }
        }

        // no need to try decoding reader data if the tag is sending
        if (tag_is_active == false) {

            if (Handle14443bSampleFromReader(ci & 0x01)) {
                uint32_t eof_time = dma_start_time + (samples * 16) + 8; // - DELAY_READER_TO_ARM_SNIFF; // end of EOF
                if (Uart.byteCnt > 0) {
                    uint32_t sof_time = eof_time
                                        - Uart.byteCnt * 1 // time for byte transfers
                                        - 32 * 16          // time for SOF transfer
                                        - 16 * 16;         // time for EOF transfer
                    LogTrace(Uart.output, Uart.byteCnt, (sof_time * 4), (eof_time * 4), NULL, true);
                }
                // And ready to receive another command.
                Uart14bReset();
                Demod14bReset();
                expect_tag_answer = true;
            }

            if (Handle14443bSampleFromReader(cq & 0x01)) {

                uint32_t eof_time = dma_start_time + (samples * 16) + 16; // - DELAY_READER_TO_ARM_SNIFF; // end of EOF
                if (Uart.byteCnt > 0) {
                    uint32_t sof_time = eof_time
                                        - Uart.byteCnt * 1 // time for byte transfers
                                        - 32 * 16          // time for SOF transfer
                                        - 16 * 16;         // time for EOF transfer
                    LogTrace(Uart.output, Uart.byteCnt, (sof_time * 4), (eof_time * 4), NULL, true);
                }
                // And ready to receive another command
                Uart14bReset();
                Demod14bReset();
                expect_tag_answer = true;
            }

            reader_is_active = (Uart.state > STATE_14B_GOT_FALLING_EDGE_OF_SOF);
        }

        // no need to try decoding tag data if the reader is sending - and we cannot afford the time
        if (reader_is_active == false && expect_tag_answer) {

            if (Handle14443bSamplesFromTag((ci >> 1), (cq >> 1))) {

                uint32_t eof_time = dma_start_time + (samples * 16); // - DELAY_TAG_TO_ARM_SNIFF; // end of EOF
                uint32_t sof_time = eof_time
                                    - Demod.len * 8 * 8 * 16 // time for byte transfers
                                    - (32 * 16)             // time for SOF transfer
                                    - 0;                    // time for EOF transfer

                LogTrace(Demod.output, Demod.len, (sof_time * 4), (eof_time * 4), NULL, false);
                // And ready to receive another response.
                Uart14bReset();
                Demod14bReset();
                expect_tag_answer = false;
                tag_is_active = false;
            } else {
                tag_is_active = (Demod.state > WAIT_FOR_RISING_EDGE_OF_SOF);
            }
        }
    }

    FpgaDisableTracing();
    switch_off();

    DbpString("");
    DbpString(_CYAN_("Sniff statistics"));
    DbpString("=================================");
    Dbprintf("  DecodeTag State........%d", Demod.state);
    Dbprintf("  DecodeTag byteCnt......%d", Demod.len);
    Dbprintf("  DecodeTag posCount.....%d", Demod.posCount);
    Dbprintf("  DecodeReader State.....%d", Uart.state);
    Dbprintf("  DecodeReader byteCnt...%d", Uart.byteCnt);
    Dbprintf("  DecodeReader posCount..%d", Uart.posCnt);
    Dbprintf("  Trace length..........." _YELLOW_("%d"), BigBuf_get_traceLen());
    DbpString("");
}

static void iso14b_set_trigger(bool enable) {
    g_trigger = enable;
}

void SendRawCommand14443B(iso14b_raw_cmd_t *p) {

    // turn on trigger (LED_A)
    if ((p->flags & ISO14B_REQUEST_TRIGGER) == ISO14B_REQUEST_TRIGGER) {
        iso14b_set_trigger(true);
    }

    if ((p->flags & ISO14B_CONNECT) == ISO14B_CONNECT) {
        iso14443b_setup();
    }

    if ((p->flags & ISO14B_SET_TIMEOUT) == ISO14B_SET_TIMEOUT) {
        iso14b_set_timeout(p->timeout);
    }

    if ((p->flags & ISO14B_CLEARTRACE) == ISO14B_CLEARTRACE) {
        clear_trace();
    }

    set_tracing(true);

    // receive buffer
    uint8_t buf[PM3_CMD_DATA_SIZE] = {0x00};

    int status = 0;
    uint32_t sendlen = sizeof(iso14b_card_select_t);
    iso14b_card_select_t *card = (iso14b_card_select_t *)buf;

    if ((p->flags & ISO14B_SELECT_STD) == ISO14B_SELECT_STD) {
        status = iso14443b_select_card(card);
        reply_ng(CMD_HF_ISO14443B_COMMAND, status, (uint8_t *)card, sendlen);
        if (status != PM3_SUCCESS) goto out;
    }

    if ((p->flags & ISO14B_SELECT_SR) == ISO14B_SELECT_SR) {
        memset(card, 0, sizeof(iso14b_card_select_t));
        status = iso14443b_select_srx_card(card);
        reply_ng(CMD_HF_ISO14443B_COMMAND, status, (uint8_t *)card, sendlen);
        if (status != PM3_SUCCESS) goto out;
    }

    if ((p->flags & ISO14B_SELECT_XRX) == ISO14B_SELECT_XRX) {
        memset(card, 0, sizeof(iso14b_card_select_t));
        status = iso14443b_select_xrx_card(card);
        reply_ng(CMD_HF_ISO14443B_COMMAND, status, (uint8_t *)card, sendlen);
        // 0: OK, 1: select fail, 2: attrib fail, 3: crc fail, 4: password fail
        if (status != PM3_SUCCESS) goto out;
    }

    if ((p->flags & ISO14B_SELECT_CTS) == ISO14B_SELECT_CTS) {
        iso14b_cts_card_select_t *cts = (iso14b_cts_card_select_t *)buf;
        memset(cts, 0, sizeof(iso14b_cts_card_select_t));
        sendlen = sizeof(iso14b_cts_card_select_t);
        status = iso14443b_select_cts_card(cts);
        reply_ng(CMD_HF_ISO14443B_COMMAND, status, (uint8_t *)cts, sendlen);
        if (status != PM3_SUCCESS) goto out;
    }

    if ((p->flags & ISO14B_SELECT_PICOPASS) == ISO14B_SELECT_PICOPASS) {
        picopass_hdr_t *hdr = (picopass_hdr_t *)buf;
        memset(hdr, 0, sizeof(picopass_hdr_t));
        sendlen = sizeof(picopass_hdr_t);
        status = iso14443b_select_picopass_card(hdr);
        reply_ng(CMD_HF_ISO14443B_COMMAND, status, (uint8_t *)hdr, sendlen);
        if (status != PM3_SUCCESS) goto out;
    }

    // if field is off...
    if (
        ((p->flags & ISO14B_APDU) == ISO14B_APDU) ||
        ((p->flags & ISO14B_RAW) == ISO14B_RAW)
    ) {
        if (s_field_on == false) {
            DbpString("Field is off");
            reply_ng(CMD_HF_ISO14443B_COMMAND, PM3_ERFTRANS, NULL, 0);
            goto out;
        }
    }

    if ((p->flags & ISO14B_APDU) == ISO14B_APDU) {

        uint16_t responselen = 0;
        uint8_t response_byte = 0;
        bool chaining = ((p->flags & ISO14B_SEND_CHAINING) == ISO14B_SEND_CHAINING);
        status = iso14443b_apdu(p->raw, p->rawlen, chaining, buf, sizeof(buf), &response_byte, &responselen);

        if (tearoff_hook() == PM3_ETEAROFF) { // tearoff occurred
            reply_ng(CMD_HF_ISO14443B_COMMAND, PM3_ETEAROFF, NULL, 0);
        } else {
            uint8_t packet[responselen + 1 + 2];
            iso14b_raw_apdu_response_t *payload = (iso14b_raw_apdu_response_t *)packet;
            payload->response_byte = response_byte;
            payload->datalen = responselen;
            memcpy(payload->data, buf, payload->datalen);
            reply_ng(CMD_HF_ISO14443B_COMMAND, status, packet, sizeof(packet));
        }
    }

    if ((p->flags & ISO14B_RAW) == ISO14B_RAW) {

        if (((p->flags & ISO14B_APPEND_CRC) == ISO14B_APPEND_CRC) && (p->rawlen)) {

            // Picopass uses different CRC algo
            // it also excludes the first instruction byte
            if ((p->flags & ISO14B_SELECT_PICOPASS) == ISO14B_SELECT_PICOPASS) {
                AddCrc15(p->raw + 1, p->rawlen - 1);
            } else {
                AddCrc14B(p->raw, p->rawlen);
            }
            p->rawlen += 2;
        }

        uint32_t start_time = 0;
        uint32_t eof_time = 0;
        CodeAndTransmit14443bAsReader(p->raw, p->rawlen, &start_time, &eof_time, true);

        if (tearoff_hook() == PM3_ETEAROFF) { // tearoff occurred
            reply_ng(CMD_HF_ISO14443B_COMMAND, PM3_ETEAROFF, NULL, 0);
        } else {

            eof_time += DELAY_ISO14443B_PCD_TO_PICC_READER;

            uint16_t retlen = 0;
            status = Get14443bAnswerFromTag(buf, sizeof(buf), s_iso14b_timeout, &eof_time, &retlen);
            if (status == PM3_SUCCESS) {
                sendlen = MIN(retlen, PM3_CMD_DATA_SIZE);
                reply_ng(CMD_HF_ISO14443B_COMMAND, status, Demod.output, sendlen);
            } else {
                reply_ng(CMD_HF_ISO14443B_COMMAND, status, NULL, 0);
            }
        }
    }

out:
    // turn off trigger (LED_A)
    if ((p->flags & ISO14B_REQUEST_TRIGGER) == ISO14B_REQUEST_TRIGGER) {
        iso14b_set_trigger(false);
    }

    // turn off antenna et al
    // we don't send a HALT command.
    if ((p->flags & ISO14B_DISCONNECT) == ISO14B_DISCONNECT) {
        switch_off(); // disconnect raw
        SpinDelay(20);
        BigBuf_free_keep_EM();
        s_field_on = false;
    }
}
