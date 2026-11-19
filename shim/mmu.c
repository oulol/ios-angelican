
#include "mmu.h"

#include <stddef.h>
#include <stdint.h>

/* ============================================================
 * Physical memory layout
 * ============================================================ */

#define RAM_BASE            0x40000000ULL
#define RAM_END             0x60000000ULL

#define FRAMEBUFFER_BASE    0x7EC00000ULL
#define FRAMEBUFFER_SIZE    0x00E00000ULL

#define GIC_BASE            0x0C000000ULL
#define GIC_SIZE            0x00400000ULL

#define MTK_MMIO_BASE       0x10000000ULL
#define MTK_MMIO_SIZE       0x00400000ULL

/* ============================================================
 * Translation table configuration
 * ============================================================ */

#define PAGE_SIZE           0x1000ULL
#define BLOCK_SIZE          0x200000ULL

#define TABLE_ENTRIES       512

#define L1_INDEX(addr)      (((addr) >> 30) & 0x1FF)
#define L2_INDEX(addr)      (((addr) >> 21) & 0x1FF)

/* Descriptor bits */

#define DESC_VALID          (1ULL << 0)
#define DESC_TABLE          (1ULL << 1)
#define DESC_AF             (1ULL << 10)

#define DESC_SH_OUTER       (2ULL << 8)
#define DESC_SH_INNER       (3ULL << 8)

#define DESC_AP_EL1_RW      (0ULL << 6)

#define DESC_PXN            (1ULL << 53)
#define DESC_UXN            (1ULL << 54)

/* MAIR attribute indices */

#define ATTR_DEVICE         0
#define ATTR_NORMAL_WB      1
#define ATTR_NORMAL_NC      2

#define ATTR_INDEX(x)       ((uint64_t)(x) << 2)

/* ============================================================
 * Translation tables
 *
 * 39-bit VA:
 *
 * L1[0] -> 0x00000000 - 0x40000000
 * L1[1] -> 0x40000000 - 0x80000000
 *
 * Each L2 entry maps a 2 MiB block.
 * ============================================================ */

static uint64_t l1_table[TABLE_ENTRIES]
    __attribute__((aligned(4096)));

static uint64_t l2_low[TABLE_ENTRIES]
    __attribute__((aligned(4096)));

static uint64_t l2_high[TABLE_ENTRIES]
    __attribute__((aligned(4096)));

/* ============================================================
 * Register access
 * ============================================================ */

static inline uint64_t read_current_el(void)
{
    uint64_t value;

    __asm__ volatile(
        "mrs %0, CurrentEL"
        : "=r"(value)
    );

    return value;
}

static inline uint64_t read_sctlr_el1(void)
{
    uint64_t value;

    __asm__ volatile(
        "mrs %0, SCTLR_EL1"
        : "=r"(value)
    );

    return value;
}

static inline void write_mair_el1(uint64_t value)
{
    __asm__ volatile(
        "msr MAIR_EL1, %0"
        :
        : "r"(value)
        : "memory"
    );
}

static inline void write_tcr_el1(uint64_t value)
{
    __asm__ volatile(
        "msr TCR_EL1, %0"
        :
        : "r"(value)
        : "memory"
    );
}

static inline void write_ttbr0_el1(uint64_t value)
{
    __asm__ volatile(
        "msr TTBR0_EL1, %0"
        :
        : "r"(value)
        : "memory"
    );
}

/* ============================================================
 * Table helpers
 * ============================================================ */

static void zero_tables(void)
{
    for (size_t i = 0; i < TABLE_ENTRIES; i++) {
        l1_table[i] = 0;
        l2_low[i] = 0;
        l2_high[i] = 0;
    }
}

static uint64_t* get_l2_table(uint64_t address)
{
    switch (L1_INDEX(address)) {
        case 0:
            return l2_low;

        case 1:
            return l2_high;

        default:
            return NULL;
    }
}

/*
 * Create a 2 MiB block descriptor.
 *
 * RAM:
 *   Normal, write-back, write-allocate
 *   Inner shareable
 *   EL1 read/write
 *   EL1 executable
 *
 * MMIO:
 *   Device-nGnRE
 *   Outer shareable
 *   EL1 read/write
 *   Execute-never
 *
 * Framebuffer:
 *   Normal, non-cacheable
 *   Inner shareable
 *   EL1 read/write
 *   Execute-never
 */

static uint64_t make_block_descriptor(
    uint64_t address,
    unsigned int attribute,
    uint64_t shareability,
    int privileged_execute_never
)
{
    uint64_t descriptor =
        (address & ~(BLOCK_SIZE - 1)) |
        DESC_VALID |
        DESC_AF |
        DESC_AP_EL1_RW |
        ATTR_INDEX(attribute) |
        shareability |
        DESC_UXN;

    if (privileged_execute_never) {
        descriptor |= DESC_PXN;
    }

    return descriptor;
}

/*
 * Map a physical range using 2 MiB blocks.
 *
 * Virtual address == physical address.
 *
 * Both address and size must be 2 MiB aligned.
 */

