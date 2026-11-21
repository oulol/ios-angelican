#pragma once

#include <stdint.h>

#define FDT_MAX_MEMORY_RANGES 16u
#define FDT_MAX_RESERVED      160u

typedef struct phys_range {
    uint64_t base;
    uint64_t size;
} phys_range_t;

typedef struct fdt_layout {
    phys_range_t memory[FDT_MAX_MEMORY_RANGES];
    uint32_t memory_count;

    phys_range_t reserved[FDT_MAX_RESERVED];
    uint32_t reserved_count;

    uint32_t unresolved_count;
    uint64_t total_ram_size;
    uint64_t dram_base;
    uint64_t dram_end;
    uint32_t total_size;
} fdt_layout_t;

int fdt_validate(uintptr_t fdt, uint32_t *total_size);

int fdt_get_chosen_property(
    uintptr_t fdt,
    const char *name,
    uintptr_t *value,
    uint32_t *length
);

int fdt_chosen_u64(
    uintptr_t fdt,
    const char *name,
    uint64_t *out
);

/* Parse actual RAM and reserved ranges from the runtime Linux FDT. */
int fdt_parse_memory_layout(uintptr_t fdt, fdt_layout_t *layout);

/* Add an extra range to a layout's reserved list (initrd, FDT, ADT, etc.). */
int fdt_add_range(
    phys_range_t *ranges,
    uint32_t *count,
    uint32_t capacity,
    uint64_t base,
    uint64_t size
);

void fdt_dump_memory(uintptr_t fdt);
