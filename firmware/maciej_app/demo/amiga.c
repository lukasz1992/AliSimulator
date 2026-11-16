/* An Amiga 500 (OCS, PAL): see amiga.h.

   The 68000: an interpreter.  Each of the 65536 opcodes is decoded once, at
   the start, into a byte naming its handler (optab, 64 KB of RAM); a
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

/* ---- the 68000 ---- */
static struct m68k {
    u32 r[16];                          /* D0..D7, A0..A7 (A7: the stack pointer of the mode it is in) */
    u32 pc, op;
    u32 other_sp;                       /* the other mode's stack pointer (USP in supervisor mode, SSP in user) */
    u32 s, t, im;                       /* SR's supervisor and trace bits, its interrupt mask */
    u32 n, z, v, c, x;
    s32 clock;                          /* cycles since the frame started */
    u32 ipl;                            /* the interrupt level Paula asks for (0: none) */
    u32 pending;                        /* something to take before the next instruction (an interrupt, a trace) */
    u32 stopped;                        /* STOP: waiting for an interrupt */
    u32 fault, fault_write;             /* an address error in this instruction: the address | 1, read or write */
    u32 fault_pc;
} m;

static u8 *rmap[256], *wmap[256];       /* each 64 KB page's memory (0: the chips, through io_read / io_write) */
static u32 io_read(u32 a, int size);
static void io_write(u32 a, u32 v, int size);
static void cpu_reset_line(void);       /* RESET: the chips back to their power-on state */

static u32 odd(u32 a, int write)
{
    if (!m.fault) {
        m.fault = a | 1;
        m.fault_write = (u32)write;
    }
    return 0;
}

INLINE u32 rb(u32 a)
{
    const u8 *p = rmap[(a >> 16) & 0xFF];
    return p ? p[(a & 0xFFFF) ^ 1] : io_read(a & 0xFFFFFF, 1);
}

INLINE u32 rw(u32 a)
{
    if (a & 1)
        return odd(a, 0);
    const u8 *p = rmap[(a >> 16) & 0xFF];
    return p ? *(const u16 *)(p + (a & 0xFFFF)) : io_read(a & 0xFFFFFF, 2);
}

static u32 rl(u32 a)
{
    u32 hi = rw(a);
    return hi << 16 | rw(a + 2);
}

INLINE void wb(u32 a, u32 v)
{
    u8 *p = wmap[(a >> 16) & 0xFF];
    if (p)
        p[(a & 0xFFFF) ^ 1] = (u8)v;
    else
        io_write(a & 0xFFFFFF, v & 0xFF, 1);
}

INLINE void ww(u32 a, u32 v)
{
    if (a & 1) {
        odd(a, 1);
        return;
    }
    u8 *p = wmap[(a >> 16) & 0xFF];
    if (p)
        *(u16 *)(p + (a & 0xFFFF)) = (u16)v;
    else
        io_write(a & 0xFFFFFF, v & 0xFFFF, 2);
}

static void wl(u32 a, u32 v)
{
    ww(a, v >> 16);
    ww(a + 2, v);
}

INLINE u32 fetch(void)
{
    u32 v = rw(m.pc);
    m.pc += 2;
    return v;
}

static u32 fetch32(void)
{
    u32 hi = fetch();
    return hi << 16 | fetch();
}

INLINE u32 msk(int s)
{
    return s == 1 ? 0xFFu : s == 2 ? 0xFFFFu : 0xFFFFFFFFu;
}

INLINE u32 nf(u32 r, int s)              /* N: the sign bit into bit 7 */
{
    return s == 1 ? r : s == 2 ? r >> 8 : r >> 24;
}

INLINE u32 sx(u32 v, int s)
{
    return s == 1 ? (u32)(s32)(s8)v : s == 2 ? (u32)(s32)(s16)v : v;
}

INLINE u32 rd(u32 a, int s)
{
    return s == 1 ? rb(a) : s == 2 ? rw(a) : rl(a);
}

INLINE void wr(u32 a, u32 v, int s)
{
    if (s == 1)
        wb(a, v);
    else if (s == 2)
        ww(a, v);
    else
        wl(a, v);
}

INLINE void set_dn(u32 r, u32 v, int s)
{
    if (s == 4)
        m.r[r] = v;
    else
        m.r[r] = (m.r[r] & ~msk(s)) | (v & msk(s));
}

static u32 get_sr(void)
{
    return m.t << 15 | m.s << 13 | m.im << 8 | ((m.x & 0x100) >> 4) | ((m.n & 0x80) >> 4) | (u32)(!m.z) << 2 |
           ((m.v & 0x80) >> 6) | ((m.c & 0x100) >> 8);
}

static void set_ccr(u32 v)
{
    m.x = (v << 4) & 0x100;
    m.n = (v << 4) & 0x80;
    m.z = !(v & 4);
    m.v = (v << 6) & 0x80;
    m.c = (v << 8) & 0x100;
}

static void check_pending(void)
{
    m.pending = m.ipl > m.im || m.t;
}

static void set_sr(u32 v)
{
    set_ccr(v);
    m.t = (v >> 15) & 1;
    m.im = (v >> 8) & 7;
    u32 s = (v >> 13) & 1;
    if (s != m.s) {
        u32 sp = m.r[15];
        m.r[15] = m.other_sp;
        m.other_sp = sp;
        m.s = s;
    }
    check_pending();
}

static void supervisor(void)
{
    if (!m.s) {
        u32 sp = m.r[15];
        m.r[15] = m.other_sp;
        m.other_sp = sp;
        m.s = 1;
    }
    m.t = 0;
}

/* an exception of group 1 or 2: SR and the PC pushed, the vector's handler */
static void exception(u32 vector, u32 pc)
{
    u32 sr = get_sr();
    supervisor();
    m.r[15] -= 4;
    wl(m.r[15], pc);
    m.r[15] -= 2;
    ww(m.r[15], sr);
    m.pc = rl(vector * 4);
    m.clock += 34;
    check_pending();
}

/* an address error: the 14-byte frame of group 0 (the access, the opcode, SR, PC) */
static void address_error(void)
{
    u32 a = m.fault & ~1u, w = m.fault_write;
    m.fault = 0;
    u32 sr = get_sr();
    supervisor();
    m.r[15] -= 4;
    wl(m.r[15], m.pc);
    m.r[15] -= 2;
    ww(m.r[15], sr);
    m.r[15] -= 2;
    ww(m.r[15], m.op);
    m.r[15] -= 4;
    wl(m.r[15], a | 1u);
    m.r[15] -= 2;
    ww(m.r[15], (w ? 0 : 0x10) | (m.s ? 5 : 1));
    m.pc = rl(3 * 4);
    m.fault = 0;
    m.clock += 50;
    check_pending();
}

#define OP   m.op
#define RX   ((m.op >> 9) & 7)
#define RY   (m.op & 7)
#define MODE ((m.op >> 3) & 7)

static u32 indexed(u32 base)
{
    u32 ext = fetch();
    u32 x = m.r[ext >> 12];
    if (!(ext & 0x800))
        x = (u32)(s32)(s16)x;
    return base + x + (u32)(s32)(s8)ext;
}

