#ifndef PRESSURE_POWER_H
#define PRESSURE_POWER_H

#include <stdint.h>

/* 仅在线程模式、ADC 空闲时调用。读到已确认的断电状态时返回 0。 */
uint8_t power_supply_ok(void);

#endif
