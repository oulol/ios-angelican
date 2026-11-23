#include <stdint.h>
#include "fdt.h"

#define PAGE_16K 0x4000ULL
#define ONE_GIB  0x40000000ULL

int add_overflow_u64(uint64_t a, uint64_t b, uint64_t *result);

int range_inside(uint64_t address, uint64_t size,
                 uint64_t start, uint64_t end);

int ranges_overlap(uint64_t a, uint64_t a_size,
                   uint64_t b, uint64_t b_size);

int align_up_checked(uint64_t value, uint64_t alignment,
                     uint64_t *result);

uint64_t align_down(uint64_t value, uint64_t alignment);

int translate_address(uint64_t virt_base, uint64_t phys_base,
                      uint64_t virtual_address,
                      uint64_t *physical_address);

int find_kernel_memory_window(fdt_layout_t *layout,
                              uint64_t *base_out,
                              uint64_t *size_out);
