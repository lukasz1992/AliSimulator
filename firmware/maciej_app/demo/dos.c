/* An IBM PC AT with MS-DOS: see dos.h.

   The CPU: an 80286 in real mode, interpreted.  The flags are kept in
   three parts: CF, AF and OF where FLAGS has them (fl), ZF and SF from
   the last result sign-extended to 32 bits (zs: ZF when zs << 1 is 0, SF
   when it is negative -- 0x80000000 is both, for a POPF that sets both),
   PF from the parity of a byte (pb).  The cycles are about the 286's:
   each opcode's own count, 3 more for a memory operand, a REP's elements
   counted one by one.  As on a 286: PUSH SP pushes the SP before it, the
   shift counts are taken modulo 32, PUSHF's bits 12-15 are 0, a divide
   error returns to the DIV itself, and the 286's own opcodes (0F xx) in
   real mode are SMSW / LMSW; 386 opcodes are invalid (INT 6).

   The time: the CPU runs 12 cycles to each of the 8253 timer's (1.193182
   MHz), so a 14.3 MHz 286 (Magicland Dizzy needs about 10.4 MHz of it in
   its busiest screens to keep its 25 frames a second: its main loop waits
   for two ticks of the 50 Hz timer it sets); a frame is the VGA's 449
   lines at 31.47 kHz, 17024 timer ticks (14268 us).  Its first 400 lines
   are the picture (200 lines each drawn twice), then the blanking; the
   vertical retrace bit of 0x3DA is set for lines 412-431, the display-
   enable one in each line's last fifth and the blanking.  The picture is
   made when the blanking starts, from video memory and the registers as
   they are then.  The timer interrupt comes at its own times through the
   frame (channel 0 counts whatever the program loads in it); the
   keyboard's next byte, when it has one, is put in port 0x60 sixteen times
   a frame.  A loop that only waits for an interrupt is seen (see idle())
   and its time passed over at once: the host does the program's work, not
   its waiting.

   The BIOS and DOS: each interrupt vector points at a stub in F000 --
   F1 nn (an opcode the 286 does not use: here, "the BIOS / DOS function
   nn") then IRET; the timer's and the keyboard's stubs then send the 8259
   its EOI, the timer's calls INT 1Ch, as a BIOS does.  Their results in CF
   and ZF are put in the FLAGS the INT pushed.  A function that waits (a
   key from INT 16h or DOS) runs its F1 again with interrupts on and the
   CPU halted until the next interrupt.  DOS keeps its memory as DOS does,
   in memory control blocks from segment 0x100 (so a program's own MCB
   walks work); the program is loaded as COMMAND.COM would (its PSP at
   0x112, its relocations, its environment with its path, C:\NAME.EXE);
   files are read from the ones given, writes to them are taken and
   forgotten, new files cannot be made.

   The VGA: video memory in the 256-colour mode 13h (chain-4) and the text
   / CGA modes is plain memory (A0000 / B8000); in the planar modes (EGA's
   0Dh, 0Eh, 10h, 12h and an unchained 256-colour mode) A0000-AFFFF goes
   through the graphics controller: the four planes, the latches, the map
   mask, write modes 0-3 with the set / reset, the logical function, the
   rotation and the bit mask, read modes 0 and 1.  The picture goes through
   the attribute controller's palette (16-colour modes) and the DAC.  The
   CRTC's start address, line width and the panning are used; the line
   compare (a split screen) is not.  Not done: the sound (the speaker's
   timer channel counts, nothing is heard), a joystick (port 0x201 reads
   as none), the mouse (INT 33h: no driver), EMS / XMS, the FPU. */
#include "dos.h"
#include "font5x7.h"

typedef signed char s8;
typedef signed short s16;

#define INLINE static inline __attribute__((always_inline))

/* ---- time ---- */
#ifndef PIT_CYCLES
#define PIT_CYCLES   12                 /* the CPU's cycles to one of the timer's: 14.318 MHz */
#endif
#define FRAME_PIT    17024              /* the timer's ticks in a frame */
#define FRAME_CYCLES (FRAME_PIT * PIT_CYCLES)
#define LINES        449
#define SHOWN        400                /* the lines with the picture */
#define RETRACE_FROM 412
#define RETRACE_TO   432
#define DRAW_AT      (SHOWN * (FRAME_CYCLES / LINES))
#define SLICES       16                 /* the keyboard's next byte looked at this often */

/* ---- memory ---- */
static u8 mem[0x100000 + 16];           /* the 1 MB the 8086 reaches (A20 off: FFFF:0010 and on wrap to 0) */
static u32 vga_lo = 0xA0000, vga_n;     /* the planar video memory's window when it is planar (vga_n 0: none) */
static u8 vga_read(u32 a);
static void vga_write(u32 a, u8 v);
static void hi_write(u32 a, u32 v);
static u32 writes;                      /* memory writes and port accesses, counted (for idle()) */

INLINE u8 rd8(u32 a)
{
    a &= 0xFFFFF;
    if (a - vga_lo < vga_n)
        return vga_read(a);
    return mem[a];
}

INLINE void wr8(u32 a, u32 v)
{
    a &= 0xFFFFF;
    writes++;
    if (a < 0xA0000)
        mem[a] = (u8)v;
    else
        hi_write(a, v);
}

static u16 rd16(u32 base, u16 off)
{
    u32 a = (base + off) & 0xFFFFF;
    if (off == 0xFFFF || a >= 0x9FFFF)
        return (u16)(rd8(base + off) | rd8(base + (u16)(off + 1)) << 8);
    return (u16)(mem[a] | mem[a + 1] << 8);
}

static void wr16(u32 base, u16 off, u32 v)
{
    u32 a = (base + off) & 0xFFFFF;
    if (off == 0xFFFF || a >= 0x9FFFF) {
        wr8(base + off, v);
        wr8(base + (u16)(off + 1), v >> 8);
        return;
    }
    writes++;
    mem[a] = (u8)v;
    mem[a + 1] = (u8)(v >> 8);
}

static u16 get16(u32 a)                 /* (plain memory: the BIOS's and DOS's own reads) */
{
    return (u16)(mem[a & 0xFFFFF] | mem[(a + 1) & 0xFFFFF] << 8);
}

static void put16(u32 a, u32 v)
{
    mem[a & 0xFFFFF] = (u8)v;
    mem[(a + 1) & 0xFFFFF] = (u8)(v >> 8);
}

/* ---- the CPU ---- */
enum { ES, CS, SS, DS };
#define F_CF 0x0001
#define F_PF 0x0004
#define F_AF 0x0010
#define F_ZF 0x0040
#define F_SF 0x0080
#define F_TF 0x0100
#define F_IF 0x0200
#define F_DF 0x0400
#define F_OF 0x0800

static struct {
    union {
        u16 w[8];                       /* AX CX DX BX SP BP SI DI */
        u8 b[16];                       /* AL AH CL CH DL DH BL BH (a little-endian host's halves) */
    } r;
    u16 sr[4];                          /* ES CS SS DS */
    u32 base[4];                        /* their segments' addresses */
    u16 ip, op_ip;                      /* op_ip: the instruction's start (its prefixes') */
    u32 fl;                             /* CF, AF, OF where FLAGS has them */
    s32 zs;                             /* ZF: zs << 1 == 0; SF: zs < 0 */
    u32 pb;                             /* PF: the parity of its low byte */
    u32 sys;                            /* TF, IF, DF where FLAGS has them */
    s32 cycles;                         /* since the frame's start */
    int halted;
    int check;                          /* look at the interrupt line before the next instruction (2: after it) */
} c;

#define rAX c.r.w[0]
#define rCX c.r.w[1]
#define rDX c.r.w[2]
#define rBX c.r.w[3]
#define rSP c.r.w[4]
#define rBP c.r.w[5]
#define rSI c.r.w[6]
#define rDI c.r.w[7]
#define rAL c.r.b[0]
#define rAH c.r.b[1]
#define rCL c.r.b[2]
#define rCH c.r.b[3]
#define rDL c.r.b[4]
#define rDH c.r.b[5]
#define rBL c.r.b[6]
#define rBH c.r.b[7]
#define R8(n) c.r.b[((n) & 3) << 1 | ((n) >> 2 & 1)]   /* a byte register by its number: AL CL DL BL AH CH DH BH */
#define R16(n) c.r.w[(n) & 7]

static u8 parity[256];                  /* 1: an even number of bits set */
static u32 ea_base;                     /* the operand's segment's address and offset */
static u16 ea_off;
static int sego;                        /* a segment prefix's register, or -1 */
static s32 run_end;                     /* run() stops here (made sooner when the timer is set) */
static u32 cycles_hi;
static u8 ended;                        /* the program ended (INT 21h 4Ch) */

static void set_seg(int s, u32 v)
{
    c.sr[s] = (u16)v;
    c.base[s] = (v & 0xFFFF) << 4;
}

INLINE u32 fetch8(void)
{
    return mem[(c.base[CS] + c.ip++) & 0xFFFFF];
}

INLINE u32 fetch16(void)
{
    u32 v = fetch8();
    return v | fetch8() << 8;
}

static void push(u32 v)
{
    rSP -= 2;
    wr16(c.base[SS], rSP, v);
}

static u32 pop(void)
{
    u32 v = rd16(c.base[SS], rSP);
    rSP += 2;
    return v;
}

static u32 get_flags(void)
{
    return (c.fl & (F_CF | F_AF | F_OF)) | (parity[c.pb & 0xFF] ? F_PF : 0) | (((u32)c.zs << 1) ? 0 : F_ZF) |
           (c.zs < 0 ? F_SF : 0) | c.sys | 0x0002;
}

static void set_flags(u32 f)
{
    c.fl = f & (F_CF | F_AF | F_OF);
    c.pb = (f & F_PF) ? 0 : 1;
    c.zs = (f & F_ZF) ? ((f & F_SF) ? (s32)0x80000000u : 0) : ((f & F_SF) ? -1 : 1);
    c.sys = f & (F_TF | F_IF | F_DF);
}

static void interrupt(u32 n)
{
    push(get_flags());
    c.sys &= ~(u32)(F_IF | F_TF);
    push(c.sr[CS]);
    push(c.ip);
    c.ip = get16(n * 4);
    set_seg(CS, get16(n * 4 + 2));
    c.cycles += 23;
}

/* the operand of a ModRM byte in memory: ea_base, ea_off */
static void ea(u32 m)
{
    u32 off;
    int s = DS;
    switch (m & 7) {
    case 0: off = (u32)rBX + rSI; break;
    case 1: off = (u32)rBX + rDI; break;
    case 2: off = (u32)rBP + rSI; s = SS; break;
    case 3: off = (u32)rBP + rDI; s = SS; break;
    case 4: off = rSI; break;
    case 5: off = rDI; break;
    case 6:
        if (!(m & 0xC0)) {
            off = fetch16();
            break;
        }
        off = rBP;
        s = SS;
        break;
    default: off = rBX; break;
    }
    if ((m & 0xC0) == 0x40)
        off += (u32)(s32)(s8)fetch8();
    else if ((m & 0xC0) == 0x80)
        off += fetch16();
    ea_base = c.base[sego >= 0 ? sego : s];
    ea_off = (u16)off;
    c.cycles += 3;
}

INLINE u32 get8(u32 m)
{
    if (m >= 0xC0)
        return R8(m);
    ea(m);
    return rd8(ea_base + ea_off);
}

INLINE u32 getw(u32 m)
{
    if (m >= 0xC0)
        return R16(m);
    ea(m);
    return rd16(ea_base, ea_off);
}

INLINE void put8(u32 m, u32 v)          /* (after get8 or ea: the same operand) */
{
    if (m >= 0xC0)
        R8(m) = (u8)v;
    else
        wr8(ea_base + ea_off, v);
}

INLINE void putw(u32 m, u32 v)
{
    if (m >= 0xC0)
        R16(m) = (u16)v;
    else
        wr16(ea_base, ea_off, v);
}

INLINE void ea_only(u32 m)
{
    if (m < 0xC0)
        ea(m);
}

/* ---- the ALU ---- */
static u32 add8(u32 a, u32 b, u32 carry)
{
    u32 r = a + b + carry;
    c.fl = (r >> 8 & 1) | ((a ^ b ^ r) & F_AF) | ((a ^ r) & (b ^ r) & 0x80) << 4;
    c.zs = (s8)r;
    c.pb = r;
    return r & 0xFF;
}

static u32 add16(u32 a, u32 b, u32 carry)
{
    u32 r = a + b + carry;
    c.fl = (r >> 16 & 1) | ((a ^ b ^ r) & F_AF) | ((a ^ r) & (b ^ r) & 0x8000) >> 4;
    c.zs = (s16)r;
    c.pb = r;
    return r & 0xFFFF;
}

static u32 sub8(u32 a, u32 b, u32 borrow)
{
    u32 r = a - b - borrow;
    c.fl = (r >> 8 & 1) | ((a ^ b ^ r) & F_AF) | ((a ^ b) & (a ^ r) & 0x80) << 4;
    c.zs = (s8)r;
    c.pb = r;
    return r & 0xFF;
}

static u32 sub16(u32 a, u32 b, u32 borrow)
{
    u32 r = a - b - borrow;
    c.fl = (r >> 16 & 1) | ((a ^ b ^ r) & F_AF) | ((a ^ b) & (a ^ r) & 0x8000) >> 4;
    c.zs = (s16)r;
    c.pb = r;
    return r & 0xFFFF;
}

INLINE u32 logic8(u32 r)
{
    c.fl = 0;
    c.zs = (s8)r;
    c.pb = r;
    return r & 0xFF;
}

INLINE u32 logic16(u32 r)
{
    c.fl = 0;
    c.zs = (s16)r;
    c.pb = r;
    return r & 0xFFFF;
}

static u32 alu8(u32 k, u32 a, u32 b)    /* ADD OR ADC SBB AND SUB XOR CMP */
{
    switch (k & 7) {
    case 0: return add8(a, b, 0);
    case 1: return logic8(a | b);
    case 2: return add8(a, b, c.fl & F_CF);
    case 3: return sub8(a, b, c.fl & F_CF);
    case 4: return logic8(a & b);
    case 6: return logic8(a ^ b);
    default: return sub8(a, b, 0);
    }
}

static u32 alu16(u32 k, u32 a, u32 b)
{
    switch (k & 7) {
    case 0: return add16(a, b, 0);
    case 1: return logic16(a | b);
    case 2: return add16(a, b, c.fl & F_CF);
    case 3: return sub16(a, b, c.fl & F_CF);
    case 4: return logic16(a & b);
    case 6: return logic16(a ^ b);
    default: return sub16(a, b, 0);
    }
}

static u32 inc8(u32 a)
{
    u32 r = (a + 1) & 0xFF;
    c.fl = (c.fl & F_CF) | ((a ^ r) & F_AF) | (r == 0x80 ? F_OF : 0);
    c.zs = (s8)r;
    c.pb = r;
    return r;
}

static u32 dec8(u32 a)
{
    u32 r = (a - 1) & 0xFF;
    c.fl = (c.fl & F_CF) | ((a ^ r) & F_AF) | (a == 0x80 ? F_OF : 0);
    c.zs = (s8)r;
    c.pb = r;
    return r;
}

static u32 inc16(u32 a)
{
    u32 r = (a + 1) & 0xFFFF;
    c.fl = (c.fl & F_CF) | ((a ^ r) & F_AF) | (r == 0x8000 ? F_OF : 0);
    c.zs = (s16)r;
    c.pb = r;
    return r;
}

static u32 dec16(u32 a)
{
    u32 r = (a - 1) & 0xFFFF;
    c.fl = (c.fl & F_CF) | ((a ^ r) & F_AF) | (a == 0x8000 ? F_OF : 0);
    c.zs = (s16)r;
    c.pb = r;
    return r;
}

/* ROL ROR RCL RCR SHL SHR SAL SAR on a byte (bits 8) or a word (16) */
static u32 shift(u32 k, u32 v, u32 n, u32 bits)
{
    u32 top = bits - 1, mask = (1u << bits) - 1, cf = c.fl & F_CF, of, r;
    n &= 31;
    if (!n)
        return v;
    c.cycles += n;
    if (n == 1 && (k & 7) == 3) {      /* (RCR 1 and SHR 1: a sprite shifter's, often) */
        r = v >> 1 | cf << top;
        c.fl = (c.fl & F_AF) | (v & 1) | ((v >> top ^ cf) & 1) << 11;
        return r;
    }
    if (n == 1 && (k & 7) == 5) {
        r = v >> 1;
        c.fl = (v & 1) | (v >> top & 1) << 11;
        c.zs = (s32)r;
        c.pb = r;
        return r;
    }
    switch (k & 7) {
    case 0:
        n &= top;
        r = (v << n | v >> (bits - n)) & mask;
        cf = r & 1;                     /* (a count of 8 or 16: the same byte, CF its bit 0) */
        of = (r >> top ^ r) & 1;
        break;
    case 1:
        n &= top;
        r = (v >> n | v << (bits - n)) & mask;
        cf = r >> top & 1;
        of = (r >> top ^ r >> (top - 1)) & 1;
        break;
    case 2:
        n %= bits + 1;
        if (!n)
            return v;
        r = v;
        for (u32 i = 0; i < n; i++) {
            u32 out = r >> top & 1;
            r = (r << 1 | cf) & mask;
            cf = out;
        }
        of = (r >> top ^ cf) & 1;
        break;
    case 3:
        n %= bits + 1;
        if (!n)
            return v;
        r = v;
        of = (r >> top ^ cf) & 1;
        for (u32 i = 0; i < n; i++) {
            u32 out = r & 1;
            r = r >> 1 | cf << top;
            cf = out;
        }
        break;
    case 5:
        cf = n <= bits ? v >> (n - 1) & 1 : 0;
        r = n < bits ? v >> n : 0;
        c.fl = cf | (v >> top & 1) << 11;
        c.zs = bits == 8 ? (s8)r : (s16)r;
        c.pb = r;
        return r;
    case 7: {
        s32 sv = bits == 8 ? (s8)v : (s16)v;
        cf = (u32)(sv >> (n < bits ? n - 1 : top)) & 1;
        r = (u32)(sv >> (n < bits ? n : top)) & mask;
        c.fl = cf;
        c.zs = bits == 8 ? (s8)r : (s16)r;
        c.pb = r;
        return r;
    }
    default:                            /* SHL, SAL */
        cf = n <= bits ? v << n >> bits & 1 : 0;
        r = n < bits ? v << n & mask : 0;
        c.fl = cf | ((r >> top ^ cf) & 1) << 11;
        c.zs = bits == 8 ? (s8)r : (s16)r;
        c.pb = r;
        return r;
    }
    c.fl = (c.fl & F_AF) | cf | of << 11;
    return r;
}

