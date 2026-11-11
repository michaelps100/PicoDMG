#include "platform.h"
#include "core_state.h"

unsigned char timer_primary_value = 0xff;
int timer_secondary_value = 1024;
int timer_prescaler = 1024;
bool timer_underflow_bit7, timer_underflow_bit6;
bool timer_edge_positive, timer_pa7_last = true;
bool timer_wrapped_this_cycle;

void timer_reset(void) {
    timer_primary_value=0xff; timer_secondary_value=timer_prescaler=1024;
    timer_underflow_bit7=timer_underflow_bit6=timer_edge_positive=false;
    timer_wrapped_this_cycle=false; timer_pa7_last=true;
}
uint8_t A26_RAMFUNC(timer_INTIM_read)(void) {
    if (!timer_wrapped_this_cycle) timer_underflow_bit7=false;
    return timer_primary_value;
}
uint8_t A26_RAMFUNC(timer_INSTAT_read)(void) {
    uint8_t v=(timer_underflow_bit7?0x80:0)|(timer_underflow_bit6?0x40:0);
    timer_underflow_bit6=false;
    return v;
}
void A26_RAMFUNC(timer_edge_control)(uint8_t value) { timer_edge_positive=(value&1)!=0; }
void A26_RAMFUNC(timer_sample_port)(void) {
    bool pin=(input_SWCHA_read()&0x80)!=0;
    if(pin!=timer_pa7_last && pin==timer_edge_positive) timer_underflow_bit6=true;
    timer_pa7_last=pin;
}
static inline void set_timer(uint8_t value, int divider) {
    timer_primary_value=value; timer_secondary_value=1; timer_prescaler=divider;
    if(!timer_wrapped_this_cycle) timer_underflow_bit7=false;
}
void A26_RAMFUNC(timer_TIM1T_write)(uint8_t v) { set_timer(v,1); }
void A26_RAMFUNC(timer_TIM8T_write)(uint8_t v) { set_timer(v,8); }
void A26_RAMFUNC(timer_TIM64T_write)(uint8_t v) { set_timer(v,64); }
void A26_RAMFUNC(timer_T1024T_write)(uint8_t v) { set_timer(v,1024); }
void __not_in_flash_func(timer_tick_underflow)(void) {
    --timer_primary_value;
    if(timer_primary_value==255) { timer_underflow_bit7=true; timer_wrapped_this_cycle=true; }
    timer_secondary_value=timer_underflow_bit7?1:timer_prescaler;
}
void __not_in_flash_func(timer_tick)(void) {
    timer_wrapped_this_cycle=false;
    if(--timer_secondary_value<=0) timer_tick_underflow();
}
