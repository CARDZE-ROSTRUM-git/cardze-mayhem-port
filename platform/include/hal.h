/*
 * Host replacement for the ChibiOS HAL surface touched by headers and the
 * UI/event code. No real hardware is accessed.
 */
#ifndef __HAL_H__
#define __HAL_H__

#include "ch.h"
#include "nvic.h"

#include <cstdint>
#include <cstddef>

typedef uint32_t ioportid_t;
typedef uint32_t iopadid_t;
typedef uint32_t ioportmask_t;
typedef uint32_t halrtcnt_t;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#define PAL_MODE_RESET 0U
#define PAL_MODE_INPUT 1U
#define PAL_MODE_OUTPUT_PUSHPULL 2U
#define PAL_MODE_OUTPUT_OPENDRAIN 3U
#define PAL_MODE_ALTERNATE(n) (0x10U + (n))
#define PAL_MODE_INPUT_ANALOG 8U
#define PAL_MODE_INPUT_PULLUP 9U
#define PAL_MODE_INPUT_PULLDOWN 10U

static inline void halInit() {}
static inline void halPolledDelay(uint32_t usec) {
    (void)usec;
}
static inline void palSetPad(ioportid_t port, iopadid_t pad) {
    (void)port;
    (void)pad;
}
static inline void palClearPad(ioportid_t port, iopadid_t pad) {
    (void)port;
    (void)pad;
}
static inline void palTogglePad(ioportid_t port, iopadid_t pad) {
    (void)port;
    (void)pad;
}
static inline void palWritePad(ioportid_t port, iopadid_t pad, uint32_t value) {
    (void)port;
    (void)pad;
    (void)value;
}
static inline uint32_t palReadPad(ioportid_t port, iopadid_t pad) {
    (void)port;
    (void)pad;
    return 0;
}
static inline uint32_t palReadLatch(ioportid_t port) {
    (void)port;
    return 0;
}
static inline void palSetPadMode(ioportid_t port, iopadid_t pad, uint32_t mode) {
    (void)port;
    (void)pad;
    (void)mode;
}
static inline void palSetPortMode(ioportid_t port, uint32_t mode) {
    (void)port;
    (void)mode;
}

/* SPI/I2C driver types: the desktop port has no real bus drivers, but the
 * upstream class headers keep these pointers and build SPIConfig/I2CConfig
 * literals, so the field names must match the ChibiOS ones. */
#define CR0_DSSMASK 0x0F
#define CR0_DSS4BIT 3
#define CR0_DSS5BIT 4
#define CR0_DSS6BIT 5
#define CR0_DSS7BIT 6
#define CR0_DSS8BIT 7
#define CR0_DSS9BIT 8
#define CR0_DSS10BIT 9
#define CR0_DSS11BIT 0xA
#define CR0_DSS12BIT 0xB
#define CR0_DSS13BIT 0xC
#define CR0_DSS14BIT 0xD
#define CR0_DSS15BIT 0xE
#define CR0_DSS16BIT 0xF
#define CR0_FRFSPI 0
#define CR0_FRFSSI 0x10
#define CR0_FRFMW 0x20
#define CR0_CPOL 0x40
#define CR0_CPHA 0x80
#define CR0_CLOCKRATE(n) ((n) << 8)
#define CR1_LBM 1
#define CR1_SSE 2
#define CR1_MS 4
#define CR1_SOD 8

typedef uint32_t spistate_t;
typedef uint32_t i2cstate_t;
typedef uint32_t i2cflags_t;
typedef uint8_t ssport_t;
typedef uint8_t sspad_t;

struct SPIDriver;

struct SPIConfig {
    void (*end_cb)(SPIDriver*);
    sspad_t ssport;
    ssport_t sspad;
    uint32_t cr0;
    uint32_t cpsr;
};

struct I2CConfig {
    uint16_t high_count{0};
    uint16_t low_count{0};
};

struct SPIDriver {
    spistate_t state{0};
    const SPIConfig* config{nullptr};
};

struct I2CDriver {
    i2cstate_t state{0};
    const I2CConfig* config{nullptr};
    i2cflags_t errors{0};
};

using i2caddr_t = uint8_t;

#define I2CD_NO_ERROR ((i2cflags_t)0U)
#define I2C_ACTIVE_TX ((i2cstate_t)1U)
#define I2C_ACTIVE_RX ((i2cstate_t)2U)
#define I2C_LOCKED ((i2cstate_t)4U)
#define I2C_READY ((i2cstate_t)0U)
#define RDY_TIMEOUT ((msg_t)0)
#define RDY_OK ((msg_t)0)

static inline void spiStart(SPIDriver*, const SPIConfig*) {}
static inline void spiStop(SPIDriver*) {}
static inline void spiAcquireBus(SPIDriver*) {}
static inline void spiReleaseBus(SPIDriver*) {}
static inline void spiSelect(SPIDriver*) {}
static inline void spiUnselect(SPIDriver*) {}
static inline void spiExchange(SPIDriver*, size_t, const void*, void*) {}
static inline void spiSend(SPIDriver*, size_t, const void*) {}
static inline void spiReceive(SPIDriver*, size_t, void*) {}

