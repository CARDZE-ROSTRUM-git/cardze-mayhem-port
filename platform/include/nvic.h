/*
 * Minimal host stand-in for the LPC43xx CMSIS NVIC/SCB subset referenced by
 * the UI/event layer.
 */
#ifndef __NVIC_H__
#define __NVIC_H__

#include <cstdint>

/* IRQn values used by the code that is compiled on the host. */
enum IRQn {    Reset_IRQn = -15,
    NonMaskableInt_IRQn = -14,
    HardFault_IRQn = -13,
    SVCall_IRQn = -5,
    PendSV_IRQn = -2,
    SysTick_IRQn = -1,
    RTC_IRQn = 0,
    M4CORE_IRQn = 1,
    DMA_IRQn = 2,
    PIN_INT4_IRQn = 14,
    I2C0_OR_I2C1_IRQn = 18,
    EVENTROUTER_IRQn = 23,
};

typedef int IRQn_Type;

struct HostNVIC {
    volatile uint32_t ISER[8]{};
    volatile uint32_t ICER[8]{};
    volatile uint32_t ISPR[8]{};
    volatile uint32_t ICPR[8]{};
    volatile uint32_t IP[64]{};
};

struct HostSCB {
    volatile uint32_t ICSR{};
    volatile uint32_t SCR{};
};

extern HostNVIC host_nvic;
extern HostSCB host_scb;

#define NVIC (&host_nvic)
#define SCB (&host_scb)

#define SCB_ICSR_PENDSTCLR_Msk (1UL << 25)
#define SCB_SCR_SLEEPDEEP_Msk (1UL << 2)

#define NVIC_EnableIRQ(irq) ((void)(irq))
#define NVIC_DisableIRQ(irq) ((void)(irq))
#define NVIC_SetPendingIRQ(irq) ((void)(irq))
#define NVIC_ClearPendingIRQ(irq) ((void)(irq))

static inline void __disable_irq() {}
static inline void __enable_irq() {}
static inline void __DSB() {}
static inline void __DMB() {}
static inline void __ISB() {}
static inline void __WFI() {}
static inline void __WFE() {}

static inline void nvicEnableVector(int irq, uint32_t prio) {
    (void)irq;
    (void)prio;
}
static inline void nvicDisableVector(int irq) {
    (void)irq;
}

#ifndef CORTEX_PRIORITY_MASK
#define CORTEX_PRIORITY_MASK(prio) (prio)
#endif

#ifndef LPC43XX_M4TXEVENT_IRQ_PRIORITY
#define LPC43XX_M4TXEVENT_IRQ_PRIORITY 0
#endif
#ifndef LPC43XX_M0APPTXEVENT_IRQ_PRIORITY
#define LPC43XX_M0APPTXEVENT_IRQ_PRIORITY 0
#endif
#ifndef LPC43XX_PIN_INT4_IRQ_PRIORITY
#define LPC43XX_PIN_INT4_IRQ_PRIORITY 0
#endif
#ifndef LPC43XX_RTC_IRQ_PRIORITY
#define LPC43XX_RTC_IRQ_PRIORITY 0
#endif
#ifndef LPC_DMA_IRQ_PRIORITY
#define LPC_DMA_IRQ_PRIORITY 0
#endif

#endif /* __NVIC_H__ */
