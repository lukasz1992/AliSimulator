/* An Amiga 500 (OCS, PAL): see amiga.h.

   The 68000: an interpreter (m68k.inc, shared with md.c).  Each of the
   65536 opcodes is decoded once, at the start, into a byte naming its handler (optab, 64 KB of RAM); a
   handler reads the opcode's fields itself.  Memory is kept the way the
   68000 sees it, a 16-bit word in each host u16 (so a word access is one
   load, a byte one is the word's other half: address ^ 1).  The 16 MB
   address space is 256 pages of 64 KB: a page of plain memory (chip RAM,
   the slow RAM at 0xC00000, the ROMs) is a pointer, the rest (the custom
   chips at 0xDFF000, the CIAs at 0xBFE001 / 0xBFD000) goes through
   io_read / io_write.  The cycle counts are the 68000's own (from its
   manual's tables: the operand's addressing mode, MULU / MULS by their
   operand's bits, DIVU / DIVS a fixed 140 / 158); the chip-memory cycles
   that the picture and the blitter take from it are not counted.  Address
   errors (a word at an odd address) are taken at the end of the
   instruction that made them; the trace exception is there (after each
   instruction run with T set); the bus error and the exact prefetch are
   not.  Checked against the SingleStepTests/m68000 files (MAME's 68000):
   every test passes but those of address errors (not compared) and STOP's
   (its PC as those tests count it).  The flags are kept Musashi's way: N in
   bit 7 of n (after a shift by the size), Z set when z is 0, V in bit 7 of
   v, C and X in bit 8 of c and x.

   The frame: 313 lines (312 in an interlaced short frame) of 227 colour
   clocks, two 68000 cycles each.  In a line the 68000 runs up to the next
   thing that happens -- the copper's next instruction or the end of its
   WAIT (worked out from the beam position it waits for), the blitter or
   the floppy's DMA finishing, a CIA timer's interrupt, an audio block's
   interrupt -- so the copper's register writes and the 68000's interleave
   by their times.  The line's bitplanes are fetched, and the line drawn,
   when the fetch ends (DDFSTOP + 16 colour clocks), from the registers as
   they are then; colour changes made during the line are kept with their
   beam position and drawn where they fall (copper colour bars, splits).
   A line whose registers, colours, sprites and bitplane words are those of
   the frame before is not drawn again.

   The blitter does a whole blit at BLTSIZE's write (area mode with its
   shifts, masks, descending mode and fills; line mode as UAE does it) and
   stays busy for the time it would have taken (2 to 4 colour clocks a
   word, by the channels used), then raises its interrupt.  The floppy:
   each track is made, when the head reaches it, the way AmigaDOS writes it
   (11 sectors with their 0x4489 syncs, odd / even MFM halves, checksums,
   the gap) from the .adf; the disk turns (a word under the head every 32
   us), DSKLEN's second write reads from where it is (after the next sync
   word with WORDSYNC), a word of DMA taking 4 us -- 8 times the real time;
   writes are accepted and forgotten.  The drive (DF0: only) steps, keeps
   its side, motor, track 0, ready and disk-change lines through the CIAs.

   Not done: the sound itself (the audio registers are taken, their
   interrupts come on time), the collision register (CLXDAT reads no
   collision), interlace (each field drawn alone), the 68000 slowed by DMA,
   the copper changing anything but colours in the middle of a line, the
   keyboard's power-up codes and its reset, the mouse's movement (its left
   button is pad B), ECS / AGA. */
#include "amiga.h"

typedef signed char s8;
typedef signed short s16;

#define INLINE static inline __attribute__((always_inline))

/* ---- the 68000 (m68k.inc, shared with md.c) ---- */
#include "m68k.inc"

/* ---- the machine: memory, the beam ---- */
#define LINE_CYCLES 454                 /* a line: 227 colour clocks of 3.546895 MHz, two 68000 cycles each */
#define CHIP_MASK   0x7FFFEu            /* 512 KB of chip RAM */
#define NEVER       0x3FFFFFFF
#define DMA_LINE    25                  /* PAL: the first line after the vertical blanking (sprite DMA starts) */
#define X0          AMIGA_X0            /* the picture's first column and line, in the chips' coordinates */
#define Y0          AMIGA_Y0

static u16 chip[0x40000], slow[0x40000], rom[0x40000], ext[0x40000];
static u32 rom_bytes, ext_bytes;
static int overlay;                     /* the ROM at 0 too (CIA-A's OVL, at power-on) */
static u32 lines = 313, lof = 1;        /* this frame's lines; the long-frame bit */
static u32 frames, cycles_hi;
static u16 *pic;
static char info[128];

#define CHIPW(a) chip[((a) & CHIP_MASK) >> 1]

static void map_memory(void)
{
    for (int i = 0; i < 256; i++)
        rmap[i] = wmap[i] = 0;
    for (int i = 0; i < 0x20; i++)      /* (512 KB, seen four times up to 0x200000) */
        rmap[i] = wmap[i] = (u8 *)chip + ((i & 7) << 16);
    for (int i = 0; i < 8; i++)
        rmap[0xC0 + i] = wmap[0xC0 + i] = (u8 *)slow + (i << 16);
    if (ext_bytes)
        for (u32 i = 0; i < 8; i++)
            rmap[0xE0 + i] = (u8 *)ext + ((i << 16) & (ext_bytes - 1));
    if (rom_bytes)
        for (u32 i = 0; i < 8; i++)
            rmap[0xF8 + i] = (u8 *)rom + ((i << 16) & (rom_bytes - 1));
    if (overlay && rom_bytes)
        for (int i = 0; i < 8; i++)
            rmap[i] = rmap[0xF8 + i];
}

static u32 beam_line(s32 t)
{
    u32 l = (u32)t / LINE_CYCLES;
    return l >= lines ? l - lines : l;
}

static u32 beam_h(s32 t)
{
    return ((u32)t % LINE_CYCLES) >> 1;
}

/* ---- Paula's interrupts ---- */
static u16 dmacon, intena, intreq, adkcon;

static void update_ipl(void)
{
    static const u8 level[14] = {1, 1, 1, 2, 3, 3, 3, 4, 4, 4, 4, 5, 5, 6};
    u32 act = (intena & 0x4000) ? intena & intreq & 0x3FFF : 0, l = 0;
    for (int b = 13; b >= 0; b--)
        if (act & (1u << b)) {
            l = level[b];
            break;
        }
    m.ipl = l;
    check_pending();
}

static void irq(u32 bits)
{
    intreq |= (u16)bits;
    update_ipl();
}

/* ---- the custom registers ---- */
enum {
    DMACONR = 0x002, VPOSR = 0x004, VHPOSR = 0x006, JOY0DAT = 0x00A, JOY1DAT = 0x00C, CLXDAT = 0x00E,
    ADKCONR = 0x010, POTGOR = 0x016, SERDATR = 0x018, DSKBYTR = 0x01A, INTENAR = 0x01C, INTREQR = 0x01E,
    DSKPTH = 0x020, DSKPTL = 0x022, DSKLEN = 0x024, VPOSW = 0x02A, COPCON = 0x02E, POTGO = 0x034,
    JOYTEST = 0x036, BLTCON0 = 0x040, BLTCON1 = 0x042, BLTAFWM = 0x044, BLTALWM = 0x046, BLTCPTH = 0x048,
    BLTBPTH = 0x04C, BLTAPTH = 0x050, BLTDPTH = 0x054, BLTSIZE = 0x058, BLTCMOD = 0x060, BLTBMOD = 0x062,
    BLTAMOD = 0x064, BLTDMOD = 0x066, BLTCDAT = 0x070, BLTBDAT = 0x072, BLTADAT = 0x074, DSKSYNC = 0x07E,
    COP1LCH = 0x080, COP2LCH = 0x084, COPJMP1 = 0x088, COPJMP2 = 0x08A, DIWSTRT = 0x08E, DIWSTOP = 0x090,
    DDFSTRT = 0x092, DDFSTOP = 0x094, DMACON = 0x096, CLXCON = 0x098, INTENA = 0x09A, INTREQ = 0x09C,
    ADKCON = 0x09E, AUD0 = 0x0A0, BPL1PTH = 0x0E0, BPLCON0 = 0x100, BPLCON1 = 0x102, BPLCON2 = 0x104,
    BPL1MOD = 0x108, BPL2MOD = 0x10A, SPR0PTH = 0x120, SPR0POS = 0x140, COLOR00 = 0x180
};
static u16 reg[256];                    /* every register as last written (index: offset / 2) */
#define R(o) reg[(o) >> 1]
static s32 now_t;                       /* when the write being made is made (the copper's time or the 68000's) */
static u8 joy_fire, joy_dir;            /* the joystick: fire (CIA-A PRA bit 7 low), JOY1DAT's directions */
static u8 mouse_button;                 /* the mouse's left button (CIA-A PRA bit 6 low) */
static u16 potgor = 0xFF00;

/* the colours: as written, at the start of the line, the changes made during the line */
static u16 pal[32], line_pal[32], c15[4096];
static struct {
    u16 x, i, v;
} chg[128];
static int nchg;

/* ---- the copper ---- */
static struct {
    u32 lc1, lc2, pc, w1, w2;
    int state;                          /* 0 stopped, 1 running, 2 waiting */
    s32 t;                              /* when it does its next thing */
} cop;