/* the address of a memory operand (modes 2..7) of `s` bytes, the time to fetch it counted; #imm: where it is */
static u32 ea(u32 mode, u32 reg, int s)
{
    u32 a, l = s == 4 ? 4 : 0;
    switch (mode) {
    case 2:
        m.clock += 4 + l;
        return m.r[8 + reg];
    case 3:
        a = m.r[8 + reg];
        m.r[8 + reg] += (s == 1 && reg == 7) ? 2 : (u32)s;
        m.clock += 4 + l;
        return a;
    case 4:
        m.r[8 + reg] -= (s == 1 && reg == 7) ? 2 : (u32)s;
        m.clock += 6 + l;
        return m.r[8 + reg];
    case 5:
        a = m.r[8 + reg];
        m.clock += 8 + l;
        return a + (u32)(s32)(s16)fetch();
    case 6:
        m.clock += 10 + l;
        return indexed(m.r[8 + reg]);
    default:
        switch (reg) {
        case 0:
            m.clock += 8 + l;
            return (u32)(s32)(s16)fetch();
        case 1:
            m.clock += 12 + l;
            return fetch32();
        case 2:
            a = m.pc;
            m.clock += 8 + l;
            return a + (u32)(s32)(s16)fetch();
        case 3:
            m.clock += 10 + l;
            return indexed(m.pc);
        default:
            a = m.pc + (s == 1);
            m.pc += s == 4 ? 4 : 2;
            m.clock += 4 + l;
            return a;
        }
    }
}

/* the address of a control operand (LEA, PEA, JMP, JSR, MOVEM): no time counted */
static u32 ea_ctl(u32 mode, u32 reg)
{
    u32 a;
    switch (mode) {
    case 2:
        return m.r[8 + reg];
    case 5:
        a = m.r[8 + reg];
        return a + (u32)(s32)(s16)fetch();
    case 6:
        return indexed(m.r[8 + reg]);
    default:
        switch (reg) {
        case 0:
            return (u32)(s32)(s16)fetch();
        case 1:
            return fetch32();
        case 2:
            a = m.pc;
            return a + (u32)(s32)(s16)fetch();
        default:
            return indexed(m.pc);
        }
    }
}

/* a control operand's time, by its mode (Dn An (An) (An)+ -(An) d16 d8 abs.W abs.L d16(PC) d8(PC) #): LEA's,
   JMP's (PEA's is LEA's + 8, JSR's JMP's + 8), MOVEM's beyond its (An) */
static const u8 lea_time[12] = {0, 0, 4, 0, 0, 8, 12, 8, 12, 8, 12, 0};
static const u8 jmp_time[12] = {0, 0, 8, 0, 0, 10, 14, 10, 12, 10, 14, 0};
static const u8 movem_time[12] = {0, 0, 0, 0, 0, 4, 6, 4, 8, 4, 6, 0};

INLINE int ctl(u32 mode, u32 reg)
{
    return mode < 7 ? (int)mode : 7 + (int)reg;
}

INLINE u32 src(int s)                    /* the operand of the low six bits */
{
    u32 mode = MODE, reg = RY;
    if (mode == 0)
        return m.r[reg] & msk(s);
    if (mode == 1)
        return m.r[8 + reg] & msk(s);
    return rd(ea(mode, reg, s), s);
}

static int cond(u32 cc)
{
    switch (cc & 15) {
    case 0: return 1;
    case 1: return 0;
    case 2: return !(m.c & 0x100) && m.z;
    case 3: return (m.c & 0x100) || !m.z;
    case 4: return !(m.c & 0x100);
    case 5: return (m.c & 0x100) != 0;
    case 6: return m.z != 0;
    case 7: return !m.z;
    case 8: return !(m.v & 0x80);
    case 9: return (m.v & 0x80) != 0;
    case 10: return !(m.n & 0x80);
    case 11: return (m.n & 0x80) != 0;
    case 12: return !((m.n ^ m.v) & 0x80);
    case 13: return ((m.n ^ m.v) & 0x80) != 0;
    case 14: return m.z && !((m.n ^ m.v) & 0x80);
    default: return !m.z || ((m.n ^ m.v) & 0x80);
    }
}

INLINE void logic(u32 r, int s)
{
    m.n = nf(r, s);
    m.z = r & msk(s);
    m.v = 0;
    m.c = 0;
}

INLINE u32 add_flags(u32 sv, u32 d, int s, u32 xin, int addx)
{
    u32 r = d + sv + xin;
    if (s == 4) {
        m.c = m.x = ((sv & d) | (~r & (sv | d))) >> 23;
        m.v = ((sv ^ r) & (d ^ r)) >> 24;
    } else {
        m.c = m.x = s == 1 ? r : r >> 8;
        m.v = s == 1 ? (sv ^ r) & (d ^ r) : ((sv ^ r) & (d ^ r)) >> 8;
    }
    m.n = nf(r, s);
    if (addx)
        m.z |= r & msk(s);
    else
        m.z = r & msk(s);
    return r & msk(s);
}

INLINE u32 sub_flags(u32 sv, u32 d, int s, u32 xin, int subx, int cmp)
{
    u32 r = d - sv - xin;
    if (s == 4) {
        m.c = ((sv & r) | (~d & (sv | r))) >> 23;
        m.v = ((sv ^ d) & (r ^ d)) >> 24;
    } else {
        m.c = s == 1 ? r : r >> 8;
        m.v = s == 1 ? (sv ^ d) & (r ^ d) : ((sv ^ d) & (r ^ d)) >> 8;
    }
    if (!cmp)
        m.x = m.c;
    m.n = nf(r, s);
    if (subx)
        m.z |= r & msk(s);
    else
        m.z = r & msk(s);
    return r & msk(s);
}

/* a read-modify-write of the low six bits' operand: d the value, EXPR makes res */
#define RMW(s, EXPR)                                                                                                \
    do {                                                                                                            \
        u32 mode_ = MODE, reg_ = RY, a_ = 0, d, res;                                                                \
        if (mode_ == 0)                                                                                             \
            d = m.r[reg_] & msk(s);                                                                                 \
        else {                                                                                                      \
            a_ = ea(mode_, reg_, s);                                                                                \
            d = rd(a_, s);                                                                                          \
        }                                                                                                           \
        EXPR;                                                                                                       \
        if (mode_ == 0)                                                                                             \
            set_dn(reg_, res, s);                                                                                   \
        else                                                                                                        \
            wr(a_, res, s);                                                                                         \
    } while (0)

/* ---- the instructions ---- */
static void op_illegal(void)            /* ILLEGAL and the opcodes that are not the 68000's; lines A and F */
{
    u32 v = (OP & 0xF000) == 0xA000 ? 10 : (OP & 0xF000) == 0xF000 ? 11 : 4;
    exception(v, m.pc - 2);
}

static void privileged(void)
{
    exception(8, m.pc - 2);
}

/* MOVE, MOVEA */
INLINE void move(int s)
{
    u32 v = src(s), mode = (OP >> 6) & 7, reg = RX;
    if (mode == 0) {
        logic(v, s);
        set_dn(reg, v, s);
        m.clock += 4;
        return;
    }
    if (mode == 1) {                    /* MOVEA: the word sign-extended, no flags */
        m.r[8 + reg] = sx(v, s);
        m.clock += 4;
        return;
    }
    logic(v, s);
    u32 a = ea(mode, reg, s);
    if (mode == 4)
        m.clock -= 2;                   /* (-(An) as a destination takes no longer than (An)) */
    wr(a, v, s);
    m.clock += 4;
}
static void op_move_b(void) { move(1); }
static void op_move_w(void) { move(2); }
static void op_move_l(void) { move(4); }

