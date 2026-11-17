/* A Sega Master System and a Game Gear: see sms.h.

   NTSC timing: 262 lines of 228 T-states (342 pixel clocks), 59.92 frames
   a second.  A line here starts where the picture chip's V counter changes:
   the line is drawn there in one go from the registers as they are then
   (the chip latches its X scroll at about the same point), the frame and
   line interrupts come 1 and 2 T-states later, and the CPU runs the line's
   228 T-states.  So a game that changes the scroll or the palette in a line
   interrupt gets it on the next line, as on the console (a change in the
   middle of a line shows from the next one).  These points, the H counter
   (latched by TH) and the sprite flags in the border are set so that
   FluBBa's VDPTEST passes all its tests but the two that time a sprite
   collision / overflow to the pixel.

   The picture chip in mode 4: 32 x 28 tiles of 8 x 8 pixels, 16 colours
   each from one of two palettes; the scroll, its two locks (the top two
   rows not scrolled sideways, the right eight columns not up or down);
   the left column blanked; 64 sprites, 8 x 8 or 8 x 16, doubled in size
   ("zoom"), eight to a line (the ninth sets the overflow flag), their
   collision flag; the 224-line mode.  The Game Gear's chip is the same
   with a palette of 12-bit colours, and its screen shows the middle
   160 x 144.

   The older TMS9918A modes, shared by the SG-1000's own chip and the
   Master System's (whose mode 4 bit, R0 bit 2, clear selects them):
   Graphics I (mode 0: 32 x 24 tiles, a colour pair for every 8 patterns),
   Graphics II (mode 2: three thirds of 256 patterns, a colour pair for
   every pattern row; R4 bits 0-1 and R3 bits 0-6 AND-mask the pattern /
   colour table index, as games rely on), Multicolour (mode 3: 4 x 4
   blocks), Text (mode 1: 40 columns of 6 pixels between 8-pixel borders,
   R7's two colours, no sprites); 32 sprites, 8 x 8 or 16 x 16, magnified,
   four to a line (the fifth sets status bit 6 and its number in bits 0-4,
   else they hold the last sprite looked at), the early clock bit (32
   pixels to the left), Y = 208 ends the list, the collision flag (pixels
   of transparent sprites collide too); colour 0 is the backdrop, R7's low
   four bits.  The SG-1000's colours are the TMS9918A's (TI's NTSC
   values); the Master System's chip shows the 15 colours as its own fixed
   6-bit ones.  The SG-1000's frame flag comes a line earlier than the
   Master System's (at the first border line, as the TMS's), and it has no
   line interrupt, no V / H counters, no mode 4.

   The cartridge: Sega's mapper (three 16 KB slots, the first 1 KB fixed;
   32 KB of cartridge RAM switched in at 0x8000), or Codemasters' (detected
   by its header's checksum).  Up to 4 MB of ROM.  The sound chip's ports
   take writes and do nothing (no sound).

   The SG-1000 / SC-3000: the same Z80 and timing; ROM at 0x0000-0xBFFF
   (up to 48 KB used as it is, mirrored when smaller; bigger: Sega's
   mapper); RAM at 0xC000-0xFFFF, 1 KB mirrored (the SC-3000: 2 KB).  A few
   cartridges have 8 KB of their own RAM, which nothing in a cartridge file
   says -- a rule here: a ROM of 32 KB or less gets 8 KB of RAM at
   0x8000-0x9FFF, mirrored at 0xA000 (The Castle, Othello Multivision's
   carts); a bigger one whose 0x2000-0x3FFF is one byte throughout gets it
   there (the Taiwanese conversions of MSX games).  The picture chip at
   ports 0x80-0xBF (A0: data / control), the sound chip's at 0x40-0x7F
   (taken), the pads at 0xC0-0xFF (A0: 0xDC / 0xDD; the SC-3000's keyboard
   behind its 8255 is not there: the pads read whatever row is chosen),
   the Pause button an NMI. */
#include "sms.h"
#include "z80.h"

#define LINE_T  228
#define LINES   262
#define FRAME_T (LINE_T * LINES)
/* where things happen in a line, in T-states from its start (VDPTEST's timing tests pass with these) */
#define FRAME_IRQ_T 1                   /* the frame interrupt */
#define LINE_IRQ_T  2                   /* a line interrupt */
#define H_PHASE     214                 /* the H counter at the line's start: (214 * 3 + 2) / 4 = 161, 0xF6 */

static struct z80 cpu;

