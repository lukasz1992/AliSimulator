/* A Commodore 64 (PAL): see c64.h.

   The frame: 312 raster lines of 63 cycles (19656 cycles, at 985248 Hz).
   The 6510 runs one instruction at a time; the machine around it is moved
   on at three points of each line:
   - cycle 0: the raster compare ($D011 bit 7, $D012) raises the VIC's
     interrupt; the sprites shown on this line take their fetches' cycles
     (2 a sprite and 3 to stop the CPU: the 6569 fetches sprites 0-2 at the
     end of the line before, 3-7 at its start -- counted here in one go);
   - cycle 12: a bad line -- a line of the display window (raster $30-$F7)
     whose low 3 bits are YSCROLL, the display enabled in line $30 -- reads
     the 40 characters and their colours of the next text row and holds the
     CPU from cycle 15 to 54 (40 cycles; it then has 23 of the line's 63);
   - cycle 36: the line's pixels are made, from the registers and memory as
     they are then: so a register a program changes in the first half of a
     line shows on that line, one changed later on the next (the 6569 draws
     the 320 pixels from cycle 17 to 56; the middle of that is the closest
     one point can be).  The graphics follow Christian Bauer's description
     of the chip: the video counter (VC, VCBASE), the row counter (RC), the
     display and idle states (idle: the byte at $3FFF, in black), the 8
     display modes (the 3 invalid ones black), XSCROLL, the 38 / 24
     column-and-row windows and the border's flip-flops (so a program that
     switches to 24 rows on the right line opens the top and bottom border;
     the side border stays closed), the 8 sprites with their DMA, MCBASE
     and Y expansion flip-flops (21 lines each, 42 expanded), X expansion,
     multicolour, the priority bits (a sprite behind the background hides
     the sprites under it all the same, as on the chip) and both kinds of
     collision with their interrupts;
   - cycle 63: the sprites' DMA for the next line (Y compared with the
     raster), VCBASE and RC at the line's end, the vertical border.
   The picture: raster lines 31..270 (the 200 lines of the 25 rows start at
   51), each from X 8 to 359 (the 320 pixels start at 24).

   The CIAs are counted lazily: a timer's counter is worked out from the
   cycles since it was last looked at, whenever the program reads or writes
   the chip, and the 6510 stops at the cycle a timer would raise an enabled
   interrupt (so raster-timed and timer-timed code both get their
   interrupts at the right cycle, give or take an instruction).  A timer
   with latch N underflows every N + 1 cycles (one-shot: once); timer B
   counts cycles or timer A's underflows; a timer counts from the second
   cycle after its start (the third with a force load) and for two cycles
   after its stop, as the chip's pipeline does.  CIA 1 reads the keyboard (port
   A drives the columns low, port B reads the rows) and the joysticks (port
   2 on port A, port 1 on port B, low when pushed); CIA 2's port A picks the
   VIC's 16 KB bank (inverted) and its interrupt is the NMI, as is RESTORE.
   The time of day counts at 50 Hz (the frames).

   Memory: 64 KB of RAM, the 6510's port at $00/$01 choosing what the
   upper 24 KB show (LORAM, HIRAM, CHAREN: BASIC at $A000, the KERNAL at
   $E000, the I/O or the character ROM at $D000 -- writes always go to the
   RAM below a ROM), 1K x 4 bits of colour RAM (its upper 4 bits read what
   the VIC last fetched).  The VIC sees its bank, with the character ROM at
   $1000-$1FFF of banks 0 and 2.

   The SID makes no sound; voice 3's oscillator and envelope run (counted
   lazily like the timers) so $D41B (OSC3: the noise generator many games
   take random numbers from) and $D41C (ENV3) read as on the chip; the
   paddles read $FF.

   Files: the KERNAL's LOAD ($F4A5, reached through $FFD5 and the $0330
   vector) is caught when a program jumps there: a LOAD from device 8 (or
   1, the tape) finds the file's programs by name and puts them in memory
   at once, as the KERNAL would (secondary address 0: at X/Y, else at the
   file's own address), and returns.  The KERNAL's serial bus routines are
   caught too, for device 8 (see 'A 1541' below): OPEN, CHKIN, CHRIN and
   the command channel work, so loaders that read files through the KERNAL
   do.  A game's own fast loader (code sent to the 1541) finds no drive
   that runs it -- most then fall back to the KERNAL.

   Cartridges (.crt, VICE's / CCS64's files): the chips' 8 KB banks for
   ROML ($8000) and ROMH ($A000, or $E000 in Ultimax mode), the EXROM and
   GAME lines and the PLA's configurations with them: 8K (EXROM low: ROML
   over the RAM at $8000 with LORAM and HIRAM), 16K (both low: ROMH in
   BASIC's place with HIRAM; the character ROM not with HIRAM low),
   Ultimax (GAME low: RAM at $0000-$0FFF, ROML, the I/O, ROMH at $E000
   whatever the 6510's port says, nothing elsewhere -- reads there give the
   bus's last byte, writes go nowhere; the VIC sees ROMH's upper 4 KB at
   $3000-$3FFF of each bank and no character ROM).  ROM reads come from the
   bank, writes go to the RAM below (in Ultimax mode: nowhere).  The banking
   of the game cartridges' kinds is done at I/O 1 and 2 ($DE00-$DFFF): see
   cart_read / cart_write.  The machine then starts in the cartridge (its
   CBM80 at $8004, or its own reset vector in Ultimax mode), at bank 0. */
#include "c64.h"

#define LINE_CYCLES  63u
#define LINES        312u
#define FRAME_CYCLES (LINE_CYCLES * LINES)      /* 19656 */
#define TOP_LINE     31u                        /* the picture's first raster line */
#define NEVER        0xFFFFFFFFu
#define LOAD_ENTRY   0xF4A5                     /* the KERNAL's LOAD, behind the $0330 vector */
#define SERIAL_FIRST 0xED09                     /* its serial bus routines: TALK $ED09 ... ACPTR $EE13 */
#define WAIT_FIRST   0xE5CD                     /* its screen editor's loop waiting for a key ... */
#define WAIT_LAST    0xE5D5                     /* ... to here */

#define FC 0x01
#define FZ 0x02
#define FI 0x04
#define FD 0x08
#define FB 0x10
#define FU 0x20
#define FV 0x40
#define FN 0x80

enum { IMP, IMM, ZP, ZPX, ZPY, ABS, ABX, ABY, IND, IZX, IZY, REL };

/* Pepto's colodore palette */
const u8 c64_rgb[16][3] = {
    {0x00, 0x00, 0x00}, {0xFF, 0xFF, 0xFF}, {0x81, 0x33, 0x38}, {0x75, 0xCE, 0xC8},
    {0x8E, 0x3C, 0x97}, {0x56, 0xAC, 0x4D}, {0x2E, 0x2C, 0x9B}, {0xED, 0xF1, 0x71},
    {0x8E, 0x50, 0x29}, {0x55, 0x38, 0x00}, {0xC4, 0x6C, 0x71}, {0x4A, 0x4A, 0x4A},
    {0x7B, 0x7B, 0x7B}, {0xA9, 0xFF, 0x9F}, {0x70, 0x6D, 0xEB}, {0xB2, 0xB2, 0xB2},
};

static u16 pal[16];                     /* the colours as 15-bit ones */

/* ---- the machine's state ---- */
static struct {
    u8 a, x, y, s, p;
    u16 pc;
    u8 jam;
    u8 delay;                           /* CLI / PLP just cleared I: an interrupt waits one more instruction */
    u32 cyc;                            /* cycles since the frame's start */
} cpu;
static u32 now;                         /* the cycle of the bus access being made (I/O looks at it) */
static u32 frames_done;
static u8 irq_pins;                     /* bit 0 the VIC's, bit 1 CIA 1's */
static u8 nmi_line, nmi_pending, restore;
static u32 next_event;                  /* the cycle a CIA next raises its interrupt pin (0: work it out again) */

static u8 ram[65536], colram[1024];
static u8 *stack = ram + 0x100;
static const u8 *kernal, *basic, *chargen;
static const u8 *rdp[256];              /* each page's bytes for reads; 0: I/O */
static u8 *wrp[256];                    /* for writes; 0: I/O or the 6510's port */
static u8 port_dir, port_out;           /* $00 and $01 */
static u8 open_bus;                     /* the last byte the VIC fetched */
static u8 trap_ok, wait_ok;             /* the KERNAL is the one whose LOAD and key loop are known */

/* a cartridge */
#define CART_BANKS 128
static struct {
    u8 on;                              /* one is plugged in */
    u8 type;                            /* its hardware type (the .crt's numbers) */
    u8 sub;                             /* the .crt's hardware revision */
    u8 exrom, game;                     /* its lines now (1: high, not asserted) ... */
    u8 exrom0, game0;                   /* ... and at the reset */
    u8 bank, mask, locked;              /* the bank; the bank numbers' mask; its register no longer written */
    u8 lo_on, hi_at;                    /* ROML is mapped at $8000; ROMH at this page ($A0, $E0; 0: not) */
    const u8 *roml, *romh;              /* the bank's 8 KB at $8000 and $A000 / $E000 (0: none) */
    const u8 *lo[CART_BANKS], *hi[CART_BANKS];
    u8 ram[256];                        /* EasyFlash's, at $DF00 */
    u32 size;                           /* its ROM's bytes */
    u8 name[32];
} cart;
static u8 ultimax;                      /* the cartridge's Ultimax mode is on */
static u8 nowhere[256];                 /* where writes to nothing go */
static u8 short_chip[4][0x2000];        /* chips of less than 8 KB, repeated to 8 KB */

struct cia {
    u8 pra, prb, ddra, ddrb, cra, crb;
    u8 icr, mask, sdr, pb67;            /* the interrupt flags (bits 0-4) and which reach the pin; PB6/PB7 */
    u16 ta, tb, la, lb;                 /* the timers' counters and latches */
    u8 tod[4], alarm[4], hold[4];       /* tenths, seconds, minutes, hours (BCD; hours 1-12, bit 7 PM) */
    u8 held, stopped, ticks;
    u8 wait_a, wait_b;                  /* the cycles before a timer just started counts */
    u8 tail_a, tail_b;                  /* the cycles a timer just stopped still counts */
    u32 last;                           /* the cycle the timers were counted to */
};
static struct cia cia[2];
static u8 keys[8];                      /* the keys held: [column] bit row */
static u8 joy[2];                       /* ports 1 and 2: bits 0-4 up down left right fire (1: pushed) */

static struct {
    u8 reg[32];
    u32 acc, noise, last;               /* voice 3: its 24-bit oscillator, the noise generator, counted to */
    u8 env, state;                      /* the envelope: its level, 0 attack 1 decay / sustain 2 release */
    u32 env_cycles;
} sid;

static struct {
    u8 reg[0x40];
    u16 cmp;                            /* the raster compare line */
    u8 irr, imr, ss, sb;                /* interrupt flags and mask; the collision registers */
    u16 vc, vcbase;
    u8 rc, display, bad, den30, vborder;
    u8 dma, expand, shown;              /* sprites (a bit each): DMA on, Y expansion flip-flop, shown this line */
    u8 mcbase[8], mc[8];
    u32 data[8];                        /* each sprite's 24 pixels for its next line */
    u8 stall;                           /* the cycles the next line's sprite fetches take from the 6510 */
    u8 cbuf[40], colbuf[40];            /* what the last bad line read: the 40 screen codes, their colours */
} vic;
static const u8 *vmem[16];              /* the VIC's 16 KB, 1 KB a slot */
#define VREAD(a) (vmem[((a) >> 10) & 15][(a) & 0x3FF])

static char info[96];

/* ---- text ---- */
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

static char *put_name(char *p, const u8 *name, int n)  /* PETSCII to ASCII, as far as they agree */
{
    for (int i = 0; i < n && p < info + sizeof(info) - 1; i++) {
        u8 c = name[i];
        if (c >= 0xC1 && c <= 0xDA)
            c = (u8)(c - 0x80);                 /* (shifted letters) */
        *p++ = (char)(c >= 0x20 && c < 0x7F ? c : '?');
    }
    *p = 0;
    return p;
}

/* ---- the keyboard ---- */
#define K(col, row) ((col) * 8 + (row))
#define SH   0x40                       /* with SHIFT */
#define NONE 0xFF
enum {
    K_DEL = K(0, 0), K_RETURN = K(0, 1), K_RIGHT = K(0, 2), K_F7 = K(0, 3), K_F1 = K(0, 4), K_F3 = K(0, 5),
    K_F5 = K(0, 6), K_DOWN = K(0, 7), K_LSHIFT = K(1, 7), K_PLUS = K(5, 0), K_MINUS = K(5, 3), K_DOT = K(5, 4),
    K_COLON = K(5, 5), K_AT = K(5, 6), K_COMMA = K(5, 7), K_POUND = K(6, 0), K_STAR = K(6, 1), K_SEMI = K(6, 2),
    K_HOME = K(6, 3), K_RSHIFT = K(6, 4), K_EQUAL = K(6, 5), K_UPARROW = K(6, 6), K_SLASH = K(6, 7),
    K_LEFTARROW = K(7, 1), K_CTRL = K(7, 2), K_SPACE = K(7, 4), K_CBM = K(7, 5), K_STOP = K(7, 7)
};
static const u8 letters[26] = {K(1, 2), K(3, 4), K(2, 4), K(2, 2), K(1, 6), K(2, 5), K(3, 2), K(3, 5), K(4, 1),
                               K(4, 2), K(4, 5), K(5, 2), K(4, 4), K(4, 7), K(4, 6), K(5, 1), K(7, 6), K(2, 1),
                               K(1, 5), K(2, 6), K(3, 6), K(3, 7), K(1, 1), K(2, 7), K(3, 1), K(1, 4)};