static void op_moveq(void)
{
    u32 v = (u32)(s32)(s8)OP;
    m.r[RX] = v;
    logic(v, 4);
    m.clock += 4;
}

/* the arithmetic and logic of lines 8, 9, B, C, D: <ea>,Dn and Dn,<ea> */
enum { K_OR, K_AND, K_SUB, K_ADD, K_CMP, K_EOR };

INLINE u32 alu(int k, u32 sv, u32 d, int s)
{
    switch (k) {
    case K_OR: d |= sv; logic(d, s); return d;
    case K_AND: d &= sv; logic(d, s); return d;
    case K_EOR: d ^= sv; logic(d, s); return d;
    case K_ADD: return add_flags(sv, d, s, 0, 0);
    case K_SUB: return sub_flags(sv, d, s, 0, 0, 0);
    default: return sub_flags(sv, d, s, 0, 0, 1);
    }
}

INLINE void alu_to_dn(int k, int s)      /* <ea> op Dn -> Dn */
{
    u32 mode = MODE;
    u32 sv = src(s), r = RX;
    u32 res = alu(k, sv, m.r[r] & msk(s), s);
    if (k != K_CMP)
        set_dn(r, res, s);
    if (s != 4)
        m.clock += 4;
    else
        m.clock += k != K_CMP && (mode == 0 || mode == 1 || (mode == 7 && (m.op & 7) == 4)) ? 8 : 6;
}

INLINE void alu_to_ea(int k, int s)      /* Dn op <ea> -> <ea> */
{
    u32 sv = m.r[RX] & msk(s);
    if (MODE == 0) {                    /* (EOR Dn,Dn) */
        u32 r = RY;
        set_dn(r, alu(k, sv, m.r[r] & msk(s), s), s);
        m.clock += s == 4 ? 8 : 4;
        return;
    }
    RMW(s, res = alu(k, sv, d, s));
    m.clock += s == 4 ? 12 : 8;
}

#define ALU_EA(name, k)                                                                                             \
    static void op_##name##_b_ea(void) { alu_to_ea(k, 1); }                                                         \
    static void op_##name##_w_ea(void) { alu_to_ea(k, 2); }                                                         \
    static void op_##name##_l_ea(void) { alu_to_ea(k, 4); }
#define ALU(name, k)                                                                                                \
    static void op_##name##_b_dn(void) { alu_to_dn(k, 1); }                                                         \
    static void op_##name##_w_dn(void) { alu_to_dn(k, 2); }                                                         \
    static void op_##name##_l_dn(void) { alu_to_dn(k, 4); }                                                         \
    ALU_EA(name, k)
ALU(or, K_OR)
ALU(and, K_AND)
ALU(sub, K_SUB)
ALU(add, K_ADD)
ALU_EA(eor, K_EOR)

static void op_cmp_b(void) { alu_to_dn(K_CMP, 1); }
static void op_cmp_w(void) { alu_to_dn(K_CMP, 2); }
static void op_cmp_l(void) { alu_to_dn(K_CMP, 4); }

/* ADDA, SUBA, CMPA */
INLINE void adda(int kind, int s)        /* 0 ADDA, 1 SUBA, 2 CMPA */
{
    u32 mode = MODE;
    u32 sv = sx(src(s), s), r = 8 + RX;
    if (kind == 0)
        m.r[r] += sv;
    else if (kind == 1)
        m.r[r] -= sv;
    else
        sub_flags(sv, m.r[r], 4, 0, 0, 1);
    if (kind == 2)
        m.clock += 6;
    else
        m.clock += s == 2 ? 8 : (mode == 0 || mode == 1 || (mode == 7 && RY == 4)) ? 8 : 6;
}
static void op_adda_w(void) { adda(0, 2); }
static void op_adda_l(void) { adda(0, 4); }
static void op_suba_w(void) { adda(1, 2); }
static void op_suba_l(void) { adda(1, 4); }
static void op_cmpa_w(void) { adda(2, 2); }
static void op_cmpa_l(void) { adda(2, 4); }

/* ORI, ANDI, SUBI, ADDI, EORI, CMPI */
INLINE void imm_op(int k, int s)
{
    u32 iv = s == 4 ? fetch32() : fetch() & msk(s);
    if (MODE == 0) {
        u32 r = RY;
        u32 res = alu(k, iv, m.r[r] & msk(s), s);
        if (k != K_CMP)
            set_dn(r, res, s);
        m.clock += s == 4 ? (k == K_CMP ? 14 : 16) : 8;
        return;
    }
    if (k == K_CMP) {
        u32 a = ea(MODE, RY, s);
        alu(k, iv, rd(a, s), s);
        m.clock += s == 4 ? 12 : 8;
        return;
    }
    RMW(s, res = alu(k, iv, d, s));
    m.clock += s == 4 ? 20 : 12;
}
#define IMM(name, k)                                                                                                \
    static void op_##name##_b(void) { imm_op(k, 1); }                                                               \
    static void op_##name##_w(void) { imm_op(k, 2); }                                                               \
    static void op_##name##_l(void) { imm_op(k, 4); }
IMM(ori, K_OR)
IMM(andi, K_AND)
IMM(subi, K_SUB)
IMM(addi, K_ADD)
IMM(eori, K_EOR)
IMM(cmpi, K_CMP)

/* ORI / ANDI / EORI to CCR and to SR */
static void op_ccr_sr(void)
{
    u32 to_sr = OP & 0x40, k = (OP >> 9) & 7;
    if (to_sr && !m.s) {
        privileged();
        return;
    }
    u32 iv = fetch();
    u32 v = get_sr();
    if (!to_sr)
        iv &= 0x1F;
    if (k == 0)
        v |= iv;
    else if (k == 1)
        v &= to_sr ? iv : (iv | 0xFF00);
    else
        v ^= iv;
    if (to_sr)
        set_sr(v);
    else
        set_ccr(v);
    m.clock += 20;
}

/* ADDQ, SUBQ */
INLINE void addq(int sub, int s)
{
    u32 q = ((OP >> 9) & 7) ? (OP >> 9) & 7 : 8;
    if (MODE == 1) {                    /* to An: the whole register, no flags */
        if (sub)
            m.r[8 + RY] -= q;
        else
            m.r[8 + RY] += q;
        m.clock += 8;
        return;
    }
    if (MODE == 0) {
        u32 r = RY;
        set_dn(r, sub ? sub_flags(q, m.r[r] & msk(s), s, 0, 0, 0) : add_flags(q, m.r[r] & msk(s), s, 0, 0), s);
        m.clock += s == 4 ? 8 : 4;
        return;
    }
    RMW(s, res = sub ? sub_flags(q, d, s, 0, 0, 0) : add_flags(q, d, s, 0, 0));
    m.clock += s == 4 ? 12 : 8;
}
static void op_addq_b(void) { addq(0, 1); }
static void op_addq_w(void) { addq(0, 2); }
static void op_addq_l(void) { addq(0, 4); }
static void op_subq_b(void) { addq(1, 1); }
static void op_subq_w(void) { addq(1, 2); }
static void op_subq_l(void) { addq(1, 4); }

