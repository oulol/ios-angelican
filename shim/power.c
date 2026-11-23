#include "power.h"
#include "watchdog.h"
#include "console.h"

#include <stdint.h>

/* MediaTek PWRAP */
#define PWRAP_BASE              0x1000D000UL

#define PWRAP_WACS2_EN          0x09C
#define PWRAP_INIT_DONE2        0x0A0

#define PWRAP_WACS2_CMD         0xC20
#define PWRAP_WACS2_RDATA       0xC24
#define PWRAP_WACS2_VLDCLR      0xC28

/* WACS2 FSM states */
#define WACS_FSM_IDLE           0
#define WACS_FSM_WFVLDCLR       6

#define WACS_FSM_MASK           0x7
#define WACS_FSM_SHIFT          16

#define PWRAP_TIMEOUT           1000000U

/* MT6357 PMIC */
#define MT6357_TOPSTATUS        0x0024
#define PMIC_POWER_KEY_MASK     0x0002

/* MediaTek TOPRGU */
#define WDT_BASE                0x10007000UL
#define WDT_SWRST_OFFSET        0x014
#define WDT_SWRST_KEY           0x1209

static inline volatile uint32_t *pwrap_register(uintptr_t offset)
{
    return (volatile uint32_t *)(PWRAP_BASE + offset);
}

static inline volatile uint32_t *wdt_register(uintptr_t offset)
{
    return (volatile uint32_t *)(WDT_BASE + offset);
}

static int pwrap_wait_for_state(uint32_t expected_state)
{
    for (uint32_t i = 0; i < PWRAP_TIMEOUT; ++i) {
        uint32_t value = *pwrap_register(PWRAP_WACS2_RDATA);

        uint32_t state =
            (value >> WACS_FSM_SHIFT) & WACS_FSM_MASK;

        if (state == expected_state) {
            return POWER_SUCCESS;
        }

        watchdog_service();
    }

    return POWER_ERROR_POLL;
}

static int pwrap_read(uint16_t address, uint16_t *result)
{
    if (result == 0) {
        return POWER_ERROR_PMIC;
    }

    if (pwrap_wait_for_state(WACS_FSM_IDLE) != POWER_SUCCESS) {
        return POWER_ERROR_POLL;
    }

    /*
     * Linux MT6765 PWRAP WACS2 read command format:
     * address is expressed in 16-bit register units.
     */
    *pwrap_register(PWRAP_WACS2_CMD) =
        ((uint32_t)(address >> 1) << 16);

    if (pwrap_wait_for_state(WACS_FSM_WFVLDCLR) != POWER_SUCCESS) {
        return POWER_ERROR_POLL;
    }

    uint32_t value = *pwrap_register(PWRAP_WACS2_RDATA);

    *result = (uint16_t)(value & 0xFFFF);

    *pwrap_register(PWRAP_WACS2_VLDCLR) = 1;

    return POWER_SUCCESS;
}


#define KPD_BASE             0x10010000UL
#define KPD_KP_MEM1          0x004

#define VOLUME_UP_MASK       0x0201
#define VOLUME_DOWN_MASK     0x0402

static inline uint32_t kpd_read(void)
{
    return *(volatile uint32_t *)(KPD_BASE + KPD_KP_MEM1);
}

int volume_up_button_pressed(void)
{
    return (kpd_read() & VOLUME_UP_MASK) != VOLUME_UP_MASK;
}

int volume_down_button_pressed(void)
{
    return (kpd_read() & VOLUME_DOWN_MASK) != VOLUME_DOWN_MASK;
}


int power_button_pressed(void)
{
    uint16_t status = 0;

    if (pwrap_read(MT6357_TOPSTATUS, &status) != POWER_SUCCESS) {
        return POWER_ERROR_PMIC;
    }

    /*
     * MT6357 TOPSTATUS bit 1 is active-low:
     *   0 = pressed
     *   1 = released
     */
    return (status & PMIC_POWER_KEY_MASK) == 0;
}

#define WDT_BASE       0x10007000UL
#define WDT_SWRST      (*(volatile uint32_t *)(WDT_BASE + 0x14))

void power_reset(void)
{
    volatile uint32_t *wdt_mode =
        (volatile uint32_t *)0x10007000UL;

    volatile uint32_t *wdt_swrst =
        (volatile uint32_t *)0x10007014UL;

    /* Disable watchdog IRQ mode. */
    uint32_t mode = *wdt_mode;
    mode &= ~(1U << 3);

    /* MediaTek watchdog mode unlock key. */
    *wdt_mode = mode | 0x22000000U;

    __asm__ volatile("dsb sy" ::: "memory");

    /* Repeatedly request software reset, like Linux does. */
    uint64_t frequency = watchdog_read_frequency();

    if (frequency == 0)
        frequency = 13000000ULL;

    const uint64_t delay_ticks = frequency / 200ULL;

    for (;;) {
        *wdt_swrst = 0x1209;

        __asm__ volatile("dsb sy" ::: "memory");

        uint64_t start = watchdog_read_counter();

        while ((watchdog_read_counter() - start) < delay_ticks) {
            __asm__ volatile("yield");
        }
    }
}

void power_poll(void)
{
    static int error_reported = 0;

    int result = power_button_pressed();

    if (result < 0) {
        if (!error_reported) {
            con_str("POWER", "PWRAP ERROR");
            error_reported = 1;
        }
        return;
    }

    error_reported = 0;

    if (result == 1) {
        con_str("POWER", "PRESSED");
        power_reset();
    }
}