static inline void i2cStart(I2CDriver*, const I2CConfig*) {}
static inline void i2cStop(I2CDriver*) {}
static inline void i2cAcquireBus(I2CDriver*) {}
static inline void i2cReleaseBus(I2CDriver*) {}
static inline msg_t i2cMasterTransmitTimeout(I2CDriver*, i2caddr_t, const uint8_t*, size_t, uint8_t*, size_t, systime_t) {
    return RDY_OK;
}
static inline msg_t i2cMasterReceiveTimeout(I2CDriver*, i2caddr_t, uint8_t*, size_t, systime_t) {
    return RDY_OK;
}
/* Low-level LLD entry point used directly by common/i2c_pp.cpp (I2C::probe).
 * It has the same transaction semantics as i2cMasterTransmitTimeout. */
static inline msg_t i2c_lld_master_transmit_timeout(I2CDriver*, i2caddr_t, const uint8_t*, size_t, uint8_t*, size_t, systime_t) {
    return RDY_OK;
}

/* SDC driver: the SD card is replaced by a host directory in stage 3. */
struct SDCDriver {
    int dummy{0};
};
extern SDCDriver SDCD1;

#define CH_SUCCESS 0
void sdcStart(SDCDriver* sdcp, const void* config);
void sdcStop(SDCDriver* sdcp);
msg_t sdcConnect(SDCDriver* sdcp);
void sdcDisconnect(SDCDriver* sdcp);
bool sdcIsCardInserted(SDCDriver* sdcp);
void sdio_cclk_set(int divider);

/* ChibiOS queue types referenced by the USB serial driver header. */
#define _base_asynchronous_channel_methods void* vmt_dummy;
struct InputQueue {
    uint8_t* buffer{nullptr};
};
struct OutputQueue {
    uint8_t* buffer{nullptr};
};

/* ChibiOS output-queue write, referenced by the upstream USB serial header
 * (UsbSerialInputHandler::write, used by the KISS TNC app). The host has no USB
 * device controller, so the host implementation discards the bytes; it exists
 * only so the upstream header links. Returns the number accepted. */
size_t chOQWriteTimeout(OutputQueue* oqp, const uint8_t* bp, size_t n, systime_t time);

/* SD card mode bits and block device info used by the SD debug app. */
#define SDC_MODE_CARDTYPE_MASK 0x0FU
#define SDC_MODE_CARDTYPE_SDV11 0x00U
#define SDC_MODE_CARDTYPE_SDV20 0x01U
#define SDC_MODE_CARDTYPE_MMC 0x02U
#define SDC_MODE_HIGH_CAPACITY 0x10U

struct BlockDeviceInfo {
    uint32_t blk_size{512};
    uint32_t blk_num{0};
    uint32_t blk_state{0};
};

halrtcnt_t halGetCounterValue();
uint32_t halGetCounterFrequency();
int sdcGetInfo(SDCDriver* sdcp, BlockDeviceInfo* bdip);

/* LPC43xx register definitions. They are real structures, but the LPC_*
 * pointers below are redirected to host-side dummy storage by host_hal.cpp so
 * that unrelated register pokes cannot crash the process. */
extern "C" {
#define __IO volatile
#define __I volatile const
#define __O volatile
}
#include "lpc43xx.inc"
#include "lpc43xx.h"

/* Redirect the register blocks used by compiled code to plain host memory.
 * The values written there are irrelevant, but nothing may page-fault. */
namespace host_regs {
extern LPC_CREG_Type creg;
extern LPC_RTC_Type rtc;
extern LPC_CGU_Type cgu;
extern LPC_CCU1_Type ccu1;
extern LPC_CCU2_Type ccu2;
extern LPC_RGU_Type rgu;
extern LPC_SCU_Type scu;
extern LPC_GPIO_Type gpio;
extern LPC_GPIO_INT_Type gpio_int;
extern LPC_ADCx_Type adc0;
extern LPC_ADCx_Type adc1;
extern LPC_SSPx_Type ssp1;
extern LPC_SGPIO_Type sgpio;
extern LPC_EVENTROUTER_Type eventrouter;

/* Re-points the file-backed persistent-memory (backup RAM) region at the
 * folder named by `sd_dir`, for the launcher's SD-folder choice. Must be called
 * before anything reads the region. No-op if the region is already mapped to
 * the same folder. Defined in host_regs.cpp. */
void remap_backup_ram(const char* sd_dir);
}  // namespace host_regs

#undef LPC_CREG
#undef LPC_RTC
#undef LPC_CGU
#undef LPC_CCU1
#undef LPC_CCU2
#undef LPC_RGU
#undef LPC_SCU
#undef LPC_GPIO
#undef LPC_GPIO_INT
#undef LPC_ADC0
#undef LPC_ADC1
#undef LPC_SSP1
#undef LPC_SGPIO
#undef LPC_EVENTROUTER

#define LPC_CREG (&host_regs::creg)
#define LPC_RTC (&host_regs::rtc)
#define LPC_CGU (&host_regs::cgu)
#define LPC_CCU1 (&host_regs::ccu1)
#define LPC_CCU2 (&host_regs::ccu2)
#define LPC_RGU (&host_regs::rgu)
#define LPC_SCU (&host_regs::scu)
#define LPC_GPIO (&host_regs::gpio)
#define LPC_GPIO_INT (&host_regs::gpio_int)
#define LPC_ADC0 (&host_regs::adc0)
#define LPC_ADC1 (&host_regs::adc1)
#define LPC_SSP1 (&host_regs::ssp1)
#define LPC_SGPIO (&host_regs::sgpio)
#define LPC_EVENTROUTER (&host_regs::eventrouter)

/* Set true for the "r9" HackRF/PortaPack revisions (see main.cpp). */
extern bool hackrf_r9;

#endif /* __HAL_H__ */