/* ADDX, SUBX (Dy,Dx or -(Ay),-(Ax)) */
INLINE void addx(int sub, int s)
{
    u32 xin = (m.x >> 8) & 1;
    if (!(OP & 8)) {
        u32 sv = m.r[RY] & msk(s), d = m.r[RX] & msk(s);
        set_dn(RX, sub ? sub_flags(sv, d, s, xin, 1, 0) : add_flags(sv, d, s, xin, 1), s);
        m.clock += s == 4 ? 8 : 4;
        return;
    }
    u32 dec = (s == 1) ? 1 : (u32)s, ry = 8 + RY, rx = 8 + RX;
    m.r[ry] -= (s == 1 && ry == 15) ? 2 : dec;
    u32 sv = rd(m.r[ry], s);
    m.r[rx] -= (s == 1 && rx == 15) ? 2 : dec;
    u32 d = rd(m.r[rx], s);
    wr(m.r[rx], sub ? sub_flags(sv, d, s, xin, 1, 0) : add_flags(sv, d, s, xin, 1), s);
    m.clock += s == 4 ? 30 : 18;
}
static void op_addx_b(void) { addx(0, 1); }
static void op_addx_w(void) { addx(0, 2); }
static void op_addx_l(void) { addx(0, 4); }
static void op_subx_b(void) { addx(1, 1); }
static void op_subx_w(void) { addx(1, 2); }
static void op_subx_l(void) { addx(1, 4); }

/* CMPM (Ay)+,(Ax)+ */
INLINE void cmpm(int s)
{
    u32 ry = 8 + RY, rx = 8 + RX;
    u32 sv = rd(m.r[ry], s);
    m.r[ry] += (s == 1 && ry == 15) ? 2 : (u32)s;
    u32 d = rd(m.r[rx], s);
    m.r[rx] += (s == 1 && rx == 15) ? 2 : (u32)s;
    sub_flags(sv, d, s, 0, 0, 1);
    m.clock += s == 4 ? 20 : 12;
}
static void op_cmpm_b(void) { cmpm(1); }
static void op_cmpm_w(void) { cmpm(2); }
static void op_cmpm_l(void) { cmpm(4); }

/* CLR, NEG, NEGX, NOT, TST */
INLINE void unary(int k, int s)          /* 0 CLR, 1 NEG, 2 NEGX */
{
    u32 mode = MODE;
    if (k == 0) {                       /* (no read first: a register of the chips is not read) */
        if (mode == 0)
            set_dn(RY, 0, s);
        else
            wr(ea(mode, RY, s), 0, s);
        logic(0, s);
        m.clock += mode == 0 ? (s == 4 ? 6 : 4) : (s == 4 ? 12 : 8);
        return;
    }
    u32 xin = (m.x >> 8) & 1;
    RMW(s, res = k == 1 ? sub_flags(d, 0, s, 0, 0, 0) : sub_flags(d, 0, s, xin, 1, 0));
    m.clock += mode == 0 ? (s == 4 ? 6 : 4) : (s == 4 ? 12 : 8);
}
static void op_clr_b(void) { unary(0, 1); }
static void op_clr_w(void) { unary(0, 2); }
static void op_clr_l(void) { unary(0, 4); }
static void op_neg_b(void) { unary(1, 1); }
static void op_neg_w(void) { unary(1, 2); }
static void op_neg_l(void) { unary(1, 4); }
static void op_negx_b(void) { unary(2, 1); }
static void op_negx_w(void) { unary(2, 2); }
static void op_negx_l(void) { unary(2, 4); }

INLINE void op_not(int s)
{
    RMW(s, res = ~d & msk(s); logic(res, s));
    m.clock += MODE == 0 ? (s == 4 ? 6 : 4) : (s == 4 ? 12 : 8);
}
static void op_not_b(void) { op_not(1); }
static void op_not_w(void) { op_not(2); }
static void op_not_l(void) { op_not(4); }

INLINE void tst(int s)
{
    logic(src(s), s);
    m.clock += 4;
}
static void op_tst_b(void) { tst(1); }
static void op_tst_w(void) { tst(2); }
static void op_tst_l(void) { tst(4); }

static void op_tas(void)
{
    RMW(1, logic(d, 1); res = d | 0x80);
    m.clock += MODE == 0 ? 4 : 10;
}

/* Bcc, BRA, BSR */
static void op_bcc(void)
{
    u32 base = m.pc, cc = (OP >> 8) & 15;
    u32 d = (u32)(s32)(s8)OP;
    if (!(OP & 0xFF))
        d = (u32)(s32)(s16)fetch();
    if (cc == 1) {                      /* BSR */
        m.r[15] -= 4;
        wl(m.r[15], m.pc);
        m.pc = base + d;
        m.clock += 18;
        return;
    }
    if (cond(cc)) {
        m.pc = base + d;
        m.clock += 10;
    } else {
        m.clock += (OP & 0xFF) ? 8 : 12;
    }
}

static void op_dbcc(void)
{
    u32 base = m.pc;
    u32 d = (u32)(s32)(s16)fetch();
    if (cond(OP >> 8)) {
        m.clock += 12;
        return;
    }
    u32 r = RY, n = (m.r[r] - 1) & 0xFFFF;
    m.r[r] = (m.r[r] & 0xFFFF0000u) | n;
    if (n != 0xFFFF) {
        m.pc = base + d;
        m.clock += 10;
    } else {
        m.clock += 14;
    }
}

static void op_scc(void)
{
    u32 v = cond(OP >> 8) ? 0xFF : 0;
    if (MODE == 0) {
        set_dn(RY, v, 1);
        m.clock += v ? 6 : 4;
        return;
    }
    wb(ea(MODE, RY, 1), v);
    m.clock += 8;
}

/* LEA, PEA, JMP, JSR */
static void op_lea(void)
{
    u32 mode = MODE, reg = RY;
    m.clock += lea_time[ctl(mode, reg)];
    m.r[8 + RX] = ea_ctl(mode, reg);
}

static void op_pea(void)
{
    u32 mode = MODE, reg = RY;
    u32 a = ea_ctl(mode, reg);
    m.r[15] -= 4;
    wl(m.r[15], a);
    m.clock += 8 + lea_time[ctl(mode, reg)];
}

static void op_jmp(void)
{
    u32 mode = MODE, reg = RY;
    m.clock += jmp_time[ctl(mode, reg)];
    m.pc = ea_ctl(mode, reg);
}

static void op_jsr(void)
{
    u32 mode = MODE, reg = RY;
    u32 a = ea_ctl(mode, reg);
    m.r[15] -= 4;
    wl(m.r[15], m.pc);
    m.pc = a;
    m.clock += 8 + jmp_time[ctl(mode, reg)];
}

static void op_rts(void)
{
    m.pc = rl(m.r[15]);
    m.r[15] += 4;
    m.clock += 16;
}

