/**
 * PicoGBC I2S Audio Driver
 *
 * Robust, interrupt-driven ring buffer DMA streaming for RP2040/RP2350.
 * Eliminates DMA pointer overruns, memory scanning noise, and DC bias clipping.
 * Based on the reference architecture used in PicoSMS, PicoNGP, and PicoPCE.
 */

#include "i2s.h"
#include "pico/stdlib.h"
#include "hardware/irq.h"
#include "hardware/sync.h"

#define I2S_AUDIO_RING_SIZE 2048u
#define AUDIO_RING_MASK (I2S_AUDIO_RING_SIZE - 1u)
#define DMA_BLOCK_SIZE 128u

static uint32_t audio_ring[I2S_AUDIO_RING_SIZE];
static volatile size_t write_index = 0;
static volatile size_t read_index = 0;
static int s_audio_dma_chan = -1;

static int32_t dc_avg_l = 0;
static int32_t dc_avg_r = 0;

static inline uint32_t dc_block_sample(int32_t l, int32_t r) {
    dc_avg_l += (l - dc_avg_l) >> 10;
    dc_avg_r += (r - dc_avg_r) >> 10;
    l -= dc_avg_l;
    r -= dc_avg_r;
    if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
    if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
    return ((uint32_t)(uint16_t)l << 16) | (uint16_t)r;
}

static void __isr audio_dma_handler(void) {
    if (s_audio_dma_chan >= 0 && (dma_hw->ints0 & (1u << s_audio_dma_chan))) {
        // Clear interrupt flag
        dma_hw->ints0 = 1u << s_audio_dma_chan;
        read_index = (read_index + DMA_BLOCK_SIZE) & AUDIO_RING_MASK;
        size_t available = (write_index >= read_index)
                               ? (write_index - read_index)
                               : (I2S_AUDIO_RING_SIZE - read_index + write_index);
        if (available >= DMA_BLOCK_SIZE) {
            dma_channel_set_read_addr(s_audio_dma_chan, &audio_ring[read_index], false);
            dma_channel_set_trans_count(s_audio_dma_chan, DMA_BLOCK_SIZE, true);
        }
    }
}

i2s_config_t i2s_get_default_config(void) {
    i2s_config_t i2s_config = {
        .sample_freq = 44100, 
        .channel_count = 2,
        .data_pin = 26,
        .clock_pin_base = 27,
        .pio = pio0,
        .sm = 0,
        .dma_channel = 0,
        .dma_channel_b = 0,
        .dma_buf = NULL,
        .dma_buf_b = NULL,
        .dma_trans_count = 735,
        .volume = 3,
    };
    return i2s_config;
}

void i2s_init(i2s_config_t *i2s_config) {
    // Configure pins for PIO0
    gpio_set_function(i2s_config->data_pin, GPIO_FUNC_PIO0);
    gpio_set_function(i2s_config->clock_pin_base, GPIO_FUNC_PIO0);
    gpio_set_function(i2s_config->clock_pin_base + 1, GPIO_FUNC_PIO0);

    gpio_set_slew_rate(i2s_config->data_pin, GPIO_SLEW_RATE_SLOW);
    gpio_set_slew_rate(i2s_config->clock_pin_base, GPIO_SLEW_RATE_SLOW);
    gpio_set_slew_rate(i2s_config->clock_pin_base + 1, GPIO_SLEW_RATE_SLOW);
    gpio_set_drive_strength(i2s_config->data_pin, GPIO_DRIVE_STRENGTH_2MA);
    gpio_set_drive_strength(i2s_config->clock_pin_base, GPIO_DRIVE_STRENGTH_2MA);
    gpio_set_drive_strength(i2s_config->clock_pin_base + 1, GPIO_DRIVE_STRENGTH_2MA);

    i2s_config->sm = pio_claim_unused_sm(i2s_config->pio, true);
    uint offset = pio_add_program(i2s_config->pio, &audio_i2s_program);
    audio_i2s_program_init(i2s_config->pio, i2s_config->sm, offset, i2s_config->data_pin, i2s_config->clock_pin_base);

    uint32_t system_clock_frequency = clock_get_hz(clk_sys);
    uint32_t divider = system_clock_frequency * 4 / i2s_config->sample_freq;
    pio_sm_set_clkdiv_int_frac(i2s_config->pio, i2s_config->sm, divider >> 8u, divider & 0xffu);
    pio_sm_set_enabled(i2s_config->pio, i2s_config->sm, true);

    memset(audio_ring, 0, sizeof(audio_ring));
    write_index = 0;
    read_index = 0;

    i2s_config->dma_channel = dma_claim_unused_channel(true);
    s_audio_dma_chan = i2s_config->dma_channel;

    dma_channel_config c = dma_channel_get_default_config(i2s_config->dma_channel);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(i2s_config->pio, i2s_config->sm, true));

    dma_channel_configure(
        i2s_config->dma_channel,
        &c,
        &(i2s_config->pio->txf[i2s_config->sm]),
        &audio_ring[0],
        DMA_BLOCK_SIZE,
        false
    );

    dma_channel_set_irq0_enabled(i2s_config->dma_channel, true);
    irq_add_shared_handler(DMA_IRQ_0, audio_dma_handler, PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY);
    irq_set_enabled(DMA_IRQ_0, true);
}