/* ---- the cartridge ---- */
static u8 *rom;
static u32 rom_banks;                   /* 16 KB each */
static u8 codemasters, gg;
static u8 sg;                           /* 1: an SG-1000, 2: an SC-3000 (0: a Master System / Game Gear) */
static u32 rom_size;
static u16 ram_mask = 0x1FFF;           /* the console's RAM: 8 KB (the SG-1000's 1 KB, the SC-3000's 2 KB) */
static u16 sg_ram_at;                   /* an SG-1000 cartridge's 8 KB of RAM: 0x2000, 0x8000, or 0 (none) */
static u8 ram[0x2000], cart_ram[0x8000], junk[0x400], open_bus[0x400];
static u8 slot[3], ram_ctl;             /* the mapper's registers (0xFFFD-0xFFFF, 0xFFFC) */
static u8 mem_ctl;                      /* port 0x3E: bit 6 set, the cartridge slot is switched off */
static u8 ram_dirty;
static char info[96];

/* ---- the picture chip ---- */
static u8 vram[0x4000], cram[64], reg[16];
static u16 pal[32];                     /* cram as pixels */
static u16 tms_pal[16];                 /* the TMS modes' 15 colours (0: black, as the backdrop's colour 0) */
static u16 addr;
static u8 code, latch, first, buffer, gg_latch;
static u8 status, line_pending, early_read;
static int counter;                     /* the line counter (R10 is what it reloads) */
static int line;                        /* the line the chip is on (its V counter, mapped) */
static u8 hc_latched;
static u16 screen[SMS_W * SMS_H_MAX];
static u32 tile_x[256], tile_xr[256];   /* a pattern byte spread out: bit 7 (left) into bit 0, bit 6 into bit 4,
                                           ...; and mirrored */

/* ---- the controllers ---- */
static u8 pad1, pad2, io_ctl = 0xFF, pad_was;
static u32 cycles_hi;                   /* T-states of the frames before this one */

static int active_lines(void)           /* 192, or 224 with M1 and M2 set in mode 4 */
{
    return (!sg && (reg[0] & 0x06) == 0x06 && (reg[1] & 0x18) == 0x10) ? 224 : 192;
}

static int mode4(void)                  /* the Master System's mode 4 (else a TMS9918 mode) */
{
    return !sg && (reg[0] & 0x04);
}

static void irq_update(void)
{
    cpu.irq = (u8)(((status & 0x80) && (reg[1] & 0x20)) || (line_pending && (reg[0] & 0x10)));
}

/* ---- memory ---- */
static void map(void)
{
    for (int s = 0; s < 3; s++) {
        u8 *bank = rom + ((slot[s] % rom_banks) << 14);
        for (int i = 0; i < 16; i++) {
            cpu.rd[s * 16 + i] = bank + (i << 10);
            cpu.wr[s * 16 + i] = junk;
        }
    }
    if (!codemasters)
        cpu.rd[0] = rom;                /* (Sega's mapper: the first 1 KB is always bank 0's) */
    else
        cpu.wr[0] = cpu.wr[16] = cpu.wr[32] = 0;    /* (writes there are the mapper's) */
    if (ram_ctl & 0x08) {
        u8 *at = cart_ram + ((ram_ctl & 0x04) ? 0x4000 : 0);
        for (int i = 0; i < 16; i++) {
            cpu.rd[32 + i] = at + (i << 10);
            cpu.wr[32 + i] = 0;         /* (through write(): it notes the change for the save) */
        }
    }
    if (mem_ctl & 0x40)                 /* (no cartridge: the bus reads 0xFF) */
        for (int i = 0; i < 48; i++)
            cpu.rd[i] = open_bus;
    for (int i = 0; i < 16; i++) {
        cpu.rd[48 + i] = ram + ((i << 10) & ram_mask);
        cpu.wr[48 + i] = ram + ((i << 10) & ram_mask);
    }
    if (!codemasters)
        cpu.wr[63] = 0;                 /* (0xFFFC-0xFFFF: the mapper's registers, and RAM) */
}

static void sg_map(void)                /* an SG-1000 cartridge of up to 48 KB: no mapper */
{
    u32 pages = rom_size >> 10;
    for (int i = 0; i < 48; i++) {
        cpu.rd[i] = rom + (((u32)i % pages) << 10);
        cpu.wr[i] = junk;
    }
    if (sg_ram_at)
        for (int i = 0; i < (sg_ram_at == 0x8000 ? 16 : 8); i++) {
            cpu.rd[(sg_ram_at >> 10) + i] = cart_ram + ((i & 7) << 10);
            cpu.wr[(sg_ram_at >> 10) + i] = cart_ram + ((i & 7) << 10);
        }
    for (int i = 0; i < 16; i++) {
        cpu.rd[48 + i] = ram + ((i << 10) & ram_mask);
        cpu.wr[48 + i] = ram + ((i << 10) & ram_mask);
    }
}

