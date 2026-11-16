/* An Atari 2600 (VCS): its 6507 (a 6502 with 13 address lines and no
   interrupt pins -- decimal mode included, and the unofficial instructions
   games use), its picture chip the TIA, its RIOT (128 bytes of RAM, the
   timer, the joystick and console switch ports), and the cartridge boards:
   2K, 4K, F8 / F6 / F4 (with or without the Superchip's RAM), FE, E0, 3F,
   E7 and FA.  NTSC timing: 228 colour clocks a line, three to a 6507 cycle.
   There is no sound (the sound registers are taken and forgotten).

   The TIA has no picture memory: the program writes its registers while
   the beam runs, so what a pixel shows depends on the colour clock each
   write lands on.  Here the TIA is caught up lazily: a write (or a read of
   a collision register) first draws the pixels from where the TIA was up
   to the clock of the write, with the registers as they were, then changes
   the register.  The colour clock of a write is three times the 6507 cycle
   the instruction does its write in (its last), plus the delay the TIA
   itself takes for that register (one clock for GRPx / ENAxx / REFPx /
   VBLANK, two for PFx / HMxx, six for HMOVE) -- Stella's numbers, the
   reference here.

   The movable objects (the two players, the two missiles, the ball) each
   have a position counter in the TIA that counts the visible colour clocks
   and starts drawing when it wraps; here each one is the pixel where its
   counter passes 0 ('base').  RESxx sets it from the clock of the write;
   HMOVE's extra counting pulses (one every 4 clocks, as many as HMxx asks,
   counted only while the line is still in its horizontal blanking) move it
   left one pixel each; the 8 clocks HMOVE adds to the blanking at the
   start of the line move everything right by 8 (so a plain HMOVE moves an
   object by HMxx).  An HMOVE late in a line moves objects during the next
   line's blanking without the 8-pixel bar -- the late-HMOVE tricks work
   from the same rule.  A run of pixels is drawn by marking each object's
   copies (NUSIZ) over the playfield's blocks in a line of bits, then one
   pass turns the bits into colours (by CTRLPF's priority) and collisions;
   the decoding rules for the line of a reset (the first copy only appears
   on the next line; a copy being drawn finishes) are kept.

   Not done: paddles (their inputs read as not charged), RSYNC, the
   missiles' and ball's 'starfield' widths during HMOVE, NUSIZ changes in
   the middle of a copy, PAL / SECAM (an NTSC console only), the DPC
   (Pitfall II) and ARM-based boards, Supercharger tapes. */
#include "a26.h"

#define FC 0x01
#define FZ 0x02
#define FI 0x04
#define FD 0x08
#define FB 0x10
#define FU 0x20
#define FV 0x40
#define FN 0x80

#define LINE_CYCLES 76u
#define LINE_CLOCKS 228u
#define HBLANK      68u

enum { IMP, IMM, ZP, ZPX, ZPY, ABS, ABX, ABY, IND, IZX, IZY, REL };
enum { P0, P1, M0, M1, BL };            /* the movable objects */

/* the NTSC palette (Stella's): entry n is COLUxx 2n */
const u8 a26_rgb[128][3] = {
    {0, 0, 0}, {74, 74, 74}, {111, 111, 111}, {142, 142, 142}, {170, 170, 170}, {192, 192, 192}, {214, 214, 214},
    {236, 236, 236}, {72, 72, 0}, {105, 105, 15}, {134, 134, 29}, {162, 162, 42}, {187, 187, 53}, {210, 210, 64},
    {232, 232, 74}, {252, 252, 84}, {124, 44, 0}, {144, 72, 17}, {162, 98, 33}, {180, 122, 48}, {195, 144, 61},
    {210, 164, 74}, {223, 183, 85}, {236, 200, 96}, {144, 28, 0}, {163, 57, 21}, {181, 83, 40}, {198, 108, 58},
    {213, 130, 74}, {227, 151, 89}, {240, 170, 103}, {252, 188, 116}, {148, 0, 0}, {167, 26, 26}, {184, 50, 50},
    {200, 72, 72}, {214, 92, 92}, {228, 111, 111}, {240, 128, 128}, {252, 144, 144}, {132, 0, 100}, {151, 25, 122},
    {168, 48, 143}, {184, 70, 162}, {198, 89, 179}, {212, 108, 195}, {224, 124, 210}, {236, 140, 224}, {80, 0, 132},
    {104, 25, 154}, {125, 48, 173}, {146, 70, 192}, {164, 89, 208}, {181, 108, 224}, {197, 124, 238},
    {212, 140, 252}, {20, 0, 144}, {51, 26, 163}, {78, 50, 181}, {104, 72, 198}, {127, 92, 213}, {149, 111, 227},
    {169, 128, 240}, {188, 144, 252}, {0, 0, 148}, {24, 26, 167}, {45, 50, 184}, {66, 72, 200}, {84, 92, 214},
    {101, 111, 228}, {117, 128, 240}, {132, 144, 252}, {0, 28, 136}, {24, 59, 157}, {45, 87, 176}, {66, 114, 194},
    {84, 138, 210}, {101, 160, 225}, {117, 181, 239}, {132, 200, 252}, {0, 48, 100}, {24, 80, 128},
    {45, 109, 152}, {66, 136, 176}, {84, 160, 197}, {101, 183, 217}, {117, 204, 235}, {132, 224, 252}, {0, 64, 48},
    {24, 98, 78}, {45, 129, 105}, {66, 158, 130}, {84, 184, 153}, {101, 209, 174}, {117, 231, 194},
    {132, 252, 212}, {0, 68, 0}, {26, 102, 26}, {50, 132, 50}, {72, 160, 72}, {92, 186, 92}, {111, 210, 111},
    {128, 232, 128}, {144, 252, 144}, {20, 60, 0}, {53, 95, 24}, {82, 126, 45}, {110, 156, 66}, {135, 183, 84},
    {158, 208, 101}, {180, 231, 117}, {200, 252, 132}, {48, 56, 0}, {80, 89, 22}, {109, 118, 43}, {136, 146, 62},
    {160, 171, 79}, {183, 194, 95}, {204, 216, 110}, {224, 236, 124}, {72, 44, 0}, {105, 77, 20}, {134, 106, 38},
    {162, 134, 56}, {187, 159, 71}, {210, 182, 86}, {232, 204, 99}, {252, 224, 112},
};

static const u8 cycles_of[256] = {
    7, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 4, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 4, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    6, 6, 2, 8, 3, 3, 5, 5, 3, 2, 2, 2, 3, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    6, 6, 2, 8, 3, 3, 5, 5, 4, 2, 2, 2, 5, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4, 2, 6, 2, 6, 4, 4, 4, 4, 2, 5, 2, 5, 5, 5, 5, 5,
    2, 6, 2, 6, 3, 3, 3, 3, 2, 2, 2, 2, 4, 4, 4, 4, 2, 5, 2, 5, 4, 4, 4, 4, 2, 4, 2, 4, 4, 4, 4, 4,
    2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
    2, 6, 2, 8, 3, 3, 5, 5, 2, 2, 2, 2, 4, 4, 6, 6, 2, 5, 2, 8, 4, 4, 6, 6, 2, 4, 2, 7, 4, 4, 7, 7,
};

static u8 mode_of[256];

static struct {
    u8 a, x, y, s, p;
    u16 pc;
    u8 jam, wsync;
    u32 cyc;                            /* cycles since the frame's base (a multiple of 76: lines start there) */
    u32 total;                          /* cycles since the reset */
} cpu;

static u32 now;                         /* the cycle (counted like cpu.cyc) at whose end this bus access happens */
static u8 bus;                          /* the last operand byte: what a TIA read's undriven bits show */

#ifdef A26_CPU_TEST
u8 a26_test_memory[65536];
#endif

/* ---- the cartridge ---- */
enum { C2K, C4K, CF8, CF6, CF4, CFE, CE0, C3F, CE7, CFA };

static struct {
    const u8 *rom;
    u32 size;
    u8 type, superchip, banks;
    u16 slow_below, slow_from;          /* (address & 0xFFF) under / from which a read is more than ROM */
    u16 hot_first, hot_last;            /* the bank switching addresses (F8 F6 F4 FA) */
    u8 bank[3];                         /* E0: the three switchable 1K slots; E7: [0] the 2K bank, [1] the RAM bank */
    const u8 *slot[4];                  /* the four 1K windows of $1000-$1FFF */
} cart;

static u8 cart_ram[2048], small_rom[2048];
static char info[96];

static void slots_4k(u32 bank)
{
    for (int i = 0; i < 4; i++)
        cart.slot[i] = cart.rom + bank * 4096u + (u32)i * 1024u;
}

static void e0_slots(void)
{
    for (int i = 0; i < 3; i++)
        cart.slot[i] = cart.rom + cart.bank[i] * 1024u;
    cart.slot[3] = cart.rom + 7 * 1024u;
}

