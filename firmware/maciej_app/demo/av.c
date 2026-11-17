/* av.c: video files played on the S3811's MPEG-2 decoder and shown on the display engine's video plane.

   Where it comes from: ALi's SDK library libcore_hd.a (refs/MainCode/ddk) has the decoder's driver with
   symbols and DWARF (vdec_s3601*.o: the "DECV_S3601" device; vdec_s3601_hal.o has one accessor per register
   field) and the display's (vp_hal_n.o: "hal_sdhd_*" for the main picture layer).  dump.bin, the Comsat's
   original firmware, carries a newer build of the same decoder driver: its HAL matches the library's
   (0x80332B00..0x80334A00 in its RAM image), its attach (0x80328A00) picks, for the S3811 (chip 0x10048),
   register base 0xB8004200, hardware version 1 (the "console" at 0xB8004000, the down-scaled view at
   0xB8004400, the motion-adaptive filter at 0xB8004300), interrupt 9 (line 1), and its per-picture set-up
   (0x803247BC) follows the library's vdec_config_hw_new_picture on the S3811's branches.  Its memory map
   (config at 0x80A7ADD8): frames 0x06C6C200 (17.6 MB), filter 0x07D3B400, VBV 0x07ED4000..0x07FFFEFF.

   H.264 video goes to the block's other engine (vdec_avc*.o): avc.c, given the NAL units from the same stream
   buffer; its frames come back in display order and are shown here through the same layer.

   How the decoder works (MPEG-1 / MPEG-2 video, the main profile):
   the CPU parses the sequence / picture headers and their extensions itself, the hardware decodes the
   slices: it reads the stream from a buffer ("VBV") from a given bit offset for a given number of bits,
   decodes slice after slice (their start codes it finds itself) and stops at the first start code that is
   not a slice's, then raises "finished".  The picture goes into one of five frame buffers, the references
   are two others (indices given per picture).

   Decoder registers (base 0xB8004200, 32-bit):
     +0x00  the picture's coding parameters (vdec_hw_headerinfo): b0 full_pel_backward, b1 full_pel_forward,
            b2 alternate_scan, b3 intra_vlc_format, b4 q_scale_type, b5 concealment_motion_vectors,
            b6 frame_pred_frame_dct, b7 top_field_first, b8-9 picture_structure, b10-11 intra_dc_precision,
            b12-15 f_code[1][1], b16-19 f_code[1][0], b20-23 f_code[0][1], b24-27 f_code[0][0],
            b28-30 picture_coding_type
     +0x08  macroblocks: width << 8 | height (a field picture: the field's), at most 120 x 72
     +0x0C  the macroblock reached (x << 8 | y); +0x20 b0-12 macroblocks decoded
     +0x10  modes: b3-6 interrupt enables (b3 finished), b8 simplified B, b9 compressed frames (ADPCM),
            b10 down-scaled view, b11 3:2 pull-down detection, b12-13 ADPCM read/write, b14 "not write
            reconstruction", b15 motion-adaptive filter, b16 error concealment, b17 / b27 VLD-IQ-IS modes,
            b18 deblocking, b19 deblocking writes, b29 "quick mode" (dump.bin sets it on the S3811)
     +0x14  b0-3 start (0xF: go), b24-26 MPEG-1 (1) / MPEG-2 (2), b27-28 chroma format
     +0x18  status: b0 finished, b1 with errors, b2 the slices read (b0 follows); written back to clear;
            b8-13, b16, b18, b20 busy
     +0x24  the VBV's start (bits 0-23, and its address bits 24-26 at bits 27-29), b24-26 control (5: a new
            start position; 7 with packets) ; +0x30 its end (inclusive, bits 0-23)
     +0x28  where to read: bit offset from the start (it holds where the decoder stopped afterwards)
     +0x2C  the bits there are to read
     +0x40 + 4 n  frame buffer n: (C >> 9) << 16 | (Y >> 9), offsets in the 32 MB window
     +0x78  (byte) the window: address bits 25..27
     +0x80  the quantiser matrices, one entry a write: b14 intra, b8-13 index (zigzag order), b0-7 value
     +0xC0  b14-16 the frame being decoded, b11-13 the forward reference, b8-10 the backward reference,
            b24 "the display shows the frame being decoded"
     +0xC4  error flags
   Outside: 0xB8000080 b1 the decoder's reset (pulsed 5 ms), 0xB8000028 b1 set before it runs.

   A frame buffer: luma in tiles of 32 x 16 pixels (512 bytes, row after row of tiles, (width + 31) / 32
   tiles a row), chroma in tiles of 16 Cb/Cr pairs x 16 lines (the same 512 bytes) -- the library's
   de_get_1pixel.  Sizes as vdec_frm_malloc: luma align32(w) * align16(h), chroma align32(w) * align32(h) / 2.

   The display engine's main picture layer (base 0xB8006000; vp_hal_n.o, with the S3811's reset values):
     +0x160 the motion-adaptive filter's buffer, +0x164 / +0x168 the top / bottom field's luma, +0x16C /
     +0x170 their chroma (physical addresses), +0x174 the offset in the source, +0x178 the source's layout
     (reset 0x5A17242D for 720 x 576: b16-23 the tiles a row), +0x17C its size (h << 16 | w), +0x180 /
     +0x184 where on the screen (x0 << 16 | x1, y0 << 16 | y1, inclusive), +0x188 the background (YCbCr),
     +0x18C error status, +0x190 control (b21 the layer on, b1 H.264 tiling, b4 de-interlacer, b17-18
     fetch mode: 0 one field (every other line: a frame then fills half the height and the engine reads
     on past it), 1 the frame's lines in turn; b16 the scaler bypassed, b6-7 two vertical taps instead of
     four).  The chip comes out of reset set up for a 720 x 576 source at 0x00200000 shown 1:1 at the top
     left.
   Its scaler (dump.bin 0x802C3D4C, the S3811's vp_sub_sdhd_scale_simple, and 0x802C2ACC / 0x802C2548):
     +0x100 / +0x108 where in the first pixel the luma / chroma starts, +0x110 / +0x118 their horizontal
     steps (source / screen, 16.16), +0x114 / +0x11C the vertical steps (4.12), +0x104 / +0x10C the line the
     top field's luma / chroma starts at (4.12, signed), +0x1C8 / +0x1CC the bottom field's, +0x134 / +0x138
     the last column on the screen << 16, +0x13C / +0x140 0, +0x144 / +0x148 1.
     +0x1E4 the address of a parameter block in RAM, which the engine reads by itself: the filters as 16-bit
     signed coefficients (a row of taps for each of 32 phases, a row's sum 256): +0x000 vertical luma (4
     taps), +0x100 vertical chroma (4), +0x200 horizontal luma (8), +0x400 horizontal chroma (4), +0x500..
     +0x55F other tables (dump.bin: zeros and 20 bytes at +0x540).  dump.bin computes the filters for each
     source / screen ratio (0x802D3B5C: one set for every enlargement, softer ones the more it shrinks).
     With the pointer left at dump.bin's address (0x01465000: this demo's video set-up copies its values)
     the scaler filtered with whatever lay there: a broken picture, also at 1:1.

   Sound: MPEG audio Layer II decoded in software (mp2.c; dump.bin does that too: its DECA is ALi's libmpg on
   the CPU), mixed to mono, through snd.c at 48 kHz.  AC-3 and AAC sound are told, not played. */
#include "hw.h"
#include "av.h"
#include "avc.h"
#include "mp2.h"
#include "snd.h"

#define VE_BASE         0xB8004200u
#define VE(o)           REG32(VE_BASE + (o))
#define VE8(o)          REG8(VE_BASE + (o))
#define VE_CONSOLE      0xB8004000u
#define SYS8(o)         REG8(0xB8000000u + (o))
#define SYS32(o)        REG32(0xB8000000u + (o))
#define DE(o)           REG32(0xB8006000u + (o))
#define FRAMES          5

/* ---- small helpers ---- */
static void copy(u8 *to, const u8 *from, u32 n)
{
    while (n && ((u32)to & 3))
        *to++ = *from++, n--;
    if (!((u32)from & 3))
        for (; n >= 4; n -= 4, to += 4, from += 4)
            *(u32 *)to = *(const u32 *)from;
    while (n--)
        *to++ = *from++;
}

/* the data cache written back (and its lines dropped) over [p, p + n): what the CPU wrote reaches RAM before
   a DMA engine reads it (avc.c's too) */
void av_cache_flush(const void *p, u32 n)
{
    u32 a = (u32)p & ~15u, end = (u32)p + n;
    for (; a < end; a += 16)
        __asm__ volatile("cache 0x15, 0(%0)" : : "r"(a));
    __asm__ volatile("sync");
}

static int ends_with(const char *s, const char *tail)
{
    int n = 0, m = 0;
    while (s[n])
        n++;
    while (tail[m])
        m++;
    if (m > n)
        return 0;
    for (int i = 0; i < m; i++) {
        char a = s[n - m + i], b = tail[i];
        if (a >= 'A' && a <= 'Z')
            a = (char)(a + 32);
        if (a != b)
            return 0;
    }
    return 1;
}

int av_is_video(const char *name)
{
    return ends_with(name, ".ts") || ends_with(name, ".m2t") || ends_with(name, ".mpg") ||
           ends_with(name, ".mpeg") || ends_with(name, ".m2v") || ends_with(name, ".vob") ||
           ends_with(name, ".trp") || ends_with(name, ".264") || ends_with(name, ".h264");
}

/* ---- FIFOs of elementary stream bytes (linear: the rest moved down when the end is reached) ---- */
struct fifo {
    u8 *buf;
    u32 size, rd, wr;
};
static struct fifo ves, aes;

static u32 fifo_used(const struct fifo *f)
{
    return f->wr - f->rd;
}

/* room for n bytes at the end, what was read moved down first: the bytes moved by (0 none), -1 no room */
static s32 fifo_room(struct fifo *f, u32 n)
{
    if (f->wr + n <= f->size)
        return 0;
    u32 from = f->rd;
    if (f->wr - from + n > f->size)
        return -1;
    copy(f->buf, f->buf + from, f->wr - from);
    f->rd -= from;
    f->wr -= from;
    return (s32)from;
}

/* ---- the video elementary stream's parser ---- */
struct seq {
    u16 w, h;                           /* luma pixels */
    u8 aspect, rate_code, mpeg2, progressive, chroma;
    u8 intra[64], nonintra[64];         /* the quantiser matrices, zigzag order (as in the stream) */
    u32 bitrate;
};
struct pic {
    u8 type;                            /* 1 I, 2 P, 3 B, 4 D */
    u16 temporal;
    u8 full_pel[2], fcode[2][2];
    u8 dc_precision, structure, top_first, frame_pred, concealment, q_scale_type, intra_vlc, alt_scan,
       repeat_first, progressive_frame;
};
static struct seq seq;
static struct pic pic;
static int have_seq, have_pic, in_slices, eos;
static u32 scan_at, slices_at;          /* (offsets in ves.buf) */
static u32 seq_changed;                 /* the sequence header's size / matrices changed: frames set up again */

static const u8 default_intra[64] = {  /* zigzag order */
    8, 16, 16, 19, 16, 19, 22, 22, 22, 22, 22, 22, 26, 24, 26, 27, 27, 27, 26, 26, 26, 26, 27, 27, 27, 29, 29,
    29, 34, 34, 34, 29, 29, 29, 27, 27, 29, 29, 32, 32, 34, 34, 37, 38, 37, 35, 35, 34, 35, 38, 38, 40, 40,
    40, 48, 48, 46, 46, 56, 56, 58, 69, 69, 83,
};

struct reader {
    const u8 *p;
    u32 at;                             /* bits */
};

static u32 bits(struct reader *r, int n)
{
    u32 v = 0;
    while (n-- > 0) {
        v = v << 1 | ((r->p[r->at >> 3] >> (7 - (r->at & 7))) & 1);
        r->at++;
    }
    return v;
}

