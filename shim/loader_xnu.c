#include "fdt.h"
#include "console.h"
#include "exception.h"


#include <stdint.h>
#include <stddef.h>

/*
 * Angelican XNU handoff loader.
 *
 * The runtime Linux FDT is authoritative for physical RAM and reserved ranges.
 * The payload's phys_base/mem_size fields describe the ADT's actual DRAM
 * properties; they are NOT used as the kernel's physical load base.
 *
 * This loader:
 *   - finds the prepared SWEETXNU payload in /chosen's initrd;
 *   - parses /memory, /reserved-memory, and the FDT reservation map;
 *   - excludes the initrd and runtime FDT themselves;
 *   - chooses the largest 16-KiB-aligned contiguous free RAM interval;
 *   - requires that interval to exceed 1 GiB (XNU 8796 selects 16-KiB
 *     software pages for memSize > 1 GiB);
 *   - relocates the kernel segments into that interval;
 *   - places the ADT and boot_args after the kernel;
 *   - enters __start at EL1 with MMU and caches disabled, x0 = boot_args PA.
 *
 * IMPORTANT BUILD CONTRACT:
 *   Regenerate the ADT/payload with /chosen/dram-base and dram-size set to
 *   the runtime FDT's actual DRAM values. For the currently observed device:
 *       dram-base = 0x40000000
 *       dram-size = 0xC0000000
 *   The payload header's phys_base/mem_size must therefore contain those same
 *   values. The loader checks this and refuses a stale payload.
 *
 * This file does not parse Mach-O or resolve chained fixups.
 */

#define PAYLOAD_MAGIC          "SWEETXNU"
#define PAYLOAD_MAGIC_SIZE     8U
#define PAYLOAD_VERSION        1U
#define PAYLOAD_MAX_SEGS       128U
#define BOOT_LINE_LENGTH       608U
#define PAGE_16K               0x4000ULL
#define ONE_GIB                0x40000000ULL

#define ADT_MAX_DEPTH          16U

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
    uint64_t phys_base; /* ADT /chosen/dram-base: actual DRAM base. */
    uint64_t mem_size;  /* ADT /chosen/dram-size: actual DRAM size. */
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

_Static_assert(offsetof(struct payload, segs) == 0x2A8,
               "Unexpected payload header layout");
_Static_assert(offsetof(struct boot_args, Video) == 40,
               "Unexpected boot_args Video offset");
_Static_assert(offsetof(struct boot_args, CommandLine) == 108,
               "Unexpected boot_args CommandLine offset");
_Static_assert(sizeof(struct boot_args) == 736,
               "Unexpected boot_args size");

static __attribute__((noreturn))
void xnu_error(const char *message, uint64_t value)
{
    con_str("XNU", message);
    con_hex("Value", value);

    wait_loop();
}

static int add_overflow_u64(uint64_t a, uint64_t b, uint64_t *result)
{
    *result = a + b;
    return *result < a;
}

static int range_end(const struct phys_range *range, uint64_t *end)
{
    if (!range->size)
        return 0;
    return !add_overflow_u64(range->base, range->size, end);
}

static int range_inside(uint64_t address, uint64_t size,
                        uint64_t start, uint64_t end)
{
    uint64_t range_end_value;
    if (add_overflow_u64(address, size, &range_end_value))
        return 0;
    return address >= start && range_end_value <= end;
}

static int ranges_overlap(uint64_t a, uint64_t a_size,
                          uint64_t b, uint64_t b_size)
{
    uint64_t a_end, b_end;
    if (add_overflow_u64(a, a_size, &a_end) ||
        add_overflow_u64(b, b_size, &b_end))
        return 1;
    return a < b_end && b < a_end;
}

static int align_up_checked(uint64_t value, uint64_t alignment,
                            uint64_t *result)
{
    uint64_t add = alignment - 1;
    if (value > UINT64_MAX - add)
        return 0;
    *result = (value + add) & ~add;
    return 1;
}

static uint64_t align_down(uint64_t value, uint64_t alignment)
{
    return value & ~(alignment - 1);
}