static void write(struct z80 *c, u16 a, u8 v)
{
    (void)c;
    if (a >= 0xC000) {
        ram[a & ram_mask] = v;
        if (a >= 0xFFFC) {
            if (a == 0xFFFC)
                ram_ctl = v;
            else
                slot[a - 0xFFFD] = v;
            map();
        }
        return;
    }
    if (a >= 0x8000 && (ram_ctl & 0x08)) {
        u8 *p = &cart_ram[((ram_ctl & 0x04) ? 0x4000 : 0) + (a & 0x3FFF)];
        if (*p != v) {
            *p = v;
            ram_dirty = 1;
        }
        return;
    }
    if (codemasters && !(a & 0x3FFF)) { /* 0x0000, 0x4000, 0x8000: that slot's bank */
        slot[a >> 14] = v;
        map();
    }
}

/* ---- the picture chip's ports ---- */
static const u8 level[4] = {0, 10, 21, 31};         /* the Master System's 6-bit colours: --BBGGRR */

static u16 colour(int i)
{
    if (gg) {                           /* 12 bits: ----BBBB GGGGRRRR */
        u8 lo = cram[i * 2], hi = cram[i * 2 + 1];
        u32 r = lo & 15, g = lo >> 4, b = hi & 15;
        return (u16)((r << 1 | r >> 3) | (g << 1 | g >> 3) << 5 | (b << 1 | b >> 3) << 10);
    }
    u8 v = cram[i];
    return (u16)(level[v & 3] | level[(v >> 2) & 3] << 5 | level[(v >> 4) & 3] << 10);
}

static void tms_colours(void)
{
    /* the TMS9918A's: transparent, black, medium green, light green, dark blue, light blue, dark red, cyan,
       medium red, light red, dark yellow, light yellow, dark green, magenta, grey, white (TI's NTSC values) */
    static const u8 rgb[16][3] = {
        {0x00, 0x00, 0x00}, {0x00, 0x00, 0x00}, {0x21, 0xC8, 0x42}, {0x5E, 0xDC, 0x78},
        {0x54, 0x55, 0xED}, {0x7D, 0x76, 0xFC}, {0xD4, 0x52, 0x4D}, {0x42, 0xEB, 0xF5},
        {0xFC, 0x55, 0x54}, {0xFF, 0x79, 0x78}, {0xD4, 0xC1, 0x54}, {0xE6, 0xCE, 0x80},
        {0x21, 0xB0, 0x3B}, {0xC9, 0x5B, 0xBA}, {0xCC, 0xCC, 0xCC}, {0xFF, 0xFF, 0xFF}};
    /* the Master System's chip shows them as these of its own (--BBGGRR) */
    static const u8 sms[16] = {0x00, 0x00, 0x08, 0x0C, 0x10, 0x30, 0x01, 0x3C,
                               0x02, 0x03, 0x05, 0x0F, 0x04, 0x33, 0x15, 0x3F};
    for (int i = 0; i < 16; i++) {
        u8 v = sms[i];
        tms_pal[i] = sg ? (u16)(rgb[i][0] >> 3 | (rgb[i][1] >> 3) << 5 | (rgb[i][2] >> 3) << 10)
                        : (u16)(level[v & 3] | level[(v >> 2) & 3] << 5 | level[(v >> 4) & 3] << 10);
    }
}

static void data_write(u8 v)
{
    first = 0;
    if (code == 3) {
        if (gg) {                       /* (two bytes a colour: the second writes both) */
            if (!(addr & 1))
                gg_latch = v;
            else {
                cram[(addr & 0x3E)] = gg_latch;
                cram[(addr & 0x3E) | 1] = v & 0x0F;
                pal[(addr >> 1) & 31] = colour((addr >> 1) & 31);
            }
        } else {
            cram[addr & 31] = v & 0x3F;
            pal[addr & 31] = colour(addr & 31);
        }
    } else {
        vram[addr & 0x3FFF] = v;
    }
    buffer = v;
    addr = (addr + 1) & 0x3FFF;
}

static u8 data_read(void)
{
    u8 v = buffer;
    first = 0;
    buffer = vram[addr];
    addr = (addr + 1) & 0x3FFF;
    return v;
}

static void control_write(u8 v)
{
    if (!first) {
        first = 1;
        latch = v;
        addr = (u16)((addr & 0x3F00) | v);      /* (the first byte sets the address's low half at once) */
        return;
    }
    first = 0;
    code = v >> 6;
    addr = (u16)((v & 0x3F) << 8 | latch);
    if (code == 0) {
        buffer = vram[addr];
        addr = (addr + 1) & 0x3FFF;
    } else if (code == 2) {
        reg[v & 15] = latch;
        irq_update();
    }
}

static u8 status_read(void)
{
    u8 v = status | 0x1F;
    if (line == active_lines() && cpu.cycles >= (line + 1) * LINE_T) {
        v |= 0x80;                      /* (an IN at the line's very end: the flag is there, its interrupt not yet) */
        early_read = 1;
    }
    status = 0;
    line_pending = 0;
    first = 0;
    irq_update();
    return v;
}