static const u8 digits[10] = {K(4, 3), K(7, 0), K(7, 3), K(1, 0), K(1, 3), K(2, 0), K(2, 3), K(3, 0), K(3, 3),
                              K(4, 0)};

/* a typed character's key (| SH); letters of either case are the unshifted ones (what BASIC wants) */
static u8 char_key(int ch)
{
    static const struct {
        char ch;
        u8 key;
    } symbols[] = {
        {' ', K_SPACE}, {'\r', K_RETURN}, {'\n', K_RETURN}, {8, K_DEL}, {127, K_DEL}, {'\t', K_STOP},
        {'!', SH | K(7, 0)}, {'"', SH | K(7, 3)}, {'#', SH | K(1, 0)}, {'$', SH | K(1, 3)}, {'%', SH | K(2, 0)},
        {'&', SH | K(2, 3)}, {'\'', SH | K(3, 0)}, {'(', SH | K(3, 3)}, {')', SH | K(4, 0)}, {'+', K_PLUS},
        {'-', K_MINUS}, {'*', K_STAR}, {'/', K_SLASH}, {'=', K_EQUAL}, {':', K_COLON}, {';', K_SEMI},
        {',', K_COMMA}, {'.', K_DOT}, {'@', K_AT}, {'<', SH | K_COMMA}, {'>', SH | K_DOT}, {'?', SH | K_SLASH},
        {'[', SH | K_COLON}, {']', SH | K_SEMI}, {'^', K_UPARROW}, {'_', K_LEFTARROW}, {'\\', K_POUND},
    };
    if (ch >= 'a' && ch <= 'z')
        return letters[ch - 'a'];
    if (ch >= 'A' && ch <= 'Z')
        return letters[ch - 'A'];
    if (ch >= '0' && ch <= '9')
        return digits[ch - '0'];
    for (unsigned i = 0; i < sizeof(symbols) / sizeof(symbols[0]); i++)
        if (symbols[i].ch == ch)
            return symbols[i].key;
    return NONE;
}

static void press(u8 matrix[8], u8 k)
{
    if (k == NONE)
        return;
    matrix[(k & 0x3F) >> 3] |= (u8)(1u << (k & 7));
    if (k & SH)
        matrix[1] |= 0x80;              /* (LEFT SHIFT) */
}

static u8 hid_joy, hid_restore, hid_swap, f9_was, swapped;

void c64_keys_from_hid(const u8 *usages, int n, u8 matrix[8])
{
    /* - = [ ] \ (non-US #) ; ' ` , . / */
    static const u8 punct[12] = {K_PLUS, K_MINUS, K_AT, K_STAR, K_EQUAL, K_EQUAL, K_COLON, K_SEMI, K_LEFTARROW,
                                 K_COMMA, K_DOT, K_SLASH};
    static const u8 fkeys[8] = {K_F1, SH | K_F1, K_F3, SH | K_F3, K_F5, SH | K_F5, K_F7, SH | K_F7};
    /* the keypad 1-9: the joystick's directions (up 1, down 2, left 4, right 8) */
    static const u8 pad_dir[9] = {2 | 4, 2, 2 | 8, 4, 0x10, 8, 1 | 4, 1, 1 | 8};
    int f9 = 0;
    for (int i = 0; i < 8; i++)
        matrix[i] = 0;
    hid_joy = 0;
    hid_restore = 0;
    for (int i = 0; i < n; i++) {
        u8 u = usages[i];
        if (u >= 0x04 && u <= 0x1D)
            press(matrix, letters[u - 0x04]);
        else if (u >= 0x1E && u <= 0x27)
            press(matrix, digits[u == 0x27 ? 0 : u - 0x1D]);
        else if (u >= 0x2D && u <= 0x38)
            press(matrix, punct[u - 0x2D]);
        else if (u >= 0x3A && u <= 0x41)
            press(matrix, fkeys[u - 0x3A]);
        else if (u >= 0x59 && u <= 0x61)
            hid_joy |= pad_dir[u - 0x59];
        else
            switch (u) {
            case 0x28: case 0x58: press(matrix, K_RETURN); break;
            case 0x2A: case 0x4C: press(matrix, K_DEL); break;
            case 0x2B: press(matrix, K_STOP); break;
            case 0x2C: press(matrix, K_SPACE); break;
            case 0x42: f9 = 1; break;
            case 0x49: press(matrix, K_POUND); break;
            case 0x4A: press(matrix, K_HOME); break;
            case 0x4B: hid_restore = 1; break;
            case 0x4D: press(matrix, K_UPARROW); break;
            case 0x4F: press(matrix, K_RIGHT); break;
            case 0x50: press(matrix, SH | K_RIGHT); break;
            case 0x51: press(matrix, K_DOWN); break;
            case 0x52: press(matrix, SH | K_DOWN); break;
            case 0x54: press(matrix, K_SLASH); break;
            case 0x55: press(matrix, K_STAR); break;
            case 0x56: press(matrix, K_MINUS); break;
            case 0x57: press(matrix, K_PLUS); break;
            case 0x62: hid_joy |= 0x10; break;
            case 0x63: press(matrix, K_DOT); break;
            case 0xE0: press(matrix, K_CTRL); break;
            case 0xE1: press(matrix, K_LSHIFT); break;
            case 0xE2: case 0xE3: case 0xE7: press(matrix, K_CBM); break;
            case 0xE4: case 0xE6: hid_joy |= 0x10; break;
            case 0xE5: press(matrix, K_RSHIFT); break;
            }
    }
    if (f9 && !f9_was)
        hid_swap ^= 1;
    f9_was = (u8)f9;
}

/* typed characters: each held 3 frames, then nothing for 3 (the KERNAL reads the keys 60 times a second) */
static char queue[256];
static int q_head, q_tail, q_frame;

void c64_type(const char *text)
{
    while (*text) {
        int next = (q_tail + 1) & 255;
        if (next == q_head)
            break;
        queue[q_tail] = *text++;
        q_tail = next;
    }
}

int c64_typing(void)
{
    return (q_tail - q_head) & 255;
}

static void typed(u8 matrix[8])
{
    if (q_head == q_tail)
        return;
    if (q_frame < 3)
        press(matrix, char_key(queue[q_head]));
    if (++q_frame == 6) {
        q_frame = 0;
        q_head = (q_head + 1) & 255;
    }
}

/* ---- interrupts ---- */
static void pins(void)
{
    u8 line;
    irq_pins = (u8)(((vic.irr & vic.imr & 0x0F) ? 1 : 0) | ((cia[0].icr & cia[0].mask & 0x1F) ? 2 : 0));
    line = (u8)((cia[1].icr & cia[1].mask & 0x1F) || restore);
    if (line && !nmi_line)
        nmi_pending = 1;                /* (the NMI is taken on the pin's edge) */
    nmi_line = line;
}

/* ---- the CIAs ---- */
static void vic_bank(void);

/* n counts of a counter at `count` reloading from `latch`: the underflows, the counter after them */
static u32 count_down(u16 *count, u16 latch, u32 n, int one_shot)
{
    u32 r, p;
    if (n <= *count) {
        *count = (u16)(*count - n);
        return 0;
    }
    if (one_shot) {
        *count = latch;
        return 1;
    }
    r = n - *count - 1;
    p = (u32)latch + 1;
    *count = (u16)(latch - r % p);
    return 1 + r / p;
}

static void cia_count(struct cia *c, u32 t)
{
    u32 d = t - c->last, ua = 0, ub, n = 0, da = d, db = d;
    if ((s32)d <= 0)
        return;
    c->last = t;
    if (c->wait_a) {                            /* (a timer counts from the second cycle after its start) */
        u32 w = c->wait_a < d ? c->wait_a : d;
        c->wait_a = (u8)(c->wait_a - w);
        da -= w;
    }
    if (c->wait_b) {
        u32 w = c->wait_b < d ? c->wait_b : d;
        c->wait_b = (u8)(c->wait_b - w);
        db -= w;
    }
    if (!(c->cra & 0x01)) {                     /* (stopped: it counts 2 cycles more) */
        da = c->tail_a < da ? c->tail_a : da;
        c->tail_a = (u8)(c->tail_a - da);
    }
    if (!(c->crb & 0x01)) {
        db = c->tail_b < db ? c->tail_b : db;
        c->tail_b = (u8)(c->tail_b - db);
    }
    if (!(c->cra & 0x20) && da) {               /* timer A counting the clock */
        ua = count_down(&c->ta, c->la, da, c->cra & 0x08);
        if (ua) {
            c->icr |= 0x01;
            if (c->cra & 0x08)
                c->cra &= (u8)~0x01;            /* (one-shot: it stops) */
            if (ua & 1)
                c->pb67 ^= 0x40;
        }
    }
    if ((c->crb & 0x01) || db) {
        switch (c->crb & 0x60) {
        case 0x00: n = db; break;               /* the clock */
        case 0x40: case 0x60: n = ua; break;    /* timer A's underflows */
        default: n = 0; break;                  /* (CNT's edges: none come) */
        }
        if (n) {
            ub = count_down(&c->tb, c->lb, n, c->crb & 0x08);
            if (ub) {
                c->icr |= 0x02;
                if (c->crb & 0x08)
                    c->crb &= (u8)~0x01;
                if (ub & 1)
                    c->pb67 ^= 0x80;
            }
        }
    }
}

/* the cycle the CIA's pin next goes active by itself (a timer's underflow), or NEVER */
static u32 cia_next(const struct cia *c)
{
    u32 t = NEVER;
    if (c->icr & c->mask & 0x1F)
        return NEVER;                           /* (active already: nothing changes until the program reads) */
    if ((c->mask & 1) && (c->cra & 0x21) == 0x01)
        t = c->last + c->wait_a + c->ta + 1;
    if ((c->mask & 2) && (c->crb & 0x01)) {
        if (!(c->crb & 0x60)) {
            if (c->last + c->wait_b + c->tb + 1 < t)
                t = c->last + c->wait_b + c->tb + 1;
        } else if ((c->crb & 0x40) && (c->cra & 0x21) == 0x01 && (!(c->cra & 0x08) || !c->tb)) {
            u32 more = (u32)c->tb * ((u32)c->la + 1), first = c->last + c->wait_a + c->ta + 1;
            if (c->tb && more / c->tb != (u32)c->la + 1)
                more = 0x7FFFFFFF;              /* (too far to matter) */
            if (more < 0x7FFFFFFF && first + more < t)
                t = first + more;
        }
    }
    return t;
}

static void events(void)
{
    u32 a, b;
    cia_count(&cia[0], cpu.cyc);
    cia_count(&cia[1], cpu.cyc);
    pins();
    a = cia_next(&cia[0]);
    b = cia_next(&cia[1]);
    next_event = a < b ? a : b;
}

static u8 bcd_up(u8 v)
{
    v++;
    if ((v & 0x0F) == 0x0A)
        v = (u8)((v & 0xF0) + 0x10);
    return v;
}

static void tod_tick(struct cia *c)     /* a tick of the mains (50 Hz: once a frame) */
{
    u8 *t = c->tod;
    if (++c->ticks < ((c->cra & 0x80) ? 5 : 6))
        return;
    c->ticks = 0;
    if (c->stopped)
        return;
    if (++t[0] >= 10) {
        t[0] = 0;
        t[1] = bcd_up(t[1]);
        if (t[1] >= 0x60) {
            t[1] = 0;
            t[2] = bcd_up(t[2]);
            if (t[2] >= 0x60) {
                u8 h = t[3] & 0x1F, pm = t[3] & 0x80;
                t[2] = 0;
                if (h == 0x11)
                    pm ^= 0x80;
                h = h == 0x12 ? 1 : bcd_up(h);
                t[3] = (u8)(h | pm);
            }
        }
    }
    if (t[0] == c->alarm[0] && t[1] == c->alarm[1] && t[2] == c->alarm[2] && t[3] == c->alarm[3]) {
        c->icr |= 0x04;
        pins();
    }
}

static u8 cia_read(int n, u32 r)
{
    struct cia *c = &cia[n];
    u8 v;
    cia_count(c, now);
    next_event = 0;
    switch (r) {
    case 0x0:
        v = (u8)(c->pra | ~c->ddra);
        if (!n) {                               /* the joystick in port 2; keys on rows driven low by port B */
            u8 rows = (u8)(c->ddrb & ~c->prb);
            v &= (u8)~joy[1];
            if (rows)
                for (int k = 0; k < 8; k++)
                    if (keys[k] & rows)
                        v &= (u8)~(1u << k);
        } else {                                /* the serial bus's CLK and DATA in (bits 6, 7): low only
                                                   while the C64 itself pulls them (bits 4, 5 out) */
            u8 out = (u8)(c->pra & c->ddra);
            if (out & 0x10)
                v &= (u8)~0x40;
            if (out & 0x20)
                v &= (u8)~0x80;
        }
        return v;
    case 0x1:
        v = (u8)(c->prb | ~c->ddrb);
        if (!n) {                               /* the keys of the columns port A drives low; joystick 1 */
            u8 cols = (u8)(c->ddra & ~c->pra);
            for (int k = 0; k < 8; k++)
                if (cols & (1u << k))
                    v &= (u8)~keys[k];
            v &= (u8)~joy[0];
        }
        if (c->cra & 0x02)                      /* PB6 / PB7: the timers' outputs (toggle mode; a pulse
                                                   lasts one cycle and is not seen) */
            v = (u8)((v & ~0x40) | ((c->cra & 0x04) ? (c->pb67 & 0x40) : 0));
        if (c->crb & 0x02)
            v = (u8)((v & ~0x80) | ((c->crb & 0x04) ? (c->pb67 & 0x80) : 0));
        return v;
    case 0x2: return c->ddra;
    case 0x3: return c->ddrb;
    case 0x4: return (u8)c->ta;
    case 0x5: return (u8)(c->ta >> 8);
    case 0x6: return (u8)c->tb;
    case 0x7: return (u8)(c->tb >> 8);
    case 0x8:                                   /* (reading the tenths frees the latched time) */
        v = c->held ? c->hold[0] : c->tod[0];
        c->held = 0;
        return v;
    case 0x9: case 0xA:
        return c->held ? c->hold[r - 8] : c->tod[r - 8];
    case 0xB:                                   /* (reading the hours latches the time) */
        if (!c->held)
            for (int i = 0; i < 4; i++)
                c->hold[i] = c->tod[i];
        c->held = 1;
        return c->hold[3];
    case 0xC: return c->sdr;
    case 0xD:
        v = (u8)(c->icr | ((c->icr & c->mask & 0x1F) ? 0x80 : 0));
        c->icr = 0;
        pins();
        return v;
    case 0xE: return (u8)(c->cra & ~0x10);
    default: return (u8)(c->crb & ~0x10);
    }
}

