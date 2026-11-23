#include "payload.h"

int payload_magic_ok(const struct payload *p)
{
    for (unsigned i = 0; i < PAYLOAD_MAGIC_SIZE; i++) {
        if (p->magic[i] != PAYLOAD_MAGIC[i])
            return 0;
    }
    return 1;
}

struct payload *payload_find(uintptr_t initrd_start,
                                    uint64_t initrd_size)
{
    const uint8_t *data = (const uint8_t *)initrd_start;
    if (initrd_size < sizeof(struct payload))
        return NULL;

    for (uint64_t i = 0;
         i <= initrd_size - sizeof(struct payload);
         i++) {
        uintptr_t candidate = initrd_start + (uintptr_t)i;
        if (data[i] != 'A' || (candidate & 7U))
            continue;
        struct payload *p = (struct payload *)candidate;
        if (payload_magic_ok(p))
            return p;
    }
    return NULL;
}