static void parse_sequence(const u8 *p)
{
    struct reader r = {p, 0};
    u16 w = (u16)bits(&r, 12), h = (u16)bits(&r, 12);
    seq.aspect = (u8)bits(&r, 4);
    seq.rate_code = (u8)bits(&r, 4);
    seq.bitrate = bits(&r, 18) * 400;
    (void)bits(&r, 1 + 10 + 1);         /* marker, vbv_buffer_size, constrained */
    if (bits(&r, 1))
        for (int i = 0; i < 64; i++)
            seq.intra[i] = (u8)bits(&r, 8);
    else
        for (int i = 0; i < 64; i++)
            seq.intra[i] = default_intra[i];
    if (bits(&r, 1))
        for (int i = 0; i < 64; i++)
            seq.nonintra[i] = (u8)bits(&r, 8);
    else
        for (int i = 0; i < 64; i++)
            seq.nonintra[i] = 16;
    if (!have_seq || w != seq.w || h != seq.h)
        seq_changed = 1;
    seq.w = w;
    seq.h = h;
    seq.mpeg2 = 0;                      /* (until a sequence extension says so) */
    seq.progressive = 1;
    seq.chroma = 1;
    have_seq = 1;
}

static void parse_extension(const u8 *p)
{
    struct reader r = {p, 0};
    u32 id = bits(&r, 4);
    if (id == 1 && have_seq) {          /* sequence extension */
        (void)bits(&r, 8);              /* profile and level */
        seq.progressive = (u8)bits(&r, 1);
        seq.chroma = (u8)bits(&r, 2);
        u32 hx = bits(&r, 2), vx = bits(&r, 2);
        if (hx || vx) {
            seq.w = (u16)(seq.w | hx << 12);
            seq.h = (u16)(seq.h | vx << 12);
            seq_changed = 1;
        }
        seq.mpeg2 = 1;
    } else if (id == 3 && have_seq) {   /* quant matrix extension */
        if (bits(&r, 1))
            for (int i = 0; i < 64; i++)
                seq.intra[i] = (u8)bits(&r, 8);
        if (bits(&r, 1))
            for (int i = 0; i < 64; i++)
                seq.nonintra[i] = (u8)bits(&r, 8);
    } else if (id == 8 && have_pic) {   /* picture coding extension */
        pic.fcode[0][0] = (u8)bits(&r, 4);
        pic.fcode[0][1] = (u8)bits(&r, 4);
        pic.fcode[1][0] = (u8)bits(&r, 4);
        pic.fcode[1][1] = (u8)bits(&r, 4);
        pic.dc_precision = (u8)bits(&r, 2);
        pic.structure = (u8)bits(&r, 2);
        pic.top_first = (u8)bits(&r, 1);
        pic.frame_pred = (u8)bits(&r, 1);
        pic.concealment = (u8)bits(&r, 1);
        pic.q_scale_type = (u8)bits(&r, 1);
        pic.intra_vlc = (u8)bits(&r, 1);
        pic.alt_scan = (u8)bits(&r, 1);
        pic.repeat_first = (u8)bits(&r, 1);
        (void)bits(&r, 1);              /* chroma_420_type */
        pic.progressive_frame = (u8)bits(&r, 1);
    }
}

static void parse_picture(const u8 *p)
{
    struct reader r = {p, 0};
    pic.temporal = (u16)bits(&r, 10);
    pic.type = (u8)bits(&r, 3);
    (void)bits(&r, 16);                 /* vbv_delay */
    pic.full_pel[0] = pic.full_pel[1] = 0;
    pic.fcode[0][0] = pic.fcode[0][1] = pic.fcode[1][0] = pic.fcode[1][1] = 15;
    if (pic.type == 2 || pic.type == 3) {
        pic.full_pel[0] = (u8)bits(&r, 1);
        pic.fcode[0][0] = pic.fcode[0][1] = (u8)bits(&r, 3);
    }
    if (pic.type == 3) {
        pic.full_pel[1] = (u8)bits(&r, 1);
        pic.fcode[1][0] = pic.fcode[1][1] = (u8)bits(&r, 3);
    }
    /* MPEG-1's (and the defaults before a picture coding extension) */
    pic.dc_precision = 0;
    pic.structure = 3;
    pic.top_first = 0;
    pic.frame_pred = 1;
    pic.concealment = pic.q_scale_type = pic.intra_vlc = pic.alt_scan = pic.repeat_first = 0;
    pic.progressive_frame = 1;
    have_pic = 1;
}

/* the next start code (00 00 01 xx) at or after `from` in ves, or ~0 */
static u32 find_code(u32 from)
{
    const u8 *b = ves.buf;
    u32 end = ves.wr;
    for (u32 i = from; i + 3 < end; ) {
        if (b[i + 2] > 1)
            i += 3;
        else if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1)
            return i;
        else
            i++;
    }
    return ~0u;
}

/* a whole picture found: its slices are ves.buf[slices_at .. *end): 1; 0: more data needed (or the end) */
static int next_picture(u32 *end)
{
    for (;;) {
        u32 at = find_code(scan_at);
        if (at == ~0u) {
            if (eos && in_slices) {     /* the stream's last picture */
                *end = ves.wr;
                in_slices = 0;
                scan_at = ves.rd = ves.wr;
                return 1;
            }
            scan_at = ves.wr > ves.rd + 3 ? ves.wr - 3 : ves.rd;
            if (!in_slices)
                ves.rd = scan_at;
            return 0;
        }
        u8 code = ves.buf[at + 3];
        if (in_slices && (code == 0 || code > 0xAF)) {
            *end = at;
            in_slices = 0;
            scan_at = ves.rd = at;
            return 1;
        }
        if (code >= 1 && code <= 0xAF) {
            if (!in_slices && have_pic && have_seq) {
                in_slices = 1;
                slices_at = at;
                ves.rd = at;
            }
            scan_at = at + 4;
            continue;
        }
        if (ves.wr - at < 4 + 140 && !eos) {      /* a header: all of it here first (a sequence header with
                                                   both matrices is 140 bytes) */
            scan_at = ves.rd = at;
            return 0;
        }
        const u8 *p = ves.buf + at + 4;
        if (code == 0xB3)
            parse_sequence(p);
        else if (code == 0xB5)
            parse_extension(p);
        else if (code == 0x00)
            parse_picture(p);
        scan_at = at + 4;
        ves.rd = scan_at;
    }
}

/* ---- frame buffers ---- */
struct frame {
    u32 y, c;                           /* offsets in the decoder's window */
    u8 type;
    u32 number;                         /* decoded as the n-th picture */
};
static struct frame frm[FRAMES];
static u32 y_size, c_size;
static int nfrm;
static int older = -1, newer = -1;      /* the references: the one before, the latest I / P */
#define QUEUE_MAX 32                    /* (avc.c may let out all its frames at once: at an IDR, at the end) */
static int queue[QUEUE_MAX], queued;    /* waiting to be shown, in display order */
static int shown = -1, shown_before = -1;
static int h264;                        /* the video is H.264: the frames are avc.c's (else frm[]'s) */

static int frames_setup(void)
{
    y_size = ((seq.w + 31u) & ~31u) * ((seq.h + 15u) & ~15u);
    c_size = ((seq.w + 31u) & ~31u) * (((seq.h + 31u) & ~31u) / 2);
    u32 each = y_size + c_size;
    if (seq.w < 16 || seq.h < 16 || seq.w > 1920 || seq.h > 1152 || AV_FRAMES_PHYS + FRAMES * each > AV_FRAMES_END)
        return -1;                      /* (under a macroblock: a damaged header -- the scaler divides by them) */
    for (int i = 0; i < FRAMES; i++) {
        frm[i].y = AV_FRAMES_PHYS - AV_WINDOW_PHYS + (u32)i * each;
        frm[i].c = frm[i].y + y_size;
        /* black (Y 16, Cb Cr 128): what a lost picture shows */
        volatile u32 *p = (volatile u32 *)KSEG1(AV_WINDOW_PHYS + frm[i].y);
        for (u32 k = 0; k < y_size / 4; k++)
            p[k] = 0x10101010u;
        p = (volatile u32 *)KSEG1(AV_WINDOW_PHYS + frm[i].c);
        for (u32 k = 0; k < c_size / 4; k++)
            p[k] = 0x80808080u;
    }
    nfrm = FRAMES;
    older = newer = shown = shown_before = -1;
    queued = 0;
    return 0;
}

static int frame_free(int i)
{
    if (i == older || i == newer || i == shown || i == shown_before)
        return 0;
    for (int k = 0; k < queued; k++)
        if (queue[k] == i)
            return 0;
    return 1;
}

static int free_frame(void)
{
    for (int i = 0; i < nfrm; i++)
        if (frame_free(i))
            return i;
    return -1;
}

/* ---- the decoder ---- */
static struct {
    u32 pictures, types[5], skipped, errors, timeouts, slowest_us, total_us, last_status, last_mbs, last_err,
        last_pos, last_offset, last_bits, shown, late, audio_frames, audio_bad, audio_samples, audio_dropped,
        bytes_read, packets, cc_errors;
} st;
static int ve_ready;

static void ve_reset(void)
{
    /* dump.bin's vdec_reset_ve_hw (0x80323418, the S3811's branch) */
    u8 r = SYS8(0x80);
    SYS8(0x80) = (u8)(r | 0x02);
    delay_ms(5);
    SYS8(0x80) = (u8)(r & ~0x02);
    SYS8(0x28) = (u8)(SYS8(0x28) | 0x02);
    REG32(VE_CONSOLE) = 0;              /* vdec_hw_enable (hardware version 1) */
    /* vdec_hal_init_register as dump.bin's (0x80332CF8) */
    VE(0x10) |= 0x20000000u;            /* quick mode (dump.bin: on for the S3811) */
    VE(0x10) &= ~0x08020000u;           /* VLD / IQ / IS modes 0 */
    VE(0x10) &= ~0x00000800u;           /* pull-down detection off */
    VE(0x10) &= ~0x00004000u;           /* the reconstruction written */
    VE(0x10) &= ~0x00003000u;           /* ADPCM read / write 0 */
    VE(0x10) = (VE(0x10) & ~0x7Fu) | 0x08u;     /* interrupt: finished */
    VE(0x14) &= 0xE7FFFFF0u;            /* 4:2:0 */
    VE(0x14) = (VE(0x14) & 0xF8FFFFF0u) | (2u << 24);   /* MPEG-2 */
    SYS32(0x28) |= 0x02u;               /* vdec_s3601_open */
    ve_ready = 1;
}

/* one picture's slices (n bytes at p) decoded into frame `recon` with references fwd / bwd: 0 finished, -1
   timed out */
