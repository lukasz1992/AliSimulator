/* A NES (Famicom): its 6502 (the official instructions and the unofficial
   ones games and test programs use), its picture chip drawn a scanline at a
   time, its two controllers, and the cartridge boards most games came on --
   mappers 0 (NROM), 1 (MMC1), 2 (UxROM), 3 (CNROM), 4 (MMC3), 7 (AxROM),
   11 (Color Dreams), 34 (BNROM), 66 (GxROM) and 71 (Camerica).  NTSC timing:
   262 scanlines of 341 dots, three dots to a 6502 cycle.  What a program can
   read back of the sound chip (its length counters, its frame and sample
   interrupts) is kept exactly as the 6502 runs, because games wait on it; its
   sound -- two pulse voices, the triangle, the noise, the samples (DMC) and
   the mixer -- is made only when the host asks for it (nes_audio_rate), and
   then lazily (see "the sound" below), so that it costs little.

   Each scanline is drawn whole at its start from the registers as they are
   then; the 6502 then runs up to dot 256, the scroll counters step as the
   chip's do at dots 256 / 257, an MMC3 counts the line at dot 260, and the
   6502 runs on to the line's end.  So a game that changes the scroll or
   the banks during the horizontal blanking gets it on the next line, as on
   the machine; changes in the middle of a line show one line late.  The
   sprite-0 hit is told at the dot where it happens. */
#include "nes.h"

#define FC 0x01
#define FZ 0x02
#define FI 0x04
#define FD 0x08
#define FB 0x10
#define FU 0x20
#define FV 0x40
#define FN 0x80

#define LINE_DOTS  341u
#define FRAME_DOTS (262u * LINE_DOTS)
#define NONE       0xFFFFFFFFu

enum { IMP, IMM, ZP, ZPX, ZPY, ABS, ABX, ABY, IND, IZX, IZY, REL };

const u8 nes_rgb[64][3] = {
    {84, 84, 84}, {0, 30, 116}, {8, 16, 144}, {48, 0, 136}, {68, 0, 100}, {92, 0, 48}, {84, 4, 0}, {60, 24, 0},
    {32, 42, 0}, {8, 58, 0}, {0, 64, 0}, {0, 60, 0}, {0, 50, 60}, {0, 0, 0}, {0, 0, 0}, {0, 0, 0},
    {152, 150, 152}, {8, 76, 196}, {48, 50, 236}, {92, 30, 228}, {136, 20, 176}, {160, 20, 100}, {152, 34, 32},
    {120, 60, 0}, {84, 90, 0}, {40, 114, 0}, {8, 124, 0}, {0, 118, 40}, {0, 102, 120}, {0, 0, 0}, {0, 0, 0},
    {0, 0, 0}, {236, 238, 236}, {76, 154, 236}, {120, 124, 236}, {176, 98, 236}, {228, 84, 236}, {236, 88, 180},
    {236, 106, 100}, {212, 136, 32}, {160, 170, 0}, {116, 196, 0}, {76, 208, 32}, {56, 204, 108}, {56, 180, 204},
    {60, 60, 60}, {0, 0, 0}, {0, 0, 0}, {236, 238, 236}, {168, 204, 236}, {188, 188, 236}, {212, 178, 236},
    {236, 174, 236}, {236, 174, 212}, {236, 180, 176}, {228, 196, 144}, {204, 210, 120}, {180, 222, 120},
    {168, 226, 144}, {152, 226, 180}, {160, 214, 228}, {160, 162, 160}, {0, 0, 0}, {0, 0, 0},
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
    u8 nmi, nmi_hold, jam;              /* nmi_hold: it is taken after the next instruction */
    u32 cycles, instructions;
} cpu;

static struct {
    u8 ctrl, mask, status, oam_addr, w, x, buffer, latch;
    u16 v, t;
} ppu;

static struct {
    u8 *prg, *chr;
    u32 prg8k, chr1k;                   /* their sizes in 8 KB / 1 KB banks */
    u8 mapper, mirroring, battery, chr_is_ram, four_screen;
    u8 irq;                             /* the board's interrupt line */
    /* MMC1 */
    u8 shift, count, ctrl, chr0, chr1, prgreg;
    u32 last_write;
    /* MMC3 */
    u8 select, reg[8], latch, counter, reload, irq_on;
} cart;

static struct {
    u8 enabled, length[4], halt[4];
    u8 mode5, irq_inhibit, frame_irq;
    u8 step;                            /* the frame sequencer's next step (0-3; 4: the end of its round) */
    u32 frame_clock, next;              /* next: the frame_clock that step comes at */
    u8 dmc_irq_on, dmc_loop, dmc_rate, dmc_irq, dmc_bits;
    u16 dmc_length, dmc_left;
    u32 dmc_clock;
} apu;

static u8 ram[0x800], save_ram[0x2000], vram[0x1000], chr_ram[0x2000], pal[32], oam[256];
static u8 *prg_map[4], *chr_map[8], *nt_map[4];
static u8 pads[2], pad_shift[2], strobe, save_changed;
static u32 cpu_dot;                     /* where in the frame the 6502 is, in dots (at the instruction's start) */
static u8 op_cycles;                    /* the instruction being run: its cycles (a read is in its last one) */
static u8 vbl_seen;                     /* this frame's vertical blank flag was read before line 241 came */
#define VBL_DOT (241u * LINE_DOTS + 1u) /* where in the frame the vertical blank flag is set */
static u32 hit_dot = NONE;              /* where in the frame the sprite-0 hit happens */
static char info[160];               /* (the longest: an unknown board's line, about 100) */

static const u8 length_table[32] = {10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
                                    12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30};
static const u16 dmc_rates[16] = {428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54};

/* ---- the cartridge's banks ---- */
static void prg8(int slot, int bank)
{
    if (bank < 0)
        bank += (int)cart.prg8k;
    prg_map[slot] = cart.prg + ((u32)bank % cart.prg8k) * 0x2000u;
}

static void prg16(int slot, int bank)
{
    if (bank < 0)
        bank += (int)(cart.prg8k / 2);
    prg8(slot * 2, bank * 2);
    prg8(slot * 2 + 1, bank * 2 + 1);
}

static void chr1(int slot, int bank)
{
    chr_map[slot] = cart.chr + ((u32)bank % cart.chr1k) * 0x400u;
}

static void chr4(int slot, int bank)
{
    for (int i = 0; i < 4; i++)
        chr1(slot * 4 + i, bank * 4 + i);
}

static void chr8(int bank)
{
    chr4(0, bank * 2);
    chr4(1, bank * 2 + 1);
}

/* the name tables: 0 horizontal mirroring, 1 vertical, 2 / 3 one screen (the first / the second), 4 four */
static void mirror(int how)
{
    static const u8 maps[5][4] = {{0, 0, 1, 1}, {0, 1, 0, 1}, {0, 0, 0, 0}, {1, 1, 1, 1}, {0, 1, 2, 3}};
    for (int i = 0; i < 4; i++)
        nt_map[i] = vram + maps[how][i] * 0x400u;
}

static void mmc1_apply(void)
{
    static const u8 mirrors[4] = {2, 3, 1, 0};
    mirror(mirrors[cart.ctrl & 3]);
    int outer = cart.prg8k > 32 ? (cart.chr0 & 0x10) : 0;      /* a 512 KB board: CHR bit 4 picks the half */
    int bank = (cart.prgreg & 0x0F) | outer;
    switch ((cart.ctrl >> 2) & 3) {
    case 0:
    case 1:
        prg16(0, bank & ~1);
        prg16(1, bank | 1);
        break;
    case 2:
        prg16(0, outer);
        prg16(1, bank);
        break;
    default:
        prg16(0, bank);
        prg16(1, outer | 0x0F);
        break;
    }
    if (cart.ctrl & 0x10) {
        chr4(0, cart.chr0);
        chr4(1, cart.chr1);
    } else {
        chr4(0, cart.chr0 & 0x1E);
        chr4(1, cart.chr0 | 1);
    }
}