static void cia_write(int n, u32 r, u8 v)
{
    static const u8 tod_bits[4] = {0x0F, 0x7F, 0x7F, 0x9F};
    struct cia *c = &cia[n];
    cia_count(c, now);
    next_event = 0;
    switch (r) {
    case 0x0: c->pra = v; if (n) vic_bank(); break;
    case 0x1: c->prb = v; break;
    case 0x2: c->ddra = v; if (n) vic_bank(); break;
    case 0x3: c->ddrb = v; break;
    case 0x4: c->la = (u16)((c->la & 0xFF00) | v); break;
    case 0x5:                                   /* (a stopped timer loads the latch with its high byte) */
        c->la = (u16)((c->la & 0x00FF) | v << 8);
        if (!(c->cra & 0x01))
            c->ta = c->la;
        break;
    case 0x6: c->lb = (u16)((c->lb & 0xFF00) | v); break;
    case 0x7:
        c->lb = (u16)((c->lb & 0x00FF) | v << 8);
        if (!(c->crb & 0x01))
            c->tb = c->lb;
        break;
    case 0x8: case 0x9: case 0xA: case 0xB:    /* the time (writing the hours stops it until the tenths), or
                                                   with CRB bit 7 the alarm */
        v &= tod_bits[r - 8];
        if (c->crb & 0x80) {
            c->alarm[r - 8] = v;
        } else {
            c->tod[r - 8] = v;
            c->stopped = r == 0xB ? 1 : r == 0x8 ? 0 : c->stopped;
        }
        break;
    case 0xC:
        c->sdr = v;
        if (c->cra & 0x40)
            c->icr |= 0x08;                     /* (sent at once) */
        pins();
        break;
    case 0xD:
        if (v & 0x80)
            c->mask |= v & 0x1F;
        else
            c->mask &= (u8)~v;
        pins();
        break;
    case 0xE:                                   /* (a timer counts from the second cycle after its start; a
                                                   force load costs it one more) */
        if ((v & 0x01) && !(c->cra & 0x01)) {
            c->pb67 |= 0x40;
            c->wait_a = 2;
        } else if (!(v & 0x01) && (c->cra & 0x01)) {
            c->tail_a = 2;
        }
        if (v & 0x10) {
            c->ta = c->la;
            if (v & 0x01)
                c->wait_a++;
        }
        c->cra = (u8)(v & ~0x10);
        break;
    default:
        if ((v & 0x01) && !(c->crb & 0x01)) {
            c->pb67 |= 0x80;
            c->wait_b = 2;
        } else if (!(v & 0x01) && (c->crb & 0x01)) {
            c->tail_b = 2;
        }
        if (v & 0x10) {
            c->tb = c->lb;
            if (v & 0x01)
                c->wait_b++;
        }
        c->crb = (u8)(v & ~0x10);
        break;
    }
}

/* ---- the SID: voice 3's oscillator and envelope ---- */
static void sid_run(u32 t)
{
    static const u16 rate[16] = {9, 32, 63, 95, 149, 220, 267, 313, 392, 977, 1954, 3126, 3907, 11720, 19532,
                                 31251};    /* cycles per envelope step */
    u32 d = t - sid.last, freq, a, adv, clocks;
    if ((s32)d <= 0)
        return;
    sid.last = t;
    if (d > 0x8000)
        d = 0x8000;
    freq = sid.reg[0x0E] | (u32)sid.reg[0x0F] << 8;
    if (sid.reg[0x12] & 0x08) {                 /* the test bit: the oscillator held at 0 */
        sid.acc = 0;
    } else {
        /* the noise generator is clocked when the oscillator's bit 19 goes high */
        a = sid.acc + 0x80000;
        adv = freq * d;
        clocks = ((a + adv) >> 20) - (a >> 20);
        sid.acc = (sid.acc + adv) & 0xFFFFFF;
        if (clocks > 1024)
            clocks = 1024 + (clocks & 15);
        while (clocks--)
            sid.noise = ((sid.noise << 1) | (((sid.noise >> 22) ^ (sid.noise >> 17)) & 1)) & 0x7FFFFF;
    }
    sid.env_cycles += d;
    for (;;) {
        u32 step, level = sid.env;
        if (sid.state == 0) {
            step = rate[sid.reg[0x13] >> 4];
        } else {
            u32 r = sid.state == 1 ? sid.reg[0x13] & 15 : sid.reg[0x14] & 15;
            u32 floor = sid.state == 1 ? (u32)(sid.reg[0x14] >> 4) * 17 : 0;
            if (level <= floor) {
                sid.env_cycles = 0;
                break;
            }
            step = rate[r] * (level >= 93 ? 1 : level >= 54 ? 2 : level >= 26 ? 4 : level >= 14 ? 8 : level >= 6 ?
                                                                                                     16 : 30);
        }
        if (sid.env_cycles < step)
            break;
        sid.env_cycles -= step;
        if (sid.state == 0) {
            if (++sid.env == 0xFF)
                sid.state = 1;
        } else {
            sid.env--;
        }
    }
}

static u8 sid_read(u32 r)
{
    u8 ctl = sid.reg[0x12], out = 0xFF;
    u32 n;
    switch (r) {
    case 0x19: case 0x1A:
        return 0xFF;                            /* the paddles (none) */
    case 0x1B:
        sid_run(now);
        if (!(ctl & 0xF0))
            return 0;
        if (ctl & 0x80) {
            n = sid.noise;
            out &= (u8)(((n >> 15) & 0x80) | ((n >> 14) & 0x40) | ((n >> 11) & 0x20) | ((n >> 9) & 0x10) |
                        ((n >> 8) & 0x08) | ((n >> 5) & 0x04) | ((n >> 3) & 0x02) | ((n >> 2) & 0x01));
        }
        if (ctl & 0x40)
            out &= ((sid.acc >> 12) >= (sid.reg[0x10] | (u32)(sid.reg[0x11] & 15) << 8) || (ctl & 0x08)) ? 0xFF : 0;
        if (ctl & 0x20)
            out &= (u8)(sid.acc >> 16);
        if (ctl & 0x10)
            out &= (u8)(((sid.acc & 0x800000) ? ~sid.acc : sid.acc) >> 15);
        return out;
    case 0x1C:
        sid_run(now);
        return sid.env;
    default:
        return 0;                               /* (write-only) */
    }
}

static void sid_write(u32 r, u8 v)
{
    if (r >= 0x0E && r <= 0x14)
        sid_run(now);
    if (r == 0x12) {
        if ((v & 1) && !(sid.reg[0x12] & 1)) {
            sid.state = 0;                      /* the gate opens: attack */
            sid.env_cycles = 0;
        } else if (!(v & 1) && (sid.reg[0x12] & 1)) {
            sid.state = 2;                      /* closes: release */
        }
        if (v & 0x08)
            sid.acc = 0;
    }
    sid.reg[r] = v;
}

/* ---- the VIC's registers ---- */
static void vic_bank(void)
{
    u32 base = (u32)(3 - ((cia[1].pra | ~cia[1].ddra) & 3)) << 14;
    for (int i = 0; i < 16; i++)
        vmem[i] = ram + base + ((u32)i << 10);
    if (!(base & 0x4000))
        for (int i = 4; i < 8; i++)
            vmem[i] = chargen + ((u32)(i - 4) << 10);
    if (ultimax) {                              /* (no character ROM; ROMH's upper 4 KB at $3000) */
        for (int i = 4; i < 8; i++)
            vmem[i] = ram + base + ((u32)i << 10);
        if (cart.romh)
            for (int i = 12; i < 16; i++)
                vmem[i] = cart.romh + ((u32)(i - 8) << 10);
    }
}

static u32 raster_now(void)
{
    u32 line = now / LINE_CYCLES;
    return line >= LINES ? line - LINES : line;
}

static void raster_compare(u16 cmp)
{
    u16 was = vic.cmp;
    vic.cmp = cmp;
    if (cmp != was && cmp == raster_now()) {    /* (set to the line it is on: at once) */
        vic.irr |= 1;
        pins();
    }
}

static u8 vic_read(u32 r)
{
    u8 v;
    switch (r) {
    case 0x11: return (u8)((vic.reg[0x11] & 0x7F) | ((raster_now() & 0x100) >> 1));
    case 0x12: return (u8)raster_now();
    case 0x13: case 0x14: return 0;            /* (no light pen) */
    case 0x16: return (u8)(vic.reg[0x16] | 0xC0);
    case 0x18: return (u8)(vic.reg[0x18] | 0x01);
    case 0x19: return (u8)(vic.irr | 0x70 | ((vic.irr & vic.imr & 0x0F) ? 0x80 : 0));
    case 0x1A: return (u8)(vic.imr | 0xF0);
    case 0x1E: v = vic.ss; vic.ss = 0; return v;
    case 0x1F: v = vic.sb; vic.sb = 0; return v;
    default:
        if (r >= 0x2F)
            return 0xFF;
        if (r >= 0x20)
            return (u8)(vic.reg[r] | 0xF0);
        return vic.reg[r];
    }
}

static void vic_write(u32 r, u8 v)
{
    if (r >= 0x2F)
        return;
    switch (r) {
    case 0x11:
        vic.reg[0x11] = v;
        raster_compare((u16)((vic.cmp & 0xFF) | (v & 0x80) << 1));
        break;
    case 0x12:
        raster_compare((u16)((vic.cmp & 0x100) | v));
        break;
    case 0x17:
        vic.reg[0x17] = v;
        vic.expand |= (u8)~v;                   /* (the flip-flop stays set while Y expansion is off) */
        break;
    case 0x19:
        vic.irr &= (u8)~v;
        pins();
        break;
    case 0x1A:
        vic.imr = v & 0x0F;
        pins();
        break;
    case 0x1E: case 0x1F:
        break;
    default:
        vic.reg[r] = v;
        break;
    }
}

/* ---- memory ---- */
static void map_cart(u32 lines);

static void map(void)
{
    u8 lines = (u8)(((port_out & port_dir) | ~port_dir) & 7);  /* (inputs read high) */
    int loram = lines & 1, hiram = lines & 2, charen = lines & 4;
    if (cart.on) {
        map_cart(lines);
        return;
    }
    for (int i = 0xA0; i < 0x100; i++) {
        rdp[i] = ram + (i << 8);
        wrp[i] = ram + (i << 8);
    }
    if (loram && hiram)
        for (int i = 0; i < 0x20; i++)
            rdp[0xA0 + i] = basic + (i << 8);
    if (hiram)
        for (int i = 0; i < 0x20; i++)
            rdp[0xE0 + i] = kernal + (i << 8);
    if (loram || hiram) {
        for (int i = 0; i < 0x10; i++) {
            if (charen)
                rdp[0xD0 + i] = wrp[0xD0 + i] = 0;
            else
                rdp[0xD0 + i] = chargen + (i << 8);
        }
    }
}

/* the cartridge's banks where the PLA has mapped them (0: nothing there -- the slow way, the bus's last byte) */
static void cart_pages(void)
{
    if (cart.lo_on)
        for (int i = 0; i < 0x20; i++)
            rdp[0x80 + i] = cart.roml ? cart.roml + (i << 8) : 0;
    if (cart.hi_at)
        for (int i = 0; i < 0x20; i++)
            rdp[cart.hi_at + i] = cart.romh ? cart.romh + (i << 8) : 0;
    if (ultimax)
        vic_bank();
}

/* the PLA with a cartridge: LORAM, HIRAM, CHAREN (`lines` bits 0-2) with EXROM and GAME */
static void map_cart(u32 lines)
{
    int loram = lines & 1, hiram = lines & 2, charen = lines & 4, was = ultimax;
    int mode = (cart.exrom ? 0 : 1) | (cart.game ? 0 : 2);     /* 0 none, 1 8K, 2 Ultimax, 3 16K */
    ultimax = mode == 2;
    cart.lo_on = cart.hi_at = 0;
    if (ultimax) {                              /* RAM to $0FFF, ROML, the I/O, ROMH; nothing else */
        for (int i = 0x10; i < 0x100; i++) {
            rdp[i] = 0;
            wrp[i] = nowhere;
        }
        for (int i = 0xD0; i < 0xE0; i++)
            wrp[i] = 0;
        cart.lo_on = 1;
        cart.hi_at = 0xE0;
    } else {
        for (int i = was ? 0x10 : 0x80; i < 0x100; i++) {     /* (below $8000: RAM but after Ultimax) */
            rdp[i] = ram + (i << 8);
            wrp[i] = ram + (i << 8);
        }
        if (loram && hiram && mode)
            cart.lo_on = 1;
        if (hiram && mode == 3)
            cart.hi_at = 0xA0;
        else if (loram && hiram)
            for (int i = 0; i < 0x20; i++)
                rdp[0xA0 + i] = basic + (i << 8);
        if (hiram)
            for (int i = 0; i < 0x20; i++)
                rdp[0xE0 + i] = kernal + (i << 8);
        if (loram || hiram)
            for (int i = 0; i < 0x10; i++) {
                if (charen)
                    rdp[0xD0 + i] = wrp[0xD0 + i] = 0;
                else if (hiram || mode != 3)
                    rdp[0xD0 + i] = chargen + (i << 8);
            }
    }
    cart_pages();
    if (was && !ultimax)
        vic_bank();
}

