/* A Game Boy (DMG): see gb.h.

   The CPU (a Z80's cousin, Sharp's SM83) runs one machine cycle -- four
   ticks of the 4.194304 MHz clock -- per memory access, and everything else
   is moved on by that much before each access: the timer (a 16-bit counter;
   TIMA counts the falling edges of one of its bits), the link port, and the
   picture chip (456 ticks a line: 80 of looking through the sprites, 172 of
   drawing, the rest waiting; 144 lines of picture, 10 of blanking).  A line
   is drawn in one go when its drawing time ends, from the registers as they
   are then: background, window, up to ten sprites.

   Cartridges: plain ROM, MBC1, MBC2, MBC3 (its clock counts while the game
   runs and starts at zero), MBC5; RAM with a battery is the caller's to keep.
   There is no sound: its registers hold what was written, and every channel
   says it has finished.

   A cartridge made for the Game Boy Color (its header says so) runs as on
   one: the second bank of picture memory (tile numbers' attributes, more
   tiles), eight banks of work RAM, the colour palettes (eight for the
   background, eight for the sprites, 15 bits a colour), the copies into
   picture memory (all at once, or 16 bytes in each line's waiting time),
   the double speed (the CPU and the timer twice as fast, the picture chip
   not).  An older game runs as on the first Game Boy, in its four shades. */
#include "gb.h"

typedef signed char s8;

#define FZ 0x80
#define FN 0x40
#define FH 0x20
#define FC 0x10

/* I/O registers (0xFF00 + ...) */
#define P1   0x00
#define SB   0x01
#define SC   0x02
#define DIV  0x04
#define TIMA 0x05
#define TMA  0x06
#define TAC  0x07
#define IF   0x0F
#define LCDC 0x40
#define STAT 0x41
#define SCY  0x42
#define SCX  0x43
#define LY   0x44
#define LYC  0x45
#define DMA  0x46
#define BGP  0x47
#define OBP0 0x48
#define OBP1 0x49
#define WY   0x4A
#define WX   0x4B
/* the Game Boy Color's */
#define KEY1 0x4D
#define VBK  0x4F
#define HDMA5 0x55
#define RP   0x56
#define BCPS 0x68
#define BCPD 0x69
#define OCPS 0x6A
#define OCPD 0x6B
#define OPRI 0x6C
#define SVBK 0x70

const u8 gb_rgb[4][3] = {{0xE0, 0xF8, 0xD0}, {0x88, 0xC0, 0x70}, {0x34, 0x68, 0x56}, {0x08, 0x18, 0x20}};
void (*gb_serial_out)(u8 byte);

/* ---- the cartridge ---- */
enum { ROM_ONLY, MBC1, MBC2, MBC3, MBC5 };
static u8 *rom;
static u32 rom_banks;                   /* 16 KB each */
static u8 mbc, has_battery, has_ram, has_clock;
static u8 cart_ram[0x20000];
static u32 ram_size;
static u8 ram_on, bank_lo, bank_hi, mode, ram_bank, ram_dirty;
static u32 rom0_at, romx_at, ram_at;    /* where 0x0000, 0x4000 and 0xA000 are now */
static u8 rtc[5], rtc_latched[5], rtc_latch, rtc_reg;
static u32 rtc_cycles;
static char info[96];

/* ---- the machine ---- */
static u8 vram[0x4000], wram[0x8000], oam[0xA0], hram[0x80], io[0x80], ie;
static u8 cgb;                          /* a Game Boy Color game: the Color's mode */
static u8 vbank, wbank;                 /* picture memory's bank (0, 1); work RAM's at 0xD000 (1 to 7) */
static u8 dstep = 4;                    /* the picture chip's clock ticks a machine cycle: 4, 2 at double speed */
static u8 key1, rp, r72, r73, r74, r75;
static u8 bg_pal[64], ob_pal[64];       /* the Color's palettes, 2 bytes a colour */
static u16 bg_col[32], ob_col[32], dmg_col[4];  /* the same as pixels: 0bbbbbgggggrrrrr */
static u16 hdma_src, hdma_dst;          /* the copy's next bytes (they go on counting from one copy to the next) */
static u8 hdma_left, hdma_hblank, hdma_due;     /* blocks of 16 bytes still to copy; in each line's waiting time;
                                                   one block to copy before the next instruction */
static u8 halt_mode;                    /* the picture chip's mode when the CPU halted */
static u16 switch_left;                 /* machine cycles of a speed switch's wait still to come */
static u8 A, F, B, C, D, E, H, L;
static u16 SP, PC;
static u8 ime, ei_pending, halted, halt_bug, stopped, jammed, test_done;
static u32 cycles;
static u16 divc;                        /* the timer's counter; DIV is its high byte */
static u8 tima_state;                    /* 1: TIMA overflowed in the last cycle (it reads 0), 2: reloaded in this one */
static u8 dma_delay, dma_at, dma_block; /* the copy into the sprite table: cycles until it starts, its next byte */
static u16 dma_from;
static u16 serial_left;
static u8 pad_now;

static u16 dot;                         /* the picture chip's place in its line, 0..455 */
static u8 line, ppu_mode, mode_read, stat_line, win_trig, win_line, frame_done;
static u32 off_ticks;
static u16 screen[GB_W * GB_H];

static void map_banks(void)
{
    u32 lo = 0, x = 1, r = 0;
    switch (mbc) {
    case MBC1:
        x = (u32)(bank_hi << 5) | (bank_lo ? bank_lo : 1);
        if (mode) {
            lo = (u32)bank_hi << 5;
            r = bank_hi;
        }
        break;
    case MBC2:
        x = bank_lo ? bank_lo : 1;
        break;
    case MBC3:
        x = bank_lo ? bank_lo : 1;
        r = ram_bank;
        break;
    case MBC5:
        x = (u32)(bank_hi << 8) | bank_lo;
        r = ram_bank;
        break;
    }
    rom0_at = (lo % rom_banks) << 14;
    romx_at = (x % rom_banks) << 14;
    ram_at = ram_size > 0x2000 ? (r << 13) % ram_size : 0;
}