static void mmc3_apply(void)
{
    int last = (int)cart.prg8k - 1;
    if (cart.select & 0x40) {
        prg8(0, last - 1);
        prg8(2, cart.reg[6]);
    } else {
        prg8(0, cart.reg[6]);
        prg8(2, last - 1);
    }
    prg8(1, cart.reg[7]);
    prg8(3, last);
    int flip = (cart.select & 0x80) ? 4 : 0;
    chr1(0 ^ flip, cart.reg[0] & 0xFE);
    chr1(1 ^ flip, cart.reg[0] | 1);
    chr1(2 ^ flip, cart.reg[1] & 0xFE);
    chr1(3 ^ flip, cart.reg[1] | 1);
    chr1(4 ^ flip, cart.reg[2]);
    chr1(5 ^ flip, cart.reg[3]);
    chr1(6 ^ flip, cart.reg[4]);
    chr1(7 ^ flip, cart.reg[5]);
}

/* an MMC3 counts a scanline (the picture chip's address line 12 rises once a line while it draws) */
static void mapper_line(void)
{
    if (cart.mapper != 4)
        return;
    if (!cart.counter || cart.reload) {
        cart.counter = cart.latch;
        cart.reload = 0;
    } else {
        cart.counter--;
    }
    if (!cart.counter && cart.irq_on)
        cart.irq = 1;
}

static void mapper_write(u16 a, u8 v)
{
    switch (cart.mapper) {
    case 1:
        /* (the second write of one instruction is not seen: INC on a ROM address resets the board) */
        if (cart.last_write == cpu.instructions)
            return;
        cart.last_write = cpu.instructions;
        if (v & 0x80) {
            cart.shift = cart.count = 0;
            cart.ctrl |= 0x0C;
            mmc1_apply();
            return;
        }
        cart.shift |= (u8)((v & 1) << cart.count);
        if (++cart.count == 5) {
            switch ((a >> 13) & 3) {
            case 0: cart.ctrl = cart.shift; break;
            case 1: cart.chr0 = cart.shift; break;
            case 2: cart.chr1 = cart.shift; break;
            default: cart.prgreg = cart.shift; break;
            }
            cart.shift = cart.count = 0;
            mmc1_apply();
        }
        break;
    case 2:
        prg16(0, v);
        break;
    case 3:
        chr8(v);
        break;
    case 4:
        switch (a & 0xE001) {
        case 0x8000:
            cart.select = v;
            mmc3_apply();
            break;
        case 0x8001:
            cart.reg[cart.select & 7] = v;
            mmc3_apply();
            break;
        case 0xA000:
            if (!cart.four_screen)
                mirror((v & 1) ? 0 : 1);
            break;
        case 0xC000:
            cart.latch = v;
            break;
        case 0xC001:
            cart.reload = 1;
            break;
        case 0xE000:
            cart.irq_on = 0;
            cart.irq = 0;
            break;
        case 0xE001:
            cart.irq_on = 1;
            break;
        }
        break;
    case 7:
        prg16(0, (v & 7) * 2);
        prg16(1, (v & 7) * 2 + 1);
        mirror((v & 0x10) ? 3 : 2);
        break;
    case 11:
        prg16(0, (v & 3) * 2);
        prg16(1, (v & 3) * 2 + 1);
        chr8(v >> 4);
        break;
    case 34:
        prg16(0, v * 2);
        prg16(1, v * 2 + 1);
        break;
    case 66:
        prg16(0, ((v >> 4) & 3) * 2);
        prg16(1, ((v >> 4) & 3) * 2 + 1);
        chr8(v & 3);
        break;
    case 71:
        if (a >= 0xC000)
            prg16(0, v);
        break;
    }
}

static void mapper_start(void)
{
    cart.irq = 0;
    cart.shift = cart.count = 0;
    cart.ctrl = 0x0C;
    cart.chr0 = cart.chr1 = cart.prgreg = 0;
    cart.last_write = NONE;
    cart.select = cart.latch = cart.counter = cart.reload = cart.irq_on = 0;
    for (int i = 0; i < 8; i++)
        cart.reg[i] = 0;
    cart.reg[6] = 0;
    cart.reg[7] = 1;
    mirror(cart.four_screen ? 4 : cart.mirroring);
    prg16(0, 0);
    prg16(1, -1);
    chr8(0);
    if (cart.mapper == 1)
        mmc1_apply();
    else if (cart.mapper == 4)
        mmc3_apply();
    else if (cart.mapper == 7)
        mapper_write(0x8000, 0);
    else if (cart.mapper == 11 || cart.mapper == 34 || cart.mapper == 66) {
        prg16(0, 0);
        prg16(1, 1);
    }
}

/* ---- the picture chip ---- */
static u8 ppu_get(u16 a)
{
    a &= 0x3FFF;
    if (a < 0x2000)
        return chr_map[a >> 10][a & 0x3FF];
    if (a < 0x3F00)
        return nt_map[(a >> 10) & 3][a & 0x3FF];
    a &= 0x1F;
    if ((a & 0x13) == 0x10)
        a &= 0x0F;
    return pal[a];
}

static void ppu_put(u16 a, u8 v)
{
    a &= 0x3FFF;
    if (a < 0x2000) {
        if (cart.chr_is_ram)
            chr_map[a >> 10][a & 0x3FF] = v;
    } else if (a < 0x3F00) {
        nt_map[(a >> 10) & 3][a & 0x3FF] = v;
    } else {
        a &= 0x1F;
        if ((a & 0x13) == 0x10)
            a &= 0x0F;
        pal[a] = v & 0x3F;
    }
}

/* the picture chip's address as the program sets it: outside the picture an MMC3 counts its line 12's rises
   too (programs clock its counter by hand that way) */
static u8 a12_was;

static void address_set(u16 v)
{
    u8 now = (v >> 12) & 1;
    ppu.v = v;
    if (now && !a12_was && (!(ppu.mask & 0x18) || (cpu_dot >= 240 * LINE_DOTS && cpu_dot < 261 * LINE_DOTS)))
        mapper_line();
    a12_was = now;
}

static void hit_check(void)
{
    if (hit_dot != NONE && cpu_dot >= hit_dot) {
        ppu.status |= 0x40;
        hit_dot = NONE;
    }
}

static u8 ppu_read(u8 r)
{
    u8 v;
    switch (r) {
    case 2:
        hit_check();
        /* The instructions run whole and the vertical blank flag is set between two of them, its NMI taken
           before the next one.  A program that polls the flag (LDA $2002 / BPL) while its NMI handler reads
           $2002 too would then never see it: the handler always clears it first.  On the console the poll's
           read, in its instruction's last cycle, often comes after the flag rose and before the NMI is taken
           (Solomon's Key waits like that; without this it never starts).  So with the NMI on, a read whose
           instruction began before the flag rose but reads after it sees it set.  (With the NMI off nothing
           changes: the timing test programs synchronise on this flag cycle by cycle, against the rest of the
           timing as it is.) */
        if ((ppu.ctrl & 0x80) && !(ppu.status & 0x80) && !vbl_seen && cpu_dot < VBL_DOT &&
            cpu_dot + 3u * (op_cycles - 1u) >= VBL_DOT) {
            ppu.status |= 0x80;
            vbl_seen = 1;
        }
        v = (u8)((ppu.status & 0xE0) | (ppu.latch & 0x1F));
        ppu.status &= 0x7F;
        ppu.w = 0;
        return ppu.latch = v;
    case 4:
        return ppu.latch = oam[ppu.oam_addr];
    case 7: {
        u16 a = ppu.v & 0x3FFF;
        if (a < 0x3F00) {
            v = ppu.buffer;
            ppu.buffer = ppu_get(a);
        } else {
            v = ppu_get(a);
            ppu.buffer = ppu_get((u16)(a - 0x1000));
        }
        address_set((u16)(ppu.v + ((ppu.ctrl & 4) ? 32 : 1)));
        return ppu.latch = v;
    }
    }
    return ppu.latch;
}