static void two_k_slots(u32 bank)               /* 3F and E7: $1000-$17FF switched, $1800-$1FFF the last bank */
{
    u32 last = cart.banks - 1u;
    cart.slot[0] = cart.rom + bank * 2048u;
    cart.slot[1] = cart.slot[0] + 1024;
    cart.slot[2] = cart.rom + last * 2048u;
    cart.slot[3] = cart.slot[2] + 1024;
}

static void e7_switch(u32 o)
{
    static const u8 six[8] = {0, 1, 0, 1, 2, 3, 4, 5};
    if (o >= 0xFE8) {
        cart.bank[1] = o & 3;
        return;
    }
    if (cart.banks == 4) {
        if (o < 0xFE4)
            return;
        cart.bank[0] = o & 3;
    } else {
        cart.bank[0] = cart.banks == 6 ? six[o & 7] : o & 7;
    }
    if (cart.bank[0] != cart.banks - 1)
        two_k_slots(cart.bank[0]);
}

/* an access to a hotspot or to the cartridge's RAM ports (o: the address's low 12 bits) */
static u8 cart_read(u32 o)
{
    switch (cart.type) {
    case CF8: case CF6: case CF4: case CFA:
        if (o < cart.slow_below)                        /* RAM: the read port is the upper half */
            return cart_ram[o & (cart.slow_below / 2 - 1)];
        if (o >= cart.hot_first && o <= cart.hot_last)
            slots_4k(o - cart.hot_first);
        break;
    case CE0:
        if (o >= 0xFE0 && o < 0xFF8) {
            cart.bank[(o - 0xFE0) >> 3] = o & 7;
            e0_slots();
        }
        break;
    case CE7:
        if (o < 0x800 && cart.bank[0] == cart.banks - 1)
            return cart_ram[o & 0x3FF];
        if (o >= 0x800 && o < 0xA00)
            return cart_ram[0x400 + cart.bank[1] * 256u + (o & 0xFF)];
        if (o >= 0xFE0 && o < 0xFEC)
            e7_switch(o);
        break;
    }
    return cart.slot[o >> 10][o & 0x3FF];
}

static void cart_write(u32 o, u8 v)
{
    switch (cart.type) {
    case CF8: case CF6: case CF4: case CFA:
        if (o < cart.slow_below / 2)                    /* RAM: the write port is the lower half */
            cart_ram[o] = v;
        else if (o >= cart.hot_first && o <= cart.hot_last)
            slots_4k(o - cart.hot_first);
        break;
    case CE0:
        if (o >= 0xFE0 && o < 0xFF8) {
            cart.bank[(o - 0xFE0) >> 3] = o & 7;
            e0_slots();
        }
        break;
    case CE7:
        if (o < 0x400 && cart.bank[0] == cart.banks - 1)
            cart_ram[o] = v;
        else if (o >= 0x800 && o < 0x900)
            cart_ram[0x400 + cart.bank[1] * 256u + (o & 0xFF)] = v;
        else if (o >= 0xFE0 && o < 0xFEC)
            e7_switch(o);
        break;
    }
}

static void cart_start(void)
{
    for (unsigned i = 0; i < sizeof(cart_ram); i++)
        cart_ram[i] = 0;
    cart.bank[0] = cart.bank[1] = cart.bank[2] = 0;
    switch (cart.type) {
    case C2K:
        cart.slot[0] = cart.slot[2] = cart.rom;
        cart.slot[1] = cart.slot[3] = cart.rom + 1024;
        break;
    case CE0:
        cart.bank[0] = 4;
        cart.bank[1] = 5;
        cart.bank[2] = 6;
        e0_slots();
        break;
    case C3F: case CE7:
        two_k_slots(0);
        break;
    default:
        slots_4k(0);
        break;
    }
}

/* ---- the RIOT: RAM, timer, ports ---- */
static struct {
    u8 ram[128];
    u8 timer, flags, shift, wrapped;    /* flags: bit 7 the timer passed 0 */
    u32 sub, last;                      /* cycles into the current divide-by-N step; the cycle of the last update */
    u8 ddra, ddrb, outa, outb;
    u8 joy, switches;                   /* the joystick's lines (active low) and the console's switches */
} riot;

/* the timer, brought up to the cycle of this access: it counts down once every 1, 8, 64 or 1024 cycles until
   it passes 0, then once every cycle */
static void riot_update(void)
{
    u32 cycles = now - riot.last, sub = riot.sub, div = 1u << riot.shift;
    if (!cycles)
        return;
    riot.last = now;
    riot.wrapped = 0;
    riot.sub = (cycles + sub) & (div - 1);
    if (!(riot.flags & 0x80)) {
        u32 ticks = (cycles + sub) >> riot.shift;
        if (ticks > riot.timer) {
            cycles -= ((riot.timer + 1u) << riot.shift) - sub;
            riot.wrapped = cycles == 0;
            riot.timer = 0xFF;
            riot.flags |= 0x80;
        } else {
            riot.timer = (u8)(riot.timer - ticks);
            cycles = 0;
        }
    }
    if (riot.flags & 0x80) {
        riot.timer = (u8)(riot.timer - cycles);
        riot.wrapped = riot.timer == 0xFF;
    }
}

static u8 riot_read(u32 a)
{
    switch (a & 7) {
    case 0:
        return (u8)((riot.outa | ~riot.ddra) & (riot.joy << 4 | 0x0F));
    case 1:
        return riot.ddra;
    case 2:
        return (u8)((riot.outb | ~riot.ddrb) & (riot.switches | riot.ddrb));
    case 3:
        return riot.ddrb;
    case 4: case 6:
        riot_update();
        if (!riot.wrapped)
            riot.flags &= 0x7F;
        return riot.timer;
    default: {
        riot_update();
        u8 v = riot.flags;
        riot.flags &= 0xBF;
        return v;
    }
    }
}

static void riot_write(u32 a, u8 v)
{
    static const u8 shifts[4] = {0, 3, 6, 10};
    if (a & 4) {
        if (a & 0x10) {                         /* TIM1T, TIM8T, TIM64T, T1024T */
            riot_update();
            riot.shift = shifts[a & 3];
            riot.timer = v;
            riot.sub = (1u << riot.shift) - 1;
            if (!riot.wrapped)
                riot.flags &= 0x7F;
        }
        return;
    }
    switch (a & 3) {
    case 0: riot.outa = v; break;
    case 1: riot.ddra = v; break;
    case 2: riot.outb = v; break;
    default: riot.ddrb = v; break;
    }
}

/* ---- the TIA ---- */
static struct {
    u32 done;                           /* colour clocks drawn, counted like 3 x cpu.cyc */
    u32 h;                              /* where in its line 'done' is: 0..227, the picture from 68 */
    u32 aline;                          /* lines since the reset */
    u32 fline;                          /* lines since the frame began (the line its vertical sync ended on: 0) */
    u32 last_lines;                     /* the lines of the frame before */
    u8 extended;                        /* HMOVE made this line's blanking 8 clocks longer */
    u8 vsync, vsync_lines, vblank, frame_done;
    u8 *picture, *row;                  /* the frame's picture, this line's row of it (0: outside) */
    u32 rows_drawn;
    u8 pf_bit;                          /* the playfield's pixel, latched at the start of each 4-clock block */
    u8 fire, latch_mode, latched;       /* the joystick button, INPT4's latch (VBLANK bit 6) */
} tia;

static u8 base[5];                      /* where each object's position counter passes 0 (a pixel, 0..159) */
static u8 hm[5];                        /* the HMOVE pulses each gets: (HMxx >> 4) ^ 8 */
static u32 res_line[5];                 /* the line of the object's last reset */
static struct {
    s32 x;                              /* a copy being drawn when its object was reset: it finishes */
    u32 len;
    u8 shift, on;
} tail[4];
static struct {
    u8 active, k, moving;               /* HMOVE's pulse counter, the objects still taking pulses */
} mv;
static u8 grp_new[2], grp_old[2], refp[2], vdelp[2], nusiz[2];
static u8 enam[2], resmp[2], enabl_new, enabl_old, vdelbl;
static u8 pf0, pf1, pf2, ctrlpf;
static u32 pf_left, pf_right;           /* each half's 20 blocks, bit n = block n from the left */
static u8 colup0, colup1, colupf, colubk;       /* palette entries */
static u16 cx;                          /* the collision latches: register r's D7 / D6 at bits 2r + 1 / 2r */
static u8 scratch[A26_W];               /* a line outside the picture (its collisions still count) */

/* tables built at load, and the line of object bits a run of pixels is drawn through */
static u8 rev[256];                     /* a byte's bits in the other order */
static u8 prio[3][64];                  /* [mode][objects] -> 0 P0's colour, 1 P1's, 2 the playfield's, 3 the
                                           background, 4 the ball's */
static u16 coll[64];                    /* [objects] -> the collision latches they set */

/* the objects' bits in prio's and coll's index */
#define OP0 0x01
#define OM0 0x02
#define OP1 0x04
#define OM1 0x08
#define OPF 0x10
#define OBL 0x20

