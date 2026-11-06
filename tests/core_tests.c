#include "a26_core.h"
#include "core_state.h"
#include "atari/atari_audio.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static uint8_t rom[65536+128], saved[4096], second[4096];
static unsigned checks;
#define CHECK(x) do { ++checks; if(!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); abort(); } } while(0)
static void prepare(size_t n) {
    // Distinct contents and halves prevent accidental Superchip detection.
    for(size_t i=0;i<n;++i) rom[i]=(uint8_t)((i/4096)+(i/128)%7);
    if(n>=4096) { rom[n-4]=0; rom[n-3]=0x10; }
}
static void load_program(const uint8_t *code,size_t n) {
    memset(rom,0xea,4096); memcpy(rom,code,n); rom[4092]=0; rom[4093]=0x10;
    CHECK(a26_core_load_rom(rom,4096));
}
static void riot_tests(void) {
    a26_core_reset();
    a26_core_set_controls(false,false,false,true,false,false,false);
    for(unsigned mirror=0;mirror<32;mirror+=8) {
        CHECK(memory_read(0x280+mirror)==0x7f);
        memory_write(0x281+mirror,0xf0);
        memory_write(0x280+mirror,0xff);
        CHECK(memory_read(0x280+mirror)==0x7f); // external LOW wins, even output HIGH
        CHECK(memory_read(0x281+mirror)==0xf0);
    }
    memory_write(0x281,0);
    a26_core_set_controls(true,false,true,false,true,true,true);
    CHECK(memory_read(0x280)==0xaf);
    CHECK((memory_read(0x282)&3)==0);
    CHECK(input_INPT4_read()==0);
    video_VBLANK_write(0x40);
    CHECK(input_INPT4_read()==0);
    a26_core_set_controls(false,false,false,false,false,false,false);
    CHECK(input_INPT4_read()==0);
    video_VBLANK_write(0); CHECK(input_INPT4_read()==0x80);
    // A press while latching is disabled must not contaminate the next latch.
    a26_core_set_controls(false,false,false,false,true,false,false);
    a26_core_set_controls(false,false,false,false,false,false,false);
    video_VBLANK_write(0x40); CHECK(input_INPT4_read()==0x80);
    for(unsigned m=0;m<32;m+=8) {
        timer_primary_value=0x53;
        CHECK(memory_read(0x284+m)==0x53);
        CHECK(memory_read(0x286+m)==0x53);
    }
    timer_TIM64T_write(5);
    memory_write(0x284,0); CHECK(timer_primary_value==5 && timer_prescaler==64);
    memory_write(0x294,3); CHECK(timer_prescaler==1 && timer_primary_value==3);
    memory_write(0x29d,3); CHECK(timer_prescaler==8 && timer_primary_value==3);
    timer_tick(); CHECK(timer_primary_value==2);
    for(int i=0;i<7;++i) timer_tick();
    CHECK(timer_primary_value==2);
    timer_tick(); CHECK(timer_primary_value==1);
    timer_TIM1T_write(0); timer_tick();
    CHECK(timer_primary_value==255 && timer_underflow_bit7);
    CHECK(timer_INTIM_read()==255 && timer_underflow_bit7); // same underflow cycle
    timer_tick(); CHECK(timer_INTIM_read()==254 && !timer_underflow_bit7);
    timer_tick(); CHECK(timer_primary_value==253);
    timer_reset(); CHECK(!timer_underflow_bit6);
    memory_write(0x284,0); // negative PA7 edge
    a26_core_set_controls(false,false,false,true,false,false,false); timer_tick();
    CHECK((timer_INSTAT_read()&0x40)!=0); CHECK((timer_INSTAT_read()&0x40)==0);
    timer_TIM1T_write(0); timer_tick(); CHECK((timer_INSTAT_read()&0x40)==0);
}
static void mapper_tests(void) {
    const size_t sizes[]={2048,4096,8192,12288,16384,32768,65536};
    const int types[]={CARTRIDGE_2K,CARTRIDGE_4K,CARTRIDGE_F8,CARTRIDGE_FA,CARTRIDGE_F6,CARTRIDGE_F4,CARTRIDGE_F0};
    for(unsigned i=0;i<7;++i) {
        prepare(sizes[i]); CHECK(cartridge_detect(rom,sizes[i])==types[i]);
        CHECK(a26_core_load_rom(rom,sizes[i])); CHECK(cartridge.type==types[i]);
    }
    for(int variant=0;variant<6;++variant) {
        unsigned banks=2u<<(variant%3); size_t n=banks*4096u;
        prepare(n);
        if(variant>=3) for(size_t k=0;k<n;k+=4096) memcpy(rom+k+128,rom+k,128);
        CHECK(a26_core_load_rom(rom,n));
        unsigned first=banks==2?0x1ff8:banks==4?0x1ff6:0x1ff4;
        for(unsigned bank=0;bank<banks;++bank) {
            if(bank&1) memory_write(first+bank,0); else memory_read(first+bank);
            CHECK(cartridge.F8.current_bank==(int)bank);
            CHECK(memory_read(0x1234)==rom[bank*4096+0x234]);
        }
        if(variant>=3) {
            memory_write(0x1005,0x77); memory_write(0x80,0x6a);
            memory_read(0x1005); CHECK(memory_read(0x1085)==0x6a); // open-bus write-port read
        }
    }
    prepare(8192); rom[100]=0xad; rom[101]=0xe0; rom[102]=0x1f;
    CHECK(a26_core_load_rom(rom,8192)); CHECK(cartridge.type==CARTRIDGE_E0);
    memory_read(0x1fe3); CHECK(cartridge.E0.current_lower_bank==3);
    CHECK(memory_read(0x1000)==rom[3*1024]); CHECK(memory_read(0x1c00)==rom[7*1024]);
    prepare(16384); rom[100]=0xad; rom[101]=0xe7; rom[102]=0x1f;
    CHECK(a26_core_load_rom(rom,16384)); CHECK(cartridge.type==CARTRIDGE_E7);
    memory_write(0x1fe7,0); memory_write(0x1007,0x59);
    CHECK(memory_read(0x1407)==0x59);
    memory_read(0x1007); CHECK(memory_read(0x1407)==0x59); // read never zeroes RAM
    memory_write(0x1fea,0); memory_write(0x1803,0x93); CHECK(memory_read(0x1903)==0x93);
    CHECK(memory_read(0x1ffc)==rom[16380]);
    const unsigned e7sizes[]={8192,12288};
    for(unsigned i=0;i<2;++i) {
        size_t n=e7sizes[i]; prepare(n);
        memcpy(rom+100,i==0?"\xad\xe4\xff":"\xad\xe7\x1f",3);
        CHECK(a26_core_load_rom(rom,n)); CHECK(cartridge.type==CARTRIDGE_E7);
        memory_read(0x1fe6); CHECK(cartridge.E7.current_lower_bank==(i==0?2:4));
        CHECK(memory_read(0x1000)==rom[(i==0?2:4)*2048]);
        memory_read(0x1fe7); memory_write(0x1003,0x71); CHECK(memory_read(0x1403)==0x71);
        CHECK(memory_read(0x1ffc)==rom[n-4]);
    }
    for(int t=0;t<3;++t) {
        size_t n=8192u<<t; prepare(n);
        for(size_t bank=0;bank<n;bank+=4096) memcpy(rom+bank+128,rom+bank,128);
        CHECK(a26_core_load_rom(rom,n)); CHECK(cartridge.type==CARTRIDGE_F8SC+t);
        memory_write(0x1012,0x81); CHECK(memory_read(0x1092)==0x81);
        memory_read(0x1012); CHECK(memory_read(0x1092)==0x81);
    }
    prepare(65536); memcpy(rom+65528,"EFEF",4);
    CHECK(a26_core_load_rom(rom,65536)); CHECK(cartridge.type==CARTRIDGE_EF);
    for(unsigned bank=0;bank<16;++bank) {
        memory_read(0x1fe0+bank); CHECK(memory_read(0x1200)==rom[bank*4096+0x200]);
    }
    prepare(65536); CHECK(a26_core_load_rom(rom,65536));
    for(unsigned bank=0;bank<16;++bank) {
        memory_write(0x1ff0,0); CHECK(cartridge.F0.current_bank==(int)bank);
        CHECK(memory_read(0x1200)==rom[bank*4096+0x200]);
    }
    prepare(12288); CHECK(a26_core_load_rom(rom,12288));
    memory_write(0x1005,0x99); CHECK(memory_read(0x1105)==0x99);
    memory_read(0x1005); CHECK(memory_read(0x1105)==0x99);
    prepare(4096); memcpy(rom+100,"\x9d\xff\xf3",3);
    CHECK(a26_core_load_rom(rom,4096)); CHECK(cartridge.type==CARTRIDGE_CV);
    memory_write(0x1403,0x5a); CHECK(memory_read(0x1003)==0x5a);
    prepare(8192); memcpy(rom+100,"\x20\x00\xd0\xc6\xc5",5);
    CHECK(a26_core_load_rom(rom,8192)); CHECK(cartridge.type==CARTRIDGE_FE);
    memory_read(0x1fe); memory_write(0x1ff,0); CHECK(cartridge.FE.current_bank==1);
    memory_read(0x1fe); memory_write(0x1ff,0x20); CHECK(cartridge.FE.current_bank==0);
    CHECK(!a26_core_load_rom(rom,6144)); CHECK(!a26_core_load_rom(rom,10240));
    CHECK(!a26_core_load_rom(rom,65537)); CHECK(!a26_core_load_rom(rom,0));
    CHECK(!a26_core_load_rom(NULL,4096));
    prepare(4096); memmove(rom+128,rom,4096); memset(rom,0x99,128);
    CHECK(a26_core_load_rom(rom,4224)); CHECK(memory_read(0x1200)==rom[128+512]);
    prepare(65536); memmove(rom+128,rom,65536); memset(rom,0x99,128);
    CHECK(a26_core_load_rom(rom,65536+128)); CHECK(cartridge_size==65536);
}
static uint32_t audio_count, line_hash;
static void sample(void) { ++audio_count; (void)atari_audio_get_sample(5); }
static void line(int y,const uint16_t *pixels) {
    line_hash=line_hash*33+(unsigned)y;
    if(pixels) for(int x=0;x<160;++x) line_hash=line_hash*33+pixels[x];
}
static void frame_tests(void) {
    static const uint8_t endless[]={0x4c,0x00,0x10};
    load_program(endless,sizeof(endless)); audio_count=0;
    a26_frame_result_t f=a26_core_run_frame(line,sample);
    CHECK(f.reason==A26_FRAME_LIMIT && f.color_clocks==A26_FRAME_CLOCK_LIMIT);
    CHECK(audio_count==(uint64_t)f.color_clocks*A26_AUDIO_HZ/A26_NTSC_COLOR_HZ);
    static const uint8_t rsync[]={0x85,0x03,0x4c,0x00,0x10};
    load_program(rsync,sizeof(rsync));
    f=a26_core_run_frame(NULL,NULL); CHECK(f.reason==A26_FRAME_LIMIT && f.color_clocks==A26_FRAME_CLOCK_LIMIT);
    // Two VSYNC pulses with different delays; not a fixed 262-line budget.
    static const uint8_t sync[]={0xa9,2,0x85,0,0xa2,16,0xca,0xd0,0xfd,0xa9,0,0x85,0,0x4c,0,0x10};
    load_program(sync,sizeof(sync)); f=a26_core_run_frame(NULL,NULL);
    CHECK(f.reason==A26_FRAME_VSYNC && f.color_clocks<1000);
    uint32_t clocks=f.color_clocks;
    uint8_t longer[sizeof(sync)]; memcpy(longer,sync,sizeof(sync)); longer[5]=32;
    load_program(longer,sizeof(longer)); f=a26_core_run_frame(NULL,NULL);
    CHECK(f.reason==A26_FRAME_VSYNC && f.color_clocks>clocks);
    static const uint8_t jam[]={2}; load_program(jam,1);
    f=a26_core_run_frame(NULL,NULL); CHECK(f.reason==A26_FRAME_JAM && f.color_clocks<20);
    a26_core_reset(); CHECK(!cpu.is_killed && s_dw_active==0 && swacnt==0);
}
static void state_tests(void) {
    static const uint8_t code[]={0xa9,0xff,0x85,0x1b,0x85,0x0d,0xa9,0,0x85,1,0x4c,0,0x10};
    load_program(code,sizeof(code));
    size_t reset_n=a26_core_save_state(saved,sizeof(saved));
    cpu.is_killed=true; memory[4]=0x99; player0.main_sprite[3]=true;
    video_VBLANK_write(0x40); input_SWACNT_write(0xff);
    timer_TIM8T_write(0); timer_tick();
    write_after_delay(video_GRP0_write,true,0xff,3);
    atari_audio_set_reg(0x19,15); atari_audio_step();
    a26_core_reset();
    CHECK(a26_core_save_state(second,sizeof(second))==reset_n && !memcmp(saved,second,reset_n));
    video_VBLANK_write(0); video_GRP0_write(0x81);
    write_after_delay(video_GRP1_write,true,0x55,2);
    a26_core_set_controls(true,false,false,false,true,true,false);
    video_VBLANK_write(0x40); memory_write(0x294,93);
    atari_audio_set_reg(0x15,8); atari_audio_set_reg(0x19,15);
    size_t n=a26_core_save_state(saved,sizeof(saved)); CHECK(n>0 && n<=4096);
    line_hash=0; audio_count=0;
    a26_frame_result_t a=a26_core_run_frame(line,sample);
    uint32_t hash=line_hash, samples=audio_count;
    size_t m=a26_core_save_state(second,sizeof(second));
    CHECK(a26_core_load_state(saved,n)); line_hash=0; audio_count=0;
    a26_frame_result_t b=a26_core_run_frame(line,sample);
    CHECK(a.color_clocks==b.color_clocks && line_hash==hash && audio_count==samples);
    CHECK(a26_core_save_state(saved,sizeof(saved))==m && memcmp(saved,second,m)==0);
    saved[n-1]^=1; Cpu before=cpu;
    CHECK(!a26_core_load_state(saved,n)); CHECK(memcmp(&cpu,&before,sizeof(cpu))==0);
    CHECK(!a26_core_load_state(second,n-1));
    second[4]=3; CHECK(!a26_core_load_state(second,m));
    // Every mapper, including E7 RAM, must survive without union overwrite.
    for(int t=CARTRIDGE_2K;t<=CARTRIDGE_EFSC;++t) {
        size_t bytes=t==CARTRIDGE_E7?16384:65536;
        prepare(bytes); CHECK(a26_core_load_rom(rom,bytes));
        cartridge.type=t; cartridge_reset();
        memset(cartridge.sc_ram,0xa7,128);
        if(t==CARTRIDGE_E7) { cartridge.E7.current_lower_bank=7; memset(cartridge.E7.extra_ram,0x59,2048); }
        if(t==CARTRIDGE_FA) memset(cartridge.FA.extra_ram,0x71,256);
        n=a26_core_save_state(saved,sizeof(saved)); CHECK(n>0);
        memset(&cartridge.F8,0,cartridge_state_size(t)); memset(cartridge.sc_ram,0,128);
        CHECK(a26_core_load_state(saved,n));
        CHECK(a26_core_save_state(second,sizeof(second))==n && !memcmp(saved,second,n));
    }
    prepare(4096); CHECK(a26_core_load_rom(rom,4096)); n=a26_core_save_state(saved,sizeof(saved));
    rom[300]^=1; CHECK(a26_core_load_rom(rom,4096)); CHECK(!a26_core_load_state(saved,n));
}
static void audio_tests(void) {
    for(int mode=0;mode<16;++mode) {
        atari_audio_reset(); atari_audio_set_reg(0x15,mode); atari_audio_set_reg(0x19,15);
        unsigned changes=0; uint8_t previous=1;
        for(int i=0;i<4096;++i) {
            atari_audio_step(); atari_audio_state_t s; atari_audio_export(&s);
            CHECK(atari_audio_validate(&s)); changes+=previous!=s.channels[0].out; previous=s.channels[0].out;
        }
        CHECK((mode==0 || mode==11)?changes==0:changes>0);
    }
    atari_audio_reset(); atari_audio_set_reg(0x15,4); atari_audio_set_reg(0x17,3);
    atari_audio_state_t s; unsigned changes=0; uint8_t old=1;
    for(int i=0;i<800;++i) { atari_audio_step(); atari_audio_export(&s); changes+=s.channels[0].out!=old; old=s.channels[0].out; }
    CHECK(changes==200); // CPU clock/38/(AUDF+1)/2 for a square wave
    const unsigned period[16]={1,15,465,465,2,2,31,31,511,31,31,1,6,6,93,93};
    for(unsigned mode=0;mode<16;++mode) {
        atari_audio_reset(); atari_audio_set_reg(0x15,mode);
        uint8_t sequence[1022];
        for(unsigned i=0;i<period[mode]*2;++i) {
            atari_audio_step(); atari_audio_export(&s); sequence[i]=s.channels[0].out;
        }
        CHECK(!memcmp(sequence,sequence+period[mode],period[mode]));
    }
    atari_audio_reset(); atari_audio_set_reg(0x19,15);
    int16_t output=0; for(int i=0;i<8000;++i) output=atari_audio_get_sample(5);
    CHECK(abs(output)<256); CHECK(atari_audio_get_sample(0)==0);
}
int main(void) {
    a26_core_init(); riot_tests(); mapper_tests(); frame_tests(); state_tests(); audio_tests();
    printf("core: %u checks passed\n",checks); return 0;
}