static void ppu_write(u8 r, u8 v)
{
    ppu.latch = v;
    switch (r) {
    case 0:
        if (!(ppu.ctrl & 0x80) && (v & 0x80) && (ppu.status & 0x80))
            cpu.nmi = cpu.nmi_hold = 1;
        ppu.ctrl = v;
        ppu.t = (u16)((ppu.t & 0xF3FF) | ((v & 3) << 10));
        break;
    case 1:
        ppu.mask = v;
        break;
    case 3:
        ppu.oam_addr = v;
        break;
    case 4:
        oam[ppu.oam_addr++] = v;
        break;
    case 5:
        if (!ppu.w) {
            ppu.t = (u16)((ppu.t & 0xFFE0) | (v >> 3));
            ppu.x = v & 7;
        } else {
            ppu.t = (u16)((ppu.t & 0x8C1F) | ((v & 7) << 12) | ((v & 0xF8) << 2));
        }
        ppu.w ^= 1;
        break;
    case 6:
        if (!ppu.w) {
            ppu.t = (u16)((ppu.t & 0x00FF) | ((v & 0x3F) << 8));
        } else {
            ppu.t = (u16)((ppu.t & 0xFF00) | v);
            address_set(ppu.t);
        }
        ppu.w ^= 1;
        break;
    case 7:
        ppu_put(ppu.v, v);
        address_set((u16)(ppu.v + ((ppu.ctrl & 4) ? 32 : 1)));
        break;
    }
}

/* one scanline of the picture, from the registers as they are now */
static void draw_line(int line, u8 *out)
{
    u8 bg[8 + 256 + 16], sp[256];
    u8 gray = (ppu.mask & 1) ? 0x30 : 0x3F, backdrop = pal[0];
    if (!(ppu.mask & 0x18)) {
        for (int x = 0; x < 256; x++)
            out[x] = backdrop & gray;
        return;
    }
    for (int x = 0; x < (int)sizeof(bg); x++)
        bg[x] = 0;
    if (ppu.mask & 0x08) {
        u16 v = ppu.v;
        int fine_y = (v >> 12) & 7, x = 8 - ppu.x;
        u16 table = (ppu.ctrl & 0x10) ? 0x1000 : 0;
        for (int tile = 0; tile < 33; tile++, x += 8) {
            u8 id = nt_map[(v >> 10) & 3][v & 0x3FF];
            u16 at = (u16)(0x3C0 | ((v >> 4) & 0x38) | ((v >> 2) & 0x07));
            u8 attr = nt_map[(v >> 10) & 3][at];
            u8 high = (u8)(((attr >> (((v >> 4) & 4) | (v & 2))) & 3) << 2);
            u16 pa = (u16)(table + id * 16 + fine_y);
            const u8 *bank = chr_map[pa >> 10];
            u8 lo = bank[pa & 0x3FF], hi = bank[(pa & 0x3FF) + 8];
            if (lo | hi) {
                for (int b = 0; b < 8; b++) {
                    u8 c = (u8)(((lo >> (7 - b)) & 1) | (((hi >> (7 - b)) & 1) << 1));
                    if (c)
                        bg[x + b] = high | c;
                }
            }
            if ((v & 0x1F) == 31)
                v = (u16)((v & ~0x1F) ^ 0x0400);
            else
                v++;
        }
        if (!(ppu.mask & 0x02))
            for (int i = 0; i < 8; i++)
                bg[8 + i] = 0;
    }
    for (int x = 0; x < 256; x++)
        sp[x] = 0;
    if (ppu.mask & 0x10) {
        int h = (ppu.ctrl & 0x20) ? 16 : 8, n = 0;
        u8 list[8];
        for (int i = 0; i < 64; i++) {
            int row = line - (oam[i * 4] + 1);
            if (row < 0 || row >= h)
                continue;
            if (n == 8) {
                ppu.status |= 0x20;
                break;
            }
            list[n++] = (u8)i;
        }
        while (n--) {                           /* the later ones first: the earlier are drawn over them */
            const u8 *o = oam + list[n] * 4;
            int row = line - (o[0] + 1);
            u8 attr = o[2], tile = o[1];
            u16 table;
            if (attr & 0x80)
                row = h - 1 - row;
            if (h == 16) {
                table = (tile & 1) ? 0x1000 : 0;
                tile &= 0xFE;
                if (row >= 8) {
                    tile++;
                    row -= 8;
                }
            } else {
                table = (ppu.ctrl & 0x08) ? 0x1000 : 0;
            }
            u16 pa = (u16)(table + tile * 16 + row);
            const u8 *bank = chr_map[pa >> 10];
            u8 lo = bank[pa & 0x3FF], hi = bank[(pa & 0x3FF) + 8];
            u8 mark = (u8)(((attr & 3) << 2) | ((attr & 0x20) ? 0x80 : 0) | (list[n] ? 0 : 0x40));
            for (int b = 0; b < 8 && o[3] + b < 256; b++) {
                int bit = (attr & 0x40) ? b : 7 - b;
                u8 c = (u8)(((lo >> bit) & 1) | (((hi >> bit) & 1) << 1));
                if (c)
                    sp[o[3] + b] = mark | c;
            }
        }
        if (!(ppu.mask & 0x04))
            for (int i = 0; i < 8; i++)
                sp[i] = 0;
    }
    for (int x = 0; x < 256; x++) {
        u8 b = bg[8 + x], s = sp[x], c;
        if ((s & 0x40) && b && x != 255 && hit_dot == NONE && !(ppu.status & 0x40))
            hit_dot = (u32)line * LINE_DOTS + (u32)x + 2;
        if (s && (!(s & 0x80) || !b))
            c = pal[0x10 | (s & 0x0F)];
        else
            c = b ? pal[b] : backdrop;
        out[x] = c & gray;
    }
}

/* ---- the sound ----
   Made lazily: the voices' registers are kept as they are written, and the sound is made up to the 6502's cycle
   only when something it depends on changes -- before a sound register is written, when the frame sequencer
   steps (envelopes, sweeps, length and linear counters), before a cartridge bank switch while a sample plays --
   and at each frame's end.  In between it goes from one voice step to the next (a pulse's eighth of a wave,
   the triangle's 32nd, a noise shift, a sample's bit), adding the mixer's level x the cycles it held into the
   sample being made: each sample is the average of the 6502 cycles it covers (a box filter), then the NES's
   two high-pass filters (90 Hz, 440 Hz) and its low-pass (14 kHz), first order, in fixed point; the samples
   wait in a ring for the host.  The interrupts and what $4015 reads are not touched: they stay apu_tick's.
   Not as the chip: a triangle with a period under 2 (ultrasonic) holds still; a sample's bytes are read as
   they are played, not a byte ahead, and with no stolen 6502 cycles. */
