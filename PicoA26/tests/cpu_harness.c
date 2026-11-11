#include "atari2600emulator.h"
#include <string.h>
static uint8_t ram[65536];
static unsigned count;
static unsigned trace[16][3];
uint8_t memory_read(uint16_t a) {
    if(count<16) { trace[count][0]=a; trace[count][1]=ram[a]; trace[count][2]=0; }
    ++count; return ram[a];
}
void memory_write(uint16_t a,uint8_t v) {
    if(count<16) { trace[count][0]=a; trace[count][1]=v; trace[count][2]=1; }
    ++count; ram[a]=v;
}
void vector_init(unsigned pc,unsigned a,unsigned x,unsigned y,unsigned s,unsigned p) {
    memset(&cpu,0,sizeof(cpu)); memset(ram,0,sizeof(ram)); count=0;
    cpu.pc=pc; cpu.a=a; cpu.x=x; cpu.y=y; cpu.s=s; cpu.cycle=1;
    cpu.flag_c=p&1; cpu.flag_z=p&2; cpu.flag_i=p&4; cpu.flag_d=p&8;
    cpu.flag_v=p&64; cpu.flag_n=p&128;
}
void vector_poke(unsigned a,unsigned v) { ram[a&65535]=v; }
unsigned vector_peek(unsigned a) { return ram[a&65535]; }
unsigned vector_run(void) {
    unsigned n=0;
    do { cpu_run_for_one_machine_cycle(); ++n; } while(cpu.cycle!=1 && !cpu.is_killed && n<12);
    return n;
}
unsigned vector_register(unsigned n) {
    unsigned regs[]={cpu.pc,cpu.a,cpu.x,cpu.y,cpu.s,
        cpu.flag_c|(cpu.flag_z<<1)|(cpu.flag_i<<2)|(cpu.flag_d<<3)|(cpu.flag_v<<6)|(cpu.flag_n<<7)};
    return n<6?regs[n]:0;
}
unsigned vector_trace(unsigned cycle,unsigned field) { return cycle<16 && field<3?trace[cycle][field]:0; }