static void copy_bytes(void *destination, const void *source, uint64_t size)
{
    uint8_t *dst = (uint8_t *)destination;
    const uint8_t *src = (const uint8_t *)source;
    for (uint64_t i = 0; i < size; i++)
        dst[i] = src[i];
}

static void zero_bytes(void *destination, uint64_t size)
{
    uint8_t *dst = (uint8_t *)destination;
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

static struct payload *find_payload(uintptr_t initrd_start,
                                    uint64_t initrd_size)
{
    const uint8_t *data = (const uint8_t *)initrd_start;
    if (initrd_size < sizeof(struct payload))
        return NULL;

    for (uint64_t i = 0;
         i <= initrd_size - sizeof(struct payload);
         i++) {
        uintptr_t candidate = initrd_start + (uintptr_t)i;
        if (data[i] != 'S' || (candidate & 7U))
            continue;
        struct payload *p = (struct payload *)candidate;
        if (payload_magic_ok(p))
            return p;
    }
    return NULL;
}

static int translate_address(uint64_t virt_base, uint64_t phys_base,
                             uint64_t virtual_address,
                             uint64_t *physical_address)
{
    if (virtual_address < virt_base)
        return 0;
    return !add_overflow_u64(phys_base,
                             virtual_address - virt_base,
                             physical_address);
}

/* Apple ADT uses little-endian nodes: nprops, nchildren, then properties. */
static uint32_t adt_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t adt_le64(const uint8_t *p)
{
    return (uint64_t)adt_le32(p) | ((uint64_t)adt_le32(p + 4) << 32);
}

static int adt_fixed_name_equal(const uint8_t name[32], const char *text)
{
    uint32_t i = 0;
    while (text[i]) {
        if (i >= 32 || name[i] != (uint8_t)text[i])
            return 0;
        i++;
    }
    return i < 32 && name[i] == 0;
}

static int adt_value_equal(const uint8_t *value, uint32_t length,
                           const char *text)
{
    uint32_t i = 0;
    while (text[i]) {
        if (i >= length || value[i] != (uint8_t)text[i])
            return 0;
        i++;
    }
    return i < length && value[i] == 0;
}

struct adt_memory_info {
    uint64_t dram_base;
    uint64_t dram_size;
    uint8_t have_dram_base;
    uint8_t have_dram_size;
};

static int adt_walk_node(const uint8_t *blob, uint32_t blob_size,
                         uint32_t *offset, uint32_t depth,
                         struct adt_memory_info *info,
                         struct fdt_layout *layout)
{
    uint32_t nprops, nchildren;
    const uint8_t *node_name = NULL;
    uint32_t node_name_len = 0;
    const uint8_t *dram_base_value = NULL;
    const uint8_t *dram_size_value = NULL;
    uint32_t dram_base_len = 0, dram_size_len = 0;

    if (depth >= ADT_MAX_DEPTH || *offset > blob_size ||
        blob_size - *offset < 8)
        return 0;

    nprops = adt_le32(blob + *offset);
    nchildren = adt_le32(blob + *offset + 4);
    *offset += 8;

    for (uint32_t i = 0; i < nprops; i++) {
        const uint8_t *prop_name;
        const uint8_t *value;
        uint32_t size_field, length, padded;

        if (*offset > blob_size || blob_size - *offset < 36)
            return 0;

        prop_name = blob + *offset;
        size_field = adt_le32(blob + *offset + 32);
        length = size_field & 0x7FFFFFFFU;
        *offset += 36;

        if (length > blob_size - *offset)
            return 0;
        value = blob + *offset;
        padded = (length + 3U) & ~3U;
        if (padded > blob_size - *offset)
            return 0;

        if (adt_fixed_name_equal(prop_name, "name")) {
            node_name = value;
            node_name_len = length;
        } else if (depth == 1 &&
                   adt_fixed_name_equal(prop_name, "dram-base")) {
            dram_base_value = value;
            dram_base_len = length;
        } else if (depth == 1 &&
                   adt_fixed_name_equal(prop_name, "dram-size")) {
            dram_size_value = value;
            dram_size_len = length;
        } else if (adt_fixed_name_equal(prop_name, "reg") &&
                length >= 16 && (length % 16) == 0 &&
                node_name &&
                (adt_value_equal(node_name, node_name_len, "pram") ||
                    adt_value_equal(node_name, node_name_len, "angelican-fbcon"))) {

            for (uint32_t reg_off = 0; reg_off < length; reg_off += 16) {
                uint64_t base = adt_le64(value + reg_off);
                uint64_t size = adt_le64(value + reg_off + 8);

                if (!fdt_add_range(layout->reserved,
                                &layout->reserved_count,
                                FDT_MAX_RESERVED, base, size))
                    return 0;
            }
        }

        *offset += padded;
    }

    if (depth == 1 && node_name &&
        adt_value_equal(node_name, node_name_len, "chosen")) {
        if (!dram_base_value || dram_base_len != 8 ||
            !dram_size_value || dram_size_len != 8)
            return 0;
        info->dram_base = adt_le64(dram_base_value);
        info->dram_size = adt_le64(dram_size_value);
        info->have_dram_base = 1;
        info->have_dram_size = 1;
    }

    for (uint32_t i = 0; i < nchildren; i++) {
        if (!adt_walk_node(blob, blob_size, offset, depth + 1,
                           info, layout))
            return 0;
    }

    return 1;
}

static int adt_parse_memory_info(const uint8_t *blob, uint32_t blob_size,
                                 struct adt_memory_info *info,
                                 struct fdt_layout *layout)
{
    uint32_t offset = 0;
    info->dram_base = 0;
    info->dram_size = 0;
    info->have_dram_base = 0;
    info->have_dram_size = 0;

    if (!blob || !blob_size ||
        !adt_walk_node(blob, blob_size, &offset, 0, info, layout))
        return 0;

    return info->have_dram_base && info->have_dram_size;
}

static void sort_ranges(struct phys_range *ranges, uint32_t count)
{
    for (uint32_t i = 1; i < count; i++) {
        struct phys_range key = ranges[i];
        uint32_t j = i;
        while (j > 0 && ranges[j - 1].base > key.base) {
            ranges[j] = ranges[j - 1];
            j--;
        }
        ranges[j] = key;
    }
}

static int find_kernel_memory_window(
    fdt_layout_t *layout,
    uint64_t *base_out,
    uint64_t *size_out
) {
    if (!base_out || !size_out)
        return 0;

    *base_out = 0;
    *size_out = 0;

    if (!layout || layout->memory_count != 1)
        return 0;

    const uint64_t ram_base = layout->memory[0].base;
    const uint64_t ram_size = layout->memory[0].size;

    if (!ram_size || ram_base > UINT64_MAX - ram_size)
        return 0;

    const uint64_t ram_end = ram_base + ram_size;

    uint64_t cursor = ram_base;
    uint64_t best_base = 0;
    uint64_t best_size = 0;

    // Sort reservations by ascending base address.
    for (uint32_t i = 0; i < layout->reserved_count; i++) {
        for (uint32_t j = i + 1; j < layout->reserved_count; j++) {
            if (layout->reserved[j].base < layout->reserved[i].base) {
                phys_range_t temp = layout->reserved[i];
                layout->reserved[i] = layout->reserved[j];
                layout->reserved[j] = temp;
            }
        }
    }

    for (uint32_t i = 0; i < layout->reserved_count; i++) {
        const phys_range_t *r = &layout->reserved[i];

        if (!r->size || r->base > UINT64_MAX - r->size)
            continue;

        uint64_t reserved_base = r->base;
        uint64_t reserved_end = r->base + r->size;

        // Ignore reservations outside RAM.
        if (reserved_end <= ram_base || reserved_base >= ram_end)
            continue;

        // Clip reservation to RAM boundaries.
        if (reserved_base < ram_base)
            reserved_base = ram_base;

        if (reserved_end > ram_end)
            reserved_end = ram_end;

        // Check the gap before this reservation.
        if (reserved_base > cursor) {
            uint64_t aligned_base =
                (cursor + PAGE_16K - 1) & ~(PAGE_16K - 1);

            uint64_t aligned_end =
                reserved_base & ~(PAGE_16K - 1);

            if (aligned_end > aligned_base) {
                uint64_t gap_size = aligned_end - aligned_base;

                if (gap_size > best_size) {
                    best_base = aligned_base;
                    best_size = gap_size;
                }
            }
        }

        // Advance cursor, accounting for overlapping reservations.
        if (reserved_end > cursor)
            cursor = reserved_end;

        if (cursor >= ram_end)
            break;
    }

    // Check the remaining RAM after the final reservation.
    if (cursor < ram_end) {
        uint64_t aligned_base =
            (cursor + PAGE_16K - 1) & ~(PAGE_16K - 1);

        uint64_t aligned_end =
            ram_end & ~(PAGE_16K - 1);

        if (aligned_end > aligned_base) {
            uint64_t gap_size = aligned_end - aligned_base;

            if (gap_size > best_size) {
                best_base = aligned_base;
                best_size = gap_size;
            }
        }
    }

    *base_out = best_base;
    *size_out = best_size;

    return best_size > ONE_GIB;
}

static int segment_contains_entry(const struct payload *p)
{
    for (uint32_t i = 0; i < p->nsegs; i++) {
        uint64_t end;
        const struct payload_seg *s = &p->segs[i];
        if (add_overflow_u64(s->vmaddr, s->vmsize, &end))
            return 0;
        if (p->entry_va >= s->vmaddr && p->entry_va < end)
            return 1;
    }
    return 0;
}

static int validate_payload_segments(const struct payload *p,
                                     uint64_t payload_size,
                                     uint64_t segment_table_end)
{
    if (p->kernel_end <= p->virt_base ||
        p->entry_va < p->virt_base ||
        p->entry_va >= p->kernel_end)
        return 0;

    for (uint32_t i = 0; i < p->nsegs; i++) {
        const struct payload_seg *s = &p->segs[i];
        uint64_t vm_end, file_end;

        if (!s->vmsize || s->file_size > s->vmsize ||
            s->vmaddr < p->virt_base ||
            add_overflow_u64(s->vmaddr, s->vmsize, &vm_end) ||
            vm_end > p->kernel_end)
            return 0;

        if (s->file_off < segment_table_end ||
            add_overflow_u64(s->file_off, s->file_size, &file_end) ||
            file_end > payload_size ||
            file_end > p->adt_off)
            return 0;

        for (uint32_t j = i + 1; j < p->nsegs; j++) {
            uint64_t other_end;
            const struct payload_seg *other = &p->segs[j];
            if (add_overflow_u64(other->vmaddr, other->vmsize, &other_end))
                return 0;
            if (s->vmaddr < other_end && other->vmaddr < vm_end)
                return 0;
        }
    }

    return segment_contains_entry(p);
}

/*
 * Public entry point. Call from EL1 with the runtime FDT address.
 */
__attribute__((noreturn))
void loader_xnu_boot(uintptr_t dtb)
{
    uint64_t initrd_start, initrd_end, initrd_size;
    uint64_t sctlr, current_el;
    uint64_t payload_offset, payload_size, segments_size, segment_table_end;
    uint64_t managed_base, managed_size, managed_end;
    uint64_t kernel_span, adt_pa, adt_end, boot_args_pa, top_of_kernel_data;
    uint64_t entry_pa;
    struct fdt_layout layout;
    struct adt_memory_info adt_memory;
    struct payload *p;
    struct boot_args *args;

    con_skip();
    con_str("XNU", "Preparing kernel handoff");

    /* Do not allow an IRQ/FIQ/SError to interrupt the handoff. */
    __asm__ volatile("msr daifset, #0xf" ::: "memory");

    __asm__ volatile("mrs %0, CurrentEL" : "=r"(current_el));
    if (current_el != 4)
        xnu_error("Expected EL1", current_el);

    __asm__ volatile("mrs %0, SCTLR_EL1" : "=r"(sctlr));
    if (sctlr & ((1ULL << 0) | (1ULL << 2) | (1ULL << 12)))
        xnu_error("MMU/I-cache/D-cache must be disabled", sctlr);

    uint32_t runtime_fdt_size;
    if (!fdt_validate(dtb, &runtime_fdt_size))
        xnu_error("Invalid runtime FDT structure", dtb);

    if (!fdt_parse_memory_layout(dtb, &layout))
        xnu_error("Unable to parse runtime FDT memory map", dtb);

    if (layout.total_size != runtime_fdt_size)
        xnu_error("Runtime FDT size mismatch", layout.total_size);

    if (layout.unresolved_count) {
        con_str("FDT", "Linux dynamic reservation requests present");
        con_hex("Dynamic request count", layout.unresolved_count);
    }

    if (!range_inside((uint64_t)dtb, layout.total_size,
                      layout.dram_base, layout.dram_end))
        xnu_error("Runtime FDT outside actual DRAM", dtb);

    if (!fdt_chosen_u64(dtb, "linux,initrd-start", &initrd_start))
        xnu_error("Missing initrd-start", dtb);
    if (!fdt_chosen_u64(dtb, "linux,initrd-end", &initrd_end))
        xnu_error("Missing initrd-end", dtb);
    if (initrd_end <= initrd_start)
        xnu_error("Invalid initrd range", initrd_start);
    initrd_size = initrd_end - initrd_start;

    if (!range_inside(initrd_start, initrd_size,
                      layout.dram_base, layout.dram_end))
        xnu_error("Initrd outside runtime DRAM", initrd_start);

    /* Reserve both source blobs so relocation cannot overwrite them. */
    if (!fdt_add_range(layout.reserved, &layout.reserved_count,
                       FDT_MAX_RESERVED, initrd_start, initrd_size))
        xnu_error("Too many runtime reservations", layout.reserved_count);
    if (!fdt_add_range(layout.reserved, &layout.reserved_count,
                       FDT_MAX_RESERVED, (uint64_t)dtb,
                       layout.total_size))
        xnu_error("Too many runtime reservations", layout.reserved_count);

    con_str("FDT", "Runtime memory map");
    con_hex("DRAM b", layout.dram_base);
    con_hex("DRAM s", layout.total_ram_size);
    con_hex("DRAM e", layout.dram_end);
    con_hex("ResRanges", layout.reserved_count);
    con_hex("IRD strt", initrd_start);
    con_hex("IRD end", initrd_end);

    p = find_payload((uintptr_t)initrd_start, initrd_size);
    if (!p)
        xnu_error("SWEETXNU payload not found", initrd_start);

    if (p->version != PAYLOAD_VERSION)
        xnu_error("Unsupported payload version", p->version);
    if (!p->nsegs || p->nsegs > PAYLOAD_MAX_SEGS)
        xnu_error("Invalid segment count", p->nsegs);

    payload_offset = (uint64_t)(uintptr_t)p - initrd_start;
    payload_size = initrd_size - payload_offset;
    segments_size = (uint64_t)p->nsegs * sizeof(struct payload_seg);

    if (add_overflow_u64(offsetof(struct payload, segs),
                         segments_size, &segment_table_end) ||
        segment_table_end > payload_size)
        xnu_error("Payload segment table OOB", segment_table_end);

    if (p->adt_size == 0 || p->adt_size > UINT32_MAX ||
        p->adt_off < segment_table_end ||
        p->adt_off > payload_size ||
        p->adt_size > payload_size - p->adt_off ||
        (p->adt_off & 7ULL))
        xnu_error("Invalid ADT payload range", p->adt_off);

    if (!validate_payload_segments(p, payload_size, segment_table_end))
        xnu_error("Invalid kernel segment table/entry", p->entry_va);

    if (!adt_parse_memory_info(
            (const uint8_t *)p + p->adt_off,
            (uint32_t)p->adt_size,
            &adt_memory,
            &layout))
        xnu_error("Malformed ADT or missing DRAM props", p->adt_size);

    if (adt_memory.dram_base != layout.dram_base ||
        adt_memory.dram_size != layout.total_ram_size)
        xnu_error("ADT DRAM values differ from FDT",
                  adt_memory.dram_size);

    /*
     * mkpayload.py obtains these two fields from ADT /chosen/dram-base and
     * dram-size. They must describe the runtime FDT's actual DRAM, not the
     * kernel-managed window and not RAM minus the panic-log reservation.
     */
    if (p->phys_base != layout.dram_base)
        xnu_error("Payload ADT dram-base differs from FDT",
                  p->phys_base);
    if (p->mem_size != layout.total_ram_size)
        xnu_error("Payload ADT dram-size is stale; regenerate ADT",
                  p->mem_size);
        
    con_skip();
    con_str("FDT", "Reserved ranges");


    if (!find_kernel_memory_window(&layout, &managed_base, &managed_size)) {
        con_hex("Largest free base", managed_base);
        con_hex("Largest free size", managed_size);
        //xnu_error("No RAM window > 1 GiB", managed_size);
    }

    if (add_overflow_u64(managed_base, managed_size, &managed_end))
        xnu_error("Managed memory range overflow", managed_base);

    /* The kernel's VA span begins at virt_base, even before the first segment. */
    kernel_span = p->kernel_end - p->virt_base;
    if (kernel_span >= managed_size)
        xnu_error("Kernel image exceeds memory window", kernel_span);

    if (!translate_address(p->virt_base, managed_base,
                           p->kernel_end, &adt_pa))
        xnu_error("ADT destination translation overflow", p->kernel_end);

    if (!range_inside(adt_pa, p->adt_size, managed_base, managed_end))
        xnu_error("ADT destination outside memory", adt_pa);

    if (ranges_overlap(adt_pa, p->adt_size, initrd_start, initrd_size) ||
        ranges_overlap(adt_pa, p->adt_size, dtb, layout.total_size))
        xnu_error("ADT destination overlaps source data", adt_pa);

    if (add_overflow_u64(adt_pa, p->adt_size, &adt_end) ||
        !align_up_checked(adt_end, PAGE_16K, &boot_args_pa))
        xnu_error("boot_args address overflow", adt_pa);

    if (!range_inside(boot_args_pa, PAGE_16K, managed_base, managed_end))
        xnu_error("boot_args outside managed memory", boot_args_pa);
    if (ranges_overlap(boot_args_pa, PAGE_16K, initrd_start, initrd_size) ||
        ranges_overlap(boot_args_pa, PAGE_16K, dtb, layout.total_size))
        xnu_error("boot_args overlaps source data", boot_args_pa);

    if (add_overflow_u64(boot_args_pa, PAGE_16K, &top_of_kernel_data) ||
        top_of_kernel_data > managed_end)
        xnu_error("topOfKernelData outside managed memory",
                  top_of_kernel_data);

    /* Precompute and validate the actual physical entry address. */
    if (!translate_address(p->virt_base, managed_base,
                           p->entry_va, &entry_pa))
        xnu_error("Invalid XNU entry translation", p->entry_va);
    if (!range_inside(entry_pa, 4, managed_base, managed_end))
        xnu_error("XNU entry outside managed memory", entry_pa);

    con_skip();
    con_str("XNU", "Selected kernel memory window");
    con_hex("KBase", managed_base);
    con_hex("KSize", managed_size);
    con_hex("KMemEnd", managed_end);
    con_hex("KSpan", kernel_span);
    con_hex("KEVA", p->entry_va);
    con_hex("KEPA", entry_pa);

    con_skip();
    con_str("XNU", "Copying kernel segments");

    for (uint32_t i = 0; i < p->nsegs; i++) {
        const struct payload_seg *s = &p->segs[i];
        uint64_t destination_pa;
        const void *source;
        void *destination;

        if (!translate_address(p->virt_base, managed_base,
                               s->vmaddr, &destination_pa) ||
            !range_inside(destination_pa, s->vmsize,
                          managed_base, managed_end))
            xnu_error("Segment dest invalid", i);

        if (ranges_overlap(destination_pa, s->vmsize,
                           initrd_start, initrd_size) ||
            ranges_overlap(destination_pa, s->vmsize,
                           dtb, layout.total_size))
            xnu_error("Segment overlaps source data", destination_pa);

        source = (const void *)(uintptr_t)((uintptr_t)p + s->file_off);
        destination = (void *)(uintptr_t)destination_pa;

        copy_bytes(destination, source, s->file_size);
        if (s->vmsize > s->file_size) {
            zero_bytes((void *)(uintptr_t)(destination_pa + s->file_size),
                       s->vmsize - s->file_size);
        }

        con_hex("SEG PA", destination_pa);
        con_hex("SEG size", s->vmsize);
    }

    /* Copy the prepared Apple Device Tree immediately after the kernel. */
    copy_bytes((void *)(uintptr_t)adt_pa,
               (const void *)(uintptr_t)((uintptr_t)p + p->adt_off),
               p->adt_size);

    /* Allocate one zeroed 16-KiB boot-args area after the ADT. */
    args = (struct boot_args *)(uintptr_t)boot_args_pa;
    zero_bytes(args, PAGE_16K);

    args->Revision = 2;
    args->Version = 2;
    args->virtBase = p->virt_base;
    args->physBase = managed_base;
    args->memSize = managed_size;
    args->topOfKernelData = top_of_kernel_data;

    args->Video.v_baseAddr = FB_BASE;
    args->Video.v_display = 1;
    args->Video.v_rowBytes = FB_STRIDE;
    args->Video.v_width = FB_W;
    args->Video.v_height = FB_H;
    args->Video.v_depth = FB_DEPTH;

    /* machineType remains zero until the Angelican XNU platform defines it. */
    if (adt_pa < managed_base)
        xnu_error("ADT is below kernel physBase", adt_pa);
    if (add_overflow_u64(p->virt_base, adt_pa - managed_base,
                         &args->deviceTreeP))
        xnu_error("ADT virtual address overflow", adt_pa);
    args->deviceTreeLength = (uint32_t)p->adt_size;

    copy_bytes(args->CommandLine, p->cmdline, BOOT_LINE_LENGTH - 1);
    args->CommandLine[BOOT_LINE_LENGTH - 1] = '\0';

    args->bootFlags = 0;
    /* XNU 8796 treats memSizeActual as the highest physical address + 1. */
    args->memSizeActual = layout.dram_end;
    
    con_skip();
    con_str("XNU", "Boot arguments / memory map");
    con_hex("BA PA", boot_args_pa);
    con_hex("BA size", sizeof(*args));
    con_hex("vBase", args->virtBase);
    con_hex("pBase", args->physBase);
    con_hex("mSize", args->memSize);
    con_hex("mEnd", args->physBase + args->memSize);
    con_hex("mActual", args->memSizeActual);
    con_hex("kernelTop", args->topOfKernelData);
    con_hex("ADTPoint", args->deviceTreeP);
    con_hex("ADTLen", args->deviceTreeLength);
    con_hex("DRAMb", layout.dram_base);
    con_hex("DRAMs", layout.total_ram_size);
    con_hex("DRAMe", layout.dram_end);
    con_hex("IRD strt", initrd_start);
    con_hex("IRD end", initrd_end);
    con_hex("ADT PA", adt_pa);
    con_hex("ADT end", adt_end);

    /* MMU and I/D caches are already disabled; ensure all writes are visible. */
    __asm__ volatile(
        "dsb sy\n"
        "ic iallu\n"
        "dsb sy\n"
        "isb\n"
        ::: "memory"
    );

    __asm__ volatile("mrs %0, SCTLR_EL1" : "=r"(sctlr));
    if (sctlr & ((1ULL << 0) | (1ULL << 2) | (1ULL << 12)))
        xnu_error("MMU/cache state changed before branch", sctlr);

    con_hex("CurrentEL", current_el);
    con_hex("SCTLR_EL1", sctlr);
    con_hex("Entry PA", entry_pa);
    con_hex("boot_args PA", boot_args_pa);
    con_hex("Entry offset", p->entry_va - p->virt_base);
    con_str("XNU", "Branching to kernel");

    /* x0 = physical boot_args pointer; PC = physical __start. */
    __asm__ volatile(
        "mov x0, %0\n"
        "br  %1\n"
        :
        : "r"(boot_args_pa), "r"(entry_pa)
        : "x0", "memory"
    );

    con_str("XNU", "Returned");

    __builtin_unreachable();
}