static int ve_decode(const u8 *p, u32 n, int recon, int fwd, int bwd)
{
    if (!ve_ready)
        ve_reset();
    /* the input: the slices, a sequence end code after them (where the decoder stops), whole words */
    u8 *vbv = (u8 *)KSEG0(AV_VBV_PHYS);
    if (n > AV_VBV_SIZE - 4096)
        n = AV_VBV_SIZE - 4096;
    copy(vbv, p, n);
    /* after them a sequence end code and 2 KB of zeros, all of it given to the decoder as data: it reads
       ahead of the macroblock it decodes, and with nothing more to read it never finishes the last one
       (seen on the box: 1619 of 1620 macroblocks, no "finished") -- dump.bin's driver always gives it all
       the stream it has, the next pictures' too */
    static const u8 tail[4] = {0, 0, 1, 0xB7};
    copy(vbv + n, tail, sizeof(tail));
    for (u32 i = n + 4; i < n + 4 + 2048 + 4; i++)
        vbv[i] = 0;
    u32 len = (n + 4 + 2048 + 3) & ~3u;
    av_cache_flush(vbv, len + 4);
    /* vdec_config_hw_new_picture (dump.bin 0x803247BC) */
    VE8(0x78) = (u8)(AV_WINDOW_PHYS >> 25);
    for (int i = 0; i < nfrm; i++)
        VE(0x40 + 4 * i) = ((frm[i].c >> 9) & 0xFFFFu) << 16 | ((frm[i].y >> 9) & 0xFFFFu);
    u32 start = KSEG0(AV_VBV_PHYS), end = start + AV_VBV_SIZE - 1;
    VE(0x28) = 0;                                               /* from its first bit */
    VE(0x30) = (VE(0x30) & 0xFF000000u) | (end & 0xFFFFFFu);
    VE(0x2C) = (len * 8) & 0x03FFFFFFu;
    VE(0x24) = (VE(0x24) & 0xC0000000u) | (start & 0xFFFFFFu) | ((start & 0x07000000u) << 3) | (5u << 24);
    VE(0xC0) = (VE(0xC0) & 0xFFFE00FFu) | (u32)recon << 14 | (u32)(fwd < 0 ? 0 : fwd) << 11 |
               (u32)(bwd < 0 ? 0 : bwd) << 8;
    VE(0x10) &= ~(0x00004000u | 0x00040000u | 0x00080000u | 0x00000400u | 0x00000200u | 0x00008000u |
                  0x00000100u | 0x00010000u | 0x00000800u);
    /* (the reconstruction written; deblocking off and its writes (dump.bin: not_write_dblk_r(1) clears b19);
       no down-scaled view, ADPCM, motion-adaptive filter, simplified B, error concealment, pull-down) */
    VE(0xC0) &= ~0x01000000u;           /* the display does not show this frame */
    VE(0x00) = (u32)pic.full_pel[1] | (u32)pic.full_pel[0] << 1 | (u32)pic.alt_scan << 2 |
               (u32)pic.intra_vlc << 3 | (u32)pic.q_scale_type << 4 | (u32)pic.concealment << 5 |
               (u32)pic.frame_pred << 6 | (u32)pic.top_first << 7 | (u32)pic.structure << 8 |
               (u32)pic.dc_precision << 10 | (u32)pic.fcode[1][1] << 12 | (u32)pic.fcode[1][0] << 16 |
               (u32)pic.fcode[0][1] << 20 | (u32)pic.fcode[0][0] << 24 | (u32)(pic.type & 7) << 28;
    u32 mbw = (seq.w + 15u) >> 4, lines = pic.structure == 3 ? seq.h : seq.h / 2u, mbh = (lines + 15u) >> 4;
    VE(0x08) = mbw << 8 | mbh;
    for (u32 i = 0; i < 64; i++)
        VE(0x80) = 0x4000u | i << 8 | seq.intra[i];
    for (u32 i = 0; i < 64; i++)
        VE(0x80) = i << 8 | seq.nonintra[i];
    VE(0x14) = (VE(0x14) & 0xF8FFFFF0u) | (seq.mpeg2 ? 2u : 1u) << 24;
    VE(0x20) = 0;
    VE(0xC4) = 0;
    VE(0x18) = 0xFFFFFFFFu;
    VE(0x0C) = 0;
    for (int i = 0; i < 100 && (VE(0x18) & 0x00153F00u); i++)
        ;
    u64 t0 = now_ticks();
    VE(0x14) = (VE(0x14) & ~0xFu) | 0xFu;                       /* go */
    u32 limit = ticks_per_ms() * 300, s;
    /* b0 "finished" (dump.bin's ISR 0x80326570 finishes a picture on b0 alone, b1 with it: errors in +0xC4);
       b2 comes first, when the slices have been read (the last macroblocks are still being written) */
    while (!((s = VE(0x18)) & 0x01u)) {
        if ((u32)(now_ticks() - t0) > limit) {
            st.timeouts++;
            st.last_status = s;
            st.last_mbs = VE(0x20) & 0x1FFFu;
            st.last_pos = VE(0x0C);
            st.last_offset = VE(0x28) & 0x07FFFFFFu;
            st.last_err = VE(0xC4);
            ve_reset();                 /* (stuck: started again from a reset) */
            return -1;
        }
    }
    u32 us = (u32)(now_ticks() - t0) / (ticks_per_ms() / 1000u);
    VE(0x18) = s;                       /* (the events cleared: written back) */
    st.last_status = s;
    st.last_mbs = VE(0x20) & 0x1FFFu;
    st.last_pos = VE(0x0C);
    st.last_offset = VE(0x28) & 0x07FFFFFFu;
    st.last_bits = len * 8;
    st.last_err = VE(0xC4);
    if (st.last_err)
        st.errors++;
    st.total_us += us;
    if (us > st.slowest_us)
        st.slowest_us = us;
    return 0;
}

/* ---- the display engine's main picture layer ---- */
static u32 de_saved[0x30];              /* 0x160..0x19C as they were, restored at the end */
static int de_on, osd_was_on;

static void de_window(int on)
{
    DE(0x190) = on ? DE(0x190) | 0x00200000u : DE(0x190) & ~0x00200000u;
}

/* the scaler's steps (vp_sub_sdhd_scale_simple in the SDK's vpo_m36f_lld.o): source / screen as 16.16
   horizontally, 4.12 vertically, chroma (4:2:0) at half the luma's, each packed as the hardware takes it.
   For a 720 x 576 picture shown 1:1 these give exactly the S3811's reset values (0x6110 = 1, 0x6114 = 1,
   0x6118 = 0x10000000, 0x611C = 0x10000000): its scaler is the same */
static u32 step_h(u32 src, u32 dst)
{
    u32 v = (src << 16) / dst;
    return ((v >> 11) & 0x1Fu) << 24 | (v & 0x7FFu) << 8 | ((v >> 16) & 0x1Fu);
}

static u32 step_v(u32 src, u32 dst)
{
    u32 v = (src << 12) / dst;
    return ((v >> 7) & 0x1Fu) << 24 | (v & 0x7Fu) << 8 | ((v >> 12) & 0x3Fu);
}

static u32 phase_v(s32 v)               /* a vertical start: 4.12, signed */
{
    u32 u = (u32)v;
    return ((u >> 7) & 0x1Fu) << 24 | (u & 0x7Fu) << 8 | ((u >> 12) & 0x1Fu) | (u & 0x80000000u) >> 26;
}

/* the scaler's filters: what dump.bin's own generator (0x802D3B5C, run on the PC) gives for these source :
   screen ratios, 32 phases each; the one row of it whose sum was 257 has its largest tap one less */
struct scale_filter {
    u32 ratio;                          /* source / screen, 16.16 */
    short h8[32][8];                    /* 8 taps: horizontal luma */
    short v4[32][4];                    /* 4 taps: vertical luma and chroma, horizontal chroma */
};

