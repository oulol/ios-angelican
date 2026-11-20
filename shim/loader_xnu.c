
#include "loader.h"
#include "boot_state.h"
#include "fdt.h"
#include "console.h"

#include <stdint.h>
#include <stddef.h>

/*
 * Angelican XNU handoff loader.
 *
 * Input:
 *   - Runtime Linux FDT passed in x0.
 *   - Initrd location obtained from /chosen.
 *   - Initrd contains a prepared SWEETXNU payload.
 *
 * Output:
 *   - XNU entry receives physical boot_args in x0.
 *   - MMU and caches disabled.
 *
 * This does NOT parse Mach-O or resolve chained fixups.
 */

#define PAYLOAD_MAGIC       "SWEETXNU"
#define PAYLOAD_MAGIC_SIZE  8

#define PAYLOAD_MAX_SEGS    128
#define BOOT_LINE_LENGTH    608
#define PAGE_16K            0x4000ULL

#define RAM_BASE            0x40000000ULL
#define RAM_SIZE            0x20000000ULL
#define RAM_END             (RAM_BASE + RAM_SIZE)

/* Redmi 9C framebuffer. */
#define FB_BASE             0x7EC50000ULL
#define FB_STRIDE           2944ULL
#define FB_DEPTH            32ULL

#define CACHE_LINE_SIZE     64ULL

struct payload_seg {
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t file_off;
    uint64_t file_size;
};

struct payload {
    char magic[8];

    uint32_t version;
    uint32_t nsegs;

    uint64_t virt_base;
    uint64_t entry_va;

    uint64_t adt_off;
    uint64_t adt_size;

    uint64_t kernel_end;

    uint64_t phys_base;
    uint64_t mem_size;

    char cmdline[BOOT_LINE_LENGTH];

    struct payload_seg segs[];
};

struct boot_video {
    uint64_t v_baseAddr;
    uint64_t v_display;
    uint64_t v_rowBytes;
    uint64_t v_width;
    uint64_t v_height;
    uint64_t v_depth;
};

struct boot_args {
    uint16_t Revision;
    uint16_t Version;

    uint64_t virtBase;
    uint64_t physBase;
    uint64_t memSize;
    uint64_t topOfKernelData;

    struct boot_video Video;

    uint32_t machineType;

    uint64_t deviceTreeP;
    uint32_t deviceTreeLength;

    char CommandLine[BOOT_LINE_LENGTH];

    uint64_t bootFlags;
    uint64_t memSizeActual;
};

_Static_assert(
    offsetof(struct payload, segs) == 0x2A8,
    "Unexpected payload header layout"
);

_Static_assert(
    offsetof(struct boot_args, Video) == 40,
    "Unexpected boot_args Video offset"
);

_Static_assert(
    offsetof(struct boot_args, CommandLine) == 108,
    "Unexpected boot_args CommandLine offset"
);

_Static_assert(
    sizeof(struct boot_args) == 736,
    "Unexpected boot_args size"
);

typedef void (*xnu_entry_t)(uint64_t boot_args_pa)
    __attribute__((noreturn));

static __attribute__((noreturn))
void xnu_error(const char *message, uint64_t value)
{
    con_str("XNU ERROR", message);
    con_hex("Value", value);

    for (;;) {
        __asm__ volatile("wfe");
    }
}

static int add_overflow_u64(
    uint64_t a,
    uint64_t b,
    uint64_t *result
)
{
    *result = a + b;
    return *result < a;
}

static uint64_t align_up(
    uint64_t value,
    uint64_t alignment
)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

static int range_inside(
    uint64_t address,
    uint64_t size,
    uint64_t start,
    uint64_t end
)
{
    uint64_t range_end;

    if (add_overflow_u64(address, size, &range_end))
        return 0;

    return address >= start && range_end <= end;
}

