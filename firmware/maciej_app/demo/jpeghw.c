/* jpeghw.c: baseline JPEG decoded by the S3811's video engine -- the JPEG mode of its MPEG-2 block (0xB8004200,
   "hardware version 1": extension registers at 0xB8004100), driven as the box's own firmware drives it for
   MJPEG (dump.bin 0x800FEB48..: the same registers) and as the SDK's picture decoder does (libplus_hd.a:
   imagedec_acc36's jh_* accessors, jh_m3602_hw_init, imagedec_main's buildhuff_hwtable / write_hufftable and
   decode_next_row_hw_vld).  The engine does the Huffman decoding, the dequantisation, the IDCT and the
   colour planes' layout, one MCU per start; the CPU parses the headers, loads the tables, points the engine at
   each MCU's first bit and reads where it stopped.

   Registers (off 0xB8004200; EXT: off 0xB8004100):
     +0x10  work mode: b17, b27 cleared (Huffman decoding by the engine)
     +0x14  b0-7 start (0x0C: an MCU with the engine's VLD), b24-26 the standard (3: JPEG; MPEG-2's is 2),
            b27-31 the chroma format (0 4:2:0, 1 4:1:1, 2 4:2:2, 3 4:4:4, 4 4:4:0, 5 grey); +0x17 b6: the DC
            predictors reset (at the start and each restart marker)
     +0x18  status: b8 the VLD busy, b19 the JPEG engine busy
     +0x24  the bitstream's start (b0-23, b24-26 into b27-29) with its control in b24-26 (7: the data's end is
            inside), +0x28 the bit to start at (read back: where the MCU ended), +0x2C the bits there are,
            +0x30 the end (b0-23)
     +0x7C  the Huffman tables (index << 16 | value), +0x80 the quantisers (luma << 14 | i << 8 | q), +0xC4
            b0-5 errors
     EXT +0 / +4  the luma / chroma frame (physical), +8 b0-1 / b2-3 the horizontal / vertical pre-scaling
            (1/2^n), b4-13 the MCUs a row less one, b16-31 the frame's stride, +0x0C the MCU's x (b0-10) and
            y (b16-26)
   The frames are the MPEG-2 decoder's tiles (av.c): luma 32 x 16 pixels, 512 bytes, row after row of tiles;
   chroma Cb / Cr pairs at half the width, 16 pairs x 16 lines a tile -- a line for every two luma lines from a
   4:2:0 file, for every line from 4:2:2 and 4:4:4 (the engine makes 4:4:4 4:2:2: half its colour across).  Both
   shown by dumping the frames from the box and matching them against Pillow's decode.

   What it takes: 8-bit baseline (SOF0 / SOF1, Huffman), one scan with every component, grey, 4:2:0, 4:2:2 or
   4:4:4 (4:1:1 and 4:4:0 have codes, but their frames' layout is untried: no such file was at hand), Cb and Cr
   with the same tables, the quantisers 1..127 (the engine keeps 7 bits: quality 50 and better), restart
   markers, a file under 12 MB.  The rest (progressive, 12-bit, arithmetic, CMYK, a table it lacks) is image.c's.
   An engine that does not move -- the emulator's, whose registers are plain RAM -- is found at the first MCU
   (no bits read): image.c then.

   On the box (2026-11-18, 'view -c': both decoders at full size, every value compared): 1920 x 1080 4:2:0 in
   56 ms against the CPU's 1030 ms, 4000 x 2250 in 227 against 4075, 1280 x 720 4:2:2 in 40 against 566; the
   largest difference 2 or 3 (the IDCTs' rounding), none above 8 -- grey, an odd size (1001 x 777) and a file of
   67 restart markers too.  In the viewer (pre-scaled to no less than shown): 1080p 504 ms against 1221, the
   4000 x 2250 photo 689 against 4721, a 6000 x 4000 one (1/4) 985 against 14381 -- the rest of the time is the
   viewer's fitting to the screen and this file's conversion to RGB, not the engine. */
#include "hw.h"
#include "image.h"
#include "av.h"

#define VE(o)           REG32(0xB8004200u + (o))
#define VE8(o)          REG8(0xB8004200u + (o))
#define EXT(o)          REG32(0xB8004100u + (o))
#define CONSOLE         REG32(0xB8004000u)
#define SYS8(o)         REG8(0xB8000000u + (o))
#define MCU_TIMEOUT_MS  50

static u32 be16(const u8 *p)
{
    return (u32)p[0] << 8 | p[1];
}