static int blit_busy, bzero;
static s32 blit_end = NEVER;
static void custom_write(u32 r, u32 v);

static s32 cop_wake(s32 t)
{
    u32 line = (u32)t / LINE_CYCLES, h = ((u32)t % LINE_CYCLES) >> 1;
    u32 ve = ((cop.w2 >> 8) & 0x7F) | 0x80, he = cop.w2 & 0xFE, vp = (cop.w1 >> 8) & ve, hp = cop.w1 & 0xFE & he;
    for (; line < lines; line++, h = 0) {
        u32 v = line & 0xFF & ve;
        if (v > vp)
            return (s32)(line * LINE_CYCLES + h * 2);
        if (v == vp) {
            if (he == 0xFE) {
                if (hp < 227)
                    return (s32)(line * LINE_CYCLES + (h > hp ? h : hp) * 2);
            } else {
                for (u32 x = h; x < 227; x++)
                    if ((x & he) >= hp)
                        return (s32)(line * LINE_CYCLES + x * 2);
            }
        }
    }
    return NEVER;
}

static void copper_run(s32 until)
{
    while (cop.state && cop.t <= until) {
        if ((dmacon & 0x280) != 0x280) {
            cop.t = NEVER;              /* (DMA off: DMACON's write starts it again) */
            return;
        }
        if (cop.state == 2) {
            s32 w = cop_wake(cop.t);
            if (w >= NEVER) {
                cop.t = NEVER;
                return;
            }
            if (!(cop.w2 & 0x8000) && blit_busy && blit_end > w)
                w = blit_end;           /* (BFD clear: the blitter finished too) */
            cop.state = 1;
            cop.t = w + 4;
            continue;
        }
        u32 w1 = CHIPW(cop.pc), w2 = CHIPW(cop.pc + 2);
        s32 t = cop.t;
        cop.pc += 4;
        cop.t += 8;                     /* (two words fetched: 4 colour clocks) */
        if (!(w1 & 1)) {                /* MOVE */
            u32 r = w1 & 0x1FE;
            if (r < 0x40 || (r < 0x80 && !(R(COPCON) & 2))) {
                cop.state = 0;
                cop.t = NEVER;
                return;
            }
            now_t = t;
            custom_write(r, w2);
        } else if (!(w2 & 1)) {         /* WAIT */
            cop.w1 = w1, cop.w2 = w2;
            cop.state = 2;
        } else {                        /* SKIP: the next instruction when the beam is there */
            u32 line = beam_line(t), h = beam_h(t);
            u32 ve = ((w2 >> 8) & 0x7F) | 0x80, he = w2 & 0xFE, v = line & 0xFF & ve, vp = (w1 >> 8) & ve;
            if (v > vp || (v == vp && (h & he) >= (w1 & 0xFE & he)))
                cop.pc += 4;
        }
    }
}

static void copper_jump(u32 lc, s32 t)
{
    cop.pc = lc;
    cop.state = 1;
    cop.t = t + 4;
}

/* ---- the blitter ---- */
static u32 bltapt, bltbpt, bltcpt, bltdpt;

static u32 minterm(u32 lf, u32 a, u32 b, u32 c)
{
    switch (lf) {
    case 0xF0: return a;
    case 0xCC: return b;
    case 0xAA: return c;
    case 0x00: return 0;
    case 0xFF: return 0xFFFF;
    case 0xCA: return (a & b) | (~a & c);
    case 0xFA: return a | c;
    case 0xEA: return (a & b) | c;
    case 0x0A: return ~a & c;
    case 0x5A: return a ^ c;
    case 0xFC: return a | b;
    case 0x3C: return a ^ b;
    default: {
        u32 d = 0;
        if (lf & 0x01) d |= ~a & ~b & ~c;
        if (lf & 0x02) d |= ~a & ~b & c;
        if (lf & 0x04) d |= ~a & b & ~c;
        if (lf & 0x08) d |= ~a & b & c;
        if (lf & 0x10) d |= a & ~b & ~c;
        if (lf & 0x20) d |= a & ~b & c;
        if (lf & 0x40) d |= a & b & ~c;
        if (lf & 0x80) d |= a & b & c;
        return d;
    }
    }
}

/* the fill: from bit 0 up (right to left on the screen), the carry flipped at each set bit */
static u32 fill_word(u32 d, u32 *carry, u32 inclusive)
{
    u32 out = 0, c = *carry;
    for (int i = 0; i < 16; i++) {
        u32 bit = (d >> i) & 1;
        if (inclusive) {
            out |= (bit | c) << i;
            c ^= bit;
        } else {
            c ^= bit;
            out |= c << i;
        }
    }
    *carry = c;
    return out;
}

static void blit_area(u32 w, u32 h)
{
    u32 con0 = R(BLTCON0), con1 = R(BLTCON1), use = con0 >> 8, lf = con0 & 0xFF;
    u32 ash = con0 >> 12, bsh = con1 >> 12, desc = con1 & 2, fill = con1 & 0x18, nz = 0;
    u32 fwm = R(BLTAFWM), lwm = R(BLTALWM), aold = 0, bold = 0, adat = R(BLTADAT), bdat = R(BLTBDAT);
    u32 cdat = R(BLTCDAT);
    s32 inc = desc ? -2 : 2;
    s32 amod = (s16)R(BLTAMOD), bmod = (s16)R(BLTBMOD), cmod = (s16)R(BLTCMOD), dmod = (s16)R(BLTDMOD);
    if (desc)
        amod = -amod, bmod = -bmod, cmod = -cmod, dmod = -dmod;
    for (u32 y = 0; y < h; y++) {
        u32 carry = (con1 >> 2) & 1;
        for (u32 x = 0; x < w; x++) {
            if (use & 8) {
                adat = CHIPW(bltapt);
                bltapt += (u32)inc;
            }
            if (use & 4) {
                bdat = CHIPW(bltbpt);
                bltbpt += (u32)inc;
            }
            if (use & 2) {
                cdat = CHIPW(bltcpt);
                bltcpt += (u32)inc;
            }
            u32 a = adat;
            if (x == 0)
                a &= fwm;
            if (x == w - 1)
                a &= lwm;
            u32 as, bs;
            if (desc) {
                as = (a << 16 | aold) >> (16 - ash);
                bs = (bdat << 16 | bold) >> (16 - bsh);
            } else {
                as = (aold << 16 | a) >> ash;
                bs = (bold << 16 | bdat) >> bsh;
            }
            aold = a, bold = bdat;
            u32 d = minterm(lf, as & 0xFFFF, bs & 0xFFFF, cdat) & 0xFFFF;
            if (fill)
                d = fill_word(d, &carry, con1 & 8);
            nz |= d;
            if (use & 1) {
                CHIPW(bltdpt) = (u16)d;
                bltdpt += (u32)inc;
            }
        }
        if (use & 8)
            bltapt += (u32)amod;
        if (use & 4)
            bltbpt += (u32)bmod;
        if (use & 2)
            bltcpt += (u32)cmod;
        if (use & 1)
            bltdpt += (u32)dmod;
    }
    R(BLTADAT) = (u16)adat, R(BLTBDAT) = (u16)bdat, R(BLTCDAT) = (u16)cdat;
    bzero = !nz;
}

/* line mode: the octant from BLTCON1, the error term in BLTAPT, one pixel a step (UAE's way) */
static void blit_line(u32 h)
{
    u32 con0 = R(BLTCON0), con1 = R(BLTCON1), lf = con0 & 0xFF, ash = con0 >> 12, bsh = con1 >> 12;
    u32 b = R(BLTBDAT), tex = ((b >> bsh) | (b << (16 - bsh))) & 0xFFFF, a = R(BLTADAT) & R(BLTAFWM);
    u32 cmod = (u32)(s32)(s16)R(BLTCMOD), amod = (u32)(s32)(s16)R(BLTAMOD), bmod = (u32)(s32)(s16)R(BLTBMOD);
    int sign = (con1 >> 6) & 1, onedot = 0;
    u32 nz = 0;
    for (u32 i = 0; i < h; i++) {
        u32 c = CHIPW(bltcpt);
        u32 ad = a >> ash;
        if ((con1 & 2) && onedot)
            ad = 0;
        onedot = 1;
        u32 d = minterm(lf, ad, (tex & 1) ? 0xFFFF : 0, c) & 0xFFFF;
        nz |= d;
        if (con0 & 0x800)
            bltapt += sign ? bmod : amod;
        if (!sign) {
            if (con1 & 0x10) {
                bltcpt += (con1 & 8) ? -cmod : cmod;
                onedot = 0;
            } else if (con1 & 8) {
                if (ash-- == 0)
                    ash = 15, bltcpt -= 2;
            } else if (++ash == 16) {
                ash = 0, bltcpt += 2;
            }
        }
        if (con1 & 0x10) {
            if (con1 & 4) {
                if (ash-- == 0)
                    ash = 15, bltcpt -= 2;
            } else if (++ash == 16) {
                ash = 0, bltcpt += 2;
            }
        } else {
            bltcpt += (con1 & 4) ? -cmod : cmod;
            onedot = 0;
        }
        sign = (s16)bltapt < 0;
        tex = ((tex << 1) | (tex >> 15)) & 0xFFFF;
        if (con0 & 0x100)
            CHIPW(bltdpt) = (u16)d;
        bltdpt = bltcpt;
    }
    R(BLTCON0) = (u16)((con0 & 0x0FFF) | ash << 12);
    R(BLTCON1) = (u16)((con1 & ~0x40u) | (u32)sign << 6);
    bzero = !nz;
}