static void cart_write(u16 a, u8 v)
{
    switch (mbc) {
    case MBC1:
        if (a < 0x2000)
            ram_on = (v & 0x0F) == 0x0A;
        else if (a < 0x4000)
            bank_lo = v & 0x1F;
        else if (a < 0x6000)
            bank_hi = v & 3;
        else
            mode = v & 1;
        break;
    case MBC2:
        if (a < 0x4000) {
            if (a & 0x0100)
                bank_lo = v & 0x0F;
            else
                ram_on = (v & 0x0F) == 0x0A;
        }
        break;
    case MBC3:
        if (a < 0x2000) {
            ram_on = (v & 0x0F) == 0x0A;
        } else if (a < 0x4000) {
            bank_lo = v & 0x7F;
        } else if (a < 0x6000) {
            rtc_reg = v >= 0x08 && v <= 0x0C ? v : 0;
            if (!rtc_reg)
                ram_bank = v & 3;
        } else {
            if (!rtc_latch && (v & 1))
                for (int i = 0; i < 5; i++)
                    rtc_latched[i] = rtc[i];
            rtc_latch = v & 1;
        }
        break;
    case MBC5:
        if (a < 0x2000)
            ram_on = (v & 0x0F) == 0x0A;
        else if (a < 0x3000)
            bank_lo = v;
        else if (a < 0x4000)
            bank_hi = v & 1;
        else if (a < 0x6000)
            ram_bank = v & 0x0F;
        break;
    }
    map_banks();
}

static u8 ram_read(u16 a)
{
    if (!ram_on)
        return 0xFF;
    if (mbc == MBC3 && rtc_reg)
        return has_clock ? rtc_latched[rtc_reg - 8] : 0xFF;
    if (!ram_size)
        return 0xFF;
    if (mbc == MBC2)
        return (u8)(0xF0 | cart_ram[a & 0x1FF]);
    return cart_ram[ram_at + ((a & 0x1FFF) % ram_size)];
}

static void ram_write(u16 a, u8 v)
{
    if (!ram_on)
        return;
    if (mbc == MBC3 && rtc_reg) {
        if (has_clock) {
            static const u8 mask[5] = {0x3F, 0x3F, 0x1F, 0xFF, 0xC1};
            rtc[rtc_reg - 8] = v & mask[rtc_reg - 8];
            if (rtc_reg == 8)
                rtc_cycles = 0;
        }
        return;
    }
    if (!ram_size)
        return;
    u8 *p = mbc == MBC2 ? &cart_ram[a & 0x1FF] : &cart_ram[ram_at + ((a & 0x1FFF) % ram_size)];
    if (mbc == MBC2)
        v &= 0x0F;
    if (*p != v) {
        *p = v;
        ram_dirty = 1;
    }
}

static void rtc_second(void)
{
    if (rtc[4] & 0x40)
        return;                         /* stopped */
    if (++rtc[0] == 60) {
        rtc[0] = 0;
        if (++rtc[1] == 60) {
            rtc[1] = 0;
            if (++rtc[2] == 24) {
                rtc[2] = 0;
                if (++rtc[3] == 0) {
                    if (rtc[4] & 1)
                        rtc[4] |= 0x80; /* past 511 days */
                    rtc[4] ^= 1;
                }
            }
        }
    }
    rtc[0] &= 0x3F;
    rtc[1] &= 0x3F;
    rtc[2] &= 0x1F;
}

/* ---- the picture chip ---- */
static void stat_update(void)
{
    u8 s = io[STAT];
    u8 lyc = io[LY] == io[LYC];
    io[STAT] = (u8)((s & 0x78) | (lyc ? 4 : 0) | ppu_mode);
    u8 now = (u8)(((s & 0x08) && ppu_mode == 0) || ((s & 0x10) && ppu_mode == 1) ||
                  ((s & 0x20) && ppu_mode == 2) || ((s & 0x40) && lyc));
    if (!(io[LCDC] & 0x80))
        now = 0;
    if (now && !stat_line)
        io[IF] |= 2;
    stat_line = now;
}

/* `x` to `end` of a line from a row of a tile map (32 tile numbers at vram + map), starting `px` pixels into
   that row, the tiles' pixel row `r`: the colours into out, the colour numbers into num (| 0x80: the tile is
   in front of the sprites, the Color's attribute) */
static void tile_span(u16 *out, u8 *num, int x, int end, u16 map, u8 px, u8 r)
{
    u8 lcdc = io[LCDC], bgp = io[BGP], attr = 0, lo = 0, hi = 0;
    const u16 *pal = bg_col;
    for (int first = 1; x < end; x++, px++, first = 0) {
        if (first || !(px & 7)) {
            u16 at = (u16)(map + (px >> 3));
            u8 tile = vram[at];
            attr = cgb ? vram[0x2000 + at] : 0;
            u8 row = (attr & 0x40) ? (u8)(7 - r) : r;
            const u8 *t = vram + ((attr & 0x08) ? 0x2000 : 0) +
                          ((lcdc & 0x10) ? tile * 16 : 0x1000 + (s8)tile * 16) + row * 2;
            lo = t[0], hi = t[1];
            pal = bg_col + (attr & 7) * 4;
        }
        int bit = (attr & 0x20) ? (px & 7) : 7 - (px & 7);
        u8 ci = (u8)(((hi >> bit) & 1) << 1 | ((lo >> bit) & 1));
        num[x] = (u8)(ci | (attr & 0x80));
        out[x] = cgb ? pal[ci] : dmg_col[(bgp >> (ci * 2)) & 3];
    }
}

static void draw_line(int y)
{
    u8 lcdc = io[LCDC];
    u8 num[GB_W];
    u16 bg[GB_W];
    u16 *out = screen + y * GB_W;
    if (cgb || (lcdc & 0x01)) {         /* (on the Color bit 0 only puts the sprites in front: below) */
        u8 sy = (u8)(y + io[SCY]);
        int win = (lcdc & 0x20) && win_trig && io[WX] <= 166, wx = io[WX] - 7;
        int split = win ? (wx < 0 ? 0 : wx) : GB_W;
        tile_span(out, num, 0, split, (u16)(((lcdc & 0x08) ? 0x1C00 : 0x1800) + (sy >> 3) * 32), (u8)(io[SCX]),
                  sy & 7);
        if (win) {
            tile_span(out, num, split, GB_W, (u16)(((lcdc & 0x40) ? 0x1C00 : 0x1800) + (win_line >> 3) * 32),
                      (u8)(split - wx), win_line & 7);
            win_line++;
        }
    } else {
        for (int x = 0; x < GB_W; x++)
            num[x] = 0, out[x] = dmg_col[0];
    }
    if (!(lcdc & 0x02))
        return;
    /* the first ten sprites on this line, as they come; on the first Game Boy the one further left is in
       front, of two at the same place the earlier one; on the Color the earlier one */
    int h = (lcdc & 0x04) ? 16 : 8, n = 0;
    u8 list[10];
    for (int i = 0; i < 40 && n < 10; i++) {
        int top = oam[i * 4] - 16;
        if (y >= top && y < top + h)
            list[n++] = (u8)i;
    }
    if (!cgb || (io[OPRI] & 1))
        for (int i = 1; i < n; i++) {
            u8 s = list[i];
            int k = i;
            while (k > 0 && oam[list[k - 1] * 4 + 1] > oam[s * 4 + 1])
                list[k] = list[k - 1], k--;
            list[k] = s;
        }
    for (int x = 0; x < GB_W; x++)
        bg[x] = out[x];
    /* the Color with LCDC bit 0 clear: every sprite in front of the background */
    u8 behind_ok = !cgb || (lcdc & 0x01);
    for (int k = n - 1; k >= 0; k--) {
        const u8 *s = oam + list[k] * 4;
        int sx = s[1] - 8, row = y - (s[0] - 16);
        u8 attr = s[3], tile = s[2];
        if (attr & 0x40)
            row = h - 1 - row;
        if (h == 16)
            tile = (u8)((tile & 0xFE) | (row >> 3));
        const u8 *t = vram + ((cgb && (attr & 0x08)) ? 0x2000 : 0) + tile * 16 + (row & 7) * 2;
        const u16 *pal = ob_col + (attr & 7) * 4;
        u8 obp = io[(attr & 0x10) ? OBP1 : OBP0];
        for (int px = 0; px < 8; px++) {
            int x = sx + px, bit = (attr & 0x20) ? px : 7 - px;
            u8 ci = (u8)(((t[1] >> bit) & 1) << 1 | ((t[0] >> bit) & 1));
            if (!ci || x < 0 || x >= GB_W)
                continue;
            if (behind_ok && (num[x] & 3) && ((attr | num[x]) & 0x80))
                out[x] = bg[x];         /* behind the background's colours 1 to 3 */
            else
                out[x] = cgb ? pal[ci] : dmg_col[(obp >> (ci * 2)) & 3];
        }
    }
}