static struct jh {
    u32 w, h, ncomp, code, mcu_w, mcu_h, mcus_x, mcus_y, dri, shift, ow, oh, stride, rows;
    u8 id[3], hs[3], vs[3], tq[3], td[3], ta[3];
    u8 q[4][64];                        /* zigzag order, as in the file */
    u8 qdef[4], hdef[2][4];
    u8 bits[2][4][17];                  /* class (DC, AC), table: the codes of each length 1..16 */
    u8 vals[2][4][256];
    const u8 *data, *scan, *end;        /* the file, its entropy-coded data, the file's end */
    u32 data_phys;
} j;

/* the file's headers up to its scan: 0, or why the engine does not take it (image.c's decoder will) */
static const char *parse(const u8 *d, u32 n)
{
    u32 at = 2;
    int sof = 0;
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8)
        return "not a JPEG";
    for (;;) {
        if (at + 4 > n)
            return "no scan";
        if (d[at] != 0xFF) {
            at++;                       /* (stray bytes between markers: as a decoder skips them) */
            continue;
        }
        u8 m = d[at + 1];
        if (m == 0xFF) {
            at++;
            continue;
        }
        at += 2;
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD8))
            continue;
        if (m == 0xD9)
            return "no scan";
        u32 len = be16(d + at);
        if (len < 2 || at + len > n)
            return "a damaged marker";
        const u8 *p = d + at + 2;
        u32 l = len - 2;
        if (m == 0xC0 || m == 0xC1) {
            if (l < 6 || p[0] != 8)
                return "not 8-bit";
            j.h = be16(p + 1), j.w = be16(p + 3), j.ncomp = p[5];
            if (j.ncomp != 1 && j.ncomp != 3)
                return "not grey or YCbCr";
            if (l < 6 + 3 * j.ncomp || !j.w || !j.h)
                return "a damaged frame header";
            for (u32 c = 0; c < j.ncomp; c++) {
                j.id[c] = p[6 + 3 * c];
                j.hs[c] = p[7 + 3 * c] >> 4, j.vs[c] = p[7 + 3 * c] & 15;
                j.tq[c] = p[8 + 3 * c];
                if (j.tq[c] > 3)
                    return "a damaged frame header";
            }
            sof = 1;
        } else if (m >= 0xC2 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            return m == 0xC2 ? "progressive" : "not baseline";
        } else if (m == 0xC4) {
            for (u32 k = 0; k < l;) {
                u32 tc = p[k] >> 4, th = p[k] & 15, total = 0;
                if (tc > 1 || th > 3 || k + 17 > l)
                    return "a damaged Huffman table";
                for (u32 i = 1; i <= 16; i++)
                    total += j.bits[tc][th][i] = p[k + i];
                if (total > 256 || k + 17 + total > l)
                    return "a damaged Huffman table";
                for (u32 i = 0; i < 256; i++)
                    j.vals[tc][th][i] = i < total ? p[k + 17 + i] : 0;
                j.hdef[tc][th] = 1;
                k += 17 + total;
            }
        } else if (m == 0xDB) {
            for (u32 k = 0; k < l;) {
                u32 pq = p[k] >> 4, tq = p[k] & 15;
                if (pq)
                    return "16-bit quantisers";
                if (tq > 3 || k + 65 > l)
                    return "a damaged quantiser table";
                for (u32 i = 0; i < 64; i++)
                    j.q[tq][i] = p[k + 1 + i];
                j.qdef[tq] = 1;
                k += 65;
            }
        } else if (m == 0xDD) {
            if (l < 2)
                return "a damaged restart interval";
            j.dri = be16(p);
        } else if (m == 0xDA) {
            if (!sof)
                return "a scan before its frame";
            if (l < 1 || p[0] != j.ncomp || l < 4 + 2 * j.ncomp)
                return "not one scan with every component";
            for (u32 c = 0; c < j.ncomp; c++) {
                if (p[1 + 2 * c] != j.id[c])
                    return "scan components out of order";
                j.td[c] = p[2 + 2 * c] >> 4, j.ta[c] = p[2 + 2 * c] & 15;
                if (j.td[c] > 3 || j.ta[c] > 3)
                    return "a damaged scan header";
            }
            const u8 *s = p + 1 + 2 * j.ncomp;
            if (s[0] != 0 || s[1] != 63 || s[2] != 0)
                return "not a sequential scan";
            j.scan = d + at + len;
            j.end = d + n;
            return 0;
        }
        at += len;
    }
}

/* the file read: what the engine does with it, at what pre-scaling for a picture shown at least min_w x min_h:
   0, or why not (image.c's decoder then) */