static void cart_switch(u32 bank)               /* bank n (of those there are) */
{
    bank &= cart.mask;
    cart.bank = (u8)bank;
    cart.roml = cart.lo[bank];
    cart.romh = cart.hi[bank];
    cart_pages();
}

static void cart_lines(u8 exrom, u8 game)       /* EXROM and GAME (1: high) */
{
    if (exrom == cart.exrom && game == cart.game)
        return;
    cart.exrom = exrom;
    cart.game = game;
    map();
}

/* I/O 1 ($DE00-$DEFF) and I/O 2 ($DF00-$DFFF) read: the bank switched by a read, what is there */
static u8 cart_read(u16 a)
{
    int io1 = a < 0xDF00;
    switch (cart.type) {
    case 3:                                     /* Final Cartridge III: its ROM's last 512 bytes */
        if (cart.roml)
            return cart.roml[a - 0xC000];
        break;
    case 4:                                     /* Simons' BASIC: a read of I/O 1 maps out ROMH (8K) */
        if (io1)
            cart_lines(0, 1);
        break;
    case 15:                                    /* C64 Game System: a read of I/O 1 is bank 0 */
        if (io1)
            cart_switch(0);
        break;
    case 17:                                    /* Dinamic: a read of $DE00 + n is bank n */
        if (io1)
            cart_switch(a & 0xFF);
        break;
    case 32:                                    /* EasyFlash: its RAM at I/O 2 */
        if (!io1)
            return cart.ram[a & 0xFF];
        break;
    }
    return open_bus;
}

static void cart_write(u16 a, u8 v)
{
    int io1 = a < 0xDF00;
    switch (cart.type) {
    case 3:                                     /* Final Cartridge III: $DFFF -- bit 7 hides the register,
                                                   5 GAME, 4 EXROM, 0-1 the 16 KB bank (6: its NMI, not done) */
        if (a == 0xDFFF && !cart.locked) {
            cart.locked = v >> 7;
            cart_switch(v & 3);
            cart_lines((v >> 4) & 1, (v >> 5) & 1);
        }
        break;
    case 4:                                     /* Simons' BASIC: a write to I/O 1 maps ROMH in (16K) */
        if (io1)
            cart_lines(0, 0);
        break;
    case 5:                                     /* Ocean type 1: $DE00 bits 0-5 the bank (bit 7 always set) */
        if (io1)
            cart_switch(v & 0x3F);
        break;
    case 7:                                     /* Fun Play, Power Play: $DE00 bits 3-5 the bank's 0-2, bit 0
                                                   its bit 3; $86 switches the cartridge off */
        if (io1) {
            if (v == 0x86) {
                cart_lines(1, 1);
            } else {
                cart_switch(((v >> 3) & 7) | (v & 1) << 3);
                cart_lines(0, 1);
            }
        }
        break;
    case 8:                                     /* Super Games: $DF00 bits 0-1 the 16 KB bank, bit 2 off, bit 3
                                                   no more writes until the reset */
        if (!io1 && !cart.locked) {
            cart_switch(v & 3);
            cart_lines((v >> 2) & 1, (v >> 2) & 1);
            cart.locked = (v >> 3) & 1;
        }
        break;
    case 15:                                    /* C64 Game System: a write to $DE00 + n is bank n */
        if (io1)
            cart_switch(a & 0xFF);
        break;
    case 19:                                    /* Magic Desk, Domark, HES Australia: $DE00 bits 0-6 the bank,
                                                   bit 7 switches it off */
        if (io1) {
            cart_switch(v & 0x7F);
            cart_lines(v >> 7, 1);
        }
        break;
    case 32:                                    /* EasyFlash: $DE00 the bank; $DE02 bit 2 GAME from bit 0 (else
                                                   from the boot jumper: low), bit 1 EXROM, bit 0 GAME (1: low);
                                                   its RAM at I/O 2 (writes to the flash: not done) */
        if (!io1)
            cart.ram[a & 0xFF] = v;
        else if (a & 2)
            cart_lines(!(v & 2), (v & 4) ? !(v & 1) : 0);
        else
            cart_switch(v & 0x3F);
        break;
    case 57:                                    /* RGCD: $DE00 bits 0-2 the bank, bit 3 off until the reset */
        if (io1 && !cart.locked) {
            cart_switch(cart.sub == 1 ? (v & 7) ^ 7 : v & 7);
            if (v & 8) {
                cart.locked = 1;
                cart_lines(1, 1);
            }
        }
        break;
    }
}

static void cart_reset(void)
{
    for (u32 i = 0; i < sizeof(cart.ram); i++)
        cart.ram[i] = 0;
    cart.locked = 0;
    cart.lo_on = cart.hi_at = 0;
    cart.exrom = cart.exrom0;
    cart.game = cart.game0;
    cart_switch(0);
}

/* $00 / $01: the port's direction and data.  The 6510 reads its port there (the RAM under it is the
   VIC's alone), so those two bytes of `ram` hold what a read of the port gives: the outputs as written,
   the inputs high -- but the cassette's motor (bit 5) and the unconnected bits 6 and 7 low */
static void port_write(u16 a, u8 v)
{
    if (a)
        port_out = v;
    else
        port_dir = v;
    ram[0] = port_dir;
    ram[1] = (u8)((port_out & port_dir) | (~port_dir & 0x17));
    map();
}

static u8 slow_read(u16 a)                      /* $D000-$DFFF with the I/O there (or nothing there) */
{
    if ((a >> 12) != 0xD)
        return open_bus;                        /* (a cartridge's Ultimax mode, or a bank it has not) */
    switch ((a >> 8) & 0x0F) {
    case 0x0: case 0x1: case 0x2: case 0x3: return vic_read(a & 0x3F);
    case 0x4: case 0x5: case 0x6: case 0x7: return sid_read(a & 0x1F);
    case 0x8: case 0x9: case 0xA: case 0xB: return (u8)((colram[a & 0x3FF] & 0x0F) | (open_bus & 0xF0));
    case 0xC: return cia_read(0, a & 0x0F);
    case 0xD: return cia_read(1, a & 0x0F);
    default: return cart.on ? cart_read(a) : open_bus;     /* I/O 1 and 2 */
    }
}

static void slow_write(u16 a, u8 v)             /* page 0 and the I/O */
{
    if (a < 0x100) {
        if (a < 2)
            port_write(a, v);
        else
            ram[a] = v;
        return;
    }
    switch ((a >> 8) & 0x0F) {
    case 0x0: case 0x1: case 0x2: case 0x3: vic_write(a & 0x3F, v); break;
    case 0x4: case 0x5: case 0x6: case 0x7: sid_write(a & 0x1F, v); break;
    case 0x8: case 0x9: case 0xA: case 0xB: colram[a & 0x3FF] = v & 0x0F; break;
    case 0xC: cia_write(0, a & 0x0F, v); break;
    case 0xD: cia_write(1, a & 0x0F, v); break;
    default:
        if (cart.on)
            cart_write(a, v);
        break;
    }
}

static inline u8 rd(u16 a)
{
    const u8 *p = rdp[a >> 8];
    return p ? p[a & 0xFF] : slow_read(a);
}

static inline void wr(u16 a, u8 v)
{
    u8 *p = wrp[a >> 8];
    if (p)
        p[a & 0xFF] = v;
    else
        slow_write(a, v);
}

/* ---- the 6510 ---- */
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

static void push(u8 v)
{
    stack[cpu.s--] = v;
}

static u8 pull(void)
{
    return stack[++cpu.s];
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

static void interrupt(u16 vector)
{
    push((u8)(cpu.pc >> 8));
    push((u8)cpu.pc);
    push((u8)((cpu.p & ~FB) | FU));
    cpu.p |= FI;
    now = cpu.cyc + 6;
    cpu.pc = (u16)(rd(vector) | rd((u16)(vector + 1)) << 8);
    cpu.cyc += 7;
}

static void kernal_trap(void);

/* one instruction.  Its bus accesses happen at the instruction's last cycle (a read-modify-write: it reads
   at its third last, writes the value back unchanged at the second last -- which an I/O register sees:
   INC $D019 acknowledges the VIC's interrupts -- and the new one at the last) */
static void step(void)
{
    u32 start = cpu.cyc;
    u8 op = rd(cpu.pc++), mode = mode_of[op], v;
    u32 cycles = cycles_of[op];
    u16 ea = 0, base = 0;
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
        ea = rd(cpu.pc++);
        ea |= (u16)(rd(cpu.pc++) << 8);
        break;
    case ABX:
        base = rd(cpu.pc++);
        base |= (u16)(rd(cpu.pc++) << 8);
        ea = (u16)(base + cpu.x);
        crossed = ((base ^ ea) & 0xFF00) != 0;
        break;
    case ABY:
        base = rd(cpu.pc++);
        base |= (u16)(rd(cpu.pc++) << 8);
        ea = (u16)(base + cpu.y);
        crossed = ((base ^ ea) & 0xFF00) != 0;
        break;
    case IND:
        base = rd(cpu.pc++);
        base |= (u16)(rd(cpu.pc++) << 8);
        ea = rd(base);
        ea |= (u16)(rd((u16)((base & 0xFF00) | ((base + 1) & 0xFF))) << 8);    /* (no carry into the page) */
        break;
    case IZX:
        v = (u8)(rd(cpu.pc++) + cpu.x);
        ea = rd(v);
        ea |= (u16)(rd((u8)(v + 1)) << 8);
        break;
    case IZY:
        v = rd(cpu.pc++);
        base = rd(v);
        base |= (u16)(rd((u8)(v + 1)) << 8);
        ea = (u16)(base + cpu.y);
        crossed = ((base ^ ea) & 0xFF00) != 0;
        break;
    case REL:
        v = rd(cpu.pc++);
        ea = (u16)(cpu.pc + (signed char)v);
        break;
    }
    if (crossed && ((mode == IZY && cycles == 5) || (mode != IZY && cycles == 4)))
        cycles++;
    now = start + cycles - 1;
#define BRANCH(taken) \
    if (taken) { \
        cycles += ((cpu.pc ^ ea) & 0xFF00) ? 2 : 1; \
        cpu.pc = ea; \
    } \
    break
#define MODIFY(expr) \
    now = start + cycles - 3; \
    v = rd(ea); \
    if ((ea >> 8) && !wrp[ea >> 8]) { \
        now++; \
        wr(ea, v); \
    } \
    v = (expr); \
    now = start + cycles - 1; \
    wr(ea, v)
/* SHA SHX SHY TAS: the register ANDed with the base address's high byte + 1; when the index crosses a page,
   that value is the high byte of the address written as well */
#define UNSTABLE_STORE(reg) \
    v = (u8)((reg) & ((base >> 8) + 1)); \
    if (crossed) \
        ea = (u16)((ea & 0xFF) | v << 8); \
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
        if (ea >= SERIAL_FIRST && ea <= LOAD_ENTRY)
            kernal_trap();
        break;
    case 0x20: {                                /* JSR */
        u8 lo = rd(cpu.pc++);
        push((u8)(cpu.pc >> 8));
        push((u8)cpu.pc);
        cpu.pc = (u16)(lo | rd(cpu.pc) << 8);
        if (cpu.pc >= SERIAL_FIRST && cpu.pc <= LOAD_ENTRY)
            kernal_trap();
        break;
    }
    case 0x60: {                                /* RTS */
        u8 lo = pull();
        cpu.pc = (u16)((lo | pull() << 8) + 1);
        break;
    }
    case 0x40:                                  /* RTI */
        cpu.p = (u8)((pull() & ~FB) | FU);
        cpu.pc = pull();
        cpu.pc |= (u16)(pull() << 8);
        break;
    case 0x00:                                  /* BRK */
        cpu.pc++;
        push((u8)(cpu.pc >> 8));
        push((u8)cpu.pc);
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
    case 0x08: push(cpu.p | FB | FU); break;
    case 0x28:
        v = cpu.p;
        cpu.p = (u8)((pull() & ~FB) | FU);
        if ((v & FI) && !(cpu.p & FI))
            cpu.delay = 1;
        break;
    case 0x48: push(cpu.a); break;
    case 0x68: nz(cpu.a = pull()); break;
    case 0x18: cpu.p &= (u8)~FC; break;
    case 0x38: cpu.p |= FC; break;
    case 0x58:
        if (cpu.p & FI)
            cpu.delay = 1;
        cpu.p &= (u8)~FI;
        break;
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
    case 0x9C: UNSTABLE_STORE(cpu.y); break;   /* SHY */
    case 0x9E: UNSTABLE_STORE(cpu.x); break;   /* SHX */
    case 0x93: case 0x9F: UNSTABLE_STORE(cpu.a & cpu.x); break;   /* SHA */
    case 0x9B:                                  /* TAS */
        cpu.s = cpu.a & cpu.x;
        UNSTABLE_STORE(cpu.s);
        break;
    case 0x02: case 0x12: case 0x22: case 0x32: case 0x42: case 0x52: case 0x62: case 0x72: case 0x92: case 0xB2:
    case 0xD2: case 0xF2:
        cpu.jam = 1;                            /* the 6510 stops for good */
        cpu.pc--;
        break;
    default:
        if (mode != IMP && mode != IMM)
            (void)rd(ea);                       /* (the NOPs with an operand read it) */
        break;
    }
