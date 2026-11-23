/*
 * Freestanding flattened-device-tree parser for the Angelican boot shim.
 *
 * No libfdt, libc, malloc, memcpy, or memset required.
 */
#include "fdt.h"
#include "console.h"

#include <stddef.h>

#define FDT_MAGIC       0xd00dfeedu
#define FDT_BEGIN_NODE  1u
#define FDT_END_NODE    2u
#define FDT_PROP        3u
#define FDT_NOP         4u
#define FDT_END         9u
#define FDT_HEADER_SIZE 40u
#define FDT_MAX_DEPTH   32u

struct fdt_view {
    const uint8_t *blob;
    const uint8_t *structure;
    const uint8_t *strings;
    uint32_t total_size;
    uint32_t structure_size;
    uint32_t strings_size;
    uint32_t reserve_offset;
    uint32_t structure_offset;
};

struct fdt_node_state {
    const uint8_t *name;
    uint32_t name_length;
    uint32_t address_cells;
    uint32_t size_cells;
    uint32_t parent_address_cells;
    uint32_t parent_size_cells;
    const uint8_t *reg_data;
    uint32_t reg_length;
    uint8_t is_root;
    uint8_t is_memory;
    uint8_t is_reserved_root;
    uint8_t is_reserved_child;
    uint8_t has_reg;
    uint8_t has_dynamic_request;
    uint8_t disabled;
    uint8_t ranges_nonempty;
};

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