static const struct scale_filter filters[] = {
    {0x10000,                           /* 720 : 720 */
        {
         {0, 0, 0, 256, 0, 0, 0, 0}, {-1, 2, -7, 256, 8, -3, 1, 0},
         {-2, 5, -13, 255, 15, -6, 2, 0}, {-2, 7, -19, 251, 24, -8, 3, 0},
         {-3, 9, -24, 248, 33, -11, 4, 0}, {-3, 10, -29, 247, 42, -15, 5, -1},
         {-4, 12, -33, 241, 52, -17, 6, -1}, {-4, 13, -36, 236, 62, -21, 7, -1},
         {-4, 14, -39, 229, 73, -24, 8, -1}, {-4, 15, -41, 222, 83, -27, 9, -1},
         {-4, 16, -43, 215, 94, -30, 10, -2}, {-4, 16, -44, 206, 105, -32, 11, -2},
         {-4, 16, -45, 198, 116, -35, 12, -2}, {-4, 16, -45, 189, 127, -37, 13, -3},
         {-4, 16, -45, 179, 138, -39, 14, -3}, {-4, 16, -44, 169, 148, -41, 15, -3},
         {-3, 15, -43, 159, 159, -43, 15, -3}, {-3, 15, -41, 148, 169, -44, 16, -4},
         {-3, 14, -39, 138, 179, -45, 16, -4}, {-3, 13, -37, 127, 189, -45, 16, -4},
         {-2, 12, -35, 116, 198, -45, 16, -4}, {-2, 11, -32, 105, 206, -44, 16, -4},
         {-2, 10, -30, 94, 215, -43, 16, -4}, {-1, 9, -27, 83, 222, -41, 15, -4},
         {-1, 8, -24, 73, 229, -39, 14, -4}, {-1, 7, -21, 62, 236, -36, 13, -4},
         {-1, 6, -17, 52, 241, -33, 12, -4}, {-1, 5, -15, 42, 247, -29, 10, -3},
         {0, 4, -11, 33, 248, -24, 9, -3}, {0, 3, -8, 24, 251, -19, 7, -2},
         {0, 2, -6, 15, 255, -13, 5, -2}, {0, 1, -3, 8, 256, -7, 2, -1}},
        {
         {0, 256, 0, 0}, {-5, 256, 5, 0}, {-9, 254, 11, 0}, {-13, 252, 18, -1},
         {-15, 247, 25, -1}, {-18, 243, 33, -2}, {-20, 238, 41, -3}, {-21, 231, 50, -4},
         {-22, 223, 60, -5}, {-22, 215, 69, -6}, {-22, 206, 79, -7}, {-21, 196, 90, -9},
         {-21, 187, 100, -10}, {-20, 177, 111, -12}, {-19, 166, 122, -13}, {-17, 155, 133, -15},
         {-16, 144, 144, -16}, {-15, 133, 155, -17}, {-13, 122, 166, -19}, {-12, 111, 177, -20},
         {-10, 100, 187, -21}, {-9, 90, 196, -21}, {-7, 79, 206, -22}, {-6, 69, 215, -22},
         {-5, 60, 223, -22}, {-4, 50, 231, -21}, {-3, 41, 238, -20}, {-2, 33, 243, -18},
         {-1, 25, 247, -15}, {-1, 18, 252, -13}, {0, 11, 254, -9}, {0, 5, 256, -5}}},
    {0x12000,                           /* 1440 : 1280 */
        {
         {7, -18, 27, 224, 27, -18, 7, 0}, {7, -16, 20, 223, 34, -20, 8, 0},
         {6, -14, 14, 223, 41, -22, 8, 0}, {5, -11, 8, 220, 49, -24, 9, 0},
         {5, -9, 2, 219, 57, -26, 9, -1}, {4, -7, -3, 216, 65, -28, 10, -1},
         {3, -5, -8, 214, 73, -30, 10, -1}, {3, -3, -13, 210, 81, -32, 11, -1},
         {2, -2, -17, 206, 90, -33, 11, -1}, {2, 0, -21, 201, 98, -34, 11, -1},
         {1, 2, -24, 195, 107, -35, 11, -1}, {1, 3, -27, 190, 115, -36, 11, -1},
         {0, 5, -30, 184, 124, -37, 11, -1}, {0, 6, -32, 177, 132, -37, 11, -1},
         {0, 7, -34, 171, 140, -37, 10, -1}, {-1, 8, -35, 164, 148, -37, 10, -1},
         {-1, 9, -36, 156, 156, -36, 9, -1}, {-1, 10, -37, 148, 164, -35, 8, -1},
         {-1, 10, -37, 140, 171, -34, 7, 0}, {-1, 11, -37, 132, 177, -32, 6, 0},
         {-1, 11, -37, 124, 184, -30, 5, 0}, {-1, 11, -36, 115, 190, -27, 3, 1},
         {-1, 11, -35, 107, 195, -24, 2, 1}, {-1, 11, -34, 98, 201, -21, 0, 2},
         {-1, 11, -33, 90, 206, -17, -2, 2}, {-1, 11, -32, 81, 210, -13, -3, 3},
         {-1, 10, -30, 73, 214, -8, -5, 3}, {-1, 10, -28, 65, 216, -3, -7, 4},
         {-1, 9, -26, 57, 219, 2, -9, 5}, {0, 9, -24, 49, 220, 8, -11, 5},
         {0, 8, -22, 41, 223, 14, -14, 6}, {0, 8, -20, 34, 223, 20, -16, 7}},
        {
         {19, 218, 19, 0}, {14, 218, 24, 0}, {9, 218, 30, -1}, {5, 217, 36, -2},
         {1, 215, 43, -3}, {-2, 211, 50, -3}, {-5, 208, 57, -4}, {-7, 203, 65, -5},
         {-9, 198, 73, -6}, {-11, 192, 82, -7}, {-12, 186, 90, -8}, {-13, 180, 99, -10},
         {-14, 174, 107, -11}, {-14, 165, 116, -11}, {-14, 157, 125, -12}, {-14, 150, 133, -13},
         {-14, 142, 142, -14}, {-13, 133, 150, -14}, {-12, 125, 157, -14}, {-11, 116, 165, -14},
         {-11, 107, 174, -14}, {-10, 99, 180, -13}, {-8, 90, 186, -12}, {-7, 82, 192, -11},
         {-6, 73, 198, -9}, {-5, 65, 203, -7}, {-4, 57, 208, -5}, {-3, 50, 211, -2},
         {-3, 43, 215, 1}, {-2, 36, 217, 5}, {-1, 30, 218, 9}, {0, 24, 218, 14}}},
    {0x14000,                           /* 1600 : 1280 */
        {
         {7, -26, 46, 202, 46, -26, 7, 0}, {7, -24, 40, 200, 53, -27, 7, 0},
         {7, -23, 35, 199, 59, -28, 7, 0}, {7, -22, 29, 198, 66, -29, 7, 0},
         {7, -20, 23, 196, 72, -29, 7, 0}, {6, -19, 18, 196, 79, -30, 6, 0},
         {6, -17, 13, 193, 85, -30, 6, 0}, {6, -16, 9, 190, 92, -31, 6, 0},
         {5, -14, 4, 188, 99, -31, 5, 0}, {5, -13, 0, 184, 106, -30, 4, 0},
         {5, -11, -4, 179, 112, -30, 4, 1}, {4, -9, -8, 175, 119, -29, 3, 1},
         {4, -8, -11, 171, 126, -29, 2, 1}, {3, -6, -14, 166, 132, -27, 1, 1},
         {3, -5, -17, 161, 138, -26, 0, 2}, {3, -4, -20, 156, 144, -24, -1, 2},
         {2, -2, -22, 150, 150, -22, -2, 2}, {2, -1, -24, 144, 156, -20, -4, 3},
         {2, 0, -26, 138, 161, -17, -5, 3}, {1, 1, -27, 132, 166, -14, -6, 3},
         {1, 2, -29, 126, 171, -11, -8, 4}, {1, 3, -29, 119, 175, -8, -9, 4},
         {1, 4, -30, 112, 179, -4, -11, 5}, {0, 4, -30, 106, 184, 0, -13, 5},
         {0, 5, -31, 99, 188, 4, -14, 5}, {0, 6, -31, 92, 190, 9, -16, 6},
         {0, 6, -30, 85, 193, 13, -17, 6}, {0, 6, -30, 79, 196, 18, -19, 6},
         {0, 7, -29, 72, 196, 23, -20, 7}, {0, 7, -29, 66, 198, 29, -22, 7},
         {0, 7, -28, 59, 199, 35, -23, 7}, {0, 7, -27, 53, 200, 40, -24, 7}},
        {
         {32, 192, 32, 0}, {27, 193, 37, -1}, {23, 192, 42, -1}, {18, 192, 48, -2},
         {14, 191, 54, -3}, {11, 189, 60, -4}, {8, 185, 67, -4}, {5, 182, 74, -5},
         {2, 180, 80, -6}, {0, 176, 87, -7}, {-2, 171, 94, -7}, {-4, 166, 102, -8},
         {-5, 160, 109, -8}, {-6, 155, 116, -9}, {-7, 149, 123, -9}, {-8, 143, 130, -9},
         {-8, 136, 136, -8}, {-9, 130, 143, -8}, {-9, 123, 149, -7}, {-9, 116, 155, -6},
         {-8, 109, 160, -5}, {-8, 102, 166, -4}, {-7, 94, 171, -2}, {-7, 87, 176, 0},
         {-6, 80, 180, 2}, {-5, 74, 182, 5}, {-4, 67, 185, 8}, {-4, 60, 189, 11},
         {-3, 54, 191, 14}, {-2, 48, 192, 18}, {-1, 42, 192, 23}, {-1, 37, 193, 27}}},
    {0x18000,                           /* 1920 : 1280 */
        {
         {0, -23, 65, 172, 65, -23, 0, 0}, {0, -23, 60, 173, 70, -23, -1, 0},
         {1, -23, 55, 173, 74, -22, -2, 0}, {1, -23, 50, 173, 79, -22, -2, 0},
         {2, -22, 46, 169, 84, -21, -3, 1}, {2, -22, 41, 169, 89, -20, -4, 1},
         {2, -22, 37, 167, 94, -19, -4, 1}, {2, -21, 32, 166, 99, -18, -5, 1},
         {3, -20, 28, 162, 104, -16, -6, 1}, {3, -20, 24, 161, 108, -15, -7, 2},
         {3, -19, 20, 158, 113, -13, -8, 2}, {3, -18, 17, 155, 118, -12, -9, 2},
         {3, -17, 13, 152, 122, -9, -10, 2}, {3, -17, 10, 150, 126, -7, -11, 2},
         {3, -16, 6, 147, 131, -5, -12, 2}, {3, -15, 3, 142, 135, -2, -13, 3},
         {3, -14, 0, 139, 139, 0, -14, 3}, {3, -13, -2, 135, 142, 3, -15, 3},
         {2, -12, -5, 131, 147, 6, -16, 3}, {2, -11, -7, 126, 150, 10, -17, 3},
         {2, -10, -9, 122, 152, 13, -17, 3}, {2, -9, -12, 118, 155, 17, -18, 3},
         {2, -8, -13, 113, 158, 20, -19, 3}, {2, -7, -15, 108, 161, 24, -20, 3},
         {1, -6, -16, 104, 162, 28, -20, 3}, {1, -5, -18, 99, 166, 32, -21, 2},
         {1, -4, -19, 94, 167, 37, -22, 2}, {1, -4, -20, 89, 169, 41, -22, 2},
         {1, -3, -21, 84, 169, 46, -22, 2}, {0, -2, -22, 79, 173, 50, -23, 1},
         {0, -2, -22, 74, 173, 55, -23, 1}, {0, -1, -23, 70, 173, 60, -23, 0}},
        {
         {44, 168, 44, 0}, {40, 168, 49, -1}, {36, 167, 54, -1}, {32, 167, 59, -2},
         {28, 166, 64, -2}, {25, 164, 69, -2}, {22, 163, 74, -3}, {18, 161, 80, -3},
         {16, 158, 85, -3}, {13, 155, 91, -3}, {10, 153, 96, -3}, {8, 149, 102, -3},
         {6, 146, 107, -3}, {4, 141, 113, -2}, {3, 137, 118, -2}, {1, 133, 123, -1},
         {0, 128, 128, 0}, {-1, 123, 133, 1}, {-2, 118, 137, 3}, {-2, 113, 141, 4},
         {-3, 107, 146, 6}, {-3, 102, 149, 8}, {-3, 96, 153, 10}, {-3, 91, 155, 13},
         {-3, 85, 158, 16}, {-3, 80, 161, 18}, {-3, 74, 163, 22}, {-2, 69, 164, 25},
         {-2, 64, 166, 28}, {-2, 59, 167, 32}, {-1, 54, 167, 36}, {-1, 49, 168, 40}}},
    {0x20000,                           /* 1440 : 720 */
        {
         {-8, 1, 73, 124, 73, 1, -8, 0}, {-8, 0, 70, 126, 75, 2, -9, 0},
         {-7, -2, 67, 126, 78, 3, -9, 0}, {-7, -3, 64, 126, 80, 5, -9, 0},
         {-7, -4, 62, 125, 83, 6, -9, 0}, {-6, -5, 59, 124, 86, 8, -10, 0},
         {-6, -5, 56, 122, 89, 10, -10, 0}, {-6, -6, 54, 122, 91, 12, -10, -1},
         {-5, -7, 51, 122, 93, 13, -10, -1}, {-5, -8, 48, 121, 96, 15, -10, -1},
         {-4, -8, 46, 119, 98, 17, -11, -1}, {-4, -9, 43, 119, 100, 19, -11, -1},
         {-4, -9, 40, 118, 102, 21, -11, -1}, {-3, -10, 38, 115, 105, 24, -11, -2},
         {-3, -10, 36, 113, 107, 26, -11, -2}, {-3, -10, 33, 113, 108, 28, -11, -2},
         {-2, -10, 31, 109, 109, 31, -10, -2}, {-2, -11, 28, 108, 113, 33, -10, -3},
         {-2, -11, 26, 107, 113, 36, -10, -3}, {-2, -11, 24, 105, 115, 38, -10, -3},
         {-1, -11, 21, 102, 118, 40, -9, -4}, {-1, -11, 19, 100, 119, 43, -9, -4},
         {-1, -11, 17, 98, 119, 46, -8, -4}, {-1, -10, 15, 96, 121, 48, -8, -5},
         {-1, -10, 13, 93, 122, 51, -7, -5}, {-1, -10, 12, 91, 122, 54, -6, -6},
         {0, -10, 10, 89, 122, 56, -5, -6}, {0, -10, 8, 86, 124, 59, -5, -6},
         {0, -9, 6, 83, 125, 62, -4, -7}, {0, -9, 5, 80, 126, 64, -3, -7},
         {0, -9, 3, 78, 126, 67, -2, -7}, {0, -9, 2, 75, 126, 70, 0, -8}},
        {
         {58, 140, 58, 0}, {54, 141, 61, 0}, {51, 140, 65, 0}, {47, 141, 68, 0},
         {44, 139, 72, 1}, {41, 138, 76, 1}, {38, 136, 80, 2}, {35, 136, 83, 2},
         {32, 134, 87, 3}, {29, 132, 91, 4}, {26, 130, 95, 5}, {24, 128, 98, 6},
         {21, 126, 102, 7}, {19, 124, 105, 8}, {17, 120, 109, 10}, {15, 118, 112, 11},
         {13, 115, 115, 13}, {11, 112, 118, 15}, {10, 109, 120, 17}, {8, 105, 124, 19},
         {7, 102, 126, 21}, {6, 98, 128, 24}, {5, 95, 130, 26}, {4, 91, 132, 29},
         {3, 87, 134, 32}, {2, 83, 136, 35}, {2, 80, 136, 38}, {1, 76, 138, 41},
         {1, 72, 139, 44}, {0, 68, 141, 47}, {0, 65, 140, 51}, {0, 61, 141, 54}}},
    {0x2AAAA,                           /* 1920 : 720 */
        {
         {-3, 18, 66, 94, 66, 18, -3, 0}, {-3, 17, 65, 93, 68, 19, -3, 0},
         {-3, 16, 63, 93, 69, 21, -3, 0}, {-3, 14, 62, 92, 71, 22, -2, 0},
         {-3, 13, 60, 94, 72, 23, -2, -1}, {-3, 12, 59, 93, 73, 25, -2, -1},
         {-3, 11, 57, 93, 75, 26, -2, -1}, {-3, 10, 55, 93, 76, 27, -1, -1},
         {-3, 9, 54, 92, 77, 29, -1, -1}, {-3, 8, 52, 93, 79, 30, -1, -2},
         {-3, 8, 51, 90, 80, 32, 0, -2}, {-3, 7, 49, 91, 81, 33, 0, -2},
         {-3, 6, 47, 90, 82, 35, 1, -2}, {-3, 5, 46, 90, 83, 36, 1, -2},
         {-3, 4, 44, 88, 85, 38, 2, -2}, {-3, 4, 43, 88, 85, 40, 2, -3},
         {-3, 3, 41, 87, 87, 41, 3, -3}, {-3, 2, 40, 85, 88, 43, 4, -3},
         {-2, 2, 38, 85, 88, 44, 4, -3}, {-2, 1, 36, 83, 90, 46, 5, -3},
         {-2, 1, 35, 82, 90, 47, 6, -3}, {-2, 0, 33, 81, 91, 49, 7, -3},
         {-2, 0, 32, 80, 90, 51, 8, -3}, {-2, -1, 30, 79, 93, 52, 8, -3},
         {-1, -1, 29, 77, 92, 54, 9, -3}, {-1, -1, 27, 76, 93, 55, 10, -3},
         {-1, -2, 26, 75, 93, 57, 11, -3}, {-1, -2, 25, 73, 93, 59, 12, -3},
         {-1, -2, 23, 72, 94, 60, 13, -3}, {0, -2, 22, 71, 92, 62, 14, -3},
         {0, -3, 21, 69, 93, 63, 16, -3}, {0, -3, 19, 68, 93, 65, 17, -3}},
        {
         {64, 128, 64, 0}, {61, 128, 67, 0}, {58, 127, 70, 1}, {55, 126, 73, 2},
         {52, 126, 75, 3}, {49, 125, 78, 4}, {46, 124, 81, 5}, {43, 123, 84, 6},
         {40, 122, 87, 7}, {38, 119, 90, 9}, {35, 119, 92, 10}, {32, 117, 95, 12},
         {30, 115, 97, 14}, {28, 113, 100, 15}, {25, 112, 102, 17}, {23, 109, 105, 19},
         {21, 107, 107, 21}, {19, 105, 109, 23}, {17, 102, 112, 25}, {15, 100, 113, 28},
         {14, 97, 115, 30}, {12, 95, 117, 32}, {10, 92, 119, 35}, {9, 90, 119, 38},
         {7, 87, 122, 40}, {6, 84, 123, 43}, {5, 81, 124, 46}, {4, 78, 125, 49},
         {3, 75, 126, 52}, {2, 73, 126, 55}, {1, 70, 127, 58}, {0, 67, 128, 61}}},
    {0x40000,                           /* 2880 : 720 */
        {
         {7, 29, 57, 70, 57, 29, 7, 0}, {7, 28, 56, 70, 58, 30, 7, 0},
         {6, 28, 56, 69, 58, 31, 8, 0}, {6, 27, 55, 69, 59, 32, 8, 0},
         {5, 26, 54, 69, 60, 33, 9, 0}, {5, 25, 53, 70, 60, 34, 9, 0},
         {4, 24, 52, 70, 61, 35, 10, 0}, {4, 23, 51, 71, 62, 35, 10, 0},
         {4, 22, 51, 70, 62, 36, 11, 0}, {3, 22, 50, 68, 63, 37, 12, 1},
         {3, 21, 49, 69, 63, 38, 12, 1}, {3, 20, 48, 68, 64, 39, 13, 1},
         {3, 19, 47, 67, 65, 40, 14, 1}, {2, 19, 46, 68, 65, 41, 14, 1},
         {2, 18, 45, 68, 65, 42, 15, 1}, {2, 17, 45, 66, 66, 43, 16, 1},
         {2, 16, 44, 66, 66, 44, 16, 2}, {1, 16, 43, 66, 66, 45, 17, 2},
         {1, 15, 42, 65, 68, 45, 18, 2}, {1, 14, 41, 65, 68, 46, 19, 2},
         {1, 14, 40, 65, 67, 47, 19, 3}, {1, 13, 39, 64, 68, 48, 20, 3},
         {1, 12, 38, 63, 69, 49, 21, 3}, {1, 12, 37, 63, 68, 50, 22, 3},
         {0, 11, 36, 62, 70, 51, 22, 4}, {0, 10, 35, 62, 71, 51, 23, 4},
         {0, 10, 35, 61, 70, 52, 24, 4}, {0, 9, 34, 60, 70, 53, 25, 5},
         {0, 9, 33, 60, 69, 54, 26, 5}, {0, 8, 32, 59, 69, 55, 27, 6},
         {0, 8, 31, 58, 69, 56, 28, 6}, {0, 7, 30, 58, 70, 56, 28, 7}},
        {
         {69, 118, 69, 0}, {66, 118, 71, 1}, {63, 118, 73, 2}, {60, 117, 75, 4},
         {57, 117, 77, 5}, {54, 116, 79, 7}, {52, 114, 82, 8}, {49, 113, 84, 10},
         {46, 112, 86, 12}, {44, 111, 88, 13}, {41, 110, 90, 15}, {39, 108, 92, 17},
         {37, 107, 93, 19}, {34, 106, 95, 21}, {32, 104, 97, 23}, {30, 102, 99, 25},
         {27, 101, 101, 27}, {25, 99, 102, 30}, {23, 97, 104, 32}, {21, 95, 106, 34},
         {19, 93, 107, 37}, {17, 92, 108, 39}, {15, 90, 110, 41}, {13, 88, 111, 44},
         {12, 86, 112, 46}, {10, 84, 113, 49}, {8, 82, 114, 52}, {7, 79, 116, 54},
         {5, 77, 117, 57}, {4, 75, 117, 60}, {2, 73, 118, 63}, {1, 71, 118, 66}}}
};

