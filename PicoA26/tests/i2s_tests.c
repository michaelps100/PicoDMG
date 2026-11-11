#define A26_I2S_HOST_TEST 1
#include "pico_shared/drivers/pico_audio_i2s/audio_i2s.c"
#include <assert.h>
int main(void) {
    assert(audio_i2s_setup(2,44100,-1));
    assert(fake_irq==DMA_IRQ_1 && fake_starts==1);
    assert(s_dma_silence && fake_read_address==&s_silence && !fake_config.increment);
    assert(fake_transfer_count==128);
    for(unsigned i=0;i<128;++i) audio_i2s_enqueue_sample(0x10000+i);
    assert(fake_starts==1); // enqueue cannot restart a completed-but-unserviced DMA
    dma_handler();
    assert(!s_dma_silence && fake_read_address==audio_ring && fake_config.increment);
    assert(read_index==0 && fake_dma.ints1==(1u<<2) && fake_dma.ints0==0);
    for(unsigned i=0;i<128;++i) assert(audio_ring[i]==0x10000+i);
    dma_handler(); assert(read_index==128 && s_dma_silence && s_underruns==1);
    dma_handler(); assert(read_index==128 && s_underruns==1); // silence never consumes ring data
    for(unsigned i=0;i<128;++i) audio_i2s_enqueue_sample(0x20000+i);
    dma_handler(); assert(!s_dma_silence && fake_read_address==audio_ring+128);
    dma_handler(); assert(read_index==256 && s_dma_silence && s_underruns==2);
    // Wrap all block boundaries; DMA addresses must remain within the allocation.
    for(unsigned block=0;block<24;++block) {
        for(unsigned i=0;i<128;++i) audio_i2s_enqueue_sample(block*128+i);
        dma_handler(); assert(!s_dma_silence);
        assert((const uint32_t *)fake_read_address>=audio_ring &&
               (const uint32_t *)fake_read_address+128<=audio_ring+1024);
        dma_handler();
    }
    audio_i2s_flush(); assert(fake_aborts==1 && s_dma_silence && read_index==0 && write_index==0);
    for(unsigned i=0;i<1024;++i) audio_i2s_enqueue_sample(i);
    assert(s_overruns==1 && audio_i2s_get_freebuffer_size()==0);
    dma_handler(); assert(!s_dma_silence);
    audio_i2s_flush(); assert(audio_i2s_get_fill_permille()==0);
    audio_i2s_disable();
    puts("i2s: IRQ routing, DMA ownership, underrun, recovery, wrap and flush passed");
}