/* BLTSIZE: the whole blit done now; busy (and its interrupt) after the time it would take */
static void blit_start(u32 size)
{
    u32 h = size >> 6, w = size & 63, use = (R(BLTCON0) >> 8) & 0xF;
    if (!h)
        h = 1024;
    if (!w)
        w = 64;
    s32 t;
    if (R(BLTCON1) & 1) {
        blit_line(h);
        t = (s32)h * 8;
    } else {
        blit_area(w, h);
        static const u8 cck[16] = {2, 2, 2, 3, 3, 3, 3, 4, 2, 2, 2, 3, 3, 3, 3, 4};
        t = (s32)(w * h * cck[use]) * 2;
    }
    blit_busy = 1;
    blit_end = now_t + t + 8;
}

/* ---- the picture: bitplanes, sprites ---- */
static u32 bplpt[6];
static u16 planes[6][64];
static u32 chunky[64 * 4];              /* a line's colour numbers, 4 to a word */
static u32 nibble[16];                  /* a nibble's 4 bits as 4 bytes (leftmost pixel first) */

static struct sprite {
    u32 pt, x, vstart, vstop, state;    /* state: 0 its control words next, 1 waiting for vstart, 2 showing */
    u16 pos, ctl, data, datb;
    int armed;
} spr[8];

static void sprite_decode(struct sprite *s)
{
    s->vstart = (u32)(s->pos >> 8) | ((s->ctl & 4u) << 6);
    s->vstop = (u32)(s->ctl >> 8) | ((s->ctl & 2u) << 7);
    s->x = (u32)(s->pos & 0xFF) << 1 | (s->ctl & 1u);
}

static void sprite_dma(u32 line)
{
    if ((dmacon & 0x220) != 0x220 || line < DMA_LINE)
        return;
    for (int i = 0; i < 8; i++) {
        struct sprite *s = &spr[i];
        if (s->state == 0 || (s->state == 2 && line == s->vstop)) {
            s->pos = CHIPW(s->pt);
            s->ctl = CHIPW(s->pt + 2);
            s->pt += 4;
            sprite_decode(s);
            s->armed = 0;
            s->state = 1;
            continue;
        }
        if (s->state == 1 && line == s->vstart)
            s->state = 2;
        if (s->state == 2) {
            s->data = CHIPW(s->pt);
            s->datb = CHIPW(s->pt + 2);
            s->pt += 4;
            s->armed = 1;
        }
    }
}

static u16 colour_of(u32 v)             /* a 12-bit colour as 15 bits */
{
    return c15[v & 0xFFF];
}

/* the colour numbers 0..63 as 15-bit colours, by the mode: plain, extra half-brite, dual playfield */
static u16 map[64], map_pal[32];       /* (the palette and the mode it was made for) */
static u32 map_key = ~0u;
static u8 dpf_front[64];                /* dual playfield: 1 when playfield 1 shows, 2 playfield 2, 0 neither */

static void build_map(u32 bpu, u32 dpf, const u16 *p)
{
    u32 pf2pri = R(BPLCON2) & 0x40;
    map_key = bpu << 8 | dpf | pf2pri;
    for (int i = 0; i < 32; i++)
        map_pal[i] = p[i];
    for (u32 i = 0; i < 64; i++) {
        if (dpf) {
            u32 p1 = (i & 1) | ((i >> 1) & 2) | ((i >> 2) & 4), p2 = ((i >> 1) & 1) | ((i >> 2) & 2) | ((i >> 3) & 4);
            u32 f = p1 && (!p2 || !pf2pri) ? 1 : p2 ? 2 : 0;
            dpf_front[i] = (u8)f;
            map[i] = colour_of(p[f == 1 ? p1 : f == 2 ? 8 + p2 : 0]);
        } else if (i < 32) {
            map[i] = colour_of(p[i]);
        } else {
            map[i] = bpu == 6 ? colour_of((p[i - 32] >> 1) & 0x777) : colour_of(p[i & 31]);
        }
    }
}

static void draw_sprites(u16 *out, u32 dl, u32 dr, u32 xs, u32 width, u32 hires, u32 dpf, const u16 *p)
{
    const u8 *ch = (const u8 *)chunky;
    u32 code1 = R(BPLCON2) & 7, code2 = (R(BPLCON2) >> 3) & 7;
    for (int n = 3; n >= 0; n--) {
        struct sprite *e = &spr[2 * n], *o = &spr[2 * n + 1];
        int attached = (o->ctl & 0x80) && o->armed && e->armed;
        for (int k = attached ? 0 : 1; k >= 0; k--) {
            struct sprite *s = attached ? e : k ? o : e;
            if (!s->armed)
                continue;
            for (u32 i = 0; i < 16; i++) {
                u32 x = s->x + 1 + i;
                if (x < dl || x >= dr || x < X0 || x >= X0 + AMIGA_W)
                    continue;
                u32 v = ((s->data >> (15 - i)) & 1) | ((s->datb >> (15 - i)) & 1) << 1, c;
                if (attached) {
                    u32 j = x - (o->x + 1);
                    u32 hi = j < 16 ? (((o->data >> (15 - j)) & 1) | ((o->datb >> (15 - j)) & 1) << 1) : 0;
                    v |= hi << 2;
                    c = 16 + v;
                } else {
                    c = 16 + 4 * (u32)n + v;
                }
                if (!v)
                    continue;
                u32 pf = 0, code = code2;   /* the playfield's colour number there, its priority code */
                u32 k2 = hires ? 2 * (x - xs) : x - xs;
                if (x >= xs && k2 < width)
                    pf = ch[k2];
                if (dpf) {
                    u32 f = dpf_front[pf & 63];
                    code = f == 1 ? code1 : code2;
                    pf = f;
                }
                if (pf && (u32)n >= code)
                    continue;
                out[x - X0] = colour_of(p[c]);
            }
        }
    }
}

/* what a line of the picture was drawn from (the registers, the colours, the sprites, the bitplanes' words):
   a line made from the same as in the frame before is not drawn again (the picture is kept from frame to frame) */
#define KEY_MAX (8 + 32 + 32 + 6 * 64)
static u16 row_key[AMIGA_H][KEY_MAX], row_len[AMIGA_H];
static u16 *row_pic;

static int same_as_before(u32 row, u32 bpu, u32 words, u32 xs, u32 dl, u32 dr)
{
    u16 k[KEY_MAX];
    u32 n = 0;
    if (pic != row_pic) {
        row_pic = pic;
        for (u32 i = 0; i < AMIGA_H; i++)
            row_len[i] = 0;
    }
    if (nchg) {                         /* (colours changed during the line: always drawn) */
        row_len[row] = 0;
        return 0;
    }
    k[n++] = R(BPLCON0), k[n++] = R(BPLCON1), k[n++] = R(BPLCON2), k[n++] = (u16)dl, k[n++] = (u16)dr;
    k[n++] = (u16)xs, k[n++] = (u16)words, k[n++] = (u16)bpu;
    for (int i = 0; i < 32; i++)
        k[n++] = line_pal[i];
    for (int i = 0; i < 8; i++) {
        struct sprite *s = &spr[i];
        k[n++] = (u16)(s->armed ? s->x | 0x8000u : 0), k[n++] = s->armed ? s->data : 0;
        k[n++] = s->armed ? s->datb : 0, k[n++] = s->ctl & 0x80;
    }
    for (u32 p = 0; p < bpu; p++)
        for (u32 i = 0; i < words; i++)
            k[n++] = planes[p][i];
    int same = n == row_len[row];
    u16 *old = row_key[row];
    for (u32 i = 0; i < n && same; i++)
        same = k[i] == old[i];
    if (same)
        return 1;
    for (u32 i = 0; i < n; i++)
        old[i] = k[i];
    row_len[row] = (u16)n;
    return 0;
}

