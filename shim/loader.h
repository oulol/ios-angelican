#pragma once

#include <stdint.h>

#define LOADER_SUCCESS          0
#define LOADER_ERROR_BAD_EL    -1
#define LOADER_ERROR_MMU       -2

void loader_main(void) __attribute__((noreturn));
void minimal_el1_main(void) __attribute__((noreturn));
void main(void) __attribute__((noreturn));

uint64_t loader_current_el(void);
uint64_t loader_read_daif(void);