#define IDLE      0x40000000u           /* a voice's countdown while it does not step (what it puts out holds) */
#define RING      4096u                 /* samples kept for the host */
#define CPU_HZ    1789773u

struct envelope {
    u8 start, divider, decay, constant, period;  /* period: the register's low 4 bits, also the constant volume */
};

static struct {                         /* the five voices (cleared by a reset) */
    u32 count[5];                       /* cycles to each one's next step: pulse 1, pulse 2, triangle, noise, DMC */
    u8 out[4];                          /* what the pulses, the triangle and the noise put out now (0-15) */
    u8 volume[4];                       /* the pulses' and the noise's volume while they sound (0: silent) */
    struct envelope env[4];             /* the pulses' and the noise's (env[2], the triangle's, is unused) */
    u16 period[3];                      /* the pulses' and the triangle's timers (11 bits) */
    u8 duty[2], seq[2];
    u8 sweep_on[2], sweep_period[2], sweep_negate[2], sweep_shift[2], sweep_reload[2], sweep_divider[2];
    u8 tri_seq, linear, linear_period, linear_reload;
    u8 noise_mode, noise_period;
    u16 lfsr;
    u8 dmc_level, dmc_bits;             /* the sample's output (0-127); the bit of its byte played next */
    u16 dmc_start, dmc_addr, dmc_left;  /* where the sample starts, the byte played now, the bytes still to play */
    u32 at;                             /* the 6502 cycle the sound is made up to */
} vc;

static struct {                         /* the samples (kept over a reset) */
    u32 rate;                           /* the host's samples a second (0: no sound made) */
    u32 level;                          /* the mixer's output now (1.0 = 40960) */
    u32 acc, left, cur;                 /* the sample being made: level x cycles so far, its cycles still to come,
                                           which of the two lengths it has (0: the shorter) */
    u32 frac, length;                   /* the fraction of a cycle carried over; a sample's length in 1/1024 cycles */
    u32 inv[2];                         /* 65536 / the two lengths a sample can have (whole cycles) */
    s32 k90, k440, k14k;                /* the filters' coefficients (x 65536) */
    s32 hp90, hp440, lp;                /* their states (x 256) */
    u32 head, tail;
    s16 ring[RING];
} snd;

static u16 pulse_mix[31], tnd_mix[203];        /* the mixer: the pulses' sum, 3 triangle + 2 noise + DMC */
static const u8 duties[4] = {0x02, 0x06, 0x1E, 0xF9};  /* the pulses' waves, a bit for each eighth */
static const u16 noise_periods[16] = {4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068};

static void snd_mix(void)
{
    snd.level = (u32)pulse_mix[vc.out[0] + vc.out[1]] + tnd_mix[3 * vc.out[2] + 2 * vc.out[3] + vc.dmc_level];
}

/* the sample made: its average level, the filters, into the ring */
static void snd_sample(void)
{
    s32 x = (s32)((snd.acc * snd.inv[snd.cur]) >> 16);
    snd.acc = 0;
    snd.hp90 += (s32)(((long long)(x * 256 - snd.hp90) * snd.k90) >> 16);
    x -= snd.hp90 >> 8;
    snd.hp440 += (s32)(((long long)(x * 256 - snd.hp440) * snd.k440) >> 16);
    x -= snd.hp440 >> 8;
    snd.lp += (s32)(((long long)(x * 256 - snd.lp) * snd.k14k) >> 16);
    x = snd.lp >> 8;
    x += (x >> 1) + (x >> 3);           /* x 1.625: the mixer's 0-1, swinging +-0.5 about 0 after them, full scale */
    if (x > 32767)
        x = 32767;
    else if (x < -32768)
        x = -32768;
    snd.ring[snd.head++ & (RING - 1)] = (s16)x;
    if (snd.head - snd.tail > RING)
        snd.tail++;                     /* (the host is not taking them: the oldest goes) */
    u32 t = snd.frac + snd.length;
    snd.frac = t & 1023;
    snd.left = t >> 10;
    snd.cur = snd.left - (snd.length >> 10);
}

static u32 least(u32 a, u32 b, u32 c, u32 d, u32 e)
{
    a = b < a ? b : a;
    a = c < a ? c : a;
    a = d < a ? d : a;
    return e < a ? e : a;
}

/* n cycles of sound: a turn to the next sample's end or the next voice step, whichever comes first */
static void snd_run(u32 n)
{
    u32 c0 = vc.count[0], c1 = vc.count[1], c2 = vc.count[2], c3 = vc.count[3], c4 = vc.count[4];
    u32 next = least(c0, c1, c2, c3, c4), gone = 0;    /* the next step; the cycles gone since c0-c4 were right */
    u32 acc = snd.acc, left = snd.left, level = snd.level;
    while (n) {
        u32 d = left < next ? left : next;
        if (n < d)
            d = n;
        acc += level * d;
        n -= d;
        left -= d;
        next -= d;
        gone += d;
        if (!next) {
            c0 -= gone;
            c1 -= gone;
            c2 -= gone;
            c3 -= gone;
            c4 -= gone;
            gone = 0;
            if (!c0) {
                vc.seq[0] = (vc.seq[0] + 1) & 7;
                vc.out[0] = ((duties[vc.duty[0]] >> vc.seq[0]) & 1) ? vc.volume[0] : 0;
                c0 = 2u * (vc.period[0] + 1u);
            }
            if (!c1) {
                vc.seq[1] = (vc.seq[1] + 1) & 7;
                vc.out[1] = ((duties[vc.duty[1]] >> vc.seq[1]) & 1) ? vc.volume[1] : 0;
                c1 = 2u * (vc.period[1] + 1u);
            }
            if (!c2) {
                vc.tri_seq = (vc.tri_seq + 1) & 31;
                vc.out[2] = (u8)(vc.tri_seq < 16 ? 15 - vc.tri_seq : vc.tri_seq - 16);
                c2 = vc.period[2] + 1u;
            }
            if (!c3) {
                u32 r = vc.lfsr, feedback = (r ^ (r >> (vc.noise_mode ? 6 : 1))) & 1;
                vc.lfsr = (u16)((r >> 1) | (feedback << 14));
                vc.out[3] = (vc.lfsr & 1) ? 0 : vc.volume[3];
                c3 = noise_periods[vc.noise_period];
            }
            if (!c4) {                  /* a sample's bit: as apu_tick counts them */
                u8 b = prg_map[(vc.dmc_addr >> 13) & 3][vc.dmc_addr & 0x1FFF];
                if ((b >> vc.dmc_bits) & 1) {
                    if (vc.dmc_level <= 125)
                        vc.dmc_level += 2;
                } else if (vc.dmc_level >= 2) {
                    vc.dmc_level -= 2;
                }
                c4 = dmc_rates[apu.dmc_rate];
                if (++vc.dmc_bits == 8) {
                    vc.dmc_bits = 0;
                    vc.dmc_addr = (u16)((vc.dmc_addr + 1) | 0x8000);
                    if (!--vc.dmc_left) {
                        if (apu.dmc_loop) {
                            vc.dmc_left = apu.dmc_length;
                            vc.dmc_addr = vc.dmc_start;
                        } else {
                            c4 = IDLE;
                        }
                    }
                }
            }
            snd_mix();
            level = snd.level;
            next = least(c0, c1, c2, c3, c4);
        }
        if (!left) {
            snd.acc = acc;
            snd_sample();
            acc = 0;
            left = snd.left;
        }
    }
    vc.count[0] = c0 - gone;
    vc.count[1] = c1 - gone;
    vc.count[2] = c2 - gone;
    vc.count[3] = c3 - gone;
    vc.count[4] = c4 - gone;
    snd.acc = acc;
    snd.left = left;
}