static uint64_t be64(const uint8_t *p)
{
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

static uint32_t align4_u32(uint32_t value)
{
    return (value + 3u) & ~3u;
}

static int bytes_name_equal(const uint8_t *p, uint32_t length,
                            const char *text)
{
    uint32_t i = 0;

    while (text[i]) {
        if (i >= length || p[i] != (uint8_t)text[i])
            return 0;
        i++;
    }

    return i == length;
}

static int bytes_value_equal(const uint8_t *p, uint32_t length,
                             const char *text)
{
    uint32_t i = 0;

    while (text[i]) {
        if (i >= length || p[i] != (uint8_t)text[i])
            return 0;
        i++;
    }

    return i < length && p[i] == 0;
}

static int bytes_cstr_equal(const uint8_t *p, uint32_t available,
                            const char *text)
{
    uint32_t i = 0;

    while (text[i]) {
        if (i >= available || p[i] != (uint8_t)text[i])
            return 0;
        i++;
    }

    return i < available && p[i] == 0;
}

static int node_name_is_memory(const uint8_t *name, uint32_t length)
{
    static const char prefix[] = "memory";

    for (uint32_t i = 0; i < sizeof(prefix) - 1; i++) {
        if (i >= length || name[i] != (uint8_t)prefix[i])
            return 0;
    }

    return length == sizeof(prefix) - 1 ||
           name[sizeof(prefix) - 1] == '@';
}

static int open_fdt(uintptr_t address, struct fdt_view *view)
{
    const uint8_t *blob = (const uint8_t *)address;
    uint32_t total_size, off_struct, off_strings, off_reserve;
    uint32_t size_strings, size_struct;

    if (!address || (address & 7u) || be32(blob) != FDT_MAGIC)
        return 0;

    total_size = be32(blob + 4);
    off_struct = be32(blob + 8);
    off_strings = be32(blob + 12);
    off_reserve = be32(blob + 16);
    size_strings = be32(blob + 32);
    size_struct = be32(blob + 36);

    if (total_size < FDT_HEADER_SIZE ||
        off_struct < FDT_HEADER_SIZE || off_struct > total_size ||
        off_strings < FDT_HEADER_SIZE || off_strings > total_size ||
        off_reserve < FDT_HEADER_SIZE || off_reserve > total_size ||
        (off_struct & 3u) || (off_strings & 3u) || (off_reserve & 7u))
        return 0;

    /* For old/vendor headers with a zero structure size, use the next block. */
    if (!size_struct) {
        if (off_strings <= off_struct)
            return 0;
        size_struct = off_strings - off_struct;
    }
    if (!size_strings)
        size_strings = total_size - off_strings;

    if (size_struct > total_size - off_struct ||
        size_strings > total_size - off_strings)
        return 0;

    view->blob = blob;
    view->structure = blob + off_struct;
    view->strings = blob + off_strings;
    view->total_size = total_size;
    view->structure_size = size_struct;
    view->strings_size = size_strings;
    view->reserve_offset = off_reserve;
    view->structure_offset = off_struct;
    return 1;
}

static int validate_reserve_map(const struct fdt_view *view)
{
    uint32_t off = view->reserve_offset;

    /* The terminator must occur before the structure block begins. */
    while (off <= view->structure_offset &&
           view->structure_offset - off >= 16u) {
        uint64_t base = be64(view->blob + off);
        uint64_t size = be64(view->blob + off + 8);
        off += 16;

        if (!base && !size)
            return 1;
    }

    return 0;
}

static int validate_structure(const struct fdt_view *view)
{
    uint32_t pos = 0;
    uint32_t depth = 0;

    while (pos <= view->structure_size &&
           view->structure_size - pos >= 4u) {
        uint32_t token = be32(view->structure + pos);
        pos += 4;

        if (token == FDT_BEGIN_NODE) {
            while (pos < view->structure_size && view->structure[pos] != 0)
                pos++;
            if (pos >= view->structure_size || depth >= FDT_MAX_DEPTH)
                return 0;
            pos = align4_u32(pos + 1u);
            if (pos > view->structure_size)
                return 0;
            depth++;
            continue;
        }

        if (token == FDT_END_NODE) {
            if (!depth)
                return 0;
            depth--;
            continue;
        }

        if (token == FDT_PROP) {
            uint32_t length, nameoff;
            uint32_t i;

            if (!depth || view->structure_size - pos < 8u)
                return 0;

            length = be32(view->structure + pos);
            nameoff = be32(view->structure + pos + 4);
            pos += 8;

            if (length > view->structure_size - pos ||
                nameoff >= view->strings_size)
                return 0;

            for (i = nameoff; i < view->strings_size; i++) {
                if (view->strings[i] == 0)
                    break;
            }
            if (i == view->strings_size)
                return 0;

            pos = align4_u32(pos + length);
            if (pos > view->structure_size)
                return 0;
            continue;
        }

        if (token == FDT_NOP)
            continue;

        if (token == FDT_END)
            return depth == 0;

        return 0;
    }

    return 0;
}

int fdt_validate(uintptr_t fdt, uint32_t *total_size)
{
    struct fdt_view view;

    if (!open_fdt(fdt, &view)) {
        con_str("FDT chk", "header/block bounds failed");
        return 0;
    }
    if (!validate_reserve_map(&view)) {
        con_str("FDT chk", "reservation map is malformed");
        con_hex("RES off", view.reserve_offset);
        con_hex("STR off", view.structure_offset);
        return 0;
    }
    if (!validate_structure(&view)) {
        con_str("FDT chk", "structure tokens are malformed");
        return 0;
    }

    if (total_size)
        *total_size = view.total_size;
    return 1;
}

int fdt_get_chosen_property(uintptr_t fdt, const char *name,
                            uintptr_t *value, uint32_t *length)
{
    struct fdt_view view;
    uint8_t chosen[FDT_MAX_DEPTH];
    uint32_t pos = 0;
    uint32_t depth = 0;

    if (!name || !value || !length || !open_fdt(fdt, &view))
        return 0;

    while (pos <= view.structure_size &&
           view.structure_size - pos >= 4u) {
        uint32_t token = be32(view.structure + pos);
        pos += 4;

        if (token == FDT_BEGIN_NODE) {
            uint32_t name_start = pos;
            uint32_t name_length;

            while (pos < view.structure_size && view.structure[pos] != 0)
                pos++;
            if (pos >= view.structure_size || depth >= FDT_MAX_DEPTH)
                return 0;

            name_length = pos - name_start;
            chosen[depth] = (uint8_t)(depth == 1 &&
                bytes_name_equal(view.structure + name_start,
                                 name_length, "chosen"));
            pos = align4_u32(pos + 1u);
            if (pos > view.structure_size)
                return 0;
            depth++;
            continue;
        }

        if (token == FDT_END_NODE) {
            if (!depth)
                return 0;
            depth--;
            continue;
        }

        if (token == FDT_PROP) {
            uint32_t prop_length, nameoff;
            uintptr_t prop_value;

            if (!depth || view.structure_size - pos < 8u)
                return 0;

            prop_length = be32(view.structure + pos);
            nameoff = be32(view.structure + pos + 4);
            pos += 8;
            prop_value = (uintptr_t)(view.structure + pos);

            if (prop_length > view.structure_size - pos ||
                nameoff >= view.strings_size)
                return 0;

            if (chosen[depth - 1] &&
                bytes_cstr_equal(view.strings + nameoff,
                                 view.strings_size - nameoff, name)) {
                *value = prop_value;
                *length = prop_length;
                return 1;
            }

            pos = align4_u32(pos + prop_length);
            if (pos > view.structure_size)
                return 0;
            continue;
        }

        if (token == FDT_NOP)
            continue;
        if (token == FDT_END)
            return 0;
        return 0;
    }

    return 0;
}

int fdt_chosen_u64(uintptr_t fdt, const char *name, uint64_t *out)
{
    uintptr_t value;
    uint32_t length;

    if (!out || !fdt_get_chosen_property(fdt, name, &value, &length))
        return 0;

    if (length == 4)
        *out = be32((const uint8_t *)value);
    else if (length == 8)
        *out = be64((const uint8_t *)value);
    else
        return 0;

    return 1;
}

int fdt_add_range(phys_range_t *ranges, uint32_t *count,
                  uint32_t capacity, uint64_t base, uint64_t size)
{
    uint64_t end;

    if (!size)
        return 1;
    if (base + size < base)
        return 0;
    end = base + size;
    (void)end;

    /* Avoid duplicate entries from overlapping FDT reservation mechanisms. */
    for (uint32_t i = 0; i < *count; i++) {
        if (ranges[i].base == base && ranges[i].size == size)
            return 1;
    }

    if (*count >= capacity)
        return 0;

    ranges[*count].base = base;
    ranges[*count].size = size;
    (*count)++;
    return 1;
}

static int read_cells(const uint8_t *p, uint32_t cells, uint64_t *value)
{
    if (cells == 1) {
        *value = be32(p);
        return 1;
    }
    if (cells == 2) {
        *value = be64(p);
        return 1;
    }
    return 0;
}

static int parse_reg(const uint8_t *data, uint32_t length,
                     uint32_t address_cells, uint32_t size_cells,
                     phys_range_t *ranges, uint32_t *count,
                     uint32_t capacity)
{
    uint32_t tuple_cells = address_cells + size_cells;
    uint32_t tuple_size;

    if (!address_cells || !size_cells ||
        address_cells > 2 || size_cells > 2)
        return 0;

    tuple_size = tuple_cells * 4u;
    if (!tuple_size || (length % tuple_size))
        return 0;

    for (uint32_t off = 0; off < length; off += tuple_size) {
        uint64_t base, size;

        if (!read_cells(data + off, address_cells, &base) ||
            !read_cells(data + off + address_cells * 4u,
                        size_cells, &size))
            return 0;

        if (!fdt_add_range(ranges, count, capacity, base, size))
            return 0;
    }

    return 1;
}

static int parse_reservation_map(const struct fdt_view *view,
                                 fdt_layout_t *layout)
{
    uint32_t off = view->reserve_offset;

    while (off <= view->structure_offset &&
           view->structure_offset - off >= 16u) {
        uint64_t base = be64(view->blob + off);
        uint64_t size = be64(view->blob + off + 8u);
        off += 16u;

        if (!base && !size)
            return 1;

        if (!fdt_add_range(layout->reserved, &layout->reserved_count,
                           FDT_MAX_RESERVED, base, size))
            return 0;
    }

    return 0;
}

int fdt_parse_memory_layout(uintptr_t fdt_address, fdt_layout_t *layout)
{
    struct fdt_view view;
    struct fdt_node_state stack[FDT_MAX_DEPTH];
    uint32_t pos = 0;
    uint32_t depth = 0;
    uint32_t last_token = 0;
    int saw_end = 0;
    int reserved_ranges_unsupported = 0;

    if (!layout) {
        con_str("FDT parse", "NULL layout output");
        return 0;
    }
    if (!open_fdt(fdt_address, &view)) {
        con_str("FDT parse", "open_fdt/header failed");
        return 0;
    }

    layout->memory_count = 0;
    layout->reserved_count = 0;
    layout->unresolved_count = 0;
    layout->total_ram_size = 0;
    layout->dram_base = UINT64_MAX;
    layout->dram_end = 0;
    layout->total_size = view.total_size;

    if (!parse_reservation_map(&view, layout)) {
        con_str("FDT parse", "reservation-map parse failed");
        return 0;
    }

    while (pos <= view.structure_size &&
           view.structure_size - pos >= 4u) {
        uint32_t token = be32(view.structure + pos);
        uint32_t token_pos = pos;
        pos += 4u;
        last_token = token;

        if (token == FDT_BEGIN_NODE) {
            uint32_t name_start = pos;
            struct fdt_node_state *node;
            struct fdt_node_state *parent = depth ? &stack[depth - 1] : 0;

            if (depth >= FDT_MAX_DEPTH) {
                con_str("FDT parse", "node depth limit exceeded");
                con_hex("TOK off", token_pos);
                return 0;
            }

            while (pos < view.structure_size && view.structure[pos] != 0)
                pos++;
            if (pos >= view.structure_size) {
                con_str("FDT parse", "unterminated node name");
                con_hex("TOK off", token_pos);
                return 0;
            }

            node = &stack[depth];
            node->name = view.structure + name_start;
            node->name_length = pos - name_start;
            node->parent_address_cells = parent ? parent->address_cells : 2;
            node->parent_size_cells = parent ? parent->size_cells : 1;
            node->address_cells = node->parent_address_cells;
            node->size_cells = node->parent_size_cells;
            node->reg_data = 0;
            node->reg_length = 0;
            node->is_root = (uint8_t)(depth == 0);
            node->is_memory = (uint8_t)(depth == 1 &&
                node_name_is_memory(node->name, node->name_length));
            node->is_reserved_root = (uint8_t)(depth == 1 &&
                bytes_name_equal(node->name, node->name_length,
                                 "reserved-memory"));
            node->is_reserved_child = (uint8_t)(parent &&
                                                parent->is_reserved_root);
            node->has_reg = 0;
            node->has_dynamic_request = 0;
            node->disabled = 0;
            node->ranges_nonempty = 0;

            pos = align4_u32(pos + 1u);
            if (pos > view.structure_size) {
                con_str("FDT parse", "node-name alignment out of bounds");
                con_hex("TOK off", token_pos);
                return 0;
            }
            depth++;
            continue;
        }

        if (token == FDT_END_NODE) {
            struct fdt_node_state *node;

            if (!depth) {
                con_str("FDT parse", "END_NODE at depth zero");
                con_hex("TOK off", token_pos);
                return 0;
            }
            node = &stack[depth - 1];

            if (node->is_memory && !node->disabled) {
                if (!node->has_reg) {
                    con_str("FDT parse", "memory node has no reg");
                    con_str("Node", (const char *)node->name);
                    return 0;
                }
                if (!parse_reg(node->reg_data, node->reg_length,
                               node->parent_address_cells,
                               node->parent_size_cells,
                               layout->memory,
                               &layout->memory_count,
                               FDT_MAX_MEMORY_RANGES)) {
                    con_str("FDT parse", "invalid memory reg property");
                    con_str("Node", (const char *)node->name);
                    con_hex("REG len", node->reg_length);
                    con_hex("ADDR cel", node->parent_address_cells);
                    con_hex("SIZE cel", node->parent_size_cells);
                    return 0;
                }
            }

            if (node->is_reserved_root && node->ranges_nonempty) {
                reserved_ranges_unsupported = 1;
                con_str("FDT parse", "reserved-memory ranges is nonempty");
                con_hex("RNG len", node->reg_length);
            }

            if (node->is_reserved_child && !node->disabled) {
                if (node->has_reg) {
                    if (!parse_reg(node->reg_data, node->reg_length,
                                   node->parent_address_cells,
                                   node->parent_size_cells,
                                   layout->reserved,
                                   &layout->reserved_count,
                                   FDT_MAX_RESERVED)) {
                        con_str("FDT parse", "invalid reserved-memory reg");
                        con_str("Node", (const char *)node->name);
                        con_hex("REG len", node->reg_length);
                        con_hex("ADDR cel", node->parent_address_cells);
                        con_hex("SIZE cel", node->parent_size_cells);
                        return 0;
                    }
                } else if (node->has_dynamic_request) {
                    layout->unresolved_count++;
                    con_str("FDT unresolved reservation",
                            (const char *)node->name);
                }
            }

            depth--;
            continue;
        }

        if (token == FDT_NOP)
            continue;

        if (token == FDT_END) {
            if (depth != 0) {
                con_str("FDT parse", "END token with unclosed nodes");
                con_hex("Depth", depth);
                con_hex("TOK off", token_pos);
                return 0;
            }
            saw_end = 1;
            break;
        }

        if (token != FDT_PROP || !depth ||
            view.structure_size - pos < 8u) {
            con_str("FDT parse", "unexpected token or truncated property");
            con_hex("FDT token", token);
            con_hex("TOK off", token_pos);
            con_hex("Depth", depth);
            return 0;
        }

        {
            uint32_t length = be32(view.structure + pos);
            uint32_t nameoff = be32(view.structure + pos + 4u);
            const uint8_t *propname;
            const uint8_t *value;
            struct fdt_node_state *node = &stack[depth - 1];

            pos += 8u;
            if (length > view.structure_size - pos ||
                nameoff >= view.strings_size) {
                con_str("FDT parse", "property outside block bounds");
                con_hex("PROP len", length);
                con_hex("NAME off", nameoff);
                con_hex("PROP off", token_pos);
                return 0;
            }

            propname = view.strings + nameoff;
            value = view.structure + pos;

            if (bytes_cstr_equal(propname,
                                 view.strings_size - nameoff,
                                 "#address-cells")) {
                if (length != 4u) {
                    con_str("FDT parse", "bad #address-cells length");
                    return 0;
                }
                node->address_cells = be32(value);
                /* Zero is valid on non-memory buses in a general FDT.
                 * parse_reg() rejects zero when a physical range actually
                 * requires an address cell count.
                 */
                if (node->address_cells > 2) {
                    con_str("FDT parse", "unsupported #address-cells value");
                    con_hex("#address-cells", node->address_cells);
                    return 0;
                }
            } else if (bytes_cstr_equal(propname,
                                        view.strings_size - nameoff,
                                        "#size-cells")) {
                if (length != 4u) {
                    con_str("FDT parse", "bad #size-cells length");
                    return 0;
                }
                node->size_cells = be32(value);
                /* #size-cells = <0> is explicitly valid in Device Tree.
                 * It is only rejected if a RAM/reservation reg tuple uses it.
                 */
                if (node->size_cells > 2) {
                    con_str("FDT parse", "unsupported #size-cells value");
                    con_hex("#size-cells", node->size_cells);
                    return 0;
                }
            } else if (bytes_cstr_equal(propname,
                                        view.strings_size - nameoff,
                                        "device_type")) {
                if (!node->is_root && depth == 2 &&
                    bytes_value_equal(value, length, "memory"))
                    node->is_memory = 1;
            } else if (bytes_cstr_equal(propname,
                                        view.strings_size - nameoff,
                                        "reg")) {
                node->reg_data = value;
                node->reg_length = length;
                node->has_reg = 1;
            } else if (node->is_reserved_child &&
                       bytes_cstr_equal(propname,
                                        view.strings_size - nameoff,
                                        "size")) {
                uint64_t requested;
                if (!read_cells(value, node->parent_size_cells, &requested)) {
                    con_str("FDT parse", "bad reserved size property");
                    con_str("Node", (const char *)node->name);
                    return 0;
                }
                if (requested)
                    node->has_dynamic_request = 1;
            } else if (node->is_reserved_child &&
                       bytes_cstr_equal(propname,
                                        view.strings_size - nameoff,
                                        "alloc-ranges")) {
                if (length)
                    node->has_dynamic_request = 1;
            } else if (node->is_reserved_child &&
                       bytes_cstr_equal(propname,
                                        view.strings_size - nameoff,
                                        "status")) {
                if (bytes_value_equal(value, length, "disabled"))
                    node->disabled = 1;
            } else if (node->is_reserved_root &&
                       bytes_cstr_equal(propname,
                                        view.strings_size - nameoff,
                                        "ranges")) {
                if (length != 0) {
                    node->ranges_nonempty = 1;
                    node->reg_length = length;
                }
            }

            pos = align4_u32(pos + length);
            if (pos > view.structure_size) {
                con_str("FDT parse", "property alignment out of bounds");
                con_hex("PROP off", token_pos);
                return 0;
            }
        }
    }

    if (!saw_end) {
        con_str("FDT parse", "missing FDT_END token");
        con_hex("FDT end", pos);
        con_hex("STR size", view.structure_size);
        con_hex("TOK last", last_token);
        return 0;
    }
    if (depth != 0) {
        con_str("FDT parse", "nonzero depth at end");
        con_hex("Depth", depth);
        return 0;
    }
    if (reserved_ranges_unsupported) {
        con_str("FDT parse", "nonempty reserved-memory ranges unsupported");
        return 0;
    }
    if (layout->memory_count == 0) {
        con_str("FDT parse", "no /memory reg ranges found");
        return 0;
    }

    for (uint32_t i = 0; i < layout->memory_count; i++) {
        uint64_t end;
        if (layout->memory[i].size == 0 ||
            layout->memory[i].base + layout->memory[i].size <
                layout->memory[i].base) {
            con_str("FDT parse", "invalid RAM range size/overflow");
            con_hex("RAM base", layout->memory[i].base);
            con_hex("RAM size", layout->memory[i].size);
            return 0;
        }
        end = layout->memory[i].base + layout->memory[i].size;
        if (layout->memory[i].base < layout->dram_base)
            layout->dram_base = layout->memory[i].base;
        if (end > layout->dram_end)
            layout->dram_end = end;
        if (layout->total_ram_size > UINT64_MAX - layout->memory[i].size) {
            con_str("FDT parse", "total RAM size overflow");
            return 0;
        }
        layout->total_ram_size += layout->memory[i].size;
    }

    con_str("FDT parse", "memory map parsed successfully");
    con_hex("REG cnt", layout->memory_count);
    con_hex("RES cnt", layout->reserved_count);
    con_hex("UNRES", layout->unresolved_count);
    return 1;
}


static void fdt_write_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static int fdt_store_u64(uintptr_t property,
                         uint32_t length,
                         uint64_t value)
{
    uint8_t *p = (uint8_t *)property;

    if (length == 4) {
        if (value > UINT32_MAX)
            return 0;

        fdt_write_be32(p, (uint32_t)value);
        return 1;
    }

    if (length == 8) {
        fdt_write_be32(p, (uint32_t)(value >> 32));
        fdt_write_be32(p + 4, (uint32_t)value);
        return 1;
    }

    return 0;
}

int fdt_set_initrd(uintptr_t fdt,
                   uint64_t start,
                   uint64_t size)
{
    uintptr_t start_prop;
    uintptr_t end_prop;
    uint32_t start_len;
    uint32_t end_len;
    uint64_t end;

    if (!size || start > UINT64_MAX - size)
        return 0;

    end = start + size;

    if (!fdt_get_chosen_property(
            fdt, "linux,initrd-start",
            &start_prop, &start_len))
        return 0;

    if (!fdt_get_chosen_property(
            fdt, "linux,initrd-end",
            &end_prop, &end_len))
        return 0;

    /* Validate both before modifying either property. */
    if ((start_len != 4 && start_len != 8) ||
        (end_len != 4 && end_len != 8))
        return 0;

    if ((start_len == 4 && start > UINT32_MAX) ||
        (end_len == 4 && end > UINT32_MAX))
        return 0;

    fdt_store_u64(start_prop, start_len, start);
    fdt_store_u64(end_prop, end_len, end);

    return 1;
}


int fdt_get_initrd(uintptr_t fdt,
                   uintptr_t *initrd_start,
                   uint64_t *initrd_size)
{
    uint64_t start;
    uint64_t end;

    if (!initrd_start || !initrd_size)
        return 0;

    if (!fdt_chosen_u64(fdt, "linux,initrd-start", &start))
        return 0;

    if (!fdt_chosen_u64(fdt, "linux,initrd-end", &end))
        return 0;

    if (end <= start || start > UINTPTR_MAX)
        return 0;

    *initrd_start = (uintptr_t)start;
    *initrd_size = end - start;

    return 1;
}

void fdt_dump_memory(uintptr_t fdt)
{
    fdt_layout_t layout;

    if (!fdt_parse_memory_layout(fdt, &layout)) {
        con_str("FDT", "runtime memory map parse failed");
        con_hex("FDT addr", fdt);
        return;
    }

    con_str("FDT", "runtime memory map");
    con_hex("RAM cnt", layout.memory_count);
    for (uint32_t i = 0; i < layout.memory_count; i++) {
        con_hex("RAM B", layout.memory[i].base);
        con_hex("RAM S", layout.memory[i].size);
    }

    con_hex("RES cnt", layout.reserved_count);
    for (uint32_t i = 0; i < layout.reserved_count; i++) {
        con_hex("RES base", layout.reserved[i].base);
        con_hex("RES size", layout.reserved[i].size);
    }

    con_hex("Unres", layout.unresolved_count);
    con_hex("DRAM bse", layout.dram_base);
    con_hex("DRAM sze", layout.total_ram_size);
    con_hex("DRAM end", layout.dram_end);
}