static void ppu_tick(void)
{
    mode_read = ppu_mode;               /* (a read of STAT sees the mode a cycle late; its interrupts do not wait) */
    if (!(io[LCDC] & 0x80)) {
        off_ticks += dstep;
        return;
    }
    dot += dstep;                       /* (">=": a speed switch can leave it at 2 more than a multiple of 4) */
    if (line < 144) {
        if (dot >= 80 && ppu_mode == 2) {
            ppu_mode = 3;
            stat_update();
        } else if (dot >= 252 && ppu_mode == 3) {
            draw_line(line);
            ppu_mode = 0;
            stat_update();
            if (hdma_hblank && !halted && !stopped)
                hdma_due = 1;           /* (16 bytes into picture memory: step(), before the next instruction) */
        } else if (dot >= 456) {
            dot -= 456;
            io[LY] = ++line;
            if (line == 144) {
                ppu_mode = 1;
                io[IF] |= 1;
                frame_done = 1;
                if (io[STAT] & 0x20) {  /* (the sprite search's interrupt comes with this line too) */
                    if (!stat_line)
                        io[IF] |= 2;
                }
            } else {
                ppu_mode = 2;
                if (line == io[WY])
                    win_trig = 1;
            }
            stat_update();
        }
    } else if (dot >= 456) {
        dot -= 456;
        if (++line == 154) {
            line = 0;
            ppu_mode = 2;
            win_line = 0;
            win_trig = io[WY] == 0;
        } else {
            io[LY] = line;
        }
        stat_update();
    } else if (line == 153 && io[LY] == 153) {
        io[LY] = 0;                     /* the last line calls itself line 0 from its second machine cycle */
        stat_update();
    }
}

static void lcd_switch(u8 was, u8 now)
{
    if ((was ^ now) & 0x80) {
        dot = 0;
        line = 0;
        io[LY] = 0;
        win_line = 0;
        win_trig = 0;
        ppu_mode = (now & 0x80) ? 2 : 0;
        if (!(now & 0x80)) {
            if (hdma_hblank && ppu_mode)
                hdma_due = 1;
            u16 blank = cgb ? 0x7FFF : dmg_col[0];          /* (a switched-off screen is its lightest) */
            for (unsigned i = 0; i < GB_W * GB_H; i++)
                screen[i] = blank;
        } else {
            off_ticks = 0;
            win_trig = io[WY] == 0;
        }
    }
}

/* ---- timer, link port ---- */
static u16 timer_bit(void)
{
    static const u16 bits[4] = {0x0200, 0x0008, 0x0020, 0x0080};
    return (io[TAC] & 4) ? bits[io[TAC] & 3] : 0;
}

static void tima_step(void)
{
    if (++io[TIMA] == 0)
        tima_state = 1;                 /* (reads 0 for a machine cycle, then TMA and the interrupt) */
}

static u8 peek(u16 a);
static void hdma_run(void);

static void tick(void)
{
    cycles++;
    if (tima_state == 1) {
        tima_state = 2;
        io[TIMA] = io[TMA];
        io[IF] |= 4;
    } else {
        tima_state = 0;
    }
    /* a write to DMA: a cycle later 160 bytes go into the sprite table, one a cycle; the program sees 0xFF
       there meanwhile (one started over the last keeps that one running until its own first byte) */
    if (dma_delay && !--dma_delay) {
        dma_from = (u16)(io[DMA] << 8);
        dma_at = 0;
    }
    if (dma_at < 0xA0) {
        u16 from = (u16)(dma_from + dma_at);
        dma_block = 0;
        oam[dma_at++] = from >= 0xE000 ? peek((u16)(from - 0x2000)) : peek(from);
        dma_block = 1;
    } else {
        dma_block = 0;
    }
    u16 bit = timer_bit(), was = divc;
    divc += 4;
    if ((was & bit) && !(divc & bit))
        tima_step();
    if (serial_left && !--serial_left) {
        if (gb_serial_out)
            gb_serial_out(io[SB]);
        io[SB] = 0xFF;                  /* nobody at the other end */
        io[SC] &= 0x7F;
        io[IF] |= 8;
    }
    if (has_clock && (rtc_cycles += dstep) >= 4194304) {     /* (clock ticks: a second at either speed) */
        rtc_cycles = 0;
        rtc_second();
    }
    ppu_tick();
}

/* ---- memory ---- */
static u8 pad_read(void)
{
    u8 v = (u8)(io[P1] & 0x30) | 0xC0, low = 0x0F;
    if (!(v & 0x20))
        low &= (u8)~(pad_now & 0x0F);                           /* A, B, Select, Start */
    if (!(v & 0x10))
        low &= (u8)~(((pad_now & GB_RIGHT) ? 1 : 0) | ((pad_now & GB_LEFT) ? 2 : 0) |
                     ((pad_now & GB_UP) ? 4 : 0) | ((pad_now & GB_DOWN) ? 8 : 0));
    return v | low;
}