static const u8 copy_at[8][3] = {       /* NUSIZ: the copies after the first, pixels from it (0: none) */
    {0, 0, 0}, {16, 0, 0}, {32, 0, 0}, {16, 32, 0}, {64, 0, 0}, {0, 0, 0}, {32, 64, 0}, {0, 0, 0},
};

static void build_tables(void)
{
    for (int i = 0; i < 256; i++) {
        u8 r = 0;
        for (int b = 0; b < 8; b++)
            if (i & (1 << b))
                r |= (u8)(0x80 >> b);
        rev[i] = r;
    }
    for (int i = 0; i < 64; i++) {
        int p0 = i & OP0, m0 = i & OM0, p1 = i & OP1, m1 = i & OM1, pf = i & OPF, bl = i & OBL;
        u8 normal = (p0 || m0) ? 0 : (p1 || m1) ? 1 : pf ? 2 : bl ? 4 : 3;
        u8 score = (p0 || m0) ? 0 : pf ? 2 : (p1 || m1) ? 1 : bl ? 4 : 3;
        u8 pfp = pf ? 2 : bl ? 4 : (p0 || m0) ? 0 : (p1 || m1) ? 1 : 3;
        prio[0][i] = normal;
        prio[1][i] = score;
        prio[2][i] = pfp;
        u16 c = 0;
        if (m0 && p1) c |= 1 << 1;      /* CXM0P */
        if (m0 && p0) c |= 1 << 0;
        if (m1 && p0) c |= 1 << 3;      /* CXM1P */
        if (m1 && p1) c |= 1 << 2;
        if (p0 && pf) c |= 1 << 5;      /* CXP0FB */
        if (p0 && bl) c |= 1 << 4;
        if (p1 && pf) c |= 1 << 7;      /* CXP1FB */
        if (p1 && bl) c |= 1 << 6;
        if (m0 && pf) c |= 1 << 9;      /* CXM0FB */
        if (m0 && bl) c |= 1 << 8;
        if (m1 && pf) c |= 1 << 11;     /* CXM1FB */
        if (m1 && bl) c |= 1 << 10;
        if (bl && pf) c |= 1 << 13;     /* CXBLPF */
        if (p0 && p1) c |= 1 << 15;     /* CXPPMM */
        if (m0 && m1) c |= 1 << 14;
        coll[i] = c;
    }
}

static void pf_update(void)
{
    u32 l = (u32)(pf0 >> 4) | (u32)rev[pf1] << 4 | (u32)pf2 << 12;
    pf_left = l;
    pf_right = (ctrlpf & 1) ? ((u32)rev[l & 0xFF] << 12 | (u32)rev[(l >> 8) & 0xFF] << 4 | (u32)rev[l >> 16] >> 4)
                            : l;
}

static u32 pf_at(u32 x)
{
    u32 b = x >> 2;
    return b < 20 ? (pf_left >> b) & 1 : (pf_right >> (b - 20)) & 1;
}

static u32 rule_of(int o)
{
    return tia.aline == res_line[o] ? 1 : tia.aline == res_line[o] + 1 ? 2 : 0;
}

static u32 first_pixel(int o)           /* players start 2 pixels after their counter's 0, missiles and the ball 1 */
{
    u32 p = base[o] + (o < 2 ? 2u : 1u);
    return p >= 160 ? p - 160 : p;
}

/* sets 'bit' in mask[x0..x1) where a copy shows: it starts u0 pixels into the line (u0 >= 160: the part carried
   over from the line before shows at u0 - 160), len pixels, each bit of g (lowest first) 1 << shift pixels;
   parts: 1 this line's start, 2 the carried-over part */
static u8 mask[A26_W];

static void paint(u32 u0, u32 len, u32 shift, u32 g, u8 bit, u32 parts, u32 x0, u32 x1)
{
    u32 u1 = u0 + len, lo, hi, u;
    if (parts & 1) {
        lo = u0 > x0 ? u0 : x0;
        hi = u1 < x1 ? u1 : x1;
        for (u = lo; u < hi; u++)
            if ((g >> ((u - u0) >> shift)) & 1)
                mask[u] |= bit;
    }
    if (parts & 2) {
        lo = u0 > x0 + 160 ? u0 : x0 + 160;
        hi = u1 < x1 + 160 ? u1 : x1 + 160;
        for (u = lo; u < hi; u++)
            if ((g >> ((u - u0) >> shift)) & 1)
                mask[u - 160] |= bit;
    }
}

/* a player's (g: its graphics, the leftmost pixel in bit 0) or a missile's (w: its width) copies.  On the line of
   its reset an object's first copy is not drawn (its counter only gets there on the next line), nor on the next
   line the part of it carried over from that line; a copy that was being drawn when it was reset finishes */
static void paint_object(int o, u32 g, u32 w, u32 x0, u32 x1)
{
    static const u8 bits[4] = {OP0, OP1, OM0, OM1};
    u32 n = nusiz[o & 1] & 7, rule = rule_of(o), p = first_pixel(o), len, shift = 0;
    if (o < M0) {
        shift = n == 5 ? 1 : n == 7 ? 2 : 0;
        len = 8u << shift;
        p += shift ? 1 : 0;                     /* (a wide player starts a pixel later) */
    } else {
        len = w;
    }
    for (int c = 0; c < 4; c++) {
        u32 at = c ? copy_at[n][c - 1] : 0;
        if (c && !at)
            break;
        if (!c && rule == 1)
            continue;
        paint(p + at, len, shift, g, bits[o], (!c && rule == 2) ? 1 : 3, x0, x1);
    }
    if (tail[o].on && (o < M0 || w))
        paint((u32)(tail[o].x + 160), tail[o].len, tail[o].shift, g, bits[o], 2, x0, x1);
}

/* pixels x0..x1-1 of this line (all on one side of the middle), from the registers as they are now */
static void draw(u32 x0, u32 x1)
{
    u8 *out = tia.row ? tia.row : scratch;
    u32 x, pf = tia.pf_bit;
    if (tia.extended && x0 < 8) {                       /* HMOVE's black bar */
        u32 e = x1 < 8 ? x1 : 8;
        for (x = x0; x < e; x++)
            out[x] = 0;
        x0 = e;
    }
    if (x0 >= x1)
        return;
    if (tia.vblank) {
        if (tia.row)
            for (x = x0; x < x1; x++)
                out[x] = 0;
        tia.pf_bit = (u8)pf_at((x1 - 1) & ~3u);
        return;
    }
    u32 mode = (ctrlpf & 4) ? 2 : (ctrlpf & 2) ? 1 : 0;
    const u8 *pr = prio[mode];
    u8 col[5] = {colup0, colup1, mode == 1 ? (x0 < 80 ? colup0 : colup1) : colupf, colubk, colupf};
    u32 g0 = vdelp[0] ? grp_old[0] : grp_new[0], g1 = vdelp[1] ? grp_old[1] : grp_new[1];
    if (!refp[0])
        g0 = rev[g0];
    if (!refp[1])
        g1 = rev[g1];
    u32 w2 = (enam[0] && !resmp[0]) ? 1u << ((nusiz[0] >> 4) & 3) : 0;
    u32 w3 = (enam[1] && !resmp[1]) ? 1u << ((nusiz[1] >> 4) & 3) : 0;
    u32 w4 = (vdelbl ? enabl_old : enabl_new) ? 1u << ((ctrlpf >> 4) & 3) : 0;
    int tails = tail[0].on | tail[1].on | tail[2].on | tail[3].on;    /* (only on the line of a reset) */
    if (!(g0 | g1 | w2 | w3 | w4) && !tails) {          /* only the playfield */
        if (tia.row) {
            u8 fg = col[pr[OPF]], bg = col[3];
            for (x = x0; x < x1; x++) {
                if (!(x & 3))
                    pf = pf_at(x);
                out[x] = pf ? fg : bg;
            }
            tia.pf_bit = (u8)pf;
        } else {
            tia.pf_bit = (u8)pf_at((x1 - 1) & ~3u);
        }
        return;
    }
    /* the objects' bits over the playfield's, then the colours and the collisions */
    for (x = x0; x < x1 && (x & 3); x++)
        mask[x] = (u8)(pf << 4);
    for (; x < x1; x += 4) {                            /* a block at a time */
        pf = pf_at(x);
        u32 e = x + 4 < x1 ? x + 4 : x1;
        for (u32 y = x; y < e; y++)
            mask[y] = (u8)(pf << 4);
    }
    if (g0 | tail[0].on)
        paint_object(P0, g0, 0, x0, x1);
    if (g1 | tail[1].on)
        paint_object(P1, g1, 0, x0, x1);
    if (w2)
        paint_object(M0, 0xFF, w2, x0, x1);
    if (w3)
        paint_object(M1, 0xFF, w3, x0, x1);
    if (w4)
        paint(first_pixel(BL), w4, 0, 0xFF, OBL, 3, x0, x1);
    u32 c = cx;
    for (x = x0; x < x1; x++) {
        u32 i = mask[x];
        out[x] = col[pr[i]];
        c |= coll[i];
    }
    tia.pf_bit = (u8)pf;
    cx = (u16)c;
}