static void op_rtr(void)
{
    set_ccr(rw(m.r[15]));
    m.pc = rl(m.r[15] + 2);
    m.r[15] += 6;
    m.clock += 20;
}

static void op_rte(void)
{
    if (!m.s) {
        privileged();
        return;
    }
    u32 sp = m.r[15], sr = rw(sp);
    m.pc = rl(sp + 2);
    m.r[15] = sp + 6;
    set_sr(sr);
    m.clock += 20;
}

static void op_nop(void)
{
    m.clock += 4;
}

static void op_stop(void)
{
    if (!m.s) {
        privileged();
        return;
    }
    set_sr(fetch());
    m.stopped = 1;
    m.clock += 4;
}

static void op_reset(void)
{
    if (!m.s) {
        privileged();
        return;
    }
    cpu_reset_line();
    m.clock += 132;
}

static void op_trapv(void)
{
    m.clock += 4;
    if (m.v & 0x80)
        exception(7, m.pc);
}

static void op_trap(void)
{
    exception(32 + (OP & 15), m.pc);
}

static void op_link(void)
{
    u32 r = 8 + RY, v = m.r[r];
    m.r[15] -= 4;
    wl(m.r[15], v);
    m.r[r] = m.r[15];
    m.r[15] += (u32)(s32)(s16)fetch();
    m.clock += 16;
}

static void op_unlk(void)
{
    u32 r = 8 + RY;
    m.r[15] = m.r[r];
    m.r[r] = rl(m.r[15]);
    if (r != 15)
        m.r[15] += 4;
    m.clock += 12;
}

static void op_move_usp(void)
{
    if (!m.s) {
        privileged();
        return;
    }
    if (OP & 8)
        m.r[8 + RY] = m.other_sp;
    else
        m.other_sp = m.r[8 + RY];
    m.clock += 4;
}

static void op_swap(void)
{
    u32 r = RY, v = m.r[r] >> 16 | m.r[r] << 16;
    m.r[r] = v;
    logic(v, 4);
    m.clock += 4;
}

static void op_ext_w(void)
{
    u32 r = RY, v = (u32)(s32)(s8)m.r[r] & 0xFFFF;
    set_dn(r, v, 2);
    logic(v, 2);
    m.clock += 4;
}

static void op_ext_l(void)
{
    u32 r = RY, v = (u32)(s32)(s16)m.r[r];
    m.r[r] = v;
    logic(v, 4);
    m.clock += 4;
}

static void op_exg(void)
{
    u32 a, b;
    switch ((OP >> 3) & 0x1F) {
    case 0x08: a = RX, b = RY; break;
    case 0x09: a = 8 + RX, b = 8 + RY; break;
    default: a = RX, b = 8 + RY; break;
    }
    u32 t = m.r[a];
    m.r[a] = m.r[b];
    m.r[b] = t;
    m.clock += 6;
}

/* MOVE from SR, to CCR, to SR */
static void op_move_from_sr(void)
{
    u32 v = get_sr();
    if (MODE == 0) {
        set_dn(RY, v, 2);
        m.clock += 6;
        return;
    }
    u32 a = ea(MODE, RY, 2);
    ww(a, v);
    m.clock += 8;
}

static void op_move_to_ccr(void)
{
    set_ccr(src(2));
    m.clock += 12;
}

static void op_move_to_sr(void)
{
    if (!m.s) {
        privileged();
        return;
    }
    set_sr(src(2));
    m.clock += 12;
}

/* MOVEM */
static void op_movem(void)
{
    u32 mask = fetch(), mode = MODE, reg = RY, to_regs = OP & 0x400, s = (OP & 0x40) ? 4 : 2;
    u32 n = 0;
    if (!to_regs && mode == 4) {        /* registers to -(An): from A7 down to D0 */
        u32 a = m.r[8 + reg], start = a;
        for (int i = 15; i >= 0; i--)
            if (mask & (1u << (15 - i))) {
                a -= s;
                u32 v = (u32)i == 8 + reg ? start : m.r[i];
                if (s == 4)
                    wl(a, v);
                else
                    ww(a, v);
                n++;
            }
        m.r[8 + reg] = a;
        m.clock += 8 + (int)(n * (s == 4 ? 8 : 4));
        return;
    }
    u32 a = mode == 3 ? m.r[8 + reg] : ea_ctl(mode, reg);
    int t = movem_time[ctl(mode, reg)];
    for (int i = 0; i < 16; i++)
        if (mask & (1u << i)) {
            if (to_regs)
                m.r[i] = s == 4 ? rl(a) : (u32)(s32)(s16)rw(a);
            else if (s == 4)
                wl(a, m.r[i]);
            else
                ww(a, m.r[i]);
            a += s;
            n++;
        }
    if (mode == 3)
        m.r[8 + reg] = a;
    m.clock += (to_regs ? 12 : 8) + t + (int)(n * (s == 4 ? 8 : 4));
}

/* MOVEP: bytes at every other address */
static void op_movep(void)
{
    u32 a = m.r[8 + RY] + (u32)(s32)(s16)fetch(), r = RX;
    switch ((OP >> 6) & 3) {
    case 0:
        set_dn(r, rb(a) << 8 | rb(a + 2), 2);
        m.clock += 16;
        break;
    case 1:
        m.r[r] = rb(a) << 24 | rb(a + 2) << 16 | rb(a + 4) << 8 | rb(a + 6);
        m.clock += 24;
        break;
    case 2:
        wb(a, m.r[r] >> 8);
        wb(a + 2, m.r[r]);
        m.clock += 16;
        break;
    default:
        wb(a, m.r[r] >> 24);
        wb(a + 2, m.r[r] >> 16);
        wb(a + 4, m.r[r] >> 8);
        wb(a + 6, m.r[r]);
        m.clock += 24;
        break;
    }
}

/* BTST, BCHG, BCLR, BSET: the bit number from Dn or an extension word */
static void op_bitop(void)
{
    u32 bit = (OP & 0x100) ? m.r[RX] : fetch(), kind = (OP >> 6) & 3, mode = MODE;
    if (mode == 0) {
        u32 r = RY, b = 1u << (bit & 31);
        m.z = m.r[r] & b;
        if (kind == 1)
            m.r[r] ^= b;
        else if (kind == 2)
            m.r[r] &= ~b;
        else if (kind == 3)
            m.r[r] |= b;
        m.clock += (kind == 0 ? 6 : kind == 2 ? 10 : 8) + ((OP & 0x100) ? 0 : 4);
        return;
    }
    u32 b = 1u << (bit & 7);
    if (kind == 0) {
        m.z = src(1) & b;
        m.clock += (OP & 0x100) ? 4 : 8;
        return;
    }
    RMW(1, m.z = d & b; res = kind == 1 ? d ^ b : kind == 2 ? d & ~b : d | b);
    m.clock += (OP & 0x100) ? 8 : 12;
}

/* MULU, MULS, DIVU, DIVS */
static void op_mulu(void)
{
    u32 sv = src(2), r = RX, v = (m.r[r] & 0xFFFF) * sv;
    m.r[r] = v;
    logic(v, 4);
    int n = 0;
    for (u32 b = sv; b; b >>= 1)
        n += b & 1;
    m.clock += 38 + 2 * n;
}

