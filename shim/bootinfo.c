#include "bootinfo.h"

#include "fdt.h"
#include "console.h"

#include <stdint.h>

#define CMDLINE_BUFFER_SIZE 192

static void print_chosen_string(
    uintptr_t dtb,
    const char *property,
    const char *label
)
{
    uintptr_t value;
    uint32_t length;

    if (!fdt_get_chosen_property(
            dtb,
            property,
            &value,
            &length
        )) {
        con_str(label, "(not present)");
        return;
    }

    char buffer[CMDLINE_BUFFER_SIZE];

    uint32_t count = length;

    if (count >= sizeof(buffer))
        count = sizeof(buffer) - 1;

    volatile const uint8_t *src =
        (volatile const uint8_t *)value;

    for (uint32_t i = 0; i < count; i++) {
        if (src[i] == 0) {
            count = i;
            break;
        }

        buffer[i] = (char)src[i];
    }

    buffer[count] = '\0';

    con_str(label, buffer);
}

static void print_chosen_u64(
    uintptr_t dtb,
    const char *property,
    const char *label
)
{
    uint64_t value;

    if (!fdt_chosen_u64(dtb, property, &value)) {
        con_str(label, "(not present)");
        return;
    }

    con_hex(label, value);
}

static void print_initrd_magic(uintptr_t start)
{
    volatile const uint8_t *p =
        (volatile const uint8_t *)start;

    con_str("INITRD", "First 16 bytes:");

    for (unsigned i = 0; i < 16; i++) {
        con_hex("BYTE", p[i]);
    }
}

void bootinfo_dump(uintptr_t dtb)
{
    uint32_t total_size = 0;

    con_skip();
    con_str("DTB", "Runtime device tree");

    con_hex("DTB address", dtb);

    if (!fdt_validate(dtb, &total_size)) {
        con_str("DTB ERROR", "Invalid FDT header or bounds");
        return;
    }

    con_hex("DTB size", total_size);

    con_str("DTB", "FDT validation successful");

    con_skip();
    con_str("CHOSEN", "/chosen properties");

    print_chosen_string(
        dtb,
        "bootargs",
        "Boot arguments"
    );

    print_chosen_u64(
        dtb,
        "linux,initrd-start",
        "IRDS"
    );

    print_chosen_u64(
        dtb,
        "linux,initrd-end",
        "IRDE"
    );

    con_skip();

    print_initrd_magic(0x51B00000);
}