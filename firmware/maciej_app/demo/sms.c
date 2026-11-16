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
   160 x 144.  The older TMS9918 modes (SG-1000 games) are not there: their
   registers are kept, and mode 4's picture is drawn whatever the mode.

   The cartridge: Sega's mapper (three 16 KB slots, the first 1 KB fixed;
   32 KB of cartridge RAM switched in at 0x8000), or Codemasters' (detected
   by its header's checksum).  Up to 4 MB of ROM.  The sound chip's ports
   take writes and do nothing (no sound). */
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
static u8 ram[0x2000], cart_ram[0x8000], junk[0x400], open_bus[0x400];
static u8 slot[3], ram_ctl;             /* the mapper's registers (0xFFFD-0xFFFF, 0xFFFC) */
static u8 mem_ctl;                      /* port 0x3E: bit 6 set, the cartridge slot is switched off */
static u8 ram_dirty;
static char info[96];

/* ---- the picture chip ---- */
static u8 vram[0x4000], cram[64], reg[16];
static u16 pal[32];                     /* cram as pixels */
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
    return ((reg[0] & 0x06) == 0x06 && (reg[1] & 0x18) == 0x10) ? 224 : 192;
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
        cpu.rd[48 + i] = ram + ((i & 7) << 10);
        cpu.wr[48 + i] = ram + ((i & 7) << 10);
    }
    if (!codemasters)
        cpu.wr[63] = 0;                 /* (0xFFFC-0xFFFF: the mapper's registers, and RAM) */
}

static void write(struct z80 *c, u16 a, u8 v)
{
    (void)c;
    if (a >= 0xC000) {
        ram[a & 0x1FFF] = v;
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
static u16 colour(int i)
{
    if (gg) {                           /* 12 bits: ----BBBB GGGGRRRR */
        u8 lo = cram[i * 2], hi = cram[i * 2 + 1];
        u32 r = lo & 15, g = lo >> 4, b = hi & 15;
        return (u16)((r << 1 | r >> 3) | (g << 1 | g >> 3) << 5 | (b << 1 | b >> 3) << 10);
    }
    static const u8 level[4] = {0, 10, 21, 31};     /* 6 bits: --BBGGRR */
    u8 v = cram[i];
    return (u16)(level[v & 3] | level[(v >> 2) & 3] << 5 | level[(v >> 4) & 3] << 10);
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
    case 0xC0:                          /* the controllers: 1 not pressed */
    case 0xC1: {
        u8 a = (u8)~(((pad1 & SMS_UP) ? 1 : 0) | ((pad1 & SMS_DOWN) ? 2 : 0) | ((pad1 & SMS_LEFT) ? 4 : 0) |
                     ((pad1 & SMS_RIGHT) ? 8 : 0) | ((pad1 & SMS_1) ? 0x10 : 0) | ((pad1 & SMS_2) ? 0x20 : 0) |
                     ((pad2 & SMS_UP) ? 0x40 : 0) | ((pad2 & SMS_DOWN) ? 0x80 : 0));
        if (!(p & 1))
            return a;
        u8 b = (u8)~(((pad2 & SMS_LEFT) ? 1 : 0) | ((pad2 & SMS_RIGHT) ? 2 : 0) | ((pad2 & SMS_1) ? 4 : 0) |
                     ((pad2 & SMS_2) ? 8 : 0));
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

const char *sms_load(u8 *image, u32 size, int game_gear)
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
    rom_banks = (size + 0x3FFF) >> 14;
    gg = game_gear ? 1 : 0;
    /* Codemasters' header: a checksum and its complement to 0x10000 at 0x7FE6 */
    codemasters = 0;
    if (size >= 0x8000) {
        u32 sum = (u32)(image[0x7FE6] | image[0x7FE7] << 8), inv = (u32)(image[0x7FE8] | image[0x7FE9] << 8);
        codemasters = sum + inv == 0x10000 && sum;
    }
    char *p = put(info, gg ? "Game Gear" : "Master System");
    p = put(p, ", ROM ");
    p = put_number(p, size >> 10);
    p = put(p, " KB, ");
    p = put(p, codemasters ? "Codemasters mapper" : "Sega mapper");
    if (size >= 0x8000 && image[0x7FF0] == 'T' && image[0x7FF1] == 'M' && image[0x7FF2] == 'R')
        p = put(p, ", TMR SEGA header");
    for (int i = 0; i < (int)sizeof(cart_ram); i++)
        cart_ram[i] = 0;
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
    cpu.in = in;
    cpu.out = out;
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
        reg[i] = i < 11 ? start[i] : 0;
    addr = 0, code = 0, latch = 0, first = 0, buffer = 0, gg_latch = 0;
    status = 0, line_pending = 0, counter = 0xFF, line = 0, hc_latched = 0;
    slot[0] = 0, slot[1] = 1, slot[2] = 2, ram_ctl = 0, ram_dirty = 0;
    pad1 = pad2 = pad_was = 0, io_ctl = 0xFF;
    mem_ctl = 0xAB;                     /* (as the console's start-up program leaves it, a copy at 0xC000) */
    ram[0] = mem_ctl;
    for (unsigned i = 0; i < sizeof(open_bus); i++)
        open_bus[i] = 0xFF;
    cycles_hi = 0;
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
    int height = active_lines();
    for (line = 0; line < LINES; line++) {
        if (line < height)
            draw_line(line, height);
        else
            sprites(v_counter(), 0, height);
        z80_run(&cpu, line * LINE_T + FRAME_IRQ_T);
        if (line == height + 1 && !early_read) {
            status |= 0x80;
            irq_update();
        }
        z80_run(&cpu, line * LINE_T + LINE_IRQ_T);
        if (line <= height) {
            if (--counter < 0) {
                counter = reg[10];
                line_pending = 1;
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
    *size = sizeof(cart_ram);
    *battery = 1;
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