static int ranges_overlap(
    uint64_t a,
    uint64_t a_size,
    uint64_t b,
    uint64_t b_size
)
{
    uint64_t a_end;
    uint64_t b_end;

    if (add_overflow_u64(a, a_size, &a_end))
        return 1;

    if (add_overflow_u64(b, b_size, &b_end))
        return 1;

    return a < b_end && b < a_end;
}

static void copy_bytes(
    void *destination,
    const void *source,
    uint64_t size
)
{
    uint8_t *dst = destination;
    const uint8_t *src = source;

    for (uint64_t i = 0; i < size; i++)
        dst[i] = src[i];
}

static void zero_bytes(
    void *destination,
    uint64_t size
)
{
    uint8_t *dst = destination;

    for (uint64_t i = 0; i < size; i++)
        dst[i] = 0;
}

static int payload_magic_ok(const struct payload *p)
{
    for (unsigned i = 0; i < PAYLOAD_MAGIC_SIZE; i++) {
        if (p->magic[i] != PAYLOAD_MAGIC[i])
            return 0;
    }

    return 1;
}

/*
 * Locate SWEETXNU anywhere inside the initrd.
 */
static struct payload *find_payload(
    uintptr_t initrd_start,
    uint64_t initrd_size
)
{
    const uint8_t *data = (const uint8_t *)initrd_start;

    if (initrd_size < sizeof(struct payload))
        return NULL;

    for (uint64_t i = 0;
         i <= initrd_size - sizeof(struct payload);
         i++) {

        if (data[i] != 'S')
            continue;

        struct payload *p =
            (struct payload *)(uintptr_t)(initrd_start + i);

        if (payload_magic_ok(p))
            return p;
    }

    return NULL;
}

/*
 * Translate an XNU virtual address into its physical address.
 */
static int translate_address(
    const struct payload *p,
    uint64_t virtual_address,
    uint64_t *physical_address
)
{
    if (virtual_address < p->virt_base)
        return 0;

    uint64_t offset = virtual_address - p->virt_base;

    if (add_overflow_u64(
            p->phys_base,
            offset,
            physical_address
        ))
        return 0;

    return 1;
}

/*
 * Clean a memory range to the point of coherency.
 *
 * Cortex-A53 cache lines are 64 bytes.
 */
static void cache_clean_range(
    uint64_t address,
    uint64_t size
)
{
    uint64_t start = address & ~(CACHE_LINE_SIZE - 1);
    uint64_t end = align_up(address + size, CACHE_LINE_SIZE);

    for (uint64_t p = start; p < end; p += CACHE_LINE_SIZE) {
        __asm__ volatile(
            "dc cvac, %0"
            :
            : "r"(p)
            : "memory"
        );
    }
}

/*
 * Public entry point.
 *
 * Call this from EL1 with the runtime FDT address.
 */
