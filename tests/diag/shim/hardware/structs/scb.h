#pragma once
#include <stdint.h>
struct test_scb_t { uint32_t cfsr, hfsr, bfar, mmfar; };
extern test_scb_t *scb_hw;