/* the SG-1000's TMS9918A: the second byte's bit 7 writes a register (0-7), whatever its bit 6 */
static void tms_control_write(u8 v)
{
    if (!first) {
        first = 1;
        latch = v;
        addr = (u16)((addr & 0x3F00) | v);
        return;
    }
    first = 0;
    addr = (u16)((v & 0x3F) << 8 | latch);
    if (v & 0x80) {
        code = 2;
        reg[v & 7] = latch;
        irq_update();
    } else {
        code = (v >> 6) & 1;
        if (!code) {
            buffer = vram[addr];
            addr = (addr + 1) & 0x3FFF;
        }
    }
}

static u8 tms_status_read(void)         /* its bits 0-4: the fifth sprite's number, or the last one looked at */
{
    u8 v = status;
    if (line == active_lines() - 1 && cpu.cycles >= (line + 1) * LINE_T) {
        v |= 0x80;                      /* (as status_read: at the very end of the line before the flag's) */
        early_read = 1;
    }
    status &= 0x1F;
    first = 0;
    irq_update();
    return v;
}

static u8 v_counter(void)
{
    int top = active_lines() == 224 ? 0xEA : 0xDA;  /* (then it jumps back by 6, to count up to 0xFF) */
    int y = cpu.cycles / LINE_T;        /* (an IN that began at a line's end may read in the next one) */
    if (y >= LINES)
        y -= LINES;
    return (u8)(y <= top ? y : y - 6);
}

static u8 h_counter(void)
{
    /* 3 counts every 4 T-states (171 a line: 0x00-0x93, then 0xE9-0xFF), 0xF6 where the line starts here */
    int k = (cpu.cycles - line * LINE_T + H_PHASE) % LINE_T, v = (k * 3 + 2) >> 2;
    return (u8)(v > 0x93 ? v + 0xE9 - 0x94 : v);
}

/* ---- I/O ports ---- */
static u8 port_dc(void)                 /* the controllers: 1 not pressed */
{
    return (u8)~(((pad1 & SMS_UP) ? 1 : 0) | ((pad1 & SMS_DOWN) ? 2 : 0) | ((pad1 & SMS_LEFT) ? 4 : 0) |
                 ((pad1 & SMS_RIGHT) ? 8 : 0) | ((pad1 & SMS_1) ? 0x10 : 0) | ((pad1 & SMS_2) ? 0x20 : 0) |
                 ((pad2 & SMS_UP) ? 0x40 : 0) | ((pad2 & SMS_DOWN) ? 0x80 : 0));
}

static u8 port_dd(void)                 /* (bits 4-7 1) */
{
    return (u8)~(((pad2 & SMS_LEFT) ? 1 : 0) | ((pad2 & SMS_RIGHT) ? 2 : 0) | ((pad2 & SMS_1) ? 4 : 0) |
                 ((pad2 & SMS_2) ? 8 : 0));
}

static u8 in(struct z80 *c, u16 port)
{
    (void)c;
    u8 p = (u8)port;
    if (gg && p < 7) {
        static const u8 link[7] = {0, 0x7F, 0xFF, 0x00, 0xFF, 0x00, 0xFF};
        return p ? link[p] : (u8)(0x40 | ((pad1 & SMS_START) ? 0 : 0x80));   /* (bit 6: not a Japanese one) */
    }
    switch (p & 0xC1) {
    case 0x40: return v_counter();
    case 0x41: return hc_latched;
    case 0x80: return data_read();
    case 0x81: return status_read();
    case 0xC0:
    case 0xC1: {
        if (!(p & 1))
            return port_dc();
        u8 b = port_dd();
        b &= 0x3F;
        /* TH: an export console reads back what port 0x3F drives it to, as an output */
        b |= (u8)(((io_ctl & 0x02) ? 0x40 : (io_ctl & 0x20) << 1) | ((io_ctl & 0x08) ? 0x80 : (io_ctl & 0x80)));
        return b;
    }
    }
    return 0xFF;
}

static void out(struct z80 *c, u16 port, u8 v)
{
    (void)c;
    u8 p = (u8)port;
    if (gg && p < 7)
        return;                         /* (the link port, the stereo register) */
    switch (p & 0xC1) {
    case 0x01: {                        /* the controllers' pins: TH going from 0 to 1 latches the H counter */
        u8 th_was = (u8)(((io_ctl & 0x02) ? 1 : (io_ctl >> 5) & 1) | ((io_ctl & 0x08) ? 2 : (io_ctl >> 6) & 2));
        u8 th_now = (u8)(((v & 0x02) ? 1 : (v >> 5) & 1) | ((v & 0x08) ? 2 : (v >> 6) & 2));
        if (th_now & ~th_was)
            hc_latched = h_counter();
        io_ctl = v;
        return;
    }
    case 0x00:                          /* memory control (not on the Game Gear) */
        if (!gg) {
            mem_ctl = v;
            map();
        }
        return;
    case 0x80: data_write(v); return;
    case 0x81: control_write(v); return;
    }
    /* 0x40-0x7F the sound chip, 0xC0-0xFF nothing on an export console */
}