/* the line's bitplanes fetched (their pointers moved on) and, when it is one of the picture's, drawn */
static void render(u32 line)
{
    u32 con0 = R(BPLCON0), bpu = (con0 >> 12) & 7, hires = con0 & 0x8000, ham = con0 & 0x800;
    u32 dpf = con0 & 0x400, v0 = R(DIWSTRT) >> 8, v1 = (R(DIWSTOP) >> 8) | ((R(DIWSTOP) & 0x8000) ? 0 : 0x100);
    if (bpu > 6)
        bpu = 4;
    u32 vdiw = line >= v0 && line < v1;
    u32 words = 0, xs = 0, width = 0;
    if (vdiw && bpu && (dmacon & 0x300) == 0x300 && line >= DMA_LINE) {
        u32 mask = hires ? 0xFC : 0xF8, start = R(DDFSTRT) & mask, stop = R(DDFSTOP) & mask;
        if (start < 0x18)
            start = 0x18;
        if (stop > 0xD8)
            stop = 0xD8;
        if (stop < start)
            stop = start;
        words = hires ? ((stop - start) >> 2) + 2 : ((stop - start) >> 3) + 1;
        xs = 2 * start + (hires ? 9 : 17);
        for (u32 p = 0; p < bpu; p++) {
            u32 a = bplpt[p];
            for (u32 i = 0; i < words; i++, a += 2)
                planes[p][i] = CHIPW(a);
            bplpt[p] = a + (u32)(s32)(s16)R((p & 1) ? BPL2MOD : BPL1MOD);
        }
    }
    if (!pic || line < Y0 || line >= Y0 + AMIGA_H)
        return;
    u16 *out = pic + (line - Y0) * AMIGA_W;
    u32 dl = R(DIWSTRT) & 0xFF, dr = 0x100 | (R(DIWSTOP) & 0xFF);
    if (!vdiw)
        dl = dr = 0;
    if (same_as_before(line - Y0, bpu, words, xs, dl, dr))
        return;
    /* the colour numbers of the fetched pixels, each plane delayed by its playfield's scroll */
    if (words) {
        u32 n = words + 2;
        for (u32 i = 0; i < n * 4; i++)
            chunky[i] = 0;
        for (u32 p = 0; p < bpu; p++) {
            u32 d = (R(BPLCON1) >> ((p & 1) ? 4 : 0)) & 15;
            if (hires)
                d *= 2;
            u32 q = d >> 4, r = d & 15;
            const u16 *w = planes[p];
            for (u32 j = 0; j < n; j++) {
                s32 k = (s32)j - (s32)q;
                u32 cur = (k >= 0 && k < (s32)words) ? w[k] : 0, prev = (k >= 1 && k - 1 < (s32)words) ? w[k - 1] : 0;
                u32 o = r ? ((prev << (16 - r)) | (cur >> r)) & 0xFFFF : cur;
                if (!o)
                    continue;
                u32 *c = chunky + 4 * j;
                c[0] |= nibble[o >> 12] << p;
                c[1] |= nibble[(o >> 8) & 15] << p;
                c[2] |= nibble[(o >> 4) & 15] << p;
                c[3] |= nibble[o & 15] << p;
            }
        }
        width = n * 16;
    }
    const u8 *ch = (const u8 *)chunky;
    u32 kind = ham && !hires && !dpf ? 2 : hires ? 1 : 0;
    /* the playfield's pixels: [pl, pr), inside the window (the rest of the window and the border: colour 0) */
    u32 pl = xs > dl ? xs : dl, pr = xs + (hires ? width / 2 : width);
    if (pr > dr)
        pr = dr;
    if (!words || pl > pr)
        pl = pr = 0;
    /* the line in pieces between the colour changes made during it; the colour map kept while nothing changes */
    u16 p[32];
    int same = (bpu << 8 | dpf | (R(BPLCON2) & 0x40)) == map_key;
    for (int i = 0; i < 32; i++) {
        p[i] = line_pal[i];
        same &= p[i] == map_pal[i];
    }
    if (!same)
        build_map(bpu, dpf, p);
    u32 hold = p[0];                    /* (HAM: the colour held from the pixel before) */
    int next = 0;
    for (u32 x = X0; x < X0 + AMIGA_W;) {
        u32 end = X0 + AMIGA_W;
        if (next < nchg && chg[next].x <= x) {
            while (next < nchg && chg[next].x <= x) {
                p[chg[next].i] = chg[next].v;
                next++;
            }
            build_map(bpu, dpf, p);
        }
        if (next < nchg && chg[next].x < end)
            end = chg[next].x;
        u32 a = x > pl ? x : pl, b = end < pr ? end : pr;
        if (a >= b)
            a = b = end;
        u16 *o = out + (x - X0), c0 = map[0];
        if (x < a)
            hold = p[0];
        for (; x < a; x++)
            *o++ = c0;
        if (kind == 0) {
            const u8 *c = ch + (x - xs);
            for (; x < b; x++)
                *o++ = map[*c++];
        } else if (kind == 1) {
            const u8 *c = ch + 2 * (x - xs);
            for (; x < b; x++, c += 2) {
                u32 l = map[c[0]], r = map[c[1]];
                *o++ = (u16)(((l & 0x7BDE) >> 1) + ((r & 0x7BDE) >> 1) + (l & r & 0x0421));
            }
        } else {
            const u8 *c = ch + (x - xs);
            for (; x < b; x++) {
                u32 i = *c++, v = i & 15;
                switch (i >> 4) {
                case 0: hold = p[v]; break;
                case 1: hold = (hold & 0xFF0) | v; break;
                case 2: hold = (hold & 0x0FF) | v << 8; break;
                default: hold = (hold & 0xF0F) | v << 4; break;
                }
                *o++ = colour_of(hold);
            }
        }
        for (; x < end; x++)
            *o++ = c0;
    }
    int any = 0;
    for (int i = 0; i < 8; i++)
        any |= spr[i].armed;
    if (any)
        draw_sprites(out, dl, dr, xs, width, hires, dpf, p);
}

/* ---- Paula: audio (silent, its interrupts on time), the disk ---- */
static struct {
    u32 lc;
    u16 len, per;
    s32 next;
    int on;
} aud[4];

static s32 audio_block(int ch)          /* a block's time: its length in words, two samples each, per period */
{
    u32 len = aud[ch].len ? aud[ch].len : 65536, per = aud[ch].per < 124 ? 124 : aud[ch].per, t = len * per;
    if (t > 0x0FFFFFFFu)
        t = 0x0FFFFFFFu;                /* (64K words at a long period: over 32 bits of cycles -- a negative time
                                           raised the channel's interrupt again at every instruction) */
    return (s32)(t * 4);
}

static void audio_dma(void)
{
    for (int ch = 0; ch < 4; ch++) {
        int on = (dmacon & 0x200) && (dmacon & (1u << ch));
        if (on && !aud[ch].on)
            aud[ch].next = now_t + 64;  /* (the first block starts: its interrupt soon) */
        if (!on)
            aud[ch].next = NEVER;
        aud[ch].on = on;
    }
}

#define TRACK_WORDS 6334                /* a track's MFM words (300 rpm, 2 us a bit) */
#define WORD_CYCLES 227                 /* the time one passes the head: 32 us */
#define DISK_TURBO  28                  /* the time a word of disk DMA takes here (8 times faster than real) */
static struct {
    const u8 *adf;
    u32 size;
    int cyl, side, motor, sel, changed;
    int track_cyl, track_side;
    u16 track[TRACK_WORDS];
    u16 len_was;
    u32 pt;
    int dma;
    s32 end;
} disk;

static void put_long_mfm(u16 *w, u32 v)        /* a long's data bits (odd then even halves made by the caller) */
{
    w[0] = (u16)(v >> 16);
    w[1] = (u16)v;
}

/* the track as AmigaDOS writes it: 11 sectors (0xAAAA 0xAAAA, sync 0x4489 0x4489, the header, its checksum, the
   data's checksum, 512 bytes, all in odd / even halves), the gap after them */
static void build_track(void)
{
    u16 *t = disk.track;
    u32 n = 0, trk = (u32)(disk.cyl * 2 + disk.side);
    static u8 sync_at[TRACK_WORDS];
    for (u32 i = 0; i < TRACK_WORDS; i++)
        t[i] = 0, sync_at[i] = 0;
    for (u32 sec = 0; sec < 11; sec++) {
        const u8 *data = disk.adf && trk < 160 ? disk.adf + (trk * 11 + sec) * 512 : 0;
        t[n++] = 0, t[n++] = 0;
        t[n] = 0x4489, sync_at[n++] = 1;
        t[n] = 0x4489, sync_at[n++] = 1;
        u32 hdr[5], sum = 0;
        hdr[0] = 0xFF000000u | trk << 16 | sec << 8 | (11 - sec);
        for (int i = 1; i < 5; i++)
            hdr[i] = 0;
        u16 *h = t + n;
        for (int i = 0; i < 5; i++) {   /* the info long, then the 16-byte label: odd halves, even halves */
            u32 odd = (hdr[i] >> 1) & 0x55555555u, even = hdr[i] & 0x55555555u;
            int at = i == 0 ? 0 : 4 + (i - 1) * 2, at_even = i == 0 ? 2 : 4 + 8 + (i - 1) * 2;
            put_long_mfm(h + at, odd);
            put_long_mfm(h + at_even, even);
            sum ^= odd ^ even;
        }
        n += 20;
        put_long_mfm(t + n, (sum >> 1) & 0x55555555u);
        put_long_mfm(t + n + 2, sum & 0x55555555u);
        n += 4;
        u32 dsum = 0;
        u16 *d = t + n + 4;
        for (u32 i = 0; i < 128; i++) {
            u32 v = data ? (u32)data[4 * i] << 24 | (u32)data[4 * i + 1] << 16 | (u32)data[4 * i + 2] << 8 |
                               data[4 * i + 3] : 0;
            u32 odd = (v >> 1) & 0x55555555u, even = v & 0x55555555u;
            put_long_mfm(d + 2 * i, odd);
            put_long_mfm(d + 256 + 2 * i, even);
            dsum ^= odd ^ even;
        }
        dsum &= 0x55555555u;
        put_long_mfm(t + n, (dsum >> 1) & 0x55555555u);
        put_long_mfm(t + n + 2, dsum & 0x55555555u);
        n += 4 + 512;
    }
    /* the clock bits: 1 between two 0 data bits (the sync words keep their missing one) */
    u32 prev = 0;
    for (u32 i = 0; i < TRACK_WORDS; i++) {
        if (sync_at[i]) {
            prev = 1;
            continue;
        }
        u32 d = t[i] & 0x5555;
        u32 clk = ~((d << 1) | (d >> 1) | (prev << 15)) & 0xAAAA;
        t[i] = (u16)(d | clk);
        prev = d & 1;
    }
    disk.track_cyl = disk.cyl;
    disk.track_side = disk.side;
}