static int cond(u32 k)
{
    int r;
    switch (k >> 1 & 7) {
    case 0: r = (c.fl & F_OF) != 0; break;
    case 1: r = c.fl & F_CF; break;
    case 2: r = !((u32)c.zs << 1); break;
    case 3: r = (c.fl & F_CF) || !((u32)c.zs << 1); break;
    case 4: r = c.zs < 0; break;
    case 5: r = parity[c.pb & 0xFF]; break;
    case 6: r = (c.zs < 0) != ((c.fl & F_OF) != 0); break;
    default: r = !((u32)c.zs << 1) || ((c.zs < 0) != ((c.fl & F_OF) != 0)); break;
    }
    return r ^ (int)(k & 1);
}

static void divide_error(void)
{
    c.ip = c.op_ip;                     /* (the 286: back to the DIV) */
    interrupt(0);
}

/* F6 / F7: TEST NOT NEG MUL IMUL DIV IDIV */
static void group3(u32 op)
{
    u32 m = fetch8(), k = m >> 3 & 7, v, r;
    if (op == 0xF6) {
        v = get8(m);
        switch (k) {
        case 0: case 1: logic8(v & fetch8()); break;
        case 2: put8(m, ~v); break;
        case 3: put8(m, sub8(0, v, 0)); c.fl = (c.fl & ~(u32)F_CF) | (v != 0); break;
        case 4:
            r = rAL * v;
            rAX = (u16)r;
            c.fl = (r >> 8) ? F_CF | F_OF : 0;
            c.zs = (s8)r;
            c.pb = r;
            c.cycles += 11;
            break;
        case 5: {
            s32 p = (s8)rAL * (s8)v;
            rAX = (u16)p;
            c.fl = p != (s8)p ? F_CF | F_OF : 0;
            c.zs = (s8)p;
            c.pb = (u32)p;
            c.cycles += 11;
            break;
        }
        case 6: {
            u32 n = rAX;
            if (!v || n / v > 0xFF) {
                divide_error();
                return;
            }
            rAL = (u8)(n / v);
            rAH = (u8)(n % v);
            c.cycles += 14;
            break;
        }
        default: {
            s32 n = (s16)rAX, d = (s8)v;
            if (!d || n / d > 127 || n / d < -128) {
                divide_error();
                return;
            }
            rAL = (u8)(n / d);
            rAH = (u8)(n % d);
            c.cycles += 17;
            break;
        }
        }
        return;
    }
    v = getw(m);
    switch (k) {
    case 0: case 1: logic16(v & fetch16()); break;
    case 2: putw(m, ~v); break;
    case 3: putw(m, sub16(0, v, 0)); c.fl = (c.fl & ~(u32)F_CF) | (v != 0); break;
    case 4:
        r = (u32)rAX * v;
        rAX = (u16)r;
        rDX = (u16)(r >> 16);
        c.fl = (r >> 16) ? F_CF | F_OF : 0;
        c.zs = (s16)r;
        c.pb = r;
        c.cycles += 19;
        break;
    case 5: {
        s32 p = (s16)rAX * (s16)v;
        rAX = (u16)p;
        rDX = (u16)((u32)p >> 16);
        c.fl = p != (s16)p ? F_CF | F_OF : 0;
        c.zs = (s16)p;
        c.pb = (u32)p;
        c.cycles += 19;
        break;
    }
    case 6: {
        u32 n = (u32)rDX << 16 | rAX;
        if (!v || n / v > 0xFFFF) {
            divide_error();
            return;
        }
        rAX = (u16)(n / v);
        rDX = (u16)(n % v);
        c.cycles += 20;
        break;
    }
    default: {
        s32 n = (s32)((u32)rDX << 16 | rAX), d = (s16)v;
        if (!d || (d == -1 && n == (s32)0x80000000u) || n / d > 32767 || n / d < -32768) {
            divide_error();
            return;
        }
        rAX = (u16)(n / d);
        rDX = (u16)(n % d);
        c.cycles += 23;
        break;
    }
    }
}

static u32 imul3(u32 a, u32 b)          /* IMUL r16, r/m16, imm */
{
    s32 p = (s16)a * (s16)b;
    c.fl = p != (s16)p ? F_CF | F_OF : 0;
    c.zs = (s16)p;
    c.pb = (u32)p;
    c.cycles += 18;
    return (u32)p & 0xFFFF;
}

static u8 in8(u32 port);
static void out8(u32 port, u32 v);

static u32 in16(u32 port)
{
    u32 lo = in8(port);
    return lo | (u32)in8(port + 1) << 8;
}

static void out16(u32 port, u32 v)
{
    out8(port, v & 0xFF);
    out8(port + 1, v >> 8 & 0xFF);
}

/* MOVS CMPS STOS LODS SCAS INS OUTS, with a REP prefix (rep: F2 / F3) */
static void string(u32 op, u32 rep)
{
    u32 src = c.base[sego >= 0 ? sego : DS], dst = c.base[ES];
    int step = (c.sys & F_DF) ? -1 : 1, w = op & 1;
    u16 d = (u16)(w ? step * 2 : step);
    if (rep && rCX && step > 0 && (op == 0xA4 || op == 0xA5 || op == 0xAA || op == 0xAB)) {
        /* REP MOVS / STOS forward, within their segments, from plain memory to plain memory or the planes */
        u32 n = (u32)rCX << w, s = (src + rSI) & 0xFFFFF, t = (dst + rDI) & 0xFFFFF, movs = op < 0xA8;
        int plain = t + n <= 0xA0000, planes = vga_n && t >= vga_lo && t + n <= vga_lo + vga_n;
        if (rDI + n <= 0x10000 && (!movs || (rSI + n <= 0x10000 && s + n <= 0xA0000 && !(w && t == s + 1))) &&
            (plain || planes)) {
            for (u32 i = 0; i < n; i++) {
                u8 b = movs ? mem[s + i] : (u8)(i & w ? rAH : rAL);
                if (plain)
                    mem[t + i] = b;
                else
                    vga_write(t + i, b);
            }
            if (movs)
                rSI = (u16)(rSI + n);
            rDI = (u16)(rDI + n);
            c.cycles += 4 * (s32)rCX;
            rCX = 0;
            writes++;
            return;
        }
    }
    for (;;) {
        if (rep && !rCX)
            break;
        switch (op) {
        case 0xA4: wr8(dst + rDI, rd8(src + rSI)); rSI += d; rDI += d; break;
        case 0xA5: wr16(dst, rDI, rd16(src, rSI)); rSI += d; rDI += d; break;
        case 0xA6: sub8(rd8(src + rSI), rd8(dst + rDI), 0); rSI += d; rDI += d; break;
        case 0xA7: sub16(rd16(src, rSI), rd16(dst, rDI), 0); rSI += d; rDI += d; break;
        case 0xAA: wr8(dst + rDI, rAL); rDI += d; break;
        case 0xAB: wr16(dst, rDI, rAX); rDI += d; break;
        case 0xAC: rAL = rd8(src + rSI); rSI += d; break;
        case 0xAD: rAX = rd16(src, rSI); rSI += d; break;
        case 0xAE: sub8(rAL, rd8(dst + rDI), 0); rDI += d; break;
        case 0xAF: sub16(rAX, rd16(dst, rDI), 0); rDI += d; break;
        case 0x6C: wr8(dst + rDI, in8(rDX)); rDI += d; break;
        case 0x6D: wr16(dst, rDI, in16(rDX)); rDI += d; break;
        case 0x6E: out8(rDX, rd8(src + rSI)); rSI += d; break;
        default: out16(rDX, rd16(src, rSI)); rSI += d; break;
        }
        if (!rep)
            break;
        c.cycles += 4;
        rCX--;
        if ((op & 0xF6) == 0xA6 && (rep == 0xF3 ? ((u32)c.zs << 1) != 0 : ((u32)c.zs << 1) == 0))
            break;                      /* (CMPS / SCAS: REPE ends on a difference, REPNE on a match) */
    }
}

static void hle(u32 n);
static void note(const char *what, u32 a, u32 b);

static void invalid(void)
{
    note("invalid opcode at", (u32)c.sr[CS] << 16 | c.op_ip, mem[(c.base[CS] + c.op_ip) & 0xFFFFF]);
    c.ip = c.op_ip;
    interrupt(6);
}

/* the 286's 0F opcodes, in real mode */
static void op_0f(void)
{
    u32 op = fetch8(), m;
    if (op == 0x01) {
        m = fetch8();
        if ((m >> 3 & 7) == 4) {        /* SMSW: real mode, no FPU */
            ea_only(m);
            putw(m, 0xFFF0);
            return;
        }
        if ((m >> 3 & 7) == 6) {        /* LMSW: protected mode is not here */
            u32 v = getw(m);
            if (v & 1) {
                note("LMSW into protected mode at", (u32)c.sr[CS] << 16 | c.op_ip, v);
                ended = 1;
                c.halted = 1;
                c.sys &= ~(u32)F_IF;
            }
            return;
        }
        ea_only(m);                     /* (SGDT, SIDT, LGDT, LIDT: taken and forgotten) */
        return;
    }
    if (op == 0x06)                     /* CLTS */
        return;
    invalid();
}

static const u8 cyc[256] = {
    /* 0x00 */ 2, 2, 2, 2, 3, 3, 3, 5, 2, 2, 2, 2, 3, 3, 3, 0,
    /* 0x10 */ 2, 2, 2, 2, 3, 3, 3, 5, 2, 2, 2, 2, 3, 3, 3, 5,
    /* 0x20 */ 2, 2, 2, 2, 3, 3, 0, 3, 2, 2, 2, 2, 3, 3, 0, 3,
    /* 0x30 */ 2, 2, 2, 2, 3, 3, 0, 3, 2, 2, 2, 2, 3, 3, 0, 3,
    /* 0x40 */ 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    /* 0x50 */ 3, 3, 3, 3, 3, 3, 3, 3, 5, 5, 5, 5, 5, 5, 5, 5,
    /* 0x60 */ 17, 19, 13, 2, 2, 2, 2, 2, 3, 21, 3, 21, 5, 5, 5, 5,
    /* 0x70 */ 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    /* 0x80 */ 3, 3, 3, 3, 2, 2, 3, 3, 2, 2, 2, 2, 2, 3, 2, 5,
    /* 0x90 */ 3, 3, 3, 3, 3, 3, 3, 3, 2, 2, 13, 3, 3, 5, 2, 2,
    /* 0xA0 */ 5, 5, 3, 3, 5, 5, 8, 8, 3, 3, 3, 3, 5, 5, 7, 7,
    /* 0xB0 */ 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    /* 0xC0 */ 5, 5, 11, 11, 7, 7, 3, 3, 11, 5, 15, 15, 23, 23, 3, 17,
    /* 0xD0 */ 2, 2, 5, 5, 16, 14, 2, 5, 2, 2, 2, 2, 2, 2, 2, 2,
    /* 0xE0 */ 8, 8, 8, 8, 5, 5, 3, 3, 7, 7, 11, 7, 5, 5, 3, 3,
    /* 0xF0 */ 0, 0, 0, 0, 2, 2, 3, 3, 2, 2, 2, 2, 2, 2, 3, 3,
};

/* a loop that waits for an interrupt: a backward jump taken to the same place as the last one, with every
   register and flag as they were then and nothing written to memory or a port in between (the game's wait for
   its timer's ticks: CMP [ticks], 2 / JB back) -- nothing can change until an interrupt comes, so the time up
   to the next thing that happens (run_end: the timer's next interrupt, the keyboard's next byte, the picture)
   goes by at once.  A loop that reads a port (the retrace's) is not one: IN counts as a write. */
static struct {
    u32 writes, cs_ip, fl, pb, sys;
    s32 zs;
    u16 r[8], sr[4];
} was;
static u32 idle_cycles;

static void idle(void)
{
    u32 cs_ip = (u32)c.sr[CS] << 16 | c.ip;
    if (cs_ip == was.cs_ip && writes == was.writes && c.fl == was.fl && c.zs == was.zs && c.pb == was.pb &&
        c.sys == was.sys) {
        int same = 1;
        for (int i = 0; i < 8 && same; i++)
            same = c.r.w[i] == was.r[i];
        for (int i = 0; i < 4 && same; i++)
            same = c.sr[i] == was.sr[i];
        if (same && (c.sys & F_IF) && c.cycles < run_end) {
            idle_cycles += (u32)(run_end - c.cycles);
            c.cycles = run_end;
            return;
        }
    }
    was.cs_ip = cs_ip, was.writes = writes, was.fl = c.fl, was.zs = c.zs, was.pb = c.pb, was.sys = c.sys;
    for (int i = 0; i < 8; i++)
        was.r[i] = c.r.w[i];
    for (int i = 0; i < 4; i++)
        was.sr[i] = c.sr[i];
}

