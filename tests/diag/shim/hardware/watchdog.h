#pragma once
#include <stdint.h>
struct test_watchdog_t { uint32_t scratch[8]; };
extern test_watchdog_t *watchdog_hw;
bool watchdog_enable_caused_reboot();
void watchdog_enable(uint32_t, bool);
void watchdog_disable();
void watchdog_update();