#undef BRANCH
#undef MODIFY
#undef UNSTABLE_STORE
    cpu.cyc = start + cycles;
}

/* the 6510 up to cycle `until` of the frame (it may end an instruction a few cycles after) */
static void run(u32 until)
{
    while (cpu.cyc < until) {
        if (cpu.cyc >= next_event)
            events();
        if (cpu.jam) {
            cpu.cyc = until;
            break;
        }
        if (nmi_pending) {
            nmi_pending = 0;
            interrupt(0xFFFA);
            continue;
        }
        if (irq_pins && !(cpu.p & FI) && !cpu.delay) {
            interrupt(0xFFFE);
            continue;
        }
        cpu.delay = 0;
        step();
    }
}

/* ---- files: a program, a tape's or a disk's ---- */
#define FILES 400
static struct file {
    u8 name[16], len;                   /* PETSCII, without the padding */
    u8 type;                            /* as a directory has it: 0x82 a closed PRG, 0x81 SEQ ... */
    u16 blocks;
    const u8 *data;                     /* a program's or a tape's: the bytes after the load address ... */
    u32 size;
    u16 addr;                           /* ... and it */
    u8 track, sector;                   /* a disk's: its first block */
} files[FILES];
static int nfiles, tape_at;
static enum { NO_FILE, PRG, T64, D64 } kind;
static const u8 *disk;
static u32 tracks;
static u8 disk_name[16], disk_id[5], name_len;
static struct file listing_file;
static u8 listing[8192];
static u8 asked[24], asked_len;          /* the last LOAD's name */
static int asked_found = -1;
static u16 loaded_start, loaded_end;
static u8 loads;

static u32 sectors_on(u32 t)
{
    return t <= 17 ? 21 : t <= 24 ? 19 : t <= 30 ? 18 : 17;
}

static const u8 *block_at(u32 t, u32 s)
{
    u32 n = 0;
    if (t < 1 || t > tracks || s >= sectors_on(t))
        return 0;
    for (u32 i = 1; i < t; i++)
        n += sectors_on(i);
    return disk + (n + s) * 256;
}

/* a file's bytes one by one, its load address first */
struct reader {
    const struct file *f;
    const u8 *blk;
    u32 pos, end, hops;
};

static void reader_start(struct reader *r, const struct file *f)
{
    r->f = f;
    r->pos = 0;
    r->hops = 0;
    r->blk = f->data ? 0 : block_at(f->track, f->sector);
    if (r->blk) {
        r->pos = 2;
        r->end = r->blk[0] ? 256 : (u32)r->blk[1] + 1;
    }
}

static int reader_get(struct reader *r)
{
    if (r->f->data) {
        u32 at = r->pos++;
        if (at < 2)
            return at ? r->f->addr >> 8 : r->f->addr & 0xFF;
        return at - 2 < r->f->size ? r->f->data[at - 2] : -1;
    }
    while (r->blk && r->pos >= r->end) {
        if (!r->blk[0] || ++r->hops > 800)
            return -1;
        r->blk = block_at(r->blk[0], r->blk[1]);
        if (r->blk) {
            r->pos = 2;
            r->end = r->blk[0] ? 256 : (u32)r->blk[1] + 1;
        }
    }
    return r->blk ? r->blk[r->pos++] : -1;
}

static int name_match(const u8 *pattern, int n, const struct file *f)
{
    for (int i = 0; i < n; i++) {
        if (pattern[i] == '*')
            return 1;
        if (i >= f->len)
            return 0;
        if (pattern[i] != '?' && pattern[i] != f->name[i])
            return 0;
    }
    return n == f->len;
}

/* "$": the directory as the 1541 sends it -- a BASIC program, a line for each file */
static u8 *dir_text(u8 *p, const char *s)
{
    while (*s)
        *p++ = (u8)*s++;
    return p;
}

static const struct file *make_listing(void)
{
    static const char *types[8] = {"DEL", "SEQ", "PRG", "USR", "REL", "???", "???", "???"};
    u8 *p = listing, *line;
    u32 free_blocks = 0;
    *p++ = 0x01;
    *p++ = 0x04;
    line = p;
    p[0] = p[1] = 1, p[2] = p[3] = 0;           /* (line 0: the disk's name, reversed) */
    p += 4;
    *p++ = 0x12;
    *p++ = '"';
    for (int i = 0; i < 16; i++)
        *p++ = i < name_len ? disk_name[i] : ' ';
    *p++ = '"';
    *p++ = ' ';
    for (int i = 0; i < 5; i++)
        *p++ = disk_id[i];
    *p++ = 0;
    for (int i = 0; i < nfiles && p < listing + sizeof(listing) - 64; i++) {
        const struct file *f = &files[i];
        u32 b = f->blocks;
        p[0] = p[1] = 1, p[2] = (u8)b, p[3] = (u8)(b >> 8);
        p += 4;
        p = dir_text(p, b < 10 ? "   " : b < 100 ? "  " : " ");
        *p++ = '"';
        for (int k = 0; k < f->len; k++)
            *p++ = f->name[k];
        *p++ = '"';
        for (int k = f->len; k < 16; k++)
            *p++ = ' ';
        *p++ = (f->type & 0x80) ? ' ' : '*';
        p = dir_text(p, types[f->type & 7]);
        *p++ = (f->type & 0x40) ? '<' : ' ';
        *p++ = 0;
    }
    if (kind == D64) {
        const u8 *bam = block_at(18, 0);
        for (u32 t = 1; t <= 35 && bam; t++)
            if (t != 18)
                free_blocks += bam[4 * t];
    }
    p[0] = p[1] = 1, p[2] = (u8)free_blocks, p[3] = (u8)(free_blocks >> 8);
    p += 4;
    p = dir_text(p, "BLOCKS FREE.             ");
    *p++ = 0;
    *p++ = 0;
    *p++ = 0;
    /* the links: each line's to the next, as at $0401 */
    for (line = listing + 2; line[0] || line[1];) {
        u8 *next = line + 4;
        while (*next)
            next++;
        next++;
        u16 at = (u16)(0x0401 + (next - listing - 2));
        line[0] = (u8)at;
        line[1] = (u8)(at >> 8);
        line = next;
    }
    listing_file.data = listing + 2;
    listing_file.size = (u32)(p - listing - 2);
    listing_file.addr = 0x0401;
    listing_file.type = 0x82;
    return &listing_file;
}

/* the KERNAL's LOAD: A 0 load / 1 verify, $BA the device, $B9 the secondary address, $B7 / $BB the name, $C3
   the address to load at (secondary address 0).  Done here: carry clear, X/Y and $AE the end, ST $40; or
   carry set and A the error (4 file not found, 8 missing file name).  Then it returns (RTS) */
/* a file by its name as the 1541 takes it ("0:NAME", ":NAME"; "NAME,P,R": up to the comma; * and ?), or
   "$"; device 1 (the tape), no name: the next file.  0: none */
static const struct file *find_file(const u8 *name, int n, int dev, int *len_out)
{
    const struct file *f = 0;
    int skip = 0, len;
    for (int i = 0; i < n && i < 3; i++)
        if (name[i] == ':')
            skip = i + 1;
    for (len = skip; len < n && name[len] != ','; len++)
        ;
    len -= skip;
    *len_out = len;
    asked_len = (u8)(len < (int)sizeof(asked) ? len : (int)sizeof(asked));
    for (int i = 0; i < asked_len; i++)
        asked[i] = name[skip + i];
    if (!len && dev == 1) {
        if (tape_at < nfiles)
            f = &files[tape_at++];
    } else if (len == 1 && name[skip] == '$' && dev == 8) {
        f = make_listing();
    } else if (len) {
        for (int pass = 0; pass < 2 && !f; pass++)      /* (a program first, then any file that matches) */
            for (int i = 0; i < nfiles && !f; i++)
                if ((files[i].type & 0x87) > 0x80 && (pass || (files[i].type & 7) == 2) &&
                    name_match(name + skip, len, &files[i])) {
                    f = &files[i];
                    if (kind == T64)
                        tape_at = i + 1;        /* (the tape wound past it) */
                }
    }
    asked_found = f != 0;
    return f;
}

static void rts(void)                           /* a routine done here returns to its caller */
{
    cpu.pc = pull();
    cpu.pc = (u16)((cpu.pc | pull() << 8) + 1);
}

static void load_trap(void)
{
    u8 dev = ram[0xBA], n = ram[0xB7], name[24];
    u16 at = (u16)(ram[0xBB] | ram[0xBC] << 8);
    const struct file *f;
    int len, verify = cpu.a != 0, bad = 0, c;
    struct reader r;
    if (dev != 8 && dev != 1)
        return;                                 /* (the KERNAL's own code goes on) */
    if (n > sizeof(name))
        n = sizeof(name);
    for (int i = 0; i < n; i++)
        name[i] = rd((u16)(at + i));
    f = find_file(name, n, dev, &len);
    rts();
    if (!f) {
        cpu.a = !len ? 8 : 4;
        cpu.p |= FC;
        ram[0x90] = dev == 8 ? 0x42 : 0x00;
        return;
    }
    reader_start(&r, f);
    c = reader_get(&r);
    loaded_start = (u16)(c | reader_get(&r) << 8);
    if (!ram[0xB9])                             /* (secondary address 0: where X/Y said) */
        loaded_start = (u16)(ram[0xC3] | ram[0xC4] << 8);
    at = loaded_start;
    now = cpu.cyc;
    while ((c = reader_get(&r)) >= 0) {
        if (verify) {
            if (rd(at) != (u8)c)
                bad = 1;
        } else {
            wr(at, (u8)c);
        }
        at++;
    }
    loaded_end = at;
    ram[0xC1] = (u8)loaded_start, ram[0xC2] = (u8)(loaded_start >> 8);
    ram[0xAE] = (u8)at, ram[0xAF] = (u8)(at >> 8);
    ram[0x90] = (u8)((dev == 8 ? 0x40 : 0x00) | (bad ? 0x10 : 0));
    ram[0x93] = (u8)verify;
    cpu.x = (u8)at;
    cpu.y = (u8)(at >> 8);
    cpu.p &= (u8)~FC;
    loads++;
}

/* A 1541 as device 8 on the serial bus, one level up from the wires: the KERNAL's routines that work the
   bus -- TALK, LISTEN, SECOND (the secondary address after LISTEN: $6x data, $Ex close, $Fx open), TKSA,
   CIOUT (a byte out), ACPTR (a byte in; the last one comes with EOI, ST bit 6), UNTLK, UNLSN -- are done
   here when they talk to device 8, so OPEN / CHKIN / CHRIN / CLOSE read the file's programs and its
   directory, and the command channel (15) answers: I, UI (73,CBM DOS V2.6 1541), M-R (zeros), M-W and M-E
   taken and forgotten (a fast loader's drive code never runs: the C64 then waits for it in vain, or --
   most loaders -- goes on with the KERNAL), writing: 26,WRITE PROTECT ON. */
static struct channel {
    u8 open;
    struct reader r;                    /* a file being read ... */
    const u8 *buf;                      /* ... or bytes from here (the command channel's message) */
    u32 len, pos;
    int ahead;                          /* the byte sent next; -1: none (the one before came with EOI) */
} chan[16];
static struct {
    u8 ours, talk, sa, cmd;             /* device 8 addressed; talking; the channel; $60 / $E0 / $F0 */
    u8 text[64], n;                     /* the bytes LISTENed since the SECOND */
} bus;
static u8 status_text[48], memory_read[256];

static int chan_get(struct channel *c)
{
    if (c->buf)
        return c->pos < c->len ? c->buf[c->pos++] : -1;
    return reader_get(&c->r);
}

static void drive_status(u32 code, const char *text)    /* "62,FILE NOT FOUND,00,00" + RETURN */
{
    u8 *p = status_text;
    *p++ = (u8)('0' + code / 10);
    *p++ = (u8)('0' + code % 10);
    *p++ = ',';
    while (*text)
        *p++ = (u8)*text++;
    p = (u8 *)dir_text(p, ",00,00\r");
    chan[15].open = 1;
    chan[15].buf = status_text;
    chan[15].len = (u32)(p - status_text);
    chan[15].pos = 0;
    chan[15].ahead = chan_get(&chan[15]);
}

static void drive_command(const u8 *t, int n)
{
    while (n && (t[n - 1] == '\r' || t[n - 1] == ' '))
        n--;
    if (!n) {
        drive_status(0, " OK");
    } else if (n >= 5 && t[0] == 'M' && t[1] == '-' && t[2] == 'R') {
        u32 count = n >= 6 ? t[5] : 1;          /* (the drive's memory: none here, zeros) */
        chan[15].buf = memory_read;
        chan[15].len = count ? count : 256;
        chan[15].pos = 0;
        chan[15].ahead = chan_get(&chan[15]);
    } else if (t[0] == 'U' && n >= 2 && (t[1] == 'I' || t[1] == 'J' || t[1] == ':' || t[1] == '9')) {
        drive_status(73, "CBM DOS V2.6 1541");
    } else if (t[0] == 'I' || t[0] == 'M' || t[0] == 'U' || t[0] == 'B' || t[0] == 'V') {
        drive_status(0, " OK");
    } else if (t[0] == 'S' || t[0] == 'R' || t[0] == 'N' || t[0] == 'C') {
        drive_status(26, "WRITE PROTECT ON");
    } else {
        drive_status(31, "SYNTAX ERROR");
    }
}