static void draw_range(u32 x0, u32 x1)
{
    if ((ctrlpf & 6) == 2 && x0 < 80 && x1 > 80) {     /* the score mode colours the halves differently */
        draw(x0, 80);
        draw(80, x1);
    } else {
        draw(x0, x1);
    }
}

static void set_row(void)
{
    u32 r = tia.fline - A26_TOP;
    tia.row = (tia.picture && r < A26_H) ? tia.picture + r * A26_W : 0;
}

/* the line VSYNC ended on (or, for a game that never syncs, the 524th) begins a frame */
static void frame_begins(void)
{
    if (tia.row) {                                      /* a frame ending inside the picture: the rest black */
        u32 x = tia.h > HBLANK ? tia.h - HBLANK : 0;
        while (x < A26_W)
            tia.row[x++] = 0;
        tia.rows_drawn = (u32)(tia.row - tia.picture) / A26_W + 1;
    }
    tia.last_lines = tia.fline;
    tia.fline = 0;
    tia.frame_done = 1;
    set_row();
}

static void new_line(void)
{
    if (tia.row)
        tia.rows_drawn = (u32)(tia.row - tia.picture) / A26_W + 1;
    tia.h = 0;
    tia.aline++;
    tia.fline++;
    tia.extended = 0;
    tail[0].on = tail[1].on = tail[2].on = tail[3].on = 0;
    if (tia.vsync && tia.vsync_lines < 255)
        tia.vsync_lines++;
    if (tia.fline >= 2 * 262)
        frame_begins();
    else
        set_row();
}

/* one of HMOVE's pulses (every 4 clocks): an object still taking them counts one more, if the line is still
   blanked (in the picture the pulse falls on a clock the counter takes anyway) */
static void move_tick(void)
{
    u32 k = mv.k > 15 ? 0 : mv.k, blank = tia.h < (tia.extended ? HBLANK + 8 : HBLANK);
    for (int o = 0; o < 5; o++) {
        if (!(mv.moving & (1 << o)))
            continue;
        if (k == hm[o])
            mv.moving &= (u8)~(1 << o);
        else if (blank)
            base[o] = base[o] ? (u8)(base[o] - 1) : 159;
    }
    mv.k++;
    if (!mv.moving)
        mv.active = 0;
}

/* draw (and move) up to colour clock 'to' */
static void catch_up(u32 to)
{
    while (tia.done < to) {
        u32 h = tia.h, end = h + (to - tia.done);
        if (end > LINE_CLOCKS)
            end = LINE_CLOCKS;
        if (mv.active) {
            if (!(h & 3))
                move_tick();
            if (end > (h | 3) + 1)
                end = (h | 3) + 1;
        }
        if (end > HBLANK)
            draw_range(h > HBLANK ? h - HBLANK : 0, end - HBLANK);
        tia.done += end - h;
        tia.h = end;
        if (end == LINE_CLOCKS)
            new_line();
    }
}

/* RESP0 .. RESBL: the object's counter restarts now */
static void reset_object(int o)
{
    u32 blank_end = tia.extended ? HBLANK + 8 : HBLANK, blank = tia.h < blank_end;
    u32 x = blank ? blank_end - HBLANK : tia.h - HBLANK;       /* the first clock the counter will take */
    u32 nb = blank ? x + (tia.h >= 73 ? 2 : 1) : x + 3;
    if (o < BL) {
        /* a copy whose drawing has begun goes on (one that has only just been decoded starts again) */
        int player = o < 2, i = player ? o : o - 2;
        u32 n = nusiz[i] & 7, w = player ? (n == 5 ? 2 : n == 7 ? 4 : 1) : 1u << ((nusiz[i] >> 4) & 3);
        u32 shift = player ? (w == 4 ? 2 : w == 2 ? 1 : 0) : 0, lead = player ? 6 + (w > 1) : 5;
        u32 len = player ? 8 * w : w, restart = player ? (blank ? 3 + (tia.h >= 73) : 5) + (w > 1)
                                                       : (blank ? 2 + (tia.h >= 73) : 4);
        u32 d = (x + 160 - first_pixel(o)) % 160;
        tail[o].on = 0;
        for (int c = 0; c < 4 && !(player ? 0 : resmp[i]); c++) {
            u32 at = c ? copy_at[n][c - 1] : 0;
            if (c && (!at || (!player && (n == 5 || n == 7))))
                break;
            if (!c && res_line[o] == tia.aline)
                continue;
            u32 since = (d + 160 + lead - (player ? (w > 1) : 0) - at) % 160;
            if (since >= 1 && since < lead + len) {
                tail[o].x = since < 5 ? (s32)(x + restart) : (s32)x - (s32)since + (s32)lead;
                tail[o].len = len;
                tail[o].shift = (u8)shift;
                tail[o].on = 1;
                break;
            }
        }
        res_line[o] = tia.aline;
    }
    base[o] = (u8)(nb % 160);
}

static void hmove(void)
{
    mv.active = 1;
    mv.k = 0;
    mv.moving = 0x1F;
    if (tia.h >= 1 && tia.h < HBLANK && !tia.extended) {
        tia.extended = 1;                       /* the counters miss this line's first 8 clocks */
        for (int o = 0; o < 5; o++)
            base[o] = (u8)((base[o] + 8) % 160);
    }
}

static void tia_write(u32 r, u8 v)
{
    u32 t = now * 3;
    switch (r) {
    case 0x00:                                  /* VSYNC */
        catch_up(t);
        if (v & 2) {
            if (!tia.vsync) {
                tia.vsync = 1;
                tia.vsync_lines = 0;
            }
        } else if (tia.vsync) {
            tia.vsync = 0;
            if (tia.vsync_lines)
                frame_begins();
        }
        break;
    case 0x01:                                  /* VBLANK */
        catch_up(t + 1);
        tia.vblank = v & 2;
        if (v & 0x40) {
            if (!tia.latch_mode) {
                tia.latch_mode = 1;
                tia.latched = 0x80;
            }
        } else {
            tia.latch_mode = 0;
        }
        break;
    case 0x02:                                  /* WSYNC: the 6507 waits for the line's end */
        cpu.wsync = 1;
        break;
    case 0x04: case 0x05:                       /* NUSIZ0/1 */
        catch_up(t);
        nusiz[r - 4] = v;
        break;
    case 0x06: catch_up(t); colup0 = v >> 1; break;
    case 0x07: catch_up(t); colup1 = v >> 1; break;
    case 0x08: catch_up(t); colupf = v >> 1; break;
    case 0x09: catch_up(t); colubk = v >> 1; break;
    case 0x0A:                                  /* CTRLPF */
        catch_up(t);
        ctrlpf = v;
        pf_update();
        break;
    case 0x0B: case 0x0C:                       /* REFP0/1 */
        catch_up(t + 1);
        refp[r - 0x0B] = v & 8;
        break;
    case 0x0D: case 0x0E: case 0x0F:            /* PF0..PF2 */
        catch_up(t + 2);
        if (r == 0x0D)
            pf0 = v;
        else if (r == 0x0E)
            pf1 = v;
        else
            pf2 = v;
        pf_update();
        break;
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14:      /* RESP0 RESP1 RESM0 RESM1 RESBL */
        catch_up(t);
        reset_object((int)(r - 0x10));
        break;
    case 0x1B:                                  /* GRP0 (and VDELP1's copy of GRP1) */
        catch_up(t + 1);
        grp_new[0] = v;
        grp_old[1] = grp_new[1];
        break;
    case 0x1C:                                  /* GRP1 (and VDELP0's and VDELBL's copies) */
        catch_up(t + 1);
        grp_new[1] = v;
        grp_old[0] = grp_new[0];
        enabl_old = enabl_new;
        break;
    case 0x1D: case 0x1E:                       /* ENAM0/1 */
        catch_up(t + 1);
        enam[r - 0x1D] = v & 2;
        break;
    case 0x1F:                                  /* ENABL */
        catch_up(t + 1);
        enabl_new = v & 2;
        break;
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24:      /* HMP0 HMP1 HMM0 HMM1 HMBL */
        catch_up(t + 2);
        hm[r - 0x20] = (u8)((v >> 4) ^ 8);
        break;
    case 0x25: case 0x26:                       /* VDELP0/1 */
        catch_up(t);
        vdelp[r - 0x25] = v & 1;
        break;
    case 0x27:
        catch_up(t);
        vdelbl = v & 1;
        break;
    case 0x28: case 0x29: {                     /* RESMP0/1: the missile kept at its player's middle */
        u32 i = r - 0x28, n = nusiz[i] & 7;
        catch_up(t);
        if (resmp[i] && !(v & 2))
            base[M0 + i] = (u8)((base[i] + (n == 5 ? 8 : n == 7 ? 12 : 5)) % 160);
        resmp[i] = v & 2;
        break;
    }
    case 0x2A:                                  /* HMOVE */
        catch_up(t + 6);
        hmove();
        break;
    case 0x2B:                                  /* HMCLR */
        catch_up(t + 2);
        for (int o = 0; o < 5; o++)
            hm[o] = 8;
        break;
    case 0x2C:                                  /* CXCLR */
        catch_up(t);
        cx = 0;
        break;
    }
}