static u8 io_read(u8 r)
{
    static const u8 sound_or[0x20] = {0x80, 0x3F, 0x00, 0xFF, 0xBF, 0xFF, 0x3F, 0x00, 0xFF, 0xBF, 0x7F, 0xFF, 0x9F, 0xFF,
                                      0xBF, 0xFF, 0xFF, 0x00, 0x00, 0xBF, 0x00, 0x00, 0x70, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                      0xFF, 0xFF, 0xFF, 0xFF};
    switch (r) {
    case P1: return pad_read();
    case SB: return io[SB];
    case SC: return io[SC] | 0x7E;
    case DIV: return (u8)(divc >> 8);
    case TIMA: return io[TIMA];
    case TMA: return io[TMA];
    case TAC: return io[TAC] | 0xF8;
    case IF: return io[IF] | 0xE0;
    case STAT: return (u8)((io[STAT] & 0x7C) | mode_read | 0x80);
    }
    if (r >= 0x10 && r < 0x30)
        return io[r] | sound_or[r - 0x10];
    if (r >= 0x30 && r < 0x40)
        return io[r];
    if (r >= 0x40 && r <= 0x4B)
        return io[r];
    if (cgb)
        switch (r) {
        case SC: return io[SC] | 0x7C;
        case KEY1: return (u8)(0x7E | (dstep == 2 ? 0x80 : 0) | key1);
        case VBK: return 0xFE | vbank;
        case HDMA5: return (u8)((hdma_hblank ? 0 : 0x80) | ((hdma_left - 1) & 0x7F));
        case RP: return (u8)(rp | 0x3E);                        /* (no infrared light seen) */
        case BCPS: return io[BCPS] | 0x40;
        case BCPD: return bg_pal[io[BCPS] & 0x3F];
        case OCPS: return io[OCPS] | 0x40;
        case OCPD: return ob_pal[io[OCPS] & 0x3F];
        case OPRI: return io[OPRI] | 0xFE;
        case SVBK: return io[SVBK] | 0xF8;
        case 0x72: return r72;
        case 0x73: return r73;
        case 0x74: return r74;
        case 0x75: return r75 | 0x8F;
        case 0x76: case 0x77: return 0x00;                      /* (the sound channels' outputs: silent) */
        }
    return 0xFF;
}

/* a palette byte written: `which` 0 the background's, 1 the sprites' */
static void palette_write(int which, u8 v)
{
    u8 *sel = &io[which ? OCPS : BCPS], i = *sel & 0x3F;
    u8 *pal = which ? ob_pal : bg_pal;
    pal[i] = v;
    (which ? ob_col : bg_col)[i >> 1] = (u16)((pal[i | 1] << 8 | pal[i & 0x3E]) & 0x7FFF);
    if (*sel & 0x80)
        *sel = (u8)(0x80 | ((i + 1) & 0x3F));
}

static void io_write(u8 r, u8 v)
{
    switch (r) {
    case P1:
        io[P1] = v & 0x30;
        return;
    case SC:
        io[SC] = cgb ? v : (v & 0x81);
        if ((v & 0x81) == 0x81)
            serial_left = (cgb && (v & 2)) ? 32 : 1024;         /* eight bits at 8192 a second (the Color's fast
                                                                   one: 262144) */
        return;
    case DIV: {
        u16 bit = timer_bit();
        if (divc & bit)
            tima_step();
        divc = 0;
        return;
    }
    case TIMA:
        if (tima_state == 2)
            return;                     /* (the cycle TMA goes in: the write is lost) */
        tima_state = 0;                 /* (the cycle after the overflow: no reload, no interrupt) */
        io[TIMA] = v;
        return;
    case TMA:
        io[TMA] = v;
        if (tima_state == 2)
            io[TIMA] = v;
        return;
    case TAC: {
        u16 was = (u16)(divc & timer_bit());
        io[TAC] = v & 7;
        if (was && !(divc & timer_bit()))
            tima_step();
        return;
    }
    case IF:
        io[IF] = v & 0x1F;
        return;
    case 0x26:                          /* sound on/off: off clears its registers; no channel ever plays */
        io[0x26] = v & 0x80;
        if (!(v & 0x80))
            for (int i = 0x10; i < 0x26; i++)
                io[i] = 0;
        return;
    case LCDC: {
        u8 was = io[LCDC];
        io[LCDC] = v;
        lcd_switch(was, v);
        stat_update();
        return;
    }
    case STAT:
        io[STAT] = (u8)((io[STAT] & 0x07) | (v & 0x78));
        stat_update();
        return;
    case LY:
        return;
    case LYC:
        io[LYC] = v;
        stat_update();
        return;
    case DMA:
        io[DMA] = v;
        dma_delay = 2;
        return;
    }
    if (cgb)
        switch (r) {
        case KEY1: key1 = v & 1; return;
        case VBK: vbank = v & 1; return;
        case 0x51: hdma_src = (u16)((hdma_src & 0x00F0) | v << 8); return;
        case 0x52: hdma_src = (u16)((hdma_src & 0xFF00) | (v & 0xF0)); return;
        case 0x53: hdma_dst = (u16)((hdma_dst & 0x00F0) | v << 8); return;
        case 0x54: hdma_dst = (u16)((hdma_dst & 0xFF00) | (v & 0xF0)); return;
        case HDMA5:
            hdma_left = (u8)((v & 0x7F) + 1);
            if (hdma_hblank && !(v & 0x80)) {
                hdma_hblank = 0;        /* stopped (HDMA5 then reads 0x80 | the new length - 1) */
                return;
            }
            if (v & 0x80) {
                hdma_hblank = 1;        /* 16 bytes in each line's waiting time: at once when it is that now (or
                                           the picture is off) */
                if (!ppu_mode)
                    hdma_run();
            } else {
                hdma_run();             /* all of it now, the CPU waiting */
            }
            return;
        case RP: rp = v & 0xC1; return;
        case BCPS: io[BCPS] = v & 0xBF; return;
        case BCPD: palette_write(0, v); return;
        case OCPS: io[OCPS] = v & 0xBF; return;
        case OCPD: palette_write(1, v); return;
        case OPRI: io[OPRI] = v & 1; return;
        case SVBK:
            io[SVBK] = v & 7;
            wbank = (v & 7) ? (v & 7) : 1;
            return;
        case 0x72: r72 = v; return;
        case 0x73: r73 = v; return;
        case 0x74: r74 = v; return;
        case 0x75: r75 = v & 0x70; return;
        }
    if (r < 0x80)
        io[r] = v;
}

/* into picture memory (its bank now) 16 bytes at a time, 8 machine cycles each at normal speed, 16 at double:
   one block of an HBlank copy, all of a general one; it ends early when its address passes 0xFFFF */
static void hdma_run(void)
{
    do {
        for (int i = 0; i < 16; i++) {
            u16 from = hdma_src++;
            u8 b = from < 0x8000 || (from >= 0xA000 && from < 0xE000) ? peek(from) : 0xFF;
            vram[(vbank << 13) | (hdma_dst++ & 0x1FFF)] = b;
            if (dstep == 2 || (i & 1))
                tick();
        }
        if (--hdma_left == 0 || hdma_dst == 0) {
            hdma_hblank = 0;
            break;
        }
    } while (!hdma_hblank);
}