/* one instruction */
static void step(void)
{
    u32 op, m, v, r, rep = 0;
    sego = -1;
    c.op_ip = c.ip;
#ifdef DOS_PROFILE
    DOS_PROFILE(c.sr[CS], c.ip);        /* (a PC harness's count of where the time goes) */
#endif
next:
    op = fetch8();
    c.cycles += cyc[op];
    switch (op) {
    case 0x00: case 0x08: case 0x10: case 0x18: case 0x20: case 0x28: case 0x30: case 0x38:
        m = fetch8();
        r = alu8(op >> 3, get8(m), R8(m >> 3));
        if (op != 0x38)
            put8(m, r);
        break;
    case 0x01: case 0x09: case 0x11: case 0x19: case 0x21: case 0x29: case 0x31: case 0x39:
        m = fetch8();
        r = alu16(op >> 3, getw(m), R16(m >> 3));
        if (op != 0x39)
            putw(m, r);
        break;
    case 0x02: case 0x0A: case 0x12: case 0x1A: case 0x22: case 0x2A: case 0x32: case 0x3A:
        m = fetch8();
        r = alu8(op >> 3, R8(m >> 3), get8(m));
        if (op != 0x3A)
            R8(m >> 3) = (u8)r;
        break;
    case 0x03: case 0x0B: case 0x13: case 0x1B: case 0x23: case 0x2B: case 0x33: case 0x3B:
        m = fetch8();
        r = alu16(op >> 3, R16(m >> 3), getw(m));
        if (op != 0x3B)
            R16(m >> 3) = (u16)r;
        break;
    case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C:
        r = alu8(op >> 3, rAL, fetch8());
        if (op != 0x3C)
            rAL = (u8)r;
        break;
    case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D:
        r = alu16(op >> 3, rAX, fetch16());
        if (op != 0x3D)
            rAX = (u16)r;
        break;
    case 0x06: case 0x0E: case 0x16: case 0x1E:
        push(c.sr[op >> 3 & 3]);
        break;
    case 0x07: case 0x17: case 0x1F:
        set_seg(op >> 3 & 3, pop());
        break;
    case 0x0F:
        op_0f();
        break;
    case 0x26: case 0x2E: case 0x36: case 0x3E:
        sego = op >> 3 & 3;
        goto next;
    case 0x27: {                        /* DAA */
        u32 a = rAL, cf = c.fl & F_CF, af = 0;
        if ((a & 0xF) > 9 || (c.fl & F_AF)) {
            rAL = (u8)(a + 6);
            af = F_AF;
        }
        if (a > 0x99 || cf) {
            rAL += 0x60;
            cf = 1;
        }
        c.fl = cf | af;
        c.zs = (s8)rAL;
        c.pb = rAL;
        break;
    }
    case 0x2F: {                        /* DAS */
        u32 a = rAL, cf = c.fl & F_CF, af = 0;
        if ((a & 0xF) > 9 || (c.fl & F_AF)) {
            rAL = (u8)(a - 6);
            af = F_AF;
        }
        if (a > 0x99 || cf) {
            rAL -= 0x60;
            cf = 1;
        }
        c.fl = cf | af;
        c.zs = (s8)rAL;
        c.pb = rAL;
        break;
    }
    case 0x37: case 0x3F:               /* AAA, AAS */
        if ((rAL & 0xF) > 9 || (c.fl & F_AF)) {
            rAX = (u16)(op == 0x37 ? rAX + 0x106 : rAX - 0x106);
            c.fl = F_AF | F_CF;
        } else {
            c.fl = 0;
        }
        rAL &= 0xF;
        c.zs = (s8)rAL;
        c.pb = rAL;
        break;
    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        R16(op) = (u16)inc16(R16(op));
        break;
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        R16(op) = (u16)dec16(R16(op));
        break;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        push(R16(op));                  /* (PUSH SP: the SP before, as on a 286) */
        break;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        v = pop();
        R16(op) = (u16)v;
        break;
    case 0x60: {                        /* PUSHA */
        u32 sp = rSP;
        for (int i = 0; i < 8; i++)
            push(i == 4 ? sp : c.r.w[i]);
        break;
    }
    case 0x61:                          /* POPA */
        for (int i = 7; i >= 0; i--) {
            v = pop();
            if (i != 4)
                c.r.w[i] = (u16)v;
        }
        break;
    case 0x62: {                        /* BOUND */
        m = fetch8();
        ea_only(m);
        s32 i = (s16)R16(m >> 3), lo = (s16)rd16(ea_base, ea_off), hi = (s16)rd16(ea_base, (u16)(ea_off + 2));
        if (i < lo || i > hi) {
            c.ip = c.op_ip;
            interrupt(5);
        }
        break;
    }
    case 0x68:
        push(fetch16());
        break;
    case 0x6A:
        push((u32)(s32)(s8)fetch8());
        break;
    case 0x69:
        m = fetch8();
        v = getw(m);
        R16(m >> 3) = (u16)imul3(v, fetch16());
        break;
    case 0x6B:
        m = fetch8();
        v = getw(m);
        R16(m >> 3) = (u16)imul3(v, (u32)(s32)(s8)fetch8());
        break;
    case 0x6C: case 0x6D: case 0x6E: case 0x6F:
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        string(op, rep);
        break;
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        v = (u32)(s32)(s8)fetch8();
        if (cond(op)) {
            c.ip = (u16)(c.ip + v);
            c.cycles += 4;
            if (v & 0x80000000u)
                idle();
        }
        break;
    case 0x80: case 0x82:
        m = fetch8();
        v = get8(m);
        r = alu8(m >> 3, v, fetch8());
        if ((m >> 3 & 7) != 7)
            put8(m, r);
        break;
    case 0x81:
        m = fetch8();
        v = getw(m);
        r = alu16(m >> 3, v, fetch16());
        if ((m >> 3 & 7) != 7)
            putw(m, r);
        break;
    case 0x83:
        m = fetch8();
        v = getw(m);
        r = alu16(m >> 3, v, (u32)(s32)(s8)fetch8() & 0xFFFF);
        if ((m >> 3 & 7) != 7)
            putw(m, r);
        break;
    case 0x84:
        m = fetch8();
        logic8(get8(m) & R8(m >> 3));
        break;
    case 0x85:
        m = fetch8();
        logic16(getw(m) & R16(m >> 3));
        break;
    case 0x86:
        m = fetch8();
        v = get8(m);
        put8(m, R8(m >> 3));
        R8(m >> 3) = (u8)v;
        break;
    case 0x87:
        m = fetch8();
        v = getw(m);
        putw(m, R16(m >> 3));
        R16(m >> 3) = (u16)v;
        break;
    case 0x88:
        m = fetch8();
        ea_only(m);
        put8(m, R8(m >> 3));
        break;
    case 0x89:
        m = fetch8();
        ea_only(m);
        putw(m, R16(m >> 3));
        break;
    case 0x8A:
        m = fetch8();
        R8(m >> 3) = (u8)get8(m);
        break;
    case 0x8B:
        m = fetch8();
        R16(m >> 3) = (u16)getw(m);
        break;
    case 0x8C:
        m = fetch8();
        ea_only(m);
        putw(m, c.sr[m >> 3 & 3]);
        break;
    case 0x8D:
        m = fetch8();
        ea_only(m);
        R16(m >> 3) = ea_off;
        break;
    case 0x8E:
        m = fetch8();
        v = getw(m);
        if ((m >> 3 & 3) != CS)
            set_seg(m >> 3 & 3, v);
        break;
    case 0x8F:
        m = fetch8();
        v = pop();
        ea_only(m);
        putw(m, v);
        break;
    case 0x90:
        break;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        v = rAX;
        rAX = R16(op);
        R16(op) = (u16)v;
        break;
    case 0x98:
        rAH = (rAL & 0x80) ? 0xFF : 0;
        break;
    case 0x99:
        rDX = (rAX & 0x8000) ? 0xFFFF : 0;
        break;
    case 0x9A: {
        u32 off = fetch16(), seg = fetch16();
        push(c.sr[CS]);
        push(c.ip);
        set_seg(CS, seg);
        c.ip = (u16)off;
        break;
    }
    case 0x9B:
        break;
    case 0x9C:
        push(get_flags());
        break;
    case 0x9D:
        set_flags(pop());
        c.check = 1;
        break;
    case 0x9E:
        set_flags((get_flags() & 0xFF00) | rAH);
        break;
    case 0x9F:
        rAH = (u8)get_flags();
        break;
    case 0xA0:
        rAL = rd8(c.base[sego >= 0 ? sego : DS] + fetch16());
        break;
    case 0xA1:
        rAX = rd16(c.base[sego >= 0 ? sego : DS], (u16)fetch16());
        break;
    case 0xA2:
        wr8(c.base[sego >= 0 ? sego : DS] + fetch16(), rAL);
        break;
    case 0xA3:
        wr16(c.base[sego >= 0 ? sego : DS], (u16)fetch16(), rAX);
        break;
    case 0xA8:
        logic8(rAL & fetch8());
        break;
    case 0xA9:
        logic16(rAX & fetch16());
        break;
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        R8(op) = (u8)fetch8();
        break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        R16(op) = (u16)fetch16();
        break;
    case 0xC0: case 0xD0: case 0xD2:
        m = fetch8();
        v = get8(m);
        put8(m, shift(m >> 3, v, op == 0xC0 ? fetch8() : op == 0xD0 ? 1 : rCL, 8));
        break;
    case 0xC1: case 0xD1: case 0xD3:
        m = fetch8();
        v = getw(m);
        putw(m, shift(m >> 3, v, op == 0xC1 ? fetch8() : op == 0xD1 ? 1 : rCL, 16));
        break;
    case 0xC2:
        v = fetch16();
        c.ip = (u16)pop();
        rSP += (u16)v;
        break;
    case 0xC3:
        c.ip = (u16)pop();
        break;
    case 0xC4: case 0xC5:
        m = fetch8();
        ea_only(m);
        R16(m >> 3) = rd16(ea_base, ea_off);
        set_seg(op == 0xC4 ? ES : DS, rd16(ea_base, (u16)(ea_off + 2)));
        break;
    case 0xC6:
        m = fetch8();
        ea_only(m);
        put8(m, fetch8());
        break;
    case 0xC7:
        m = fetch8();
        ea_only(m);
        putw(m, fetch16());
        break;
    case 0xC8: {                        /* ENTER */
        u32 size = fetch16(), level = fetch8() & 31, frame;
        push(rBP);
        frame = rSP;
        if (level) {
            for (u32 i = 1; i < level; i++) {
                rBP -= 2;
                push(rd16(c.base[SS], rBP));
            }
            push(frame);
        }
        rBP = (u16)frame;
        rSP = (u16)(rSP - size);
        break;
    }
    case 0xC9:
        rSP = rBP;
        rBP = (u16)pop();
        break;
    case 0xCA: case 0xCB:
        v = op == 0xCA ? fetch16() : 0;
        c.ip = (u16)pop();
        set_seg(CS, pop());
        rSP += (u16)v;
        break;
    case 0xCC:
        interrupt(3);
        break;
    case 0xCD:
        v = fetch8();
        interrupt(v);
        break;
    case 0xCE:
        if (c.fl & F_OF)
            interrupt(4);
        break;
    case 0xCF:
        c.ip = (u16)pop();
        set_seg(CS, pop());
        set_flags(pop());
        c.check = 1;
        break;
    case 0xD4:                          /* AAM */
        v = fetch8();
        if (!v) {
            divide_error();
            break;
        }
        rAH = (u8)(rAL / v);
        rAL = (u8)(rAL % v);
        logic8(rAL);
        break;
    case 0xD5:                          /* AAD */
        v = fetch8();
        rAX = (u16)logic8(rAL + rAH * v);
        break;
    case 0xD6:                          /* SALC */
        rAL = (c.fl & F_CF) ? 0xFF : 0;
        break;
    case 0xD7:
        rAL = rd8(c.base[sego >= 0 ? sego : DS] + (u16)(rBX + rAL));
        break;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        m = fetch8();                   /* (no FPU: its instructions do nothing) */
        ea_only(m);
        break;
    case 0xE0: case 0xE1: case 0xE2:
        v = (u32)(s32)(s8)fetch8();
        rCX--;
        if (rCX && (op == 0xE2 || (op == 0xE1) == !((u32)c.zs << 1)))
            c.ip = (u16)(c.ip + v);
        break;
    case 0xE3:
        v = (u32)(s32)(s8)fetch8();
        if (!rCX)
            c.ip = (u16)(c.ip + v);
        break;
    case 0xE4:
        rAL = in8(fetch8());
        break;
    case 0xE5:
        rAX = (u16)in16(fetch8());
        break;
    case 0xE6:
        out8(fetch8(), rAL);
        break;
    case 0xE7:
        out16(fetch8(), rAX);
        break;
    case 0xEC:
        rAL = in8(rDX);
        break;
    case 0xED:
        rAX = (u16)in16(rDX);
        break;
    case 0xEE:
        out8(rDX, rAL);
        break;
    case 0xEF:
        out16(rDX, rAX);
        break;
    case 0xE8:
        v = fetch16();
        push(c.ip);
        c.ip = (u16)(c.ip + v);
        break;
    case 0xE9:
        v = fetch16();
        c.ip = (u16)(c.ip + v);
        if (v & 0x8000)
            idle();
        break;
    case 0xEA: {
        u32 off = fetch16();
        set_seg(CS, fetch16());
        c.ip = (u16)off;
        break;
    }
    case 0xEB:
        v = (u32)(s32)(s8)fetch8();
        c.ip = (u16)(c.ip + v);
        if (v & 0x80000000u)
            idle();
        break;
    case 0xF0:
        goto next;
    case 0xF1:
        if (c.sr[CS] == 0xF000) {
            hle(fetch8());
            break;
        }
        goto next;
    case 0xF2: case 0xF3:
        rep = op;
        goto next;
    case 0xF4:
        c.halted = 1;
        break;
    case 0xF5:
        c.fl ^= F_CF;
        break;
    case 0xF6: case 0xF7:
        group3(op);
        break;
    case 0xF8:
        c.fl &= ~(u32)F_CF;
        break;
    case 0xF9:
        c.fl |= F_CF;
        break;
    case 0xFA:
        c.sys &= ~(u32)F_IF;
        break;
    case 0xFB:
        c.sys |= F_IF;
        c.check = 2;                    /* (the next instruction first) */
        break;
    case 0xFC:
        c.sys &= ~(u32)F_DF;
        break;
    case 0xFD:
        c.sys |= F_DF;
        break;
    case 0xFE:
        m = fetch8();
        v = get8(m);
        if ((m >> 3 & 7) == 0)
            put8(m, inc8(v));
        else if ((m >> 3 & 7) == 1)
            put8(m, dec8(v));
        else
            invalid();
        break;
    case 0xFF:
        m = fetch8();
        switch (m >> 3 & 7) {
        case 0: v = getw(m); putw(m, inc16(v)); break;
        case 1: v = getw(m); putw(m, dec16(v)); break;
        case 2: v = getw(m); push(c.ip); c.ip = (u16)v; c.cycles += 5; break;
        case 3: case 5:
            if (m >= 0xC0) {
                invalid();
                break;
            }
            ea(m);
            v = rd16(ea_base, ea_off);
            r = rd16(ea_base, (u16)(ea_off + 2));
            if ((m >> 3 & 7) == 3) {
                push(c.sr[CS]);
                push(c.ip);
                c.cycles += 10;
            }
            set_seg(CS, r);
            c.ip = (u16)v;
            break;
        case 4: c.ip = (u16)getw(m); c.cycles += 4; break;
        case 6: push(getw(m)); break;
        default: invalid(); break;
        }
        break;
    default:
        invalid();
        break;
    }
}

/* ---- the 8259 (the master; the slave's ports are taken and ignored) ---- */
static u8 pic_irr, pic_imr, pic_isr, pic_base, pic_init, pic_need4, pic_read_isr;

static void pic_raise(int n)
{
    pic_irr |= (u8)(1u << n);
    if (!c.check)
        c.check = 1;
}

static void irq_check(void)
{
    c.check = 0;
    if (!(c.sys & F_IF))
        return;
    u8 pending = pic_irr & ~pic_imr;
    for (int n = 0; n < 8 && pending; n++) {
        if (pic_isr & (1u << n))
            return;                     /* (one of a higher priority in service) */
        if (pending & (1u << n)) {
            pic_irr &= (u8)~(1u << n);
            pic_isr |= (u8)(1u << n);
            c.halted = 0;
            interrupt(pic_base + (u32)n);
            return;
        }
    }
}

static void pic_out(u32 port, u32 v)
{
    if (!(port & 1)) {
        if (v & 0x10) {                 /* ICW1 */
            pic_init = 1;
            pic_need4 = v & 1;
            pic_imr = 0;
            pic_isr = 0;
            pic_irr = 0;
        } else if (v & 0x08) {          /* OCW3 */
            if (v & 2)
                pic_read_isr = v & 1;
        } else if ((v & 0xE0) == 0x20) {       /* the non-specific EOI */
            for (int n = 0; n < 8; n++)
                if (pic_isr & (1u << n)) {
                    pic_isr &= (u8)~(1u << n);
                    break;
                }
        } else if ((v & 0xE0) == 0x60) {
            pic_isr &= (u8)~(1u << (v & 7));
        }
    } else if (pic_init == 1) {         /* ICW2: the vectors; ICW3 (a master), ICW4 */
        pic_base = (u8)(v & 0xF8);
        pic_init = 2;
    } else if (pic_init == 2) {
        pic_init = pic_need4 ? 3 : 0;
    } else if (pic_init == 3) {
        pic_init = 0;
    } else {
        pic_imr = (u8)v;
    }
    if (!c.check)
        c.check = 1;
}

/* ---- the 8253 timer ----
   A channel in mode 2 or 3 that is counting goes on counting when it is given a new count (or the same mode
   again): the count is taken at the end of the period (the Sound Images driver's delay loop writes its count
   again and again while it waits for the count to go down); the gate's rise (channel 2's, port 0x61 bit 0)
   starts a period again.  Other modes stop at the control word and start at the count. */
static u32 pit_n[3], pit_new[3];        /* the period's count (1..65536); one written for the next period (0: none) */
static u32 pit_mode[3], pit_access[3], pit_low[3], pit_latch[3];
static u8 pit_half[3], pit_latched[3], pit_read_hi[3], pit_running[3], pit_gate2;
static s32 pit_start[3];                /* the cycle its period started at (from the frame's start) */
static s32 pit0_next;                   /* channel 0's next interrupt */
static int pit0_on;

static int periodic(int ch)
{
    return pit_mode[ch] == 2 || pit_mode[ch] == 3;
}

static void pit_catch_up(int ch)        /* the periods ended by now: the next period's start, its count */
{
    if (!periodic(ch) || !pit_running[ch])
        return;
    s32 span = (s32)(pit_n[ch] * PIT_CYCLES);
    if (c.cycles - pit_start[ch] < span)
        return;
    pit_start[ch] += span;
    if (pit_new[ch]) {
        pit_n[ch] = pit_new[ch];
        pit_new[ch] = 0;
        span = (s32)(pit_n[ch] * PIT_CYCLES);
    }
    if (c.cycles - pit_start[ch] >= span)
        pit_start[ch] += (c.cycles - pit_start[ch]) / span * span;
}

static u32 pit_ticks(int ch)
{
    s32 t = c.cycles - pit_start[ch];
    return t < 0 ? 0 : (u32)t / PIT_CYCLES;
}

static u32 pit_count(int ch)
{
    u32 n = pit_n[ch];
    if (!pit_running[ch] || (ch == 2 && !pit_gate2))
        return n & 0xFFFF;
    pit_catch_up(ch);
    n = pit_n[ch];
    u32 t = pit_ticks(ch);
    switch (pit_mode[ch]) {
    case 2: return (n - t % n) & 0xFFFF;
    case 3: return (n - (t * 2) % n) & 0xFFFE;
    default: return (n - t) & 0xFFFF;
    }
}

static int pit_out2(void)
{
    if (!pit_gate2 || !pit_running[2])
        return 1;
    pit_catch_up(2);
    u32 n = pit_n[2], t = pit_ticks(2);
    if (pit_mode[2] == 3)
        return t % n < n / 2;
    if (pit_mode[2] == 2)
        return t % n != n - 1;
    return t >= n;
}

static void pit_start_now(int ch)
{
    pit_start[ch] = c.cycles;
    pit_running[ch] = 1;
    if (ch == 0) {
        pit0_next = c.cycles + (s32)(pit_n[0] * PIT_CYCLES);
        pit0_on = 1;
        run_end = c.cycles;             /* (run() looks again at when the next one comes) */
    }
}

static void pit_count_written(int ch, u32 v)
{
    u32 n = v ? v : 0x10000;
    if (periodic(ch) && pit_running[ch]) {
        pit_catch_up(ch);
        pit_new[ch] = n;
        return;
    }
    pit_n[ch] = n;
    pit_new[ch] = 0;
    pit_start_now(ch);
}

static void pit_out(u32 port, u32 v)
{
    if (port == 0x43) {
        int ch = (int)(v >> 6);
        if (ch == 3)
            return;                     /* (the 8254's read-back) */
        if (!(v & 0x30)) {              /* latch the count */
            pit_latch[ch] = pit_count(ch);
            pit_latched[ch] = 2;
            return;
        }
        u32 mode = v >> 1 & 7;
        if (mode > 5)
            mode -= 4;
        if (!(periodic(ch) && (mode == 2 || mode == 3))) {
            pit_running[ch] = 0;        /* (stopped until its count comes) */
            if (ch == 0)
                pit0_on = 0;
        }
        pit_access[ch] = v >> 4 & 3;
        pit_mode[ch] = mode;
        pit_half[ch] = 0;
        pit_read_hi[ch] = 0;
        return;
    }
    int ch = (int)(port & 3);
    switch (pit_access[ch]) {
    case 1: pit_count_written(ch, v); break;
    case 2: pit_count_written(ch, v << 8); break;
    default:
        if (!pit_half[ch]) {
            pit_low[ch] = v;
            pit_half[ch] = 1;
        } else {
            pit_half[ch] = 0;
            pit_count_written(ch, pit_low[ch] | v << 8);
        }
        break;
    }
}

static u8 pit_in(u32 port)
{
    int ch = (int)(port & 3);
    u32 v;
    if (ch == 3)
        return 0xFF;
    if (pit_latched[ch]) {
        v = pit_latch[ch];
        if (pit_access[ch] == 3) {
            pit_latched[ch]--;
            return (u8)(pit_latched[ch] ? v : v >> 8);
        }
        pit_latched[ch] = 0;
    } else {
        v = pit_count(ch);
        if (pit_access[ch] == 3) {
            pit_read_hi[ch] ^= 1;
            return (u8)(pit_read_hi[ch] ? v : v >> 8);
        }
    }
    return (u8)(pit_access[ch] == 2 ? v >> 8 : v);
}

/* ---- the keyboard controller (an 8042) ---- */
static u8 kbd_queue[64];
static int kq_head, kq_tail;
static u8 kbc_data, kbc_full, kbc_command, kbd_command, port61;

static void kbd_put(u32 b)
{
    int next = (kq_tail + 1) & 63;
    if (next != kq_head) {
        kbd_queue[kq_tail] = (u8)b;
        kq_tail = next;
    }
}

static void kbd_deliver(void)
{
    if (kbc_full || kq_head == kq_tail)
        return;
    kbc_data = kbd_queue[kq_head];
    kq_head = (kq_head + 1) & 63;
    kbc_full = 1;
    pic_raise(1);
}

