/* A ZX Spectrum 48K: see zx.h.

   The frame: 312 lines of 224 T-states (69888, at 3.5 MHz).  The ULA holds
   the INT line low for the first 32 T-states; the picture's first pixel is
   drawn at 14336 (line 64), each of its lines 128 T-states of 256 pixels,
   then 96 of border and blanking.  While it draws the picture the ULA
   holds the CPU up when it touches 0x4000-0x7FFF (or an I/O port with bit
   0 clear), by 6 5 4 3 2 1 0 0 T-states as the access falls in each group
   of 8 -- z80.c asks this file's table at each access, the cycles where
   the CPU only works inside (with the address it leaves on the bus) too.

   The picture: each line's 256 pixels are drawn when the ULA starts on
   it, from screen memory as it is then (a program that changes the colours
   between lines gets that); the border from the colour changes of the
   frame, each at its T-state, two pixels a T-state.  A port the ULA does
   not answer reads the byte it is fetching from screen memory then (the
   "floating bus" some games wait on), else 0xFF.

   Tapes: no sound, no real time.  The ROM's LD-BYTES (0x0556) is caught:
   the tape's next block goes straight into memory and the ROM goes on as
   if it had read it.  So LOAD "" and every loader that uses the ROM's
   routine take no time; a game's own loader (a turbo one) finds no tape.
   After the ROM has started, LOAD "" and Enter are typed for a tape. */
#include "zx.h"
#include "z80.h"

#define LINE_T   224
#define LINES    312
#define FRAME_T  (LINE_T * LINES)       /* 69888 */
#define PAPER_T  14336                  /* the picture's first pixel: line 64 */
#define INT_T    32                     /* the INT line held low this long */
#define TOP      40                     /* the first line shown: 24 above the picture */
#define LD_BYTES 0x0556                 /* the ROM's routine that loads a block */
#define SA_LD_RET 0x053F                /* its end: the border back, BREAK looked at, EI, RET */

const u8 zx_rgb[16][3] = {
    {0x00, 0x00, 0x00}, {0x00, 0x00, 0xD7}, {0xD7, 0x00, 0x00}, {0xD7, 0x00, 0xD7},
    {0x00, 0xD7, 0x00}, {0x00, 0xD7, 0xD7}, {0xD7, 0xD7, 0x00}, {0xD7, 0xD7, 0xD7},
    {0x00, 0x00, 0x00}, {0x00, 0x00, 0xFF}, {0xFF, 0x00, 0x00}, {0xFF, 0x00, 0xFF},
    {0x00, 0xFF, 0x00}, {0x00, 0xFF, 0xFF}, {0xFF, 0xFF, 0x00}, {0xFF, 0xFF, 0xFF},
};

static struct z80 cpu;
static const u8 *rom;
static u8 ram[0xC000], junk[0x400];
static u8 cont[FRAME_T + 256];          /* the hold-up at each T-state of the frame (and a little after it) */
static u16 colour[16];
static u8 border, keys[8], kempston, ear_out;
static u32 frames, cycles_hi;
static u16 *pic;
static char info[96];

/* the border's colour changes in this frame */
#define CHANGES 4096
static s32 change_t[CHANGES];
static u8 change_c[CHANGES];
static int changes;
static u8 border_was;                   /* the colour at the frame's start */

/* the tape: its blocks (flag byte, data, checksum) */
#define BLOCKS 1024
static const u8 *block[BLOCKS];
static u32 block_n[BLOCKS];
static int blocks, block_at;
static u8 auto_load;                    /* type LOAD "" once the ROM has started */

/* ---- the keyboard ---- */
#define K(row, bit) ((row) * 8 + (bit))
#define CAPS 0x40                       /* (with CAPS SHIFT) */
#define SYM  0x80                       /* (with SYMBOL SHIFT) */
#define NONE 0xFF