static void drive_open(u32 sa, const u8 *name, int n)
{
    struct channel *c = &chan[sa];
    const struct file *f;
    int len, write = sa == 1;
    if (sa == 15) {
        drive_command(name, n);
        return;
    }
    c->open = 0;
    for (int i = 0; i + 1 < n; i++)
        if (name[i] == ',' && (name[i + 1] == 'W' || name[i + 1] == 'A'))
            write = 1;
    if (write) {
        drive_status(26, "WRITE PROTECT ON");
        return;
    }
    if (n && name[0] == '#') {                  /* a buffer for the block commands: empty */
        c->open = 1;
        c->buf = memory_read;
        c->len = 0;
        c->ahead = -1;
        drive_status(0, " OK");
        return;
    }
    f = find_file(name, n, 8, &len);
    if (!f) {
        drive_status(len ? 62 : 34, len ? "FILE NOT FOUND" : "SYNTAX ERROR");
        return;
    }
    c->open = 1;
    c->buf = 0;
    reader_start(&c->r, f);
    c->ahead = chan_get(c);
    drive_status(0, " OK");
}

/* a serial bus routine of the KERNAL's, called at `pc`: 1 when done here (for device 8) */
static int serial_trap(u16 pc)
{
    u8 a = cpu.a;
    if (pc == 0xED09 || pc == 0xED0C) {         /* TALK, LISTEN: A the device */
        bus.ours = (a & 0x1F) == 8;
        if (!bus.ours)
            return 0;
        bus.talk = pc == 0xED09;
        bus.cmd = 0;
        bus.n = 0;
        return 1;
    }
    if (!bus.ours)
        return 0;
    switch (pc) {
    case 0xEDB9:                                /* SECOND */
        bus.sa = a & 15;
        bus.cmd = a & 0xF0;
        bus.n = 0;
        if (bus.cmd == 0xE0) {
            chan[bus.sa].open = 0;
            if (bus.sa == 15)
                for (int i = 0; i < 15; i++)
                    chan[i].open = 0;
        }
        return 1;
    case 0xEDC7:                                /* TKSA */
        bus.sa = a & 15;
        return 1;
    case 0xEDCC:                                /* (TKSA's end alone: the channel as it was) */
        return 1;
    case 0xEDDD:                                /* CIOUT */
        if (bus.n < sizeof(bus.text))
            bus.text[bus.n++] = a;
        return 1;
    case 0xEE13: {                              /* ACPTR */
        struct channel *c = &chan[bus.sa];
        if (!bus.talk || !c->open || c->ahead < 0) {
            ram[0x90] |= 0x42;                  /* (nothing to send: a timeout, and EOI) */
            cpu.a = 0x0D;
            if (bus.sa == 15)
                drive_status(0, " OK");
            return 1;
        }
        cpu.a = (u8)c->ahead;
        c->ahead = chan_get(c);
        if (c->ahead < 0) {
            ram[0x90] |= 0x40;
            if (bus.sa == 15)
                drive_status(0, " OK");         /* (the message read: OK next time) */
        }
        return 1;
    }
    case 0xEDEF:                                /* UNTLK */
        bus.ours = bus.talk = 0;
        return 1;
    case 0xEDFE:                                /* UNLSN: an OPEN's name, or a command, is complete */
        if (bus.cmd == 0xF0)
            drive_open(bus.sa, bus.text, bus.n);
        else if (bus.cmd == 0x60 && bus.sa == 15 && bus.n)
            drive_command(bus.text, bus.n);
        bus.ours = 0;
        bus.n = 0;
        return 1;
    default:
        return 0;
    }
}

/* a JSR or JMP into the KERNAL's file routines: done here when they are for the file (the KERNAL in place,
   the one whose code is known) */
static void kernal_trap(void)
{
    u16 pc = cpu.pc;
    if (!trap_ok || kind == NO_FILE || rdp[pc >> 8] != kernal + (pc - 0xE000 - (pc & 0xFF)))
        return;
    if (pc == LOAD_ENTRY) {
        load_trap();
        return;
    }
    if (pc > 0xEE13 || !serial_trap(pc))
        return;
    rts();                                      /* (the KERNAL's routines end with CLI, CLC) */
    cpu.p &= (u8)~(FC | FI);
}

/* ---- typing the file's LOAD and RUN into BASIC: straight into the keyboard buffer ($0277, 10 bytes, $C6
   how many) while the screen editor waits for keys ---- */
static const char *auto_text;
static u8 boot, boot_frames;            /* 1 waits for BASIC, 2 LOAD typed, 3 loaded: RUN or SYS next, 4 typed */
static char sys_text[16];

static int basic_waits(void)
{
    return wait_ok && cpu.pc >= WAIT_FIRST && cpu.pc <= WAIT_LAST && rdp[0xE5] == kernal + 0x0500 && !ram[0xC6];
}

static void autostart(void)
{
    int waits;
    if (!boot)
        return;
    waits = basic_waits();
    if (auto_text && *auto_text) {
        if (waits) {                            /* (up to the next RETURN, 10 at a time) */
            int n = 0;
            while (auto_text[n] && n < 10) {
                ram[0x277 + n] = (u8)auto_text[n];
                if (auto_text[n++] == '\r')
                    break;
            }
            ram[0xC6] = (u8)n;
            auto_text += n;
        }
        boot_frames = 0;
        return;
    }
    boot_frames++;
    switch (boot) {
    case 1:
        if (waits) {
            auto_text = "LOAD\"*\",8,1\r";
            boot = 2;
            loads = 0;
        }
        break;
    case 2:
        if (loads) {
            boot = 3;
            boot_frames = 0;
        } else if (waits && boot_frames > 10) {
            boot = 0;                           /* (it did not load) */
        }
        break;
    case 3:
        if (waits) {
            if (loaded_start <= 0x0801 && loaded_end > 0x0805) {
                auto_text = "RUN\r";
            } else {
                char *p = sys_text, d[6];
                u32 v = loaded_start, k = 0;
                *p++ = 'S', *p++ = 'Y', *p++ = 'S';
                do
                    d[k++] = (char)('0' + v % 10);
                while (v /= 10);
                while (k)
                    *p++ = d[--k];
                *p++ = '\r';
                *p = 0;
                auto_text = sys_text;
            }
            boot = 4;
        } else if (boot_frames > 25) {
            boot = 0;                           /* (it started by itself) */
        }
        break;
    default:
        boot = 0;
        break;
    }
}

/* ---- the VIC's pictures ---- */
static u16 scratch[C64_W + 16];         /* a line made only for its sprites' collisions */
static u8 fg_bits[40];                  /* each cell's foreground pixels (bit 7 the leftmost) ... */
static u32 fg_x;                        /* ... from this x of the line on (16 + XSCROLL) */
static u8 sp_col[512 + 64], sp_mask[512 + 64], sp_behind[512 + 64];  /* the sprites' pixels by X */

static inline void hires(u16 *o, u32 g, u16 fg, u16 bg)        /* 1 bits fg, 0 bits bg */
{
    o[0] = (g & 0x80) ? fg : bg;
    o[1] = (g & 0x40) ? fg : bg;
    o[2] = (g & 0x20) ? fg : bg;
    o[3] = (g & 0x10) ? fg : bg;
    o[4] = (g & 0x08) ? fg : bg;
    o[5] = (g & 0x04) ? fg : bg;
    o[6] = (g & 0x02) ? fg : bg;
    o[7] = (g & 0x01) ? fg : bg;
}

static inline void multi(u16 *o, u32 g, const u16 c[4])        /* pairs of bits, two pixels each */
{
    o[0] = o[1] = c[g >> 6];
    o[2] = o[3] = c[(g >> 4) & 3];
    o[4] = o[5] = c[(g >> 2) & 3];
    o[6] = o[7] = c[g & 3];
}

#define MULTI_FG(g) ((u8)(((g) & 0xAA) | (((g) & 0xAA) >> 1)))     /* (pairs 10 and 11 are foreground) */

/* the 40 cells of graphics (or the idle state's byte, with no colours) into the line from x 16 + XSCROLL */
static void graphics(u16 *line)
{
    static const u16 none[4] = {0, 0, 0, 0};
    const u8 *r = vic.reg;
    u32 ecm = r[0x11] & 0x40, xs = r[0x16] & 7, rc = vic.rc, vc = vic.vc, i;
    u32 mode = (u32)((r[0x11] & 0x60) | (r[0x16] & 0x10)) >> 4;
    u32 chars = (u32)(r[0x18] & 0x0E) << 10, bitmap = (u32)(r[0x18] & 0x08) << 10;
    u32 mask = ecm ? 0x39FF : 0x3FFF;
    u32 c = 0, col = 0, g = VREAD(mask);
    u16 *o = line + 16, bg = pal[r[0x21] & 15], mc[4];
    for (i = 0; i < xs; i++)
        *o++ = bg;
    fg_x = 16 + xs;
    mc[0] = bg, mc[1] = pal[r[0x22] & 15], mc[2] = pal[r[0x23] & 15];
    for (i = 0; i < 40; i++, o += 8) {
        if (vic.display) {
            c = vic.cbuf[i];
            col = vic.colbuf[i];
            if (mode & 2)
                g = VREAD((bitmap | ((vc + i) & 0x3FF) << 3 | rc) & mask);
            else
                g = VREAD(chars | (ecm ? c & 0x3F : c) << 3 | rc);
        }
        switch (mode) {
        case 0:                                 /* text */
            hires(o, g, pal[col], bg);
            fg_bits[i] = (u8)g;
            break;
        case 1:                                 /* multicolour text: colours 8-15 make a multicolour cell */
            if (col & 8) {
                mc[3] = pal[col & 7];
                multi(o, g, mc);
                fg_bits[i] = MULTI_FG(g);
            } else {
                hires(o, g, pal[col & 7], bg);
                fg_bits[i] = (u8)g;
            }
            break;
        case 2:                                 /* bitmap: the colours from the screen codes */
            hires(o, g, pal[c >> 4], pal[c & 15]);
            fg_bits[i] = (u8)g;
            break;
        case 3: {                               /* multicolour bitmap */
            u16 m[4] = {bg, pal[c >> 4], pal[c & 15], pal[col]};
            multi(o, g, m);
            fg_bits[i] = MULTI_FG(g);
            break;
        }
        case 4:                                 /* extended colour text: a code's top 2 bits pick the
                                                   background */
            hires(o, g, pal[col], pal[r[0x21 + (c >> 6)] & 15]);
            fg_bits[i] = (u8)g;
            break;
        default:                                /* the invalid ones: black, the foreground as in their mode */
            multi(o, 0, none);
            fg_bits[i] = (mode == 7 || (mode == 5 && (col & 8))) ? MULTI_FG(g) : (u8)g;
            break;
        }
    }
    open_bus = (u8)g;
}

static inline int foreground(u32 x)            /* the graphics' pixel at x is foreground */
{
    u32 d = x - fg_x;
    return d < 320 && ((fg_bits[d >> 3] << (d & 7)) & 0x80);
}

static void put_sprite_pixel(u32 x, u8 colour, u8 bit, u8 behind, u32 *lo, u32 *hi)
{
    if (x >= 504)
        x -= 504;                               /* (a PAL line has 504 positions) */
    sp_col[x] = colour;
    sp_mask[x] |= bit;
    sp_behind[x] = behind;
    if (x < *lo)
        *lo = x;
    if (x > *hi)
        *hi = x;
}

/* the line's sprites: their collisions, and over the line's graphics (`window`: there are graphics, not
   the border) */
static void sprites(u16 *line, int window)
{
    const u8 *r = vic.reg;
    u32 lo = 1000, hi = 0;
    u8 ss = 0, sb = 0;
    for (int n = 7; n >= 0; n--) {
        u8 bit = (u8)(1u << n), behind = (r[0x1B] & bit) ? 1 : 0;
        u32 d = vic.data[n], x = r[2 * n] | ((r[0x10] & bit) ? 0x100u : 0), w = (r[0x1D] & bit) ? 2 : 1;
        if (!(vic.shown & bit) || !d || x >= 504)
            continue;
        if (r[0x1C] & bit) {                    /* multicolour: pairs of bits, 01 $D025, 10 its own, 11 $D026 */
            u8 c3[4] = {0, (u8)(r[0x25] & 15), (u8)(r[0x27 + n] & 15), (u8)(r[0x26] & 15)};
            for (u32 k = 0; k < 12; k++) {
                u32 c = (d >> (22 - 2 * k)) & 3;
                if (c)
                    for (u32 j = 0; j < 2 * w; j++)
                        put_sprite_pixel(x + 2 * w * k + j, c3[c], bit, behind, &lo, &hi);
            }
        } else {
            u8 colour = r[0x27 + n] & 15;
            for (u32 k = 0; k < 24; k++)
                if (d & (0x800000u >> k))
                    for (u32 j = 0; j < w; j++)
                        put_sprite_pixel(x + w * k + j, colour, bit, behind, &lo, &hi);
        }
    }
    for (u32 x = lo; x <= hi; x++) {
        u8 m = sp_mask[x];
        if (!m)
            continue;
        sp_mask[x] = 0;
        if (m & (m - 1))
            ss |= m;
        if (window && x >= 8 && x < 8 + C64_W) {
            int fg = foreground(x - 8);
            if (fg)
                sb |= m;
            if (!sp_behind[x] || !fg)
                line[x - 8] = pal[sp_col[x]];
        }
    }
    if ((ss && !vic.ss) || (sb && !vic.sb)) {
        vic.irr |= (u8)((ss && !vic.ss ? 4 : 0) | (sb && !vic.sb ? 2 : 0));
        pins();
    }
    vic.ss |= ss;
    vic.sb |= sb;
}