static u32 disk_pos(s32 t)              /* the word under the head */
{
    return ((cycles_hi + (u32)t) / WORD_CYCLES) % TRACK_WORDS;
}

static void disk_start(u32 len)
{
    if (disk.track_cyl != disk.cyl || disk.track_side != disk.side)
        build_track();
    u32 n = len & 0x3FFF, at = disk_pos(now_t), wait = 0;
    disk.pt = (u32)R(DSKPTH) << 16 | R(DSKPTL);
    if (!(len & 0x4000) && disk.adf && disk.sel) {
        if (adkcon & 0x400) {           /* WORDSYNC: from the word after the next sync word */
            u16 sync = R(DSKSYNC);
            for (wait = 0; wait < TRACK_WORDS; wait++)
                if (disk.track[(at + wait) % TRACK_WORDS] == sync)
                    break;
            if (wait == TRACK_WORDS) {
                disk.dma = 1;           /* (none: it waits for ever) */
                disk.end = NEVER;
                return;
            }
            at += wait + 1;
            irq(0x1000);                /* DSKSYN */
        }
        for (u32 i = 0; i < n; i++, disk.pt += 2)
            CHIPW(disk.pt) = disk.track[(at + i) % TRACK_WORDS];
    } else {
        disk.pt += n * 2;               /* (a write: nothing is written) */
    }
    disk.dma = 1;
    disk.end = now_t + (s32)((wait + n) * DISK_TURBO) + 64;
}

static void drive_control(u32 v, u32 old)
{
    int sel = !(v & 0x08);
    if (sel && (old & 0x08))
        disk.motor = !(v & 0x80);       /* (the motor's line latched when the drive is selected) */
    if (sel && (old & 0x01) && !(v & 0x01)) {
        if (v & 0x02) {
            if (disk.cyl > 0)
                disk.cyl--;
        } else if (disk.cyl < 83) {
            disk.cyl++;
        }
        if (disk.adf)
            disk.changed = 0;
    }
    disk.side = (v & 0x04) ? 0 : 1;
    disk.sel = sel;
}

/* ---- the CIAs (8520): CIA-A at 0xBFE001 (odd bytes), CIA-B at 0xBFD000 (even ones) ---- */
static struct cia {
    u8 pra, prb, ddra, ddrb, sdr, icr, mask, cra, crb;
    u16 ta, tb, la, lb;
    u32 tod, alarm, latch, latched, tod_run;
    s32 last, next;                     /* the time it was brought up to (a whole E clock); its next interrupt */
    u32 bit;                            /* Paula's interrupt it raises: PORTS (2) or EXTER (6) */
} cia[2];

static u32 timer_run(u16 *t, u16 latch, u32 n, u8 *cr)
{
    if (n <= *t) {
        *t = (u16)(*t - n);
        return 0;
    }
    n -= (u32)*t + 1;
    if (*cr & 8) {                      /* one-shot: stopped at the underflow */
        *t = latch;
        *cr &= (u8)~1;
        return 1;
    }
    u32 period = (u32)latch + 1;
    *t = (u16)(latch - n % period);
    return 1 + n / period;
}

static void cia_check(struct cia *c)
{
    if ((c->icr & c->mask & 0x1F) && !(c->icr & 0x80)) {
        c->icr |= 0x80;
        irq(c->bit);
    }
}

static void cia_schedule(struct cia *c)
{
    s32 n = NEVER;
    if ((c->cra & 0x21) == 0x01 && (c->mask & 1 || ((c->crb & 0x61) == 0x41 && c->mask & 2)))
        n = c->last + ((s32)c->ta + 1) * 10;
    if ((c->crb & 0x61) == 0x01 && (c->mask & 2)) {
        s32 b = c->last + ((s32)c->tb + 1) * 10;
        if (b < n)
            n = b;
    }
    c->next = n;
}

static void cia_update(struct cia *c, s32 now)
{
    if (now <= c->last)
        return;
    u32 n = (u32)(now - c->last) / 10, ua = 0;
    c->last += (s32)n * 10;
    if (!n)
        return;
    if ((c->cra & 0x21) == 0x01) {
        ua = timer_run(&c->ta, c->la, n, &c->cra);
        if (ua)
            c->icr |= 1;
    }
    if (c->crb & 1) {
        u32 in = (c->crb >> 5) & 3, nb = in == 0 ? n : in >= 2 ? ua : 0;
        if (nb && timer_run(&c->tb, c->lb, nb, &c->crb))
            c->icr |= 2;
    }
    cia_check(c);
    cia_schedule(c);
}

static void tod_tick(struct cia *c)
{
    if (!c->tod_run)
        return;
    c->tod = (c->tod + 1) & 0xFFFFFF;
    if (c->tod == c->alarm) {
        c->icr |= 4;
        cia_check(c);
    }
}

/* the keyboard: key codes into CIA-A's serial register, each after the 68000's handshake (SPMODE out, then in) */
static u8 kq[64], held_was[16];
static int kq_head, kq_tail, kb_wait;
static s32 kb_time;

static void key_event(u32 code)
{
    int next = (kq_tail + 1) & 63;
    if (next != kq_head) {
        kq[kq_tail] = (u8)code;
        kq_tail = next;
    }
}

static void keyboard_poll(s32 now)
{
    if (kb_wait && now - kb_time > 1000000)
        kb_wait = 0;                    /* (no handshake: the keyboard goes on after a while) */
    if (kb_wait || kq_head == kq_tail || now < kb_time)
        return;
    u32 code = kq[kq_head];
    kq_head = (kq_head + 1) & 63;
    cia[0].sdr = (u8)~((code << 1) | (code >> 7));
    cia[0].icr |= 8;
    cia_check(&cia[0]);
    kb_wait = 1;
    kb_time = now;
}

static u32 cia_read(int n, u32 r, s32 now)
{
    struct cia *c = &cia[n];
    cia_update(c, now);
    switch (r) {
    case 0:
        if (n == 0) {                   /* fire buttons, the drive's lines (active low) */
            u32 in = 0xFF;
            if (joy_fire)
                in &= ~0x80u;
            if (mouse_button)
                in &= ~0x40u;
            if (disk.sel) {
                if (disk.changed || !disk.adf)
                    in &= ~0x04u;
                in &= ~0x08u;           /* (write-protected) */
                if (disk.cyl == 0)
                    in &= ~0x10u;
                in &= ~0x20u;           /* ready (an internal drive's ID: all ones) */
            }
            return (c->pra & c->ddra) | (in & ~c->ddra);
        }
        return (c->pra & c->ddra) | (0xFF & ~c->ddra);
    case 1:
        return (c->prb & c->ddrb) | (0xFF & ~c->ddrb);
    case 2: return c->ddra;
    case 3: return c->ddrb;
    case 4: return c->ta & 0xFF;
    case 5: return c->ta >> 8;
    case 6: return c->tb & 0xFF;
    case 7: return c->tb >> 8;
    case 8: {
        u32 v = (c->latched ? c->latch : c->tod) & 0xFF;
        c->latched = 0;
        return v;
    }
    case 9: return ((c->latched ? c->latch : c->tod) >> 8) & 0xFF;
    case 10:
        c->latch = c->tod;
        c->latched = 1;
        return (c->latch >> 16) & 0xFF;
    case 12: return c->sdr;
    case 13: {
        u32 v = c->icr;
        c->icr = 0;
        return v;
    }
    case 14: return c->cra;
    case 15: return c->crb;
    default: return 0xFF;
    }
}

static void set_overlay(void)
{
    int ov = !(cia[0].ddra & 1) || (cia[0].pra & 1);
    if (ov != overlay) {
        overlay = ov;
        map_memory();
    }
}

static void cia_write(int n, u32 r, u32 v, s32 now)
{
    struct cia *c = &cia[n];
    cia_update(c, now);
    switch (r) {
    case 0:
        c->pra = (u8)v;
        if (n == 0)
            set_overlay();
        break;
    case 1: {
        u32 old = c->prb | (u8)~c->ddrb;
        c->prb = (u8)v;
        if (n == 1)
            drive_control(c->prb | (u8)~c->ddrb, old);
        break;
    }
    case 2:
        c->ddra = (u8)v;
        if (n == 0)
            set_overlay();
        break;
    case 3: {
        u32 old = c->prb | (u8)~c->ddrb;
        c->ddrb = (u8)v;
        if (n == 1)
            drive_control(c->prb | (u8)~c->ddrb, old);
        break;
    }
    case 4: c->la = (u16)((c->la & 0xFF00) | v); break;
    case 5:
        c->la = (u16)((c->la & 0xFF) | v << 8);
        if (!(c->cra & 1))
            c->ta = c->la;
        if (c->cra & 8) {               /* one-shot: the high byte starts it */
            c->ta = c->la;
            c->cra |= 1;
        }
        break;
    case 6: c->lb = (u16)((c->lb & 0xFF00) | v); break;
    case 7:
        c->lb = (u16)((c->lb & 0xFF) | v << 8);
        if (!(c->crb & 1))
            c->tb = c->lb;
        if (c->crb & 8) {
            c->tb = c->lb;
            c->crb |= 1;
        }
        break;
    case 8: case 9: case 10: {
        u32 shift = (r - 8) * 8, *t = (c->crb & 0x80) ? &c->alarm : &c->tod;
        *t = (*t & ~(0xFFu << shift)) | v << shift;
        if (!(c->crb & 0x80))
            c->tod_run = r == 8;        /* (the high byte stops it, the low one starts it again) */
        break;
    }
    case 12: c->sdr = (u8)v; break;
    case 13:
        if (v & 0x80)
            c->mask |= (u8)(v & 0x1F);
        else
            c->mask &= (u8)~v;
        cia_check(c);
        break;
    case 14:
        if (n == 0 && (c->cra & 0x40) && !(v & 0x40) && kb_wait) {
            kb_wait = 0;                /* the keyboard's handshake */
            kb_time = now + 400;
        }
        if (v & 0x10)
            c->ta = c->la;
        c->cra = (u8)(v & ~0x10u);
        break;
    case 15:
        if (v & 0x10)
            c->tb = c->lb;
        c->crb = (u8)(v & ~0x10u);
        break;
    default:
        break;
    }
    cia_schedule(c);
}