/* work RAM: 0xC000 its first 4 KB, 0xD000 the bank SVBK chose (the first Game Boy: always 1); 0xE000 the same */
#define WRAM(a) (((a) & 0x1000) ? (u32)(wbank << 12) | ((a) & 0x0FFF) : ((a) & 0x0FFFu))

static u8 peek(u16 a)
{
    switch (a >> 12) {
    case 0: case 1: case 2: case 3:
        return rom[rom0_at + a];
    case 4: case 5: case 6: case 7:
        return rom[romx_at + (a & 0x3FFF)];
    case 8: case 9:
        return vram[(vbank << 13) | (a & 0x1FFF)];
    case 0xA: case 0xB:
        return ram_read(a);
    case 0xC: case 0xD: case 0xE:
        return wram[WRAM(a)];
    }
    if (a < 0xFE00)
        return wram[WRAM(a)];
    if (a < 0xFEA0)
        return dma_block ? 0xFF : oam[a - 0xFE00];
    if (a < 0xFF00)
        return 0x00;
    if (a < 0xFF80)
        return io_read((u8)a);
    if (a < 0xFFFF)
        return hram[a - 0xFF80];
    return ie;
}

static void poke(u16 a, u8 v)
{
    switch (a >> 12) {
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
        cart_write(a, v);
        return;
    case 8: case 9:
        vram[(vbank << 13) | (a & 0x1FFF)] = v;
        return;
    case 0xA: case 0xB:
        ram_write(a, v);
        return;
    case 0xC: case 0xD: case 0xE:
        wram[WRAM(a)] = v;
        return;
    }
    if (a < 0xFE00)
        wram[WRAM(a)] = v;
    else if (a < 0xFEA0) {
        if (!dma_block)
            oam[a - 0xFE00] = v;
    } else if (a < 0xFF00)
        ;
    else if (a < 0xFF80)
        io_write((u8)a, v);
    else if (a < 0xFFFF)
        hram[a - 0xFF80] = v;
    else
        ie = v;
}

static u8 rd(u16 a)
{
    tick();
    return peek(a);
}

static void wr(u16 a, u8 v)
{
    tick();
    poke(a, v);
}

/* ---- the CPU ---- */
#define HL16 ((u16)(H << 8 | L))
#define BC16 ((u16)(B << 8 | C))
#define DE16 ((u16)(D << 8 | E))

static u8 imm(void)
{
    return rd(PC++);
}

static u16 imm16(void)
{
    u8 lo = imm();
    return (u16)(imm() << 8 | lo);
}

static void push(u16 v)
{
    wr(--SP, (u8)(v >> 8));
    wr(--SP, (u8)v);
}

static u16 pop(void)
{
    u8 lo = rd(SP++);
    return (u16)(rd(SP++) << 8 | lo);
}

static u8 reg_get(int i)
{
    switch (i) {
    case 0: return B;
    case 1: return C;
    case 2: return D;
    case 3: return E;
    case 4: return H;
    case 5: return L;
    case 6: return rd(HL16);
    }
    return A;
}

static void reg_set(int i, u8 v)
{
    switch (i) {
    case 0: B = v; break;
    case 1: C = v; break;
    case 2: D = v; break;
    case 3: E = v; break;
    case 4: H = v; break;
    case 5: L = v; break;
    case 6: wr(HL16, v); break;
    default: A = v; break;
    }
}

static void alu(int op, u8 v)
{
    u8 c = (F & FC) ? 1 : 0;
    u16 r;
    switch (op) {
    case 0:                             /* ADD */
    case 1:                             /* ADC */
        if (op == 0)
            c = 0;
        r = (u16)(A + v + c);
        F = (u8)(((r & 0xFF) ? 0 : FZ) | (((A & 15) + (v & 15) + c > 15) ? FH : 0) | (r > 0xFF ? FC : 0));
        A = (u8)r;
        break;
    case 2:                             /* SUB */
    case 3:                             /* SBC */
    case 7:                             /* CP */
        if (op != 3)
            c = 0;
        r = (u16)(A - v - c);
        F = (u8)(FN | ((r & 0xFF) ? 0 : FZ) | (((A & 15) < (v & 15) + c) ? FH : 0) | (r > 0xFF ? FC : 0));
        if (op != 7)
            A = (u8)r;
        break;
    case 4:                             /* AND */
        A &= v;
        F = (u8)((A ? 0 : FZ) | FH);
        break;
    case 5:                             /* XOR */
        A ^= v;
        F = A ? 0 : FZ;
        break;
    default:                            /* OR */
        A |= v;
        F = A ? 0 : FZ;
        break;
    }
}

static u8 inc8(u8 v)
{
    v++;
    F = (u8)((F & FC) | (v ? 0 : FZ) | ((v & 15) ? 0 : FH));
    return v;
}

static u8 dec8(u8 v)
{
    v--;
    F = (u8)((F & FC) | FN | (v ? 0 : FZ) | ((v & 15) == 15 ? FH : 0));
    return v;
}

static void add_hl(u16 v)
{
    u32 hl = HL16, r = hl + v;
    F = (u8)((F & FZ) | (((hl & 0x0FFF) + (v & 0x0FFF) > 0x0FFF) ? FH : 0) | (r > 0xFFFF ? FC : 0));
    H = (u8)(r >> 8);
    L = (u8)r;
    tick();
}

static u16 sp_plus(s8 e)
{
    F = (u8)((((SP & 15) + (e & 15) > 15) ? FH : 0) | (((SP & 0xFF) + (u8)e > 0xFF) ? FC : 0));
    return (u16)(SP + e);
}

static int cond(int i)
{
    switch (i) {
    case 0: return !(F & FZ);
    case 1: return F & FZ;
    case 2: return !(F & FC);
    }
    return F & FC;
}

static void cb_op(void)
{
    u8 op = imm();
    int r = op & 7, n = (op >> 3) & 7;
    u8 v = reg_get(r), c = (F & FC) ? 1 : 0;
    switch (op >> 6) {
    case 0:
        switch (n) {
        case 0: F = (v & 0x80) ? FC : 0; v = (u8)(v << 1 | v >> 7); break;                  /* RLC */
        case 1: F = (v & 1) ? FC : 0; v = (u8)(v >> 1 | v << 7); break;                     /* RRC */
        case 2: F = (v & 0x80) ? FC : 0; v = (u8)(v << 1 | c); break;                       /* RL */
        case 3: F = (v & 1) ? FC : 0; v = (u8)(v >> 1 | c << 7); break;                     /* RR */
        case 4: F = (v & 0x80) ? FC : 0; v = (u8)(v << 1); break;                           /* SLA */
        case 5: F = (v & 1) ? FC : 0; v = (u8)((v >> 1) | (v & 0x80)); break;               /* SRA */
        case 6: F = 0; v = (u8)(v << 4 | v >> 4); break;                                    /* SWAP */
        default: F = (v & 1) ? FC : 0; v >>= 1; break;                                      /* SRL */
        }
        if (!v)
            F |= FZ;
        reg_set(r, v);
        break;
    case 1:                             /* BIT */
        F = (u8)((F & FC) | FH | ((v >> n) & 1 ? 0 : FZ));
        break;
    case 2:                             /* RES */
        reg_set(r, (u8)(v & ~(1u << n)));
        break;
    default:                            /* SET */
        reg_set(r, (u8)(v | (1u << n)));
        break;
    }
}