static u8 sg_in(struct z80 *c, u16 port)
{
    (void)c;
    switch ((u8)port & 0xC1) {
    case 0x80: return data_read();
    case 0x81: return tms_status_read();
    case 0xC0: return port_dc();
    case 0xC1: return port_dd();
    }
    return 0xFF;                        /* (0x00-0x7F: nothing to read) */
}

static void sg_out(struct z80 *c, u16 port, u8 v)
{
    (void)c;
    switch ((u8)port & 0xC1) {
    case 0x80: data_write(v); return;
    case 0x81: tms_control_write(v); return;
    }
    /* 0x40-0x7F the sound chip; 0xC0-0xFF the SC-3000's 8255 (its keyboard's rows) */
}

/* ---- drawing ---- */
/* the sprites on line y (its V counter's value): the first eight (the earlier one in front) into row, the
   ninth only a flag; row 0: only the flags (the picture switched off, or a line of the border -- the chip
   looks at the sprites there too) */
static void sprites(int y, u8 *row, int height)
{
    int h = (reg[1] & 2) ? 16 : 8, zoom = reg[1] & 1;
    const u8 *sat = vram + ((reg[5] & 0x7E) << 7);
    u8 taken[256 + 16];
    int n = 0;
    for (int i = 0; i < 64; i++) {
        int sy = sat[i];
        if (sy == 0xD0 && height == 192)
            break;
        int top = sy + 1;
        if (top > 240 && y < height)
            top -= 256;                 /* (partly above the picture's top) */
        if (y < top || y >= top + (h << zoom))
            continue;
        if (n == 8) {
            if (y < height)
                status |= 0x40;         /* (not in the border, where collisions still happen) */
            break;
        }
        if (!n)
            for (int x = 0; x < 256 + 16; x++)
                taken[x] = 0;
        n++;
        if (!(reg[1] & 0x40))
            continue;                   /* (switched off: no collisions) */
        int sx = sat[0x80 + i * 2] - ((reg[0] & 0x08) ? 8 : 0);
        u32 tile = sat[0x81 + i * 2] | ((reg[6] & 0x04) ? 0x100 : 0);
        int r = (y - top) >> zoom;
        if (h == 16)
            tile = (tile & ~1u) + (u32)(r >> 3);
        const u8 *p = vram + ((tile * 32 + (u32)(r & 7) * 4) & 0x3FFF);
        u32 px = tile_x[p[0]] | tile_x[p[1]] << 1 | tile_x[p[2]] << 2 | tile_x[p[3]] << 3;
        for (int i2 = 0; i2 < (8 << zoom); i2++) {
            u8 v = (u8)((px >> ((i2 >> zoom) * 4)) & 15);
            int x = sx + i2;
            if (!v || x < 0 || x >= 256)
                continue;
            if (taken[x]) {
                status |= 0x20;         /* two sprites' pixels on each other */
                continue;
            }
            taken[x] = 1;
            if (row && !(row[x] & 0x40))
                row[x] = (u8)(16 | v);
        }
    }
}

static void draw_line(int y, int height)
{
    u8 buf[256 + 24];                   /* colour numbers (0-31), 8 pixels off; | 0x40: a tile in front of the
                                           sprites, its colour not 0 */
    u8 *row = buf + 8;
    u16 *out = screen + y * SMS_W;
    if (!(reg[1] & 0x40)) {             /* the picture switched off: the backdrop */
        u16 back = pal[16 + (reg[7] & 15)];
        for (int x = 0; x < SMS_W; x++)
            out[x] = back;
        sprites(y, 0, height);
        return;
    }
    /* the background */
    int hs = ((reg[0] & 0x40) && y < 16) ? 0 : reg[8];
    u16 names;
    int rows;
    if (height == 224) {
        names = (u16)(((reg[2] & 0x0C) << 10) | 0x0700);
        rows = 256;
    } else {
        names = (u16)((reg[2] & 0x0E) << 10);
        rows = 224;
    }
    for (int k = -1; k < 32; k++) {
        int vs = ((reg[0] & 0x80) && k >= 24) ? 0 : reg[9];
        int sy = (y + vs) % rows;
        int col = (k - (hs >> 3)) & 31;
        const u8 *n = vram + names + (sy >> 3) * 64 + col * 2;
        u32 tile = (u32)(n[0] | (n[1] & 1) << 8);
        u8 attr = n[1];
        int r = (attr & 4) ? 7 - (sy & 7) : (sy & 7);
        const u8 *p = vram + tile * 32 + r * 4;
        const u32 *x = (attr & 2) ? tile_xr : tile_x;
        u32 px = x[p[0]] | x[p[1]] << 1 | x[p[2]] << 2 | x[p[3]] << 3;
        u8 high = (attr & 0x10) ? 0x40 : 0, base = (attr & 0x08) ? 16 : 0;
        u8 *to = row + k * 8 + (hs & 7);
        for (int i = 0; i < 8; i++, px >>= 4) {
            u8 v = (u8)(px & 15);
            to[i] = (u8)(base | v | (v ? high : 0));
        }
    }
    sprites(y, row, height);
    if (reg[0] & 0x20)                  /* the left column blanked: the backdrop */
        for (int x = 0; x < 8; x++)
            row[x] = (u8)(16 | (reg[7] & 15));
    for (int x = 0; x < SMS_W; x++)
        out[x] = pal[row[x] & 31];
}