/* ---- the custom registers: reads, writes ---- */
static u32 custom_read(u32 r, s32 now)
{
    switch (r) {
    case DMACONR:
        return dmacon | (blit_busy ? 0x4000u : 0) | (bzero ? 0x2000u : 0);
    case VPOSR:
        return lof << 15 | ((beam_line(now) >> 8) & 1);
    case VHPOSR:
        return (beam_line(now) & 0xFF) << 8 | beam_h(now);
    case JOY0DAT:
        return R(JOYTEST);
    case JOY1DAT: {
        u32 v = 0;
        if (joy_dir & AMIGA_RIGHT) v |= 0x0002;
        if (joy_dir & AMIGA_LEFT) v |= 0x0200;
        if (((joy_dir & AMIGA_DOWN) != 0) ^ ((v & 2) != 0)) v |= 0x0001;
        if (((joy_dir & AMIGA_UP) != 0) ^ ((v & 0x200) != 0)) v |= 0x0100;
        return v;
    }
    case CLXDAT:
        return 0x8000;
    case ADKCONR:
        return adkcon;
    case POTGOR:
        return potgor;
    case SERDATR:
        return 0x3000;                  /* (transmit buffer empty) */
    case DSKBYTR: {
        u32 at = disk_pos(now), w = disk.track[at];
        return 0x8000u | (disk.dma ? 0x4000u : 0) | (w == R(DSKSYNC) ? 0x1000u : 0) | (w & 0xFF);
    }
    case INTENAR:
        return intena;
    case INTREQR:
        return intreq;
    default:
        return 0;
    }
}

static void set_clr(u16 *r, u32 v)
{
    if (v & 0x8000)
        *r |= (u16)(v & 0x7FFF);
    else
        *r &= (u16)~v;
}

static void custom_write(u32 r, u32 v)
{
    v &= 0xFFFF;
    reg[r >> 1] = (u16)v;
    if (r >= COLOR00 + 64)
        return;                         /* (the ECS / AGA registers past COLOR31) */
    if (r >= COLOR00) {
        u32 i = (r - COLOR00) >> 1;
        pal[i] = (u16)(v & 0xFFF);
        u32 h = beam_h(now_t), x = 2 * h + 1;
        if (nchg < 128 && x < X0 + AMIGA_W) {
            chg[nchg].x = (u16)x, chg[nchg].i = (u16)i, chg[nchg].v = (u16)(v & 0xFFF);
            nchg++;
        }
        return;
    }
    if (r >= SPR0POS) {
        struct sprite *s = &spr[(r - SPR0POS) >> 3];
        switch (r & 6) {
        case 0: s->pos = (u16)v; sprite_decode(s); break;
        case 2: s->ctl = (u16)v; sprite_decode(s); s->armed = 0; break;
        case 4: s->data = (u16)v; s->armed = 1; break;
        default: s->datb = (u16)v; break;
        }
        return;
    }
    if (r >= SPR0PTH && r < SPR0POS) {
        struct sprite *s = &spr[(r - SPR0PTH) >> 2];
        s->pt = (r & 2) ? (s->pt & 0xFFFF0000u) | (v & 0xFFFE) : (s->pt & 0xFFFF) | v << 16;
        return;
    }
    if (r >= BPL1PTH && r < BPL1PTH + 24) {
        u32 *p = &bplpt[(r - BPL1PTH) >> 2];
        *p = (r & 2) ? (*p & 0xFFFF0000u) | (v & 0xFFFE) : (*p & 0xFFFF) | v << 16;
        return;
    }
    if (r >= AUD0 && r < AUD0 + 0x40) {
        int ch = (int)(r - AUD0) >> 4;
        switch (r & 15) {
        case 4: aud[ch].len = (u16)v; break;
        case 6: aud[ch].per = (u16)v; break;
        case 10:                        /* AUDxDAT without DMA: two samples, then its interrupt */
            if (!aud[ch].on)
                aud[ch].next = now_t + (s32)((aud[ch].per < 124 ? 124 : aud[ch].per) * 4);
            break;
        default: break;
        }
        return;
    }
    switch (r) {
    case DSKLEN:
        if (v & 0x8000) {
            if ((disk.len_was & 0x8000) && (dmacon & 0x210) == 0x210)
                disk_start(v);
        } else {
            disk.dma = 0;
            disk.end = NEVER;
        }
        disk.len_was = (u16)v;
        break;
    case JOYTEST:
        break;
    case POTGO:
        break;
    case BLTCPTH: bltcpt = (bltcpt & 0xFFFF) | v << 16; break;
    case BLTCPTH + 2: bltcpt = (bltcpt & 0xFFFF0000u) | (v & 0xFFFE); break;
    case BLTBPTH: bltbpt = (bltbpt & 0xFFFF) | v << 16; break;
    case BLTBPTH + 2: bltbpt = (bltbpt & 0xFFFF0000u) | (v & 0xFFFE); break;
    case BLTAPTH: bltapt = (bltapt & 0xFFFF) | v << 16; break;
    case BLTAPTH + 2: bltapt = (bltapt & 0xFFFF0000u) | (v & 0xFFFE); break;
    case BLTDPTH: bltdpt = (bltdpt & 0xFFFF) | v << 16; break;
    case BLTDPTH + 2: bltdpt = (bltdpt & 0xFFFF0000u) | (v & 0xFFFE); break;
    case BLTSIZE:
        blit_start(v);
        break;
    case COP1LCH: cop.lc1 = (cop.lc1 & 0xFFFF) | v << 16; break;
    case COP1LCH + 2: cop.lc1 = (cop.lc1 & 0xFFFF0000u) | (v & 0xFFFE); break;
    case COP2LCH: cop.lc2 = (cop.lc2 & 0xFFFF) | v << 16; break;
    case COP2LCH + 2: cop.lc2 = (cop.lc2 & 0xFFFF0000u) | (v & 0xFFFE); break;
    case COPJMP1: copper_jump(cop.lc1, now_t); break;
    case COPJMP2: copper_jump(cop.lc2, now_t); break;
    case DMACON: {
        u32 old = dmacon;
        set_clr(&dmacon, v & 0x87FF);
        if ((dmacon & 0x280) == 0x280 && (old & 0x280) != 0x280 && cop.state && cop.t >= NEVER)
            cop.t = now_t + 4;          /* (the copper goes on where it was) */
        if ((dmacon ^ old) & 0x20F)
            audio_dma();
        break;
    }
    case INTENA:
        set_clr(&intena, v);
        update_ipl();
        break;
    case INTREQ:
        set_clr(&intreq, v);
        if (cia[0].icr & 0x80)          /* (the CIAs hold their lines down until read) */
            intreq |= 0x0008;
        if (cia[1].icr & 0x80)
            intreq |= 0x2000;
        update_ipl();
        break;
    case ADKCON:
        set_clr(&adkcon, v);
        break;
    case VPOSW:
        lof = v >> 15;
        break;
    default:
        break;
    }
}

/* ---- the 68000's view of the chips ---- */
static u32 io_read(u32 a, int size)
{
    if ((a & 0xFF0000) == 0xBF0000) {   /* the CIAs */
        u32 r = (a >> 8) & 15, hi = 0xFF, lo = 0xFF;
        if (size == 2 || !(a & 1))
            hi = (a & 0x2000) ? 0xFF : cia_read(1, r, m.clock);
        if (size == 2 || (a & 1))
            lo = (a & 0x1000) ? 0xFF : cia_read(0, r, m.clock);
        return size == 2 ? hi << 8 | lo : (a & 1) ? lo : hi;
    }
    if (a >= 0xC00000 && a < 0xE00000) {        /* the chips' registers (seen all over 0xC00000..0xDFFFFF) */
        if (a < 0xC80000)
            return 0;
        u32 v = custom_read(a & 0x1FE, m.clock);
        return size == 2 ? v : (a & 1) ? v & 0xFF : v >> 8;
    }
    return 0;
}

static void io_write(u32 a, u32 v, int size)
{
    if ((a & 0xFF0000) == 0xBF0000) {
        u32 r = (a >> 8) & 15;
        if ((size == 2 || !(a & 1)) && !(a & 0x2000))
            cia_write(1, r, size == 2 ? v >> 8 : v, m.clock);
        if ((size == 2 || (a & 1)) && !(a & 0x1000))
            cia_write(0, r, v & 0xFF, m.clock);
        return;
    }
    if (a >= 0xC80000 && a < 0xE00000) {
        now_t = m.clock;
        custom_write(a & 0x1FE, size == 2 ? v : (v & 0xFF) * 0x101);
    }
}

