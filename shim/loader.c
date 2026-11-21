
#include "loader.h"

#include "boot_state.h"
#include "console.h"
#include "mmu.h"
#include "power.h"
#include "watchdog.h"
#include "bootinfo.h"
#include "loader_xnu.h"
#include "fdt.h"

#include <stdint.h>

/* -------------------------------------------------------------------------- */
/* Configuration                                                              */
/* -------------------------------------------------------------------------- */

#define EL2_TO_EL1_SPSR     0x3C5ULL
#define HCR_EL2_RW          (1ULL << 31)

#define EL1_STACK_SIZE      (16 * 1024)

/* -------------------------------------------------------------------------- */
/* Shared boot state                                                          */
/* -------------------------------------------------------------------------- */

volatile boot_state_t boot_state = {0};

/* -------------------------------------------------------------------------- */
/* EL1 stack                                                                  */
/* -------------------------------------------------------------------------- */

static uint8_t el1_stack[EL1_STACK_SIZE]
    __attribute__((aligned(16), section(".bss.el1stack")));

/* -------------------------------------------------------------------------- */
/* External symbols                                                           */
/* -------------------------------------------------------------------------- */

extern void el1_vectors(void);

/* -------------------------------------------------------------------------- */
/* Register access                                                            */
/* -------------------------------------------------------------------------- */

uint64_t loader_current_el(void)
{
    uint64_t value;

    __asm__ volatile(
        "mrs %0, CurrentEL"
        : "=r"(value)
    );

    return value;
}

uint64_t loader_read_daif(void)
{
    uint64_t value;

    __asm__ volatile(
        "mrs %0, DAIF"
        : "=r"(value)
    );

    return value;
}

static uint64_t loader_read_sctlr_el1(void)
{
    uint64_t value;

    __asm__ volatile(
        "mrs %0, SCTLR_EL1"
        : "=r"(value)
    );

    return value;
}

/* -------------------------------------------------------------------------- */
/* Boot diagnostics                                                           */
/* -------------------------------------------------------------------------- */

static void loader_print_boot_state(void)
{
    con_str("BOOT", "Boot state:");

    con_hex("x0", boot_state.x0);
    con_hex("x1", boot_state.x1);
    con_hex("x2", boot_state.x2);
    con_hex("x3", boot_state.x3);

    con_hex("Init SP", boot_state.initial_sp);
    con_hex("Cur EL", loader_current_el());
    con_hex("DAIF", loader_read_daif());

    con_skip();
}

/* -------------------------------------------------------------------------- */
/* EL2 -> EL1 transition                                                      */
/* -------------------------------------------------------------------------- */

static void loader_enter_el1(void) __attribute__((noreturn));

static void loader_enter_el1(void)
{
    uintptr_t stack_top =
        (uintptr_t)(el1_stack + sizeof(el1_stack));

    uintptr_t vector_base = (uintptr_t)el1_vectors;
    uintptr_t entry = (uintptr_t)minimal_el1_main;

    uint64_t hcr;

    __asm__ volatile(
        "mrs %0, HCR_EL2"
        : "=r"(hcr)
    );

    hcr |= HCR_EL2_RW;

    /*
     * Configure EL1:
     * - AArch64 execution state.
     * - EL1h exception level.
     * - Dedicated EL1 stack.
     * - Dedicated exception vectors.
     * - DAIF masked during early initialization.
     */
    __asm__ volatile(
        "msr HCR_EL2, %0\n"
        "msr SP_EL1, %1\n"
        "msr VBAR_EL1, %2\n"
        "msr ELR_EL2, %3\n"
        "msr SPSR_EL2, %4\n"
        "isb\n"
        "eret\n"
        :
        : "r"(hcr),
          "r"(stack_top),
          "r"(vector_base),
          "r"(entry),
          "r"(EL2_TO_EL1_SPSR)
        : "memory"
    );

    __builtin_unreachable();
}

/* -------------------------------------------------------------------------- */
/* Early entry                                                                */
/* -------------------------------------------------------------------------- */

void loader_main(void) __attribute__((noreturn));

void loader_main(void)
{
    uint64_t current_el = loader_current_el();

    boot_state.current_el = current_el;
    boot_state.daif = loader_read_daif();

    con_clear(BLUE);
    con_str("SHIM", "BOOTED SHIM FOR ANGELICA");

    loader_print_boot_state();

    switch (current_el) {
        case 0x8:
            con_str("BOOT", "Entered at EL2.");
            con_str("BOOT", "Preparing EL1...");

            loader_enter_el1();

        case 0x4:
            con_clear(BLACK);
            con_str("BOOT", "Already at EL1.");

            minimal_el1_main();

        default:
            con_clear(RED);
            con_str("ERROR", "Unsupported EL!");

            con_hex("Cur EL", current_el);

            for (;;) {
                __asm__ volatile("wfe");
            }
    }
}

/*
 * Compatibility entry point for start.S.
 */
void main(void) __attribute__((noreturn));

void main(void)
{
    loader_main();
}

/* -------------------------------------------------------------------------- */
/* EL1 main                                                                   */
/* -------------------------------------------------------------------------- */

void minimal_el1_main(void) __attribute__((noreturn));

void minimal_el1_main(void)
{
    con_clear(BLACK);
    con_str("SHIM", "EL1 ENTERED");

    boot_state.current_el = loader_current_el();
    boot_state.daif = loader_read_daif();

    con_hex("Cur EL", boot_state.current_el);
    con_hex("SCTLR", loader_read_sctlr_el1());
    con_hex("VBAR", (uintptr_t)el1_vectors);

    /* Watchdog */

    con_str("WDT", "Initializing watchdog...");

    watchdog_init();
    watchdog_service();

    /* Power button */

    con_str("POWER", "Initializing power key...");

    power_init();

    con_str("POWER", "Power polling active.");

    uint64_t frequency = watchdog_read_frequency();

    if (frequency == 0) {
        frequency = 13000000ULL;
    }

    /* Poll approximately every 10 milliseconds. */
    const uint64_t poll_interval = frequency / 100ULL;

    uint64_t last_power_poll = watchdog_read_counter();

    con_str("SHIM", "Initialization complete.");

    bootinfo_dump((uintptr_t)boot_state.x0);

    con_clear(BLACK);

    // fdt_dump_memory((uintptr_t)boot_state.x0);

    con_str("XNU", "Calling XNU Boot");
    loader_xnu_boot((uintptr_t)boot_state.x0);
    
    con_top();
    con_str("XNU", "XNU returned");

    for (;;) {
        watchdog_service();

        uint64_t now = watchdog_read_counter();

        if ((now - last_power_poll) >= poll_interval) {
            last_power_poll = now;
            power_poll();
        }

        __asm__ volatile("yield");
    }
}