/* the TMS modes' sprites on line y into row (colours 1-15): the first four, the earlier one in front */
static void tms_sprites(int y, u8 *row)
{
    int size = (reg[1] & 2) ? 16 : 8, mag = reg[1] & 1, h = size << mag;
    const u8 *sat = vram + ((reg[5] & 0x7F) << 7);
    u32 patterns = (u32)(reg[6] & 7) << 11;
    u8 taken[256];                      /* bit 0: a sprite's pixel (they collide), bit 1: a colour drawn */
    int n = 0, i;
    for (i = 0; i < 32; i++, sat += 4) {
        if (sat[0] == 0xD0)
            break;                      /* (the list's end) */
        int r = (u8)(y - sat[0] - 1);   /* (its top line is Y + 1; Y from 0xE1 up: partly above the top) */
        if (r >= h)
            continue;
        if (n == 4) {                   /* the fifth on the line: not drawn, its number noted */
            if (!(status & 0x40))
                status = (u8)((status & 0xA0) | 0x40 | i);
            break;
        }
        if (!n++)
            for (int x = 0; x < 256; x++)
                taken[x] = 0;
        r >>= mag;
        u32 a = patterns | (u32)(size == 16 ? sat[2] & 0xFC : sat[2]) << 3 | (u32)r;
        u32 bits = (u32)vram[a] << 8 | (size == 16 ? vram[a + 16] : 0);    /* (16 x 16: the right half 16 on) */
        int x0 = sat[1] - ((sat[3] & 0x80) ? 32 : 0);
        u8 c = sat[3] & 15;
        for (int k = 0; k < (size << mag); k++) {
            if (!(bits & (0x8000u >> (k >> mag))))
                continue;
            int x = x0 + k;
            if (x < 0 || x > 255)
                continue;
            if (taken[x] & 1)
                status |= 0x20;         /* two sprites' pixels on each other (a transparent one's too) */
            taken[x] |= 1;
            if (c && !(taken[x] & 2)) {
                taken[x] |= 2;
                row[x] = c;
            }
        }
    }
    if (!(status & 0x40))
        status = (u8)((status & 0xE0) | (i < 32 ? i : 31));
}

