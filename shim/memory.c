#include "memory.h"

int add_overflow_u64(uint64_t a, uint64_t b, uint64_t *result)
{
    *result = a + b;
    return *result < a;
}

int range_end(const struct phys_range *range, uint64_t *end)
{
    if (!range->size)
        return 0;
    return !add_overflow_u64(range->base, range->size, end);
}

int range_inside(uint64_t address, uint64_t size,
                        uint64_t start, uint64_t end)
{
    uint64_t range_end_value;
    if (add_overflow_u64(address, size, &range_end_value))
        return 0;
    return address >= start && range_end_value <= end;
}

int ranges_overlap(uint64_t a, uint64_t a_size,
                          uint64_t b, uint64_t b_size)
{
    uint64_t a_end, b_end;
    if (add_overflow_u64(a, a_size, &a_end) ||
        add_overflow_u64(b, b_size, &b_end))
        return 1;
    return a < b_end && b < a_end;
}

int align_up_checked(uint64_t value, uint64_t alignment,
                            uint64_t *result)
{
    uint64_t add = alignment - 1;
    if (value > UINT64_MAX - add)
        return 0;
    *result = (value + add) & ~add;
    return 1;
}

uint64_t align_down(uint64_t value, uint64_t alignment)
{
    return value & ~(alignment - 1);
}

int translate_address(uint64_t virt_base, uint64_t phys_base,
                             uint64_t virtual_address,
                             uint64_t *physical_address)
{
    if (virtual_address < virt_base)
        return 0;
    return !add_overflow_u64(phys_base,
                             virtual_address - virt_base,
                             physical_address);
}

void sort_ranges(struct phys_range *ranges, uint32_t count)
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

int find_kernel_memory_window(
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