/* a character's keys: row * 8 + bit, | CAPS / SYM */
static u8 char_keys(int ch)
{
    static const u8 letters[26] = {K(1, 0), K(7, 4), K(0, 3), K(1, 2), K(2, 2), K(1, 3), K(1, 4), K(6, 4), K(5, 2),
                                   K(6, 3), K(6, 2), K(6, 1), K(7, 2), K(7, 3), K(5, 1), K(5, 0), K(2, 0), K(2, 3),
                                   K(1, 1), K(2, 4), K(5, 3), K(0, 4), K(2, 1), K(0, 2), K(5, 4), K(0, 1)};
    static const u8 digits[10] = {K(4, 0), K(3, 0), K(3, 1), K(3, 2), K(3, 3), K(3, 4), K(4, 4), K(4, 3), K(4, 2),
                                  K(4, 1)};
    static const struct {
        char ch;
        u8 keys;
    } symbols[] = {
        {'!', SYM | K(3, 0)}, {'@', SYM | K(3, 1)}, {'#', SYM | K(3, 2)}, {'$', SYM | K(3, 3)}, {'%', SYM | K(3, 4)},
        {'&', SYM | K(4, 4)}, {'\'', SYM | K(4, 3)}, {'(', SYM | K(4, 2)}, {')', SYM | K(4, 1)}, {'_', SYM | K(4, 0)},
        {'<', SYM | K(2, 3)}, {'>', SYM | K(2, 4)}, {';', SYM | K(5, 1)}, {'"', SYM | K(5, 0)}, {'^', SYM | K(6, 4)},
        {'-', SYM | K(6, 3)}, {'+', SYM | K(6, 2)}, {'=', SYM | K(6, 1)}, {':', SYM | K(0, 1)}, {'`', SYM | K(0, 2)},
        {'?', SYM | K(0, 3)}, {'/', SYM | K(0, 4)}, {'*', SYM | K(7, 4)}, {',', SYM | K(7, 3)}, {'.', SYM | K(7, 2)},
        {' ', K(7, 0)}, {'\n', K(6, 0)}, {'\r', K(6, 0)}, {8, CAPS | K(4, 0)}, {127, CAPS | K(4, 0)},
    };
    if (ch >= 'a' && ch <= 'z')
        return letters[ch - 'a'];
    if (ch >= 'A' && ch <= 'Z')
        return CAPS | letters[ch - 'A'];
    if (ch >= '0' && ch <= '9')
        return digits[ch - '0'];
    for (unsigned i = 0; i < sizeof(symbols) / sizeof(symbols[0]); i++)
        if (symbols[i].ch == ch)
            return symbols[i].keys;
    return NONE;
}

static void press(u8 matrix[8], u8 k)
{
    if (k == NONE)
        return;
    matrix[(k & 0x3F) >> 3] |= (u8)(1u << (k & 7));
    if (k & CAPS)
        matrix[0] |= 0x01;
    if (k & SYM)
        matrix[7] |= 0x02;
}

void zx_keys_from_hid(const u8 *usages, int n, u8 matrix[8])
{
    static const char plain[] = "-=[]\\#;'`,./", shifted[] = "_+{}|~:\"~<>?";
    static const char digit_shifted[] = "!@#$%^&*()";
    int shift = 0, symbol_used = 0;
    for (int i = 0; i < 8; i++)
        matrix[i] = 0;
    for (int i = 0; i < n; i++)
        if (usages[i] == 0xE1 || usages[i] == 0xE5)
            shift = 1;
    for (int i = 0; i < n; i++) {
        u8 u = usages[i];
        if (u >= 0x04 && u <= 0x1D)
            press(matrix, char_keys('a' + (u - 0x04)));
        else if (u >= 0x1E && u <= 0x27) {
            int d = u == 0x27 ? 0 : u - 0x1D;
            if (shift) {                /* (Shift and a digit: the PC's symbol over it) */
                press(matrix, char_keys(digit_shifted[(d + 9) % 10]));
                symbol_used = 1;
            } else {
                press(matrix, char_keys('0' + d));
            }
        } else if (u == 0x28 || u == 0x58)
            press(matrix, K(6, 0));     /* Enter (and the keypad's) */
        else if (u == 0x2C)
            press(matrix, K(7, 0));
        else if (u == 0x2A || u == 0x4C)
            press(matrix, CAPS | K(4, 0));          /* Backspace, Delete: DELETE */
        else if (u == 0x29)
            press(matrix, CAPS | K(7, 0));          /* Esc: BREAK */
        else if (u >= 0x4F && u <= 0x52) {
            static const u8 arrow[4] = {K(4, 2), K(3, 4), K(4, 4), K(4, 3)};   /* right 8, left 5, down 6, up 7 */
            press(matrix, CAPS | arrow[u - 0x4F]);
        } else if (u >= 0x2D && u <= 0x38) {
            u8 k = char_keys((shift ? shifted : plain)[u - 0x2D]);
            press(matrix, k);
            if (k != NONE)
                symbol_used = 1;
        } else if (u == 0xE0 || u == 0xE2 || u == 0xE4 || u == 0xE6)
            matrix[7] |= 0x02;          /* Ctrl, Alt: SYMBOL SHIFT */
    }
    if (shift && !symbol_used)
        matrix[0] |= 0x01;
}