/* ---- the frame ---- */
static s32 next_event(void)
{
    s32 n = blit_busy ? blit_end : NEVER;
    if (disk.dma && disk.end < n)
        n = disk.end;
    for (int i = 0; i < 2; i++)
        if (cia[i].next < n)
            n = cia[i].next;
    for (int ch = 0; ch < 4; ch++)
        if (aud[ch].next < n)
            n = aud[ch].next;
    return n;
}

static void events(s32 now)
{
    if (blit_busy && now >= blit_end) {
        blit_busy = 0;
        blit_end = NEVER;
        irq(0x0040);
    }
    if (disk.dma && now >= disk.end) {
        disk.dma = 0;
        disk.end = NEVER;
        R(DSKPTH) = (u16)(disk.pt >> 16), R(DSKPTL) = (u16)disk.pt;
        irq(0x0002);
    }
    for (int i = 0; i < 2; i++)
        if (now >= cia[i].next)
            cia_update(&cia[i], now);
    for (int ch = 0; ch < 4; ch++)
        if (now >= aud[ch].next) {
            irq(0x80u << ch);
            aud[ch].next = aud[ch].on ? aud[ch].next + audio_block(ch) : NEVER;
            if (aud[ch].next < now)
                aud[ch].next = now + audio_block(ch);
        }
}

static void run_until(s32 limit)
{
    while (m.clock < limit) {
        copper_run(m.clock);
        events(m.clock);
        s32 next = next_event();
        if (cop.state && cop.t < next)
            next = cop.t;
        if (next > limit)
            next = limit;
        if (next <= m.clock)
            next = m.clock + 1;
        cpu_run(next);
    }
    copper_run(m.clock);
    events(m.clock);
}

static void run_line(u32 line)
{
    s32 t0 = (s32)(line * LINE_CYCLES);
    if (line == 0) {                    /* the vertical blanking: its interrupt, the copper from COP1LC, TOD */
        irq(0x0020);
        copper_jump(cop.lc1, t0);
        tod_tick(&cia[0]);
        for (int i = 0; i < 8; i++)
            spr[i].state = 0, spr[i].armed = 0;
    }
    tod_tick(&cia[1]);
    sprite_dma(line);
    keyboard_poll(t0);
    for (int i = 0; i < 32; i++)
        line_pal[i] = pal[i];
    nchg = 0;
    u32 stop = (R(DDFSTOP) & 0xFC) + 0x10;
    run_until(t0 + 2 * (s32)(stop < 226 ? stop : 226));
    render(line);
    nchg = 0;
    run_until(t0 + LINE_CYCLES);
}

/* ---- the keyboard's keys ---- */
/* a USB keyboard's usage -> the Amiga's key code (0xFF: none) */
static u8 hid_key(u32 u)
{
    static const u8 letters[26] = {0x20, 0x35, 0x33, 0x22, 0x12, 0x23, 0x24, 0x25, 0x17, 0x26, 0x27, 0x28, 0x37,
                                   0x36, 0x18, 0x19, 0x10, 0x13, 0x21, 0x14, 0x16, 0x34, 0x11, 0x32, 0x15, 0x31};
    /* 0x28..0x38: Enter Esc Backspace Tab Space - = [ ] \ # ; ' ` , . / */
    static const u8 marks[17] = {0x44, 0x45, 0x41, 0x42, 0x40, 0x0B, 0x0C, 0x1A, 0x1B, 0x0D, 0x2B, 0x29, 0x2A, 0x00,
                                 0x38, 0x39, 0x3A};
    /* 0x4F..0x63: Right Left Down Up NumLock KP/ KP* KP- KP+ KPEnter KP1..KP9 KP0 KP. */
    static const u8 pad[21] = {0x4E, 0x4F, 0x4D, 0x4C, 0xFF, 0x5C, 0x5D, 0x4A, 0x5E, 0x43, 0x1D, 0x1E, 0x1F, 0x2D,
                               0x2E, 0x2F, 0x3D, 0x3E, 0x3F, 0x0F, 0x3C};
    static const u8 mods[8] = {0x63, 0x60, 0x64, 0x66, 0x63, 0x61, 0x65, 0x67};
    if (u >= 0x04 && u <= 0x1D)
        return letters[u - 0x04];
    if (u >= 0x1E && u <= 0x27)
        return (u8)(u - 0x1D);          /* 1..9, 0: 0x01..0x0A */
    if (u >= 0x28 && u <= 0x38)
        return marks[u - 0x28];
    if (u == 0x39)
        return 0x62;                    /* Caps Lock */
    if (u >= 0x3A && u <= 0x43)
        return (u8)(0x50 + u - 0x3A);   /* F1..F10 */
    if (u == 0x44 || u == 0x49 || u == 0x4A)
        return 0x5F;                    /* F11, Insert, Home: Help */
    if (u == 0x4C)
        return 0x46;                    /* Delete */
    if (u >= 0x4F && u <= 0x63)
        return pad[u - 0x4F];
    if (u == 0x64)
        return 0x30;                    /* (the key left of Z) */
    if (u >= 0xE0 && u <= 0xE7)
        return mods[u - 0xE0];
    return 0xFF;
}

/* the USB keyboard's own joystick fire and mouse button (play.c gives a computer's keyboard only the arrows as
   pad bits): Right Ctrl = fire (the Amiga has one Ctrl: the left one), Page Up / Page Down = the mouse button */
static u8 hid_fire, hid_mouse;

void amiga_keys_from_hid(const u8 *usages, int n, u8 keys[16])
{
    for (int i = 0; i < 16; i++)
        keys[i] = 0;
    hid_fire = hid_mouse = 0;
    for (int i = 0; i < n; i++) {
        if (usages[i] == 0xE4) {
            hid_fire = 1;
            continue;
        }
        if (usages[i] == 0x4B || usages[i] == 0x4E) {
            hid_mouse = 1;
            continue;
        }
        u8 k = hid_key(usages[i]);
        if (k < 0x80)
            keys[k >> 3] |= (u8)(1u << (k & 7));
    }
}

/* a typed character: its key (| 0x80: with Shift) */
static u32 char_key(int ch)
{
    static const char plain[] = "`1234567890-=\\", shifted[] = "~!@#$%^&*()_+|";
    static const char qrow[] = "qwertyuiop[]", arow[] = "asdfghjkl;'", zrow[] = "zxcvbnm,./";
    static const char qrow_s[] = "QWERTYUIOP{}", arow_s[] = "ASDFGHJKL:\"", zrow_s[] = "ZXCVBNM<>?";
    for (int i = 0; plain[i]; i++) {
        if (ch == plain[i])
            return (u32)i;
        if (ch == shifted[i])
            return 0x80u | (u32)i;
    }
    for (int i = 0; qrow[i]; i++) {
        if (ch == qrow[i])
            return 0x10u + (u32)i;
        if (ch == qrow_s[i])
            return 0x90u + (u32)i;
    }
    for (int i = 0; arow[i]; i++) {
        if (ch == arow[i])
            return 0x20u + (u32)i;
        if (ch == arow_s[i])
            return 0xA0u + (u32)i;
    }
    for (int i = 0; zrow[i]; i++) {
        if (ch == zrow[i])
            return 0x31u + (u32)i;
        if (ch == zrow_s[i])
            return 0xB1u + (u32)i;
    }
    switch (ch) {
    case ' ': return 0x40;
    case '\n': case '\r': return 0x44;
    case 8: case 127: return 0x41;
    case '\t': return 0x42;
    case 27: return 0x45;
    default: return 0xFF;
    }
}

/* typed characters: each held 3 frames, then nothing for 3 */
static char queue[256];
static int q_head, q_tail, q_frame;

void amiga_type(const char *text)
{
    while (*text) {
        int next = (q_tail + 1) & 255;
        if (next == q_head)
            break;
        queue[q_tail] = *text++;
        q_tail = next;
    }
}

int amiga_typing(void)
{
    return (q_tail - q_head) & 255;
}

static void typed(u8 keys[16])
{
    if (q_head == q_tail)
        return;
    u32 k = char_key((u8)queue[q_head]);
    if (q_frame < 3 && k != 0xFF) {
        keys[(k & 0x7F) >> 3] |= (u8)(1u << (k & 7));
        if (k & 0x80)
            keys[0x60 >> 3] |= 1;       /* (Shift) */
    }
    if (++q_frame == 6) {
        q_frame = 0;
        q_head = (q_head + 1) & 255;
    }
}

