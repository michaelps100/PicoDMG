#ifndef A26_FAKE_SPI_H
#define A26_FAKE_SPI_H
#include <stdint.h>
typedef struct { uint32_t dr; } spi_inst_t;
static spi_inst_t fake_spi;
#define spi0 (&fake_spi)
#endif