/* the sound made up to a 6502 cycle */
static void snd_catch(u32 to)
{
    u32 n = to - vc.at;
    vc.at = to;
    snd_run(n);
}

static int sweep_target(int i)
{
    int p = vc.period[i], change = p >> vc.sweep_shift[i];
    return vc.sweep_negate[i] ? p - change - (i == 0) : p + change;     /* (pulse 1 negates with one more) */
}

/* after a register write or a sequencer step: which voices step, what they put out */
static void snd_refresh(void)
{
    for (int i = 0; i < 4; i++) {
        if (i == 2)
            continue;
        struct envelope *e = &vc.env[i];
        u8 v = apu.length[i] ? (e->constant ? e->period : e->decay) : 0;
        if (i < 2 && (vc.period[i] < 8 || sweep_target(i) > 0x7FF))
            v = 0;                      /* a pulse too high, or its sweep would take it out of range: silent */
        vc.volume[i] = v;
        if (!v) {
            vc.count[i] = IDLE;
            vc.out[i] = 0;
            continue;
        }
        if (vc.count[i] > 0x100000)     /* it starts stepping */
            vc.count[i] = i < 2 ? 2u * (vc.period[i] + 1u) : noise_periods[vc.noise_period];
        if (i < 2)
            vc.out[i] = ((duties[vc.duty[i]] >> vc.seq[i]) & 1) ? v : 0;
        else
            vc.out[i] = (vc.lfsr & 1) ? 0 : v;
    }
    if (apu.length[2] && vc.linear && vc.period[2] >= 2) {      /* (an ultrasonic triangle holds instead) */
        if (vc.count[2] > 0x100000)
            vc.count[2] = vc.period[2] + 1u;
    } else {
        vc.count[2] = IDLE;
    }
    if (!vc.dmc_left)
        vc.count[4] = IDLE;
    snd_mix();
}

/* the sample's playing taken from apu_tick's count (right at the cycle the sound is made up to) */
static void snd_dmc_sync(void)
{
    u32 period = dmc_rates[apu.dmc_rate];
    vc.dmc_left = apu.dmc_left;
    vc.dmc_bits = apu.dmc_bits;
    vc.count[4] = !apu.dmc_left ? IDLE : apu.dmc_clock >= period ? 0 : period - apu.dmc_clock;
}

static void envelope_clock(struct envelope *e, u8 loop)
{
    if (e->start) {
        e->start = 0;
        e->decay = 15;
        e->divider = e->period;
    } else if (e->divider) {
        e->divider--;
    } else {
        e->divider = e->period;
        if (e->decay)
            e->decay--;
        else if (loop)
            e->decay = 15;
    }
}

/* the frame sequencer's quarter-frame clock (envelopes, the triangle's linear counter) */
static void snd_quarter(void)
{
    envelope_clock(&vc.env[0], apu.halt[0]);
    envelope_clock(&vc.env[1], apu.halt[1]);
    envelope_clock(&vc.env[3], apu.halt[3]);
    if (vc.linear_reload)
        vc.linear = vc.linear_period;
    else if (vc.linear)
        vc.linear--;
    if (!apu.halt[2])
        vc.linear_reload = 0;
}

/* its half-frame clock: the sweeps (the length counters: apu_half_frame) */
static void snd_half(void)
{
    for (int i = 0; i < 2; i++) {
        int target = sweep_target(i);
        if (!vc.sweep_divider[i] && vc.sweep_on[i] && vc.sweep_shift[i] && vc.period[i] >= 8 && target <= 0x7FF)
            vc.period[i] = (u16)target;
        if (!vc.sweep_divider[i] || vc.sweep_reload[i]) {
            vc.sweep_divider[i] = vc.sweep_period[i];
            vc.sweep_reload[i] = 0;
        } else {
            vc.sweep_divider[i]--;
        }
    }
}

/* the voices' registers (kept with the sound off too, so that it starts right) */
static void snd_write(u16 a, u8 v)
{
    int i = (a >> 2) & 1;
    switch (a) {
    case 0x4000: case 0x4004: case 0x400C:
        if (a != 0x400C)
            vc.duty[i] = v >> 6;
        i = a == 0x400C ? 3 : i;
        vc.env[i].constant = (v >> 4) & 1;
        vc.env[i].period = v & 15;
        break;
    case 0x4001: case 0x4005:
        vc.sweep_on[i] = v >> 7;
        vc.sweep_period[i] = (v >> 4) & 7;
        vc.sweep_negate[i] = (v >> 3) & 1;
        vc.sweep_shift[i] = v & 7;
        vc.sweep_reload[i] = 1;
        break;
    case 0x4002: case 0x4006: case 0x400A:
        i = a == 0x400A ? 2 : i;
        vc.period[i] = (u16)((vc.period[i] & 0x700) | v);
        break;
    case 0x4003: case 0x4007: case 0x400B:
        i = a == 0x400B ? 2 : i;
        vc.period[i] = (u16)((vc.period[i] & 0xFF) | (v & 7) << 8);
        if (i < 2) {
            vc.seq[i] = 0;
            vc.env[i].start = 1;
        } else {
            vc.linear_reload = 1;
        }
        break;
    case 0x4008:
        vc.linear_period = v & 0x7F;
        break;
    case 0x400E:
        vc.noise_mode = v >> 7;
        vc.noise_period = v & 15;
        break;
    case 0x400F:
        vc.env[3].start = 1;
        break;
    case 0x4011:
        vc.dmc_level = v & 0x7F;
        break;
    case 0x4012:
        vc.dmc_start = (u16)(0xC000 + v * 64);
        break;
    }
}

static void snd_reset(void)
{
    for (unsigned i = 0; i < sizeof(vc); i++)
        ((u8 *)&vc)[i] = 0;
    for (int i = 0; i < 5; i++)
        vc.count[i] = IDLE;
    vc.lfsr = 1;
    vc.out[2] = 15;                     /* (the triangle's first step) */
    vc.dmc_start = vc.dmc_addr = 0xC000;
    vc.at = cpu.cycles;
    snd_mix();
}

/* a filter's coefficient: 2 pi f / the sample rate, x 65536 */
static u32 omega(u32 f, u32 hz)
{
    u32 a = 6434u * f;                  /* (2 pi x 1024) */
    return a / hz * 64u + (a % hz) * 64u / hz;
}

void nes_audio_rate(u32 hz)
{
    if (hz && hz < 8000)
        hz = 8000;
    if (hz > 192000)
        hz = 192000;
    snd.rate = hz;
    if (!hz)
        return;
    for (u32 n = 1; n < 31; n++)        /* 95.52 / (8128 / n + 100) and 163.67 / (24329 / n + 100), x 40960 */
        pulse_mix[n] = (u16)(n * 3912499u / (8128u + 100u * n));
    for (u32 n = 1; n < 203; n++)
        tnd_mix[n] = (u16)(n * 6703923u / (24329u + 100u * n));
    snd.length = (CPU_HZ * 1024u + hz / 2) / hz;
    snd.inv[0] = 65536u / (snd.length >> 10);
    snd.inv[1] = 65536u / ((snd.length >> 10) + 1);
    snd.k90 = (s32)omega(90, hz);
    snd.k440 = (s32)omega(440, hz);
    u32 w = omega(14000, hz);           /* the low-pass: w / (1 + w) */
    snd.k14k = (s32)(65536u - 0xFFFFFFFFu / (65536u + w));
    snd.acc = snd.frac = snd.cur = 0;
    snd.left = snd.length >> 10;
    snd.head = snd.tail = 0;
    vc.at = cpu.cycles;
    snd_dmc_sync();
    snd_refresh();
    snd.hp90 = (s32)snd.level * 256;    /* (no thump at the start) */
    snd.hp440 = snd.lp = 0;
}