static u8 tia_read(u32 r)
{
    u8 v = 0;
    if (r < 8) {                                /* the collision latches */
        catch_up(now * 3);
        v = (u8)(((cx >> (r * 2)) & 3) << 6);
    } else if (r == 12) {                       /* INPT4: the joystick's button, low while pressed */
        if (tia.latch_mode) {
            if (tia.fire)
                tia.latched = 0;
            v = tia.latched;
        } else {
            v = tia.fire ? 0 : 0x80;
        }
    } else if (r == 13) {                       /* INPT5: the second joystick, not pressed */
        v = 0x80;
    }                                           /* (INPT0..3, the paddles: never charged) */
    return (u8)(v | (bus & 0x3F));
}

/* ---- the 6507's memory: 13 address lines ---- */
static u8 rd(u16 a)
{
#ifdef A26_CPU_TEST
    return a26_test_memory[a];
#else
    a &= 0x1FFF;
    if (a & 0x1000) {
        u32 o = a & 0xFFF;
        if (o < cart.slow_below || o >= cart.slow_from)
            return cart_read(o);
        return cart.slot[o >> 10][o & 0x3FF];
    }
    if (a & 0x80)
        return (a & 0x200) ? riot_read(a) : riot.ram[a & 0x7F];
    return tia_read(a & 0x0F);
#endif
}

static void wr(u16 a, u8 v)
{
#ifdef A26_CPU_TEST
    a26_test_memory[a] = v;
#else
    a &= 0x1FFF;
    if (a & 0x1000) {
        cart_write(a & 0xFFF, v);
    } else if (a & 0x80) {
        if (a & 0x200)
            riot_write(a, v);
        else
            riot.ram[a & 0x7F] = v;
    } else if (cart.type == C3F && a < 0x40) {  /* Tigervision: a write to $00-$3F picks the bank (the games use
                                                   the TIA at $40-$7F) */
        two_k_slots(v % cart.banks);
    } else {
        tia_write(a & 0x3F, v);
    }
#endif
}

static u8 fetch(void)
{
#ifndef A26_CPU_TEST
    u32 o = cpu.pc & 0xFFF;
    if ((cpu.pc & 0x1000) && o >= cart.slow_below && o < cart.slow_from) {
        cpu.pc++;
        return cart.slot[o >> 10][o & 0x3FF];
    }
#endif
    return rd(cpu.pc++);
}

/* ---- the 6507 ---- */
static void push(u8 v)
{
    wr((u16)(0x100 | cpu.s), v);
    cpu.s--;
}

static u8 pull(void)
{
    cpu.s++;
    return rd((u16)(0x100 | cpu.s));
}

static void nz(u8 v)
{
    cpu.p = (u8)((cpu.p & ~(FN | FZ)) | (v & 0x80) | (v ? 0 : FZ));
}

static void adc(u8 v)
{
    u32 a = cpu.a, c = cpu.p & FC, sum = a + v + c;
    if (cpu.p & FD) {                           /* decimal, as the NMOS 6502 does it (N and V from the
                                                   halfway result, Z from the binary sum) */
        s32 lo = (s32)(a & 15) + (s32)(v & 15) + (s32)c;
        if (lo >= 10)
            lo = ((lo + 6) & 15) + 16;
        s32 r = (s32)(a & 0xF0) + (s32)(v & 0xF0) + lo;
        s32 sr = (s32)(signed char)(a & 0xF0) + (s32)(signed char)(v & 0xF0) + lo;
        u8 p = (u8)(cpu.p & ~(FC | FV | FN | FZ));
        if (!(sum & 0xFF))
            p |= FZ;
        if (r & 0x80)
            p |= FN;
        if (sr < -128 || sr > 127)
            p |= FV;
        if (r >= 0xA0)
            r += 0x60;
        if (r >= 0x100)
            p |= FC;
        cpu.a = (u8)r;
        cpu.p = p;
        return;
    }
    cpu.p = (u8)((cpu.p & ~(FC | FV)) | (sum > 0xFF ? FC : 0) | ((~(a ^ v) & (a ^ sum) & 0x80) ? FV : 0));
    cpu.a = (u8)sum;
    nz(cpu.a);
}

static void sbc(u8 v)
{
    u32 a = cpu.a, c = cpu.p & FC, dif = a - v - (1 - c);
    cpu.p = (u8)((cpu.p & ~(FC | FV)) | (dif < 0x100 ? FC : 0) | (((a ^ v) & (a ^ dif) & 0x80) ? FV : 0));
    nz((u8)dif);                                /* (decimal or not, the flags are the binary ones) */
    if (cpu.p & FD) {
        s32 lo = (s32)(a & 15) - (s32)(v & 15) + (s32)c - 1;
        if (lo < 0)
            lo = ((lo - 6) & 15) - 16;
        s32 r = (s32)(a & 0xF0) - (s32)(v & 0xF0) + lo;
        if (r < 0)
            r -= 0x60;
        cpu.a = (u8)r;
        return;
    }
    cpu.a = (u8)dif;
}

static void arr(u8 v)                           /* AND, then ROR -- with its own decimal-mode quirks */
{
    u32 t = cpu.a & v, c = cpu.p & FC, a = (t >> 1) | (c << 7);
    if (!(cpu.p & FD)) {
        nz((u8)a);
        cpu.p = (u8)((cpu.p & ~(FC | FV)) | ((a >> 6) & 1) | ((((a >> 6) ^ (a >> 5)) & 1) ? FV : 0));
        cpu.a = (u8)a;
        return;
    }
    u32 hi = t >> 4, lo = t & 15;
    cpu.p = (u8)((cpu.p & ~(FN | FZ | FV | FC)) | (c ? FN : 0) | (a ? 0 : FZ) | (((t ^ a) & 0x40) ? FV : 0));
    if (lo + (lo & 1) > 5)
        a = (a & 0xF0) | ((a + 6) & 0x0F);
    if (hi + (hi & 1) > 5) {
        cpu.p |= FC;
        a = (a + 0x60) & 0xFF;
    }
    cpu.a = (u8)a;
}

static void compare(u8 r, u8 v)
{
    cpu.p = (u8)((cpu.p & ~FC) | (r >= v ? FC : 0));
    nz((u8)(r - v));
}

static u8 asl(u8 v)
{
    cpu.p = (u8)((cpu.p & ~FC) | (v >> 7));
    v = (u8)(v << 1);
    nz(v);
    return v;
}

static u8 lsr(u8 v)
{
    cpu.p = (u8)((cpu.p & ~FC) | (v & 1));
    v >>= 1;
    nz(v);
    return v;
}

static u8 rol(u8 v)
{
    u8 c = cpu.p & FC;
    cpu.p = (u8)((cpu.p & ~FC) | (v >> 7));
    v = (u8)((v << 1) | c);
    nz(v);
    return v;
}

static u8 ror(u8 v)
{
    u8 c = cpu.p & FC;
    cpu.p = (u8)((cpu.p & ~FC) | (v & 1));
    v = (u8)((v >> 1) | (c << 7));
    nz(v);
    return v;
}

/* one instruction; its bus accesses happen at the cycles the 6507 does them (the ones that can reach the TIA
   or the RIOT, that is: a load's or a store's last cycle, a read-modify-write's last three) */