static void kbc_out(u32 port, u32 v)
{
    if (port == 0x64) {
        kbc_command = (u8)v;
        if (v == 0x20) {                /* the command byte */
            kbc_data = 0x45;
            kbc_full = 1;
        } else if (v == 0xAA) {         /* the self test */
            kbc_data = 0x55;
            kbc_full = 1;
        }
        if (v != 0x60 && v != 0xD1)
            kbc_command = 0;
        return;
    }
    if (kbc_command) {                  /* (the command byte, the output port: A20 stays off) */
        kbc_command = 0;
        return;
    }
    if (kbd_command) {                  /* (the LEDs', the repeat rate's data) */
        kbd_command = 0;
        kbd_put(0xFA);
        return;
    }
    kbd_put(0xFA);
    if (v == 0xED || v == 0xF3)
        kbd_command = (u8)v;
    else if (v == 0xFF)
        kbd_put(0xAA);
    else if (v == 0xF2)
        kbd_put(0xAB), kbd_put(0x41);
}

/* ---- the CMOS clock ---- */
static u8 cmos_index;
static u32 bios_ticks(void);

static u8 bcd(u32 v)
{
    return (u8)((v / 10) << 4 | v % 10);
}

static u8 cmos_in(void)
{
    u32 s = (u32)((bios_ticks() * 10u) / 182u);
    switch (cmos_index & 0x7F) {
    case 0x00: return bcd(s % 60);
    case 0x02: return bcd(s / 60 % 60);
    case 0x04: return bcd(s / 3600 % 24);
    case 0x06: return 3;
    case 0x07: return 0x01;
    case 0x08: return 0x06;
    case 0x09: return 0x93;
    case 0x0A: return 0x26;
    case 0x0B: return 0x02;
    case 0x0D: return 0x80;
    case 0x10: return 0x40;             /* a 1.44 MB floppy drive */
    case 0x14: return 0x41;
    case 0x15: return 0x80;             /* 640 KB */
    case 0x16: return 0x02;
    case 0x32: return 0x19;
    default: return 0;
    }
}

/* ---- the VGA ---- */
enum { T_TEXT, T_CGA4, T_CGA2, T_PLANAR, T_VGA256 };
static u8 vram[4][65536];
static u8 latch[4];
static u8 seq[8], gc[16], crtc[32], attr[32], seq_i, gc_i, crtc_i, attr_i, attr_flip, misc_out;
static u8 dac[256][3], dac_w, dac_r, dac_c, dac_rc, pel_mask = 0xFF, cga_mode, cga_colour;
static u8 video_mode = 3, kind;
static u16 pal15[256];                  /* the DAC's colours as 15-bit ones */
static int dirty = 1, pal_dirty = 1;      /* the whole picture to be made again; the DAC changed */
static u8 vdirty[0x10000 >> 5];         /* the planes' 32-byte blocks written since the picture was made */

static void planar_on(void);

/* the graphics controller's write to the planes (offset o) of a CPU byte */
static void vga_write(u32 a, u8 v)
{
    u32 o = (a - 0xA0000) & 0xFFFF, mode = gc[5] & 3, mask = seq[2] & 15, bits = gc[8];
    u32 rot = gc[3] & 7, fn = gc[3] >> 3 & 3;
    vdirty[o >> 5] = 1;
    if (mode == 0 && !gc[1] && !gc[3] && bits == 0xFF) {     /* (the plain write: the byte to each plane enabled) */
        if (mask & 1)
            vram[0][o] = v;
        if (mask & 2)
            vram[1][o] = v;
        if (mask & 4)
            vram[2][o] = v;
        if (mask & 8)
            vram[3][o] = v;
        return;
    }
    if (mode == 1) {
        for (int p = 0; p < 4; p++)
            if (mask & (1u << p))
                vram[p][o] = latch[p];
        return;
    }
    if (rot && mode != 2)
        v = (u8)(v >> rot | v << (8 - rot));
    if (mode == 3) {
        bits &= v;
        for (int p = 0; p < 4; p++)
            if (mask & (1u << p)) {
                u32 s = (gc[0] >> p & 1) ? 0xFF : 0;
                vram[p][o] = (u8)((s & bits) | (latch[p] & ~bits));
            }
        return;
    }
    for (int p = 0; p < 4; p++) {
        if (!(mask & (1u << p)))
            continue;
        u32 d;
        if (mode == 2)
            d = (v >> p & 1) ? 0xFF : 0;
        else if (gc[1] & (1u << p))
            d = (gc[0] >> p & 1) ? 0xFF : 0;
        else
            d = v;
        switch (fn) {
        case 1: d &= latch[p]; break;
        case 2: d |= latch[p]; break;
        case 3: d ^= latch[p]; break;
        }
        vram[p][o] = (u8)((d & bits) | (latch[p] & ~bits));
    }
}

static u8 vga_read(u32 a)
{
    u32 o = (a - 0xA0000) & 0xFFFF;
    for (int p = 0; p < 4; p++)
        latch[p] = vram[p][o];
    if (gc[5] & 8) {                    /* read mode 1: the pixels of the compare colour */
        u32 r = 0xFF;
        for (int p = 0; p < 4; p++)
            if (gc[7] & (1u << p))
                r &= ~(latch[p] ^ ((gc[2] >> p & 1) ? 0xFFu : 0u));
        return (u8)r;
    }
    return latch[gc[4] & 3];
}

static void hi_write(u32 a, u32 v)      /* video memory, the ROMs (not written) */
{
    if (a - vga_lo < vga_n) {
        vga_write(a, (u8)v);
        return;
    }
    if (a < 0xC0000) {
        mem[a] = (u8)v;
        dirty = 1;
    }
}

/* the 256-colour mode's chain-4 bit changed: its pixels between plain memory and the planes */
static void chain4(int on)
{
    if (kind != T_VGA256)
        return;
    if (!on && !vga_n) {
        for (u32 i = 0; i < 0x10000; i++)
            vram[i & 3][i >> 2] = mem[0xA0000 + i];
        planar_on();
    } else if (on && vga_n) {
        for (u32 i = 0; i < 0x10000; i++)
            mem[0xA0000 + i] = vram[i & 3][i >> 2];
        vga_n = 0;
    }
    dirty = 1;
}

static void planar_on(void)
{
    vga_lo = 0xA0000;
    vga_n = 0x10000;
}

static u8 status1(void)                 /* 0x3DA: display enable (bit 0: not), vertical retrace (bit 3) */
{
    s32 t = c.cycles;
    if (t < 0)
        t = 0;
    t %= FRAME_CYCLES;
    u32 x = (u32)t * LINES, line = x / FRAME_CYCLES, in = x % FRAME_CYCLES;
    u8 v = 0;
    if (line >= SHOWN || in >= FRAME_CYCLES * 4 / 5)
        v |= 1;
    if (line >= RETRACE_FROM && line < RETRACE_TO)
        v |= 8;
    attr_flip = 0;
    /* the same instruction reading the same again with nothing else done in between (a loop waiting for the
       retrace): the time up to the next change passes at once, as idle() does for a loop without a port */
    static u32 last_at, last_writes;
    static u8 last_v;
    u32 at = (u32)c.sr[CS] << 16 | c.op_ip;
    if (at == last_at && v == last_v && writes == last_writes + 1) {
        u32 next;
        if (line < SHOWN)
            next = in < FRAME_CYCLES * 4 / 5 ? line * FRAME_CYCLES + FRAME_CYCLES * 4 / 5 : (line + 1) * FRAME_CYCLES;
        else
            next = (line < RETRACE_FROM ? RETRACE_FROM : line < RETRACE_TO ? RETRACE_TO : LINES) * FRAME_CYCLES;
        s32 skip = (s32)((next + LINES - 1) / LINES) - t;
        if (skip > run_end - c.cycles)
            skip = run_end - c.cycles;
        if (skip > 0) {
            c.cycles += skip;
            idle_cycles += (u32)skip;
        }
    }
    last_at = at, last_v = v, last_writes = writes;
    return v;
}

static void vga_out(u32 port, u32 v)
{
    switch (port) {
    case 0x3C0:
        if (!attr_flip) {
            attr_i = (u8)v;
        } else if ((attr_i & 0x1F) < 0x15) {
            attr[attr_i & 0x1F] = (u8)v;
            pal_dirty = 1;
        }
        attr_flip ^= 1;
        dirty = 1;
        break;
    case 0x3C2: misc_out = (u8)v; break;
    case 0x3C4: seq_i = (u8)(v & 7); break;
    case 0x3C5:
        if (seq_i == 4 && ((seq[4] ^ v) & 8))
            chain4((v & 8) != 0);
        seq[seq_i] = (u8)v;
        dirty = 1;
        break;
    case 0x3C6: pel_mask = (u8)v; dirty = 1; break;
    case 0x3C7: dac_r = (u8)v; dac_rc = 0; break;
    case 0x3C8: dac_w = (u8)v; dac_c = 0; break;
    case 0x3C9:
        dac[dac_w][dac_c] = (u8)(v & 0x3F);
        if (++dac_c == 3) {
            dac_c = 0;
            dac_w++;
        }
        pal_dirty = 1;
        dirty = 1;
        break;
    case 0x3CE: gc_i = (u8)(v & 15); break;
    case 0x3CF: gc[gc_i] = (u8)v; break;
    case 0x3B4: case 0x3D4: crtc_i = (u8)(v & 31); break;
    case 0x3B5: case 0x3D5: crtc[crtc_i] = (u8)v; dirty = 1; break;
    case 0x3D8: cga_mode = (u8)v; dirty = 1; break;
    case 0x3D9:                         /* the CGA's colour select, as the BIOS's AH=0Bh does it on a VGA */
        cga_colour = (u8)v;
        if (kind == T_CGA4) {
            u32 bright = (v & 0x10) ? 0x10 : 0, base = (v & 0x20) ? 3 : 2;
            attr[0] = (u8)((v & 7) | ((v & 8) ? 0x10 : 0));
            for (u32 i = 1; i < 4; i++)
                attr[i] = (u8)((base + 2 * (i - 1)) | bright);
            pal_dirty = 1;
        }
        dirty = 1;
        break;
    }
}

static u8 vga_in(u32 port)
{
    switch (port) {
    case 0x3C0: return attr_i;
    case 0x3C1: return attr[attr_i & 0x1F];
    case 0x3C2: return 0x10;
    case 0x3C4: return seq_i;
    case 0x3C5: return seq[seq_i];
    case 0x3C6: return pel_mask;
    case 0x3C7: return 0;
    case 0x3C8: return dac_w;
    case 0x3C9: {
        u8 v = dac[dac_r][dac_rc];
        if (++dac_rc == 3) {
            dac_rc = 0;
            dac_r++;
        }
        return v;
    }
    case 0x3CC: return misc_out;
    case 0x3CE: return gc_i;
    case 0x3CF: return gc[gc_i];
    case 0x3B4: case 0x3D4: return crtc_i;
    case 0x3B5: case 0x3D5: return crtc[crtc_i];
    case 0x3BA: case 0x3DA: return status1();
    default: return 0xFF;
    }
}

/* ---- ports ---- */
static int sound_port(u32 port)         /* an AdLib's (0x388) or a Sound Blaster's (0x2x0-0x2xF): none here */
{
    return (port & 0xFFFC) == 0x388 || (port & 0xFF00) == 0x200;
}

static u8 in8(u32 port)
{
    port &= 0xFFFF;
    c.cycles += 2;
    writes++;
    switch (port) {
    case 0x20: return pic_read_isr ? pic_isr : pic_irr;
    case 0x21: return pic_imr;
    case 0x40: case 0x41: case 0x42: case 0x43: return pit_in(port);
    case 0x60:
        kbc_full = 0;
        return kbc_data;
    case 0x61:                          /* bit 4: the memory refresh's toggle (15 us), bit 5: the speaker channel */
        return (u8)((port61 & 0x0F) | ((pit_ticks(0) / 18) & 1 ? 0x10 : 0) | (pit_out2() ? 0x20 : 0));
    case 0x64: return (u8)(0x14 | kbc_full);
    case 0x71: return cmos_in();
    case 0xA0: case 0xA1: return 0;
    case 0x201: return 0xFF;            /* (no joystick: its timers never end) */
    }
    if (port >= 0x3B0 && port < 0x3E0)
        return vga_in(port);
    if (!sound_port(port))
        note("IN from port", port, 0);
    return 0xFF;
}

static void out8(u32 port, u32 v)
{
    port &= 0xFFFF;
    v &= 0xFF;
    c.cycles += 2;
    writes++;
    switch (port) {
    case 0x20: case 0x21: pic_out(port, v); return;
    case 0x40: case 0x41: case 0x42: case 0x43: pit_out(port, v); return;
    case 0x60: case 0x64: kbc_out(port, v); return;
    case 0x61:
        if ((v & 1) && !(port61 & 1) && pit_running[2]) {
            pit_start[2] = c.cycles;    /* (the speaker channel's gate opens: a period from its start) */
            if (pit_new[2])
                pit_n[2] = pit_new[2], pit_new[2] = 0;
        }
        pit_gate2 = v & 1;
        port61 = (u8)v;
        return;
    case 0x70: cmos_index = (u8)v; return;
    case 0x71: case 0x80: case 0xA0: case 0xA1: case 0x201: case 0x3F2: return;
    }
    if (port >= 0x3B0 && port < 0x3E0) {
        vga_out(port, v);
        return;
    }
    if (!sound_port(port))
        note("OUT to port", port, v);
}

/* ---- the picture ---- */
static u16 *pic;
static u32 frames;
static u8 squeeze[256];                 /* an 8-pixel row of a character as 4 (80 columns in 320 pixels) */
static u32 spread[256];                 /* a byte's 8 bits as 8 nibbles' bit 0, the leftmost highest */
static u8 font[256][8];

static u16 colour15(const u8 *rgb)
{
    return (u16)(rgb[0] >> 1 | (rgb[1] >> 1) << 5 | (rgb[2] >> 1) << 10);
}

static void palette(void)
{
    for (int i = 0; i < 256; i++)
        pal15[i] = colour15(dac[i]);
    pal_dirty = 0;
}

static u8 attr_dac(u32 i)               /* a 16-colour pixel's DAC entry, through the attribute palette */
{
    u32 a = attr[i & 15], cs = attr[0x14];
    if (attr[0x10] & 0x80)
        return (u8)(((a & 0x0F) | (cs & 3) << 4 | (cs & 0x0C) << 4) & pel_mask);
    return (u8)(((a & 0x3F) | (cs & 0x0C) << 4) & pel_mask);
}

static void draw_text(void)
{
    u32 cols = mem[0x44A] ? mem[0x44A] : 80, start = ((u32)crtc[0x0C] << 8 | crtc[0x0D]) * 2;
    u16 ink[16];
    for (u32 i = 0; i < 16; i++)
        ink[i] = pal15[attr_dac(i)];
    int blink = (attr[0x10] & 8) != 0, cursor_on = (frames & 16) != 0 && !(crtc[0x0A] & 0x20);
    u32 cursor = ((u32)crtc[0x0E] << 8 | crtc[0x0F]) * 2;
    for (u32 row = 0; row < 25; row++)
        for (u32 col = 0; col < cols && col < 80; col++) {
            u32 at = (start + (row * cols + col) * 2) & 0x7FFF;
            u8 ch = mem[0xB8000 + at], a = mem[0xB8001 + at];
            u32 fg = a & 15, bg = a >> 4;
            if (blink) {
                bg &= 7;
                if ((a & 0x80) && (frames & 32))
                    fg = bg;
            }
            for (u32 y = 0; y < 8; y++) {
                u32 bits = font[ch][y];
                if (cursor_on && at == cursor && y >= 6)
                    bits = 0xFF;
                if (cols <= 40) {
                    u16 *o = pic + (row * 8 + y) * DOS_W + col * 8;
                    for (int x = 0; x < 8; x++)
                        o[x] = ink[(bits << x & 0x80) ? fg : bg];
                } else {
                    u16 *o = pic + (row * 8 + y) * DOS_W + col * 4;
                    bits = squeeze[bits];
                    for (int x = 0; x < 4; x++)
                        o[x] = ink[(bits << x & 8) ? fg : bg];
                }
            }
        }
}

static void draw_cga(void)
{
    u16 ink[4];
    for (u32 i = 0; i < 4; i++)
        ink[i] = pal15[attr_dac(i)];
    for (u32 y = 0; y < DOS_H; y++) {
        const u8 *line = mem + 0xB8000 + (y & 1) * 0x2000 + (y >> 1) * 80;
        u16 *o = pic + y * DOS_W;
        for (u32 x = 0; x < 80; x++, o += 4) {
            u32 b = line[x];
            if (kind == T_CGA2) {       /* (640 pixels: each pair shown as one, lit when either is) */
                b = (b | b << 1) & 0xAA;
                o[0] = ink[b >> 7 & 1], o[1] = ink[b >> 5 & 1], o[2] = ink[b >> 3 & 1], o[3] = ink[b >> 1 & 1];
                continue;
            }
            o[0] = ink[b >> 6], o[1] = ink[b >> 4 & 3], o[2] = ink[b >> 2 & 3], o[3] = ink[b & 3];
        }
    }
}

static int line_changed(u32 at, u32 bytes, int all)     /* plane bytes at..at+bytes-1 written since */
{
    if (all)
        return 1;
    for (u32 b = at >> 5; b <= (at + bytes - 1) >> 5; b++)
        if (vdirty[b & 0x7FF])
            return 1;
    return 0;
}