static void op_muls(void)
{
    u32 sv = src(2), r = RX;
    u32 v = (u32)((s32)(s16)m.r[r] * (s32)(s16)sv);
    m.r[r] = v;
    logic(v, 4);
    int n = 0;
    for (u32 b = ((sv << 1) ^ sv) & 0xFFFF; b; b >>= 1)
        n += b & 1;
    m.clock += 38 + 2 * n;
}

static void op_divu(void)
{
    u32 sv = src(2), r = RX;
    if (!sv) {
        m.c = 0;
        exception(5, m.pc);
        return;
    }
    u32 q = m.r[r] / sv, rem = m.r[r] % sv;
    m.clock += 140;
    if (q > 0xFFFF) {
        m.v = 0x80;
        m.n = 0x80;
        m.z = 1;
        m.c = 0;
        return;
    }
    m.r[r] = q | rem << 16;
    m.n = q >> 8;
    m.z = q;
    m.v = 0;
    m.c = 0;
}

static void op_divs(void)
{
    s32 sv = (s16)src(2);
    u32 r = RX;
    if (!sv) {
        m.c = 0;
        exception(5, m.pc);
        return;
    }
    s32 d = (s32)m.r[r];
    m.clock += 158;
    if (d == (s32)0x80000000 && sv == -1) {
        m.v = 0x80;
        m.n = 0x80;
        m.z = 1;
        m.c = 0;
        return;
    }
    s32 q = d / sv, rem = d % sv;
    if (q < -32768 || q > 32767) {
        m.v = 0x80;
        m.n = 0x80;
        m.z = 1;
        m.c = 0;
        return;
    }
    m.r[r] = ((u32)q & 0xFFFF) | (u32)rem << 16;
    m.n = (u32)q >> 8;
    m.z = (u32)q & 0xFFFF;
    m.v = 0;
    m.c = 0;
}

static void op_chk(void)
{
    s32 bound = (s16)src(2), v = (s16)m.r[RX];
    m.clock += 10;
    m.z = (u32)v & 0xFFFF;
    m.v = 0;
    m.c = 0;
    m.n = v < 0 ? 0x80 : 0;
    if (v < 0 || v > bound)
        exception(6, m.pc);
}

/* ABCD, SBCD, NBCD: the decimal corrections and the (undocumented) V the 68000 really gives */
static u32 bcd_add(u32 sv, u32 d)
{
    u32 ss = sv + d + ((m.x >> 8) & 1);
    u32 bc = ((sv & d) | (~ss & (sv | d))) & 0x88;
    u32 dc = (((ss + 0x66) ^ ss) & 0x110) >> 1;
    u32 corf = (bc | dc) - ((bc | dc) >> 2);
    u32 r = ss + corf;
    m.x = m.c = (((bc | (ss & ~r)) >> 7) & 1) << 8;
    m.v = ~ss & r & 0x80;
    r &= 0xFF;
    m.n = r;
    m.z |= r;
    return r;
}

static u32 bcd_sub(u32 sv, u32 d)
{
    u32 dd = d - sv - ((m.x >> 8) & 1);
    u32 bc = ((~d & sv) | (dd & ~(d ^ sv))) & 0x88;
    u32 corf = bc - (bc >> 2);
    u32 r = dd - corf;
    m.x = m.c = (((bc | (~dd & r)) >> 7) & 1) << 8;
    m.v = dd & ~r & 0x80;
    r &= 0xFF;
    m.n = r;
    m.z |= r;
    return r;
}

static void op_abcd_sbcd(void)
{
    int sub = (OP & 0xF000) == 0x8000;
    if (!(OP & 8)) {
        u32 r = RX;
        set_dn(r, sub ? bcd_sub(m.r[RY] & 0xFF, m.r[r] & 0xFF) : bcd_add(m.r[RY] & 0xFF, m.r[r] & 0xFF), 1);
        m.clock += 6;
        return;
    }
    u32 ry = 8 + RY, rx = 8 + RX;
    m.r[ry] -= ry == 15 ? 2 : 1;
    u32 sv = rb(m.r[ry]);
    m.r[rx] -= rx == 15 ? 2 : 1;
    u32 d = rb(m.r[rx]);
    wb(m.r[rx], sub ? bcd_sub(sv, d) : bcd_add(sv, d));
    m.clock += 18;
}

static void op_nbcd(void)
{
    RMW(1, res = bcd_sub(d, 0));
    m.clock += MODE == 0 ? 6 : 8;
}

/* the shifts and rotations: register (count in the opcode or a register), memory (by one, a word) */
INLINE u32 shift(u32 type, u32 left, u32 v, u32 n, int s)
{
    u32 bits = (u32)s * 8, mk = msk(s), sign = 1u << (bits - 1), r = v;
    if (!n) {
        m.c = type == 2 ? m.x : 0;
        m.v = 0;
        m.n = nf(v, s);
        m.z = v;
        return v;
    }
    switch (type) {
    case 0:                             /* AS */
        if (left) {
            u32 top;
            if (n < bits) {
                r = (v << n) & mk;
                m.c = m.x = ((v >> (bits - n)) & 1) << 8;
                top = (bits - n - 1) ? v >> (bits - n - 1) : v;
                u32 all = n + 1 >= 32 ? 0xFFFFFFFFu : (1u << (n + 1)) - 1;
                m.v = (top != 0 && top != all) ? 0x80 : 0;
            } else {
                r = 0;
                m.c = m.x = (n == bits ? (v & 1) : 0) << 8;
                m.v = v ? 0x80 : 0;
            }
        } else {
            u32 neg = v & sign;
            if (n < bits) {
                r = v >> n;
                if (neg)
                    r |= mk & ~(mk >> n);
                m.c = m.x = ((v >> (n - 1)) & 1) << 8;
            } else {
                r = neg ? mk : 0;
                m.c = m.x = neg ? 0x100 : 0;
            }
            m.v = 0;
        }
        break;
    case 1:                             /* LS */
        if (left) {
            if (n < bits) {
                r = (v << n) & mk;
                m.c = m.x = ((v >> (bits - n)) & 1) << 8;
            } else {
                r = 0;
                m.c = m.x = (n == bits ? (v & 1) : 0) << 8;
            }
        } else {
            if (n < bits) {
                r = v >> n;
                m.c = m.x = ((v >> (n - 1)) & 1) << 8;
            } else {
                r = 0;
                m.c = m.x = (n == bits ? (v >> (bits - 1)) & 1 : 0) << 8;
            }
        }
        m.v = 0;
        break;
    case 2: {                           /* ROX: through X, bits + 1 wide */
        u32 x = (m.x >> 8) & 1;
        n %= bits + 1;
        for (u32 i = 0; i < n; i++) {
            u32 out;
            if (left) {
                out = (r >> (bits - 1)) & 1;
                r = ((r << 1) | x) & mk;
            } else {
                out = r & 1;
                r = (r >> 1) | (x << (bits - 1));
            }
            x = out;
        }
        m.c = m.x = x << 8;
        m.v = 0;
        break;
    }
    default: {                          /* RO */
        u32 k = n & (bits - 1);
        if (k)
            r = left ? ((v << k) | (v >> (bits - k))) & mk : ((v >> k) | (v << (bits - k))) & mk;
        m.c = (left ? r & 1 : (r >> (bits - 1)) & 1) << 8;
        m.v = 0;
        break;
    }
    }
    m.n = nf(r, s);
    m.z = r;
    return r;
}

