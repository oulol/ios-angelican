
#include "loader_android.h"

#if defined(BOOTMENU)

#include <stdint.h>

#include "console.h"
#include "payload.h"
#include "fdt.h"
#include "boot_state.h"
#include "exception.h"

#define ARM64_IMAGE_MAGIC 0x644D5241U
#define KERNEL_ALIGN (2ULL * 1024 * 1024)

static void panic(const char *reason) {
    con_str("BOOTMGR", reason);
    wait_loop();
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint64_t read_le64(const uint8_t *p) {
    return (uint64_t)read_le32(p)
         | ((uint64_t)read_le32(p + 4) << 32);
}


#include <stdint.h>

__attribute__((noreturn))
static void android_handoff(
    uintptr_t arg0,
    uintptr_t arg1,
    uintptr_t arg2,
    uintptr_t arg3,
    uintptr_t kernel_entry
) {
    __asm__ volatile(
        "msr daifset, #0xf\n"

        "mrs x4, hcr_el2\n"
        "orr x4, x4, #(1 << 31)\n"
        "msr hcr_el2, x4\n"

        "mrs x4, cnthctl_el2\n"
        "orr x4, x4, #3\n"
        "msr cnthctl_el2, x4\n"
        "msr cntvoff_el2, xzr\n"

        "isb\n"
        "dsb sy\n"
        "isb\n"

        "mov x0, %0\n"
        "mov x1, %1\n"
        "mov x2, %2\n"
        "mov x3, %3\n"
        "br %4\n"
        :
        : "r"(arg0), "r"(arg1), "r"(arg2), "r"(arg3),
          "r"(kernel_entry)
        : "x0", "x1", "x2", "x3", "x4", "memory"
    );

    __builtin_unreachable();
}


void loader_android_boot(const struct payload *payload) {
    uintptr_t fdt = (uintptr_t)boot_state.x0;

    uintptr_t image_base =
        (uintptr_t)payload + payload->android_kernel_offset;

    uint64_t kernel_size = payload->android_kernel_size;

    uintptr_t ramdisk_start =
        (uintptr_t)payload + payload->android_ramdisk_offset;

    uint64_t ramdisk_size = payload->android_ramdisk_size;

    if (!payload || kernel_size < 64 || !ramdisk_size)
        panic("Invalid Android payload");

    const uint8_t *image = (const uint8_t *)image_base;

    if (read_le32(image + 0x38) != ARM64_IMAGE_MAGIC)
        panic("Invalid ARM64 Image magic");

    uint64_t text_offset = read_le64(image + 0x08);
    uint64_t image_size = read_le64(image + 0x10);
    uint64_t flags = read_le64(image + 0x18);

    if (image_size && image_size < kernel_size) {
        con_hex2("KERNEL", "IMG", image_size, "KRNL", kernel_size);
        panic("Invalid Android Image size");
    }

    if (image_base < text_offset ||
        ((image_base - text_offset) & (KERNEL_ALIGN - 1)) != 0)
        panic("Android kernel misaligned");

    if (flags & 1)
        panic("Unsupported big-endian kernel");

    uintptr_t entry = image_base;

    con_str("BOOTMGR", "Updating FDT");

    if (!fdt_set_initrd(fdt, ramdisk_start, ramdisk_size))
        panic("Failed to update Android initrd");

    con_str("BOOTMGR", "FDT updated");

    con_hex2("PAYLOAD", "start=", ramdisk_start,
             "end=", ramdisk_start + ramdisk_size);

    con_hex("ENTRY", entry);

    android_handoff(
        boot_state.x0,
        boot_state.x1,
        boot_state.x2,
        boot_state.x3,
        entry
    );
    panic("Kernel returned");
}

#endif