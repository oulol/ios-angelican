#include "exception.h"

#include "console.h"
#include "watchdog.h"

#include <stdint.h>

#define PWRAP_BASE       0x1000D000UL
#define PWRAP_WACS2_EN   0x09C
#define PWRAP_INIT_DONE2 0x0A0
#define PWRAP_WACS2_CMD  0xC20
#define PWRAP_WACS2_RDATA 0xC24
#define PWRAP_WACS2_VLDCLR 0xC28

#define MT6357_TOPSTATUS 0x24
#define POWER_KEY_BIT    (1U << 1)

#define WDT_MODE         0x10007000UL
#define WDT_SWRST        0x10007014UL

static inline void mmio_write(uintptr_t addr, uint32_t value) {
    *(volatile uint32_t *)addr = value;
}

static inline uint32_t mmio_read(uintptr_t addr) {
    return *(volatile uint32_t *)addr;
}

static int exception_pwrap_read(uint16_t address, uint16_t *value) {
    uintptr_t base = PWRAP_BASE;

    if (!(mmio_read(base + PWRAP_WACS2_EN) & 1))
        return 0;

    if (!(mmio_read(base + PWRAP_INIT_DONE2) & 1))
        return 0;

    /* Wait until WACS2 is idle. */
    for (unsigned int i = 0; i < 100000; i++) {
        uint32_t status = mmio_read(base + PWRAP_WACS2_RDATA);

        if (((status >> 16) & 0x7) == 0)
            break;

        if (i == 99999)
            return 0;
    }

    /* Issue a 16-bit PMIC read. */
    mmio_write(base + PWRAP_WACS2_CMD, (uint32_t)(address >> 1) << 16);

    /* Wait for valid response. */
    for (unsigned int i = 0; i < 100000; i++) {
        uint32_t status = mmio_read(base + PWRAP_WACS2_RDATA);

        if (((status >> 16) & 0x7) == 6) {
            *value = status & 0xFFFF;
            mmio_write(base + PWRAP_WACS2_VLDCLR, 1);
            return 1;
        }
    }

    return 0;
}

static int exception_power_button_pressed(void) {
    uint16_t status;

    if (!exception_pwrap_read(MT6357_TOPSTATUS, &status))
        return 0;

    /* Active-low power-key status. */
    return !(status & POWER_KEY_BIT);
}

static void exception_power_reset(void) {
    volatile uint32_t *wdt_mode = (volatile uint32_t *)WDT_MODE;
    volatile uint32_t *wdt_swrst = (volatile uint32_t *)WDT_SWRST;

    uint32_t mode = *wdt_mode;
    mode &= ~(1U << 3);
    *wdt_mode = mode | 0x22000000U;

    __asm__ volatile("dsb sy");

    for (;;) {
        *wdt_swrst = 0x1209;
        __asm__ volatile("dsb sy");
    }
}

static volatile uint32_t exception_active;

static inline uint64_t read_esr_el1(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(value));
    return value;
}

static inline uint64_t read_elr_el1(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, elr_el1" : "=r"(value));
    return value;
}

static inline uint64_t read_far_el1(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, far_el1" : "=r"(value));
    return value;
}

static inline uint64_t read_spsr_el1(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, spsr_el1" : "=r"(value));
    return value;
}

static inline uint64_t read_sctlr_el1(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(value));
    return value;
}

static inline uint64_t read_vbar_el1(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, vbar_el1" : "=r"(value));
    return value;
}

static inline uint64_t read_mpidr_el1(void)
{
    uint64_t value;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(value));
    return value;
}

// static const char *vector_name(uint64_t vector)
// {
//     static const char *const names[16] = {
//         "Current EL, SP0, sync",
//         "Current EL, SP0, IRQ",
//         "Current EL, SP0, FIQ",
//         "Current EL, SP0, SError",
//         "Current EL, SPx, sync",
//         "Current EL, SPx, IRQ",
//         "Current EL, SPx, FIQ",
//         "Current EL, SPx, SError",
//         "Lower EL AArch64, sync",
//         "Lower EL AArch64, IRQ",
//         "Lower EL AArch64, FIQ",
//         "Lower EL AArch64, SError",
//         "Lower EL AArch32, sync",
//         "Lower EL AArch32, IRQ",
//         "Lower EL AArch32, FIQ",
//         "Lower EL AArch32, SError"
//     };

//     return vector < 16 ? names[vector] : "Invalid vector";
// }

// static const char *exception_reason(uint64_t ec)
// {
//     switch (ec) {
//         case 0x00: return "Unknown exception";
//         case 0x01: return "Trapped WFI/WFE";
//         case 0x07: return "FP/SIMD access trap";
//         case 0x0E: return "Illegal execution state";
//         case 0x15: return "AArch64 SVC";
//         case 0x16: return "AArch64 HVC";
//         case 0x17: return "AArch64 SMC";
//         case 0x18: return "System instruction trap";
//         case 0x20: return "Instruction abort, lower EL";
//         case 0x21: return "Instruction abort, same EL";
//         case 0x22: return "PC alignment fault";
//         case 0x24: return "Data abort, lower EL";
//         case 0x25: return "Data abort, same EL";
//         case 0x26: return "SP alignment fault";
//         case 0x2F: return "SError interrupt";
//         case 0x30: return "Breakpoint, lower EL";
//         case 0x31: return "Breakpoint, same EL";
//         case 0x32: return "Software step, lower EL";
//         case 0x33: return "Software step, same EL";
//         case 0x34: return "Watchpoint, lower EL";
//         case 0x35: return "Watchpoint, same EL";
//         case 0x3C: return "BRK instruction";
//         default:   return "Unclassified exception";
//     }
// }

