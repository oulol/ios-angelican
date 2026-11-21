#pragma once

#include <stdint.h>

/* Must match the 256-byte register frame built by vectors.S. */
typedef struct {
    uint64_t x[31];
    uint64_t reserved;
} el1_exception_frame_t;

_Static_assert(sizeof(el1_exception_frame_t) == 256,
               "EL1 exception frame must be 256 bytes");

void el1_exception_dispatch(
    const el1_exception_frame_t *frame,
    uint64_t vector
) __attribute__((noreturn));

void wait_loop(void);