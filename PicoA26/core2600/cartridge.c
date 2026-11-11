#include "platform.h"
#include "core_state.h"
#include <string.h>
#include <stddef.h>

size_t cartridge_size;
uint8_t cartridge_bus;

/* Detection is a heuristic. Exact sizes are required; special hardware must
 * never be silently mapped as an ordinary F8/F6/F4 cartridge. References and
 * provenance are recorded in docs/EMULATION_FIXES.md. No Stella code is linked. */
static bool contains(const uint8_t *rom, size_t n, const void *sig, size_t len) {
    for (size_t i = 0; i + len <= n; ++i)
        if (!memcmp(rom + i, sig, len)) return true;
    return false;
}
static bool absolute_access(const uint8_t *rom, size_t n, unsigned lo, unsigned hi) {
    for (size_t i = 0; i + 2 < n; ++i) {
        uint8_t op = rom[i];
        if (op != 0xad && op != 0x8d && op != 0x2c && op != 0x0c) continue;
        unsigned addr = (rom[i+1] | (rom[i+2] << 8)) & 0x1fff;
        if (addr >= lo && addr <= hi) return true;
    }
    return false;
}
static bool signatures3(const uint8_t *rom,size_t n,const uint8_t (*patterns)[3],size_t count) {
    for(size_t i=0;i<count;++i) if(contains(rom,n,patterns[i],3)) return true;
    return false;
}
static bool is_e0(const uint8_t *rom,size_t n) {
    static const uint8_t code[][3]={{0x8d,0xe0,0x1f},{0x8d,0xe0,0x5f},
        {0x8d,0xe9,0xff},{0x0c,0xe0,0x1f},{0xad,0xe0,0x1f},
        {0xad,0xe9,0xff},{0xad,0xed,0xff},{0xad,0xf3,0xbf}};
    return signatures3(rom,n,code,sizeof(code)/3);
}
static bool is_e7(const uint8_t *rom,size_t n) {
    static const uint8_t code[][3]={{0xad,0xe2,0xff},{0xad,0xe5,0xff},
        {0xad,0xe5,0x1f},{0xad,0xe7,0x1f},{0x0c,0xe7,0x1f},
        {0x8d,0xe7,0xff},{0x8d,0xe7,0x1f}};
    return signatures3(rom,n,code,sizeof(code)/3);
}
static bool is_sc(const uint8_t *rom, size_t n) {
    for (size_t bank = 0; bank < n; bank += 4096)
        if (memcmp(rom + bank, rom + bank + 128, 128)) return false;
    return true;
}
int cartridge_detect(const uint8_t *rom, size_t n) {
    if (!rom) return -1;
    if (n >= 10240 && n <= 10496) return -2; /* DPC (Pitfall II) */
    if (n != 2048 && n != 4096 && n != 8192 && n != 12288 &&
        n != 16384 && n != 32768 && n != 65536) return -1;
    unsigned cdf=0,dpc=0;
    for(size_t i=0;i+4<=n;++i) {
        cdf+=memcmp(rom+i,"CDF",3)==0;
        dpc+=memcmp(rom+i,"DPC+",4)==0;
    }
    if(cdf>=3 || dpc>=2 || contains(rom,n,"MDMC",4)) return -2;
    unsigned three_e = 0, three_f = 0;
    for (size_t i=0; i+1<n; ++i) {
        three_e += rom[i]==0x85 && rom[i+1]==0x3e;
        three_f += rom[i]==0x85 && rom[i+1]==0x3f;
    }
    if (three_e >= 2 || three_f >= 2) return -2;
    if (n <= 4096) {
        static const uint8_t cv0[]={0x9d,0xff,0xf3}, cv1[]={0x99,0x00,0xf4};
        if(contains(rom,n,cv0,3) || contains(rom,n,cv1,3)) return CARTRIDGE_CV;
        return n == 2048 ? CARTRIDGE_2K : CARTRIDGE_4K;
    }
    if (n == 8192) {
        if (is_sc(rom,n)) return CARTRIDGE_F8SC;
        static const uint8_t e78[][3]={{0xad,0xe4,0xff},{0xad,0xe5,0xff},{0xad,0xe6,0xff}};
        if(signatures3(rom,n,e78,3)) return CARTRIDGE_E7;
        if(is_e0(rom,n)) return CARTRIDGE_E0;
        const uint8_t fe[][5] = {
            {0x20,0x00,0xd0,0xc6,0xc5}, {0x20,0xc3,0xf8,0xa5,0x82},
            {0xd0,0xfb,0x20,0x73,0xfe}, {0xd0,0xfb,0x20,0x68,0xfe},
            {0x20,0x00,0xf0,0x84,0xd6}
        };
        for (size_t i=0;i<sizeof(fe)/sizeof(fe[0]);++i)
            if (contains(rom,n,fe[i],5) && !absolute_access(rom,n,0x1ff8,0x1ff9))
                return CARTRIDGE_FE;
        return CARTRIDGE_F8;
    }
    if (n == 12288 || n == 16384) {
        if(n==16384 && is_sc(rom,n)) return CARTRIDGE_F6SC;
        if(is_e7(rom,n)) return CARTRIDGE_E7;
        return n==12288 ? CARTRIDGE_FA : CARTRIDGE_F6;
    }
    if (n == 32768) return is_sc(rom,n) ? CARTRIDGE_F4SC : CARTRIDGE_F4;
    if(contains(rom+n-8,8,"EFFB",4)) return -2;
    if(contains(rom+n-8,8,"EFSC",4)) return CARTRIDGE_EFSC;
    if(contains(rom+n-8,8,"EFEF",4)) return CARTRIDGE_EF;
    static const uint8_t efcode[][3]={{0xad,0xe0,0xff},{0x0c,0xe0,0xff},
        {0xad,0xe0,0x1f},{0x0c,0xe0,0x1f}};
    bool ef=signatures3(rom,n,efcode,4);
    return ef ? (is_sc(rom,n) ? CARTRIDGE_EFSC : CARTRIDGE_EF) : CARTRIDGE_F0;
}
const char *cartridge_name(int type) {
    static const char *const names[] = {"2K","4K","CV","F8","F6","F4","FE",
        "E0","FA","E7","F0","EF","F8SC","F6SC","F4SC","EFSC"};
    return type >= 0 && type < (int)(sizeof(names)/sizeof(names[0])) ? names[type] : "unsupported";
}
static bool sc_type(int t) { return t >= CARTRIDGE_F8SC && t <= CARTRIDGE_EFSC; }
static int bank_count(int t) {
    switch(t) {
        case CARTRIDGE_F8: case CARTRIDGE_F8SC: case CARTRIDGE_FE: return 2;
        case CARTRIDGE_FA: return 3;
        case CARTRIDGE_F6: case CARTRIDGE_F6SC: return 4;
        case CARTRIDGE_F4: case CARTRIDGE_F4SC: return 8;
        case CARTRIDGE_F0: case CARTRIDGE_EF: case CARTRIDGE_EFSC: return 16;
        default: return 0;
    }
}
void cartridge_reset(void) {
    /* Keep the ROM/type; clear all mapper RAM and latches. */
    memset(&cartridge.F8,0,sizeof(Cartridge)-offsetof(Cartridge,F8));
    memset(cartridge.sc_ram,0,sizeof(cartridge.sc_ram));
    cartridge.fe_pending = false;
    cartridge_bus = 0;
    int banks = bank_count(cartridge.type);
    cartridge.F8.current_bank = banks ? banks-1 : 0;
    if (cartridge.type == CARTRIDGE_FA || cartridge.type == CARTRIDGE_FE)
        cartridge.F8.current_bank = 0;
    if (cartridge.type == CARTRIDGE_E7) {
        cartridge.E7.current_lower_bank = 0;
        cartridge.E7.current_upper_bank = 0;
    }
}
static uint8_t rom_read(size_t offset) {
    return cartridge.raw && offset < cartridge_size ? cartridge.raw[offset] : cartridge_bus;
}
static void switch_bank(unsigned a) {
    int t=cartridge.type;
    if ((t==CARTRIDGE_F8 || t==CARTRIDGE_F8SC) && a>=0x1ff8 && a<=0x1ff9)
        cartridge.F8.current_bank=a-0x1ff8;
    else if ((t==CARTRIDGE_F6 || t==CARTRIDGE_F6SC) && a>=0x1ff6 && a<=0x1ff9)
        cartridge.F8.current_bank=a-0x1ff6;
    else if ((t==CARTRIDGE_F4 || t==CARTRIDGE_F4SC) && a>=0x1ff4 && a<=0x1ffb)
        cartridge.F8.current_bank=a-0x1ff4;
    else if (t==CARTRIDGE_FA && a>=0x1ff8 && a<=0x1ffa)
        cartridge.FA.current_bank=a-0x1ff8;
    else if ((t==CARTRIDGE_EF || t==CARTRIDGE_EFSC) && a>=0x1fe0 && a<=0x1fef)
        cartridge.F8.current_bank=a-0x1fe0;
    else if (t==CARTRIDGE_F0 && a==0x1ff0)
        cartridge.F0.current_bank=(cartridge.F0.current_bank+1)&15;
    else if (t==CARTRIDGE_E0) {
        if(a>=0x1fe0 && a<=0x1fe7) cartridge.E0.current_lower_bank=a&7;
        if(a>=0x1fe8 && a<=0x1fef) cartridge.E0.current_middle_bank=a&7;
        if(a>=0x1ff0 && a<=0x1ff7) cartridge.E0.current_upper_bank=a&7;
    } else if (t==CARTRIDGE_E7) {
        if(a>=0x1fe0 && a<=0x1fe7) {
            unsigned count=cartridge_size/2048;
            if(count==8) cartridge.E7.current_lower_bank=a&7;
            else if(count==4 && a>=0x1fe4) cartridge.E7.current_lower_bank=a&3;
            else if(count==6) {
                unsigned select=a&7;
                cartridge.E7.current_lower_bank=select<2?select:select-2;
            }
        }
        if(a>=0x1fe8 && a<=0x1feb) cartridge.E7.current_upper_bank=a&3;
    }
}
static uint8_t __not_in_flash_func(access)(unsigned a, bool write, uint8_t value) {
    a &= 0x1fff;
    if (!(a & 0x1000)) return cartridge_bus;
    switch_bank(a);
    int t=cartridge.type;
    if(sc_type(t)) {
        if(a<0x1080) { cartridge.sc_ram[a&127]=write?value:cartridge_bus; return cartridge_bus; }
        if(a<0x1100) return cartridge.sc_ram[a&127];
    }
    if(t==CARTRIDGE_CV) {
        if(a>=0x1800) return rom_read((cartridge_size-2048)+(a&2047));
        if(a>=0x1400) { cartridge.CV.extra_ram[a&1023]=write?value:cartridge_bus; return cartridge_bus; }
        return cartridge.CV.extra_ram[a&1023];
    }
    if(t==CARTRIDGE_FA) {
        if(a<0x1100) { cartridge.FA.extra_ram[a&255]=write?value:cartridge_bus; return cartridge_bus; }
        if(a<0x1200) return cartridge.FA.extra_ram[a&255];
    }
    if(t==CARTRIDGE_E7) {
        if(a<0x1800) {
            unsigned bank=cartridge.E7.current_lower_bank;
            unsigned count=cartridge_size/2048;
            if(bank==count-1) {
                if(a<0x1400) { cartridge.E7.extra_ram[a&1023]=write?value:cartridge_bus; return cartridge_bus; }
                return cartridge.E7.extra_ram[a&1023];
            }
            return rom_read(bank*2048+(a&2047));
        }
        unsigned ram=1024+cartridge.E7.current_upper_bank*256+(a&255);
        if(a<0x1900) { cartridge.E7.extra_ram[ram]=write?value:cartridge_bus; return cartridge_bus; }
        if(a<0x1a00) return cartridge.E7.extra_ram[ram];
        return rom_read(cartridge_size-2048+(a&2047));
    }
    if(t==CARTRIDGE_E0) {
        unsigned segment=(a>>10)&3;
        int bank=segment==0 ? cartridge.E0.current_lower_bank : segment==1 ?
            cartridge.E0.current_middle_bank : segment==2 ? cartridge.E0.current_upper_bank : 7;
        return rom_read(bank*1024+(a&1023));
    }
    if(t==CARTRIDGE_2K) return rom_read(a&2047);
    if(t==CARTRIDGE_4K) return rom_read(a&4095);
    int count=bank_count(t), bank=cartridge.F8.current_bank;
    if(count && bank>=0 && bank<count) return rom_read(bank*4096+(a&4095));
    return cartridge_bus;
}
uint8_t __not_in_flash_func(cartridge_read)(uint16_t a) {
    /* Ordinary ROM fetches dominate CPU bus traffic. Keep their bank window
     * in SRAM code; the general mapper decoder remains for cartridge RAM and
     * segmented/special boards. Hotspot reads still switch before fetching. */
    unsigned addr=a&0x1fff;
    if(!(addr&0x1000) || !cartridge.raw) return cartridge_bus;
    unsigned t=cartridge.type;
    if(t==CARTRIDGE_2K) return cartridge.raw[addr&2047];
    if(t==CARTRIDGE_4K) return cartridge.raw[addr&4095];
    if(t>=CARTRIDGE_F8 && t<=CARTRIDGE_F4) {
        unsigned first=t==CARTRIDGE_F8?0x1ff8:t==CARTRIDGE_F6?0x1ff6:0x1ff4;
        unsigned count=2u<<(t-CARTRIDGE_F8);
        if(addr-first<count) cartridge.F8.current_bank=addr-first;
        unsigned bank=cartridge.F8.current_bank;
        if(bank<count) return cartridge.raw[(bank<<12)|(addr&4095)];
        return cartridge_bus;
    }
    return access(addr,false,0);
}
void __not_in_flash_func(cartridge_write)(uint16_t a, uint8_t v) { (void)access(a,true,v); }
size_t cartridge_state_size(int t) {
    if(t==CARTRIDGE_CV) return sizeof(cartridge.CV);
    if(t==CARTRIDGE_E0) return sizeof(cartridge.E0);
    if(t==CARTRIDGE_E7) return sizeof(cartridge.E7);
    if(t==CARTRIDGE_FA) return sizeof(cartridge.FA);
    if(t==CARTRIDGE_2K || t==CARTRIDGE_4K) return 0;
    return bank_count(t) ? sizeof(cartridge.F8) : (size_t)-1;
}
bool cartridge_validate_state(const Cartridge *c) {
    if(c->type!=cartridge.type) return false;
    int n=bank_count(c->type);
    if(n) return c->F8.current_bank>=0 && c->F8.current_bank<n;
    if(c->type==CARTRIDGE_E0) return (unsigned)c->E0.current_lower_bank<8 &&
        (unsigned)c->E0.current_middle_bank<8 && (unsigned)c->E0.current_upper_bank<8;
    if(c->type==CARTRIDGE_E7) return (unsigned)c->E7.current_lower_bank<cartridge_size/2048 &&
        (unsigned)c->E7.current_upper_bank<4;
    return cartridge_state_size(c->type)!=(size_t)-1;
}