static void step(void)
{
    if (hdma_due) {
        hdma_due = 0;
        if (hdma_hblank)
            hdma_run();
    }
    u8 pending = ie & io[IF] & 0x1F;
    if (stopped) {
        if (pad_now)
            stopped = 0;
        else {
            tick();
            return;
        }
    }
    if (halted) {
        if (!pending) {
            tick();
            if (switch_left && !--switch_left)
                halted = 0;             /* (the speed switch's wait is over) */
            return;
        }
        halted = 0;
        switch_left = 0;
        if (hdma_hblank && !ppu_mode && halt_mode && (io[LCDC] & 0x80))
            hdma_run();                 /* (woken in a line's waiting time that began while it slept) */
    }
    if (ime && pending) {
        int n = 0;
        while (!(pending & (1u << n)))
            n++;
        ime = 0;
        tick();
        tick();
        wr(--SP, (u8)(PC >> 8));
        pending = ie & io[IF] & 0x1F;   /* (the push may have written the interrupt enables: looked at again) */
        wr(--SP, (u8)PC);
        if (pending) {
            n = 0;
            while (!(pending & (1u << n)))
                n++;
            io[IF] &= (u8)~(1u << n);
            PC = (u16)(0x40 + n * 8);
        } else {
            PC = 0;                     /* none left: the jump goes to 0 */
        }
        tick();
        return;
    }
    if (ei_pending) {
        ei_pending = 0;
        ime = 1;
    }
    u8 op = rd(PC);
    if (halt_bug)
        halt_bug = 0;                   /* (the byte after HALT is read twice) */
    else
        PC++;
    if (op >= 0x40 && op < 0x80) {
        if (op == 0x76) {
            if (ime && (ie & io[IF] & 0x1F))
                PC--;                   /* (EI just before it, an interrupt waiting: taken now, returning to the
                                           HALT, which then runs again) */
            else if (ie & io[IF] & 0x1F)
                halt_bug = 1;
            else
                halted = 1, halt_mode = ppu_mode;
            return;
        }
        if (op == 0x40)
            test_done = 1;
        reg_set((op >> 3) & 7, reg_get(op & 7));
        return;
    }
    if (op >= 0x80 && op < 0xC0) {
        alu((op >> 3) & 7, reg_get(op & 7));
        return;
    }
    u16 t;
    u8 v;
    switch (op) {
    case 0x00: break;
    case 0x10:                          /* STOP: until a button is pressed */
        if (cgb && (key1 & 1)) {        /* the Color: its other speed instead -- the timer from 0, then the CPU
                                           halted for 0x20000 clock ticks at the new speed (an interrupt ends that
                                           sooner), so its 16-bit counter comes round to 0 */
            int wait = !(ie & io[IF] & 0x1F);
            if (wait) {
                tick();                 /* (the byte after it read and skipped) */
                PC++;
            }
            key1 = 0;
            dstep = dstep == 4 ? 2 : 4;
            io_write(DIV, 0);           /* (as a write to DIV: TIMA may see the edge) */
            if (wait) {
                halted = 1, halt_mode = ppu_mode;
                switch_left = 0x20000 / 4;
            }
            break;
        }
        PC++;
        stopped = 1;
        break;
    case 0x01: C = imm(); B = imm(); break;
    case 0x11: E = imm(); D = imm(); break;
    case 0x21: L = imm(); H = imm(); break;
    case 0x31: SP = imm16(); break;
    case 0x02: wr(BC16, A); break;
    case 0x12: wr(DE16, A); break;
    case 0x22: wr(HL16, A); t = (u16)(HL16 + 1); H = (u8)(t >> 8); L = (u8)t; break;
    case 0x32: wr(HL16, A); t = (u16)(HL16 - 1); H = (u8)(t >> 8); L = (u8)t; break;
    case 0x0A: A = rd(BC16); break;
    case 0x1A: A = rd(DE16); break;
    case 0x2A: A = rd(HL16); t = (u16)(HL16 + 1); H = (u8)(t >> 8); L = (u8)t; break;
    case 0x3A: A = rd(HL16); t = (u16)(HL16 - 1); H = (u8)(t >> 8); L = (u8)t; break;
    case 0x03: t = (u16)(BC16 + 1); B = (u8)(t >> 8); C = (u8)t; tick(); break;
    case 0x13: t = (u16)(DE16 + 1); D = (u8)(t >> 8); E = (u8)t; tick(); break;
    case 0x23: t = (u16)(HL16 + 1); H = (u8)(t >> 8); L = (u8)t; tick(); break;
    case 0x33: SP++; tick(); break;
    case 0x0B: t = (u16)(BC16 - 1); B = (u8)(t >> 8); C = (u8)t; tick(); break;
    case 0x1B: t = (u16)(DE16 - 1); D = (u8)(t >> 8); E = (u8)t; tick(); break;
    case 0x2B: t = (u16)(HL16 - 1); H = (u8)(t >> 8); L = (u8)t; tick(); break;
    case 0x3B: SP--; tick(); break;
    case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C:
        reg_set(op >> 3, inc8(reg_get(op >> 3)));
        break;
    case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D:
        reg_set(op >> 3, dec8(reg_get(op >> 3)));
        break;
    case 0x06: case 0x0E: case 0x16: case 0x1E: case 0x26: case 0x2E: case 0x36: case 0x3E:
        v = imm();
        reg_set(op >> 3, v);
        break;
    case 0x07: F = (A & 0x80) ? FC : 0; A = (u8)(A << 1 | A >> 7); break;                               /* RLCA */
    case 0x0F: F = (A & 1) ? FC : 0; A = (u8)(A >> 1 | A << 7); break;                                  /* RRCA */
    case 0x17: v = (F & FC) ? 1 : 0; F = (A & 0x80) ? FC : 0; A = (u8)(A << 1 | v); break;              /* RLA */
    case 0x1F: v = (F & FC) ? 0x80 : 0; F = (A & 1) ? FC : 0; A = (u8)(A >> 1 | v); break;              /* RRA */
    case 0x08:
        t = imm16();
        wr(t, (u8)SP);
        wr((u16)(t + 1), (u8)(SP >> 8));
        break;
    case 0x09: add_hl(BC16); break;
    case 0x19: add_hl(DE16); break;
    case 0x29: add_hl(HL16); break;
    case 0x39: add_hl(SP); break;
    case 0x18:
        v = imm();
        PC = (u16)(PC + (s8)v);
        tick();
        break;
    case 0x20: case 0x28: case 0x30: case 0x38:
        v = imm();
        if (cond((op >> 3) & 3)) {
            PC = (u16)(PC + (s8)v);
            tick();
        }
        break;
    case 0x27: {                        /* DAA */
        u8 adj = 0, carry = 0;
        if (F & FN) {
            if (F & FH)
                adj |= 0x06;
            if (F & FC)
                adj |= 0x60, carry = 1;
            A = (u8)(A - adj);
        } else {
            if ((F & FH) || (A & 0x0F) > 9)
                adj |= 0x06;
            if ((F & FC) || A > 0x99)
                adj |= 0x60, carry = 1;
            A = (u8)(A + adj);
        }
        F = (u8)((F & FN) | (A ? 0 : FZ) | (carry ? FC : 0));
        break;
    }
    case 0x2F: A = (u8)~A; F |= FN | FH; break;
    case 0x37: F = (u8)((F & FZ) | FC); break;
    case 0x3F: F = (u8)((F & FZ) | ((F & FC) ? 0 : FC)); break;
    case 0xC0: case 0xC8: case 0xD0: case 0xD8:
        tick();
        if (cond((op >> 3) & 3)) {
            PC = pop();
            tick();
        }
        break;
    case 0xC9: PC = pop(); tick(); break;
    case 0xD9: PC = pop(); tick(); ime = 1; break;
    case 0xC1: C = rd(SP++); B = rd(SP++); break;
    case 0xD1: E = rd(SP++); D = rd(SP++); break;
    case 0xE1: L = rd(SP++); H = rd(SP++); break;
    case 0xF1: F = rd(SP++) & 0xF0; A = rd(SP++); break;
    case 0xC5: tick(); push(BC16); break;
    case 0xD5: tick(); push(DE16); break;
    case 0xE5: tick(); push(HL16); break;
    case 0xF5: tick(); push((u16)(A << 8 | F)); break;
    case 0xC2: case 0xCA: case 0xD2: case 0xDA:
        t = imm16();
        if (cond((op >> 3) & 3)) {
            PC = t;
            tick();
        }
        break;
    case 0xC3: PC = imm16(); tick(); break;
    case 0xE9: PC = HL16; break;
    case 0xC4: case 0xCC: case 0xD4: case 0xDC:
        t = imm16();
        if (cond((op >> 3) & 3)) {
            tick();
            push(PC);
            PC = t;
        }
        break;
    case 0xCD:
        t = imm16();
        tick();
        push(PC);
        PC = t;
        break;
    case 0xC7: case 0xCF: case 0xD7: case 0xDF: case 0xE7: case 0xEF: case 0xF7: case 0xFF:
        tick();
        push(PC);
        PC = op & 0x38;
        break;
    case 0xC6: case 0xCE: case 0xD6: case 0xDE: case 0xE6: case 0xEE: case 0xF6: case 0xFE:
        alu((op >> 3) & 7, imm());
        break;
    case 0xCB: cb_op(); break;
    case 0xE0: v = imm(); wr((u16)(0xFF00 | v), A); break;
    case 0xF0: v = imm(); A = rd((u16)(0xFF00 | v)); break;
    case 0xE2: wr((u16)(0xFF00 | C), A); break;
    case 0xF2: A = rd((u16)(0xFF00 | C)); break;
    case 0xEA: t = imm16(); wr(t, A); break;
    case 0xFA: t = imm16(); A = rd(t); break;
    case 0xE8:
        v = imm();
        SP = sp_plus((s8)v);
        tick();
        tick();
        break;
    case 0xF8:
        v = imm();
        t = sp_plus((s8)v);
        H = (u8)(t >> 8);
        L = (u8)t;
        tick();
        break;
    case 0xF9: SP = HL16; tick(); break;
    case 0xF3: ime = 0; ei_pending = 0; break;
    case 0xFB: ei_pending = 1; break;
    default:                            /* D3 DB DD E3 E4 EB EC ED F4 FC FD: the CPU stops for good */
        jammed = 1;
        PC--;
        break;
    }
}