/* line r's pixels into out (0: none kept -- the sprites' collisions all the same) */
static void draw_line(u32 r, u16 *out)
{
    const u8 *g = vic.reg;
    u16 border = pal[g[0x20] & 15], *line = out ? out : scratch;
    u32 top = (g[0x11] & 8) ? 51 : 55, bottom = (g[0x11] & 8) ? 251 : 247, x;
    /* the vertical border flip-flop, as the left edge of the window passes */
    if (r == bottom)
        vic.vborder = 1;
    else if (r == top && (g[0x11] & 0x10))
        vic.vborder = 0;
    if (!out && !vic.shown)
        return;
    if (vic.vborder) {
        if (vic.shown)
            sprites(line, 0);
        if (out)
            for (x = 0; x < C64_W; x++)
                out[x] = border;
        return;
    }
    graphics(line);
    if (vic.shown)
        sprites(line, 1);
    if (out) {                                  /* the side borders, over all */
        u32 left = (g[0x16] & 8) ? 16 : 23, right = (g[0x16] & 8) ? 336 : 327;
        for (x = 0; x < left; x++)
            out[x] = border;
        for (x = right; x < C64_W; x++)
            out[x] = border;
    }
}

/* the sprites' DMA at a line's end (cycles 15-16 and 55-58 of the 6569): MCBASE moves on (with the Y
   expansion flip-flop set), the DMA ends after 21 rows, starts where Y is this line, and the 3 bytes of
   the next line are fetched */
static void sprites_end(u32 r)
{
    const u8 *g = vic.reg;
    u32 vm = (u32)(g[0x18] & 0xF0) << 6, n_dma = 0;
    for (int n = 0; n < 8; n++) {
        u8 bit = (u8)(1u << n);
        if ((vic.dma & bit) && (vic.expand & bit)) {
            vic.mcbase[n] = (u8)((vic.mcbase[n] + 3) & 63);
            if (vic.mcbase[n] == 63)
                vic.dma &= (u8)~bit;
        }
        if (g[0x17] & bit)
            vic.expand ^= bit;
        else
            vic.expand |= bit;
        if ((g[0x15] & bit) && g[2 * n + 1] == (u8)r && !(vic.dma & bit)) {
            vic.dma |= bit;
            vic.mcbase[n] = 0;
            if (g[0x17] & bit)
                vic.expand &= (u8)~bit;
        }
        vic.mc[n] = vic.mcbase[n];
        if (vic.dma & bit) {
            u32 at = (u32)VREAD(vm | 0x3F8 | (u32)n) << 6, mc = vic.mc[n];
            vic.data[n] = (u32)VREAD(at | mc) << 16 | (u32)VREAD(at | ((mc + 1) & 63)) << 8 |
                          VREAD(at | ((mc + 2) & 63));
            n_dma++;
        }
    }
    vic.shown = vic.dma;
    vic.stall = (u8)(n_dma ? 2 * n_dma + 3 : 0);
}

/* ---- the frame ---- */
void c64_frame(u8 pad, const u8 matrix[8], u16 *picture)
{
    u8 held[8] = {0}, j;
    u32 r;
    if (matrix)
        for (int i = 0; i < 8; i++)
            held[i] = matrix[i];
    typed(held);
    if (pad & C64_START)
        held[7] |= 0x10;                        /* SPACE */
    if (pad & C64_SELECT)
        held[0] |= 0x08;                        /* F7 */
    for (int i = 0; i < 8; i++)
        keys[i] = held[i];
    j = (u8)(((pad & C64_UP) ? 1 : 0) | ((pad & C64_DOWN) ? 2 : 0) | ((pad & C64_LEFT) ? 4 : 0) |
             ((pad & C64_RIGHT) ? 8 : 0) | ((pad & (C64_A | C64_B)) ? 0x10 : 0) | hid_joy);
    swapped = hid_swap;
    joy[0] = swapped ? j : 0;
    joy[1] = swapped ? 0 : j;
    restore = hid_restore;
    pins();
    autostart();
    tod_tick(&cia[0]);
    tod_tick(&cia[1]);
    for (r = 0; r < LINES; r++) {
        u32 base = r * LINE_CYCLES;
        const u8 *g = vic.reg;
        if (!r) {
            vic.vcbase = 0;
            vic.den30 = 0;
        }
        if (r == vic.cmp) {
            vic.irr |= 1;
            pins();
        }
        cpu.cyc += vic.stall;                   /* (the sprites' fetches) */
        run(base + 12);
        if (r == 0x30)
            vic.den30 |= g[0x11] & 0x10;
        vic.bad = r >= 0x30 && r <= 0xF7 && (r & 7) == (g[0x11] & 7u) && vic.den30;
        vic.vc = vic.vcbase;
        if (vic.bad) {                          /* the next row's characters and colours; the CPU waits */
            u32 vm = (u32)(g[0x18] & 0xF0) << 6;
            vic.rc = 0;
            vic.display = 1;
            for (u32 i = 0; i < 40; i++) {
                u32 a = (vic.vc + i) & 0x3FF;
                vic.cbuf[i] = VREAD(vm | a);
                vic.colbuf[i] = colram[a] & 15;
            }
            cpu.cyc += 40;
        }
        run(base + 36);
        if (r == 0x30)
            vic.den30 |= g[0x11] & 0x10;
        draw_line(r, picture && r >= TOP_LINE && r < TOP_LINE + C64_H ? picture + (r - TOP_LINE) * C64_W : 0);
        if (vic.display)
            vic.vc = (u16)((vic.vc + 40) & 0x3FF);
        run(base + LINE_CYCLES);
        if (vic.rc == 7) {
            vic.vcbase = vic.vc;
            if (!vic.bad)
                vic.display = 0;
        }
        if (vic.display)
            vic.rc = (u8)((vic.rc + 1) & 7);
        sprites_end(r);
        if (r == (u32)((g[0x11] & 8) ? 251 : 247))
            vic.vborder = 1;
        else if (r == (u32)((g[0x11] & 8) ? 51 : 55) && (g[0x11] & 0x10))
            vic.vborder = 0;
    }
    /* counted from the next frame's start again */
    events();
    sid_run(cpu.cyc);
    cpu.cyc -= FRAME_CYCLES;
    cia[0].last -= FRAME_CYCLES;
    cia[1].last -= FRAME_CYCLES;
    sid.last -= FRAME_CYCLES;
    next_event = 0;
    frames_done++;
}

/* ---- power-on ---- */
void c64_reset(void)
{
    for (u32 i = 0; i < sizeof(ram); i++)       /* (RAM wakes up in stripes of $00 and $FF) */
        ram[i] = (u8)((((i >> 6) ^ (i >> 14)) & 1) ? 0xFF : 0x00);
    for (u32 i = 0; i < sizeof(colram); i++)
        colram[i] = (u8)(i * 7 & 15);
    for (int i = 0; i < 16; i++) {
        const u8 *c = c64_rgb[i];
        pal[i] = (u16)(c[0] >> 3 | (c[1] >> 3) << 5 | (c[2] >> 3) << 10);
    }
    for (int i = 0; i < 0xA0; i++) {
        rdp[i] = ram + (i << 8);
        wrp[i] = ram + (i << 8);
    }
    wrp[0] = 0;                                 /* (the port: written the slow way) */
    stack = ram + 0x100;
    if (cart.on)
        cart_reset();
    port_dir = port_out = 0;
    port_write(0, 0);
    for (int n = 0; n < 2; n++) {
        struct cia *c = &cia[n];
        for (u32 i = 0; i < sizeof(*c); i++)
            ((u8 *)c)[i] = 0;
        c->ta = c->tb = c->la = c->lb = 0xFFFF;
        c->tod[3] = 0x01;
    }
    for (u32 i = 0; i < sizeof(vic); i++)
        ((u8 *)&vic)[i] = 0;
    vic.vborder = 1;
    vic_bank();
    for (u32 i = 0; i < sizeof(sid); i++)
        ((u8 *)&sid)[i] = 0;
    sid.noise = 0x7FFFF8;
    for (int i = 0; i < 8; i++)
        keys[i] = 0;
    joy[0] = joy[1] = 0;
    irq_pins = nmi_line = nmi_pending = restore = 0;
    next_event = 0;
    frames_done = 0;
    now = 0;
    open_bus = 0;
    cpu.a = cpu.x = cpu.y = 0;
    cpu.s = 0xFD;
    cpu.p = FI | FU;
    cpu.jam = cpu.delay = 0;
    cpu.cyc = 0;
    cpu.pc = (u16)(rd(0xFFFC) | rd(0xFFFD) << 8);
    q_head = q_tail = q_frame = 0;
    for (int i = 0; i < 16; i++)
        chan[i].open = 0;
    bus.ours = bus.talk = bus.n = 0;
    drive_status(73, "CBM DOS V2.6 1541");
    tape_at = 0;
    asked_len = 0;
    asked_found = -1;
    auto_text = 0;
    boot = kind != NO_FILE ? 1 : 0;
    boot_frames = 0;
    loads = 0;
}

/* ---- the file ---- */
static int ends_with(const char *name, const char *ext)
{
    int n = 0, e = 0;
    while (name && name[n])
        n++;
    while (ext[e])
        e++;
    if (n < e)
        return 0;
    for (int i = 0; i < e; i++) {
        char c = name[n - e + i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c + 32);
        if (c != ext[i])
            return 0;
    }
    return 1;
}

static void set_name(struct file *f, const u8 *s, int n)   /* without the padding ($A0, or spaces) */
{
    while (n > 0 && (s[n - 1] == 0xA0 || s[n - 1] == 0x20 || !s[n - 1]))
        n--;
    if (n > 16)
        n = 16;
    for (int i = 0; i < n; i++)
        f->name[i] = s[i];
    f->len = (u8)n;
}

static const char *from_prg(const u8 *f, u32 size, const char *path)
{
    u8 name[16];
    int n = 0, from = 0, dot = -1;
    if (size < 3)
        return "a .prg file of less than 3 bytes";
    for (int i = 0; path && path[i]; i++) {    /* (its name: the file's, without the directory or ending) */
        if (path[i] == '/' || path[i] == '\\' || path[i] == ':')
            from = i + 1, dot = -1;
        else if (path[i] == '.')
            dot = i;
    }
    for (int i = from; path && path[i] && i != dot && n < 16; i++) {
        char c = path[i];
        name[n++] = (u8)(c >= 'a' && c <= 'z' ? c - 32 : c);
    }
    set_name(&files[0], name, n);
    files[0].type = 0x82;
    files[0].addr = (u16)(f[0] | f[1] << 8);
    files[0].data = f + 2;
    files[0].size = size - 2;
    files[0].blocks = (u16)((size + 253) / 254);
    nfiles = 1;
    for (int i = 0; i < files[0].len; i++)
        disk_name[i] = files[0].name[i];
    name_len = files[0].len;
    return 0;
}

static const char *from_p00(const u8 *f, u32 size)
{
    static const char sig[8] = {'C', '6', '4', 'F', 'i', 'l', 'e', 0};
    for (int i = 0; i < 8; i++)
        if (size < 29 || f[i] != (u8)sig[i])
            return "not a .p00 file (no C64File signature)";
    from_prg(f + 26, size - 26, 0);
    set_name(&files[0], f + 8, 16);
    for (int i = 0; i < files[0].len; i++)
        disk_name[i] = files[0].name[i];
    name_len = files[0].len;
    return 0;
}

static const char *from_t64(const u8 *f, u32 size)
{
    u32 slots;
    if (size < 64 || f[0] != 'C' || f[1] != '6' || f[2] != '4')
        return "not a .t64 tape (no C64 signature)";
    slots = f[0x22] | (u32)f[0x23] << 8;
    if (!slots)
        slots = 1;
    for (u32 i = 0; i < slots && 64 + 32 * (i + 1) <= size && nfiles < FILES; i++) {
        const u8 *e = f + 64 + 32 * i;
        u32 start = e[2] | (u32)e[3] << 8, end = e[4] | (u32)e[5] << 8, len, limit = size;
        u32 off = e[8] | (u32)e[9] << 8 | (u32)e[10] << 16 | (u32)e[11] << 24;
        if (!e[0] || off >= size)
            continue;                           /* (a free slot) */
        if (!end)
            end = 0x10000;
        len = end > start ? end - start : 0;
        /* (end addresses are often wrong: no further than the next file, or the tape's end) */
        for (u32 k = 0; k < slots && 64 + 32 * (k + 1) <= size; k++) {
            const u8 *o = f + 64 + 32 * k;
            u32 other = o[8] | (u32)o[9] << 8 | (u32)o[10] << 16 | (u32)o[11] << 24;
            if (o[0] && other > off && other < limit)
                limit = other;
        }
        if (!len || off + len > limit)
            len = limit - off;
        struct file *t = &files[nfiles++];
        set_name(t, e + 16, 16);
        t->type = 0x82;
        t->addr = (u16)start;
        t->data = f + off;
        t->size = len;
        t->blocks = (u16)((len + 2 + 253) / 254);
    }
    set_name(&listing_file, f + 0x28, 24);      /* (the tape's name, for "$") */
    for (int i = 0; i < listing_file.len; i++)
        disk_name[i] = listing_file.name[i];
    name_len = listing_file.len;
    return nfiles ? 0 : "a .t64 tape with no files";
}

static const char *from_d64(const u8 *f, u32 size)
{
    const u8 *b;
    u32 t = 18, s = 1, hops = 0;
    if (size < 683 * 256)
        return "not a .d64 disk (174848 bytes or more)";
    disk = f;
    tracks = size >= 802 * 256 ? 42 : size >= 768 * 256 ? 40 : 35;
    b = block_at(18, 0);
    if (b[0] == 18)
        t = 18, s = b[1];
    set_name(&listing_file, b + 0x90, 16);
    for (int i = 0; i < listing_file.len; i++)
        disk_name[i] = listing_file.name[i];
    name_len = listing_file.len;
    for (int i = 0; i < 5; i++)
        disk_id[i] = b[0xA2 + i] == 0xA0 ? ' ' : b[0xA2 + i];
    while ((b = block_at(t, s)) != 0 && hops++ < 40) {
        for (int k = 0; k < 8 && nfiles < FILES; k++) {
            const u8 *e = b + 32 * k;
            if (!(e[2] & 7) && !(e[2] & 0x80))
                continue;                       /* (a deleted or empty entry) */
            struct file *d = &files[nfiles++];
            set_name(d, e + 5, 16);
            d->type = e[2];
            d->track = e[3];
            d->sector = e[4];
            d->blocks = (u16)(e[30] | e[31] << 8);
            d->data = 0;
        }
        if (!b[0])
            break;
        t = b[0], s = b[1];
    }
    return 0;
}