int nes_audio(s16 *out, int max)
{
    int n = 0;
    while (n < max && snd.tail != snd.head)
        out[n++] = snd.ring[snd.tail++ & (RING - 1)];
    return n;
}

/* ---- what the 6502 reads back of the sound chip, kept exactly as it runs ---- */
static void apu_half_frame(void)
{
    for (int i = 0; i < 4; i++)
        if (apu.length[i] && !apu.halt[i])
            apu.length[i]--;
}

/* the frame sequencer's steps (in 6502 cycles from its start) and the end of its round: 4-step, 5-step mode */
static const u16 sequence[2][5] = {{7457, 14913, 22371, 29829, 29830}, {7457, 14913, 22371, 37281, 37282}};

static void apu_sequence(void)
{
    do {
        u8 s = apu.step;
        if (s == 4) {
            apu.frame_clock -= sequence[apu.mode5][4];
            apu.step = 0;
        } else {
            if (snd.rate) {
                snd_catch(cpu.cycles - (apu.frame_clock - sequence[apu.mode5][s]));
                snd_quarter();
                if (s & 1)
                    snd_half();
            }
            if (s & 1)
                apu_half_frame();
            if (s == 3 && !apu.mode5 && !apu.irq_inhibit)
                apu.frame_irq = 1;
            apu.step++;
            if (snd.rate)
                snd_refresh();
        }
        apu.next = sequence[apu.mode5][apu.step];
    } while (apu.frame_clock >= apu.next);
}

static void apu_tick(u32 cycles)
{
    apu.frame_clock += cycles;
    if (apu.frame_clock >= apu.next)
        apu_sequence();
    if (apu.dmc_left) {
        apu.dmc_clock += cycles;
        u32 period = dmc_rates[apu.dmc_rate];
        while (apu.dmc_left && apu.dmc_clock >= period) {
            apu.dmc_clock -= period;
            if (++apu.dmc_bits == 8) {
                apu.dmc_bits = 0;
                if (!--apu.dmc_left) {
                    if (apu.dmc_loop)
                        apu.dmc_left = apu.dmc_length;
                    else if (apu.dmc_irq_on)
                        apu.dmc_irq = 1;
                }
            }
        }
    }
}

static void apu_write(u16 a, u8 v)
{
    int ch = (a - 0x4000) >> 2;
    if (snd.rate)
        snd_catch(cpu.cycles);
    switch (a) {
    case 0x4000: case 0x4004: case 0x400C:
        apu.halt[ch] = (v >> 5) & 1;
        break;
    case 0x4008:
        apu.halt[2] = v >> 7;
        break;
    case 0x4003: case 0x4007: case 0x400B: case 0x400F:
        if (apu.enabled & (1u << ch))
            apu.length[ch] = length_table[v >> 3];
        break;
    case 0x4010:
        apu.dmc_irq_on = v >> 7;
        apu.dmc_loop = (v >> 6) & 1;
        apu.dmc_rate = v & 0x0F;
        if (!apu.dmc_irq_on)
            apu.dmc_irq = 0;
        break;
    case 0x4013:
        apu.dmc_length = (u16)(v * 16 + 1);
        break;
    case 0x4015:
        apu.enabled = v;
        for (int i = 0; i < 4; i++)
            if (!(v & (1u << i)))
                apu.length[i] = 0;
        apu.dmc_irq = 0;
        if (!(v & 0x10))
            apu.dmc_left = 0;
        else if (!apu.dmc_left) {
            apu.dmc_left = apu.dmc_length;
            apu.dmc_clock = 0;
            apu.dmc_bits = 0;
            vc.dmc_addr = vc.dmc_start;
        }
        break;
    case 0x4017:
        apu.mode5 = v >> 7;
        apu.irq_inhibit = (v >> 6) & 1;
        if (apu.irq_inhibit)
            apu.frame_irq = 0;
        apu.frame_clock = 0;
        apu.step = 0;
        apu.next = sequence[apu.mode5][0];
        if (apu.mode5) {
            apu_half_frame();
            if (snd.rate) {
                snd_quarter();
                snd_half();
            }
        }
        break;
    }
    snd_write(a, v);
    if (snd.rate) {
        snd_dmc_sync();
        snd_refresh();
    }
}

/* ---- the 6502's memory ---- */
static u8 rd(u16 a)
{
    if (a >= 0x8000)
        return prg_map[(a >> 13) & 3][a & 0x1FFF];
    if (a < 0x2000)
        return ram[a & 0x7FF];
    if (a < 0x4000)
        return ppu_read(a & 7);
    if (a >= 0x6000)
        return save_ram[a & 0x1FFF];
    if (a == 0x4016 || a == 0x4017) {
        int i = a & 1;
        u8 b = strobe ? (pads[i] & 1) : (pad_shift[i] & 1);
        if (!strobe)
            pad_shift[i] = (u8)((pad_shift[i] >> 1) | 0x80);
        return 0x40 | b;
    }
    if (a == 0x4015) {
        u8 v = (u8)((apu.frame_irq << 6) | (apu.dmc_irq << 7) | (apu.dmc_left ? 0x10 : 0));
        for (int i = 0; i < 4; i++)
            if (apu.length[i])
                v |= (u8)(1u << i);
        apu.frame_irq = 0;
        return v;
    }
    return (u8)(a >> 8);
}

static void wr(u16 a, u8 v)
{
    if (a < 0x2000) {
        ram[a & 0x7FF] = v;
    } else if (a < 0x4000) {
        ppu_write(a & 7, v);
    } else if (a >= 0x8000) {
        if (snd.rate && vc.dmc_left)
            snd_catch(cpu.cycles);              /* (a sample playing is read from the banks as they were) */
        mapper_write(a, v);
    } else if (a >= 0x6000) {
        if (save_ram[a & 0x1FFF] != v) {
            save_ram[a & 0x1FFF] = v;
            save_changed = 1;
        }
    } else if (a == 0x4014) {                   /* the sprites, 256 bytes copied from a page of memory */
        for (int i = 0; i < 256; i++)
            oam[(u8)(ppu.oam_addr + i)] = rd((u16)(v << 8 | i));
        cpu.cycles += 513;
        cpu_dot += 513 * 3;
        apu_tick(513);
    } else if (a == 0x4016) {
        if (v & 1) {
            strobe = 1;
        } else if (strobe) {
            strobe = 0;
            pad_shift[0] = pads[0];
            pad_shift[1] = pads[1];
        }
    } else if (a < 0x4018) {
        apu_write(a, v);
    }
}

/* ---- the 6502 ---- */
static void push(u8 v)
{
    ram[0x100 | cpu.s--] = v;
}

static u8 pull(void)
{
    return ram[0x100 | ++cpu.s];
}

static u16 rd16(u16 a)
{
    return (u16)(rd(a) | rd((u16)(a + 1)) << 8);
}

static void nz(u8 v)
{
    cpu.p = (u8)((cpu.p & ~(FN | FZ)) | (v & 0x80) | (v ? 0 : FZ));
}

static void interrupt(u16 vector, int brk)
{
    push((u8)(cpu.pc >> 8));
    push((u8)cpu.pc);
    push((u8)((cpu.p & ~FB) | FU | (brk ? FB : 0)));
    cpu.p |= FI;
    cpu.pc = rd16(vector);
}