static const struct scale_filter *filter_for(u32 src, u32 dst)
{
    u32 ratio = (src << 16) / dst;
    unsigned i = 0;
    while (i + 1 < sizeof(filters) / sizeof(filters[0]) && ratio > (filters[i].ratio + filters[i + 1].ratio) / 2)
        i++;
    return &filters[i];
}

/* the parameter block the display engine reads through +0x1E4 */
static void de_filters(u32 w, u32 h, u32 dw, u32 dh)
{
    static const u8 tail[20] = {0x20, 0x2C, 0x07, 0x08, 0x0A, 0x20, 0x03, 0x00, 0x05, 0x28,
                                0x14, 0x09, 0x08, 0x17, 0x0B, 0x21, 0x08, 0x0B, 0x14, 0x16};
    const struct scale_filter *fh = filter_for(w, dw), *fv = filter_for(h, dh);
    volatile short *p = (volatile short *)KSEG1(AV_PARAM_PHYS);
    volatile u8 *b = (volatile u8 *)KSEG1(AV_PARAM_PHYS);
    for (int i = 0; i < 0x560 / 2; i++)
        p[i] = 0;
    for (int i = 0; i < 32 * 4; i++) {
        p[i] = fv->v4[i / 4][i % 4];                /* +0x000 vertical luma */
        p[0x080 + i] = fv->v4[i / 4][i % 4];        /* +0x100 vertical chroma */
        p[0x200 + i] = fh->v4[i / 4][i % 4];        /* +0x400 horizontal chroma */
    }
    for (int i = 0; i < 32 * 8; i++)
        p[0x100 + i] = fh->h8[i / 8][i % 8];        /* +0x200 horizontal luma */
    for (int i = 0; i < 20; i++)
        b[0x540 + i] = tail[i];
    DE(0x1E4) = AV_PARAM_PHYS;
}

static u32 de_saved_scale[20];          /* 0x100..0x14C as they were */
static u32 de_saved_bottom[2];          /* 0x1C8, 0x1CC */
static int one_to_one;                  /* 'play -1': the picture's own size, not fitted to the screen */

/* the layer set up for a w x h picture shown fit_w wide at the screen's height (its aspect ratio); avc_mbw: an
   H.264 frame (avc.c's layout) that many macroblocks wide, 0 an MPEG-2 one */
static void de_setup(u32 w, u32 h, u32 fit_w, u32 avc_mbw)
{
    for (int i = 0; i < 0x10; i++)
        de_saved[i] = DE(0x160 + 4 * i);
    for (int i = 0; i < 20; i++)
        de_saved_scale[i] = DE(0x100 + 4 * i);
    de_saved_bottom[0] = DE(0x1C8);
    de_saved_bottom[1] = DE(0x1CC);
    u32 dw, dh;
    if (one_to_one) {
        dw = w < SCREEN_W ? w : SCREEN_W;
        dh = h < SCREEN_H ? h : SCREEN_H;
    } else {                            /* the screen's height, the width by the aspect ratio */
        dh = SCREEN_H;
        dw = fit_w > SCREEN_W ? SCREEN_W : fit_w;
        dw &= ~1u;
    }
    u32 x0 = (SCREEN_W - dw) / 2, y0 = ((SCREEN_H - dh) / 2) & ~1u, x1 = x0 + dw - 1, y1 = y0 + dh - 1;
    de_filters(w, h, dw, dh);
    DE(0x100) = 0;                      /* the picture from its first column */
    DE(0x108) = 0;
    DE(0x110) = step_h(w, dw);
    DE(0x114) = step_v(h, dh);
    DE(0x118) = step_h(w / 2, dw);
    DE(0x11C) = step_v(h / 2, dh);
    DE(0x104) = phase_v(0);             /* a frame for a progressive screen: the luma from line 0, the */
    DE(0x10C) = phase_v(-0x400);        /* chroma (4:2:0) a quarter of its line higher; no bottom field */
    DE(0x1C8) = 0;
    DE(0x1CC) = 0;
    DE(0x134) = (x1 & 0x7FFu) << 16;
    DE(0x138) = (x1 & 0x7FFu) << 16;
    DE(0x13C) = 0;
    DE(0x140) = 0;
    DE(0x144) = 1;
    DE(0x148) = 1;
    DE(0x174) = 0;
    /* the source's layout: an H.264 frame's tiles are one macroblock wide (vdec_avc_decore's Display_Info: stride
       (w + 15) / 16, hal_sdhd_mp_size_insource: the low half (w + 15) / 16) */
    DE(0x178) = avc_mbw ? avc_mbw << 16 | avc_mbw
                        : (w / 8) << 24 | ((w + 31) / 32) << 16 | ((h + 15) / 16) << 8 | ((w + 15) / 16);
    DE(0x17C) = h << 16 | w;
    DE(0x180) = x0 << 16 | x1;
    DE(0x184) = y0 << 16 | y1;
    DE(0x188) = 0x00108080u;            /* black around it */
    /* MPEG-2 tiling, or H.264's (b1: hal_sdhd_h264_mapping_enable); the scaler in, four vertical taps; the frame's
       lines in turn (as fields, mode 0, the screen showed the picture at half its height and what lies after it
       in RAM below) */
    DE(0x190) = (DE(0x190) & ~0x000700CEu) | 0x00020000u | (avc_mbw ? 0x2u : 0);
}

/* the layer for the stream being played (or played last) */
static void show_setup(void)
{
    if (h264) {
        const struct avc_info *a = avc_info();
        de_setup(a->w, a->h, a->h && a->sar_h ? a->w * SCREEN_H / a->h * a->sar_w / a->sar_h : SCREEN_W, a->mbw);
    } else
        de_setup(seq.w, seq.h, seq.aspect == 2 ? SCREEN_H * 4 / 3 : seq.aspect == 3 || seq.aspect == 4 ? SCREEN_W
                                                                 : seq.w * SCREEN_H / seq.h, 0);
}

static void de_show(int f)
{
    u32 y = h264 ? avc_luma(f) : AV_WINDOW_PHYS + frm[f].y, c = h264 ? avc_chroma(f) : AV_WINDOW_PHYS + frm[f].c;
    DE(0x164) = y;
    DE(0x168) = y;
    DE(0x16C) = c;
    DE(0x170) = c;
    if (!de_on) {
        de_window(1);
        de_on = 1;
    }
}

static void de_restore(void)
{
    de_window(0);
    de_on = 0;
    for (int i = 0; i < 0x0C; i++)      /* 0x160..0x18C */
        DE(0x160 + 4 * i) = de_saved[i];
    DE(0x190) = de_saved[0x0C] & ~0x00200000u;      /* the control word as it was, the layer off */
    for (int i = 0; i < 20; i++)
        DE(0x100 + 4 * i) = de_saved_scale[i];
    DE(0x1C8) = de_saved_bottom[0];
    DE(0x1CC) = de_saved_bottom[1];
}

/* ---- the file and the transport stream / program stream ---- */
enum { KIND_TS, KIND_PS, KIND_ES };
static int kind, file_end;
static u16 pmt_pid, video_pid, audio_pid;
static u8 video_type, audio_type, video_cc = 0xFF;
static int pes_video_on, pes_audio_on;
static u32 first_pts_v, first_pts_a;
static u8 tsp[188];
static int tsp_n;
static struct fifo stage;               /* a program stream's bytes */

/* n bytes added to a FIFO (the video's: the parser's positions moved with its bytes; it keeps rd at the first
   byte it still needs) */
static void put(struct fifo *f, const u8 *p, u32 n, int video)
{
    if (!n)
        return;
    s32 moved = fifo_room(f, n);
    if (moved < 0) {
        if (!video)
            st.audio_dropped += n;      /* (sound far ahead of the picture: dropped) */
        return;
    }
    if (moved && video) {
        scan_at -= (u32)moved;
        if (in_slices)
            slices_at -= (u32)moved;
    }
    copy(f->buf + f->wr, p, n);
    f->wr += n;
}

/* a PES packet's start: its payload's offset in p[0..n), -1 not a PES header; *pts (90 kHz) when it has one */
static int pes_header(const u8 *p, int n, u32 *pts, int *has_pts)
{
    *has_pts = 0;
    if (n < 9 || p[0] || p[1] || p[2] != 1)
        return -1;
    if ((p[6] & 0xC0) == 0x80) {        /* MPEG-2 PES */
        int hl = 9 + p[8];
        if (p[7] & 0x80 && n >= 14) {
            *pts = (u32)(p[9] & 0x0E) << 28 | (u32)p[10] << 21 | (u32)(p[11] >> 1) << 14 | (u32)p[12] << 6 |
                   p[13] >> 2;
            *has_pts = 1;
        }
        return hl <= n ? hl : -1;
    }
    int i = 6;                          /* MPEG-1 PES (a program stream's) */
    while (i < n && p[i] == 0xFF)
        i++;
    if (i < n && (p[i] & 0xC0) == 0x40)
        i += 2;
    if (i < n && (p[i] & 0xF0) == 0x20) {
        if (i + 5 <= n) {
            *pts = (u32)(p[i] & 0x0E) << 28 | (u32)p[i + 1] << 21 | (u32)(p[i + 2] >> 1) << 14 |
                   (u32)p[i + 3] << 6 | p[i + 4] >> 2;
            *has_pts = 1;
        }
        i += 5;
    } else if (i < n && (p[i] & 0xF0) == 0x30)
        i += 10;
    else
        i++;
    return i <= n ? i : -1;
}

