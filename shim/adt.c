#include "adt.h"
#include <stddef.h>
uint32_t adt_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

uint64_t adt_le64(const uint8_t *p)
{
    return (uint64_t)adt_le32(p) | ((uint64_t)adt_le32(p + 4) << 32);
}

int adt_fixed_name_equal(const uint8_t name[32], const char *text)
{
    uint32_t i = 0;
    while (text[i]) {
        if (i >= 32 || name[i] != (uint8_t)text[i])
            return 0;
        i++;
    }
    return i < 32 && name[i] == 0;
}

int adt_value_equal(const uint8_t *value, uint32_t length,
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

int adt_walk_node(const uint8_t *blob, uint32_t blob_size,
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


int adt_parse_memory_info(const uint8_t *blob, uint32_t blob_size,
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