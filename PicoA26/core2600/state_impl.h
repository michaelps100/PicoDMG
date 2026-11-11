/* Included only by a26_core.c: keeps the beam/audio phases private to the core. */
#include <stddef.h>

typedef struct {
    uint32_t magic, version, length, rom_size, rom_crc, mapper, crc;
} A26StateHeader;
typedef struct {
    Cpu cpu;
    Memory ram;
    Player0 p0; Player1 p1; Missile0 m0; Missile1 m1; Ball ball; Playfield pf;
    TV tv;
    int priority, timer_secondary, timer_prescale;
    uint8_t timer_primary, phi2, audio_divider, bus;
    bool timer7, timer6, edge_positive, pa7_last, wrapped;
    bool vsync, vblank, wsync, fe_pending, line_active;
    uint32_t sample_phase;
    int region_hint, region_candidate;
    uint16_t capture_height;
    uint8_t region_votes, color_regs[4];
    uint64_t total_clocks;
    uint16_t line[160];
    A26InputState input;
    A26DelayedState delayed;
    atari_audio_state_t audio;
    /* Only the active mapper bytes follow this prefix. */
} A26StateBody;
_Static_assert(sizeof(A26StateHeader)+sizeof(A26StateBody)+2056+128<=A26_STATE_MAX_SIZE,
    "State storage must fit the static file buffer");
static A26StateBody s_snapshot;
static Cartridge s_restore_cart;

