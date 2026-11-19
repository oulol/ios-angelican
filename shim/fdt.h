#pragma once

#include <stdint.h>

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