/* typed characters: each held 3 frames, then nothing for 6 (the ROM takes the same key twice only after that) */
static char queue[256];
static int q_head, q_tail, q_frame;

void zx_type(const char *text)
{
    while (*text) {
        int next = (q_tail + 1) & 255;
        if (next == q_head)
            break;
        queue[q_tail] = *text++;
        q_tail = next;
    }
}

int zx_typing(void)
{
    return (q_tail - q_head) & 255;
}

static void typed(u8 matrix[8])
{
    if (q_head == q_tail)
        return;
    if (q_frame < 3)
        press(matrix, char_keys(queue[q_head]));
    if (++q_frame == 9) {
        q_frame = 0;
        q_head = (q_head + 1) & 255;
    }
}

/* ---- the ULA ---- */
static u8 floating_bus(void)
{
    s32 t = cpu.cycles - 1 - PAPER_T;   /* (the IN reads at its last T-state) */
    if (t < 0 || t >= 192 * LINE_T)
        return 0xFF;
    int y = t / LINE_T, x = t % LINE_T;
    if (x >= 128 || (x & 7) >= 4)
        return 0xFF;
    int col = (x >> 3) * 2 + ((x >> 1) & 1);
    if (x & 1)
        return ram[0x1800 + (y >> 3) * 32 + col];
    return ram[((y & 0xC0) << 5) | ((y & 7) << 8) | ((y & 0x38) << 2) | col];
}

static u8 in(struct z80 *c, u16 port)
{
    (void)c;
    if (!(port & 1)) {                  /* the ULA: the keyboard's half-rows whose address bit is 0; bit 6 the
                                           EAR socket (no tape playing: what the last OUT put on it, as on an
                                           issue 3 board) */
        u8 v = (u8)(0xBF | ((ear_out & 0x10) << 2));
        for (int r = 0; r < 8; r++)
            if (!(port & (0x100u << r)))
                v &= (u8)~keys[r];
        return v;
    }
    if (!(port & 0xE0))
        return kempston;
    return floating_bus();
}

static void out(struct z80 *c, u16 port, u8 v)
{
    if (port & 1)
        return;
    ear_out = v;
    if ((v & 7) != border) {
        border = v & 7;
        if (changes < CHANGES) {
            change_t[changes] = c->cycles;
            change_c[changes++] = border;
        } else {
            change_c[CHANGES - 1] = border;
        }
    }
}

static void write(struct z80 *c, u16 a, u8 v)
{
    (void)c, (void)a, (void)v;          /* (the ROM) */
}

static void paper_line(int y)
{
    u16 *out = pic + (24 + y) * ZX_W + 32;
    const u8 *bits = ram + (((y & 0xC0) << 5) | ((y & 7) << 8) | ((y & 0x38) << 2));
    const u8 *attr = ram + 0x1800 + (y >> 3) * 32;
    u8 flash = (frames & 16) ? 0xFF : 0;
    for (int col = 0; col < 32; col++, out += 8) {
        u8 a = attr[col], b = bits[col];
        u16 ink = colour[(a & 7) | ((a >> 3) & 8)], paper = colour[((a >> 3) & 7) | ((a >> 3) & 8)];
        if (a & 0x80)
            b ^= flash;
        for (int i = 0; i < 8; i++, b <<= 1)
            out[i] = (b & 0x80) ? ink : paper;
    }
}