static uint32_t state_crc(const uint8_t *data,size_t n) {
    uint32_t crc=~0u;
    for(size_t i=0;i<n;++i) {
        crc^=data[i];
        for(int j=0;j<8;++j) crc=(crc>>1)^((0u-(crc&1))&0xedb88320u);
    }
    return ~crc;
}
static size_t state_mapper_bytes(void) {
    return cartridge_state_size(cartridge.type)+(cartridge.type>=CARTRIDGE_F8SC?128:0);
}
size_t a26_core_save_state(uint8_t *dest,size_t max_size) {
    size_t cart_bytes=state_mapper_bytes();
    size_t n=sizeof(A26StateHeader)+sizeof(s_snapshot)+cart_bytes;
    if(!dest || !s_cart_size || n>max_size || n>A26_STATE_MAX_SIZE) return 0;
    memset(&s_snapshot,0,sizeof(s_snapshot));
    video_sync_collision_flags();
    s_snapshot.cpu=cpu; memcpy(s_snapshot.ram,memory,sizeof(memory));
    s_snapshot.p0=player0; s_snapshot.p1=player1; s_snapshot.m0=missile0;
    s_snapshot.m1=missile1; s_snapshot.ball=ball; s_snapshot.pf=playfield;
    s_snapshot.tv=tv; s_snapshot.priority=video_priority;
    s_snapshot.timer_secondary=timer_secondary_value; s_snapshot.timer_prescale=timer_prescaler;
    s_snapshot.timer_primary=timer_primary_value; s_snapshot.timer7=timer_underflow_bit7;
    s_snapshot.timer6=timer_underflow_bit6; s_snapshot.edge_positive=timer_edge_positive;
    s_snapshot.pa7_last=timer_pa7_last; s_snapshot.wrapped=timer_wrapped_this_cycle;
    s_snapshot.phi2=s_phi2_cnt; s_snapshot.audio_divider=s_audio_divider;
    s_snapshot.sample_phase=s_sample_phase; s_snapshot.total_clocks=s_color_clocks_total;
    s_snapshot.region_hint=s_region_hint; s_snapshot.region_candidate=s_region_candidate;
    s_snapshot.capture_height=s_capture_height; s_snapshot.region_votes=s_region_votes;
    memcpy(s_snapshot.color_regs,video_color_regs,4);
    s_snapshot.vsync=vsync; s_snapshot.vblank=vblank; s_snapshot.wsync=wsync;
    s_snapshot.line_active=s_line_active;
    s_snapshot.fe_pending=cartridge.fe_pending; s_snapshot.bus=cartridge_bus;
    memcpy(s_snapshot.line,s_line_buf,sizeof(s_line_buf));
    input_export_state(&s_snapshot.input); delayed_writer_export(&s_snapshot.delayed);
    atari_audio_export(&s_snapshot.audio);
    uint8_t *body=dest+sizeof(A26StateHeader);
    memcpy(body,&s_snapshot,sizeof(s_snapshot));
    size_t mapper_n=cartridge_state_size(cartridge.type);
    memcpy(body+sizeof(s_snapshot),&cartridge.F8,mapper_n);
    if(cart_bytes>mapper_n) memcpy(body+sizeof(s_snapshot)+mapper_n,cartridge.sc_ram,128);
    A26StateHeader header={0x53363241u,6,(uint32_t)n,(uint32_t)s_cart_size,s_rom_crc,
        (uint32_t)cartridge.type,state_crc(body,n-sizeof(A26StateHeader))};
    memcpy(dest,&header,sizeof(header));
    return n;
}
static bool state_sprite_sizes(void) {
    return (s_snapshot.p0.size==8 || s_snapshot.p0.size==16 || s_snapshot.p0.size==32) &&
        (s_snapshot.p1.size==8 || s_snapshot.p1.size==16 || s_snapshot.p1.size==32) &&
        (s_snapshot.m0.size==1 || s_snapshot.m0.size==2 || s_snapshot.m0.size==4 || s_snapshot.m0.size==8) &&
        (s_snapshot.m1.size==1 || s_snapshot.m1.size==2 || s_snapshot.m1.size==4 || s_snapshot.m1.size==8) &&
        (s_snapshot.ball.size==1 || s_snapshot.ball.size==2 || s_snapshot.ball.size==4 || s_snapshot.ball.size==8);
}
bool a26_core_load_state(const uint8_t *src,size_t n) {
    A26StateHeader header;
    size_t expected=sizeof(header)+sizeof(s_snapshot)+state_mapper_bytes();
    if(!src || !s_cart_size || n!=expected || n>A26_STATE_MAX_SIZE) return false;
    memcpy(&header,src,sizeof(header));
    const uint8_t *body=src+sizeof(header);
    if(header.magic!=0x53363241u || header.version!=6 || header.length!=n ||
        header.rom_size!=s_cart_size || header.rom_crc!=s_rom_crc ||
        header.mapper!=(uint32_t)cartridge.type ||
        header.crc!=state_crc(body,n-sizeof(header))) return false;
    memcpy(&s_snapshot,body,sizeof(s_snapshot));
    memset(&s_restore_cart,0,sizeof(s_restore_cart));
    s_restore_cart.type=cartridge.type;
    size_t mapper_n=cartridge_state_size(cartridge.type);
    memcpy(&s_restore_cart.F8,body+sizeof(s_snapshot),mapper_n);
    if(state_mapper_bytes()>mapper_n)
        memcpy(s_restore_cart.sc_ram,body+sizeof(s_snapshot)+mapper_n,128);
    if(!cartridge_validate_state(&s_restore_cart) || !state_sprite_sizes() ||
        !atari_audio_validate(&s_snapshot.audio) || !delayed_writer_validate(&s_snapshot.delayed) ||
        s_snapshot.phi2>2 || s_snapshot.audio_divider>=114 ||
        s_snapshot.sample_phase>=(s_snapshot.tv.standard==TV_NTSC?A26_NTSC_COLOR_HZ:A26_PAL_COLOR_HZ) ||
        s_snapshot.region_hint < -1 || s_snapshot.region_hint>TV_SECAM ||
        s_snapshot.region_candidate<TV_NTSC || s_snapshot.region_candidate>TV_SECAM ||
        s_snapshot.capture_height<192 || s_snapshot.capture_height>320 || s_snapshot.region_votes>8 ||
        s_snapshot.cpu.cycle<1 || s_snapshot.cpu.cycle>8 ||
        s_snapshot.tv.x<0 || s_snapshot.tv.x>=228 || s_snapshot.tv.y<0 || s_snapshot.tv.y>1000000 ||
        s_snapshot.tv.standard<TV_NTSC || s_snapshot.tv.standard>TV_SECAM ||
        (s_snapshot.timer_prescale!=1 && s_snapshot.timer_prescale!=8 &&
         s_snapshot.timer_prescale!=64 && s_snapshot.timer_prescale!=1024) ||
        s_snapshot.timer_secondary<1 || s_snapshot.timer_secondary>1024 ||
        s_snapshot.priority<0 || s_snapshot.priority>2) return false;
    /* No emulated state is changed until all validation has succeeded. */
    cpu=s_snapshot.cpu; memcpy(memory,s_snapshot.ram,sizeof(memory));
    s_restore_cart.raw=cartridge.raw; s_restore_cart.fe_pending=s_snapshot.fe_pending;
    cartridge=s_restore_cart; cartridge_bus=s_snapshot.bus;
    player0=s_snapshot.p0; player1=s_snapshot.p1; missile0=s_snapshot.m0;
    missile1=s_snapshot.m1; ball=s_snapshot.ball; playfield=s_snapshot.pf;
    video_priority=s_snapshot.priority; tv=s_snapshot.tv;
    video_restore_collision_bits();
    timer_primary_value=s_snapshot.timer_primary; timer_secondary_value=s_snapshot.timer_secondary;
    timer_prescaler=s_snapshot.timer_prescale; timer_underflow_bit7=s_snapshot.timer7;
    timer_underflow_bit6=s_snapshot.timer6; timer_edge_positive=s_snapshot.edge_positive;
    timer_pa7_last=s_snapshot.pa7_last; timer_wrapped_this_cycle=s_snapshot.wrapped;
    s_phi2_cnt=s_snapshot.phi2; s_audio_divider=s_snapshot.audio_divider;
    s_sample_phase=s_snapshot.sample_phase; s_color_clocks_total=s_snapshot.total_clocks;
    s_region_hint=s_snapshot.region_hint; s_region_candidate=s_snapshot.region_candidate;
    s_capture_height=s_snapshot.capture_height; s_region_votes=s_snapshot.region_votes;
    memcpy(video_color_regs,s_snapshot.color_regs,4);
    vsync=s_snapshot.vsync; vblank=s_snapshot.vblank; wsync=s_snapshot.wsync;
    s_line_active=s_snapshot.line_active;
    memcpy(s_line_buf,s_snapshot.line,sizeof(s_line_buf));
    a26_video_rebase();
    input_import_state(&s_snapshot.input); delayed_writer_import(&s_snapshot.delayed);
    atari_audio_import(&s_snapshot.audio); video_invalidate_cache();
    return true;
}
