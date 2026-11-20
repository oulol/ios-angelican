#pragma once

#include <stdint.h>

#define LOADER_XNU_SUCCESS       0
#define LOADER_XNU_ERROR_DTB   -1
#define LOADER_XNU_ERROR_INITRD -2
#define LOADER_XNU_ERROR_PAYLOAD -3
#define LOADER_XNU_ERROR_SEGMENT -4
#define LOADER_XNU_ERROR_ADT   -5
#define LOADER_XNU_ERROR_BOOT_ARGS -6

void loader_xnu_boot(uintptr_t dtb) __attribute__((noreturn));