static void border_lines(void)
{
    int at = 0;
    u16 now = colour[border_was];
    for (int y = 0; y < ZX_H; y++) {
        int line = TOP + y;
        s32 t0 = line * LINE_T - 16;    /* (pixel 0: 32 pixels before the picture's) */
        u16 *out = pic + y * ZX_W;
        int paper = line >= 64 && line < 256;
        for (int x = 0; x < ZX_W; x += 2) {
            if (paper && x == 32)
                x = 288;
            s32 t = t0 + (x >> 1);
            while (at < changes && change_t[at] <= t)
                now = colour[change_c[at++]];
            out[x] = out[x + 1] = now;
        }
    }
}

/* ---- the tape ---- */
static void tape_block(void)
{
    /* LD-BYTES: A the flag byte wanted, IX where to, DE how many, carry set to load (clear: to verify) */
    const u8 *b = block[block_at];
    u32 n = block_n[block_at], len = cpu.de.w, k = 0;
    int ok = 0;
    block_at++;
    if (n >= 2 && b[0] == cpu.af.b.h) {
        u8 sum = b[0];
        int load = cpu.af.b.l & Z80_FC;
        for (; k < len && k + 2 < n; k++) {
            u8 v = b[1 + k];
            u16 a = (u16)(cpu.ix.w + k);
            sum ^= v;
            if (load && a >= 0x4000)
                ram[a - 0x4000] = v;
        }
        ok = k == len && (sum ^ b[1 + k]) == 0;
        cpu.ix.w = (u16)(cpu.ix.w + k);
        cpu.de.w = (u16)(cpu.de.w - k);
    }
    cpu.af.b.l = (u8)(ok ? cpu.af.b.l | Z80_FC : cpu.af.b.l & ~Z80_FC);
    cpu.pc.w = SA_LD_RET;               /* (its stack is as LD-BYTES found it: it returns to LD-BYTES' caller) */
    if (block_at >= blocks)
        cpu.trap = -1;                  /* (the tape's end: the ROM waits for a tape that does not come) */
}

static void run_to(s32 t)
{
    while (z80_run(&cpu, t))
        tape_block();
}

/* ---- the outside ---- */
void zx_frame(u8 pad, const u8 matrix[8], u16 *picture)
{
    pic = picture;
    if (auto_load) {                    /* the ROM started: its frame counter (FRAMES) going (not the RAM test's
                                           pattern there: no more than the frames run) */
        u32 count = (u32)(ram[0x1C78] | ram[0x1C79] << 8);
        if (count >= 20 && count <= frames) {
            zx_type("j\"\"\n");         /* LOAD "" (J is LOAD where a keyword goes) */
            auto_load = 0;
        }
    }
    u8 now[8] = {0};
    if (matrix)
        for (int r = 0; r < 8; r++)
            now[r] = matrix[r];
    typed(now);
    if (pad & ZX_START)
        now[6] |= 0x01;
    if (pad & ZX_SELECT)
        now[7] |= 0x01;
    for (int r = 0; r < 8; r++)
        keys[r] = now[r] & 0x1F;
    kempston = (u8)(((pad & ZX_RIGHT) ? 1 : 0) | ((pad & ZX_LEFT) ? 2 : 0) | ((pad & ZX_DOWN) ? 4 : 0) |
                    ((pad & ZX_UP) ? 8 : 0) | ((pad & (ZX_A | ZX_B)) ? 0x10 : 0));
    border_was = border;
    changes = 0;
    cpu.irq = 1;
    run_to(INT_T);
    cpu.irq = 0;
    for (int y = 0; y < 192; y++) {
        run_to(PAPER_T + y * LINE_T);
        if (pic)
            paper_line(y);
    }
    run_to(FRAME_T);
    if (pic)
        border_lines();
    cpu.cycles -= FRAME_T;
    cycles_hi += FRAME_T;
    frames++;
}