/* a section's bytes (PAT / PMT) from a packet's payload */
static void ts_section(const u8 *p, int n, int pid)
{
    if (n < 1)
        return;
    int ptr = p[0];
    p += 1 + ptr;
    n -= 1 + ptr;
    if (n < 12)
        return;
    int len = (p[1] & 0x0F) << 8 | p[2];
    if (len + 3 > n)
        len = n - 3;
    if (pid == 0 && p[0] == 0x00) {     /* PAT: the first programme's PMT */
        for (int i = 8; i + 4 <= len + 3 - 4; i += 4) {
            int prog = p[i] << 8 | p[i + 1];
            if (prog) {
                pmt_pid = (u16)((p[i + 2] & 0x1F) << 8 | p[i + 3]);
                break;
            }
        }
    } else if (pid == pmt_pid && p[0] == 0x02 && !video_pid) {
        int info = (p[10] & 0x0F) << 8 | p[11];
        for (int i = 12 + info; i + 5 <= len + 3 - 4; ) {
            u8 type = p[i];
            u16 epid = (u16)((p[i + 1] & 0x1F) << 8 | p[i + 2]);
            int es_info = (p[i + 3] & 0x0F) << 8 | p[i + 4];
            if (!video_pid && (type == 0x01 || type == 0x02 || type == 0x1B || type == 0x10 || type == 0x24))
                video_pid = epid, video_type = type;
            else if ((type == 0x03 || type == 0x04) && (!audio_pid || (audio_type != 0x03 && audio_type != 0x04)))
                audio_pid = epid, audio_type = type;    /* (MPEG audio, the kind decoded here: before an AC-3 or
                                                           a private stream -- teletext -- listed ahead of it) */
            else if (!audio_pid && (type == 0x0F || type == 0x81 || type == 0x06))
                audio_pid = epid, audio_type = type;
            i += 5 + es_info;
        }
        uprintf("stream: PMT PID 0x%x, ", pmt_pid);
        if (video_pid)
            uprintf("video PID 0x%x (type 0x%02x%s), ", video_pid, video_type, video_type == 0x1B ? ": H.264" :
                    video_type == 1 ? ": MPEG-1" : video_type == 2 ? ": MPEG-2" : video_type == 0x24 ?
                    ": HEVC, not decoded here" : "");
        else
            uprintf("no video, ");
        if (audio_pid)
            uprintf("audio PID 0x%x (type 0x%02x%s)\n", audio_pid, audio_type, audio_type == 3 || audio_type == 4 ?
                    ": MPEG audio" : audio_type == 0x0F || audio_type == 0x11 ? ": AAC, not decoded here" :
                    audio_type == 0x81 ? ": AC-3, not decoded here" : "");
        else
            uprintf("no sound\n");
    }
}

static void ts_packet(const u8 *p)
{
    st.packets++;
    int pid = (p[1] & 0x1F) << 8 | p[2], pusi = p[1] & 0x40, afc = (p[3] >> 4) & 3, i = 4;
    if (afc == 2 || afc == 0)
        return;
    if (afc == 3)
        i = 5 + p[4];
    if (i >= 188)
        return;
    const u8 *pl = p + i;
    int n = 188 - i;
    if (pid == 0 || (pmt_pid && pid == pmt_pid)) {
        if (pusi)
            ts_section(pl, n, pid);
        return;
    }
    u32 pts;
    int has;
    if (pid && pid == video_pid && (video_type <= 2 || video_type == 0x1B)) {
        u8 cc = p[3] & 15;
        if (video_cc != 0xFF && cc != ((video_cc + 1) & 15))
            st.cc_errors++;
        video_cc = cc;
        if (pusi) {
            int h = pes_header(pl, n, &pts, &has);
            if (h < 0)
                return;
            if (has && !first_pts_v)
                first_pts_v = pts | 1;
            pes_video_on = 1;
            pl += h, n -= h;
        }
        if (pes_video_on)
            put(&ves, pl, (u32)n, 1);
    } else if (pid && pid == audio_pid && (audio_type == 3 || audio_type == 4)) {
        if (pusi) {
            int h = pes_header(pl, n, &pts, &has);
            if (h < 0)
                return;
            if (has && !first_pts_a)
                first_pts_a = pts | 1;
            pes_audio_on = 1;
            pl += h, n -= h;
        }
        if (pes_audio_on)
            put(&aes, pl, (u32)n, 0);
    }
}

/* a program stream: packs, a system header, PES packets (video 0xE0.., audio 0xC0..) */
static void ps_parse(void)
{
    u8 *b = stage.buf;
    for (;;) {
        u32 at = stage.rd, end = stage.wr;
        while (at + 4 <= end && !(b[at] == 0 && b[at + 1] == 0 && b[at + 2] == 1 && b[at + 3] >= 0xB9))
            at++;
        stage.rd = at;
        if (at + 14 > end)
            return;
        u8 id = b[at + 3];
        if (id == 0xB9) {               /* program end */
            stage.rd = at + 4;
            continue;
        }
        if (id == 0xBA) {               /* pack header */
            u32 len = (b[at + 4] & 0xC0) == 0x40 ? 14u + (b[at + 13] & 7u) : 12u;
            stage.rd = at + len;
            continue;
        }
        u32 len = 6u + ((u32)b[at + 4] << 8 | b[at + 5]);
        if (at + len > end) {
            if (file_end)
                stage.rd = end;
            return;
        }
        u32 pts;
        int has;
        if ((id & 0xF0) == 0xE0 && !video_pid) {
            video_pid = id;
            video_type = 2;
        }
        if ((id & 0xE0) == 0xC0 && !audio_pid) {
            audio_pid = id;
            audio_type = 3;
        }
        if (id == video_pid || id == audio_pid) {
            int h = pes_header(b + at, (int)len, &pts, &has);
            if (h >= 0) {
                if (id == video_pid) {
                    if (has && !first_pts_v)
                        first_pts_v = pts | 1;
                    put(&ves, b + at + h, len - (u32)h, 1);
                } else {
                    if (has && !first_pts_a)
                        first_pts_a = pts | 1;
                    put(&aes, b + at + h, len - (u32)h, 0);
                }
            }
        }
        stage.rd = at + len;
    }
}

static u8 chunk[4096];

/* a piece of the file read and taken apart: 1, 0 the file's end (or a read error: said) */
static int feed(void)
{
    if (file_end)
        return 0;
    int got = disk_stream_read(chunk, sizeof(chunk));
    if (got <= 0) {
        if (got < 0)
            uprintf("play: reading the file failed\n");
        file_end = 1;
        if (kind == KIND_PS)
            ps_parse();
        eos = 1;
        return 0;
    }
    st.bytes_read += (u32)got;
    if (kind == KIND_ES) {
        put(&ves, chunk, (u32)got, 1);
    } else if (kind == KIND_TS) {
        int i = 0;
        while (i < got) {
            if (tsp_n == 0 && chunk[i] != 0x47) {       /* (lost sync: the next 0x47) */
                i++;
                continue;
            }
            int take = 188 - tsp_n < got - i ? 188 - tsp_n : got - i;
            copy(tsp + tsp_n, chunk + i, (u32)take);
            tsp_n += take;
            i += take;
            if (tsp_n == 188) {
                ts_packet(tsp);
                tsp_n = 0;
            }
        }
    } else {
        if (fifo_room(&stage, (u32)got) < 0)
            stage.rd = stage.wr = 0;
        copy(stage.buf + stage.wr, chunk, (u32)got);
        stage.wr += (u32)got;
        ps_parse();
    }
    return 1;
}

/* ---- sound ---- */
static int sound_on;
static s16 pcm[1152], pcm48[1152 * 3 + 8];     /* (16 kHz sound: 1152 samples are 3456 at 48 kHz) */
static u8 aframe[1800];

static void sound_service(void)
{
    if (!sound_on) {
        aes.rd = aes.wr;                /* (not played: dropped -- kept, it would fill its buffer and want_data
                                           would let nothing more be read: 'play -x' never ended) */
        return;
    }
    for (int k = 0; k < 4; k++) {
        /* a frame header at the read position (else: look for one) */
        while (fifo_used(&aes) >= 4) {
            struct mp2_header h;
            if (mp2_header(aes.buf + aes.rd, &h))
                break;
            aes.rd++;
        }
        if (fifo_used(&aes) < 4)
            return;
        struct mp2_header h;
        (void)mp2_header(aes.buf + aes.rd, &h);
        if (fifo_used(&aes) < h.bytes || h.bytes > sizeof(aframe) - 4)
            return;
        u32 out = h.rate == 48000 ? h.samples : h.samples * 48000u / h.rate + 2;
        if ((u32)snd_room() < out)
            return;
        copy(aframe, aes.buf + aes.rd, h.bytes);
        aframe[h.bytes] = aframe[h.bytes + 1] = aframe[h.bytes + 2] = aframe[h.bytes + 3] = 0;
        aes.rd += h.bytes;
        if (mp2_decode(aframe, &h, pcm) < 0) {
            st.audio_bad++;
            continue;
        }
        st.audio_frames++;
        if (h.rate == 48000) {
            snd_write(pcm, (int)h.samples);
            st.audio_samples += h.samples;
        } else {
            int n = mp2_resample(pcm, (int)h.samples, h.rate, pcm48);
            snd_write(pcm48, n);
            st.audio_samples += (u32)n;
        }
    }
}

/* ---- keys: Esc / Ctrl-C (UART, telnet, USB keyboard), the remote's EXIT end it ---- */
static int stop_seen;                   /* one was read: every later look says so (the wait for the next frame
                                           reads the key, the player's loop asks again after it) */

static int stop_key(void)
{
    int c;
    struct ir_event ev;
    while (ir_poll(&ev))
        if (ev.valid && !ev.repeat) {
            const char *name = ir_key_name(ev.code);
            if (name && name[0] == 'E' && name[1] == 'X')      /* EXIT */
                return 1;
        }
    usb_int_poll();
    (void)kbd_poll();
    while ((c = kbd_getc()) >= 0)
        if (c == 3 || c == 27 || c == 'q')
            return 1;
    while ((c = uart_getc()) >= 0)
        if (c == 3 || c == 27 || c == 'q')
            return 1;
    return 0;
}

static int stop_asked(void)
{
    if (!stop_seen && stop_key())
        stop_seen = 1;
    return stop_seen;
}

/* ---- H.264: the video's NAL units from ves, one at a time, to avc.c ---- */
static int avc_flushed, avc_tail;       /* avc_end called; the end-of-stream NAL unit put after the last one */

/* the frames avc.c lets out (display order) to the queue */
static void avc_take(void)
{
    int f;
    while (queued < QUEUE_MAX && (f = avc_out()) >= 0)
        queue[queued++] = f;
}

/* the next NAL unit in ves (ves.rd at its start code, scan_at where the search for its end goes on): its first
   byte at *at, n bytes; 1, 0 more data needed, -1 none left */
static int next_nal(u32 *at, u32 *n)
{
    u32 s = find_code(ves.rd);
    if (s == ~0u) {
        if (eos)
            return -1;
        ves.rd = ves.wr > ves.rd + 3 ? ves.wr - 3 : ves.rd;    /* (no start code yet: what came before one goes) */
        return 0;
    }
    ves.rd = s;
    u32 e = find_code(scan_at > s + 3 ? scan_at : s + 3);
    if (e == ~0u) {
        if (!eos) {
            scan_at = ves.wr > s + 6 ? ves.wr - 3 : s + 3;
            return 0;
        }
        e = ves.wr;
    }
    *at = s + 3;
    *n = e - *at;
    return 1;
}

static int want_data(void);

/* while the engine decodes a slice: a piece of the file read -- only while it fits behind the stream's bytes
   without moving them (the engine reads them; one piece of a transport stream or an elementary stream puts at
   most its own size) -- or the sound fed */
static void while_decoding(void)
{
    if (want_data() && ves.wr + sizeof(chunk) <= ves.size && aes.wr + sizeof(chunk) <= aes.size)
        (void)feed();
    else if (sound_on)
        sound_service();
}

/* one step: 1 a NAL unit taken, 0 waiting (for data, for a free frame), -1 the end with everything out, -2 failed
   (said) */