/* ---- the outside ---- */
static void put(char **p, const char *s)
{
    while (*s && *p < info + sizeof(info) - 1)
        *(*p)++ = *s++;
    **p = 0;
}

static void put_kb(char **p, u32 bytes)
{
    char digits[12];
    int n = 0;
    u32 v = bytes >= 1024 ? bytes >> 10 : bytes;
    do
        digits[n++] = (char)('0' + v % 10);
    while (v /= 10);
    while (n && *p < info + sizeof(info) - 1)
        *(*p)++ = digits[--n];
    **p = 0;
    put(p, bytes >= 1024 ? " KB" : " bytes");
}

const char *gb_load(u8 *image, u32 size)
{
    static const u32 ram_sizes[6] = {0, 0x800, 0x2000, 0x8000, 0x20000, 0x10000};
    if (size < 0x8000 || (size & 0x3FFF))
        return "not a Game Boy cartridge file (its size is not a whole number of 16 KB banks, two at least)";
    u8 sum = 0;
    for (int i = 0x134; i <= 0x14C; i++)
        sum = (u8)(sum - image[i] - 1);
    if (sum != image[0x14D])
        return "not a Game Boy cartridge file (its header's checksum is wrong)";
    u8 type = image[0x147];
    const char *name;
    has_battery = has_ram = has_clock = 0;
    switch (type) {
    case 0x00: mbc = ROM_ONLY, name = "ROM only"; break;
    case 0x08: mbc = ROM_ONLY, has_ram = 1, name = "ROM and RAM"; break;
    case 0x09: mbc = ROM_ONLY, has_ram = has_battery = 1, name = "ROM and RAM"; break;
    case 0x01: mbc = MBC1, name = "MBC1"; break;
    case 0x02: mbc = MBC1, has_ram = 1, name = "MBC1"; break;
    case 0x03: mbc = MBC1, has_ram = has_battery = 1, name = "MBC1"; break;
    case 0x05: mbc = MBC2, has_ram = 1, name = "MBC2"; break;
    case 0x06: mbc = MBC2, has_ram = has_battery = 1, name = "MBC2"; break;
    case 0x0F: mbc = MBC3, has_clock = has_battery = 1, name = "MBC3"; break;
    case 0x10: mbc = MBC3, has_ram = has_clock = has_battery = 1, name = "MBC3"; break;
    case 0x11: mbc = MBC3, name = "MBC3"; break;
    case 0x12: mbc = MBC3, has_ram = 1, name = "MBC3"; break;
    case 0x13: mbc = MBC3, has_ram = has_battery = 1, name = "MBC3"; break;
    case 0x19: case 0x1C: mbc = MBC5, name = "MBC5"; break;
    case 0x1A: case 0x1D: mbc = MBC5, has_ram = 1, name = "MBC5"; break;
    case 0x1B: case 0x1E: mbc = MBC5, has_ram = has_battery = 1, name = "MBC5"; break;
    default:
        return "a cartridge board this player does not have (it has plain ROM, MBC1, MBC2, MBC3 and MBC5)";
    }
    rom = image;
    rom_banks = size >> 14;
    ram_size = 0;
    if (mbc == MBC2)
        ram_size = 0x200;
    else if (has_ram && image[0x149] < 6)
        ram_size = ram_sizes[image[0x149]];
    if (!ram_size)
        has_ram = 0;
    if (!has_ram && !has_clock)
        has_battery = 0;
    for (unsigned i = 0; i < sizeof(cart_ram); i++)
        cart_ram[i] = 0xFF;
    char *p = info;
    char title[17];
    int n = 0;
    for (int i = 0x134; i < 0x144 && image[i] >= 0x20 && image[i] < 0x7F; i++)
        title[n++] = (char)image[i];
    while (n && title[n - 1] == ' ')
        n--;
    title[n] = 0;
    if (n) {
        put(&p, title);
        put(&p, ": ");
    }
    put(&p, name);
    put(&p, ", ROM ");
    put_kb(&p, size);
    if (ram_size) {
        put(&p, ", RAM ");
        put_kb(&p, ram_size);
    }
    if (has_clock)
        put(&p, ", clock");
    if (has_battery)
        put(&p, ", battery");
    cgb = (image[0x143] & 0x80) != 0;
    if (cgb)
        put(&p, image[0x143] == 0xC0 ? ", Game Boy Color only" : ", Game Boy Color");
    gb_reset();
    return 0;
}