__attribute__((noreturn))
void loader_xnu_boot(uintptr_t dtb)
{
    uint64_t initrd_start;
    uint64_t initrd_end;

    con_skip();
    con_str("XNU", "Preparing kernel handoff");

    /* Verify execution level. */
    uint64_t current_el;

    __asm__ volatile(
        "mrs %0, CurrentEL"
        : "=r"(current_el)
    );

    if (current_el != 4)
        xnu_error("Expected EL1", current_el);

    /* Verify MMU is off. */
    uint64_t sctlr;

    __asm__ volatile(
        "mrs %0, sctlr_el1"
        : "=r"(sctlr)
    );

    if (sctlr & 1ULL)
        xnu_error("Shim MMU must be disabled", sctlr);

    /* Read initrd location from the runtime FDT. */
    if (!fdt_chosen_u64(
            dtb,
            "linux,initrd-start",
            &initrd_start
        ))
        xnu_error("Missing initrd-start", dtb);

    if (!fdt_chosen_u64(
            dtb,
            "linux,initrd-end",
            &initrd_end
        ))
        xnu_error("Missing initrd-end", dtb);

    if (initrd_end <= initrd_start)
        xnu_error("Invalid initrd range", initrd_start);

    uint64_t initrd_size = initrd_end - initrd_start;

    if (!range_inside(
            initrd_start,
            initrd_size,
            RAM_BASE,
            RAM_END
        ))
        xnu_error("Initrd outside RAM", initrd_start);

    con_hex("Initrd start", initrd_start);
    con_hex("Initrd size", initrd_size);

    /* Locate prepared payload. */
    struct payload *p = find_payload(
        (uintptr_t)initrd_start,
        initrd_size
    );

    if (!p)
        xnu_error("SWEETXNU payload not found", initrd_start);

    con_str("XNU", "Payload found");
    con_hex("Payload version", p->version);
    con_hex("Segment count", p->nsegs);

    if (p->nsegs == 0 || p->nsegs > PAYLOAD_MAX_SEGS)
        xnu_error("Invalid segment count", p->nsegs);

    if (p->mem_size == 0 ||
        !range_inside(p->phys_base, p->mem_size, RAM_BASE, RAM_END))
        xnu_error("Invalid kernel memory range", p->phys_base);

    uint64_t payload_offset =
        (uint64_t)(uintptr_t)p - initrd_start;

    uint64_t payload_size = initrd_size - payload_offset;

    uint64_t segments_size =
        (uint64_t)p->nsegs * sizeof(struct payload_seg);

    uint64_t minimum_size;

    if (add_overflow_u64(
            offsetof(struct payload, segs),
            segments_size,
            &minimum_size
        ) ||
        minimum_size > payload_size)
        xnu_error("Payload segment table out of bounds", minimum_size);

    /* Validate ADT source range. */
    if (p->adt_size == 0 ||
        p->adt_off > payload_size ||
        p->adt_size > payload_size - p->adt_off)
        xnu_error("Invalid ADT payload range", p->adt_off);

    if (p->adt_off & 7ULL)
        xnu_error("ADT offset is not 8-byte aligned", p->adt_off);

    uint64_t adt_pa;

    if (!translate_address(p, p->kernel_end, &adt_pa))
        xnu_error("Invalid kernel_end", p->kernel_end);

    if (!range_inside(
            adt_pa,
            p->adt_size,
            RAM_BASE,
            RAM_END
        ))
        xnu_error("ADT destination outside RAM", adt_pa);

    /*
     * Copy kernel segments.
     */
    con_skip();
    con_str("XNU", "Copying kernel segments");

    for (uint32_t i = 0; i < p->nsegs; i++) {
        const struct payload_seg *s = &p->segs[i];

        if (s->vmsize < s->file_size)
            xnu_error("Segment file_size exceeds vmsize", i);

        if (s->file_off > payload_size ||
            s->file_size > payload_size - s->file_off)
            xnu_error("Segment source outside payload", i);

        uint64_t destination_pa;

        if (!translate_address(p, s->vmaddr, &destination_pa))
            xnu_error("Invalid segment virtual address", s->vmaddr);

        if (!range_inside(
                destination_pa,
                s->vmsize,
                RAM_BASE,
                RAM_END
            ))
            xnu_error("Segment destination outside RAM", destination_pa);

        /* Do not overwrite the initrd while reading it. */
        if (ranges_overlap(
                destination_pa,
                s->vmsize,
                initrd_start,
                initrd_size
            ))
            xnu_error("Segment overlaps initrd", destination_pa);

        const void *source =
            (const void *)(uintptr_t)(
                (uintptr_t)p + s->file_off
            );

        void *destination =
            (void *)(uintptr_t)destination_pa;

        copy_bytes(destination, source, s->file_size);

        if (s->vmsize > s->file_size) {
            zero_bytes(
                (void *)(uintptr_t)(destination_pa + s->file_size),
                s->vmsize - s->file_size
            );
        }

        cache_clean_range(destination_pa, s->vmsize);

        con_hex("Segment PA", destination_pa);
        con_hex("Segment size", s->vmsize);
    }

    /*
     * Copy the prepared ADT immediately after the kernel.
     */
    const void *adt_source =
        (const void *)(uintptr_t)(
            (uintptr_t)p + p->adt_off
        );

    copy_bytes(
        (void *)(uintptr_t)adt_pa,
        adt_source,
        p->adt_size
    );

    cache_clean_range(adt_pa, p->adt_size);

    con_hex("ADT PA", adt_pa);
    con_hex("ADT size", p->adt_size);

    /*
     * Allocate boot_args on a 16 KiB boundary after the ADT.
     */
    uint64_t adt_end;

    if (add_overflow_u64(adt_pa, p->adt_size, &adt_end))
        xnu_error("ADT address overflow", adt_pa);

    uint64_t boot_args_pa = align_up(adt_end, PAGE_16K);

    if (!range_inside(
            boot_args_pa,
            PAGE_16K,
            RAM_BASE,
            RAM_END
        ))
        xnu_error("boot_args outside RAM", boot_args_pa);

    if (ranges_overlap(
            boot_args_pa,
            PAGE_16K,
            initrd_start,
            initrd_size
        ))
        xnu_error("boot_args overlaps initrd", boot_args_pa);

    struct boot_args *args =
        (struct boot_args *)(uintptr_t)boot_args_pa;

    zero_bytes(args, sizeof(*args));

    args->Revision = 2;
    args->Version = 2;

    args->virtBase = p->virt_base;
    args->physBase = p->phys_base;
    args->memSize = p->mem_size;

    args->topOfKernelData = boot_args_pa + PAGE_16K;

    args->Video.v_baseAddr = FB_BASE;
    args->Video.v_display = 0;
    args->Video.v_rowBytes = FB_STRIDE;
    args->Video.v_width = FB_W;
    args->Video.v_height = FB_H;
    args->Video.v_depth = FB_DEPTH;

    /*
     * machineType is platform-specific. Keep zero until
     * the Angelican XNU platform code defines its expected value.
     */

    /*
     * XNU expects the ADT virtual address, not its physical address.
     */
    if (adt_pa < p->phys_base)
        xnu_error("ADT below physBase", adt_pa);

    args->deviceTreeP =
        p->virt_base + (adt_pa - p->phys_base);

    if (p->adt_size > UINT32_MAX)
        xnu_error("ADT too large", p->adt_size);

    args->deviceTreeLength = (uint32_t)p->adt_size;

    copy_bytes(
        args->CommandLine,
        p->cmdline,
        BOOT_LINE_LENGTH - 1
    );

    args->CommandLine[BOOT_LINE_LENGTH - 1] = '\0';

    args->bootFlags = 0;
    args->memSizeActual = p->mem_size;

    cache_clean_range(boot_args_pa, sizeof(*args));

    con_skip();
    con_str("XNU", "Handoff information");

    con_hex("Kernel entry VA", p->entry_va);

    uint64_t entry_pa;

    if (!translate_address(p, p->entry_va, &entry_pa))
        xnu_error("Invalid XNU entry address", p->entry_va);

    if (!range_inside(entry_pa, 4, RAM_BASE, RAM_END))
        xnu_error("XNU entry outside RAM", entry_pa);

    con_hex("Kernel entry PA", entry_pa);
    con_hex("boot_args PA", boot_args_pa);
    con_hex("ADT VA", args->deviceTreeP);

    /*
     * Clean boot structures and invalidate instruction cache.
     * Then disable MMU and caches.
     */
    cache_clean_range(adt_pa, p->adt_size);
    cache_clean_range(boot_args_pa, sizeof(*args));

    __asm__ volatile("dsb sy" ::: "memory");

    /*
     * Mask interrupts before entering XNU.
     */
    __asm__ volatile("msr daifset, #0xf" ::: "memory");

    con_str("XNU", "Branching to kernel");

    /*
     * Enter XNU:
     *   x0 = physical boot_args address
     *   PC = physical kernel entry address
     *
     * No C code is executed after this branch.
     */
    __asm__ volatile(
        "mov x0, %0\n"
        "br  %1\n"
        :
        : "r"(boot_args_pa), "r"(entry_pa)
        : "x0", "memory"
    );

    __builtin_unreachable();
}