static int h264_step(void)
{
    avc_take();
    if (avc_flushed)
        return queued ? 0 : -1;
    if (eos && !avc_tail) {             /* (an end-of-stream NAL unit: the last slice ends at its start code, which
                                           the engine's start code detector needs) */
        static const u8 tail[64] = {0, 0, 1, 0x0B};
        put(&ves, tail, sizeof(tail), 1);
        avc_tail = 1;
    }
    u32 at, n;
    int r = next_nal(&at, &n);
    if (r == 0) {
        if (!eos && fifo_used(&ves) >= 0x00180000u)
            (void)feed();               /* (a NAL unit longer than the reading ahead goes: more of it now) */
        return 0;
    }
    if (r < 0) {
        avc_end();
        avc_flushed = 1;
        avc_take();
        return 1;
    }
    int got = avc_nal(ves.buf + at, n, ves.wr - at);
    if (got == AVC_FAIL)
        return -2;
    if (got == AVC_FULL) {              /* (the NAL unit stays: given again once frames were shown) */
        avc_take();
        if (queued)
            return 0;
        uprintf("play: H.264: no frame buffer free\n");
        return -2;
    }
    ves.rd = at + n;
    scan_at = ves.rd + 3;
    avc_take();
    return 1;
}

/* an H.264 elementary stream: a start code, then an SPS / PPS / AUD / SEI or a slice */
static int annexb_h264(const u8 *p, int n)
{
    int i = !p[0] && !p[1] && !p[2] && n > 4 && p[3] == 1 ? 4 : !p[0] && !p[1] && p[2] == 1 ? 3 : 0;
    if (!i || i >= n || (p[i] & 0x80))
        return 0;
    u32 type = p[i] & 0x1F;
    return type == 1 || type == 5 || type == 6 || type == 7 || type == 8 || type == 9;
}

/* ---- the player ---- */
static int want_data(void)
{
    return !file_end && fifo_used(&ves) < 0x00180000u && fifo_used(&aes) < 0x000C0000u;
}

static const u32 frame_us[9] = {40000, 41708, 41667, 40000, 33367, 33333, 20000, 16683, 16667};
static int last_shown = -1;
static u32 pictures_total;

static void reset_state(void)
{
    ves.buf = (u8 *)KSEG0(AV_ES_PHYS);
    ves.size = AV_ES_SIZE;
    aes.buf = (u8 *)KSEG0(AV_AUDIO_PHYS);
    aes.size = AV_AUDIO_SIZE;
    stage.buf = (u8 *)KSEG0(AV_STAGE_PHYS);
    stage.size = AV_STAGE_SIZE;
    ves.rd = ves.wr = aes.rd = aes.wr = stage.rd = stage.wr = 0;
    have_seq = have_pic = in_slices = eos = 0;
    scan_at = slices_at = 0;
    seq_changed = 0;
    pmt_pid = video_pid = audio_pid = 0;
    video_type = audio_type = 0;
    video_cc = 0xFF;
    pes_video_on = pes_audio_on = 0;
    first_pts_v = first_pts_a = 0;
    tsp_n = 0;
    file_end = 0;
    nfrm = 0;
    queued = 0;
    h264 = avc_flushed = avc_tail = 0;
    older = newer = shown = shown_before = -1;
    last_shown = -1;                    /* (the frames are this play's now: an earlier play's last is gone) */
    u32 *z = (u32 *)&st;
    for (u32 i = 0; i < sizeof(st) / 4; i++)
        z[i] = 0;
}

static void show_next(void)
{
    int f = queue[0];
    for (int k = 1; k < queued; k++)
        queue[k - 1] = queue[k];
    queued--;
    de_show(f);
    if (h264 && shown_before >= 0)
        avc_release(shown_before);      /* (off the screen now: avc.c may decode into it again) */
    shown_before = shown;
    shown = f;
    last_shown = f;
    st.shown++;
}

int av_play(int argc, char **argv)
{
    u32 max_frames = 0;
    int fast = 0, keep = 0, verbose = 0, quiet = 0;
    one_to_one = 0;
    const char *path = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] == 'n' && !a[2] && i + 1 < argc) {
            max_frames = 0;
            for (const char *p = argv[++i]; *p >= '0' && *p <= '9'; p++)
                max_frames = max_frames * 10 + (u32)(*p - '0');
        } else if (a[0] == '-' && a[1] == 'x' && !a[2])
            fast = 1;
        else if (a[0] == '-' && a[1] == 'k' && !a[2])
            keep = 1;
        else if (a[0] == '-' && a[1] == 'v' && !a[2])
            verbose = 1;
        else if (a[0] == '-' && a[1] == 'q' && !a[2])
            quiet = 1;
        else if (a[0] == '-' && a[1] == '1' && !a[2])
            one_to_one = 1;
        else
            path = a;
    }
    if (!path) {
        uprintf("usage: play [-n frames] [-x] [-k] [-v] [-q] [-1] <file.ts|.mpg|.m2v|.264>\n");
        return -1;
    }
    if (ftp_disk_busy() || http_disk_busy()) {      /* (disk.c has one reader: taken, their download would
                                                       end as if the file did) */
        uprintf("play: the FTP server or the web page is moving a file now; try again\n");
        return -1;
    }
    u64 size = 0;
    if (disk_stream_open(path, &size) < 0)
        return -1;
    reset_state();
    stop_seen = 0;
    int got = disk_stream_read(chunk, sizeof(chunk));
    if (got < 4) {
        uprintf("play: %s: too short\n", path);
        disk_stream_close();
        return -1;
    }
    if (chunk[0] == 0x47 && (got < 189 || chunk[188] == 0x47))
        kind = KIND_TS;
    else if (!chunk[0] && !chunk[1] && chunk[2] == 1 && chunk[3] == 0xBA)
        kind = KIND_PS;
    else if (!chunk[0] && !chunk[1] && chunk[2] == 1 && chunk[3] == 0xB3)
        kind = KIND_ES;
    else if (annexb_h264(chunk, got)) {
        kind = KIND_ES;
        video_type = 0x1B;
    } else {
        uprintf("play: %s: not a transport stream, a program stream or an MPEG / H.264 video stream\n", path);
        disk_stream_close();
        return -1;
    }
    uprintf("play: %s, %u bytes, %s\n", path, (u32)size,
            kind == KIND_TS ? "transport stream" : kind == KIND_PS ? "program stream" :
            video_type == 0x1B ? "H.264 elementary stream" : "MPEG video elementary stream");
    /* (the first chunk taken apart as feed() does) */
    st.bytes_read = (u32)got;
    if (kind == KIND_ES)
        put(&ves, chunk, (u32)got, 1);
    else if (kind == KIND_TS) {
        for (int i = 0; i + 188 <= got; i += 188)
            ts_packet(chunk + i);
        tsp_n = got % 188;
        copy(tsp, chunk + got - tsp_n, (u32)tsp_n);
    } else {
        copy(stage.buf, chunk, (u32)got);
        stage.wr = (u32)got;
        ps_parse();
    }
    ve_reset();
    mp2_reset();
    sound_on = !fast && snd_init(0) == 0;
    if (de_on)
        de_restore();                   /* (a picture 'play -k' left: it goes; the OSD it hid is still owed --
                                           osd_was_on stays as that play found it -- and comes back at this
                                           one's end) */
    else
        osd_was_on = gfx_enabled();
    de_on = 0;
    u32 period_us = 40000;
    u64 next_show = 0;
    u32 per_us = ticks_per_ms() / 1000u;
    int stopped = 0, started = 0, failed = 0;
    u32 pics = 0;
    u64 began = now_ms();
    for (;;) {
        if (stop_asked()) {
            stopped = 1;
            break;
        }
        sound_service();
        /* read: a 4 KB piece takes the card ~4 ms (1 MB/s), a 25 frames/s picture's time is 40 ms -- one piece
           a pass while the buffers hold enough, more before the first picture or when they run low */
        for (int k = 0; k < (!started || fifo_used(&ves) < 0x00040000u ? 8 : 1) && want_data(); k++)
            (void)feed();
        if (kind == KIND_TS && !video_pid && st.bytes_read > 4u << 20) {
            uprintf("play: no video in the first 4 MB\n");
            failed = 1;
            break;
        }
        if (kind == KIND_TS && video_pid && video_type > 2 && video_type != 0x1B) {
            uprintf("play: the video is not MPEG-1/2 or H.264 (stream type 0x%02x): not played\n", video_type);
            failed = 1;
            break;
        }
        if (video_type == 0x1B && !h264) {
            h264 = 1;                   /* (avc.c's engine from now on: the MPEG-2 one's reset above is undone) */
            avc_begin((quiet ? 0 : AVC_DESCRIBE) | (verbose ? AVC_VERBOSE : 0), fast ? 0 : while_decoding);
        }
        /* show */
        u64 now = now_ticks();
        if (queued && (fast || !started || now >= next_show)) {
            if (!started) {
                started = 1;
                next_show = now;
                if (h264)                   /* (avc.c described the stream when it set it up) */
                    period_us = avc_info()->frame_us ? avc_info()->frame_us : 40000;
                if (osd_was_on)
                    gfx_enable(0);
                show_setup();
            }
            show_next();
            next_show += (u64)period_us * per_us;
            if (!fast && now > next_show + 4ull * period_us * per_us) {
                next_show = now + (u64)period_us * per_us;
                st.late++;
            }
            if (max_frames && st.shown >= max_frames)
                break;
            continue;
        }
        /* H.264: the next NAL unit to avc.c while the queue is short (its frames come back in display order; four
           decoded ahead: a 1080p picture takes 21-29 ms of the 42 a frame has, a big I picture more data than the
           card reads in a frame's time) */
        if (h264 && queued < 4) {
            int r = h264_step();
            if (r == -2) {
                failed = 1;
                break;
            }
            if (r == -1)
                break;                  /* (the stream's end, every frame shown) */
            if (r > 0)
                continue;
        }
        /* MPEG-2: decode the next picture when a frame buffer is free and the queue is short */
        int f = h264 ? -1 : nfrm ? free_frame() : 0;
        if (queued < 2 && f >= 0) {
            u32 end;
            if (next_picture(&end)) {
                if (seq_changed || !nfrm) {
                    seq_changed = 0;
                    if (frames_setup() < 0) {
                        uprintf("play: a %u x %u picture: too big for this player\n", seq.w, seq.h);
                        failed = 1;
                        break;
                    }
                    period_us = frame_us[seq.rate_code <= 8 ? seq.rate_code : 0];
                    if (!quiet) {
                        uprintf("video: %s %u x %u, %u.%02u frames/s, aspect code %u, ",
                                seq.mpeg2 ? "MPEG-2" : "MPEG-1", seq.w, seq.h, 100000000u / period_us / 100,
                                100000000u / period_us % 100, seq.aspect);
                        if (seq.bitrate >= 0x3FFFFu * 400u)     /* (all ones: "variable") */
                            uprintf("variable bit rate\n");
                        else
                            uprintf("%u kbit/s\n", seq.bitrate / 1000);
                    }
                    f = free_frame();
                }
                u32 n = end - slices_at;
                pics++;
                pictures_total++;
                int type = pic.type;
                if (type < 1 || type > 3 || (type != 1 && newer < 0) || (type == 3 && older < 0)) {
                    st.skipped++;       /* (no references yet: a stream joined in the middle) */
                } else if (pic.structure != 3 && pic.structure != 1 && pic.structure != 2) {
                    st.skipped++;
                } else {
                    int fwd = type == 2 ? newer : type == 3 ? older : -1, bwd = type == 3 ? newer : -1;
                    u64 t0 = now_ticks();
                    int r = ve_decode(ves.buf + slices_at, n, f, fwd, bwd);
                    u32 us = (u32)(now_ticks() - t0) / per_us;
                    st.pictures++;
                    st.types[type]++;
                    if (verbose || (r < 0 && st.timeouts <= 3))
                        uprintf("picture %u %c %u bytes -> frame %d (refs %d %d): %s %u us, status 0x%08x, MBs %u, "
                                "at 0x%04x, read %u of %u bits, errors 0x%08x\n", pics, " IPBD"[type], n, f, fwd,
                                bwd, r < 0 ? "TIMED OUT" : "done", us, st.last_status, st.last_mbs, st.last_pos,
                                st.last_offset, st.last_bits, st.last_err);
                    frm[f].type = (u8)type;
                    frm[f].number = pics;
                    if (type == 3) {
                        queue[queued++] = f;
                    } else {
                        if (newer >= 0)
                            queue[queued++] = newer;
                        older = newer;
                        newer = f;
                    }
                }
                continue;
            }
            if (eos && file_end) {      /* the end: the last reference shown too */
                if (newer >= 0) {
                    int k;
                    for (k = 0; k < queued && queue[k] != newer; k++)
                        ;
                    if (k == queued && newer != shown)
                        queue[queued++] = newer;
                    older = newer = -1;
                    continue;
                }
                if (!queued)
                    break;
            }
        }
        if (!fast && started && queued && now < next_show) {
            /* nothing to do until the next frame: the sound kept fed, the file read on while a piece fits in
               the time left */
            while (now_ticks() < next_show - 2000u * per_us && !stop_asked()) {
                sound_service();
                if (want_data() && now_ticks() + 8000u * per_us < next_show)
                    (void)feed();
            }
        }
    }
    u32 ms = (u32)(now_ms() - began);
    disk_stream_close();
    if (sound_on) {
        u64 t = now_ms();
        while (!stopped && snd_room() < 7000 && now_ms() - t < 300)
            ;                           /* (the sound's last frames played) */
        snd_stop();
    }
    if (h264) {                         /* (avc.c's counts in the same lines) */
        const struct avc_stats *s = avc_stats();
        st.pictures = s->pictures;
        st.types[1] = s->types[2];
        st.types[2] = s->types[0];
        st.types[3] = s->types[1];
        st.skipped = s->skipped;
        st.total_us = s->total_us;
        st.slowest_us = s->slowest_us;
        st.errors = s->errors;
        st.timeouts = s->timeouts;
        pictures_total += s->pictures;
    }
    if (!quiet || failed) {
        uprintf("%u pictures decoded (I %u, P %u, B %u), %u skipped, %u shown in %u.%03u s (%u late); decoding took "
                "%u us on average, %u at most; %u with error flags, %u timed out\n", st.pictures, st.types[1],
                st.types[2], st.types[3], st.skipped, st.shown, ms / 1000, ms % 1000, st.late,
                st.pictures ? st.total_us / st.pictures : 0, st.slowest_us, st.errors, st.timeouts);
        uprintf("read %u bytes (%u TS packets, %u video continuity errors); sound: %u frames (%u bad), %u samples, "
                "%u bytes dropped%s\n", st.bytes_read, st.packets, st.cc_errors, st.audio_frames, st.audio_bad,
                st.audio_samples, st.audio_dropped, sound_on ? "" : " (no sound)");
        if (st.pictures && h264)
            uprintf("%u slices; the last: status 0x%08x, the macroblock reached 0x%08x\n", avc_stats()->slices,
                    avc_stats()->last_status, avc_stats()->last_mb);
        else if (st.pictures)
            uprintf("the last picture: status 0x%08x, MBs %u, at 0x%04x, read %u of %u bits, errors 0x%08x\n",
                    st.last_status, st.last_mbs, st.last_pos, st.last_offset, st.last_bits, st.last_err);
    }
    if (!keep) {
        if (de_on)
            de_restore();
        if (osd_was_on)
            gfx_enable(1);
    } else if (de_on)
        uprintf("the last picture stays on the TV ('av off', Esc, Ctrl-C or the remote's EXIT takes "
                "it away)\n");
    return failed ? -1 : 0;
}

