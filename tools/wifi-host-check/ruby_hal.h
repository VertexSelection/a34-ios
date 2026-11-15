/* Stand-in for bsd/dev/ruby_hal.h (ruby-ios-bringup's, not in this repository): what a34_wifi.c uses. */
#ifndef RUBY_HAL_H
#define RUBY_HAL_H
#include <stdint.h>
int ruby_pwrap_read(uintptr_t pwrap, uint16_t reg, uint16_t *val);
void a34_pmic_lock(void);
void a34_pmic_unlock(void);
#endif
