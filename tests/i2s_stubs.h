/* Hardware boundary for tests of the actual I2S ring/ISR implementation. */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define __isr
#define __not_in_flash_func(name) name
#undef __CONCAT
#define A26_CONCAT(a,b) a##b
#define __CONCAT(a,b) A26_CONCAT(a,b)
#define PICO_AUDIO_I2S_PIO 0
#define PICO_AUDIO_I2S_DMA_IRQ 1
#define PICO_AUDIO_I2S_FREQ 44100
#define PICO_AUDIO_I2S_DRIVER_NONE 0
#define PICO_AUDIO_I2S_DRIVER_TLV320 1
#define PICO_AUDIO_I2S_DRIVER_PCM5000A 2
#define PICO_AUDIO_I2S_DATA_PIN 26
#define PICO_AUDIO_I2S_CLOCK_PIN_BASE 27
#define I2S_AUDIO_RING_SIZE 1024
#define AUDIO_RING_MASK 1023
#define DMA_BLOCK_SIZE 128
#define DMA_IRQ_0 0
#define DMA_IRQ_1 1
#define DMA_SIZE_32 2
#define GPIO_FUNC_PIO0 0
#define clk_sys 0
typedef unsigned uint;
struct pio_program { int unused; };
static const struct pio_program audio_i2s_program={0};
typedef struct { uint32_t txf[4]; } FakePio;
static FakePio fake_pio;
typedef FakePio *PIO;
#define pio0 (&fake_pio)
typedef struct { int sm; PIO pio; int dma_chan; } audio_i2s_hw_t;
typedef struct { uint32_t ints0,ints1; } FakeDma;
static FakeDma fake_dma;
#define dma_hw (&fake_dma)
typedef struct { bool increment; } dma_channel_config;
static dma_channel_config fake_config;
static const void *fake_read_address;
static unsigned fake_transfer_count, fake_starts, fake_aborts;
static int fake_irq=-1;
enum headphone_toggle_t { NO_HEADPHONES };
static enum headphone_toggle_t tlv320_poll_headphone(void) { return NO_HEADPHONES; }
static void tlv320_init(void) {}
static bool tlv320_dac_error(void) { return false; }
static void tlv320_set_volume(int8_t v) { (void)v; }
static void tlv320_mute_speaker(bool v) { (void)v; }
static void gpio_set_function(unsigned a,unsigned b) { (void)a; (void)b; }
static int pio_claim_unused_sm(PIO p,bool v) { (void)p; (void)v; return 0; }
static uint pio_add_program(PIO p,const struct pio_program *v) { (void)p; (void)v; return 0; }
static void audio_i2s_program_init(PIO p,int s,uint o,int d,int b) { (void)p;(void)s;(void)o;(void)d;(void)b; }
static uint32_t clock_get_hz(int c) { (void)c; return 280000000; }
static void pio_sm_set_clkdiv_int_frac(PIO p,int s,uint32_t i,uint8_t f) { (void)p;(void)s;(void)i;(void)f; }
static void pio_sm_set_enabled(PIO p,int s,bool v) { (void)p;(void)s;(void)v; }
static void pio_sm_clear_fifos(PIO p,int s) { (void)p;(void)s; }
static void pio_sm_put_blocking(PIO p,int s,uint32_t v) { (void)p;(void)s;(void)v; }
static int pio_get_dreq(PIO p,int s,bool tx) { (void)p;(void)s;(void)tx; return 0; }
static int dma_claim_unused_channel(bool v) { (void)v; return 2; }
static dma_channel_config dma_channel_get_default_config(int n) { (void)n; return (dma_channel_config){true}; }
static dma_channel_config dma_get_channel_config(int n) { (void)n; return fake_config; }
static void channel_config_set_read_increment(dma_channel_config *c,bool v) { c->increment=v; }
static void channel_config_set_transfer_data_size(dma_channel_config *c,int v) { (void)c;(void)v; }
static void channel_config_set_write_increment(dma_channel_config *c,bool v) { (void)c;(void)v; }
static void channel_config_set_dreq(dma_channel_config *c,int v) { (void)c;(void)v; }
static void dma_channel_set_config(int n,const dma_channel_config *c,bool trigger) { (void)n;(void)trigger; fake_config=*c; }
static void dma_channel_set_read_addr(int n,const void *address,bool trigger) { (void)n;(void)trigger; fake_read_address=address; }
static void dma_channel_set_trans_count(int n,unsigned count,bool trigger) {
    (void)n; fake_transfer_count=count; fake_starts+=trigger;
}
static void dma_channel_configure(int n,const dma_channel_config *c,void *w,const void *r,unsigned count,bool trigger) {
    (void)w; dma_channel_set_config(n,c,false); dma_channel_set_read_addr(n,r,false); dma_channel_set_trans_count(n,count,trigger);
}
static void dma_channel_set_irq0_enabled(int n,bool v) { (void)n;(void)v; }
static void dma_channel_set_irq1_enabled(int n,bool v) { (void)n;(void)v; }
static void irq_set_exclusive_handler(int n,void (*f)(void)) { (void)f; fake_irq=n; }
static void irq_set_enabled(int n,bool v) { (void)n;(void)v; }
static void dma_channel_abort(int n) { (void)n; ++fake_aborts; }
static uint32_t save_and_disable_interrupts(void) { return 0; }
static void restore_interrupts(uint32_t s) { (void)s; }
static void __dmb(void) {}