/* a line of a TMS9918 mode (Graphics I / II, Multicolour, Text) */
static void draw_tms(int y)
{
    u8 row[256];                        /* colours 0-15 (0: the backdrop) */
    u16 *out = screen + y * SMS_W;
    u16 back = tms_pal[reg[7] & 15];
    if (!(reg[1] & 0x40)) {             /* the picture switched off: the backdrop, no sprites */
        for (int x = 0; x < SMS_W; x++)
            out[x] = back;
        return;
    }
    const u8 *names = vram + ((reg[2] & 0x0F) << 10);
    if (reg[1] & 0x10) {                /* Text: 40 x 24 patterns' left 6 pixels in R7's two colours */
        const u8 *p = vram + ((reg[4] & 7) << 11) + (y & 7), *n = names + (y >> 3) * 40;
        u8 fg = reg[7] >> 4, bg = reg[7] & 15;
        for (int x = 0; x < 8; x++)
            row[x] = row[248 + x] = 0;
        for (int k = 0; k < 40; k++) {
            u8 bits = p[n[k] * 8];
            for (int i = 0; i < 6; i++)
                row[8 + k * 6 + i] = (bits & (0x80 >> i)) ? fg : bg;
        }
    } else if (reg[1] & 0x08) {         /* Multicolour: a pattern's two bytes for each row of 4 x 4 blocks */
        const u8 *p = vram + ((reg[4] & 7) << 11) + ((y >> 3) & 3) * 2 + ((y >> 2) & 1), *n = names + (y >> 3) * 32;
        for (int k = 0; k < 32; k++) {
            u8 v = p[n[k] * 8];
            for (int i = 0; i < 4; i++) {
                row[k * 8 + i] = v >> 4;
                row[k * 8 + 4 + i] = v & 15;
            }
        }
    } else {                            /* Graphics I and II */
        const u8 *n = names + (y >> 3) * 32;
        for (int k = 0; k < 32; k++) {
            u8 bits, c;
            if (reg[0] & 0x02) {        /* II: (third, name, row) masked by R4 bits 0-1 and R3 bits 0-6 */
                u32 i = (u32)((y >> 6) << 11 | n[k] << 3 | (y & 7));
                bits = vram[(u32)(reg[4] & 4) << 11 | (i & ((u32)(reg[4] & 3) << 11 | 0x7FF))];
                c = vram[(u32)(reg[3] & 0x80) << 6 | (i & ((u32)(reg[3] & 0x7F) << 6 | 0x3F))];
            } else {                    /* I: a colour byte for every 8 patterns */
                bits = vram[((reg[4] & 7) << 11) + n[k] * 8 + (y & 7)];
                c = vram[(reg[3] << 6) + (n[k] >> 3)];
            }
            u8 fg = c >> 4, bg = c & 15;
            for (int i = 0; i < 8; i++)
                row[k * 8 + i] = (bits & (0x80 >> i)) ? fg : bg;
        }
    }
    if (!(reg[1] & 0x10))
        tms_sprites(y, row);
    for (int x = 0; x < SMS_W; x++)
        out[x] = row[x] ? tms_pal[row[x]] : back;
}

/* ---- the outside ---- */
static char *put(char *p, const char *s)
{
    while (*s && p < info + sizeof(info) - 1)
        *p++ = *s++;
    *p = 0;
    return p;
}

static char *put_number(char *p, u32 v)
{
    char d[12];
    int n = 0;
    do
        d[n++] = (char)('0' + v % 10);
    while (v /= 10);
    while (n && p < info + sizeof(info) - 1)
        *p++ = d[--n];
    *p = 0;
    return p;
}

const char *sms_load(u8 *image, u32 size, int kind)
{
    if ((size & 0x3FFF) == 512) {       /* a copier's header */
        image += 512;
        size -= 512;
    }
    if (size < 0x2000)
        return "not a cartridge file (shorter than 8 KB)";
    if (size > 0x400000)
        return "not a cartridge this player can hold (more than 4 MB)";
    rom = image;
    rom_size = size;
    rom_banks = (size + 0x3FFF) >> 14;
    gg = kind == SMS_GAME_GEAR;
    sg = kind == SMS_SG1000 ? 1 : kind == SMS_SC3000 ? 2 : 0;
    /* Codemasters' header: a checksum and its complement to 0x10000 at 0x7FE6 */
    codemasters = 0;
    if (size >= 0x8000 && !sg) {
        u32 sum = (u32)(image[0x7FE6] | image[0x7FE7] << 8), inv = (u32)(image[0x7FE8] | image[0x7FE9] << 8);
        codemasters = sum + inv == 0x10000 && sum;
    }
    for (int i = 0; i < (int)sizeof(cart_ram); i++)
        cart_ram[i] = 0;
    sg_ram_at = 0;
    if (sg && size <= 0xC000) {         /* the cartridges with RAM of their own: see the top */
        if (size <= 0x8000)
            sg_ram_at = 0x8000;
        else {
            u32 i = 0x2001;
            while (i < 0x4000 && image[i] == image[0x2000])
                i++;
            if (i == 0x4000) {
                sg_ram_at = 0x2000;
                for (int k = 0; k < 0x2000; k++)
                    cart_ram[k] = image[0x2000];    /* (what the ROM read there) */
            }
        }
    }
    char *p = put(info, gg ? "Game Gear" : sg == 1 ? "SG-1000" : sg ? "SC-3000" : "Master System");
    p = put(p, ", ROM ");
    p = put_number(p, size >> 10);
    p = put(p, " KB, ");
    if (sg && size <= 0xC000)
        p = put(p, sg_ram_at == 0x8000 ? "8 KB RAM at 0x8000" : sg_ram_at ? "8 KB RAM at 0x2000" : "no mapper");
    else
        p = put(p, codemasters ? "Codemasters mapper" : "Sega mapper");
    if (size >= 0x8000 && image[0x7FF0] == 'T' && image[0x7FF1] == 'M' && image[0x7FF2] == 'R')
        p = put(p, ", TMR SEGA header");
    sms_reset();
    return 0;
}