const char *jpeghw_plan(const u8 *data, u32 size, u32 data_phys, u32 min_w, u32 min_h, u32 *out_w, u32 *out_h)
{
    u8 *z = (u8 *)&j;
    for (u32 i = 0; i < sizeof(j); i++)
        z[i] = 0;
    const char *why = parse(data, size);
    if (why)
        return why;
    j.data = data;
    j.data_phys = data_phys;
    if (j.ncomp == 1) {
        j.code = 5, j.mcu_w = j.mcu_h = 8;
    } else {
        if (j.hs[1] != 1 || j.vs[1] != 1 || j.hs[2] != 1 || j.vs[2] != 1)
            return "chroma sampled more than once a block";
        u32 hv = (u32)j.hs[0] << 4 | j.vs[0];
        j.code = hv == 0x22 ? 0 : hv == 0x21 ? 2 : hv == 0x11 ? 3 : 9;
        if (j.code == 9)                /* (4:1:1 and 4:4:0 the engine has codes for, 1 and 4: their frames' layout
                                           untried -- no such file at hand) */
            return "a chroma sampling not tried with the engine";
        j.mcu_w = 8u * j.hs[0], j.mcu_h = 8u * j.vs[0];
        if (j.tq[1] != j.tq[2] || j.td[1] != j.td[2] || j.ta[1] != j.ta[2])
            return "Cb and Cr with tables of their own";
    }
    for (u32 c = 0; c < j.ncomp; c++) {
        if (!j.qdef[j.tq[c]] || !j.hdef[0][j.td[c]] || !j.hdef[1][j.ta[c]])
            return "a table missing";
        for (u32 i = 0; i < 64; i++)
            if (!j.q[j.tq[c]][i] || j.q[j.tq[c]][i] > 127)
                return "quantisers outside 1..127";
        u32 dc = 0, ac = 0;
        for (u32 i = 1; i <= 16; i++)
            dc += j.bits[0][j.td[c]][i], ac += j.bits[1][j.ta[c]][i];
        if (dc > 16 || ac > 162)        /* (the engine's room: 8 and 81 entries of two symbols) */
            return "a Huffman table larger than the engine's";
    }
    if ((u32)(j.end - data) > 0x00C00000u || (data_phys & 0xFF000000u) != ((data_phys + size) & 0xFF000000u))
        return "a file the engine's 16 MB window does not hold";
    j.mcus_x = (j.w + j.mcu_w - 1) / j.mcu_w, j.mcus_y = (j.h + j.mcu_h - 1) / j.mcu_h;
    if (j.mcus_x > 1024 || j.mcus_y > 2048)
        return "wider than the engine's 1024 MCUs";
    /* the pre-scaling: halved while the picture stays at least as large as it is shown (at most 1/8) */
    j.shift = 0;
    while (j.shift < 3 && (j.w >> (j.shift + 1)) >= min_w && (j.h >> (j.shift + 1)) >= min_h)
        j.shift++;
    j.ow = (j.w + (1u << j.shift) - 1) >> j.shift;
    j.oh = (j.h + (1u << j.shift) - 1) >> j.shift;
    j.stride = (((j.mcus_x * j.mcu_w) >> j.shift) + 31) & ~31u;
    j.rows = (((j.mcus_y * j.mcu_h) >> j.shift) + 31) & ~31u;
    *out_w = j.ow, *out_h = j.oh;
    return 0;
}

const char *jpeghw_detail(void)          /* "4:2:0, 1/2" -- after jpeghw_plan */
{
    static const char *const kind[6] = {"4:2:0", "4:1:1", "4:2:2", "4:4:4", "4:4:0", "grey"};
    static char t[32];
    usnprintf(t, sizeof(t), "%s%s%s", kind[j.code], j.shift ? ", 1/" : "",
              j.shift == 1 ? "2" : j.shift == 2 ? "4" : j.shift == 3 ? "8" : "");
    return t;
}

/* one Huffman table into the engine's: its codes' ends at max + (length - 1), the index of each length's last
   symbol at last + (length - 1), the symbols two an entry from sym (entries of them) */
static void huff_load(u32 max, u32 last, u32 sym, u32 entries, const u8 *bits, const u8 *vals)
{
    u32 code = 0, total = 0;
    for (u32 len = 1; len <= 16; len++) {
        u32 n = bits[len];
        total += n;
        code += n;
        u32 m = n ? code - 1 : len == 16 ? 0 : 0xFFFF, l = n ? total - 1 : 0;
        VE(0x7C) = (max + len - 1) << 16 | (m & 0xFFFF);
        VE(0x7C) = (last + len - 1) << 16 | (l & 0xFF);
        code <<= 1;
    }
    for (u32 k = 0; k < entries; k++)
        VE(0x7C) = (sym + k) << 16 | vals[2 * k] | (u32)vals[2 * k + 1] << 8;
}