/* a cartridge: the .crt's header (64 bytes: "C64 CARTRIDGE   ", its length, the version, the hardware type,
   EXROM, GAME, the revision, the name) and its CHIP packets (the length, ROM / RAM / flash, the bank, the
   address, the size, the bytes) */
static const char *const cart_names[] = {
    "normal", "Action Replay", "KCS Power Cartridge", "Final Cartridge III", "Simons' BASIC", "Ocean type 1",
    "Expert Cartridge", "Fun Play, Power Play", "Super Games", "Atomic Power", "Epyx Fastload",
    "Westermann Learning", "Rex Utility", "Final Cartridge I", "Magic Formel", "C64 Game System, System 3",
    "Warp Speed", "Dinamic", "Zaxxon, Super Zaxxon", "Magic Desk, Domark, HES Australia", "Super Snapshot V5",
    "Comal-80", "Structured BASIC", "Ross", "Dela EP64", "Dela EP7x8", "Dela EP256", "Rex EP256",
    "Mikro Assembler", "Final Cartridge Plus", "Action Replay 4", "Stardos", "EasyFlash", "EasyFlash Xbank",
    "Capture", "Action Replay 3", "Retro Replay", "MMC64", "MMC Replay", "IDE64", "Super Snapshot V4",
    "IEEE-488", "Game Killer", "Prophet64", "EXOS", "Freeze Frame", "Freeze Machine", "Snapshot64",
    "Super Explode V5.0", "Magic Voice", "Action Replay 2", "MACH 5", "Diashow-Maker", "Pagefox", "Kingsoft",
    "Silverrock 128K", "Formel 64", "RGCD", "RR-Net MK3", "EasyCalc", "GMod2", "MAX Basic", "GMod3",
};

static u32 be16(const u8 *p)
{
    return (u32)p[0] << 8 | p[1];
}

static const char *from_crt(const u8 *f, u32 size)
{
    static const u8 supported[] = {0, 3, 4, 5, 7, 8, 15, 17, 19, 32, 57};
    u32 type = be16(f + 0x16), at = (u32)f[0x10] << 24 | (u32)f[0x11] << 16 | (u32)f[0x12] << 8 | f[0x13];
    u32 shorts = 0, top = 0, ok = 0;
    char *p;
    for (u32 i = 0; i < sizeof(supported); i++)
        ok |= type == supported[i];
    if (!ok) {
        p = put(info, "a cartridge of type ");
        p = put_number(p, type);
        if (type < sizeof(cart_names) / sizeof(cart_names[0])) {
            p = put(p, " (");
            p = put(p, cart_names[type]);
            p = put(p, ")");
        }
        put(p, ": not supported");
        return info;
    }
    for (int i = 0; i < CART_BANKS; i++)
        cart.lo[i] = cart.hi[i] = 0;
    cart.type = (u8)type;
    cart.sub = f[0x1A];
    cart.exrom0 = f[0x18] ? 1 : 0;
    cart.game0 = f[0x19] ? 1 : 0;
    if (type == 3)
        cart.exrom0 = cart.game0 = 0;           /* (the Final Cartridge III starts as 16K) */
    if (type == 32)
        cart.exrom0 = 1, cart.game0 = 0;        /* (EasyFlash: Ultimax, its boot jumper's GAME) */
    for (int i = 0; i < 32; i++)
        cart.name[i] = f[0x20 + i];
    cart.size = 0;
    if (at < 0x40)
        at = 0x40;
    while (at <= size - 16 && f[at] == 'C' && f[at + 1] == 'H' && f[at + 2] == 'I' && f[at + 3] == 'P') {
        const u8 *c = f + at;
        u32 len = (u32)c[4] << 24 | (u32)c[5] << 16 | (u32)c[6] << 8 | c[7];
        u32 kind_of = be16(c + 8), bank = be16(c + 10), addr = be16(c + 12), n = be16(c + 14);
        const u8 *data = c + 16;
        if (len < 16 + n)
            len = 16 + n;
        if (len > size - at)
            len = size - at;                    /* (a damaged length: never back into the file, round 4 GB) */
        if (at + 16 + n > size)
            return "a .crt cartridge cut short";
        at += len;
        if (kind_of == 1 || !n)
            continue;                           /* (RAM: nothing to load) */
        if (type == 5 && addr == 0xA000)
            bank &= 15;                         /* (Ocean's 256 KB: banks 16-31 are ROMH's 0-15) */
        if (bank >= CART_BANKS)
            return "a .crt cartridge with more than 128 banks";
        if (!((addr == 0x8000 && (n == 0x4000 || n <= 0x2000)) ||
              ((addr == 0xA000 || addr == 0xE000) && n <= 0x2000) || (addr == 0xF000 && n <= 0x1000)))
            return "a .crt cartridge with a chip that is not at $8000, $A000 or $E000 (8 or 16 KB)";
        if (n < 0x2000) {                       /* (repeated to 8 KB) */
            u8 *s;
            if (shorts == sizeof(short_chip) / sizeof(short_chip[0]))
                return "a .crt cartridge with too many small chips";
            s = short_chip[shorts++];
            for (u32 i = 0; i < 0x2000; i++)
                s[i] = data[i % n];
            data = s;
        }
        if (addr == 0x8000)
            cart.lo[bank] = data;
        if (addr != 0x8000 || n == 0x4000)
            cart.hi[bank] = addr == 0x8000 ? data + 0x2000 : data;
        if (bank > top)
            top = bank;
        cart.size += n;
    }
    if (!cart.size)
        return "a .crt cartridge with no ROM";
    cart.mask = 0;
    while (cart.mask < top)
        cart.mask = (u8)(cart.mask << 1 | 1);
    cart.on = 1;
    return 0;
}

const char *c64_load(const u8 *kernal8k, const u8 *basic8k, const u8 *chargen4k, const u8 *file, u32 size,
                     const char *name)
{
    static const u8 load_code[6] = {0x85, 0x93, 0xA9, 0x00, 0x85, 0x90};      /* STA $93, LDA #0, STA $90 */
    static const u8 wait_code[4] = {0xA5, 0xC6, 0x85, 0xCC};                  /* LDA $C6, STA $CC */
    const char *why = 0;
    char *p;
    if (!kernal8k || kernal8k[0x1FFD] < 0xE0)
        return "no KERNAL ROM (8 KB, its reset address in $E000-$FFFF)";
    if (!basic8k || basic8k[4] != 'C' || basic8k[5] != 'B' || basic8k[6] != 'M')
        return "no BASIC ROM (8 KB, CBMBASIC at its start)";
    if (!chargen4k)
        return "no character ROM (4 KB)";
    kernal = kernal8k, basic = basic8k, chargen = chargen4k;
    static const u16 jumps[8][2] = {{0xFFB4, 0xED09}, {0xFFB1, 0xED0C}, {0xFF93, 0xEDB9}, {0xFF96, 0xEDC7},
                                    {0xFFA8, 0xEDDD}, {0xFFA5, 0xEE13}, {0xFFAB, 0xEDEF}, {0xFFAE, 0xEDFE}};
    trap_ok = wait_ok = 1;                      /* (the 3 revisions of the KERNAL have them all there) */
    for (int i = 0; i < 6; i++)
        trap_ok &= kernal[LOAD_ENTRY - 0xE000 + i] == load_code[i];
    for (int i = 0; i < 8; i++) {               /* (its jump table: TALK, LISTEN, ... where they are known) */
        const u8 *j = kernal + jumps[i][0] - 0xE000;
        trap_ok &= j[0] == 0x4C && (j[1] | j[2] << 8) == jumps[i][1];
    }
    for (int i = 0; i < 4; i++)
        wait_ok &= kernal[WAIT_FIRST - 0xE000 + i] == wait_code[i];
    build_modes();
    nfiles = 0;
    kind = NO_FILE;
    disk = 0;
    name_len = 0;
    for (int i = 0; i < 5; i++)
        disk_id[i] = (u8)"00 2A"[i];
    cart.on = 0;
    ultimax = 0;
    hid_swap = 0;                               /* (F9 in the game before: this one's joystick is in port 2
                                                   again, as its start says) */
    if (file && size >= 0x40 && file[0] == 'C' && file[1] == '6' && file[2] == '4' && file[3] == ' ' &&
        file[4] == 'C' && file[5] == 'A' && file[6] == 'R' && file[7] == 'T') {
        why = from_crt(file, size);             /* (a cartridge: by its header, whatever its name) */
        if (why) {
            cart.on = 0;
            return why;
        }
    } else if (file && size) {
        if (ends_with(name, ".t64") || (size >= 64 && file[0] == 'C' && file[1] == '6' && file[2] == '4' &&
                                        !ends_with(name, ".prg") && file[3] != 'F'))
            why = from_t64(file, size), kind = T64;
        else if (ends_with(name, ".d64") || ends_with(name, ".d41"))
            why = from_d64(file, size), kind = D64;
        else if (ends_with(name, ".p00"))
            why = from_p00(file, size), kind = PRG;
        else
            why = from_prg(file, size, name), kind = PRG;
        if (why) {
            nfiles = 0;
            kind = NO_FILE;
            return why;
        }
    }
    c64_reset();
    switch (kind) {
    case PRG:
        p = put(info, "a .prg file: ");
        p = put_name(p, files[0].name, files[0].len);
        p = put(p, " ($");
        for (int k = 12; k >= 0; k -= 4)
            *p++ = "0123456789ABCDEF"[(files[0].addr >> k) & 15];
        p = put(p, ", ");
        p = put_number(p, (files[0].size + 1023) >> 10);
        put(p, " KB)");
        break;
    case T64:
        p = put(info, "a .t64 tape, ");
        p = put_number(p, (u32)nfiles);
        p = put(p, nfiles == 1 ? " file: " : " files: ");
        p = put_name(p, files[0].name, files[0].len);
        p = put(p, " (");
        p = put_number(p, (files[0].size + 1023) >> 10);
        put(p, " KB)");
        break;
    case D64:
        p = put(info, "a .d64 disk (");
        p = put_number(p, tracks);
        p = put(p, " tracks): ");
        p = put_name(p, disk_name, name_len);
        p = put(p, ", ");
        p = put_number(p, (u32)nfiles);
        p = put(p, nfiles == 1 ? " file" : " files");
        if (nfiles) {
            p = put(p, ", the first ");
            put_name(p, files[0].name, files[0].len);
        }
        break;
    default:
        if (!cart.on) {
            put(info, "BASIC");
            break;
        }
        p = put(info, "a cartridge (");
        p = put(p, cart_names[cart.type]);
        if (cart.mask) {
            p = put(p, ", ");
            p = put_number(p, (u32)cart.mask + 1);
            p = put(p, " banks");
        }
        p = put(p, "): ");
        {
            int n = 0;
            while (n < 32 && cart.name[n])
                n++;
            p = put_name(p, cart.name, n);
        }
        p = put(p, " (");
        p = put_number(p, (cart.size + 1023) >> 10);
        put(p, " KB)");
        break;
    }
    return 0;
}

const char *c64_info(void)
{
    return info;
}

u8 *c64_save_ram(u32 *size, int *battery)
{
    *size = 0;
    *battery = 0;
    return ram;
}

int c64_save_changed(void)
{
    return 0;
}

u32 c64_cpu_cycles(void)
{
    return frames_done * FRAME_CYCLES + cpu.cyc;
}

int c64_jammed(void)
{
    return cpu.jam;
}

u8 c64_peek(u16 a)
{
    now = cpu.cyc;
    return rdp[a >> 8] ? rdp[a >> 8][a & 0xFF] : a >= 0xD800 && a < 0xDC00 ? colram[a & 0x3FF] : 0xFF;
}

void c64_poke(u16 a, u8 v)
{
    now = cpu.cyc;
    wr(a, v);
}

void c64_registers(u16 *pc, u8 *a, u8 *x, u8 *y, u8 *s, u8 *p)
{
    *pc = cpu.pc, *a = cpu.a, *x = cpu.x, *y = cpu.y, *s = cpu.s, *p = cpu.p;
}

u32 c64_raster(void)
{
    now = cpu.cyc;
    return raster_now();
}

const char *c64_last_load(int *found)
{
    static char text[28];
    char *p = text;
    for (int i = 0; i < asked_len; i++) {
        u8 c = asked[i];
        *p++ = (char)(c >= 0x20 && c < 0x7F ? c : '?');
    }
    *p = 0;
    *found = asked_found;
    return text;
}

#ifdef C64_CPU_TEST
u8 c64_test_memory[65536];

void c64_test_start(u16 pc)
{
    build_modes();
    for (int i = 0; i < 256; i++) {
        rdp[i] = c64_test_memory + (i << 8);
        wrp[i] = c64_test_memory + (i << 8);
    }
    stack = c64_test_memory + 0x100;
    trap_ok = 0;
    cpu.a = cpu.x = cpu.y = 0;
    cpu.s = 0xFD;
    cpu.p = FI | FU;
    cpu.jam = cpu.delay = 0;
    cpu.cyc = 0;
    cpu.pc = pc;
}

void c64_test_step(u16 *pc, u32 *cycles)
{
    step();
    *pc = cpu.pc;
    *cycles = cpu.cyc;
}
#endif