/* ---- the outside ---- */
void amiga_frame(u8 pad, const u8 keys[16], u16 *picture)
{
    pic = picture;
    joy_fire = (pad & AMIGA_A) != 0 || hid_fire;
    mouse_button = (pad & AMIGA_B) != 0 || hid_mouse;
    joy_dir = pad;
    u8 now[16];
    for (int i = 0; i < 16; i++)
        now[i] = keys ? keys[i] : 0;
    typed(now);
    if (pad & AMIGA_START)
        now[0x44 >> 3] |= 1u << (0x44 & 7);     /* Return */
    if (pad & AMIGA_SELECT)
        now[0x40 >> 3] |= 1u << (0x40 & 7);     /* Space */
    for (u32 k = 0; k < 128; k++) {
        u32 bit = 1u << (k & 7), down = now[k >> 3] & bit;
        if (down != (held_was[k >> 3] & bit))
            key_event(down ? k : k | 0x80);
    }
    for (int i = 0; i < 16; i++)
        held_was[i] = now[i];
    lines = (R(BPLCON0) & 4) ? (lof ? 313 : 312) : 313;
    for (u32 line = 0; line < lines; line++)
        run_line(line);
    s32 len = (s32)(lines * LINE_CYCLES);
    /* Every time below goes back a frame's length each frame.  A CIA nobody touched and the keyboard's last
       code were only ever taken back: after 15113 frames (five minutes) they passed -2^31 and counted as far
       in the future -- no key code for the next five minutes, a CIA timer started then not counting.  So the
       CIAs are brought up to now first (the same count as later, in two steps), and the keyboard's time is
       kept no further back than it matters */
    for (int i = 0; i < 2; i++)
        cia_update(&cia[i], m.clock);
    m.clock -= len;
    if (cop.t < NEVER)
        cop.t -= len;
    if (blit_end < NEVER)
        blit_end -= len;
    if (disk.end < NEVER)
        disk.end -= len;
    for (int i = 0; i < 2; i++) {
        cia[i].last -= len;
        if (cia[i].next < NEVER)
            cia[i].next -= len;
    }
    for (int ch = 0; ch < 4; ch++)
        if (aud[ch].next < NEVER)
            aud[ch].next -= len;
    kb_time -= len;
    if (kb_time < -0x40000000)
        kb_time = -0x40000000;
    cycles_hi += (u32)len;
    lof = (R(BPLCON0) & 4) ? lof ^ 1 : 1;
    frames++;
}

static void chips_reset(void)
{
    for (int i = 0; i < 256; i++)
        reg[i] = 0;
    dmacon = intena = intreq = adkcon = 0;
    for (int i = 0; i < 2; i++) {
        struct cia *c = &cia[i];
        c->pra = c->prb = c->ddra = c->ddrb = c->sdr = c->icr = c->mask = c->cra = c->crb = 0;
        c->ta = c->tb = c->la = c->lb = 0xFFFF;
        c->tod = c->alarm = c->latch = c->latched = 0;
        c->tod_run = 1;
        c->last = m.clock;
        c->next = NEVER;
        c->bit = i ? 0x2000 : 0x0008;
    }
    cop.state = 0, cop.t = NEVER, cop.lc1 = cop.lc2 = cop.pc = 0;
    blit_busy = 0, blit_end = NEVER, bzero = 0;
    for (int ch = 0; ch < 4; ch++)
        aud[ch].next = NEVER, aud[ch].on = 0, aud[ch].len = aud[ch].per = 0;
    disk.motor = 0, disk.dma = 0, disk.end = NEVER, disk.len_was = 0, disk.sel = 0;
    for (int i = 0; i < 6; i++)
        bplpt[i] = 0;
    for (int i = 0; i < 8; i++) {
        spr[i].state = 0, spr[i].armed = 0, spr[i].pt = 0;
        spr[i].pos = spr[i].ctl = spr[i].data = spr[i].datb = 0;
    }
    for (int i = 0; i < 32; i++)
        pal[i] = 0;
    kq_head = kq_tail = 0, kb_wait = 0;
    overlay = -1;
    set_overlay();
    update_ipl();
}

static void cpu_reset_line(void)        /* RESET: the chips back to their power-on state (the ROM over 0 again) */
{
#ifndef AMIGA_CPU_TEST
    chips_reset();
#endif
}

void amiga_reset(void)
{
    row_pic = 0;
    for (u32 i = 0; i < 0x40000; i++)
        chip[i] = slow[i] = 0;
    m.clock = 0;
    chips_reset();
    disk.cyl = 0, disk.side = 0, disk.changed = 1, disk.track_cyl = -1;
    for (int i = 0; i < 16; i++)
        held_was[i] = 0;
    q_head = q_tail = q_frame = 0;
    frames = 0, cycles_hi = 0, lof = 1, lines = 313, kb_time = 0;
    cpu_reset();
}

static void tables(void)
{
    for (u32 v = 0; v < 4096; v++) {
        u32 r = (v >> 8) & 15, g = (v >> 4) & 15, b = v & 15;
        c15[v] = (u16)((r << 1 | r >> 3) | (g << 1 | g >> 3) << 5 | (b << 1 | b >> 3) << 10);
    }
    for (u32 n = 0; n < 16; n++)
        nibble[n] = (n >> 3 & 1) | (n >> 2 & 1) << 8 | (n >> 1 & 1) << 16 | (n & 1) << 24;
}

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

static void copy_rom(u16 *to, const u8 *from, u32 size)
{
    for (u32 i = 0; i < size / 2; i++)
        to[i] = (u16)(from[2 * i] << 8 | from[2 * i + 1]);
}

const char *amiga_load(const u8 *kick, u32 kick_size, const u8 *ext_rom, u32 ext_size, const u8 *adf, u32 adf_size,
                       const char *name)
{
    static int ready;
    (void)name;
    if (!ready) {
        cpu_init();
        tables();
        ready = 1;
    }
    if (!kick)
        return "no Kickstart ROM (kick.rom: 256 or 512 KB; or AROS's aros-rom.bin with aros-ext.bin)";
    if ((kick_size != 0x40000 && kick_size != 0x80000) || kick[0] != 0x11 || (kick[1] != 0x11 && kick[1] != 0x14))
        return "not a Kickstart ROM (256 or 512 KB, starting 11 11 or 11 14)";
    if (ext_rom && ext_size != 0x80000)
        return "the extension ROM is not 512 KB";
    if (adf && adf_size != 901120)
        return "not an .adf floppy image (901120 bytes: 80 cylinders, 2 sides, 11 sectors of 512 bytes)";
    copy_rom(rom, kick, kick_size);
    rom_bytes = kick_size;
    ext_bytes = 0;
    if (ext_rom) {
        copy_rom(ext, ext_rom, ext_size);
        ext_bytes = ext_size;
    }
    disk.adf = adf;
    disk.size = adf ? adf_size : 0;
    char *p = info;
    *p = 0;
    if (kick_size == 0x80000 && kick[0x18] == 'A' && kick[0x19] == 'R' && kick[0x1A] == 'O' && kick[0x1B] == 'S') {
        p = put(p, "AROS's ROM");
    } else {                            /* (its version and revision: the words at 12 and 14) */
        p = put(p, "Kickstart ");
        p = put_number(p, (u32)(kick[12] << 8 | kick[13]));
        p = put(p, ".");
        p = put_number(p, (u32)(kick[14] << 8 | kick[15]));
    }
    p = put(p, ext_rom ? " (+ its extension)" : "");
    p = put(p, adf ? ", a floppy in DF0:" : ", no floppy");
    amiga_reset();
    return 0;
}

const char *amiga_info(void)
{
    return info;
}

u8 *amiga_save_ram(u32 *size, int *battery)
{
    *size = 0;
    *battery = 0;
    return (u8 *)chip;
}

int amiga_save_changed(void)
{
    return 0;
}

u32 amiga_cpu_cycles(void)
{
    return cycles_hi + (u32)m.clock;
}

int amiga_jammed(void)
{
    return m.stopped && m.im == 7;
}

u8 amiga_peek(u32 a)
{
    const u8 *p = rmap[(a >> 16) & 0xFF];
    return p ? p[(a & 0xFFFF) ^ 1] : 0;
}

u16 amiga_custom(u32 offset)
{
    return reg[(offset & 0x1FE) >> 1];
}

void amiga_registers(u32 r[18])
{
    for (int i = 0; i < 16; i++)
        r[i] = m.r[i];
    r[16] = m.pc;
    r[17] = get_sr();
}

#ifdef AMIGA_CPU_TEST
/* ---- the 68000 alone over 16 MB of flat memory (the single-step tests) ---- */
static u8 test_memory[16 << 20];

void amiga_test_init(void)
{
    cpu_init();
    for (int i = 0; i < 256; i++)
        rmap[i] = wmap[i] = test_memory + (i << 16);
}

void amiga_test_poke(u32 a, u8 v)
{
    test_memory[(a & 0xFFFFFF) ^ 1] = v;
}

u8 amiga_test_peek(u32 a)
{
    return test_memory[(a & 0xFFFFFF) ^ 1];
}

/* r: D0-D7, A0-A6, USP, SSP, SR, PC (the address of the instruction) */
void amiga_test_registers(int set, u32 r[19])
{
    if (set) {
        for (int i = 0; i < 15; i++)
            m.r[i] = r[i];
        m.s = (r[17] >> 13) & 1;
        m.r[15] = m.s ? r[16] : r[15];
        m.other_sp = m.s ? r[15] : r[16];
        set_ccr(r[17]);
        m.t = (r[17] >> 15) & 1;
        m.im = (r[17] >> 8) & 7;
        m.pc = r[18];
        m.ipl = 0, m.stopped = 0, m.fault = 0;
        check_pending();
        return;
    }
    for (int i = 0; i < 15; i++)
        r[i] = m.r[i];
    r[15] = m.s ? m.other_sp : m.r[15];
    r[16] = m.s ? m.r[15] : m.other_sp;
    r[17] = get_sr();
    r[18] = m.pc;
}

int amiga_test_step(void)
{
    m.clock = 0;
    m.pending = 0;
    m.op = fetch();
    handler[optab[m.op]]();
    if (m.fault)
        address_error();
    return m.clock;
}
#endif