static int map_range(
    uint64_t base,
    uint64_t size,
    unsigned int attribute,
    uint64_t shareability,
    int privileged_execute_never
)
{
    if ((base & (BLOCK_SIZE - 1)) != 0)
        return -1;

    if ((size & (BLOCK_SIZE - 1)) != 0)
        return -1;

    for (uint64_t offset = 0; offset < size; offset += BLOCK_SIZE) {
        uint64_t address = base + offset;

        uint64_t* table = get_l2_table(address);

        if (table == NULL)
            return -1;

        unsigned int index = (unsigned int)L2_INDEX(address);

        table[index] = make_block_descriptor(
            address,
            attribute,
            shareability,
            privileged_execute_never
        );
    }

    return 0;
}

/* ============================================================
 * Translation table setup
 * ============================================================ */

static int build_tables(void)
{
    zero_tables();

    /*
     * Connect L1 entries to their corresponding L2 tables.
     *
     * Table descriptor:
     * bit 0 = valid
     * bit 1 = table
     */

    l1_table[0] =
        ((uint64_t)(uintptr_t)l2_low & ~0xFFFULL) |
        DESC_VALID |
        DESC_TABLE;

    l1_table[1] =
        ((uint64_t)(uintptr_t)l2_high & ~0xFFFULL) |
        DESC_VALID |
        DESC_TABLE;

    /*
     * Physical RAM:
     * 0x40000000 - 0x60000000
     *
     * Normal WBWA, inner shareable.
     * Executable at EL1.
     */

    if (map_range(
        RAM_BASE,
        RAM_END - RAM_BASE,
        ATTR_NORMAL_WB,
        DESC_SH_INNER,
        0
    ) != 0) {
        return -1;
    }

    /*
     * Framebuffer:
     * 0x7EC00000 - 0x7FA00000
     *
     * Normal non-cacheable memory.
     *
     * This covers the three framebuffer pages and
     * their surrounding 2 MiB-aligned region.
     */

    if (map_range(
        FRAMEBUFFER_BASE,
        FRAMEBUFFER_SIZE,
        ATTR_NORMAL_NC,
        DESC_SH_INNER,
        1
    ) != 0) {
        return -1;
    }

    /*
     * GICv3:
     * Distributor and Redistributor.
     *
     * 0x0C000000 - 0x0C400000
     */

    if (map_range(
        GIC_BASE,
        GIC_SIZE,
        ATTR_DEVICE,
        DESC_SH_OUTER,
        1
    ) != 0) {
        return -1;
    }

    /*
     * MediaTek MMIO:
     *
     * Includes:
     *   TOPRGU watchdog  0x10007000
     *   PWRAP            0x1000D000
     *   GIC-related sys  0x10200A80
     *
     * 0x10000000 - 0x10400000
     */

    if (map_range(
        MTK_MMIO_BASE,
        MTK_MMIO_SIZE,
        ATTR_DEVICE,
        DESC_SH_OUTER,
        1
    ) != 0) {
        return -1;
    }

    return 0;
}

/* ============================================================
 * MMU initialization
 * ============================================================ */

int mmu_is_enabled(void)
{
    return (read_sctlr_el1() & 1ULL) != 0;
}

int mmu_init(void)
{
    /* Must execute at EL1. */

    if (read_current_el() != 0x4)
        return -1;

    /* Do not replace an already active translation setup. */

    if (mmu_is_enabled())
        return -2;

    if (build_tables() != 0)
        return -3;

    /*
     * MAIR_EL1:
     *
     * Attr0 = 0x04: Device-nGnRE
     * Attr1 = 0xFF: Normal WBWA
     * Attr2 = 0x44: Normal non-cacheable
     */

    const uint64_t mair =
        0x04ULL |
        (0xFFULL << 8) |
        (0x44ULL << 16);

    /*
     * TCR_EL1:
     *
     * T0SZ  = 25       -> 39-bit VA
     * EPD1  = 1        -> Disable TTBR1 walks
     * IRGN0 = 01       -> WBWA
     * ORGN0 = 01       -> WBWA
     * SH0   = 11       -> Inner shareable
     * TG0   = 00       -> 4 KiB granule
     * IPS   = 010      -> 40-bit physical addresses
     */

    const uint64_t tcr =
        25ULL |
        (1ULL << 23) |
        (1ULL << 8) |
        (1ULL << 10) |
        (3ULL << 12) |
        (2ULL << 32);

    const uint64_t ttbr0 =
        (uint64_t)(uintptr_t)l1_table;

    /*
     * Ensure all translation table writes are visible
     * before the hardware starts walking them.
     */

    __asm__ volatile("dsb sy" ::: "memory");

    write_mair_el1(mair);
    write_tcr_el1(tcr);
    write_ttbr0_el1(ttbr0);

    __asm__ volatile(
        "dsb sy\n"
        "tlbi vmalle1\n"
        "dsb sy\n"
        "isb\n"
        ::: "memory"
    );

    /*
     * Enable EL1 stage-1 address translation.
     *
     * Preserve all existing SCTLR_EL1 settings.
     * Do not enable caches here.
     */

    uint64_t sctlr = read_sctlr_el1();

    sctlr |= 1ULL; /* SCTLR_EL1.M */

    __asm__ volatile(
        "msr SCTLR_EL1, %0\n"
        "dsb sy\n"
        "isb\n"
        :
        : "r"(sctlr)
        : "memory"
    );

    return mmu_is_enabled() ? 0 : -4;
}
