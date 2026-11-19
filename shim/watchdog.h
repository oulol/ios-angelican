#pragma once

#include <stdint.h>

void watchdog_init(void);
void watchdog_service(void);

uint64_t watchdog_read_counter(void);
uint64_t watchdog_read_frequency(void);