static void draw_planar(int all)
{
    u16 ink[16];
    for (u32 i = 0; i < 16; i++)
        ink[i] = pal15[attr_dac(i & attr[0x12])];
    u32 start = (u32)crtc[0x0C] << 8 | crtc[0x0D], width = crtc[0x13] * 2u, pan = attr[0x13] & 7;
    u32 wide = video_mode == 0x0E || video_mode == 0x10 || video_mode == 0x12;    /* (640 pixels: every other one) */
    if (!width)
        width = 40;
    for (u32 y = 0; y < DOS_H; y++) {
        u32 at = start + y * width, x = 0;
        u16 *o = pic + y * DOS_W;
        u32 skip = pan;
        if (!line_changed(at & 0xFFFF, wide ? 81 : 41, all))
            continue;
        if (!pan && !wide && (at & 0xFFFF) + 40 <= 0x10000) {  /* (the usual line: 40 bytes, 8 pixels each) */
            const u8 *p0 = vram[0] + (at & 0xFFFF), *p1 = vram[1] + (at & 0xFFFF);
            const u8 *p2 = vram[2] + (at & 0xFFFF), *p3 = vram[3] + (at & 0xFFFF);
            for (u32 b = 0; b < 40; b++, o += 8) {
                u32 n = spread[p0[b]] | spread[p1[b]] << 1 | spread[p2[b]] << 2 | spread[p3[b]] << 3;
                o[0] = ink[n >> 28], o[1] = ink[n >> 24 & 15], o[2] = ink[n >> 20 & 15], o[3] = ink[n >> 16 & 15];
                o[4] = ink[n >> 12 & 15], o[5] = ink[n >> 8 & 15], o[6] = ink[n >> 4 & 15], o[7] = ink[n & 15];
            }
            continue;
        }
        for (u32 b = 0; x < DOS_W; b++) {
            u32 a = (at + b) & 0xFFFF;
            u32 n = spread[vram[0][a]] | spread[vram[1][a]] << 1 | spread[vram[2][a]] << 2 | spread[vram[3][a]] << 3;
            for (int i = 0; i < 8 && x < DOS_W; i += 1 + (int)wide) {
                if (skip) {
                    skip--;
                    continue;
                }
                o[x++] = ink[n >> (28 - 4 * i) & 15];
            }
        }
    }
}

static void draw_256(int all)
{
    u16 ink[256];
    for (u32 i = 0; i < 256; i++)
        ink[i] = pal15[i & pel_mask];
    u32 start = (u32)crtc[0x0C] << 8 | crtc[0x0D], width = crtc[0x13] * 8u;
    if (!width)
        width = 320;
    if (!vga_n) {                       /* chain-4: plain memory */
        if (!all)
            return;
        start *= 4;
        for (u32 y = 0; y < DOS_H; y++) {
            const u8 *line = mem + 0xA0000;
            u32 at = start + y * width;
            u16 *o = pic + y * DOS_W;
            for (u32 x = 0; x < DOS_W; x++)
                o[x] = ink[line[(at + x) & 0xFFFF]];
        }
        return;
    }
    width /= 4;                         /* unchained: 4 pixels at each plane offset */
    for (u32 y = 0; y < DOS_H; y++) {
        u32 at = start + y * width;
        u16 *o = pic + y * DOS_W;
        if (!line_changed(at & 0xFFFF, 80, all))
            continue;
        for (u32 x = 0; x < DOS_W; x++)
            o[x] = ink[vram[x & 3][(at + (x >> 2)) & 0xFFFF]];
    }
}

/* the picture, when the blanking starts: the whole of it when a register, the palette or plain video memory
   changed (or it is another picture buffer than the last one), else the lines of the planes written since */
static void draw(void)
{
    static u16 *last;
    static int black;
    int all = dirty || pal_dirty || pic != last || black;
    last = pic;
    if (pal_dirty)
        palette();
    dirty = 0;
    black = !(attr_i & 0x20) || (seq[1] & 0x20);
    if (black) {                        /* the palette being written, the screen turned off: black */
        for (u32 i = 0; i < DOS_W * DOS_H; i++)
            pic[i] = 0;
    } else {
        switch (kind) {
        case T_TEXT: draw_text(); break;
        case T_CGA4: case T_CGA2: if (all) draw_cga(); break;
        case T_PLANAR: draw_planar(all); break;
        default: draw_256(all); break;
        }
    }
    for (u32 i = 0; i < sizeof(vdirty); i++)
        vdirty[i] = 0;
}

/* ---- the BIOS ---- */
#define BDA 0x400

static void put_str(char *p, int *n, int size, const char *s)
{
    while (*s && *n < size - 1)
        p[(*n)++] = *s++;
}

static void put_hex(char *p, int *n, int size, u32 v)
{
    int digits = v > 0xFFFF ? 8 : v > 0xFF ? 4 : 2;
    for (int i = digits - 1; i >= 0 && *n < size - 1; i--)
        p[(*n)++] = "0123456789ABCDEF"[v >> (4 * i) & 15];
}

void (*dos_log)(const char *line);

static void note(const char *what, u32 a, u32 b)
{
    if (!dos_log)
        return;
    char line[96];
    int n = 0;
    put_str(line, &n, sizeof(line), what);
    put_str(line, &n, sizeof(line), " ");
    put_hex(line, &n, sizeof(line), a);
    put_str(line, &n, sizeof(line), " ");
    put_hex(line, &n, sizeof(line), b);
    line[n] = 0;
    dos_log(line);
}

static void ret_flag(u32 bit, int on)   /* a flag in the FLAGS the INT pushed (its IRET returns them) */
{
    u32 a = c.base[SS] + (u16)(rSP + 4);
    u32 f = get16(a);
    put16(a, on ? f | bit : f & ~bit);
    if (bit == F_CF)
        c.fl = on ? c.fl | F_CF : c.fl & ~(u32)F_CF;
}

static u32 bios_ticks(void)
{
    return get16(BDA + 0x6C) | (u32)get16(BDA + 0x6E) << 16;
}

static void wait_here(void)             /* the function waits: its F1 again after the next interrupt */
{
    c.ip -= 2;
    c.sys |= F_IF;
    c.halted = 1;
    c.check = 1;
}

/* the keyboard's interrupt: a scan code into the BIOS's buffer */
static void bios_key(void)
{
    static const char plain[] = "\0\0331234567890-=\b\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
    static const char shifted[] = "\0\033!@#$%^&*()_+\b\tQWERTYUIOP{}\r\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";
    static const char pad[] = "789-456+1230.";
    u32 code = in8(0x60), sc = code & 0x7F, up = code & 0x80, e0 = mem[BDA + 0x96] & 2;
    u8 *fl = mem + BDA + 0x17;
    if (code == 0xE0 || code == 0xE1) {
        mem[BDA + 0x96] |= 2;
        return;
    }
    mem[BDA + 0x96] &= (u8)~2;
    if (e0 && (sc == 0x2A || sc == 0x36))
        return;                         /* (the shifts the gray keys send around themselves) */
    u32 bit = sc == 0x36 ? 1 : sc == 0x2A ? 2 : sc == 0x1D ? 4 : sc == 0x38 ? 8 : 0;
    if (bit) {
        *fl = (u8)(up ? *fl & ~bit : *fl | bit);
        return;
    }
    if (up)
        return;
    if (sc == 0x3A || sc == 0x45 || sc == 0x46) {
        *fl ^= sc == 0x3A ? 0x40 : sc == 0x45 ? 0x20 : 0x10;
        return;
    }
    u32 ascii = 0, shift = (*fl & 3) != 0;
    if (e0) {
        ascii = sc == 0x1C ? 13 : sc == 0x35 ? '/' : 0;
    } else if (sc < sizeof(plain) - 1) {
        ascii = (u8)(shift ? shifted : plain)[sc];
        if ((*fl & 0x40) && ((ascii | 0x20) >= 'a' && (ascii | 0x20) <= 'z'))
            ascii ^= 0x20;
        if (*fl & 4)
            ascii = ((ascii | 0x20) >= 'a' && (ascii | 0x20) <= 'z') ? (ascii & 31) : 0;
        if (*fl & 8)
            ascii = 0;
    } else if (sc >= 0x47 && sc <= 0x53) {
        u32 num = (*fl & 0x20) != 0;
        if (num != shift || sc == 0x4A || sc == 0x4E)
            ascii = (u8)pad[sc - 0x47];
    }
    u32 head = get16(BDA + 0x1A), tail = get16(BDA + 0x1C), next = tail + 2;
    if (next >= get16(BDA + 0x82))
        next = get16(BDA + 0x80);
    if (next == head)
        return;                         /* (full) */
    put16(BDA + tail, sc << 8 | ascii);
    put16(BDA + 0x1C, next);
}

static int key_waiting(void)
{
    return get16(BDA + 0x1A) != get16(BDA + 0x1C);
}

static u32 key_take(void)
{
    u32 head = get16(BDA + 0x1A), k = get16(BDA + head);
    head += 2;
    if (head >= get16(BDA + 0x82))
        head = get16(BDA + 0x80);
    put16(BDA + 0x1A, head);
    return k;
}

/* -- video -- */
static const u8 ega_attr[16] = {0, 1, 2, 3, 4, 5, 0x14, 7, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F};

static void dac_ega(int cga)            /* the DAC's first 64: the EGA's colours (cga: bit 4 = intensity) */
{
    for (u32 i = 0; i < 64; i++) {
        u32 r, g, b;
        if (cga) {
            u32 in = (i >> 4 & 1) * 21;
            r = (i & 4 ? 42 : 0) + in, g = (i & 2 ? 42 : 0) + in, b = (i & 1 ? 42 : 0) + in;
            if ((i & 0x17) == 6)
                g = 21;                 /* (brown) */
        } else {
            r = (i >> 2 & 1) * 42 + (i >> 5 & 1) * 21;
            g = (i >> 1 & 1) * 42 + (i >> 4 & 1) * 21;
            b = (i & 1) * 42 + (i >> 3 & 1) * 21;
        }
        dac[i][0] = (u8)r, dac[i][1] = (u8)g, dac[i][2] = (u8)b;
    }
}

static void dac_256(void)               /* mode 13h's: the 16 colours, 16 greys, then hues at three strengths */
{
    dac_ega(1);
    for (u32 i = 0; i < 16; i++) {
        u32 in = (i >> 3 & 1) * 21;
        dac[i][0] = (u8)((i & 4 ? 42 : 0) + in), dac[i][1] = (u8)((i & 2 ? 42 : 0) + in);
        dac[i][2] = (u8)((i & 1 ? 42 : 0) + in);
        if (i == 6)
            dac[i][1] = 21;
    }
    for (u32 i = 0; i < 16; i++)
        dac[16 + i][0] = dac[16 + i][1] = dac[16 + i][2] = (u8)(i * 63 / 15);
    for (u32 i = 32; i < 256; i++) {
        u32 h = (i - 32) % 24, level = (i - 32) / 24, top = 63 - level * 7, low = level * 3;
        u32 up = low + (top - low) * (h % 4) / 4, down = top - (top - low) * (h % 4) / 4, rgb[3];
        switch (h / 4) {
        case 0: rgb[0] = top, rgb[1] = up, rgb[2] = low; break;
        case 1: rgb[0] = down, rgb[1] = top, rgb[2] = low; break;
        case 2: rgb[0] = low, rgb[1] = top, rgb[2] = up; break;
        case 3: rgb[0] = low, rgb[1] = down, rgb[2] = top; break;
        case 4: rgb[0] = up, rgb[1] = low, rgb[2] = top; break;
        default: rgb[0] = top, rgb[1] = low, rgb[2] = down; break;
        }
        dac[i][0] = (u8)rgb[0], dac[i][1] = (u8)rgb[1], dac[i][2] = (u8)rgb[2];
    }
}

static u32 text_cols(void)
{
    return mem[BDA + 0x4A] ? mem[BDA + 0x4A] : 80;
}

static void set_mode(u32 mode, int clear)
{
    u32 cols = 80;
    video_mode = (u8)mode;
    vga_n = 0;
    for (int i = 0; i < 8; i++)
        seq[i] = 0;
    for (int i = 0; i < 16; i++)
        gc[i] = 0;
    for (int i = 0; i < 32; i++)
        crtc[i] = 0;
    seq[2] = 15;
    seq[4] = 6;
    gc[8] = 0xFF;
    gc[7] = 15;
    gc[6] = 5;
    attr[0x10] = 0x01;
    attr[0x11] = 0;
    attr[0x12] = 15;
    attr[0x13] = 0;
    attr[0x14] = 0;
    attr_i = 0x20;
    attr_flip = 0;
    pel_mask = 0xFF;
    crtc[0x0A] = 6, crtc[0x0B] = 7;
    for (int i = 0; i < 16; i++)
        attr[i] = ega_attr[i];
    dac_ega(0);
    switch (mode) {
    case 0: case 1: cols = 40; kind = T_TEXT; crtc[0x13] = 20; attr[0x10] = 0x08; break;
    case 4: case 5: case 6:
        kind = mode == 6 ? T_CGA2 : T_CGA4;
        cols = mode == 6 ? 80 : 40;
        dac_ega(1);
        if (mode == 6) {
            attr[0] = 0, attr[1] = 0x17;
        } else {
            attr[0] = 0, attr[1] = 0x13, attr[2] = 0x15, attr[3] = 0x17;
        }
        crtc[0x13] = 40;
        cga_colour = 0x30;
        break;
    case 0x0D: case 0x0E: case 0x10: case 0x12:
        kind = T_PLANAR;
        cols = mode == 0x0D ? 40 : 80;
        crtc[0x13] = mode == 0x0D ? 20 : 40;
        seq[4] = 6;
        gc[5] = 0;
        if (mode == 0x0D || mode == 0x0E) {
            dac_ega(1);
            for (int i = 0; i < 16; i++)
                attr[i] = (u8)((i & 7) | (i & 8 ? 0x10 : 0));
        }
        planar_on();
        break;
    case 0x13:
        kind = T_VGA256;
        cols = 40;
        crtc[0x13] = 40;
        seq[4] = 0x0E;
        gc[5] = 0x40;
        attr[0x10] = 0x41;
        for (int i = 0; i < 16; i++)
            attr[i] = (u8)i;
        dac_256();
        break;
    default:
        if (mode != 2 && mode != 3 && mode != 7)
            note("INT 10h: an unknown mode", mode, 0);
        video_mode = 3;
        kind = T_TEXT;
        crtc[0x13] = 40;
        attr[0x10] = 0x08;
        break;
    }
    mem[BDA + 0x49] = video_mode;
    put16(BDA + 0x4A, cols);
    put16(BDA + 0x4C, kind == T_TEXT ? (cols == 40 ? 0x800 : 0x1000) : kind == T_VGA256 ? 0xFA00 :
                      mode == 0x0D ? 0x2000 : mode == 0x0E ? 0x4000 : mode == 0x10 ? 0x8000 : mode == 0x12 ? 0xA000 :
                      0x4000);          /* (a page's bytes; in the planar modes, of each plane) */
    put16(BDA + 0x4E, 0);
    for (int i = 0; i < 16; i++)
        mem[BDA + 0x50 + i] = 0;
    mem[BDA + 0x62] = 0;
    mem[BDA + 0x84] = 24;
    mem[BDA + 0x85] = 8;
    if (clear) {
        if (kind == T_TEXT)
            for (u32 i = 0; i < 0x8000; i += 2)
                mem[0xB8000 + i] = ' ', mem[0xB8001 + i] = 7;
        else if (kind == T_CGA4 || kind == T_CGA2)
            for (u32 i = 0; i < 0x8000; i++)
                mem[0xB8000 + i] = 0;
        else
            for (u32 i = 0; i < 0x10000; i++)
                mem[0xA0000 + i] = vram[0][i] = vram[1][i] = vram[2][i] = vram[3][i] = 0;
    }
    pal_dirty = 1;
    dirty = 1;
}

/* a pixel through the BIOS (AH=0Ch, its characters in graphics modes) */
static void put_pixel(u32 x, u32 y, u32 colour)
{
    switch (kind) {
    case T_VGA256:
        if (x < 320 && y < 200) {
            u32 a = y * 320 + x;
            if (vga_n)
                vram[a & 3][a >> 2] = (u8)((colour & 0x80) ? vram[a & 3][a >> 2] ^ colour : colour);
            else
                mem[0xA0000 + a] = (u8)((colour & 0x80) ? mem[0xA0000 + a] ^ (colour & 0x7F) : colour);
        }
        break;
    case T_PLANAR: {
        u32 w = crtc[0x13] * 2u, a = (y * w + (x >> 3)) & 0xFFFF, bit = 0x80u >> (x & 7);
        for (int p = 0; p < 4; p++) {
            u32 on = colour >> p & 1;
            if (colour & 0x80)
                vram[p][a] ^= (u8)(on ? bit : 0);
            else
                vram[p][a] = (u8)(on ? vram[p][a] | bit : vram[p][a] & ~bit);
        }
        break;
    }
    case T_CGA4: case T_CGA2: {
        u32 a = 0xB8000 + (y & 1) * 0x2000 + (y >> 1) * 80, bits = kind == T_CGA2 ? 1 : 2;
        a += x * bits / 8;
        u32 sh = 8 - bits - (x * bits) % 8, mask = (bits == 1 ? 1u : 3u) << sh, v = (colour << sh) & mask;
        mem[a] = (u8)((colour & 0x80) ? mem[a] ^ v : (mem[a] & ~mask) | v);
        break;
    }
    }
    dirty = 1;
}

static u32 get_pixel(u32 x, u32 y)
{
    switch (kind) {
    case T_VGA256: {
        u32 a = y * 320 + x;
        return vga_n ? vram[a & 3][(a >> 2) & 0xFFFF] : mem[0xA0000 + (a & 0xFFFF)];
    }
    case T_PLANAR: {
        u32 w = crtc[0x13] * 2u, a = (y * w + (x >> 3)) & 0xFFFF, v = 0;
        for (int p = 0; p < 4; p++)
            v |= (u32)(vram[p][a] >> (7 - (x & 7)) & 1) << p;
        return v;
    }
    case T_CGA4: case T_CGA2: {
        u32 bits = kind == T_CGA2 ? 1 : 2, a = 0xB8000 + (y & 1) * 0x2000 + (y >> 1) * 80 + x * bits / 8;
        return mem[a & 0xFFFFF] >> (8 - bits - (x * bits) % 8) & (bits == 1 ? 1 : 3);
    }
    }
    return 0;
}

static void put_char(u32 ch, u32 attrib, u32 page, u32 count, int with_attr)
{
    u32 pos = get16(BDA + 0x50 + (page & 7) * 2), col = pos & 0xFF, row = pos >> 8, cols = text_cols();
    for (u32 k = 0; k < count && col < cols; k++, col++) {
        if (kind == T_TEXT) {
            u32 a = 0xB8000 + (page & 7) * get16(BDA + 0x4C) + (row * cols + col) * 2;
            mem[a & 0xFFFFF] = (u8)ch;
            if (with_attr)
                mem[(a + 1) & 0xFFFFF] = (u8)attrib;
            continue;
        }
        for (u32 y = 0; y < 8; y++)
            for (u32 x = 0; x < 8; x++) {
                u32 on = font[ch & 255][y] << x & 0x80;
                if (on || !(attrib & 0x80))
                    put_pixel(col * 8 + x, row * 8 + y, on ? attrib : 0);
            }
    }
    dirty = 1;
}