static void step(void)
{
    u32 start = cpu.cyc;
    u8 op = fetch(), mode = mode_of[op], v;
    u32 cycles = cycles_of[op];
    u16 ea = 0, base;
    int crossed = 0;
    switch (mode) {
    case IMM:
        ea = cpu.pc++;
        break;
    case ZP:
        ea = bus = fetch();
        break;
    case ZPX:
        bus = fetch();
        ea = (u8)(bus + cpu.x);
        break;
    case ZPY:
        bus = fetch();
        ea = (u8)(bus + cpu.y);
        break;
    case ABS:
        ea = fetch();
        ea |= (u16)((bus = fetch()) << 8);
        break;
    case ABX:
        base = fetch();
        base |= (u16)((bus = fetch()) << 8);
        ea = (u16)(base + cpu.x);
        crossed = ((base ^ ea) & 0xFF00) != 0;
        break;
    case ABY:
        base = fetch();
        base |= (u16)((bus = fetch()) << 8);
        ea = (u16)(base + cpu.y);
        crossed = ((base ^ ea) & 0xFF00) != 0;
        break;
    case IND:
        base = fetch();
        base |= (u16)(fetch() << 8);
        ea = rd(base);
        ea |= (u16)(rd((u16)((base & 0xFF00) | ((base + 1) & 0xFF))) << 8);    /* (no carry into the page) */
        break;
    case IZX:
        v = (u8)(fetch() + cpu.x);
        ea = rd(v);
        ea |= (u16)((bus = rd((u8)(v + 1))) << 8);
        break;
    case IZY:
        v = fetch();
        base = rd(v);
        base |= (u16)((bus = rd((u8)(v + 1))) << 8);
        ea = (u16)(base + cpu.y);
        crossed = ((base ^ ea) & 0xFF00) != 0;
        break;
    case REL:
        v = fetch();
        ea = (u16)(cpu.pc + (signed char)v);
        break;
    }
    if (crossed && ((mode == IZY && cycles == 5) || (mode != IZY && cycles == 4)))
        cycles++;
    now = start + cycles;
#define BRANCH(taken) \
    if (taken) { \
        cycles += ((cpu.pc ^ ea) & 0xFF00) ? 2 : 1; \
        cpu.pc = ea; \
    } \
    break
#define MODIFY(expr) \
    now = start + cycles - 2; \
    v = rd(ea); \
    now++; \
    wr(ea, v); \
    v = (expr); \
    now++; \
    wr(ea, v)
    switch (op) {
    case 0x01: case 0x05: case 0x09: case 0x0D: case 0x11: case 0x15: case 0x19: case 0x1D:
        nz(cpu.a |= rd(ea));
        break;
    case 0x21: case 0x25: case 0x29: case 0x2D: case 0x31: case 0x35: case 0x39: case 0x3D:
        nz(cpu.a &= rd(ea));
        break;
    case 0x41: case 0x45: case 0x49: case 0x4D: case 0x51: case 0x55: case 0x59: case 0x5D:
        nz(cpu.a ^= rd(ea));
        break;
    case 0x61: case 0x65: case 0x69: case 0x6D: case 0x71: case 0x75: case 0x79: case 0x7D:
        adc(rd(ea));
        break;
    case 0xE1: case 0xE5: case 0xE9: case 0xED: case 0xF1: case 0xF5: case 0xF9: case 0xFD: case 0xEB:
        sbc(rd(ea));
        break;
    case 0x81: case 0x85: case 0x8D: case 0x91: case 0x95: case 0x99: case 0x9D:
        wr(ea, cpu.a);
        break;
    case 0xA1: case 0xA5: case 0xA9: case 0xAD: case 0xB1: case 0xB5: case 0xB9: case 0xBD:
        nz(cpu.a = rd(ea));
        break;
    case 0xC1: case 0xC5: case 0xC9: case 0xCD: case 0xD1: case 0xD5: case 0xD9: case 0xDD:
        compare(cpu.a, rd(ea));
        break;
    case 0x0A: cpu.a = asl(cpu.a); break;
    case 0x2A: cpu.a = rol(cpu.a); break;
    case 0x4A: cpu.a = lsr(cpu.a); break;
    case 0x6A: cpu.a = ror(cpu.a); break;
    case 0x06: case 0x0E: case 0x16: case 0x1E:
        MODIFY(asl(v));
        break;
    case 0x26: case 0x2E: case 0x36: case 0x3E:
        MODIFY(rol(v));
        break;
    case 0x46: case 0x4E: case 0x56: case 0x5E:
        MODIFY(lsr(v));
        break;
    case 0x66: case 0x6E: case 0x76: case 0x7E:
        MODIFY(ror(v));
        break;
    case 0xC6: case 0xCE: case 0xD6: case 0xDE:
        MODIFY((u8)(v - 1));
        nz(v);
        break;
    case 0xE6: case 0xEE: case 0xF6: case 0xFE:
        MODIFY((u8)(v + 1));
        nz(v);
        break;
    case 0x86: case 0x8E: case 0x96:
        wr(ea, cpu.x);
        break;
    case 0x84: case 0x8C: case 0x94:
        wr(ea, cpu.y);
        break;
    case 0xA2: case 0xA6: case 0xAE: case 0xB6: case 0xBE:
        nz(cpu.x = rd(ea));
        break;
    case 0xA0: case 0xA4: case 0xAC: case 0xB4: case 0xBC:
        nz(cpu.y = rd(ea));
        break;
    case 0xE0: case 0xE4: case 0xEC:
        compare(cpu.x, rd(ea));
        break;
    case 0xC0: case 0xC4: case 0xCC:
        compare(cpu.y, rd(ea));
        break;
    case 0x24: case 0x2C:
        v = rd(ea);
        cpu.p = (u8)((cpu.p & ~(FN | FV | FZ)) | (v & 0xC0) | ((v & cpu.a) ? 0 : FZ));
        break;
    case 0x4C: case 0x6C:
        cpu.pc = ea;
        break;
    case 0x20: {                                /* JSR: the return address goes out before the target's high
                                                   byte comes in (the FE board watches for that) */
        u8 lo = fetch(), hi, fe;
        now = start + 4;
        push((u8)(cpu.pc >> 8));
        now++;
        fe = cpu.s == 0xFE;
        push((u8)cpu.pc);
        now++;
        hi = rd(cpu.pc);
        if (fe && cart.type == CFE)
            slots_4k(!(hi & 0x20));
        cpu.pc = (u16)(lo | hi << 8);
        break;
    }
    case 0x60: {                                /* RTS */
        u8 lo, hi, fe;
        now = start + 4;
        lo = pull();
        fe = cpu.s == 0xFE;
        now++;
        hi = pull();
        if (fe && cart.type == CFE)
            slots_4k(!(hi & 0x20));
        cpu.pc = (u16)((lo | hi << 8) + 1);
        break;
    }
    case 0x40:                                  /* RTI */
        now = start + 4;
        cpu.p = (u8)((pull() & ~FB) | FU);
        now++;
        cpu.pc = pull();
        now++;
        cpu.pc |= (u16)(pull() << 8);
        break;
    case 0x00:                                  /* BRK */
        cpu.pc++;
        now = start + 3;
        push((u8)(cpu.pc >> 8));
        now++;
        push((u8)cpu.pc);
        now++;
        push(cpu.p | FB | FU);
        cpu.p |= FI;
        cpu.pc = rd(0xFFFE);
        cpu.pc |= (u16)(rd(0xFFFF) << 8);
        break;
    case 0x10: BRANCH(!(cpu.p & FN));
    case 0x30: BRANCH(cpu.p & FN);
    case 0x50: BRANCH(!(cpu.p & FV));
    case 0x70: BRANCH(cpu.p & FV);
    case 0x90: BRANCH(!(cpu.p & FC));
    case 0xB0: BRANCH(cpu.p & FC);
    case 0xD0: BRANCH(!(cpu.p & FZ));
    case 0xF0: BRANCH(cpu.p & FZ);
    case 0x08: push(cpu.p | FB | FU); break;   /* (with the stack in the TIA, Combat sets ENAxx with PHP) */
    case 0x28: cpu.p = (u8)((pull() & ~FB) | FU); break;
    case 0x48: push(cpu.a); break;
    case 0x68: nz(cpu.a = pull()); break;
    case 0x18: cpu.p &= (u8)~FC; break;
    case 0x38: cpu.p |= FC; break;
    case 0x58: cpu.p &= (u8)~FI; break;
    case 0x78: cpu.p |= FI; break;
    case 0xB8: cpu.p &= (u8)~FV; break;
    case 0xD8: cpu.p &= (u8)~FD; break;
    case 0xF8: cpu.p |= FD; break;
    case 0x88: nz(--cpu.y); break;
    case 0xC8: nz(++cpu.y); break;
    case 0xCA: nz(--cpu.x); break;
    case 0xE8: nz(++cpu.x); break;
    case 0x98: nz(cpu.a = cpu.y); break;
    case 0xA8: nz(cpu.y = cpu.a); break;
    case 0x8A: nz(cpu.a = cpu.x); break;
    case 0xAA: nz(cpu.x = cpu.a); break;
    case 0x9A: cpu.s = cpu.x; break;
    case 0xBA: nz(cpu.x = cpu.s); break;
    /* the unofficial ones */
    case 0xA3: case 0xA7: case 0xAF: case 0xB3: case 0xB7: case 0xBF:    /* LAX */
        nz(cpu.a = cpu.x = rd(ea));
        break;
    case 0xAB:                                  /* LXA (with the usual chip's 'magic' $EE) */
        nz(cpu.a = cpu.x = (u8)((cpu.a | 0xEE) & rd(ea)));
        break;
    case 0x8B:                                  /* ANE */
        nz(cpu.a = (u8)((cpu.a | 0xEE) & cpu.x & rd(ea)));
        break;
    case 0x83: case 0x87: case 0x8F: case 0x97:                         /* SAX */
        wr(ea, cpu.a & cpu.x);
        break;
    case 0xC3: case 0xC7: case 0xCF: case 0xD3: case 0xD7: case 0xDB: case 0xDF:    /* DCP */
        MODIFY((u8)(v - 1));
        compare(cpu.a, v);
        break;
    case 0xE3: case 0xE7: case 0xEF: case 0xF3: case 0xF7: case 0xFB: case 0xFF:    /* ISB */
        MODIFY((u8)(v + 1));
        sbc(v);
        break;
    case 0x03: case 0x07: case 0x0F: case 0x13: case 0x17: case 0x1B: case 0x1F:    /* SLO */
        MODIFY(asl(v));
        nz(cpu.a |= v);
        break;
    case 0x23: case 0x27: case 0x2F: case 0x33: case 0x37: case 0x3B: case 0x3F:    /* RLA */
        MODIFY(rol(v));
        nz(cpu.a &= v);
        break;
    case 0x43: case 0x47: case 0x4F: case 0x53: case 0x57: case 0x5B: case 0x5F:    /* SRE */
        MODIFY(lsr(v));
        nz(cpu.a ^= v);
        break;
    case 0x63: case 0x67: case 0x6F: case 0x73: case 0x77: case 0x7B: case 0x7F:    /* RRA */
        MODIFY(ror(v));
        adc(v);
        break;
    case 0x0B: case 0x2B:                       /* ANC */
        nz(cpu.a &= rd(ea));
        cpu.p = (u8)((cpu.p & ~FC) | (cpu.a >> 7));
        break;
    case 0x4B:                                  /* ALR */
        cpu.a &= rd(ea);
        cpu.a = lsr(cpu.a);
        break;
    case 0x6B:                                  /* ARR */
        arr(rd(ea));
        break;
    case 0xCB:                                  /* SBX */
        v = rd(ea);
        cpu.p = (u8)((cpu.p & ~FC) | ((cpu.a & cpu.x) >= v ? FC : 0));
        nz(cpu.x = (u8)((cpu.a & cpu.x) - v));
        break;
    case 0xBB:                                  /* LAS */
        nz(cpu.a = cpu.x = cpu.s = rd(ea) & cpu.s);
        break;
    case 0x9C:                                  /* SHY, SHX, SHA, TAS: the register ANDed with the address's high
                                                   byte + 1 */
        wr(ea, cpu.y & (u8)((ea >> 8) + 1));
        break;
    case 0x9E:
        wr(ea, cpu.x & (u8)((ea >> 8) + 1));
        break;
    case 0x93: case 0x9F:
        wr(ea, cpu.a & cpu.x & (u8)((ea >> 8) + 1));
        break;
    case 0x9B:
        cpu.s = cpu.a & cpu.x;
        wr(ea, cpu.s & (u8)((ea >> 8) + 1));
        break;
    case 0x02: case 0x12: case 0x22: case 0x32: case 0x42: case 0x52: case 0x62: case 0x72: case 0x92: case 0xB2:
    case 0xD2: case 0xF2:
        cpu.jam = 1;                            /* the 6507 stops for good */
        cpu.pc--;
        break;
    default:
        if (mode != IMP && mode != IMM)
            (void)rd(ea);                       /* the NOPs with an operand read it (NOP $1FF8 switches banks) */
        break;
    }
#undef BRANCH
#undef MODIFY
    cpu.cyc = start + cycles;
    cpu.total += cycles;
    if (cpu.wsync) {                            /* WSYNC: the 6507 halts until the line ends */
        u32 r = cpu.cyc % LINE_CYCLES;
        cpu.wsync = 0;
        if (r) {
            cpu.cyc += LINE_CYCLES - r;
            cpu.total += LINE_CYCLES - r;
        }
    }
}

