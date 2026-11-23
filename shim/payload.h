#include <stdint.h>
#include <stddef.h>

#define PAYLOAD_MAGIC          "ANGELXNU"
#define PAYLOAD_MAGIC_SIZE     8U
#define PAYLOAD_VERSION        1U
#define PAYLOAD_MAGIC_SIZE 8U
#define PAYLOAD_VERSION    1U
#define PAYLOAD_MAX_SEGS   128U
#define BOOT_LINE_LENGTH   608U

struct payload_seg {
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t file_off;
    uint64_t file_size;
};

struct payload {
    char magic[8];
    uint32_t version;
    uint32_t nsegs;
    uint64_t virt_base;
    uint64_t entry_va;
    uint64_t adt_off;
    uint64_t adt_size;
    uint64_t kernel_end;
    uint64_t phys_base;
    uint64_t mem_size;
    char cmdline[BOOT_LINE_LENGTH];
    uint8_t has_android;
    uint8_t reserved[7];
    uint64_t android_kernel_offset;
    uint64_t android_kernel_size;
    uint64_t android_ramdisk_offset;
    uint64_t android_ramdisk_size;
    struct payload_seg segs[];
};

struct payload *payload_find(uintptr_t initrd_start,
                             uint64_t initrd_size);

int payload_validate(const struct payload *p,
                     uint64_t payload_size);