static void scroll(u32 lines, u32 fill, u32 top, u32 left, u32 bottom, u32 right, int down)
{
    u32 cols = text_cols();
    if (right >= cols)
        right = cols - 1;
    if (bottom > 24)
        bottom = 24;
    if (kind != T_TEXT) {               /* (graphics: the window cleared, which is what programs ask for) */
        for (u32 y = top * 8; y < (bottom + 1) * 8; y++)
            for (u32 x = left * 8; x < (right + 1) * 8; x++)
                put_pixel(x, y, kind == T_VGA256 ? fill & 0xFF : 0);
        return;
    }
    u32 base = 0xB8000 + mem[BDA + 0x62] * (u32)get16(BDA + 0x4C), h = bottom - top + 1;
    if (!lines || lines > h)
        lines = h;
    for (u32 i = 0; i < h; i++) {
        u32 y = down ? bottom - i : top + i, from = down ? y - lines : y + lines;
        int blank = down ? (y < top + lines) : (y + lines > bottom);
        for (u32 x = left; x <= right; x++) {
            u32 a = base + (y * cols + x) * 2;
            if (blank) {
                mem[a] = ' ';
                mem[a + 1] = (u8)fill;
            } else {
                u32 b = base + (from * cols + x) * 2;
                mem[a] = mem[b];
                mem[a + 1] = mem[b + 1];
            }
        }
    }
    dirty = 1;
}

static void teletype(u32 ch, u32 colour)
{
    u32 page = mem[BDA + 0x62], pos = get16(BDA + 0x50 + page * 2), col = pos & 0xFF, row = pos >> 8;
    u32 cols = text_cols();
    switch (ch) {
    case 7: return;
    case 8: if (col) col--; break;
    case 10: row++; break;
    case 13: col = 0; break;
    default:
        put_char(ch, colour, page, 1, kind != T_TEXT);
        if (++col >= cols) {
            col = 0;
            row++;
        }
    }
    if (row > 24) {
        scroll(1, kind == T_TEXT ? 7 : 0, 0, 0, 24, cols - 1, 0);
        row = 24;
    }
    put16(BDA + 0x50 + page * 2, row << 8 | col);
    if (kind == T_TEXT) {
        u32 at = page * get16(BDA + 0x4C) / 2 + row * cols + col;
        crtc[0x0E] = (u8)(at >> 8), crtc[0x0F] = (u8)at;
    }
}

static void bios_video(void)
{
    u32 es = c.base[ES];
#ifdef DOS_TRACE
    note("INT 10h AX", rAX, rBX);
#endif
    switch (rAH) {
    case 0x00: set_mode(rAL & 0x7F, !(rAL & 0x80)); break;
    case 0x01:
        mem[BDA + 0x60] = rCL, mem[BDA + 0x61] = rCH;
        crtc[0x0A] = rCH, crtc[0x0B] = rCL;
        break;
    case 0x02:
        put16(BDA + 0x50 + (rBH & 7) * 2, rDX);
        if (kind == T_TEXT && rBH == mem[BDA + 0x62]) {
            u32 at = rBH * get16(BDA + 0x4C) / 2 + rDH * text_cols() + rDL;
            crtc[0x0E] = (u8)(at >> 8), crtc[0x0F] = (u8)at;
        }
        break;
    case 0x03:
        rDX = get16(BDA + 0x50 + (rBH & 7) * 2);
        rCX = get16(BDA + 0x60);
        break;
    case 0x05:
        mem[BDA + 0x62] = rAL & 7;      /* the page shown: the CRTC's start (in characters in text modes) */
        if (kind == T_TEXT || kind == T_PLANAR) {
            u32 at = (rAL & 7) * get16(BDA + 0x4C) / (kind == T_TEXT ? 2u : 1u);
            put16(BDA + 0x4E, (rAL & 7) * get16(BDA + 0x4C));
            crtc[0x0C] = (u8)(at >> 8), crtc[0x0D] = (u8)at;
        }
        dirty = 1;
        break;
    case 0x06: case 0x07: scroll(rAL, rBH, rCH, rCL, rDH, rDL, rAH == 7); break;
    case 0x08: {
        u32 pos = get16(BDA + 0x50 + (rBH & 7) * 2);
        u32 a = 0xB8000 + (rBH & 7) * get16(BDA + 0x4C) + ((pos >> 8) * text_cols() + (pos & 0xFF)) * 2;
        rAX = kind == T_TEXT ? get16(a) : 0;
        break;
    }
    case 0x09: case 0x0A: put_char(rAL, rBL, rBH, rCX, rAH == 9 || kind != T_TEXT); break;
    case 0x0B:
        if (kind == T_CGA4) {
            u32 v = cga_colour;
            v = rBH ? (v & ~0x20u) | (rBL & 1 ? 0x20 : 0) : (v & 0x20) | (rBL & 0x1F);
            vga_out(0x3D9, v);
        }
        break;
    case 0x0C: put_pixel(rCX, rDX, rAL); break;
    case 0x0D: rAL = (u8)get_pixel(rCX, rDX); break;
    case 0x0E: teletype(rAL, rBL); break;
    case 0x0F:
        rAL = video_mode;
        rAH = (u8)text_cols();
        rBH = mem[BDA + 0x62];
        break;
    case 0x10:
        switch (rAL) {
        case 0x00: if (rBL < 16) attr[rBL] = rBH; break;
        case 0x01: attr[0x11] = rBH; break;
        case 0x02:
            for (u32 i = 0; i < 16; i++)
                attr[i] = rd8(es + (u16)(rDX + i));
            attr[0x11] = rd8(es + (u16)(rDX + 16));
            break;
        case 0x03: attr[0x10] = (u8)((attr[0x10] & ~8) | (rBL & 1 ? 8 : 0)); break;
        case 0x07: rBH = attr[rBL & 0x1F]; break;
        case 0x08: rBH = attr[0x11]; break;
        case 0x09:
            for (u32 i = 0; i < 16; i++)
                wr8(es + (u16)(rDX + i), attr[i]);
            wr8(es + (u16)(rDX + 16), attr[0x11]);
            break;
        case 0x10: dac[rBX & 255][0] = rDH & 63, dac[rBX & 255][1] = rCH & 63, dac[rBX & 255][2] = rCL & 63; break;
        case 0x12:
            for (u32 i = 0; i < rCX; i++)
                for (u32 k = 0; k < 3; k++)
                    dac[(rBX + i) & 255][k] = rd8(es + (u16)(rDX + i * 3 + k)) & 63;
            break;
        case 0x13:
            if (rBL == 0)
                attr[0x10] = (u8)((attr[0x10] & 0x7F) | (rBH & 1) << 7);
            else
                attr[0x14] = rBH;
            break;
        case 0x15: rDH = dac[rBX & 255][0], rCH = dac[rBX & 255][1], rCL = dac[rBX & 255][2]; break;
        case 0x17:
            for (u32 i = 0; i < rCX; i++)
                for (u32 k = 0; k < 3; k++)
                    wr8(es + (u16)(rDX + i * 3 + k), dac[(rBX + i) & 255][k]);
            break;
        case 0x18: pel_mask = rBL; break;
        case 0x19: rBL = pel_mask; break;
        case 0x1A: rBL = attr[0x10] >> 7, rBH = attr[0x14]; break;
        case 0x1B:
            for (u32 i = 0; i < rCX; i++) {
                u8 *d = dac[(rBX + i) & 255];
                u32 g = (30 * d[0] + 59 * d[1] + 11 * d[2] + 50) / 100;
                d[0] = d[1] = d[2] = (u8)g;
            }
            break;
        default: note("INT 10h AH=10h: AL", rAL, 0); break;
        }
        attr_i |= 0x20;
        pal_dirty = 1;
        dirty = 1;
        break;
    case 0x11:
        if (rAL == 0x30) {              /* the fonts: the ROM's 8 x 8 one for every kind asked */
            set_seg(ES, 0xF000);
            rBP = rBH == 4 ? 0xA400 : rBH == 0 ? (u16)get16(0x1F * 4) : 0xA000;
            if (rBH == 1) {
                set_seg(ES, get16(0x43 * 4 + 2));
                rBP = get16(0x43 * 4);
            }
            rCX = 8;
            rDL = 24;
        }
        break;
    case 0x12:
        if (rBL == 0x10) {              /* EGA information: colour, 256 KB */
            rBX = 0x0003;
            rCX = 0x0009;
        } else if (rBL == 0x30 || rBL == 0x31 || rBL == 0x32 || rBL == 0x33 || rBL == 0x34 || rBL == 0x36) {
            rAL = 0x12;
        }
        break;
    case 0x13: {
        u32 page = rBH, n = rCX, at = rBP, attrib = rBL;
        put16(BDA + 0x50 + (page & 7) * 2, rDX);
        u32 saved = mem[BDA + 0x62];
        mem[BDA + 0x62] = (u8)(page & 7);
        for (u32 i = 0; i < n; i++) {
            u32 ch = rd8(es + (u16)at++);
            if (rAL & 2)
                attrib = rd8(es + (u16)at++);
            teletype(ch, attrib);
        }
        mem[BDA + 0x62] = (u8)saved;
        if (!(rAL & 1))
            put16(BDA + 0x50 + (page & 7) * 2, rDX);
        break;
    }
    case 0x1A:
        if (rAL == 0) {
            rAL = 0x1A;
            rBX = 0x0008;               /* a VGA with a colour monitor */
        }
        break;
    case 0x1B: case 0x1C: case 0x4F: case 0xFE: case 0xFF:
        break;                          /* (unknown to it: AL left as it was) */
    default:
        note("INT 10h: AH", rAH, rAL);
        break;
    }
}

/* ---- DOS ---- */
#define FILES 16
#define HANDLES 20
#define ENV_SEG 0x0101
#define PSP_SEG 0x0112
#define MCB_FIRST 0x0100
#define MEM_TOP 0xA000                  /* 640 KB */
static struct {
    char name[13];
    const u8 *data;
    u32 size;
} files[FILES];
static int nfiles;
static struct {
    int file;                           /* -1: closed; -2: a device (CON) */
    u32 pos;
} handle[HANDLES];
static u32 psp = PSP_SEG, dta, last_error, pending_ascii;
static char args[128], info[96];
static u8 *program;
static u32 program_size;
static int line_active;

static char upper(char ch)
{
    return ch >= 'a' && ch <= 'z' ? (char)(ch - 32) : ch;
}

/* a name as DOS keeps it: no drive or directory, upper case, 12 characters at most */
static void dos_name(const char *path, char *out)
{
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':')
            base = p + 1;
    int n = 0;
    while (base[n] && n < 12) {
        out[n] = upper(base[n]);
        n++;
    }
    out[n] = 0;
}

static void read_name(u32 a, char *out, int size)     /* an ASCIIZ string from the program's memory */
{
    int n = 0;
    for (; n < size - 1; n++) {
        char ch = (char)rd8(a + (u32)n);
        if (!ch)
            break;
        out[n] = ch;
    }
    out[n] = 0;
}

static int find_file(u32 a)
{
    char path[80], name[13];
    read_name(a, path, sizeof(path));
    dos_name(path, name);
    for (int i = 0; i < nfiles; i++) {
        int k = 0;
        while (name[k] && name[k] == files[i].name[k])
            k++;
        if (!name[k] && !files[i].name[k])
            return i;
    }
    return -1;
}

static void fcb_form(const char *name, char out[11])   /* "MAGIC.EXE" -> "MAGIC   EXE", wildcards widened */
{
    int i = 0, k = 0;
    for (; i < 11; i++)
        out[i] = ' ';
    for (i = 0; name[k] && name[k] != '.' && i < 8; k++)
        if (name[k] == '*') {
            while (i < 8)
                out[i++] = '?';
        } else {
            out[i++] = upper(name[k]);
        }
    while (name[k] && name[k] != '.')
        k++;
    if (name[k] == '.')
        k++;
    for (i = 8; name[k] && i < 11; k++)
        if (name[k] == '*') {
            while (i < 11)
                out[i++] = '?';
        } else {
            out[i++] = upper(name[k]);
        }
}

static void dos_ok(void)
{
    ret_flag(F_CF, 0);
}

static void dos_fail(u32 code)
{
    last_error = code;
    rAX = (u16)code;
    ret_flag(F_CF, 1);
}

static u32 mcb_size(u32 s)
{
    return get16((s << 4) + 3);
}

static u32 mcb_owner(u32 s)
{
    return get16((s << 4) + 1);
}

static void mcb_set(u32 s, u32 type, u32 owner, u32 size)
{
    mem[s << 4] = (u8)type;
    put16((s << 4) + 1, owner);
    put16((s << 4) + 3, size);
}

static int mcb_valid(u32 s)
{
    return s < MEM_TOP && (mem[s << 4] == 'M' || mem[s << 4] == 'Z');
}

static void mcb_absorb(u32 s)           /* the free blocks after s into it */
{
    while (mem[s << 4] == 'M') {
        u32 n = s + 1 + mcb_size(s);
        if (!mcb_valid(n) || mcb_owner(n))
            break;
        mcb_set(s, mem[n << 4], mcb_owner(s), mcb_size(s) + 1 + mcb_size(n));
    }
}

static void mcb_split(u32 s, u32 want)
{
    u32 size = mcb_size(s);
    if (size > want) {
        mcb_set(s + 1 + want, mem[s << 4], 0, size - want - 1);
        mcb_set(s, 'M', mcb_owner(s), want);
    }
}

static void dos_alloc(void)
{
    u32 want = rBX, best = 0;
    for (u32 s = MCB_FIRST; mcb_valid(s); s += 1 + mcb_size(s)) {
        if (!mcb_owner(s)) {
            mcb_absorb(s);
            if (mcb_size(s) >= want) {
                mcb_split(s, want);
                put16((s << 4) + 1, psp);
                rAX = (u16)(s + 1);
                dos_ok();
                return;
            }
            if (mcb_size(s) > best)
                best = mcb_size(s);
        }
        if (mem[s << 4] == 'Z')
            break;
    }
    rBX = (u16)best;
    dos_fail(8);
}

static void dos_resize(void)
{
    u32 s = c.sr[ES] - 1u, want = rBX;
    if (!mcb_valid(s)) {
        dos_fail(9);
        return;
    }
    u32 had = mcb_size(s);
    mcb_absorb(s);
    if (mcb_size(s) >= want) {
        mcb_split(s, want);
        dos_ok();
        return;
    }
    rBX = (u16)mcb_size(s);
    mcb_split(s, had);
    dos_fail(8);
}

static void dos_end(u32 code)
{
    note("the program ended, its code", code, 0);
    ended = 1;
    c.halted = 1;
    c.sys &= ~(u32)F_IF;
}

static void dos_out(u32 ch)
{
    teletype(ch, 7);
    if (dos_log) {
        static char line[100];
        static int n;
        if (ch == 10 || ch == 13 || n == 98) {
            if (n) {
                line[n] = 0;
                dos_log(line);
            }
            n = 0;
        } else if (ch >= 32) {
            line[n++] = (char)ch;
        }
    }
}

static int dos_key(u32 *ch)             /* a character typed (an extended key: 0, then its scan code) */
{
    if (pending_ascii) {
        *ch = pending_ascii & 0xFF;
        pending_ascii = 0;
        return 1;
    }
    if (!key_waiting())
        return 0;
    u32 k = key_take();
    *ch = k & 0xFF;
    if (!*ch)
        pending_ascii = 0x100 | k >> 8;
    return 1;
}

/* a DTA's search: its pattern at +1 (11 characters), the next file to look at in +13 */
static void find_next(u32 d)
{
    char pattern[11], name[11];
    for (int i = 0; i < 11; i++)
        pattern[i] = (char)mem[(d + 1 + (u32)i) & 0xFFFFF];
    for (u32 i = mem[(d + 13) & 0xFFFFF]; i < (u32)nfiles; i++) {
        fcb_form(files[i].name, name);
        int k = 0;
        while (k < 11 && (pattern[k] == '?' || pattern[k] == name[k]))
            k++;
        if (k < 11)
            continue;
        mem[(d + 13) & 0xFFFFF] = (u8)(i + 1);
        wr8(d + 0x15, 0x20);
        wr16(d, 0x16, 0x6000);          /* 12:00 */
        wr16(d, 0x18, (13u << 9) | (6u << 5) | 1u);    /* 1993-06-01 */
        wr16(d, 0x1A, files[i].size);
        wr16(d, 0x1C, files[i].size >> 16);
        int n = 0;
        for (; files[i].name[n]; n++)
            wr8(d + 0x1E + (u32)n, (u8)files[i].name[n]);
        wr8(d + 0x1E + (u32)n, 0);
        rAX = 0;
        dos_ok();
        return;
    }
    dos_fail(18);
}

