#include "atari2600emulator.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t ram[65536];
static unsigned checks, bus_count;
typedef struct { uint16_t address; uint8_t value; bool write; } Bus;
static Bus bus[16];
#define CHECK(condition) do { ++checks; if(!(condition)) { fprintf(stderr,"cpu:%d: %s (op=%02x A=%02x X=%02x Y=%02x PC=%04x)\n",__LINE__,#condition,cpu.opcode,cpu.a,cpu.x,cpu.y,cpu.pc); abort(); } } while(0)
uint8_t memory_read(uint16_t a) {
    if(bus_count<16) bus[bus_count]=(Bus){a,ram[a],false};
    ++bus_count;
    return ram[a];
}
void memory_write(uint16_t a,uint8_t v) {
    if(bus_count<16) bus[bus_count]=(Bus){a,v,true};
    ++bus_count;
    ram[a]=v;
}
static void init(uint8_t op) {
    memset(&cpu,0,sizeof(cpu)); cpu.pc=0x1000; cpu.s=0xfd; cpu.cycle=1;
    ram[0x1000]=op; bus_count=0;
}
static int instruction(void) {
    int n=0;
    do { cpu_run_for_one_machine_cycle(); ++n; } while(cpu.cycle!=1 && !cpu.is_killed && n<12);
    CHECK(n<12); return n;
}
static void arithmetic(void) {
    for(unsigned a=0;a<256;++a) for(unsigned b=0;b<256;++b) for(unsigned carry=0;carry<2;++carry) {
        init(0x69); cpu.a=a; cpu.flag_c=carry; ram[0x1001]=b;
        CHECK(instruction()==2);
        unsigned sum=a+b+carry; uint8_t v=sum;
        CHECK(cpu.a==v && cpu.flag_c==(sum>255) && cpu.flag_z==(v==0) && cpu.flag_n==(v>127));
        CHECK(cpu.flag_v==((~(a^b)&(a^v)&0x80)!=0));
        init(0xe9); cpu.a=a; cpu.flag_c=carry;
        CHECK(instruction()==2);
        int difference=(int)a-b-(1-carry); v=difference;
        CHECK(cpu.a==v && cpu.flag_c==(difference>=0) && cpu.flag_z==(v==0) && cpu.flag_n==(v>127));
        CHECK(cpu.flag_v==(((a^b)&(a^v)&0x80)!=0));
    }
    for(unsigned a=0;a<100;++a) for(unsigned b=0;b<100;++b) for(unsigned carry=0;carry<2;++carry) {
        uint8_t aa=(a/10)*16+a%10, bb=(b/10)*16+b%10;
        init(0x69); cpu.a=aa; cpu.flag_c=carry; cpu.flag_d=true; ram[0x1001]=bb;
        instruction(); unsigned value=(a+b+carry)%100;
        CHECK(cpu.a==(value/10)*16+value%10 && cpu.flag_c==(a+b+carry>=100));
        init(0xe9); cpu.a=aa; cpu.flag_c=carry; cpu.flag_d=true;
        instruction(); int diff=(int)a-b-(1-carry); value=(diff+100)%100;
        CHECK(cpu.a==(value/10)*16+value%10 && cpu.flag_c==(diff>=0));
    }
}
static void dispatch(void) {
    for(unsigned op=0;op<256;++op) {
        memset(ram,0,sizeof(ram)); init(op); cpu.a=0x55; cpu.x=1; cpu.y=1;
        ram[0x1001]=0x80; ram[0x1002]=0x20; ram[0x80]=0x34; ram[0x81]=0x20;
        int n=instruction(); CHECK(n>=2 && n<=8);
        bool jam=(op&15)==2 && op!=0x82 && op!=0xa2 && op!=0xc2 && op!=0xe2;
        CHECK(cpu.is_killed==jam);
    }
}
static void bus_and_flags(void) {
    init(0x91); ram[0x1001]=0x80; ram[0x80]=0xff; ram[0x81]=0x20;
    cpu.y=1; cpu.a=0x5a; CHECK(instruction()==6);
    CHECK(bus_count==6 && bus[4].address==0x2000 && !bus[4].write);
    CHECK(bus[5].address==0x2100 && bus[5].write && ram[0x2100]==0x5a);
    init(0xb1); cpu.y=1; CHECK(instruction()==6 && cpu.a==0x5a);
    init(0x00); ram[0xfffe]=0x56; ram[0xffff]=0x34; cpu.flag_c=true;
    CHECK(instruction()==7 && cpu.pc==0x3456 && cpu.flag_i && cpu.flag_c);
    CHECK(ram[0x1fd]==0x10 && ram[0x1fc]==2 && (ram[0x1fb]&0x30)==0x30);
    init(0x6c); ram[0x1001]=0xff; ram[0x1002]=0x20; ram[0x20ff]=0x34; ram[0x2000]=0x12;
    CHECK(instruction()==5 && cpu.pc==0x1234); // NMOS JMP indirect page wrap
    init(0x6b); cpu.a=0xff; ram[0x1001]=0xff;
    CHECK(instruction()==2 && cpu.a==0x7f && cpu.flag_c && !cpu.flag_v);
    init(0x8b); cpu.a=0; cpu.x=0xff; ram[0x1001]=0xff;
    CHECK(instruction()==2 && cpu.a==0xee && cpu.pc==0x1002);
    init(0xab); cpu.a=0; ram[0x1001]=0xff;
    CHECK(instruction()==2 && cpu.a==0xee && cpu.x==0xee);
    const uint8_t stores[]={0x9b,0x9c,0x9e,0x9f};
    for(unsigned i=0;i<4;++i) {
        init(stores[i]); ram[0x1001]=0xff; ram[0x1002]=0x20;
        cpu.a=0xff; cpu.x=cpu.y=1;
        CHECK(instruction()==5); CHECK(bus_count==5 && bus[4].write);
        CHECK(bus[4].address==0x0100 && bus[4].value==1);
    }
}
int main(void) {
    arithmetic(); dispatch(); bus_and_flags();
    printf("cpu: %u checks passed\n",checks); return 0;
}
