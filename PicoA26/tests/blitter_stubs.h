#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "pico_shared/drivers/ili9341/ili9341.h"
#include "pico_shared/font_8x8.h"
#define __not_in_flash_func(name) name
#define __aligned(n) __attribute__((aligned(n)))
#define GPIO_OUT 1
#define GPIO_FUNC_SPI 1
#define SPI_CPOL_0 0
#define SPI_CPHA_0 0
#define SPI_MSB_FIRST 0
#define DMG_PANEL_ST7789 1
static uint32_t fake_time, fake_resets, fake_launches;
static unsigned fake_writes;
static bool fake_gb, fake_grant;
static const uint8_t *fake_last_data;
static size_t fake_last_length;
static void fake_tick(void);
static uint32_t time_us_32(void) { return fake_time; }
static void tight_loop_contents(void) { fake_time+=1000; fake_tick(); }
static void sleep_ms(unsigned n) { fake_time+=n*1000; }
static void gpio_init(unsigned a) { (void)a; }
static void gpio_set_dir(unsigned a,unsigned b) { (void)a;(void)b; }
static void gpio_put(unsigned a,unsigned b) { (void)a;(void)b; }
static void gpio_set_function(unsigned a,unsigned b) { (void)a;(void)b; }
static int spi_write_blocking(spi_inst_t *s,const uint8_t *data,size_t n) {
    (void)s; ++fake_writes;
    fake_last_data=data; fake_last_length=n;
    return (int)n;
}
static bool spi_is_busy(spi_inst_t *s) { (void)s; return false; }
static bool spi_is_readable(spi_inst_t *s) { (void)s; return false; }
static spi_inst_t *spi_get_hw(spi_inst_t *s) { return s; }
static unsigned spi_init(spi_inst_t *s,unsigned n) { (void)s; return n; }
static void spi_deinit(spi_inst_t *s) { (void)s; }
static unsigned spi_set_baudrate(spi_inst_t *s,unsigned n) { (void)s; return n; }
static unsigned spi_get_baudrate(spi_inst_t *s) { (void)s; return 46666666; }
static void spi_set_format(spi_inst_t *s,unsigned a,unsigned b,unsigned c,unsigned d) { (void)s;(void)a;(void)b;(void)c;(void)d; }
static void multicore_launch_core1(void (*f)(void)) { (void)f; ++fake_launches; }
static void multicore_reset_core1(void) { ++fake_resets; }
static bool dmg_is_gb_mode(void) { return fake_gb; }
static void dmg_panel_config_init(void) {}
static unsigned dmg_panel_get_madctl(void) { return 0x28; }
static unsigned dmg_panel_get(void) { return 0; }
static bool dmg_panel_get_invert(void) { return false; }
static bool dmg_panel_has_explicit_madctl(void) { return false; }
static const char *dmg_panel_source(void) { return "test"; }
static void dmg_panel_st7789_timing(void (*f)(uint8_t,const uint8_t *,size_t)) { (void)f; }
typedef struct { uint16_t src_idx; uint8_t weight1; } dmg_scale_weight_t;
static void dmg_init_shimmerless_weights(unsigned src,unsigned dest,dmg_scale_weight_t *w) {
    for(unsigned i=0;i<dest;++i) { w[i].src_idx=i*src/dest; w[i].weight1=0; }
}
static uint16_t dmg_blend_rgb565(uint16_t a,uint16_t b,uint8_t w) { (void)b;(void)w; return a; }
static const uint16_t *dmg_volume_hud_get_row(uint16_t y,int *x,int *w) { (void)y;(void)x;(void)w; return NULL; }
typedef void (*dmg_batt_push_fn)(uint16_t,uint16_t,uint16_t,uint16_t,const uint16_t*);
static void dmg_batt_overlay_stamp_row(uint16_t y,uint16_t *b,int x0,int w,bool s) { (void)y;(void)b;(void)x0;(void)w;(void)s; }
static void dmg_batt_overlay_push_borders(int a,int b,int c,int d,dmg_batt_push_fn f) { (void)a;(void)b;(void)c;(void)d;(void)f; }