static void build_modes(void)
{
    for (int op = 0; op < 256; op++) {
        int col = op & 0x1F, m = IMP;
        switch (col) {
        case 0x00: m = (op >= 0x80) ? IMM : IMP; break;         /* (JSR fetches its own address) */
        case 0x01: case 0x03: m = IZX; break;
        case 0x02: m = (op >= 0x80) ? IMM : IMP; break;
        case 0x04: case 0x05: case 0x06: case 0x07: m = ZP; break;
        case 0x09: case 0x0B: m = IMM; break;
        case 0x0C: m = (op == 0x6C) ? IND : ABS; break;
        case 0x0D: case 0x0E: case 0x0F: m = ABS; break;
        case 0x10: m = REL; break;
        case 0x11: case 0x13: m = IZY; break;
        case 0x14: case 0x15: m = ZPX; break;
        case 0x16: case 0x17: m = (op == 0x96 || op == 0xB6 || op == 0x97 || op == 0xB7) ? ZPY : ZPX; break;
        case 0x19: case 0x1B: m = ABY; break;
        case 0x1C: case 0x1D: m = ABX; break;
        case 0x1E: case 0x1F: m = (op == 0x9E || op == 0xBE || op == 0x9F || op == 0xBF) ? ABY : ABX; break;
        }
        mode_of[op] = (u8)m;
    }
}

/* ---- the frame ---- */
void a26_frame(u8 pad, u8 *picture)
{
    /* right left down up -> SWCHA bits 7..4, low when pushed */
    riot.joy = (u8)(((pad & A26_UP) ? 0 : 1) | ((pad & A26_DOWN) ? 0 : 2) | ((pad & A26_LEFT) ? 0 : 4) |
                    ((pad & A26_RIGHT) ? 0 : 8));
    riot.switches = (u8)(0x08 | ((pad & A26_START) ? 0 : 1) | ((pad & A26_SELECT) ? 0 : 2));  /* colour; both
                                                                                       difficulties B */
    tia.fire = (pad & (A26_A | A26_B)) != 0;
    if (tia.latch_mode && tia.fire)
        tia.latched = 0;
    tia.picture = picture;
    tia.rows_drawn = 0;
    tia.frame_done = 0;
    set_row();
    u32 check = cpu.cyc + 8 * LINE_CYCLES;
    while (!tia.frame_done) {
        if (cpu.jam) {                          /* the TIA goes on showing what it was last told */
            cpu.cyc += LINE_CYCLES;
            catch_up(cpu.cyc * 3);
            continue;
        }
        step();
        if ((s32)(cpu.cyc - check) >= 0) {      /* (so a game that never syncs still ends its frame) */
            catch_up(cpu.cyc * 3);
            check = cpu.cyc + 8 * LINE_CYCLES;
        }
    }
    if (picture)
        for (u32 i = tia.rows_drawn * A26_W; i < A26_W * A26_H; i++)
            picture[i] = 0;
    tia.picture = tia.row = 0;
    /* counted from a line's start again, so the counters never run over */
    catch_up(cpu.cyc * 3);
    u32 lines = cpu.cyc / LINE_CYCLES;
    cpu.cyc -= lines * LINE_CYCLES;
    tia.done -= lines * LINE_CLOCKS;
    riot.last -= lines * LINE_CYCLES;
}

void a26_reset(void)
{
    for (unsigned i = 0; i < sizeof(riot); i++)
        ((u8 *)&riot)[i] = 0;
    for (unsigned i = 0; i < sizeof(tia); i++)
        ((u8 *)&tia)[i] = 0;
    riot.shift = 10;
    riot.switches = 0x0B;
    riot.joy = 0x0F;
    for (int o = 0; o < 5; o++) {
        base[o] = 0;
        hm[o] = 8;
        res_line[o] = 0xFFFFFF00u;
    }
    for (int i = 0; i < 4; i++)
        tail[i].on = 0;
    mv.active = mv.k = mv.moving = 0;
    for (int i = 0; i < 2; i++)
        grp_new[i] = grp_old[i] = refp[i] = vdelp[i] = nusiz[i] = enam[i] = resmp[i] = 0;
    enabl_new = enabl_old = vdelbl = 0;
    pf0 = pf1 = pf2 = ctrlpf = 0;
    pf_update();
    colup0 = colup1 = colupf = colubk = 0;
    cx = 0;
    cart_start();
    cpu.a = cpu.x = cpu.y = 0;
    cpu.s = 0xFD;
    cpu.p = FI | FU;
    cpu.jam = cpu.wsync = 0;
    cpu.cyc = cpu.total = 0;
    now = 0;
    cpu.pc = rd(0xFFFC);
    cpu.pc |= (u16)(rd(0xFFFD) << 8);
}

/* ---- the cartridge: which board ---- */
static char *text(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    *p = 0;
    return p;
}

static char *number(char *p, u32 v)
{
    char d[12];
    int n = 0;
    do
        d[n++] = (char)('0' + v % 10);
    while (v /= 10);
    while (n)
        *p++ = d[--n];
    *p = 0;
    return p;
}

static int same(const u8 *a, const u8 *b, u32 n)
{
    while (n--)
        if (*a++ != *b++)
            return 0;
    return 1;
}

static int found(const u8 *image, u32 size, const u8 *what, u32 n, int times)    /* Stella's signatures */
{
    for (u32 i = 0; i + n <= size; i++)
        if (same(image + i, what, n) && !--times)
            return 1;
    return 0;
}

static int superchip(const u8 *image, u32 size)   /* the RAM's 128 + 128 bytes read the same in every bank */
{
    for (u32 at = 0; at < size; at += 4096)
        if (!same(image + at, image + at + 128, 128))
            return 0;
    return 1;
}

static int any(const u8 *image, u32 size, const u8 (*sig)[3], int count)
{
    for (int i = 0; i < count; i++)
        if (found(image, size, sig[i], 3, 1))
            return 1;
    return 0;
}