void gb_reset(void)
{
    for (unsigned i = 0; i < sizeof(vram); i++)
        vram[i] = 0;
    for (unsigned i = 0; i < sizeof(wram); i++)
        wram[i] = 0;
    for (unsigned i = 0; i < sizeof(oam); i++)
        oam[i] = 0;
    for (unsigned i = 0; i < sizeof(hram); i++)
        hram[i] = 0;
    for (unsigned i = 0; i < sizeof(io); i++)
        io[i] = 0;
    for (int i = 0; i < 4; i++)
        dmg_col[i] = (u16)(gb_rgb[i][0] >> 3 | (gb_rgb[i][1] >> 3) << 5 | (gb_rgb[i][2] >> 3) << 10);
    for (unsigned i = 0; i < GB_W * GB_H; i++)
        screen[i] = cgb ? 0x7FFF : dmg_col[0];
    for (int i = 0; i < 64; i++)
        bg_pal[i] = ob_pal[i] = 0xFF;   /* (the Color's start-up program leaves them white) */
    for (int i = 0; i < 32; i++)
        bg_col[i] = ob_col[i] = 0x7FFF;
    vbank = 0, wbank = 1, dstep = 4, key1 = rp = r72 = r73 = r74 = r75 = 0;
    hdma_src = hdma_dst = 0, hdma_left = hdma_hblank = hdma_due = halt_mode = 0, switch_left = 0;
    for (int i = 0; i < 5; i++)
        rtc[i] = rtc_latched[i] = 0;
    /* as the machine's own start-up program leaves it at 0x0100 */
    if (cgb)
        A = 0x11, F = 0x80, B = 0x00, C = 0x00, D = 0xFF, E = 0x56, H = 0x00, L = 0x0D;
    else
        A = 0x01, F = 0xB0, B = 0x00, C = 0x13, D = 0x00, E = 0xD8, H = 0x01, L = 0x4D;
    SP = 0xFFFE;
    PC = 0x0100;
    static const u8 start[][2] = {{0x10, 0x80}, {0x11, 0xBF}, {0x12, 0xF3}, {0x14, 0xBF}, {0x16, 0x3F}, {0x19, 0xBF},
                                  {0x1A, 0x7F}, {0x1B, 0xFF}, {0x1C, 0x9F}, {0x1E, 0xBF}, {0x20, 0xFF}, {0x23, 0xBF},
                                  {0x24, 0x77}, {0x25, 0xF3}, {0x26, 0x80}, {LCDC, 0x91}, {BGP, 0xFC}, {OBP0, 0xFF},
                                  {OBP1, 0xFF}, {DMA, 0xFF}, {IF, 0x01}, {SC, 0x00}};
    for (unsigned i = 0; i < sizeof(start) / sizeof(start[0]); i++)
        io[start[i][0]] = start[i][1];
    ie = 0;
    ime = ei_pending = halted = halt_bug = stopped = jammed = test_done = 0;
    cycles = 0;
    divc = 0xABC8;
    tima_state = 0;
    dma_delay = 0, dma_at = 0xA0, dma_block = 0, dma_from = 0;
    serial_left = 0;
    pad_now = 0;
    ram_on = 0, bank_lo = 1, bank_hi = 0, mode = 0, ram_bank = 0, ram_dirty = 0;
    rtc_latch = rtc_reg = 0;
    rtc_cycles = 0;
    map_banks();
    line = 153;                         /* the blanking's last line, where the start-up program ends */
    dot = 400;
    io[LY] = 0;
    ppu_mode = mode_read = 1;
    stat_line = 0;
    win_trig = win_line = frame_done = 0;
    off_ticks = 0;
    stat_update();
}

void gb_frame(u8 pad, u16 *picture)
{
    if (pad & (u8)~pad_now)
        io[IF] |= 0x10;                 /* a button went down */
    pad_now = pad;
    frame_done = 0;
    while (!frame_done && !jammed) {
        step();
        if (off_ticks >= 70224) {       /* the picture is switched off: a frame's time all the same */
            off_ticks -= 70224;
            break;
        }
    }
    if (picture)
        for (unsigned i = 0; i < GB_W * GB_H; i++)
            picture[i] = screen[i];
}

const char *gb_info(void)
{
    return info;
}

u8 *gb_save_ram(u32 *size, int *battery)
{
    *size = ram_size;
    *battery = has_battery && ram_size;
    return cart_ram;
}

int gb_save_changed(void)
{
    int was = ram_dirty;
    ram_dirty = 0;
    return was;
}

u32 gb_cpu_cycles(void)
{
    return cycles;
}

int gb_jammed(void)
{
    return jammed;
}

int gb_test_done(u8 regs[6])
{
    regs[0] = B, regs[1] = C, regs[2] = D, regs[3] = E, regs[4] = H, regs[5] = L;
    return test_done;
}