static void dos(void)
{
    u32 ds = c.base[DS];
    u32 ch;
#ifdef DOS_TRACE
    note("INT 21h AX", rAX, rBX);       /* (a PC harness's list of what the program asks DOS) */
#endif
    switch (rAH) {
    case 0x00: dos_end(0); break;
    case 0x01: case 0x07: case 0x08:
        if (!dos_key(&ch)) {
            wait_here();
            return;
        }
        rAL = (u8)ch;
        if (rAH == 1)
            dos_out(ch);
        break;
    case 0x02: dos_out(rDL); break;
    case 0x06:
        if (rDL != 0xFF) {
            dos_out(rDL);
            break;
        }
        if (dos_key(&ch)) {
            rAL = (u8)ch;
            ret_flag(F_ZF, 0);
        } else {
            rAL = 0;
            ret_flag(F_ZF, 1);
        }
        break;
    case 0x09:
        for (u32 i = 0; i < 4096; i++) {
            u32 b = rd8(ds + (u16)(rDX + i));
            if (b == '$')
                break;
            dos_out(b);
        }
        break;
    case 0x0A: {                        /* a line typed: DS:DX = room, count, characters */
        u32 room = rd8(ds + rDX), n;
        if (!line_active) {
            line_active = 1;
            wr8(ds + (u16)(rDX + 1), 0);
        }
        n = rd8(ds + (u16)(rDX + 1));
        while (dos_key(&ch)) {
            if (ch == 13) {
                wr8(ds + (u16)(rDX + 2 + n), 13);
                dos_out(13);
                dos_out(10);
                line_active = 0;
                return;
            }
            if (ch == 8 && n) {
                n--;
                dos_out(8), dos_out(' '), dos_out(8);
            } else if (ch >= 32 && n + 1 < room) {
                wr8(ds + (u16)(rDX + 2 + n), ch);
                n++;
                dos_out(ch);
            }
            wr8(ds + (u16)(rDX + 1), n);
        }
        wait_here();
        return;
    }
    case 0x0B: rAL = (pending_ascii || key_waiting()) ? 0xFF : 0; break;
    case 0x0C:
        while (key_waiting())
            key_take();
        pending_ascii = 0;
        if (rAL == 1 || rAL == 6 || rAL == 7 || rAL == 8 || rAL == 0x0A) {
            rAH = rAL;
            dos();
        }
        break;
    case 0x0D: break;
    case 0x0E: rAL = 26; break;
    case 0x19: rAL = 2; break;          /* C: */
    case 0x1A: dta = ds + rDX; break;
    case 0x25: put16(rAL * 4u, rDX); put16(rAL * 4u + 2, c.sr[DS]); break;
    case 0x29: {                        /* a name into an FCB */
        u32 si = rSI, fcb = c.base[ES] + rDI;
        char name[14];
        int n = 0;
        while (rd8(ds + (u16)si) == ' ' || rd8(ds + (u16)si) == '\t')
            si++;
        while (n < 13) {
            char k = (char)rd8(ds + (u16)si);
            if (k <= ' ' || k == '/' || k == ',' || k == ';' || k == '=' || k == '+')
                break;
            name[n++] = k;
            si++;
        }
        name[n] = 0;
        char form[11];
        fcb_form(name, form);
        wr8(fcb, 0);
        for (int i = 0; i < 11; i++)
            wr8(fcb + 1 + (u32)i, (u8)form[i]);
        rSI = (u16)si;
        rAL = 0;
        break;
    }
    case 0x2A: rCX = 1993; rDH = 6; rDL = 1; rAL = 2; break;
    case 0x2B: case 0x2D: rAL = 0; break;
    case 0x2C: {
        u32 cs = bios_ticks() * 1000u / 182u, s = cs / 100;
        rCH = (u8)(s / 3600 % 24), rCL = (u8)(s / 60 % 60), rDH = (u8)(s % 60), rDL = (u8)(cs % 100);
        break;
    }
    case 0x2F: set_seg(ES, dta >> 4); rBX = (u16)(dta & 15); break;
    case 0x30: rAX = 0x0005; rBX = 0xFF00; rCX = 0; break;
    case 0x33:
        if (rAL == 6) {
            rBX = 0x0005;
            rDX = 0;
        } else if (rAL == 0) {
            rDL = 0;
        }
        break;
    case 0x34: set_seg(ES, 0x60); rBX = 0x10; break;
    case 0x35: rBX = get16(rAL * 4u); set_seg(ES, get16(rAL * 4u + 2)); break;
    case 0x36: rAX = 4; rBX = 8000; rCX = 512; rDX = 16000; break;
    case 0x37: rAL = 0; rDL = '/'; break;
    case 0x38:
        for (u32 i = 0; i < 34; i++)
            wr8(ds + (u16)(rDX + i), 0);
        wr8(ds + (u16)(rDX + 2), '$');
        wr8(ds + (u16)(rDX + 7), ',');
        wr8(ds + (u16)(rDX + 9), '.');
        wr8(ds + (u16)(rDX + 11), '-');
        wr8(ds + (u16)(rDX + 13), ':');
        rBX = 1;
        dos_ok();
        break;
    case 0x39: case 0x3A: case 0x41: case 0x56: case 0x5A: case 0x5B: case 0x3C:
        note("DOS: a file written, not done: AH", rAH, 0);
        dos_fail(5);
        break;
    case 0x3B: dos_ok(); break;
    case 0x3D: case 0x6C: {
        int f = find_file(ds + (rAH == 0x6C ? rSI : rDX));
        if (f < 0) {
            char name[64];
            read_name(ds + (rAH == 0x6C ? rSI : rDX), name, sizeof(name));
            if (dos_log) {
                char line[96];
                int n = 0;
                put_str(line, &n, sizeof(line), "DOS: no such file: ");
                put_str(line, &n, sizeof(line), name);
                line[n] = 0;
                dos_log(line);
            }
            dos_fail(2);
            break;
        }
        int h = 5;
        while (h < HANDLES && handle[h].file != -1)
            h++;
        if (h == HANDLES) {
            dos_fail(4);
            break;
        }
        handle[h].file = f;
        handle[h].pos = 0;
        rAX = (u16)h;
        if (rAH == 0x6C)
            rCX = 1;
        dos_ok();
        break;
    }
    case 0x3E:
        if (rBX < HANDLES && handle[rBX].file >= 0) {
            handle[rBX].file = -1;
            dos_ok();
        } else if (rBX < 5) {
            dos_ok();
        } else {
            dos_fail(6);
        }
        break;
    case 0x3F: {
        if (rBX >= HANDLES || handle[rBX].file == -1) {
            dos_fail(6);
            break;
        }
        if (handle[rBX].file == -2) {   /* (the console: what is typed, at once) */
            u32 n = 0;
            while (n < rCX && dos_key(&ch)) {
                wr8(ds + (u16)(rDX + n), ch);
                n++;
            }
            rAX = (u16)n;
            dos_ok();
            break;
        }
        u32 f = (u32)handle[rBX].file, pos = handle[rBX].pos, n = rCX;
        if (pos >= files[f].size)
            n = 0;
        else if (n > files[f].size - pos)
            n = files[f].size - pos;
        for (u32 i = 0; i < n; i++)
            wr8(ds + (u16)(rDX + i), files[f].data[pos + i]);
        handle[rBX].pos = pos + n;
        rAX = (u16)n;
        c.cycles += (s32)(n / 4);
        dos_ok();
        break;
    }
    case 0x40:
        if (rBX >= HANDLES || handle[rBX].file == -1) {
            dos_fail(6);
            break;
        }
        if (handle[rBX].file == -2)
            for (u32 i = 0; i < rCX; i++)
                dos_out(rd8(ds + (u16)(rDX + i)));
        rAX = rCX;                      /* (a file's writes: taken and forgotten) */
        dos_ok();
        break;
    case 0x42: {
        if (rBX >= HANDLES || handle[rBX].file < 0) {
            dos_fail(6);
            break;
        }
        s32 off = (s32)((u32)rCX << 16 | rDX), base = 0;
        if (rAL == 1)
            base = (s32)handle[rBX].pos;
        else if (rAL == 2)
            base = (s32)files[handle[rBX].file].size;
        s32 to = base + off;
        if (to < 0) {
            dos_fail(0x19);
            break;
        }
        handle[rBX].pos = (u32)to;
        rAX = (u16)to;
        rDX = (u16)((u32)to >> 16);
        dos_ok();
        break;
    }
    case 0x43:
        if (find_file(ds + rDX) < 0) {
            dos_fail(2);
            break;
        }
        rCX = 0x20;
        dos_ok();
        break;
    case 0x44:
        if (rAL == 0 || rAL == 1) {
            if (rBX >= HANDLES || handle[rBX].file == -1) {
                dos_fail(6);
                break;
            }
            if (rAL == 0)
                rDX = handle[rBX].file == -2 ? 0x80D3 : 0x0002;
            dos_ok();
        } else if (rAL == 6 || rAL == 7) {
            rAL = 0xFF;
            dos_ok();
        } else if (rAL == 8) {
            rAX = 1;
            dos_ok();
        } else if (rAL == 9) {
            rDX = 0;
            dos_ok();
        } else {
            dos_fail(1);
        }
        break;
    case 0x45: case 0x46: {
        int h = rAH == 0x46 ? rCX : 0;
        if (rBX >= HANDLES || handle[rBX].file == -1 || (rAH == 0x46 && rCX >= HANDLES)) {
            dos_fail(6);
            break;
        }
        if (rAH == 0x45) {
            h = 0;
            while (h < HANDLES && handle[h].file != -1)
                h++;
            if (h == HANDLES) {
                dos_fail(4);
                break;
            }
            rAX = (u16)h;
        }
        handle[h] = handle[rBX];
        dos_ok();
        break;
    }
    case 0x47: wr8(ds + rSI, 0); dos_ok(); break;
    case 0x48: dos_alloc(); break;
    case 0x49:
        if (!mcb_valid(c.sr[ES] - 1u)) {
            dos_fail(9);
            break;
        }
        put16(((c.sr[ES] - 1u) << 4) + 1, 0);
        dos_ok();
        break;
    case 0x4A: dos_resize(); break;
    case 0x4B:
        note("DOS: EXEC, not done: AL", rAL, 0);
        dos_fail(2);
        break;
    case 0x4C: dos_end(rAL); break;
    case 0x4D: rAX = 0; break;
    case 0x4E: {
        char path[80], name[13], form[11];
        read_name(ds + rDX, path, sizeof(path));
        dos_name(path, name);
        fcb_form(name, form);
        if ((rCX & 0x08) && !(rCX & 0x10)) {   /* (the volume's label: none) */
            dos_fail(18);
            break;
        }
        for (int i = 0; i < 11; i++)
            wr8(dta + 1 + (u32)i, (u8)form[i]);
        wr8(dta + 13, 0);
        find_next(dta);
        break;
    }
    case 0x4F: find_next(dta); break;
    case 0x50: psp = rBX; break;
    case 0x51: case 0x62: rBX = (u16)psp; break;
    case 0x52: set_seg(ES, 0x60); rBX = 0x26; break;
    case 0x54: rAL = 0; break;
    case 0x57:
        if (rAL == 0) {
            rCX = 0x6000;
            rDX = (13u << 9) | (6u << 5) | 1u;
        }
        dos_ok();
        break;
    case 0x58: if (rAL == 0) rAX = 0; dos_ok(); break;
    case 0x59: rAX = (u16)last_error; rBX = 0x0104; rCH = 1; break;
    case 0x63: dos_fail(1); break;
    case 0x66: rBX = 437; rDX = 437; dos_ok(); break;
    case 0x67: case 0x68: dos_ok(); break;
    case 0x31: dos_end(rAL); break;
    default:
        note("DOS: not done: AH", rAH, rAL);
        dos_fail(1);
        break;
    }
}

/* the BIOS's and DOS's functions: the F1 nn of each vector's stub */
static void hle(u32 n)
{
    switch (n) {
    case 0x00:
        note("divide error at", (u32)get16(c.base[SS] + (u16)(rSP + 2)) << 16 | get16(c.base[SS] + rSP), 0);
        dos_end(0xFF);
        break;
    case 0x06: dos_end(0xFF); break;
    case 0x08: {                        /* the timer: the tick count (then INT 1Ch, the EOI) */
        u32 t = bios_ticks() + 1;
        if (t >= 0x1800B0) {
            t = 0;
            mem[BDA + 0x70] = 1;
        }
        put16(BDA + 0x6C, t);
        put16(BDA + 0x6E, t >> 16);
        break;
    }
    case 0x09: bios_key(); break;
    case 0x10: bios_video(); break;
    case 0x11: rAX = get16(BDA + 0x10); break;
    case 0x12: rAX = 640; break;
    case 0x13:
        if (rAH) {
            rAH = 1;
            ret_flag(F_CF, 1);
        } else {
            ret_flag(F_CF, 0);
        }
        break;
    case 0x14: rAH = 0x80; break;
    case 0x15:
        if (rAH == 0x88) {
            rAX = 0;
            ret_flag(F_CF, 0);
        } else if (rAH == 0x86) {       /* wait CX:DX microseconds */
            u32 us = (u32)rCX << 16 | rDX;
            if (us > 1000000)
                us = 1000000;
            c.cycles += (s32)(us * 1193u / 1000u * PIT_CYCLES);
            ret_flag(F_CF, 0);
        } else if (rAH == 0x90 || rAH == 0x91) {
            rAH = 0;
        } else {
            if (rAH != 0x4F)
                rAH = 0x86;
            ret_flag(F_CF, 1);
        }
        break;
    case 0x16:
        switch (rAH) {
        case 0x00: case 0x10:
            if (!key_waiting()) {
                wait_here();
                return;
            }
            rAX = (u16)key_take();
            break;
        case 0x01: case 0x11:
            if (key_waiting()) {
                rAX = get16(BDA + get16(BDA + 0x1A));
                ret_flag(F_ZF, 0);
            } else {
                ret_flag(F_ZF, 1);
            }
            break;
        case 0x02: case 0x12: rAL = mem[BDA + 0x17]; rAH = 0; break;
        case 0x05: {
            u32 head = get16(BDA + 0x1A), tail = get16(BDA + 0x1C), next = tail + 2;
            if (next >= get16(BDA + 0x82))
                next = get16(BDA + 0x80);
            if (next == head) {
                rAL = 1;
                break;
            }
            put16(BDA + tail, rCX);
            put16(BDA + 0x1C, next);
            rAL = 0;
            break;
        }
        }
        break;
    case 0x17: rAH = 0x90; break;
    case 0x1A:
        switch (rAH) {
        case 0x00:
            rCX = get16(BDA + 0x6E);
            rDX = get16(BDA + 0x6C);
            rAL = mem[BDA + 0x70];
            mem[BDA + 0x70] = 0;
            break;
        case 0x01: put16(BDA + 0x6E, rCX); put16(BDA + 0x6C, rDX); break;
        case 0x02: {
            u32 s = (u32)((bios_ticks() * 10u) / 182u);
            rCH = bcd(s / 3600 % 24), rCL = bcd(s / 60 % 60), rDH = bcd(s % 60), rDL = 0;
            ret_flag(F_CF, 0);
            break;
        }
        case 0x04: rCX = 0x1993; rDX = 0x0601; ret_flag(F_CF, 0); break;
        default: ret_flag(F_CF, 1); break;
        }
        break;
    case 0x20: dos_end(0); break;
    case 0x21: dos(); break;
    case 0x24: rAL = 3; break;
    case 0x29: teletype(rAL, 7); break;
    case 0x2F:
        if (rAX == 0x1600 || rAX == 0x4300)
            rAL = 0;
        break;
    case 0x33:
        if (rAX == 0)
            rBX = 0;
        break;
    }
}

/* ---- the BIOS's segment: the stubs, the font ---- */
static void rom_build(void)
{
    static const u8 timer[] = {0xF1, 0x08, 0xCD, 0x1C, 0x50, 0xB0, 0x20, 0xE6, 0x20, 0x58, 0xCF};
    static const u8 keyboard[] = {0xF1, 0x09, 0x50, 0xB0, 0x20, 0xE6, 0x20, 0x58, 0xCF};
    static const u8 irq[] = {0x50, 0xB0, 0x20, 0xE6, 0x20, 0x58, 0xCF};
    static const u8 irq_slave[] = {0x50, 0xB0, 0x20, 0xE6, 0xA0, 0xE6, 0x20, 0x58, 0xCF};
    u8 *rom = mem + 0xF0000;
    for (u32 i = 0; i < 0x10000; i++)
        rom[i] = 0;
    for (u32 n = 0; n < 256; n++) {
        rom[n * 4] = 0xF1;
        rom[n * 4 + 1] = (u8)n;
        rom[n * 4 + 2] = 0xCF;
    }
    for (u32 i = 0; i < sizeof(timer); i++)
        rom[0x400 + i] = timer[i];
    for (u32 i = 0; i < sizeof(keyboard); i++)
        rom[0x410 + i] = keyboard[i];
    for (u32 i = 0; i < sizeof(irq); i++)
        rom[0x420 + i] = irq[i];
    for (u32 i = 0; i < sizeof(irq_slave); i++)
        rom[0x430 + i] = irq_slave[i];
    for (u32 ch = 0; ch < 256; ch++)
        for (u32 y = 0; y < 8; y++) {
            rom[0xA000 + ch * 8 + y] = font[ch][y];
            if (ch < 128)
                rom[0xFA6E + ch * 8 + y] = font[ch][y];
        }
    static const char date[] = "06/01/93";
    for (u32 i = 0; i < 8; i++)
        rom[0xFFF5 + i] = (u8)date[i];
    rom[0xFFFE] = 0xFC;                 /* an AT */
}

/* the font: font5x7.h's characters in 8 x 8 cells, a few block and line characters drawn */
static void font_build(void)
{
    for (u32 ch = 0; ch < 256; ch++)
        for (u32 y = 0; y < 8; y++) {
            u32 b = 0;
            if (ch >= FONT_FIRST && ch <= FONT_LAST && y < FONT_H)
                b = (u32)font5x7[ch - FONT_FIRST][y] << 2;
            else if (ch == 0xDB)
                b = 0xFF;
            else if (ch == 0xDC)
                b = y >= 4 ? 0xFF : 0;
            else if (ch == 0xDF)
                b = y < 4 ? 0xFF : 0;
            else if (ch == 0xB0 || ch == 0xB1 || ch == 0xB2)
                b = (y & 1) ? (ch == 0xB2 ? 0xFF : 0x55) : (ch == 0xB0 ? 0 : 0xAA);
            else if (ch == 0xC4 || ch == 0xCD)
                b = y == 3 || (ch == 0xCD && y == 5) ? 0xFF : 0;
            else if (ch == 0xB3 || ch == 0xBA)
                b = ch == 0xB3 ? 0x10 : 0x28;
            font[ch][y] = (u8)b;
        }
    for (u32 b = 0; b < 256; b++) {
        squeeze[b] = (u8)(((b & 0xC0) ? 8 : 0) | ((b & 0x20) ? 4 : 0) | ((b & 0x18) ? 2 : 0) | ((b & 0x06) ? 1 : 0));
        u32 s = 0;
        for (u32 i = 0; i < 8; i++)
            if (b & (0x80u >> i))
                s |= 1u << (28 - 4 * i);
        spread[b] = s;
    }
}

/* ---- the program ---- */
static u32 w16(const u8 *p)
{
    return (u32)(p[0] | p[1] << 8);
}