INLINE void shift_reg(int s)
{
    u32 n = (OP & 0x20) ? m.r[RX] & 63 : (((OP >> 9) & 7) ? (OP >> 9) & 7 : 8);
    u32 r = RY;
    set_dn(r, shift((OP >> 3) & 3, OP & 0x100, m.r[r] & msk(s), n, s), s);
    m.clock += (s == 4 ? 8 : 6) + 2 * (int)n;
}
static void op_shift_b(void) { shift_reg(1); }
static void op_shift_w(void) { shift_reg(2); }
static void op_shift_l(void) { shift_reg(4); }

static void op_shift_mem(void)
{
    u32 a = ea(MODE, RY, 2);
    ww(a, shift((OP >> 9) & 3, OP & 0x100, rw(a), 1, 2));
    m.clock += 8;
}

/* ---- the decoder ---- */
#define EA_DN    0x001u
#define EA_AN    0x002u
#define EA_MEM   0x1FCu                 /* (An) (An)+ -(An) d16(An) d8(An,Xn) abs.W abs.L */
#define EA_DA    (EA_DN | EA_MEM)       /* data alterable */
#define EA_ALT   (EA_DA | EA_AN)
#define EA_DATA  (EA_DA | 0xE00u)       /* + d16(PC) d8(PC,Xn) #imm */
#define EA_ALL   (EA_DATA | EA_AN)
#define EA_CTL   0x7E4u                 /* (An) d16(An) d8(An,Xn) abs.W abs.L d16(PC) d8(PC,Xn) */

static int ea_ok(u32 op, u32 allowed)
{
    u32 mode = (op >> 3) & 7, reg = op & 7, k = mode < 7 ? mode : reg <= 4 ? 7 + reg : 12;
    return k < 12 && (allowed >> k & 1);
}

#define HANDLERS(X)                                                                                                 \
    X(illegal) X(move_b) X(move_w) X(move_l) X(moveq)                                                               \
    X(or_b_dn) X(or_w_dn) X(or_l_dn) X(or_b_ea) X(or_w_ea) X(or_l_ea)                                               \
    X(and_b_dn) X(and_w_dn) X(and_l_dn) X(and_b_ea) X(and_w_ea) X(and_l_ea)                                         \
    X(sub_b_dn) X(sub_w_dn) X(sub_l_dn) X(sub_b_ea) X(sub_w_ea) X(sub_l_ea)                                         \
    X(add_b_dn) X(add_w_dn) X(add_l_dn) X(add_b_ea) X(add_w_ea) X(add_l_ea)                                         \
    X(eor_b_ea) X(eor_w_ea) X(eor_l_ea) X(cmp_b) X(cmp_w) X(cmp_l)                                                  \
    X(adda_w) X(adda_l) X(suba_w) X(suba_l) X(cmpa_w) X(cmpa_l)                                                     \
    X(ori_b) X(ori_w) X(ori_l) X(andi_b) X(andi_w) X(andi_l) X(subi_b) X(subi_w) X(subi_l)                          \
    X(addi_b) X(addi_w) X(addi_l) X(eori_b) X(eori_w) X(eori_l) X(cmpi_b) X(cmpi_w) X(cmpi_l) X(ccr_sr)             \
    X(addq_b) X(addq_w) X(addq_l) X(subq_b) X(subq_w) X(subq_l)                                                     \
    X(addx_b) X(addx_w) X(addx_l) X(subx_b) X(subx_w) X(subx_l) X(cmpm_b) X(cmpm_w) X(cmpm_l)                       \
    X(clr_b) X(clr_w) X(clr_l) X(neg_b) X(neg_w) X(neg_l) X(negx_b) X(negx_w) X(negx_l)                             \
    X(not_b) X(not_w) X(not_l) X(tst_b) X(tst_w) X(tst_l) X(tas)                                                    \
    X(bcc) X(dbcc) X(scc) X(lea) X(pea) X(jmp) X(jsr) X(rts) X(rtr) X(rte) X(nop) X(stop) X(reset) X(trapv)         \
    X(trap) X(link) X(unlk) X(move_usp) X(swap) X(ext_w) X(ext_l) X(exg)                                            \
    X(move_from_sr) X(move_to_ccr) X(move_to_sr) X(movem) X(movep) X(bitop)                                         \
    X(mulu) X(muls) X(divu) X(divs) X(chk) X(abcd_sbcd) X(nbcd) X(shift_b) X(shift_w) X(shift_l) X(shift_mem)

#define ENUM(name) H_##name,
enum { HANDLERS(ENUM) H_COUNT };
#define PTR(name) op_##name,
static void (*const handler[H_COUNT])(void) = {HANDLERS(PTR)};
static u8 optab[65536];

