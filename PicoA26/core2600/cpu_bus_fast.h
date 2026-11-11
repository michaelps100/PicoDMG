/* Common bus paths for the 6507 interpreter. Special mappers retain the
 * ordinary decoder and its bus-observation side effects. */
#ifndef A26_CPU_BUS_FAST_H
#define A26_CPU_BUS_FAST_H
#include "core_state.h"
#include "platform.h"
static uint8_t A26_RAMFUNC(cpu_bus_read)(uint16_t address) {
    uint8_t value;
    if ((address&0x1000) && (unsigned)cartridge.type<=CARTRIDGE_4K && cartridge.raw) {
        value=cartridge.raw[address & (cartridge.type==CARTRIDGE_2K?2047:4095)];
    } else if ((address&0x1280)==0x0080 && cartridge.type!=CARTRIDGE_FE) {
        value=memory[address&127];
    } else return memory_read(address);
    cartridge_bus=value;
    return value;
}
static void A26_RAMFUNC(cpu_bus_write)(uint16_t address,uint8_t value) {
    if ((address&0x1280)==0x0080 && cartridge.type!=CARTRIDGE_FE) {
        cartridge_bus=value;
        memory[address&127]=value;
    } else memory_write(address,value);
}
#define memory_read cpu_bus_read
#define memory_write cpu_bus_write
#endif