void i2s_audio_start(i2s_config_t *i2s_config) {
    uint32_t save = save_and_disable_interrupts();
    dma_channel_abort(i2s_config->dma_channel);
    /* An abort of an active transfer can still raise its completion IRQ
     * (RP2350). Left pending, audio_dma_handler() would advance read_index by a
     * block right after the reset below, the ring would look full to the
     * writer and nearly every sample after it would be dropped -- silence until
     * the next restart. That is what a resume did after the intro chime had
     * left the DMA running. */
    dma_hw->ints0 = 1u << i2s_config->dma_channel;
    memset(audio_ring, 0, sizeof(audio_ring));
    write_index = 0;
    read_index = 0;
    dc_avg_l = 0;
    dc_avg_r = 0;
    pio_sm_clear_fifos(i2s_config->pio, i2s_config->sm);
    restore_interrupts(save);
}

void i2s_audio_stop(i2s_config_t *i2s_config) {
    uint32_t save = save_and_disable_interrupts();
    dma_channel_abort(i2s_config->dma_channel);
    /* An abort of an active transfer can still raise its completion IRQ
     * (RP2350). Left pending, audio_dma_handler() would advance read_index by a
     * block right after the reset below, the ring would look full to the
     * writer and nearly every sample after it would be dropped -- silence until
     * the next restart. That is what a resume did after the intro chime had
     * left the DMA running. */
    dma_hw->ints0 = 1u << i2s_config->dma_channel;
    memset(audio_ring, 0, sizeof(audio_ring));
    write_index = 0;
    read_index = 0;
    pio_sm_clear_fifos(i2s_config->pio, i2s_config->sm);
    restore_interrupts(save);
}

void i2s_write(const i2s_config_t *i2s_config, const int16_t *samples, const size_t len) {
    for (size_t i = 0; i < len; i++) {
        pio_sm_put_blocking(i2s_config->pio, i2s_config->sm, (uint32_t)samples[i]);
    }
}

void i2s_dma_write(i2s_config_t *i2s_config, const int16_t *samples) {
    if (!samples) return;

    static const int32_t vol_table[6] = { 0, 51, 102, 154, 205, 256 };
    uint8_t vol_idx = i2s_config->volume;
    if (vol_idx > 5) vol_idx = 5;
    int32_t vol = vol_table[vol_idx];
    const uint16_t count = i2s_config->dma_trans_count; // 735 stereo samples

    if (vol <= 0) {
        for (uint16_t i = 0; i < count; i++) {
            size_t next_write = (write_index + 1) & AUDIO_RING_MASK;
            if (next_write != read_index) {
                audio_ring[write_index] = 0;
                write_index = next_write;
            }
        }
    } else {
        for (uint16_t i = 0; i < count; i++) {
            int32_t l = ((int32_t)samples[i * 2 + 0] * vol) >> 8;
            int32_t r = ((int32_t)samples[i * 2 + 1] * vol) >> 8;

            uint32_t sample32 = dc_block_sample(l, r);

            size_t next_write = (write_index + 1) & AUDIO_RING_MASK;
            if (next_write != read_index) {
                audio_ring[write_index] = sample32;
                write_index = next_write;
            }
        }
    }

    uint32_t save = save_and_disable_interrupts();
    if (!dma_channel_is_busy(i2s_config->dma_channel)) {
        size_t available = (write_index >= read_index)
                               ? (write_index - read_index)
                               : (I2S_AUDIO_RING_SIZE - read_index + write_index);
        if (available >= DMA_BLOCK_SIZE) {
            dma_channel_set_read_addr(i2s_config->dma_channel, &audio_ring[read_index], false);
            dma_channel_set_trans_count(i2s_config->dma_channel, DMA_BLOCK_SIZE, true);
        }
    }
    restore_interrupts(save);
}

/* Free slots in the ring, and a single-sample push. For the standby-resume
 * intro (shared_menu/dmg_boot_intro.c), which streams a 44.1 kHz chime at its
 * own pace: i2s_dma_write() drops whatever does not fit, so the intro needs the
 * real free space to avoid losing samples (which is what made it sound off). */
size_t i2s_ring_free(void) {
    uint32_t save = save_and_disable_interrupts();
    size_t w = write_index;
    size_t r = read_index;
    restore_interrupts(save);
    return (r - w - 1u) & AUDIO_RING_MASK;
}

void i2s_ring_push(i2s_config_t *i2s_config, int16_t left, int16_t right) {
    size_t next_write = (write_index + 1) & AUDIO_RING_MASK;
    if (next_write == read_index) return;
    audio_ring[write_index] = ((uint32_t)(uint16_t)left << 16) | (uint16_t)right;
    write_index = next_write;

    uint32_t save = save_and_disable_interrupts();
    if (!dma_channel_is_busy(i2s_config->dma_channel)) {
        size_t available = (write_index >= read_index)
                               ? (write_index - read_index)
                               : (I2S_AUDIO_RING_SIZE - read_index + write_index);
        if (available >= DMA_BLOCK_SIZE) {
            dma_channel_set_read_addr(i2s_config->dma_channel, &audio_ring[read_index], false);
            dma_channel_set_trans_count(i2s_config->dma_channel, DMA_BLOCK_SIZE, true);
        }
    }
    restore_interrupts(save);
}

void i2s_volume(i2s_config_t *i2s_config, uint8_t volume) {
    if (volume > 5) volume = 5;
    i2s_config->volume = volume;
}

void i2s_increase_volume(i2s_config_t *i2s_config) {
    if (i2s_config->volume < 5) {
        i2s_config->volume++;
    }
}

void i2s_decrease_volume(i2s_config_t *i2s_config) {
    if (i2s_config->volume > 0) {
        i2s_config->volume--;
    }
}
