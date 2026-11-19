
#pragma once

#include <stdint.h>

/*
 * Builds the bootstrap translation tables and enables
 * the EL1 MMU.
 *
 * Must be called from EL1 with the MMU currently disabled.
 *
 * Returns:
 *   0  Success
 *  -1  Not executing at EL1
 *  -2  MMU is already enabled
 */
int mmu_init(void);

/* Returns nonzero if SCTLR_EL1.M is set. */
int mmu_is_enabled(void);
