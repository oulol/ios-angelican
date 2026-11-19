#pragma once

#include <stdint.h>

#define POWER_SUCCESS       0
#define POWER_ERROR_PMIC   -1
#define POWER_ERROR_POLL   -2

void power_init(void);

/*
 * Returns:
 *   1  - power button pressed
 *   0  - power button released
 *  <0  - communication error
 */
int power_button_pressed(void);

/* Does not return. */
void power_reset(void);
/* Checks the key and resets the device if pressed. */
void power_poll(void);