static void map(void)
{
    for (int i = 0; i < 16; i++) {
        cpu.rd[i] = (u8 *)rom + (i << 10);
        cpu.wr[i] = 0;
    }
    for (int i = 16; i < 64; i++)
        cpu.rd[i] = cpu.wr[i] = ram + ((i - 16) << 10);
    (void)junk;
}

void zx_reset(void)
{
    static const u8 delay[8] = {6, 5, 4, 3, 2, 1, 0, 0};
    for (int t = 0; t < FRAME_T + 256; t++) {
        int d = t - (PAPER_T - 1);
        cont[t] = (d >= 0 && d < 192 * LINE_T && d % LINE_T < 128) ? delay[d & 7] : 0;
    }
    for (int i = 0; i < 16; i++)
        colour[i] = (u16)(zx_rgb[i][0] >> 3 | (zx_rgb[i][1] >> 3) << 5 | (zx_rgb[i][2] >> 3) << 10);
    z80_init(&cpu);
    cpu.write = write;
    cpu.in = in;
    cpu.out = out;
    cpu.cont_lo = 0x4000;
    cpu.cont_n = 0x4000;
    cpu.cont = cont;
    map();
    for (unsigned i = 0; i < sizeof(ram); i++)
        ram[i] = 0;
    border = 7, border_was = 7, changes = 0, ear_out = 0;
    frames = 0, cycles_hi = 0;
    q_head = q_tail = q_frame = 0;
    block_at = 0;
    cpu.trap = blocks ? LD_BYTES : -1;
    auto_load = blocks != 0;
}

/* ---- files ---- */
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

static void add_block(const u8 *b, u32 n)
{
    if (blocks < BLOCKS) {
        block[blocks] = b;
        block_n[blocks++] = n;
    }
}

static const char *tap(const u8 *f, u32 size)
{
    for (u32 at = 0; at + 2 <= size;) {
        u32 n = (u32)(f[at] | f[at + 1] << 8);
        if (at + 2 + n > size)
            return "a .tap file whose last block is cut short";
        add_block(f + at + 2, n);
        at += 2 + n;
    }
    return blocks ? 0 : "an empty .tap file";
}