void sms_reset(void)
{
    for (int i = 0; i < 256; i++) {
        u32 a = 0, b = 0;
        for (int k = 0; k < 8; k++) {
            if (i & (0x80 >> k))
                a |= 1u << (k * 4);
            if (i & (1 << k))
                b |= 1u << (k * 4);
        }
        tile_x[i] = a, tile_xr[i] = b;
    }
    z80_init(&cpu);
    cpu.write = write;
    cpu.in = sg ? sg_in : in;
    cpu.out = sg ? sg_out : out;
    ram_mask = sg == 2 ? 0x7FF : sg ? 0x3FF : 0x1FFF;
    tms_colours();
    for (unsigned i = 0; i < sizeof(ram); i++)
        ram[i] = 0;
    for (unsigned i = 0; i < sizeof(vram); i++)
        vram[i] = 0;
    for (unsigned i = 0; i < sizeof(cram); i++)
        cram[i] = 0;
    for (int i = 0; i < 32; i++)
        pal[i] = 0;
    for (int i = 0; i < SMS_W * SMS_H_MAX; i++)
        screen[i] = 0;
    /* the registers as the console's start-up program leaves them */
    static const u8 start[11] = {0x36, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFB, 0x00, 0x00, 0x00, 0xFF};
    for (int i = 0; i < 16; i++)
        reg[i] = i < 11 && !sg ? start[i] : 0;          /* (the SG-1000 has no start-up program: all 0) */
    addr = 0, code = 0, latch = 0, first = 0, buffer = 0, gg_latch = 0;
    status = 0, line_pending = 0, counter = 0xFF, line = 0, hc_latched = 0;
    slot[0] = 0, slot[1] = 1, slot[2] = 2, ram_ctl = 0, ram_dirty = 0;
    pad1 = pad2 = pad_was = 0, io_ctl = 0xFF;
    mem_ctl = 0xAB;                     /* (as the console's start-up program leaves it, a copy at 0xC000) */
    if (sg)
        mem_ctl = 0;
    else
        ram[0] = mem_ctl;
    for (unsigned i = 0; i < sizeof(open_bus); i++)
        open_bus[i] = 0xFF;
    cycles_hi = 0;
    if (sg && rom_size <= 0xC000)
        sg_map();
    else
        map();
    cpu.sp.w = 0xDFF0;
    cpu.im = 1;
}

void sms_pad2(u8 pad)
{
    pad2 = pad;
}

void sms_frame(u8 pad, u16 *picture)
{
    pad1 = pad;
    if (!gg && (pad & SMS_START) && !(pad_was & SMS_START))
        cpu.nmi = 1;                    /* the Pause button */
    pad_was = pad;
    early_read = 0;
    int height = active_lines(), flag_line = sg ? height : height + 1;
    for (line = 0; line < LINES; line++) {
        if (line < height) {
            if (mode4())
                draw_line(line, height);
            else
                draw_tms(line);
        } else if (mode4())
            sprites(v_counter(), 0, height);
        z80_run(&cpu, line * LINE_T + FRAME_IRQ_T);
        if (line == flag_line && !early_read) {
            status |= 0x80;
            irq_update();
        }
        z80_run(&cpu, line * LINE_T + LINE_IRQ_T);
        if (line <= height) {
            if (--counter < 0) {
                counter = reg[10];
                line_pending = !sg;     /* (the SG-1000's chip has no line interrupt) */
            }
        } else {
            counter = reg[10];
        }
        irq_update();
        z80_run(&cpu, (line + 1) * LINE_T);
    }
    line = 0;
    cpu.cycles -= FRAME_T;
    cycles_hi += FRAME_T;
    if (!picture)
        return;
    if (gg) {
        int top = (height - GG_H) / 2;
        for (int y = 0; y < GG_H; y++)
            for (int x = 0; x < GG_W; x++)
                picture[y * GG_W + x] = screen[(top + y) * SMS_W + 48 + x];
    } else {
        for (int i = 0; i < SMS_W * height; i++)
            picture[i] = screen[i];
    }
}

int sms_width(void)
{
    return gg ? GG_W : SMS_W;
}

int sms_height(void)
{
    return gg ? GG_H : active_lines();
}

const char *sms_info(void)
{
    return info;
}

u8 *sms_save_ram(u32 *size, int *battery)
{
    *size = sg ? 0x2000 : sizeof(cart_ram);
    *battery = !sg;                     /* (an SG-1000 cartridge's RAM has no battery) */
    return cart_ram;
}

int sms_save_changed(void)
{
    int was = ram_dirty;
    ram_dirty = 0;
    return was;
}

u32 sms_cpu_cycles(void)
{
    return cycles_hi + (u32)cpu.cycles;
}

int sms_jammed(void)
{
    return cpu.halted && !cpu.iff1 && gg;
}

struct z80 *sms_cpu(void)
{
    return &cpu;
}