static void engine_setup(u32 y_phys, u32 c_phys)
{
    u8 r = SYS8(0x80);                  /* the decoder's reset (av.c's ve_reset: dump.bin's) */
    SYS8(0x80) = (u8)(r | 0x02);
    delay_ms(5);
    SYS8(0x80) = (u8)(r & ~0x02);
    SYS8(0x28) = (u8)(SYS8(0x28) | 0x02);
    CONSOLE = 0;
    VE(0x14) = (j.code << 3 | 3u) << 24;
    for (u32 c = 0; c < j.ncomp; c++) {             /* (Cb's table serves Cr: the same, checked) */
        if (c == 2)
            break;
        const u8 *q = j.q[j.tq[c]];
        for (u32 i = 0; i < 64; i++)
            VE(0x80) = (c ? 0u : 1u << 14) | i << 8 | q[i];
    }
    VE(0x10) &= ~0x08020000u;
    EXT(0x00) = y_phys;
    EXT(0x04) = c_phys;
    EXT(0x08) = j.shift | j.shift << 2 | ((j.mcus_x - 1) & 0x3FF) << 4 | j.stride << 16;
    EXT(0x0C) &= 0xF800F800u;
    VE8(0x17) |= 0x40;
    u32 c = j.ncomp == 3 ? 1 : 0;
    huff_load(0x00, 0x40, 0x100, 8, j.bits[0][j.td[0]], j.vals[0][j.td[0]]);
    huff_load(0x10, 0x50, 0x108, 8, j.bits[0][j.td[c]], j.vals[0][j.td[c]]);
    huff_load(0x20, 0x60, 0x110, 81, j.bits[1][j.ta[0]], j.vals[1][j.ta[0]]);
    huff_load(0x30, 0x70, 0x161, 81, j.bits[1][j.ta[c]], j.vals[1][j.ta[c]]);
}

/* YCbCr to RGB (JFIF): what Cr adds to red and green, Cb to green and blue, rounded; a clamp to 0..255 */
static short cr_r[256], cr_g[256], cb_g[256], cb_b[256];
static u8 clip_table[384 + 256 + 384];

