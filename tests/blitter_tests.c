#define A26_BLITTER_HOST_TEST 1
#include "pico_shared/drivers/ili9341/ili9341.c"
#include <assert.h>
uint16_t PalTable[64];
static void fake_tick(void) {
    if(fake_grant) s_spi_ovl_idle=s_spi_ovl_request;
}
int main(void) {
    ili9341_blitter_start(); assert(fake_launches==1);
    uint16_t pixels[160]; for(unsigned i=0;i<160;++i) pixels[i]=0x1234;
    ili9341_stream_a26_line(0,pixels);
    assert(ili9341_try_submit_a26_frame());
    unsigned render=s_render_idx; uint32_t before=fake_time;
    for(int frame=0;frame<8;++frame) {
        for(unsigned i=0;i<160;++i) pixels[i]=frame;
        ili9341_stream_a26_line(0,pixels);
        assert(!ili9341_try_submit_a26_frame());
        assert(s_a26_buf[render][0]==0x1234 && s_render_idx==render);
    }
    assert(fake_time==before); // busy display never waits on the producer
    s_frame_ready=false;
    assert(ili9341_try_submit_a26_frame() && s_render_idx!=render);
    fake_time+=100001;
    assert(ili9341_is_frame_busy() && !ili9341_blitter_ok());
    ili9341_blitter_stop(); assert(fake_resets==1 && !s_blitter_active && !s_frame_ready);
    ili9341_blitter_start(); assert(ili9341_blitter_ok() && fake_launches==2);
    // Exercise the real overlay packer and handshake, including all 24 rows.
    fake_grant=true;
    memset(s_ovl_buf,0xa5,sizeof(s_ovl_buf));
    ili9341_set_fps_overlay(true,"60");
    assert(fake_last_data==s_ovl_buf && fake_last_length==312*24*2);
    assert(!s_spi_ovl_request && !s_spi_ovl_idle);
    for(unsigned y=0;y<24;++y) for(unsigned x=0;x<312;++x) {
        unsigned glyph=x/24, col=(x%24)/3;
        char c=glyph==0?'6':glyph==1?'0':' ';
        uint8_t bits=font_8x8[(c-32)+(y/3)*95];
        uint8_t expected=(bits&(1u<<col))?255:0;
        assert(s_ovl_buf[(y*312+x)*2]==expected && s_ovl_buf[(y*312+x)*2+1]==expected);
    }
    fake_gb=true; ili9341_set_fps_overlay(true,"60");
    assert(fake_last_length==264*24*2); // starts at x=53; 11 glyphs fit
    ili9341_set_fps_overlay(false,NULL);
    for(unsigned i=0;i<264*24*2;++i) assert(s_ovl_buf[i]==0);
    fake_grant=false; unsigned writes=fake_writes;
    ili9341_set_fps_overlay(true,"59");
    assert(fake_writes==writes && !ili9341_blitter_ok()); // timeout must skip SPI
    ili9341_blitter_stop();
    puts("blitter: nonblocking submission, buffer ownership, timeout, overlay and GB clamp passed");
}
