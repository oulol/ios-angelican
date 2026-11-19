#include "fdt.h"

#include <stdint.h>
#include <limits.h>

#define FDT_MAGIC       0xd00dfeedU

#define FDT_BEGIN_NODE  1U
#define FDT_END_NODE    2U
#define FDT_PROP        3U
#define FDT_NOP         4U
#define FDT_END         9U

#define FDT_HEADER_SIZE 40U

static uint32_t read_be32(uintptr_t address)
{
    volatile const uint8_t *p =
        (volatile const uint8_t *)address;

    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           ((uint32_t)p[3]);
}

static uint64_t read_be64(uintptr_t address)
{
    return ((uint64_t)read_be32(address) << 32) |
           read_be32(address + 4);
}

static int range_valid(uint32_t offset, uint32_t length, uint32_t total)
{
    return offset <= total && length <= total - offset;
}

static uint32_t align4(uint32_t value)
{
    return (value + 3U) & ~3U;
}

static int string_equal(
    uintptr_t address,
    uint32_t available,
    const char *expected
)
{
    volatile const char *p =
        (volatile const char *)address;

    uint32_t i = 0;

    while (i < available && expected[i] && p[i] == expected[i]) {
        i++;
    }

    return i < available &&
           p[i] == '\0' &&
           expected[i] == '\0';
}

int fdt_validate(uintptr_t fdt, uint32_t *total_size)
{
    if (!fdt || fdt > UINTPTR_MAX - FDT_HEADER_SIZE)
        return 0;

    if (read_be32(fdt) != FDT_MAGIC)
        return 0;

    uint32_t total = read_be32(fdt + 4);

    if (total < FDT_HEADER_SIZE)
        return 0;

    if (fdt > UINTPTR_MAX - total)
        return 0;

    uint32_t struct_offset = read_be32(fdt + 8);
    uint32_t strings_offset = read_be32(fdt + 12);

    uint32_t version = read_be32(fdt + 20);

    if (version < 16)
        return 0;

    uint32_t strings_size = read_be32(fdt + 32);
    uint32_t struct_size = read_be32(fdt + 36);

    if ((struct_offset & 3U) != 0)
        return 0;

    if (!range_valid(struct_offset, struct_size, total))
        return 0;

    if (!range_valid(strings_offset, strings_size, total))
        return 0;

    if (struct_size < 4)
        return 0;

    if (total_size)
        *total_size = total;

    return 1;
}

int fdt_get_chosen_property(
    uintptr_t fdt,
    const char *name,
    uintptr_t *value,
    uint32_t *length
)
{
    uint32_t total;

    if (!name || !value || !length)
        return 0;

    if (!fdt_validate(fdt, &total))
        return 0;

    uint32_t struct_offset = read_be32(fdt + 8);
    uint32_t strings_offset = read_be32(fdt + 12);

    uint32_t strings_size = read_be32(fdt + 32);
    uint32_t struct_size = read_be32(fdt + 36);

    uint32_t pos = struct_offset;
    uint32_t end = struct_offset + struct_size;

    uint32_t depth = 0;
    int in_chosen = 0;

    while (pos <= end && end - pos >= 4) {
        uint32_t token = read_be32(fdt + pos);
        pos += 4;

        switch (token) {
        case FDT_BEGIN_NODE: {
            uint32_t name_start = pos;

            while (pos < end &&
                   *(volatile const uint8_t *)(fdt + pos) != 0) {
                pos++;
            }

            if (pos >= end)
                return 0;

            uint32_t name_length = pos - name_start;

            depth++;

            if (depth == 2) {
                in_chosen =
                    name_length == 6 &&
                    string_equal(
                        fdt + name_start,
                        name_length + 1,
                        "chosen"
                    );
            }

            pos++;

            if (pos > UINT32_MAX - 3)
                return 0;

            pos = align4(pos);

            if (pos > end)
                return 0;

            break;
        }

        case FDT_END_NODE:
            if (depth == 0)
                return 0;

            if (depth == 2)
                in_chosen = 0;

            depth--;
            break;

        case FDT_PROP: {
            if (depth == 0 || end - pos < 8)
                return 0;

            uint32_t prop_length = read_be32(fdt + pos);
            uint32_t name_offset = read_be32(fdt + pos + 4);

            pos += 8;

            if (!range_valid(pos, prop_length, end))
                return 0;

            if (name_offset >= strings_size)
                return 0;

            uint32_t name_available =
                strings_size - name_offset;

            if (in_chosen && depth == 2 &&
                string_equal(
                    fdt + strings_offset + name_offset,
                    name_available,
                    name
                )) {

                *value = fdt + pos;
                *length = prop_length;

                return 1;
            }

            if (prop_length > UINT32_MAX - 3)
                return 0;

            uint32_t aligned_length = align4(prop_length);

            if (!range_valid(pos, aligned_length, end))
                return 0;

            pos += aligned_length;
            break;
        }

        case FDT_NOP:
            break;

        case FDT_END:
            return 0;

        default:
            return 0;
        }
    }

    return 0;
}

int fdt_chosen_u64(
    uintptr_t fdt,
    const char *name,
    uint64_t *out
)
{
    uintptr_t value;
    uint32_t length;

    if (!out)
        return 0;

    if (!fdt_get_chosen_property(
            fdt,
            name,
            &value,
            &length
        )) {
        return 0;
    }

    if (length == 4) {
        *out = read_be32(value);
        return 1;
    }

    if (length == 8) {
        *out = read_be64(value);
        return 1;
    }

    return 0;
}