/* a picture kept on the TV ('play -k', 'av show') hides the OSD: Esc on the USB keyboard / the remote's EXIT
   (main.c) take it away; 1 if there was one */
int av_dismiss(void)
{
    if (!de_on)
        return 0;
    de_restore();
    gfx_enable(1);
    return 1;
}

void play_command(int argc, char **argv)
{
    (void)av_play(argc, argv);
}

/* ---- 'av shot': the frame shown last, as a BMP (RGB) or raw I420 ---- */
static u8 row[1920 * 3 + 16];

static u8 clamp8(s32 v)
{
    return (u8)(v < 0 ? 0 : v > 255 ? 255 : v);
}

static u32 avc_tiles;                   /* the picture is an H.264 frame (avc.c's layout) this many macroblocks wide */

/* a line's place among its four in an H.264 tile: they lie in the order 0, 2, 1, 3 */
static u32 avc_line(u32 l)
{
    return (l & ~3u) | (l & 1) << 1 | (l >> 1 & 1);
}

static u8 luma(const volatile u8 *y, u32 tiles, u32 x, u32 yy)
{
    if (avc_tiles)                      /* tiles 16 x 32, a line 16 bytes */
        return y[((yy >> 5) * tiles + (x >> 4)) * 512 + avc_line(yy & 31) * 16 + (x & 15)];
    return y[((yy >> 4) * tiles + (x >> 5)) * 512 + (yy & 15) * 32 + (x & 31)];
}

/* the Cb Cr pair at luma (x, yy) */
static const volatile u8 *chroma_at(const volatile u8 *c, u32 tiles, u32 x, u32 yy)
{
    if (avc_tiles) {                    /* tiles 8 pairs x 32 lines */
        u32 cy = yy >> 1;
        return c + ((cy >> 5) * tiles + (x >> 4)) * 512 + avc_line(cy & 31) * 16 + (x & 14);
    }
    return c + ((yy >> 5) * tiles + (x >> 5)) * 512 + ((yy & 0x1E) << 4) + (x & 0x1E);
}

/* a picture in the decoder's tiled layout (luma y, chroma c, w x h) into a file: 1 written, 0 not (said) */
static int write_picture(const char *path, const volatile u8 *y, const volatile u8 *c, u32 w, u32 h)
{
    u32 tiles = avc_tiles ? avc_tiles : (w + 31) / 32;
    int raw = ends_with(path, ".yuv");
    if (ftp_disk_busy() || http_disk_busy()) {      /* (disk.c's one writer is theirs) */
        uprintf("av shot: the FTP server or the web page is moving a file now; try again\n");
        return 0;
    }
    if (disk_upload_open(path, 0) < 0)
        return 0;
    int ok = 1;
    if (raw) {                          /* I420: Y, then Cb, then Cr */
        for (u32 yy = 0; yy < h && ok; yy++) {
            for (u32 x = 0; x < w; x++)
                row[x] = luma(y, tiles, x, yy);
            ok = disk_upload_put(row, w) == 0;
        }
        for (int plane = 0; plane < 2 && ok; plane++)
            for (u32 yy = 0; yy < h && ok; yy += 2) {
                for (u32 x = 0; x < w; x += 2)
                    row[x / 2] = chroma_at(c, tiles, x, yy)[plane];
                ok = disk_upload_put(row, w / 2) == 0;
            }
    } else {
        u32 stride = (w * 3 + 3) & ~3u, size = 54 + stride * h;
        u8 hd[54] = {'B', 'M'};
        hd[2] = (u8)size, hd[3] = (u8)(size >> 8), hd[4] = (u8)(size >> 16), hd[5] = (u8)(size >> 24);
        hd[10] = 54, hd[14] = 40;
        hd[18] = (u8)w, hd[19] = (u8)(w >> 8), hd[22] = (u8)h, hd[23] = (u8)(h >> 8);
        hd[26] = 1, hd[28] = 24;
        ok = disk_upload_put(hd, 54) == 0;
        for (u32 n = 0; n < h && ok; n++) {
            u32 yy = h - 1 - n;         /* (bottom row first) */
            for (u32 x = 0; x < w; x++) {
                const volatile u8 *cc = chroma_at(c, tiles, x, yy);
                s32 Y = 298 * (luma(y, tiles, x, yy) - 16), U = cc[0] - 128, V = cc[1] - 128;
                row[x * 3 + 2] = clamp8((Y + 409 * V + 128) >> 8);
                row[x * 3 + 1] = clamp8((Y - 100 * U - 208 * V + 128) >> 8);
                row[x * 3 + 0] = clamp8((Y + 516 * U + 128) >> 8);
            }
            for (u32 x = w * 3; x < stride; x++)
                row[x] = 0;
            ok = disk_upload_put(row, stride) == 0;
        }
    }
    if (disk_upload_close() < 0)
        ok = 0;
    return ok;
}

static void shot(const char *path, int frame)
{
    if (frame < 0 || (!h264 && !nfrm) || (h264 && !avc_info()->frames)) {
        uprintf("av shot: no picture decoded yet\n");
        return;
    }
    int first = h264 ? 1 : 0, count = h264 ? avc_info()->frames : nfrm;
    if (frame >= count) {
        uprintf("av shot: the frames are %d..%d\n", first, first + count - 1);
        return;
    }
    if (h264) {                         /* (frame: avc.c's number of it; shown as 1.., the engine's own) */
        const struct avc_info *a = avc_info();
        if (!avc_number(frame)) {
            uprintf("av shot: frame %d: the last play decoded nothing into it\n", frame + 1);
            return;
        }
        avc_tiles = a->mbw;
        int ok = write_picture(path, (const volatile u8 *)KSEG1(avc_luma(frame)),
                               (const volatile u8 *)KSEG1(avc_chroma(frame)), a->w, a->h);
        avc_tiles = 0;
        if (ok)
            uprintf("av shot: frame %d (picture %u, %c) %u x %u written to %s\n", frame + 1, avc_number(frame),
                    avc_type(frame), a->w, a->h, path);
        return;
    }
    if (write_picture(path, (const volatile u8 *)KSEG1(AV_WINDOW_PHYS + frm[frame].y),
                      (const volatile u8 *)KSEG1(AV_WINDOW_PHYS + frm[frame].c), seq.w, seq.h))
        uprintf("av shot: frame %d (picture %u, %c) %u x %u written to %s\n", frame, frm[frame].number,
                " IPBD"[frm[frame].type & 3], seq.w, seq.h, path);
}

void av_command(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "status";
    if (sub[0] == 's' && sub[1] == 'h' && sub[2] == 'o' && sub[3] == 't') {
        if (argc < 3) {
            uprintf("usage: av shot <file.bmp|file.yuv> [frame]\n");
            return;
        }
        int f = last_shown;
        if (argc > 3) {                 /* (a frame by its number: MPEG-2 0..4, H.264 1..) */
            f = 0;
            for (const char *p = argv[3]; *p >= '0' && *p <= '9' && f < 1000; p++)
                f = f * 10 + (*p - '0');
            if (h264)
                f--;
        }
        shot(argv[2], f);
    } else if (sub[0] == 's' && sub[1] == 'h' && sub[2] == 'o' && sub[3] == 'w') {
        if (last_shown < 0) {
            uprintf("av show: no picture decoded yet\n");
            return;
        }
        osd_was_on = gfx_enabled() || osd_was_on;
        gfx_enable(0);
        show_setup();
        de_show(last_shown);
        uprintf("the last picture on the TV; 'av off' takes it away\n");
    } else if (sub[0] == 'o' && sub[1] == 'f') {
        if (de_on)
            de_restore();
        else
            de_window(0);
        gfx_enable(1);
    } else if (sub[0] == 'r') {
        ve_reset();
        uprintf("decoder reset\n");
    } else {
        uprintf("decoder 0xb8004200: +00 %08x +08 %08x +0c %08x +10 %08x +14 %08x +18 %08x +20 %08x\n", VE(0x00),
                VE(0x08), VE(0x0C), VE(0x10), VE(0x14), VE(0x18), VE(0x20));
        uprintf("  +24 %08x +28 %08x +2c %08x +30 %08x +78 %02x +c0 %08x +c4 %08x\n", VE(0x24), VE(0x28), VE(0x2C),
                VE(0x30), VE8(0x78), VE(0xC0), VE(0xC4));
        uprintf("  frames +40.. %08x %08x %08x %08x %08x\n", VE(0x40), VE(0x44), VE(0x48), VE(0x4C), VE(0x50));
        uprintf("display main picture 0xb8006160: %08x %08x %08x %08x %08x  +174 %08x +178 %08x +17c %08x\n",
                DE(0x160), DE(0x164), DE(0x168), DE(0x16C), DE(0x170), DE(0x174), DE(0x178), DE(0x17C));
        uprintf("  +180 %08x +184 %08x +188 %08x +18c %08x +190 %08x +194 %08x +198 %08x +19c %08x\n", DE(0x180),
                DE(0x184), DE(0x188), DE(0x18C), DE(0x190), DE(0x194), DE(0x198), DE(0x19C));
        uprintf("  scaler +100 %08x %08x %08x %08x  +110 %08x %08x %08x %08x  +134 %08x %08x +144 %08x %08x\n",
                DE(0x100), DE(0x104), DE(0x108), DE(0x10C), DE(0x110), DE(0x114), DE(0x118), DE(0x11C), DE(0x134),
                DE(0x138), DE(0x144), DE(0x148));
        uprintf("  +1c8 %08x +1cc %08x  filters at +1e4 %08x\n", DE(0x1C8), DE(0x1CC), DE(0x1E4));
        uprintf("player: %u pictures decoded in all; last run %u decoded, %u shown; ", pictures_total, st.pictures,
                st.shown);
        if (last_shown < 0)
            uprintf("nothing shown\n");
        else
            uprintf("the frame shown last %d\n", h264 ? last_shown + 1 : last_shown);
        if (h264)
            avc_status();
    }
}