const char *a26_load(u8 *image, u32 size)
{
    static const u8 f8[2][3] = {{0x8D, 0xF9, 0x1F}, {0x8D, 0xF9, 0xFF}};
    static const u8 e0[8][3] = {{0x8D, 0xE0, 0x1F}, {0x8D, 0xE0, 0x5F}, {0x8D, 0xE9, 0xFF}, {0x0C, 0xE0, 0x1F},
                                {0xAD, 0xE0, 0x1F}, {0xAD, 0xE9, 0xFF}, {0xAD, 0xED, 0xFF}, {0xAD, 0xF3, 0xBF}};
    static const u8 e7[7][3] = {{0xAD, 0xE2, 0xFF}, {0xAD, 0xE5, 0xFF}, {0xAD, 0xE5, 0x1F}, {0xAD, 0xE7, 0x1F},
                                {0x0C, 0xE7, 0x1F}, {0x8D, 0xE7, 0xFF}, {0x8D, 0xE7, 0x1F}};
    static const u8 e78k[3][3] = {{0xAD, 0xE4, 0xFF}, {0xAD, 0xE5, 0xFF}, {0xAD, 0xE6, 0xFF}};
    static const u8 fe[5][5] = {{0x20, 0x00, 0xD0, 0xC6, 0xC5}, {0x20, 0xC3, 0xF8, 0xA5, 0x82},
                                {0xD0, 0xFB, 0x20, 0x73, 0xFE}, {0xD0, 0xFB, 0x20, 0x68, 0xFE},
                                {0x20, 0x00, 0xF0, 0x84, 0xD6}};
    static const u8 sta3e[2] = {0x85, 0x3E}, sta3f[2] = {0x85, 0x3F}, dpcp[4] = {'D', 'P', 'C', '+'};
    static const u8 arm[2][4] = {{0xA0, 0xC1, 0x1F, 0xE0}, {0x00, 0x80, 0x02, 0xE0}};
    static const char *const names[] = {"2K", "4K", "F8", "F6", "F4", "FE", "E0 (Parker Brothers)",
                                        "3F (Tigervision)", "E7 (M Network)", "FA (CBS RAM Plus)"};
    const char *why = 0;
    int type = -1, sc = 0;
    if (size == 0 || size > 512 * 1024)
        why = "not a 2600 cartridge (empty or over 512 KB)";
    else if (size % 8448 == 0 || size == 6 * 1024)
        why = "a Supercharger tape image: not supported";
    else if (size <= 2048 || (size == 4096 && same(image, image + 2048, 2048)))
        type = C2K;
    else if (size == 4096)
        type = C4K;
    else if (size == 8192) {
        int is_f8 = any(image, size, f8, 2);
        if (superchip(image, size)) {
            type = CF8;
            sc = 1;
        } else if (same(image, image + 4096, 4096)) {
            type = C4K;
        } else if (any(image, size, e0, 8)) {
            type = CE0;
        } else if (found(image, size, sta3e, 2, 1) && found(image, size, sta3f, 2, 2)) {
            why = "a 3E board: not supported";
        } else if (found(image, size, sta3f, 2, 2)) {
            type = C3F;
        } else {
            int f = 0;
            for (int i = 0; i < 5; i++)
                f |= found(image, size, fe[i], 5, 1);
            type = (f && !is_f8) ? CFE : any(image, size, e78k, 3) ? CE7 : CF8;
        }
    } else if (size >= 10 * 1024 && size <= 10 * 1024 + 256) {
        why = "a DPC board (Pitfall II): not supported";
    } else if (size == 12 * 1024) {
        type = any(image, size, e7, 7) ? CE7 : CFA;
    } else if (size == 16 * 1024) {
        if (superchip(image, size)) {
            type = CF6;
            sc = 1;
        } else if (any(image, size, e7, 7)) {
            type = CE7;
        } else if (found(image, size, sta3e, 2, 1) && found(image, size, sta3f, 2, 2)) {
            why = "a 3E board: not supported";
        } else {
            type = CF6;
        }
    } else if (size == 32 * 1024) {
        int a = 0;
        for (u32 i = 0; i + 4 <= size && i < 1024; i++)
            a |= same(image + i, arm[0], 4) || same(image + i, arm[1], 4);
        if (a || found(image, size, dpcp, 4, 2) || found(image, size, (const u8 *)"CDF", 3, 1) ||
            found(image, size, (const u8 *)"BUS", 3, 1))
            why = "an ARM board (DPC+, CDF, BUS): not supported";
        else if (superchip(image, size)) {
            type = CF4;
            sc = 1;
        } else if (found(image, size, sta3e, 2, 1) && found(image, size, sta3f, 2, 2)) {
            why = "a 3E board: not supported";
        } else if (found(image, size, sta3f, 2, 2)) {
            type = C3F;
        } else {
            type = CF4;
        }
    } else if (size % 2048 == 0 && size >= 64 * 1024 && found(image, size, sta3f, 2, 2) &&
               !found(image, size, sta3e, 2, 1)) {
        type = C3F;
    } else {
        why = "a cartridge size or board this player does not know (it knows 2K 4K F8 F6 F4 FE E0 3F E7 FA, "
              "the F8 F6 F4 with Superchip RAM)";
    }
    if (why)
        return why;
    if (size < 2048) {                          /* a smaller image repeats over the 2K */
        for (u32 i = 0; i < 2048; i++)
            small_rom[i] = image[i % size];
        cart.rom = small_rom;
        cart.size = 2048;
    } else {
        cart.rom = image;
        cart.size = size;
    }
    cart.type = (u8)type;
    cart.superchip = (u8)sc;
    cart.slow_below = 0;
    cart.slow_from = 0x1000;
    cart.hot_first = 1;
    cart.hot_last = 0;
    switch (type) {
    case CF8: cart.hot_first = 0xFF8; cart.hot_last = 0xFF9; break;
    case CF6: cart.hot_first = 0xFF6; cart.hot_last = 0xFF9; break;
    case CF4: cart.hot_first = 0xFF4; cart.hot_last = 0xFFB; break;
    case CFA: cart.hot_first = 0xFF8; cart.hot_last = 0xFFA; cart.slow_below = 0x200; break;
    case CE0: cart.slow_from = 0xFE0; break;
    case CE7: cart.slow_below = 0xA00; cart.slow_from = 0xFE0; break;
    }
    if (type == CF8 || type == CF6 || type == CF4 || type == CFA)
        cart.slow_from = cart.hot_first;
    if (sc)
        cart.slow_below = 0x100;
    cart.banks = (u8)(type == C3F || type == CE7 ? size / 2048 : type == CE0 ? 8 : size / 4096);
    char *p = text(info, names[type]);
    if (sc)
        p = text(p, " + Superchip");
    p = text(p, ": ");
    p = number(p, size < 1024 ? size : size / 1024);
    p = text(p, size < 1024 ? " bytes" : " KB");
    if (type >= CF8) {
        p = text(p, " in ");
        p = number(p, type == CE0 ? 1 : (type == C3F || type == CE7) ? 2 : 4);
        p = text(p, " KB banks");
    }
    if (sc)
        p = text(p, ", 128 bytes of RAM");
    else if (type == CFA)
        p = text(p, ", 256 bytes of RAM");
    else if (type == CE7)
        p = text(p, ", 2 KB of RAM");
    build_modes();
    build_tables();
    a26_reset();
    return 0;
}

const char *a26_info(void)
{
    return info;
}

u8 *a26_save_ram(u32 *size, int *battery)
{
    *size = 0;
    *battery = 0;
    return cart_ram;
}

int a26_save_changed(void)
{
    return 0;
}

u32 a26_cpu_cycles(void)
{
    return cpu.total;
}

int a26_jammed(void)
{
    return cpu.jam;
}

u32 a26_frame_lines(void)
{
    return tia.last_lines;
}

#ifdef A26_CPU_TEST
void a26_test_start(u16 pc)
{
    (void)cart_read;                            /* (the machine's memory is not used in this build) */
    (void)cart_write;
    (void)riot_read;
    (void)riot_write;
    (void)tia_read;
    (void)tia_write;
    build_modes();
    cpu.a = cpu.x = cpu.y = 0;
    cpu.s = 0xFD;
    cpu.p = FI | FU;
    cpu.pc = pc;
    cpu.cyc = cpu.total = 0;
    cpu.jam = 0;
}

void a26_test_step(u16 *pc, u8 *a, u8 *p, u32 *cycles)
{
    step();
    *pc = cpu.pc;
    *a = cpu.a;
    *p = cpu.p;
    *cycles = cpu.total;
}

void a26_test_registers(int set, u16 *pc, u8 *a, u8 *x, u8 *y, u8 *s, u8 *p)
{
    if (set) {
        cpu.pc = *pc;
        cpu.a = *a;
        cpu.x = *x;
        cpu.y = *y;
        cpu.s = *s;
        cpu.p = *p;
        return;
    }
    *pc = cpu.pc;
    *a = cpu.a;
    *x = cpu.x;
    *y = cpu.y;
    *s = cpu.s;
    *p = cpu.p;
}
#endif
