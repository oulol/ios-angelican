#pragma once

#include <stdint.h>

/*
 * State captured during early boot.
 *
 * If start.S already captures x0-x3, keep that code and write into
 * these fields. Do not overwrite the captured values in loader.c.
 */
typedef struct boot_state {
    uint64_t x0;
    uint64_t x1;
    uint64_t x2;
    uint64_t x3;

    uint64_t initial_sp;
    uint64_t current_el;
    uint64_t daif;
} boot_state_t;

extern volatile boot_state_t boot_state;