static void adc(u8 v)
{
    u16 sum = (u16)(cpu.a + v + (cpu.p & FC));
    cpu.p = (u8)((cpu.p & ~(FC | FV)) | (sum > 0xFF ? FC : 0) | ((~(cpu.a ^ v) & (cpu.a ^ sum) & 0x80) ? FV : 0));
    cpu.a = (u8)sum;
    nz(cpu.a);
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

/* one instruction (or an interrupt's entry): the cycles it took */
static u32 step(void)
{
    cpu.instructions++;
    if (cpu.nmi && cpu.nmi_hold) {
        cpu.nmi_hold = 0;
    } else if (cpu.nmi) {
        cpu.nmi = 0;
        interrupt(0xFFFA, 0);
        return 7;
    }
    if ((cart.irq | apu.frame_irq | apu.dmc_irq) && !(cpu.p & FI)) {
        interrupt(0xFFFE, 0);
        return 7;
    }
    u8 op = rd(cpu.pc++), mode = mode_of[op], v;
    u32 cycles = cycles_of[op];
    op_cycles = (u8)cycles;
    u16 ea = 0, base;
    int crossed = 0;
    switch (mode) {
    case IMM:
        ea = cpu.pc++;
        break;
    case ZP:
        ea = rd(cpu.pc++);
        break;
    case ZPX:
        ea = (u8)(rd(cpu.pc++) + cpu.x);
        break;
    case ZPY:
        ea = (u8)(rd(cpu.pc++) + cpu.y);
        break;
    case ABS:
        ea = rd16(cpu.pc);
        cpu.pc += 2;
        break;
    case ABX:
        base = rd16(cpu.pc);
        cpu.pc += 2;
        ea = (u16)(base + cpu.x);
        crossed = ((base ^ ea) & 0xFF00) != 0;
        break;
    case ABY:
        base = rd16(cpu.pc);
        cpu.pc += 2;
        ea = (u16)(base + cpu.y);
        crossed = ((base ^ ea) & 0xFF00) != 0;
        break;
    case IND:
        base = rd16(cpu.pc);
        cpu.pc += 2;
        ea = (u16)(rd(base) | rd((u16)((base & 0xFF00) | ((base + 1) & 0xFF))) << 8);  /* (no carry into the page) */
        break;
    case IZX:
        v = (u8)(rd(cpu.pc++) + cpu.x);
        ea = (u16)(ram[v] | ram[(u8)(v + 1)] << 8);
        break;
    case IZY:
        v = rd(cpu.pc++);
        base = (u16)(ram[v] | ram[(u8)(v + 1)] << 8);
        ea = (u16)(base + cpu.y);
        crossed = ((base ^ ea) & 0xFF00) != 0;
        break;
    case REL:
        ea = (u16)(cpu.pc + 1 + (signed char)rd(cpu.pc));
        cpu.pc++;
        break;
    }
    if (crossed && ((mode == IZY && cycles == 5) || (mode != IZY && cycles == 4)))
        cycles++;
#define BRANCH(taken) \
    if (taken) { \
        cycles += ((cpu.pc ^ ea) & 0xFF00) ? 2 : 1; \
        cpu.pc = ea; \
    } \
    break
#define MODIFY(expr) \
    v = rd(ea); \
    wr(ea, v); \
    v = (expr); \
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
        adc((u8)~rd(ea));
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
    case 0x20:
        cpu.pc--;
        push((u8)(cpu.pc >> 8));
        push((u8)cpu.pc);
        cpu.pc = ea;
        break;
    case 0x60:
        cpu.pc = pull();
        cpu.pc |= (u16)(pull() << 8);
        cpu.pc++;
        break;
    case 0x40:
        cpu.p = (u8)((pull() & ~FB) | FU);
        cpu.pc = pull();
        cpu.pc |= (u16)(pull() << 8);
        break;
    case 0x00:
        cpu.pc++;
        interrupt(0xFFFE, 1);
        break;
    case 0x10: BRANCH(!(cpu.p & FN));
    case 0x30: BRANCH(cpu.p & FN);
    case 0x50: BRANCH(!(cpu.p & FV));
    case 0x70: BRANCH(cpu.p & FV);
    case 0x90: BRANCH(!(cpu.p & FC));
    case 0xB0: BRANCH(cpu.p & FC);
    case 0xD0: BRANCH(!(cpu.p & FZ));
    case 0xF0: BRANCH(cpu.p & FZ);
    case 0x08: push(cpu.p | FB | FU); break;
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
    case 0xA3: case 0xA7: case 0xAF: case 0xB3: case 0xB7: case 0xBF: case 0xAB:
        nz(cpu.a = cpu.x = rd(ea));
        break;
    case 0x83: case 0x87: case 0x8F: case 0x97:
        wr(ea, cpu.a & cpu.x);
        break;
    case 0xC3: case 0xC7: case 0xCF: case 0xD3: case 0xD7: case 0xDB: case 0xDF:
        MODIFY((u8)(v - 1));
        compare(cpu.a, v);
        break;
    case 0xE3: case 0xE7: case 0xEF: case 0xF3: case 0xF7: case 0xFB: case 0xFF:
        MODIFY((u8)(v + 1));
        adc((u8)~v);
        break;
    case 0x03: case 0x07: case 0x0F: case 0x13: case 0x17: case 0x1B: case 0x1F:
        MODIFY(asl(v));
        nz(cpu.a |= v);
        break;
    case 0x23: case 0x27: case 0x2F: case 0x33: case 0x37: case 0x3B: case 0x3F:
        MODIFY(rol(v));
        nz(cpu.a &= v);
        break;
    case 0x43: case 0x47: case 0x4F: case 0x53: case 0x57: case 0x5B: case 0x5F:
        MODIFY(lsr(v));
        nz(cpu.a ^= v);
        break;
    case 0x63: case 0x67: case 0x6F: case 0x73: case 0x77: case 0x7B: case 0x7F:
        MODIFY(ror(v));
        adc(v);
        break;
    case 0x0B: case 0x2B:
        nz(cpu.a &= rd(ea));
        cpu.p = (u8)((cpu.p & ~FC) | (cpu.a >> 7));
        break;
    case 0x4B:
        cpu.a &= rd(ea);
        cpu.a = lsr(cpu.a);
        break;
    case 0x6B:
        cpu.a &= rd(ea);
        cpu.a = (u8)((cpu.a >> 1) | ((cpu.p & FC) << 7));
        nz(cpu.a);
        cpu.p = (u8)((cpu.p & ~(FC | FV)) | ((cpu.a >> 6) & 1) | ((((cpu.a >> 6) ^ (cpu.a >> 5)) & 1) ? FV : 0));
        break;
    case 0xCB:
        v = rd(ea);
        base = (u16)((cpu.a & cpu.x) - v);
        cpu.p = (u8)((cpu.p & ~FC) | ((cpu.a & cpu.x) >= v ? FC : 0));
        cpu.x = (u8)base;
        nz(cpu.x);
        break;
    case 0x02: case 0x12: case 0x22: case 0x32: case 0x42: case 0x52: case 0x62: case 0x72: case 0x92: case 0xB2:
    case 0xD2: case 0xF2:
        cpu.jam = 1;                            /* the 6502 stops for good */
        cpu.pc--;
        break;
    default:
        if (mode != IMP && mode != IMM)
            (void)rd(ea);                       /* the NOPs with an operand read it */
        break;
    }
#undef BRANCH
#undef MODIFY
    return cycles;
}