static void tables(void)
{
    static int made;
    if (made)
        return;
    for (s32 i = 0; i < 256; i++) {
        s32 c = i - 128;
        cr_r[i] = (short)((91881 * c + 32768) >> 16);
        cr_g[i] = (short)((-46802 * c + 32768) >> 16);
        cb_g[i] = (short)((-22554 * c + 32768) >> 16);
        cb_b[i] = (short)((116130 * c + 32768) >> 16);
    }
    for (s32 i = 0; i < (s32)sizeof(clip_table); i++) {
        s32 v = i - 384;
        clip_table[i] = (u8)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    made = 1;
}

static int wait_clear(u32 bit, u64 since)
{
    while (VE(0x18) & bit)
        if (now_ms() - since > MCU_TIMEOUT_MS)
            return -1;
    return 0;
}

/* the scan decoded into the frames (y / c physical, cache already written back): 0, or why it stopped */
static const char *decode(u32 *mcus_done)
{
    const u8 *pos = j.scan;
    u32 bit = 0, left = j.dri;
    u32 end_phys = j.data_phys + (u32)(j.end - j.data);
    VE(0x30) = (end_phys - 1) & 0xFFFFFFu;
    for (u32 my = 0; my < j.mcus_y; my++) {
        for (u32 mx = 0; mx < j.mcus_x; mx++) {
            if (j.dri && !left) {       /* a restart marker: the bits left in its byte dropped, the marker passed */
                if (bit)
                    pos++, bit = 0;
                while (pos + 1 < j.end && pos[0] == 0xFF && pos[1] == 0xFF)
                    pos++;
                if (pos + 1 < j.end && pos[0] == 0xFF && (pos[1] & 0xF8) == 0xD0)
                    pos += 2;
                VE8(0x17) |= 0x40;
                left = j.dri;
            }
            if (pos >= j.end)
                return "the data ended early";
            u32 phys = j.data_phys + (u32)(pos - j.data), start = phys & ~15u, avail = (u32)(j.end - pos);
            if (avail > 0x100000u)
                avail = 0x100000u;
            u32 first = (phys - start) * 8 + bit;
            EXT(0x0C) = (EXT(0x0C) & 0xF800F800u) | my << 16 | mx;
            VE(0x28) = first & 0xFFFFFFu;
            VE(0x2C) = (avail * 8) & 0xFFFFFFu;
            VE(0x24) = (7u | ((start >> 21) & 0x38u)) << 24 | (start & 0xFFFFFFu);
            VE8(0x14) = 0x0C;
            u64 t = now_ms();
            if (wait_clear(1u << 8, t) < 0)
                return "the engine's VLD did not finish an MCU";
            u32 o = VE(0x28) & 0xFFFFFFu;
            if (o <= first || o - first > avail * 8)
                return "the engine read no bits";       /* (an MCU takes 2 bits a block at least: an engine that
                                                           did not run -- the emulator's registers are plain RAM) */
            pos = j.data + (start - j.data_phys) + (o >> 3);
            bit = o & 7;
            if (!bit && pos > j.scan && pos < j.end && pos[-1] == 0xFF && pos[0] == 0x00)
                pos++;                  /* (the MCU ended after an 0xFF: its stuffed 0x00 is the CPU's to pass) */
            if (wait_clear(1u << 19, t) < 0)
                return "the engine did not finish an MCU";
            u32 e = VE(0xC4) & 0x3F;
            if (e) {
                static char t2[48];
                usnprintf(t2, sizeof(t2), "the engine's error 0x%02x at MCU %u, %u", e, mx, my);
                return t2;
            }
            if (j.dri)
                left--;
            ++*mcus_done;
        }
        if (image_poll && image_poll())
            return IMAGE_STOPPED;
    }
    return 0;
}

/* the planned picture decoded (frames in RAM at work_phys, work_size bytes) and given as rows of red, green
   and blue, ow x oh: 0, or why not (no row given then) */
const char *jpeghw_run(u32 work_phys, u32 work_size, void (*row)(void *ctx, u32 y, const u8 *rgb), void *ctx,
                       u32 *decode_ms)
{
    u32 y_size = j.stride * j.rows, c_size = y_size;             /* (chroma: room for 4:4:4's, whatever it is) */
    u32 y_phys = (work_phys + 0xFFFu) & ~0xFFFu, c_phys = y_phys + y_size;
    u32 rgb_phys = c_phys + c_size;
    if (rgb_phys + j.ow * 3 - work_phys > work_size)
        return "no room for its frames";
    av_cache_flush(j.data, (u32)(j.end - j.data));
    av_cache_flush((const void *)KSEG0(y_phys), y_size + c_size);
    if (j.code == 5) {                                          /* grey: the chroma plain */
        volatile u32 *c = (volatile u32 *)KSEG1(c_phys);
        for (u32 i = 0; i < c_size / 4; i++)
            c[i] = 0x80808080u;
    }
    u64 t0 = now_ms();
    engine_setup(y_phys, c_phys);
    u32 done = 0;
    const char *why = decode(&done);
    *decode_ms = (u32)(now_ms() - t0);
    if (why)
        return why;
    /* the frames' tiles into rows of RGB (JFIF's full-range YCbCr): luma 32 x 16 tiles; chroma Cb / Cr pairs at
       half the width, 16 pairs x 16 lines a tile -- a line for every two luma lines from a 4:2:0 file, for every
       one from the others (the engine makes 4:4:4 4:2:2) */
    tables();
    const u8 *Y = (const u8 *)KSEG0(y_phys), *C = (const u8 *)KSEG0(c_phys);
    u8 *rgb = (u8 *)KSEG0(rgb_phys);
    u32 tiles = j.stride / 32;
    const u8 *clip = clip_table + 384;
    for (u32 y = 0; y < j.oh; y++) {
        const u8 *yl = Y + (y >> 4) * tiles * 512 + (y & 15) * 32;
        const u8 *cl = j.code ? C + (y >> 4) * tiles * 512 + (y & 15) * 32
                              : C + (y >> 5) * tiles * 512 + ((y & 0x1E) << 4);
        u8 *o = rgb;
        for (u32 x = 0; x < j.ow; x += 32, yl += 512, cl += 512) {
            u32 n = j.ow - x < 32 ? j.ow - x : 32;
            for (u32 k = 0; k < n; k += 2, o += 6) {
                s32 r = cr_r[cl[k + 1]], g = cb_g[cl[k]] + cr_g[cl[k + 1]], b = cb_b[cl[k]];
                s32 l = yl[k];
                o[0] = clip[l + r], o[1] = clip[l + g], o[2] = clip[l + b];
                if (k + 1 < n) {
                    l = yl[k + 1];
                    o[3] = clip[l + r], o[4] = clip[l + g], o[5] = clip[l + b];
                }
            }
            if (n & 1)
                o -= 3;
        }
        row(ctx, y, rgb);
    }
    return 0;
}