void el1_exception_dispatch(
    const el1_exception_frame_t *frame,
    uint64_t vector
)
{
    if (exception_active) {
        for (;;) {
            watchdog_service();
            __asm__ volatile("wfe");
        }
    }

    exception_active = 1;

    const uint64_t esr = read_esr_el1();
    const uint64_t ec = (esr >> 26) & 0x3FULL;
    const uint64_t elr = read_elr_el1();
    const uint64_t spsr = read_spsr_el1();

    const int far_is_valid =
        ec == 0x20 || ec == 0x21 || ec == 0x22 ||
        ec == 0x24 || ec == 0x25 || ec == 0x34 || ec == 0x35;

    static const char vector_names[16][32] = {
        "Current EL,     SP0, sync",
        "Current EL,     SP0, IRQ",
        "Current EL,     SP0, FIQ",
        "Current EL,     SP0, SError",
        "Current EL,     SPx, sync",
        "Current EL,     SPx, IRQ",
        "Current EL,     SPx, FIQ",
        "Current EL,     SPx, SError",
        "Lower EL        AArch64, sync",
        "Lower EL        AArch64, IRQ",
        "Lower EL       AArch64, FIQ",
        "Lower EL       AArch64, SError",
        "Lower EL       AArch32, sync",
        "Lower EL       AArch32, IRQ",
        "Lower EL       AArch32, FIQ",
        "Lower EL       AArch32, SError"
    };

    static const char exception_names[64][32] = {
        [0x00] = "Unknown exception",
        [0x01] = "Trapped WFI/WFE",
        [0x07] = "FP/SIMD access trap",
        [0x0E] = "Illegal execution state",
        [0x15] = "AArch64 SVC",
        [0x16] = "AArch64 HVC",
        [0x17] = "AArch64 SMC",
        [0x18] = "System instruction trap",
        [0x20] = "Instruction abort, lower EL",
        [0x21] = "Instruction abort, same EL",
        [0x22] = "PC alignment fault",
        [0x24] = "Data abort, lower EL",
        [0x25] = "Data abort, same EL",
        [0x26] = "SP alignment fault",
        [0x2F] = "SError interrupt",
        [0x30] = "Breakpoint, lower EL",
        [0x31] = "Breakpoint, same EL",
        [0x32] = "Software step, lower EL",
        [0x33] = "Software step, same EL",
        [0x34] = "Watchpoint, lower EL",
        [0x35] = "Watchpoint, same EL",
        [0x3C] = "BRK instruction"
    };

    static const char unknown_vector[] = "Invalid vector";
    static const char unknown_exception[] = "Unclassified exception";

    con_clear(RED);
    
    con_str("VECTOR", vector < 16
        ? vector_names[vector]
        : unknown_vector);

    con_str("REASON", ec < 64 && exception_names[ec][0]
        ? exception_names[ec]
        : unknown_exception);
    con_skip();

    con_hex("ESR", esr);
    con_hex("EC", ec);
    con_hex("ISS", esr & 0x1FFFFFFULL);
    con_hex("ELR", elr);
    con_hex("SPSR", spsr);

    if (far_is_valid) {
        con_hex("FAR", read_far_el1());
    }

    if (ec == 0x20 || ec == 0x21 || ec == 0x24 || ec == 0x25) {
        con_hex("FSC", esr & 0x3FULL);
    }

    if (ec == 0x24 || ec == 0x25) {
        con_hex("WNR", (esr >> 6) & 1ULL);
    }

    con_hex("EL1 SP", (uintptr_t)frame + sizeof(*frame));
    con_hex("MPIDR", read_mpidr_el1());
    con_hex("SCTLR", read_sctlr_el1());
    con_hex("VBAR", read_vbar_el1());

    static const char register_names[31][4] = {
        "X0",  "X1",  "X2",  "X3",
        "X4",  "X5",  "X6",  "X7",
        "X8",  "X9",  "X10", "X11",
        "X12", "X13", "X14", "X15",
        "X16", "X17", "X18", "X19",
        "X20", "X21", "X22", "X23",
        "X24", "X25", "X26", "X27",
        "X28", "X29", "X30"
    };

    con_skip();

    con_str("REGS", "Saved general-purpose registers:");

    for (unsigned int i = 0; i < 31; ++i) {
        con_hex(register_names[i], frame->x[i]);
    }

    wait_loop();
}


void wait_loop() {
    static const char spinner[] = "|/-\\";
    static unsigned int cursor = 0;

    for (;;) {
        watchdog_service();

        if (exception_power_button_pressed())
            exception_power_reset();

        char buf[] = "[ ] Waiting for power to reset";
        buf[1] = spinner[cursor];

        con_str("", buf);
        con_up();

        cursor = (cursor + 1) % 4;

        uint64_t start = watchdog_read_counter();
        uint64_t delay = watchdog_read_frequency() / 5;

        while (watchdog_read_counter() - start < delay) {
            watchdog_service();

            if (exception_power_button_pressed())
                exception_power_reset();

            __asm__ volatile("yield");
        }
    }
}