/* a .tzx: its data blocks (standard speed, turbo, pure data) as the ROM would read them; the rest skipped */
static const char *tzx(const u8 *f, u32 size)
{
    static const char sig[8] = {'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1A};
    for (int i = 0; i < 8; i++)
        if (size < 10 || f[i] != (u8)sig[i])
            return "not a .tzx file (no ZXTape! signature)";
    u32 at = 10;
#define W16(p) ((u32)((p)[0] | (p)[1] << 8))
#define W24(p) ((u32)((p)[0] | (p)[1] << 8 | (p)[2] << 16))
#define W32(p) ((u32)((p)[0] | (p)[1] << 8 | (p)[2] << 16 | (u32)(p)[3] << 24))
    while (at < size) {
        u8 id = f[at++], tail[24] = {0};
        const u8 *p = f + at;
        u32 skip;
        if (size - at < 20) {           /* (near the end: a block's header read from a copy, never past it) */
            for (u32 i = 0; at + i < size; i++)
                tail[i] = f[at + i];
            p = tail;
        }
        switch (id) {
        case 0x10: skip = 4 + W16(p + 2); if (at + skip <= size) add_block(f + at + 4, W16(p + 2)); break;
        case 0x11: skip = 18 + W24(p + 15); if (at + skip <= size) add_block(f + at + 18, W24(p + 15)); break;
        case 0x14: skip = 10 + W24(p + 7); if (at + skip <= size) add_block(f + at + 10, W24(p + 7)); break;
        case 0x12: skip = 4; break;
        case 0x13: skip = 1 + 2 * (u32)p[0]; break;
        case 0x15: skip = 8 + W24(p + 5); break;
        case 0x18: case 0x19: skip = 4 + W32(p); break;
        case 0x20: case 0x23: case 0x24: skip = 2; break;
        case 0x21: case 0x30: skip = 1 + (u32)p[0]; break;
        case 0x22: case 0x25: case 0x27: skip = 0; break;
        case 0x26: skip = 2 + 2 * W16(p); break;
        case 0x28: case 0x32: skip = 2 + W16(p); break;
        case 0x2A: skip = 4; break;
        case 0x2B: skip = 5; break;
        case 0x31: skip = 2 + (u32)p[1]; break;
        case 0x33: skip = 1 + 3 * (u32)p[0]; break;
        case 0x35: skip = 20 + W32(p + 16); break;
        case 0x5A: skip = 9; break;
        default:
            if (blocks)
                return 0;               /* (a block it does not know: the tape up to there) */
            return "a .tzx block this player does not know (it reads the data blocks: 0x10, 0x11, 0x14)";
        }
        if (at + skip > size)
            break;                      /* (cut short) */
        at += skip;
    }
    return blocks ? 0 : "a .tzx file with no data blocks";
}

/* .z80's compression: ED ED n b is n bytes b */
static u32 unpack(const u8 *src, u32 n, u8 *to, u32 room)
{
    u32 i = 0, o = 0;
    while (i < n && o < room) {
        if (i + 3 < n && src[i] == 0xED && src[i + 1] == 0xED) {
            for (u32 k = 0; k < src[i + 2] && o < room; k++)
                to[o++] = src[i + 3];
            i += 4;
        } else {
            to[o++] = src[i++];
        }
    }
    return i;
}

static const char *snapshot_z80(const u8 *h, u32 size)
{
    if (size < 30)
        return "a .z80 file shorter than its header";
    u8 flags = h[12] == 0xFF ? 1 : h[12];
    u16 pc = (u16)(h[6] | h[7] << 8);
    int version = 1;
    if (!pc) {
        u32 extra = W16(h + 30);
        version = extra == 23 ? 2 : 3;
        if (size < 32 + extra || extra < 23)
            return "a .z80 file shorter than its header";
        u8 mode = h[34];
        if (!(mode == 0 || mode == 1 || (version == 3 && mode == 3)))
            return "a snapshot of a 128K (or other) Spectrum -- this is a 48K one";
        pc = (u16)(h[32] | h[33] << 8);
        for (u32 at = 32 + extra; at + 3 <= size;) {
            u32 n = W16(h + at);
            u8 page = h[at + 2];
            at += 3;
            u32 length = n == 0xFFFF ? 0x4000 : n;
            if (at + length > size)
                return "a .z80 file cut short";
            int to = page == 8 ? 0x0000 : page == 4 ? 0x4000 : page == 5 ? 0x8000 : -1;
            if (to >= 0) {
                if (n == 0xFFFF)
                    for (u32 i = 0; i < 0x4000; i++)
                        ram[to + i] = h[at + i];
                else
                    unpack(h + at, n, ram + to, 0x4000);
            }
            at += length;
        }
    } else if (flags & 0x20) {
        unpack(h + 30, size - 30, ram, 0xC000);
    } else {
        if (size < 30 + 0xC000)
            return "a .z80 file cut short";
        for (u32 i = 0; i < 0xC000; i++)
            ram[i] = h[30 + i];
    }
    cpu.af.b.h = h[0], cpu.af.b.l = h[1];
    cpu.bc.w = (u16)W16(h + 2), cpu.hl.w = (u16)W16(h + 4);
    cpu.pc.w = pc;
    cpu.sp.w = (u16)W16(h + 8);
    cpu.i = h[10];
    cpu.r = h[11] & 0x7F, cpu.r7 = (u8)((flags & 1) << 7);
    border = (flags >> 1) & 7;
    cpu.de.w = (u16)W16(h + 13);
    cpu.bc2 = (u16)W16(h + 15), cpu.de2 = (u16)W16(h + 17), cpu.hl2 = (u16)W16(h + 19);
    cpu.af2 = (u16)(h[21] << 8 | h[22]);
    cpu.iy.w = (u16)W16(h + 23), cpu.ix.w = (u16)W16(h + 25);
    cpu.iff1 = h[27] ? 1 : 0, cpu.iff2 = h[28] ? 1 : 0;
    cpu.im = h[29] & 3;
    char *p = put(info, "a .z80 snapshot, version ");
    put_number(p, (u32)version);
    return 0;
}

static const char *snapshot_sna(const u8 *h, u32 size)
{
    if (size != 27 + 0xC000)
        return size == 27 + 0xC000 + 4 + 0x4000 * 5 || size == 27 + 0xC000 + 4 + 0x4000 * 6
                   ? "a 128K .sna snapshot -- this is a 48K Spectrum"
                   : "not a 48K .sna snapshot (it is 49179 bytes)";
    for (u32 i = 0; i < 0xC000; i++)
        ram[i] = h[27 + i];
    cpu.i = h[0];
    cpu.hl2 = (u16)W16(h + 1), cpu.de2 = (u16)W16(h + 3), cpu.bc2 = (u16)W16(h + 5), cpu.af2 = (u16)W16(h + 7);
    cpu.hl.w = (u16)W16(h + 9), cpu.de.w = (u16)W16(h + 11), cpu.bc.w = (u16)W16(h + 13);
    cpu.iy.w = (u16)W16(h + 15), cpu.ix.w = (u16)W16(h + 17);
    cpu.iff1 = cpu.iff2 = (h[19] & 4) ? 1 : 0;
    cpu.r = h[20] & 0x7F, cpu.r7 = h[20] & 0x80;
    cpu.af.w = (u16)W16(h + 21);
    cpu.sp.w = (u16)W16(h + 23);
    cpu.im = h[25] & 3;
    border = h[26] & 7;
    /* the PC is on the stack (as a RETN would take it) */
    u16 sp = cpu.sp.w;
    u8 lo = cpu.rd[sp >> 10][sp & 0x3FF];
    sp++;
    cpu.pc.w = (u16)(lo | cpu.rd[sp >> 10][sp & 0x3FF] << 8);
    cpu.sp.w = (u16)(cpu.sp.w + 2);
    put(info, "a .sna snapshot");
    return 0;
}

const char *zx_load(const u8 *rom16k, u8 *file, u32 size, const char *name)
{
    if (!rom16k || rom16k[0] != 0xF3 || rom16k[1] != 0xAF)
        return "no 48K ROM (its first bytes are DI, XOR A)";
    rom = rom16k;
    blocks = 0;
    const char *why = 0;
    int tape = 0;
    if (file && (ends_with(name, ".tap") || ends_with(name, ".tzx"))) {
        why = ends_with(name, ".tap") ? tap(file, size) : tzx(file, size);
        if (why) {
            blocks = 0;
            return why;
        }
        tape = 1;
    }
    zx_reset();
    if (tape) {
        u32 bytes = 0;
        for (int i = 0; i < blocks; i++)
            bytes += block_n[i];
        char *p = put(info, "a tape, ");
        p = put_number(p, (u32)blocks);
        p = put(p, blocks == 1 ? " block (" : " blocks (");
        p = put_number(p, (bytes + 1023) >> 10);
        put(p, " KB); LOAD \"\" is typed");
        return 0;
    }
    if (!file) {
        put(info, "BASIC");
        return 0;
    }
    if (ends_with(name, ".z80"))
        why = snapshot_z80(file, size);
    else if (ends_with(name, ".sna"))
        why = snapshot_sna(file, size);
    else
        why = "not a Spectrum file this player knows (.tap, .tzx, .z80, .sna)";
    if (why)
        zx_reset();
    return why;
}

const char *zx_info(void)
{
    return info;
}

u8 *zx_save_ram(u32 *size, int *battery)
{
    *size = 0;
    *battery = 0;
    return ram;
}

int zx_save_changed(void)
{
    return 0;
}

u32 zx_cpu_cycles(void)
{
    return cycles_hi + (u32)cpu.cycles;
}

int zx_jammed(void)
{
    return cpu.halted && !cpu.iff1;
}

struct z80 *zx_cpu(void)
{
    return &cpu;
}

u8 zx_peek(u16 a)
{
    return cpu.rd[a >> 10][a & 0x3FF];
}
