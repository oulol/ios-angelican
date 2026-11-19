
#include "watchdog.h"

#include <stdint.h>

/*
 * MediaTek MT6765 TOPRGU watchdog.
 *
 * Physical base: 0x10007000
 * WDT_RST:       0x10007008
 * Refresh key:   0x1971
 */

#define WDT_BASE                ((uintptr_t)0x10007000)
#define WDT_RST_OFFSET          0x08
#define WDT_REFRESH_KEY         0x1971

#define WDT_REFRESH_INTERVAL_MS 100ULL
#define DEFAULT_TIMER_FREQ      13000000ULL

static uint64_t last_refresh_tick;
static uint64_t refresh_interval_ticks;

static inline volatile uint32_t *wdt_register(uintptr_t offset)
{
    return (volatile uint32_t *)(WDT_BASE + offset);
}

uint64_t watchdog_read_counter(void)
{
    uint64_t value;

    __asm__ volatile(
        "mrs %0, cntpct_el0"
        : "=r"(value)
    );

    return value;
}

uint64_t watchdog_read_frequency(void)
{
    uint64_t value;

    __asm__ volatile(
        "mrs %0, cntfrq_el0"
        : "=r"(value)
    );

    return value;
}

static void watchdog_refresh(void)
{
    /*
     * Ensure previous memory operations complete before
     * accessing the watchdog register.
     */
    __asm__ volatile("dsb sy" ::: "memory");

    *wdt_register(WDT_RST_OFFSET) = WDT_REFRESH_KEY;

    /*
     * Ensure the MMIO write reaches the watchdog.
     */
    __asm__ volatile("dsb sy" ::: "memory");
}

void watchdog_init(void)
{
    uint64_t frequency = watchdog_read_frequency();

    if (frequency == 0) {
        frequency = DEFAULT_TIMER_FREQ;
    }

    /*
     * Convert milliseconds into architectural timer ticks.
     */
    refresh_interval_ticks =
        (frequency / 1000ULL) * WDT_REFRESH_INTERVAL_MS;

    if (refresh_interval_ticks == 0) {
        refresh_interval_ticks = 1;
    }

    /*
     * Refresh immediately instead of waiting for the first interval.
     */
    watchdog_refresh();

    last_refresh_tick = watchdog_read_counter();
}

void watchdog_service(void)
{
    *wdt_register(WDT_RST_OFFSET) = WDT_REFRESH_KEY;
    __asm__ volatile("dsb sy" ::: "memory");
}