static int decode(u32 op)
{
    u32 hi = op >> 12, sz = (op >> 6) & 3, mode = (op >> 3) & 7, opm = (op >> 6) & 7;
    switch (hi) {
    case 0x0:
        if (op & 0x100) {
            if (mode == 1)
                return H_movep;
            return ea_ok(op, sz == 0 ? (EA_DATA & ~0u) : EA_DA) ? H_bitop : H_illegal;
        }
        switch ((op >> 9) & 7) {
        case 4:                         /* static bit operations */
            if (sz == 0)
                return ea_ok(op, EA_DATA & ~0x800u) ? H_bitop : H_illegal;
            return ea_ok(op, EA_DA) ? H_bitop : H_illegal;
        case 0: case 1: case 5:         /* ORI ANDI EORI: to CCR (.b) / SR (.w) when the operand is #imm */
            if ((op & 0x3F) == 0x3C && sz < 2)
                return H_ccr_sr;
            /* fall through */
        case 2: case 3: case 6: {
            static const u8 base[8] = {H_ori_b, H_andi_b, H_subi_b, H_addi_b, 0, H_eori_b, H_cmpi_b, 0};
            if (sz == 3 || !ea_ok(op, EA_DA))
                return H_illegal;
            return base[(op >> 9) & 7] + (int)sz;
        }
        default:
            return H_illegal;
        }
    case 0x1: case 0x2: case 0x3: {
        u32 dmode = (op >> 6) & 7, dreg = (op >> 9) & 7;
        int h = hi == 1 ? H_move_b : hi == 3 ? H_move_w : H_move_l;
        if (!ea_ok(op, hi == 1 ? EA_DATA : EA_ALL))
            return H_illegal;
        if (dmode == 1)
            return hi == 1 ? H_illegal : h;
        if (dmode == 7 && dreg > 1)
            return H_illegal;
        return h;
    }
    case 0x4:
        if (op & 0x100) {
            if (sz == 3)
                return ea_ok(op, EA_CTL) ? H_lea : H_illegal;
            if (sz == 2)
                return ea_ok(op, EA_DATA) ? H_chk : H_illegal;
            return H_illegal;
        }
        switch ((op >> 8) & 0xF) {
        case 0x0:
            if (sz == 3)
                return ea_ok(op, EA_DA) ? H_move_from_sr : H_illegal;
            return ea_ok(op, EA_DA) ? H_negx_b + (int)sz : H_illegal;
        case 0x2:
            return sz < 3 && ea_ok(op, EA_DA) ? H_clr_b + (int)sz : H_illegal;
        case 0x4:
            if (sz == 3)
                return ea_ok(op, EA_DATA) ? H_move_to_ccr : H_illegal;
            return ea_ok(op, EA_DA) ? H_neg_b + (int)sz : H_illegal;
        case 0x6:
            if (sz == 3)
                return ea_ok(op, EA_DATA) ? H_move_to_sr : H_illegal;
            return ea_ok(op, EA_DA) ? H_not_b + (int)sz : H_illegal;
        case 0x8:
            if (sz == 0)
                return ea_ok(op, EA_DA) ? H_nbcd : H_illegal;
            if (sz == 1)
                return mode == 0 ? H_swap : ea_ok(op, EA_CTL) ? H_pea : H_illegal;
            if (mode == 0)
                return sz == 2 ? H_ext_w : H_ext_l;
            return ea_ok(op, EA_CTL & ~0x600u) || mode == 4 ? H_movem : H_illegal;
        case 0xA:
            if (op == 0x4AFC)
                return H_illegal;
            if (sz == 3)
                return ea_ok(op, EA_DA) ? H_tas : H_illegal;
            return ea_ok(op, EA_DA) ? H_tst_b + (int)sz : H_illegal;
        case 0xC:
            if (sz < 2)
                return H_illegal;
            return ea_ok(op, EA_CTL) || mode == 3 ? H_movem : H_illegal;
        case 0xE:
            if (sz == 1) {
                switch ((op >> 3) & 7) {
                case 0: case 1: return H_trap;
                case 2: return H_link;
                case 3: return H_unlk;
                case 4: case 5: return H_move_usp;
                case 6:
                    switch (op & 7) {
                    case 0: return H_reset;
                    case 1: return H_nop;
                    case 2: return H_stop;
                    case 3: return H_rte;
                    case 5: return H_rts;
                    case 6: return H_trapv;
                    case 7: return H_rtr;
                    default: return H_illegal;
                    }
                default: return H_illegal;
                }
            }
            if (sz == 2)
                return ea_ok(op, EA_CTL) ? H_jsr : H_illegal;
            if (sz == 3)
                return ea_ok(op, EA_CTL) ? H_jmp : H_illegal;
            return H_illegal;
        default:
            return H_illegal;
        }
    case 0x5:
        if (sz == 3) {
            if (mode == 1)
                return H_dbcc;
            return ea_ok(op, EA_DA) ? H_scc : H_illegal;
        }
        if (!ea_ok(op, EA_ALT) || (mode == 1 && sz == 0))
            return H_illegal;
        return ((op & 0x100) ? H_subq_b : H_addq_b) + (int)sz;
    case 0x6:
        return H_bcc;
    case 0x7:
        return (op & 0x100) ? H_illegal : H_moveq;
    case 0x8: case 0xC: {
        int and = hi == 0xC;
        if (opm == 3)
            return ea_ok(op, EA_DATA) ? (and ? H_mulu : H_divu) : H_illegal;
        if (opm == 7)
            return ea_ok(op, EA_DATA) ? (and ? H_muls : H_divs) : H_illegal;
        if (opm == 4 && mode < 2)
            return H_abcd_sbcd;
        if (and && (opm == 5 || opm == 6) && mode < 2) {
            u32 k = (op >> 3) & 0x1F;
            return (k == 0x08 || k == 0x09 || k == 0x11) ? H_exg : H_illegal;
        }
        if (opm < 3)
            return ea_ok(op, EA_DATA) ? (and ? H_and_b_dn : H_or_b_dn) + (int)opm : H_illegal;
        return ea_ok(op, EA_MEM) ? (and ? H_and_b_ea : H_or_b_ea) + (int)(opm - 4) : H_illegal;
    }
    case 0x9: case 0xD: {
        int add = hi == 0xD;
        if (opm == 3 || opm == 7)
            return ea_ok(op, EA_ALL) ? (add ? H_adda_w : H_suba_w) + (opm == 7) : H_illegal;
        if (opm < 3) {
            if (!ea_ok(op, EA_ALL) || (opm == 0 && mode == 1))
                return H_illegal;
            return (add ? H_add_b_dn : H_sub_b_dn) + (int)opm;
        }
        if (mode < 2)
            return (add ? H_addx_b : H_subx_b) + (int)(opm - 4);
        return ea_ok(op, EA_MEM) ? (add ? H_add_b_ea : H_sub_b_ea) + (int)(opm - 4) : H_illegal;
    }
    case 0xB:
        if (opm == 3 || opm == 7)
            return ea_ok(op, EA_ALL) ? H_cmpa_w + (opm == 7) : H_illegal;
        if (opm < 3) {
            if (!ea_ok(op, EA_ALL) || (opm == 0 && mode == 1))
                return H_illegal;
            return H_cmp_b + (int)opm;
        }
        if (mode == 1)
            return H_cmpm_b + (int)(opm - 4);
        return ea_ok(op, EA_DA) ? H_eor_b_ea + (int)(opm - 4) : H_illegal;
    case 0xE:
        if (sz == 3) {
            if (op & 0x800)
                return H_illegal;
            return ea_ok(op, EA_MEM) ? H_shift_mem : H_illegal;
        }
        return H_shift_b + (int)sz;
    default:
        return H_illegal;
    }
}

static void cpu_init(void)
{
    for (u32 op = 0; op < 65536; op++)
        optab[op] = (u8)decode(op);
}

static void cpu_reset(void)
{
    for (int i = 0; i < 16; i++)
        m.r[i] = 0;
    m.s = 1, m.t = 0, m.im = 7;
    m.other_sp = 0;
    set_ccr(0);
    m.stopped = 0, m.fault = 0, m.ipl = 0;
    m.r[15] = rl(0);
    m.pc = rl(4);
    check_pending();
}

/* an interrupt (autovectored: 25..31), a trace, before the next instruction */
static void take_pending(void)
{
    if (m.ipl > m.im) {
        u32 level = m.ipl, sr = get_sr();
        supervisor();
        m.im = level;
        m.r[15] -= 4;
        wl(m.r[15], m.pc);
        m.r[15] -= 2;
        ww(m.r[15], sr);
        m.pc = rl((24 + level) * 4);
        m.stopped = 0;
        m.clock += 44;
    }
    check_pending();
}

/* the 68000 until `until` cycles of the frame */
static void cpu_run(s32 until)
{
    while (m.clock < until) {
        if (m.pending) {
            if (m.ipl > m.im) {
                take_pending();
                continue;
            }
            if (m.stopped) {
                m.clock = until;
                break;
            }
            m.op = fetch();             /* the trace: an exception after the instruction run with T set */
            handler[optab[m.op]]();
            if (m.fault)
                address_error();
            exception(9, m.pc);
            continue;
        }
        if (m.stopped) {
            m.clock = until;
            break;
        }
        m.op = fetch();
        handler[optab[m.op]]();
        if (m.fault)
            address_error();
    }
}

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
    u32 len = aud[ch].len ? aud[ch].len : 65536, per = aud[ch].per < 124 ? 124 : aud[ch].per;
    return (s32)(len * per * 4);
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

