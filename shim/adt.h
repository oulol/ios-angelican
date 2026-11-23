#include <stdint.h>
#include "fdt.h"

#define ADT_MAX_DEPTH 16U

struct adt_memory_info {
    uint64_t dram_base;
    uint64_t dram_size;
    uint8_t have_dram_base;
    uint8_t have_dram_size;
};

uint32_t adt_le32(const uint8_t *p);
uint64_t adt_le64(const uint8_t *p);
int adt_fixed_name_equal(const uint8_t name[32], const char *text);
int adt_walk_node(const uint8_t *blob, uint32_t blob_size,
                         uint32_t *offset, uint32_t depth,
                         struct adt_memory_info *info,
                         struct fdt_layout *layout);
int adt_parse_memory_info(const uint8_t *blob,
                          uint32_t blob_size,
                          struct adt_memory_info *info,
                          struct fdt_layout *layout);