static const char *start_program(void)
{
    const u8 *f = program;
    u32 size = program_size, load = psp + 0x10, block, cs, ip, ss, sp;
    /* the environment, then the program's block */
    static const char env[] = "COMSPEC=C:\\COMMAND.COM\0PATH=C:\\\0PROMPT=$P$G\0";
    u32 e = ENV_SEG << 4, k = 0;
    for (; k < sizeof(env) - 1; k++)
        mem[e + k] = (u8)env[k];
    mem[e + k++] = 0;
    put16(e + k, 1);
    k += 2;
    const char *path = "C:\\";
    while (*path)
        mem[e + k++] = (u8)*path++;
    for (int i = 0; files[0].name[i]; i++)
        mem[e + k++] = (u8)files[0].name[i];
    mem[e + k] = 0;
    mcb_set(MCB_FIRST, 'M', psp, 0x10);
    for (int i = 0; i < 8; i++)
        mem[(MCB_FIRST << 4) + 8 + (u32)i] = 0;
    u32 avail = MEM_TOP - psp;          /* (paragraphs from the PSP on) */
    if (size >= 0x1C && f[0] == 'M' && f[1] == 'Z') {
        u32 header = w16(f + 8) * 16, pages = w16(f + 4), last = w16(f + 2), image;
        image = pages * 512 - (last ? 512 - last : 0);
        if (image > size)
            image = size;
        if (image < header)
            return "a broken .exe header";
        image -= header;
        u32 need = 0x10 + (image + 15) / 16 + w16(f + 0x0A), most = 0x10 + (image + 15) / 16 + w16(f + 0x0C);
        if (need > avail)
            return "an .exe that needs more than 640 KB";
        block = most > avail || most < need ? avail : most;
        for (u32 i = 0; i < image; i++)
            mem[(load << 4) + i] = f[header + i];
        u32 n = w16(f + 6), at = w16(f + 0x18);
        for (u32 i = 0; i < n && at + 4 * i + 4 <= size; i++) {
            u32 a = ((w16(f + at + 4 * i + 2) + load) << 4) + w16(f + at + 4 * i);
            put16(a, get16(a) + load);
        }
        ss = w16(f + 0x0E) + load, sp = w16(f + 0x10), cs = w16(f + 0x16) + load, ip = w16(f + 0x14);
    } else {
        if (size > 0xFF00)
            return "a .com file larger than 64 KB";
        block = avail;
        for (u32 i = 0; i < size; i++)
            mem[(psp << 4) + 0x100 + i] = f[i];
        ss = cs = psp, ip = 0x100, sp = 0xFFFE;
        put16((psp << 4) + 0xFFFE, 0);
    }
    mcb_set(psp - 1, block == avail ? 'Z' : 'M', psp, block);
    if (block < avail)
        mcb_set(psp + block, 'Z', 0, avail - block - 1);
    /* the PSP */
    u32 p = psp << 4;
    for (u32 i = 0; i < 256; i++)
        mem[p + i] = 0;
    mem[p] = 0xCD, mem[p + 1] = 0x20;
    put16(p + 2, psp + block);
    mem[p + 5] = 0x9A, put16(p + 6, 0xFEF0), put16(p + 8, 0xF01D);
    put16(p + 0x0A, get16(0x22 * 4)), put16(p + 0x0C, get16(0x22 * 4 + 2));
    put16(p + 0x0E, get16(0x23 * 4)), put16(p + 0x10, get16(0x23 * 4 + 2));
    put16(p + 0x12, get16(0x24 * 4)), put16(p + 0x14, get16(0x24 * 4 + 2));
    put16(p + 0x16, psp);
    for (u32 i = 0; i < 20; i++)
        mem[p + 0x18 + i] = (u8)(i < 5 ? (i == 3 ? 0 : i == 4 ? 2 : 1) : 0xFF);
    put16(p + 0x2C, ENV_SEG);
    put16(p + 0x32, 20), put16(p + 0x34, 0x18), put16(p + 0x36, psp);
    mem[p + 0x50] = 0xCD, mem[p + 0x51] = 0x21, mem[p + 0x52] = 0xCB;
    for (u32 i = 0; i < 11; i++)
        mem[p + 0x5D + i] = mem[p + 0x6D + i] = ' ';
    u32 n = 0;
    while (args[n] && n < 126)
        n++;
    mem[p + 0x80] = (u8)n;
    for (u32 i = 0; i < n; i++)
        mem[p + 0x81 + i] = (u8)args[i];
    mem[p + 0x81 + n] = 13;
    dta = p + 0x80;
    /* the registers as COMMAND.COM leaves them */
    for (int i = 0; i < 8; i++)
        c.r.w[i] = 0;
    set_seg(DS, psp);
    set_seg(ES, psp);
    set_seg(SS, ss);
    set_seg(CS, cs);
    rSP = (u16)sp;
    c.ip = (u16)ip;
    rCX = 0x00FF;
    rDX = (u16)psp;
    rSI = (u16)ip;
    rDI = (u16)sp;
    rBP = 0x091C;
    set_flags(F_IF);
    return 0;
}

/* ---- the machine ---- */
static void bios_data(void)
{
    for (u32 i = 0; i < 0x500; i++)
        mem[i] = 0;
    for (u32 n = 0; n < 256; n++) {     /* each vector at its stub in F000 */
        put16(n * 4, n * 4);
        put16(n * 4 + 2, 0xF000);
    }
    put16(8 * 4, 0x400);
    put16(9 * 4, 0x410);
    for (u32 n = 0x0A; n < 0x10; n++)
        put16(n * 4, 0x420);
    for (u32 n = 0x70; n < 0x78; n++)
        put16(n * 4, 0x430);
    put16(0x1F * 4, 0xA400);            /* the font's upper 128 characters */
    put16(0x43 * 4, 0xA000);            /* the whole font */
    put16(0x1D * 4, 0xB000);            /* (the video and floppy parameter tables: zeros) */
    put16(0x1E * 4, 0xB000);
    for (u32 n = 0x60; n < 0x68; n++)   /* (the user vectors: 0, as DOS leaves them) */
        put16(n * 4, 0), put16(n * 4 + 2, 0);
    put16(BDA + 0x10, 0x0021);          /* a floppy drive, a colour 80-column screen */
    put16(BDA + 0x13, 640);
    put16(BDA + 0x1A, 0x1E), put16(BDA + 0x1C, 0x1E), put16(BDA + 0x80, 0x1E), put16(BDA + 0x82, 0x3E);
    put16(BDA + 0x63, 0x3D4);
    mem[BDA + 0x87] = 0x60, mem[BDA + 0x88] = 0x09, mem[BDA + 0x89] = 0x11;
    mem[BDA + 0x96] = 0x10;             /* a 101-key keyboard */
    u32 t = 12u * 65543u;               /* noon */
    put16(BDA + 0x6C, t), put16(BDA + 0x6E, t >> 16);
    /* DOS's own: its list of lists (the first MCB before it), the InDOS flag */
    put16(0x600 + 0x24, MCB_FIRST);
}

static u8 held_was[32];
static char queue[256];
static int q_head, q_tail, q_frame;

static void machine_reset(void)
{
    for (int i = 0; i < 256; i++) {
        u32 b = (u32)i, n = 0;
        while (b) {
            n += b & 1;
            b >>= 1;
        }
        parity[i] = (u8)!(n & 1);
    }
    for (u32 i = 0; i < sizeof(mem); i++)
        mem[i] = 0;
    font_build();
    rom_build();
    bios_data();
    for (u32 i = 0; i < 0x10000; i++)
        vram[0][i] = vram[1][i] = vram[2][i] = vram[3][i] = 0;
    for (int i = 0; i < 8; i++)
        c.r.w[i] = 0;
    c.cycles = 0;
    c.halted = 0;
    c.check = 0;
    cycles_hi = 0;
    ended = 0;
    pic_irr = pic_isr = pic_init = 0;
    pic_imr = 0xB8;                     /* (the BIOS leaves the timer, the keyboard, the slave, the floppy on) */
    pic_base = 8;
    pic_read_isr = 0;
    for (int i = 0; i < 3; i++) {
        pit_n[i] = i == 1 ? 18 : 0x10000;      /* (the BIOS's: 18.2 Hz, the refresh, the speaker's beep) */
        pit_new[i] = 0;
        pit_mode[i] = i == 1 ? 2 : 3;
        pit_access[i] = 3;
        pit_half[i] = pit_latched[i] = pit_read_hi[i] = 0;
        pit_running[i] = i != 2;
        pit_start[i] = 0;
    }
    pit0_next = 0x10000 * PIT_CYCLES;
    pit0_on = 1;
    pit_gate2 = 0;
    port61 = 0;
    kq_head = kq_tail = 0;
    kbc_full = kbc_command = kbd_command = 0;
    kbc_data = 0;
    for (int i = 0; i < 32; i++)
        held_was[i] = 0;
    q_head = q_tail = q_frame = 0;
    for (int i = 0; i < HANDLES; i++)
        handle[i].file = i < 5 ? -2 : -1;
    psp = PSP_SEG;
    last_error = 0;
    pending_ascii = 0;
    line_active = 0;
    frames = 0;
    set_mode(3, 1);
}

void dos_reset(void)
{
    machine_reset();
    if (program)
        (void)start_program();
}

/* ---- running ---- */
static void run(s32 t)
{
    run_end = t;
    while (c.cycles < run_end) {
        if (c.check) {
            if (c.check > 1)
                c.check--;
            else
                irq_check();
        }
        if (c.halted) {
            if (c.cycles < run_end)
                c.cycles = run_end;
            break;
        }
        step();
    }
}

static void advance(s32 t)              /* run to t, the timer's interrupts at their times */
{
    while (c.cycles < t) {
        s32 next = t;
        if (pit0_on && pit0_next < next)
            next = pit0_next;
        run(next);
        while (pit0_on && c.cycles >= pit0_next) {
            pic_raise(0);
            if (periodic(0)) {
                pit_start[0] = pit0_next;
                if (pit_new[0])
                    pit_n[0] = pit_new[0], pit_new[0] = 0;
                pit0_next += (s32)(pit_n[0] * PIT_CYCLES);
            } else {
                pit0_on = 0;
            }
        }
    }
}

/* ---- the keyboard ---- */
/* the scan codes of the HID usages 0x04..0x67 (| 0x80: sent after 0xE0; 0: none) */
static const u8 hid_scan[0x64] = {
    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19,   /* a-p */
    0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,                                       /* q-z */
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,                                       /* 1-0 */
    0x1C, 0x01, 0x0E, 0x0F, 0x39, 0x0C, 0x0D, 0x1A, 0x1B, 0x2B, 0x2B, 0x27, 0x28, 0x29, 0x33, 0x34,   /* Enter.. */
    0x35, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58, 0x00, 0x46,   /* / Caps F1 */
    0x00, 0xD2, 0xC7, 0xC9, 0xD3, 0xCF, 0xD1, 0xCD, 0xCB, 0xD0, 0xC8, 0x45, 0xB5, 0x37, 0x4A, 0x4E,   /* Pause Ins */
    0x9C, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49, 0x52, 0x53, 0x56, 0xDD, 0x00, 0x00,   /* KP Enter.. */
};
static const u8 hid_mods[8] = {0x1D, 0x2A, 0x38, 0xDB, 0x9D, 0x36, 0xB8, 0xDC};

void dos_keys_from_hid(const u8 *usages, int n, u8 keys[32])
{
    for (int i = 0; i < 32; i++)
        keys[i] = 0;
    for (int i = 0; i < n; i++) {
        u32 u = usages[i], k = 0;
        if (u >= 0x04 && u < 0x04 + sizeof(hid_scan))
            k = hid_scan[u - 0x04];
        else if (u >= 0xE0 && u <= 0xE7)
            k = hid_mods[u - 0xE0];
        if (k)
            keys[k >> 3] |= (u8)(1u << (k & 7));
    }
}

/* a typed character's scan code (| 0x100: with Shift) */
static u32 char_scan(u32 ch)
{
    static const char plain[] = "\0\0331234567890-=\b\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./";
    static const char shifted[] = "\0\033!@#$%^&*()_+\b\tQWERTYUIOP{}\r\0ASDFGHJKL:\"~\0|ZXCVBNM<>?";
    if (ch == ' ')
        return 0x39;
    if (ch == '\n')
        return 0x1C;
    if (ch == 127)
        return 0x0E;
    for (u32 i = 1; i < sizeof(plain) - 1; i++) {
        if ((u8)plain[i] == ch && ch)
            return i;
        if ((u8)shifted[i] == ch && ch)
            return 0x100 | i;
    }
    return 0;
}

void dos_type(const char *text)
{
    while (*text) {
        int next = (q_tail + 1) & 255;
        if (next == q_head)
            break;
        queue[q_tail] = *text++;
        q_tail = next;
    }
}

int dos_typing(void)
{
    return (q_tail - q_head) & 255;
}

static void typed(u8 keys[32])          /* each held 3 frames, then nothing for 3 */
{
    if (q_head == q_tail)
        return;
    u32 k = char_scan((u8)queue[q_head]);
    if (q_frame < 3 && k) {
        keys[(k & 0xFF) >> 3] |= (u8)(1u << (k & 7));
        if (k & 0x100)
            keys[0x2A >> 3] |= 1u << (0x2A & 7);
    }
    if (++q_frame == 6) {
        q_frame = 0;
        q_head = (q_head + 1) & 255;
    }
}

static void press(u8 keys[32], u32 k)
{
    keys[k >> 3] |= (u8)(1u << (k & 7));
}

void dos_frame(u8 pad, const u8 keys[32], u16 *picture)
{
    u8 now[32];
    for (int i = 0; i < 32; i++)
        now[i] = keys ? keys[i] : 0;
    typed(now);
    if (pad & DOS_UP)
        press(now, 0xC8);
    if (pad & DOS_DOWN)
        press(now, 0xD0);
    if (pad & DOS_LEFT)
        press(now, 0xCB);
    if (pad & DOS_RIGHT)
        press(now, 0xCD);
    if (pad & DOS_A)
        press(now, 0xC8);               /* (Up again: Dizzy's jump) */
    if (pad & DOS_B)
        press(now, 0x39);
    if (pad & DOS_START)
        press(now, 0x1C);
    if (pad & DOS_SELECT)
        press(now, 0x01);
    for (u32 k = 1; k < 256; k++) {
        u32 bit = 1u << (k & 7), down = now[k >> 3] & bit;
        if (down == (held_was[k >> 3] & bit))
            continue;
        if (k & 0x80)
            kbd_put(0xE0);
        kbd_put((k & 0x7F) | (down ? 0 : 0x80));
    }
    for (int i = 0; i < 32; i++)
        held_was[i] = now[i];
    pic = picture;
    int drawn = 0;
    for (int s = 0; s < SLICES; s++) {
        s32 end = (s + 1) * (FRAME_CYCLES / SLICES);
        if (!drawn && end > DRAW_AT) {
            advance(DRAW_AT);
            if (pic)
                draw();
            drawn = 1;
        }
        advance(end);
        kbd_deliver();
    }
    for (int i = 1; i < 3; i++)
        pit_catch_up(i);
    c.cycles -= FRAME_CYCLES;
    pit0_next -= FRAME_CYCLES;
    for (int i = 0; i < 3; i++) {
        pit_start[i] -= FRAME_CYCLES;
        if (pit_start[i] < -0x40000000)
            pit_start[i] = -0x40000000;     /* (a one-shot count long over) */
    }
    cycles_hi += FRAME_CYCLES;
    frames++;
}

/* ---- files ---- */
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

void dos_clear_files(void)
{
    nfiles = 1;                         /* (files[0] is the program's) */
}

const char *dos_add_file(const char *name, const u8 *data, u32 size)
{
    if (nfiles < 1)
        nfiles = 1;
    if (nfiles >= FILES)
        return "too many files";
    dos_name(name, files[nfiles].name);
    files[nfiles].data = data;
    files[nfiles].size = size;
    nfiles++;
    return 0;
}

void dos_args(const char *text)
{
    int n = 0;
    if (text && *text && *text != ' ')
        args[n++] = ' ';
    while (text && *text && n < (int)sizeof(args) - 1)
        args[n++] = *text++;
    args[n] = 0;
}

const char *dos_load(u8 *file, u32 size, const char *name)
{
    if (!file || size < 2)
        return "no program";
    int exe = file[0] == 'M' && file[1] == 'Z';
    char base[13];
    dos_name(name ? name : "PROGRAM.EXE", base);
    int n = 0, dot = 0;
    while (base[n])
        if (base[n++] == '.')
            dot = 1;
    if (!exe) {
        if (size > 0xFF00)
            return "not a DOS program (an .exe starts with MZ; a .com file is 64 KB at most)";
        if (!dot && n < 9)
            base[n++] = '.', base[n++] = 'C', base[n++] = 'O', base[n++] = 'M', base[n] = 0;
    }
    if (nfiles < 1)
        nfiles = 1;
    for (int i = 0; i < 13; i++)
        files[0].name[i] = base[i];
    files[0].data = file;
    files[0].size = size;
    program = file;
    program_size = size;
    machine_reset();
    const char *why = start_program();
    if (why) {
        program = 0;
        return why;
    }
    char *p = put(info, files[0].name);
    p = put(p, exe ? ": an .exe of " : ": a .com file of ");
    p = put_number(p, (size + 1023) >> 10);
    p = put(p, " KB");
    if (nfiles > 1) {
        p = put(p, ", ");
        p = put_number(p, (u32)nfiles - 1);
        p = put(p, nfiles == 2 ? " more file" : " more files");
    }
    if (args[0]) {
        p = put(p, ", run as");
        put(p, args);
    }
    return 0;
}

const char *dos_info(void)
{
    return info;
}

u8 *dos_save_ram(u32 *size, int *battery)
{
    *size = 0;
    *battery = 0;
    return mem;
}

int dos_save_changed(void)
{
    return 0;
}

u32 dos_cpu_cycles(void)
{
    return cycles_hi + (u32)c.cycles;
}

int dos_jammed(void)
{
    return ended || (c.halted && !(c.sys & F_IF));
}

u32 dos_idle_cycles(void)
{
    return idle_cycles;
}

int dos_mode(void)
{
    return video_mode;
}

u8 dos_peek(u32 a)
{
    a &= 0xFFFFF;
    if (a - vga_lo < vga_n)
        return vram[gc[4] & 3][a - vga_lo];
    return mem[a];
}

void dos_registers(u16 r[14])
{
    for (int i = 0; i < 8; i++)
        r[i] = c.r.w[i];
    for (int i = 0; i < 4; i++)
        r[8 + i] = c.sr[i];
    r[12] = c.ip;
    r[13] = (u16)get_flags();
}