static void run_to(u32 dot)
{
    while (cpu_dot < dot) {
        u32 before = cpu.cycles;
        u32 c = step();
        cpu.cycles += c;
        cpu_dot += (cpu.cycles - before) * 3;   /* (a sprite copy inside the instruction counted itself) */
        cpu_dot -= (cpu.cycles - before - c) * 3;
        apu_tick(c);
    }
}

void nes_frame(u8 pad1, u8 pad2, u8 *picture)
{
    static u8 unseen[NES_W];
    pads[0] = pad1;
    pads[1] = pad2;
    for (u32 line = 0; line < 262; line++) {
        u32 base = line * LINE_DOTS;
        if (line == 241) {
            run_to(base + 1);
            if (!vbl_seen)                      /* (read already: cleared by that read) */
                ppu.status |= 0x80;
            vbl_seen = 0;
            if (ppu.ctrl & 0x80)
                cpu.nmi = 1;
        } else if (line == 261) {
            run_to(base + 1);
            ppu.status &= 0x1F;
            hit_dot = NONE;
        }
        if (line < 240)
            draw_line((int)line, picture ? picture + line * NES_W : unseen);
        if (line < 240 || line == 261) {
            run_to(base + 256);
            if (ppu.mask & 0x18) {
                if ((ppu.v & 0x7000) != 0x7000) {       /* a line down: the fine row, then the coarse one */
                    ppu.v += 0x1000;
                } else {
                    int y = (ppu.v >> 5) & 31;
                    ppu.v &= (u16)~0x7000;
                    if (y == 29) {
                        y = 0;
                        ppu.v ^= 0x0800;
                    } else {
                        y = (y + 1) & 31;
                    }
                    ppu.v = (u16)((ppu.v & ~0x03E0) | (y << 5));
                }
                ppu.v = (u16)((ppu.v & ~0x041F) | (ppu.t & 0x041F));
            }
            run_to(base + 260);
            if (ppu.mask & 0x18)
                mapper_line();
            if (line == 261) {
                run_to(base + 280);
                if (ppu.mask & 0x18)
                    ppu.v = (u16)((ppu.v & ~0x7BE0) | (ppu.t & 0x7BE0));
            }
        }
        run_to(base + LINE_DOTS);
        hit_check();
    }
    if (snd.rate)
        snd_catch(cpu.cycles);
    cpu_dot -= FRAME_DOTS;
}

void nes_reset(void)
{
    for (unsigned i = 0; i < sizeof(ram); i++)
        ram[i] = 0;
    for (unsigned i = 0; i < sizeof(vram); i++)
        vram[i] = 0;
    for (unsigned i = 0; i < sizeof(oam); i++)
        oam[i] = 0xFF;
    for (unsigned i = 0; i < sizeof(pal); i++)
        pal[i] = 0x0F;
    for (unsigned i = 0; i < sizeof(ppu); i++)
        ((u8 *)&ppu)[i] = 0;
    for (unsigned i = 0; i < sizeof(apu); i++)
        ((u8 *)&apu)[i] = 0;
    apu.dmc_length = 1;
    apu.next = sequence[0][0];
    mapper_start();
    cpu.a = cpu.x = cpu.y = 0;
    cpu.s = 0xFD;
    cpu.p = FI | FU;
    cpu.nmi = cpu.nmi_hold = cpu.jam = 0;
    cpu.cycles = cpu.instructions = 0;
    cpu.pc = rd16(0xFFFC);
    cpu_dot = 0;
    op_cycles = 2;
    vbl_seen = 0;
    hit_dot = NONE;
    a12_was = 0;
    strobe = 0;
    pads[0] = pads[1] = pad_shift[0] = pad_shift[1] = 0;
    snd_reset();
    nes_audio_rate(snd.rate);
}

/* ---- the cartridge ---- */
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

const char *nes_load(u8 *image, u32 size)
{
    static const struct {
        u8 number;
        const char *name;
    } known[] = {{0, "NROM"}, {1, "MMC1"}, {2, "UxROM"}, {3, "CNROM"}, {4, "MMC3"}, {7, "AxROM"},
                 {11, "Color Dreams"}, {34, "BNROM"}, {66, "GxROM"}, {71, "Camerica"}};
    if (size < 16 || image[0] != 'N' || image[1] != 'E' || image[2] != 'S' || image[3] != 0x1A)
        return "not a NES cartridge file (no iNES header)";
    u32 prg = image[4] * 0x4000u, chr = image[5] * 0x2000u, at = 16;
    u8 mapper = image[6] >> 4;
    int old = 0;
    for (int i = 12; i < 16; i++)
        old |= image[i];
    if ((image[7] & 0x0C) == 0x08 || !old)      /* (older tools left text in the header's second half) */
        mapper |= image[7] & 0xF0;
    if (image[6] & 0x04)
        at += 512;                              /* a trainer */
    if (!prg || at + prg + chr > size)
        return "the file is shorter than its header says";
    const char *name = 0;
    for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (known[i].number == mapper)
            name = known[i].name;
    char *p = text(info, "mapper ");
    p = number(p, mapper);
    if (!name) {
        text(p, ": a cartridge board this player does not know (it knows 0, 1, 2, 3, 4, 7, 11, 34, 66, 71)");
        return info;
    }
    cart.prg = image + at;
    cart.prg8k = prg / 0x2000;
    cart.mapper = mapper;
    cart.mirroring = image[6] & 1;
    cart.four_screen = (image[6] >> 3) & 1;
    cart.battery = (image[6] >> 1) & 1;
    cart.chr_is_ram = !chr;
    if (chr) {
        cart.chr = image + at + prg;
        cart.chr1k = chr / 0x400;
    } else {
        cart.chr = chr_ram;
        cart.chr1k = 8;
        for (unsigned i = 0; i < sizeof(chr_ram); i++)
            chr_ram[i] = 0;
    }
    p = text(p, " (");
    p = text(p, name);
    p = text(p, "), PRG ");
    p = number(p, prg / 1024);
    p = text(p, " KB, CHR ");
    if (chr) {
        p = number(p, chr / 1024);
        p = text(p, " KB");
    } else {
        p = text(p, "RAM");
    }
    if (cart.battery)
        p = text(p, ", battery");
    for (unsigned i = 0; i < sizeof(save_ram); i++)
        save_ram[i] = 0;
    save_changed = 0;
    /* each instruction's addressing mode, by its place in the 6502's table */
    for (int op = 0; op < 256; op++) {
        int col = op & 0x1F, m = IMP;
        switch (col) {
        case 0x00: m = (op == 0x20) ? ABS : (op >= 0x80) ? IMM : IMP; break;
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
    nes_reset();
    return 0;
}

const char *nes_info(void)
{
    return info;
}

u8 *nes_save_ram(u32 *size, int *battery)
{
    *size = sizeof(save_ram);
    *battery = cart.battery;
    return save_ram;
}

int nes_save_changed(void)
{
    int c = save_changed;
    save_changed = 0;
    return c;
}

u32 nes_cpu_cycles(void)
{
    return cpu.cycles;
}

int nes_jammed(void)
{
    return cpu.jam;
}

void nes_test_pc(u16 pc)
{
    cpu.pc = pc;
    cpu.cycles = 7;
}

void nes_test_step(u16 *pc, u8 *a, u8 *x, u8 *y, u8 *p, u8 *s, u32 *cycles)
{
    cpu.cycles += step();
    *pc = cpu.pc;
    *a = cpu.a;
    *x = cpu.x;
    *y = cpu.y;
    *p = cpu.p;
    *s = cpu.s;
    *cycles = cpu.cycles;
}

u8 nes_test_peek(u16 address)
{
    return address < 0x2000 ? ram[address & 0x7FF] : rd(address);
}
