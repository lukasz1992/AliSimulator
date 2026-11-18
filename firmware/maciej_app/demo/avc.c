/* avc.c: H.264 video on the S3811's own H.264 decoder.

   Where it comes from: ALi's SDK library libcore_hd.a (refs/MainCode/ddk) has the driver with symbols and DWARF
   (vdec_avc*.o: the "DECV_AVC" device; vdec_avc_hal.o writes each register as a bit-field typedef named after
   the H.264 syntax elements it carries).  dump.bin, the Comsat's original firmware, runs a newer build of it (its
   HAL at 0x8034DD5C..0x8034F558 in its RAM image).  Everything here was tried on the Comsat box -- an S3811
   revision 1 -- with test code run from its RAM, every frame compared with ffmpeg's: identical, byte for byte,
   for Baseline / Main / High, CAVLC and CABAC, I / P / B pictures, up to 5 references, B pictures as references,
   temporal and spatial direct prediction, explicit and implicit weighted prediction, the 8x8 transform, four
   slices a picture -- up to the Blender Foundation's Sintel trailer, 1920 x 1080 High at level 5.0.  A 1080p
   picture takes the engine 21-29 ms (24 on average: the trailer plays in real time, 24 frames/s, with av.c
   reading the file and decoding the sound meanwhile), a 720 x 576 one about 6, a 352 x 288 one 1-2.

   What the engine does: everything below the slice header -- CAVLC / CABAC, the predictions, the transforms,
   the deblocking filter, the implicit weights; it skips the emulation prevention bytes itself and its start code
   detector ends a slice.  The CPU parses the NAL units, the parameter sets and the slice headers, keeps the
   decoded picture buffer (the picture order counts, the reference lists and their modification, the marking of
   8.2.5, the output in display order) and gives the engine the syntax elements as register words.  It shares the
   MPEG-2 decoder's block (av.c): the "console" at 0xB8004000 picks the codec, +0 = 1 H.264 (0 MPEG-2), +4 = 0;
   the reset is the MPEG-2 decoder's (0xB8000080 bit 1 pulsed, 0xB8000028 bit 1 set).

   Registers (base 0xB8004200, 32-bit):
     +0x00  the SPS: b0-6 pic_height_in_map_units_minus1, b8-14 pic_width_in_mbs_minus1, b16
            direct_8x8_inference, b17 mb_adaptive_frame_field, b18 frame_mbs_only, b19-20 chroma_format_idc,
            b21-28 level_idc
     +0x04  the PPS: b0 transform_8x8_mode, b1 constrained_intra_pred, b2-3 weighted_bipred_idc, b4
            weighted_pred, b5-9 / b10-14 num_ref_idx_l1 / l0_default_active_minus1, b15 entropy_coding_mode, b31
            redundant_pic_cnt_present
     +0x08  the slice, 1: b0-1 cabac_init_idc, b2 direct_spatial_mv_pred, b3 bottom_field, b4 field_pic, b5 the
            picture's first slice, b8-11 slice_type (0 P, 1 B, 2 I), b12 nal_ref_idc != 0, b16-23 / b24-31 the
            first macroblock's y / x
     +0x0C  the slice, 2: b0-3 / b4-7 slice_beta / alpha_c0_offset_div2, b8-9 disable_deblocking_filter_idc,
            b12 num_ref_idx_active_override, b16-20 / b24-28 the slice's num_ref_idx_l1 / l0_active_minus1
     +0x10  explicit weights: b24-26 chroma_log2_weight_denom, b28-30 luma's
     +0x14  the weight table, an entry a write (0 without one): b0-7 the weight, b8-15 the offset, b16-23 its
            place (list 0: luma i, Cb 32 + 2i, Cr 33 + 2i; list 1: 96 + i, 128 + 2i, 129 + 2i), b31 the weight was
            in the header (else the default); dump.bin writes every third entry -- the luma ones -- first
     +0x18  the scaling matrices: 0x00800000 flat; else two entries a write (b0-7, b8-15, raster order), b16-18
            the list, b20 set at a list's first write (revision 1)
     +0x1C  b0-5 the slice's QP, b8-13 chroma_qp_index_offset, b16-21 second_chroma_qp_index_offset
     +0x30  the input buffer: b8-27 its address, b28-30 set (valid, last, first)
     +0x34  the bit where the slice data starts, counted from the buffer's start in the stream's own (escaped)
            bytes;  +0x38 the bits it may read;  +0x3C the buffer's end (b8-27)
     +0x40  the reference lists, an entry a write: b31, b16-20 the index, b1-5 / b9-13 the frame buffer of list 0
            / 1, b0 / b8 its bottom field
     +0x44  a frame buffer's settings: b31, b8-12 the buffer, b0-2 the item -- then +0x48 its value: item 0 / 1
            the top / bottom field's POC, 2: b0-1 / b4-5 the top / bottom field as a reference (0 short-term, 1
            long-term, 2 not one, 3 absent) and b8-9 the structure (0 frame), 3 / 4 the luma / chroma (512-byte
            aligned), 5 / 6 the top / bottom half of its motion vectors (256-byte aligned)
     +0x4C  the frame buffer being decoded
     +0x50  the "MB col" buffer: the engine keeps a region for every frame buffer number k there, at +0x1000 + k x
            (32 x the picture's macroblocks + 0x3000, rounded up to 256 bytes) -- measured on the box with the
            area filled with a pattern: 0x6200 apart at 352 x 288, 0xFB00 at 720 x 576, 0x42C00 at 1080p; the
            co-located pictures' data for B pictures (dump.bin gives it 0x22000 bytes: its motion vector area
            follows, the rest of the regions land there)
     +0x54  the "MB neighbour" buffer (0x20000 bytes, as dump.bin)
     +0x58  error case;  +0xE8 b0-6 the macroblock hang threshold (20)
     and from revision 1 on (on revision 0 at +0x20.., where the library has them; dump.bin's driver moves them
     for chip 0x10049 and for the 0x10048 -- the S3811 -- whose revision is not 0): +0x80 control (0x6000: the
     emulation prevention and start code detectors, | the width in macroblocks << 16), +0x84 start (0xF), +0x88
     status (b0 finished, b1 error; written back to clear), +0x8C b0-15 the macroblock count (0 at a picture's
     first slice, the rest of the word kept), +0x90 the macroblock reached (b0-6 y, b16-22 x).  Written to the
     old places, nothing happened on the revision-1 box.
   Traps found on the box: the "MB col" regions above -- with dump.bin's 0x22000 bytes and the neighbour buffer
   behind them, a frame buffer whose region reached into it ended its pictures with the error bit (number 0 at
   1080p, at macroblock row 65; number 5 at 352 x 288, at its third row): the buffer has room for every number
   used here (from 1, as when that was found).  A picture is marked (sliding window, MMCO) after its last slice:
   marked after its first, a four-slice picture became its own reference.  And the input registers are written
   once a slice: written twice, the engine timed out.  (The SDK hands its register writes to a command queue in
   RAM that the console runs: +0xC0 = 0x10000000 | its address, +0xC4 the count, +0xC8 = 1; plain writes, as
   here, do the same.)

   A frame: luma macroblock width x ceil(height / 2) x 512 bytes, chroma half as many tiles: tiles one macroblock
   (16 pixels, 8 Cb Cr pairs) wide and 32 lines high, a line 16 bytes, every four lines stored in the order 0, 2,
   1, 3 (the SDK's convert_h264_de2yuv; measured against ffmpeg).  The display engine's main picture layer reads
   this layout with 0xB8006190 bit 1 set ("H.264 mapping") and the width in macroblocks in +0x178's bits 16-23
   (vdec_avc_decore's Display_Info: stride (w + 15) / 16).  Motion vectors: 64 bytes a macroblock per frame.

   RAM: the frames in the picture viewer's work area (IMAGE_WORK, as av.c's MPEG-2 frames) and, for big
   pictures, in the cartridge / picture area (MEDIA) -- never used while a video plays; the motion vectors in
   netload's area and the neighbour buffer where its stub runs (both used only while an image comes in), the
   "MB col" buffer in wget's (8 MB).  1080p takes 3.1 MB a frame, 0.5 MB of motion vectors and 0.27 MB of "MB
   col" region; with 5 references and the player's 4, 12 frames: 46 MB.

   Not here: field pictures and MBAFF (interlaced H.264), 4:2:2 / 4:4:4, more than 8 bits, slice groups, data
   partitioning, SP / SI slices, pictures over 1920 x 1088.  Custom scaling matrices are sent as the SDK sends
   them, not tried on the box. */
#include "hw.h"
#include "av.h"
#include "avc.h"

typedef signed short s16;

#define VE(o)           REG32(0xB8004200u + (o))
#define CON(o)          REG32(0xB8004000u + (o))
#define SYS8(o)         REG8(0xB8000000u + (o))

#define MV_PHYS         NETLOAD_PHYS
#define MV_END          NETLOAD_STUB_PHYS
#define MBCOL_PHYS      WGET_PHYS               /* (8 MB: 'wget -O's, never at the same time) */
#define MBCOL_END       (WGET_PHYS + WGET_MAX)
#define MBNEI_PHYS      NETLOAD_STUB_PHYS
#define MBNEI_SIZE      0x00020000u
#define FRAMES_MAX      30                      /* (the engine's buffer numbers are 5 bits; 0 is not used) */
#define READ_AHEAD      4096u                   /* the bytes after a slice the engine is given: the next start code */
#define TIMEOUT_MS      300

/* the registers that moved on revision 1 */
static u32 r_ctrl, r_start, r_stat, r_mbnum, r_mba;

/* ---- bits: a NAL unit's first bytes, its emulation prevention bytes taken out ---- */
static u8 rbsp[1024];
static u16 gone[1024];                  /* the bytes taken out before rbsp[i] */
static u32 rbsp_n, pos;

static void unescape(const u8 *p, u32 n)
{
    u32 zeros = 0, out = 0, taken = 0;
    for (u32 i = 1; i < n && out < sizeof(rbsp); i++) {
        u8 b = p[i];
        if (zeros >= 2 && b == 3) {
            zeros = 0;
            taken++;
            continue;
        }
        zeros = b ? 0 : zeros + 1;
        gone[out] = (u16)taken;
        rbsp[out++] = b;
    }
    rbsp_n = out;
    pos = 0;
}

static u32 bits(int n)
{
    u32 v = 0;
    for (int i = 0; i < n; i++, pos++)
        v = v << 1 | ((pos >> 3) < rbsp_n ? (rbsp[pos >> 3] >> (7 - (pos & 7))) & 1u : 0);
    return v;
}

static u32 ue(void)
{
    int zeros = 0;
    while (zeros < 31 && !bits(1))
        zeros++;
    return zeros ? ((1u << zeros) - 1) + bits(zeros) : 0;
}

static s32 se(void)
{
    u32 k = ue();
    return k & 1 ? (s32)((k + 1) >> 1) : -(s32)(k >> 1);
}

/* more data before the RBSP's stop bit? (a PPS's optional tail) */
static int more_data(void)
{
    u32 last = rbsp_n;
    while (last && !rbsp[last - 1])
        last--;
    if (!last)
        return 0;
    u32 stop = (last - 1) * 8 + 7;
    for (int k = 0; k < 8; k++)
        if (rbsp[last - 1] & (1u << k)) {
            stop = (last - 1) * 8 + (7 - (u32)k);
            break;
        }
    return pos < stop;
}

/* a * b / c without 64-bit division (the demo links no libgcc) */
static u32 muldiv(u32 a, u32 b, u32 c)
{
    u64 n = (u64)a * b, q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = r << 1 | ((n >> i) & 1);
        if (r >= c) {
            r -= c;
            q |= 1ull << i;
        }
    }
    return q > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)q;
}

/* ---- scaling matrices (7.3.2.1.1.1): kept in raster order, as the SDK's (JM's) are ---- */
static const u8 zz4[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
static const u8 zz8[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21,
    28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61,
    54, 47, 55, 62, 63,
};
/* Table 7-3 / 7-4 (zigzag order) */
static const u8 default4[2][16] = {
    {6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 37, 37, 42},
    {10, 14, 14, 20, 20, 20, 24, 24, 24, 24, 27, 27, 27, 30, 30, 34},
};
static const u8 default8[2][64] = {
    {6, 10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18, 18, 18, 18, 23, 23, 23, 23, 23, 23, 25, 25, 25, 25, 25, 25, 25,
     27, 27, 27, 27, 27, 27, 27, 27, 29, 29, 29, 29, 29, 29, 29, 31, 31, 31, 31, 31, 31, 33, 33, 33, 33, 33, 36, 36,
     36, 36, 38, 38, 38, 40, 40, 42},
    {9, 13, 13, 15, 13, 15, 17, 17, 17, 17, 19, 19, 19, 19, 19, 21, 21, 21, 21, 21, 21, 22, 22, 22, 22, 22, 22, 22,
     24, 24, 24, 24, 24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27, 27, 28, 28, 28, 28, 28, 30, 30,
     30, 30, 32, 32, 32, 33, 33, 35},
};

struct matrices {
    u8 present;                         /* the parameter set has its own (seq_ / pic_scaling_matrix_present) */
    u8 listed[8];                       /* that list is in it (else a fall-back rule) */
    u8 deflt[8];                        /* it said "the default" */
    u8 m4[6][16], m8[2][64];            /* raster order */
};

static void default_list(int i, u8 *out)
{
    if (i < 6)
        for (int k = 0; k < 16; k++)
            out[zz4[k]] = default4[i >= 3][k];
    else
        for (int k = 0; k < 64; k++)
            out[zz8[k]] = default8[i == 7][k];
}

/* `lists` of them (12 for 4:4:4, which is not played: the four more are only read past) */
static void read_matrices(struct matrices *m, int lists)
{
    static u8 spare[64];
    for (int i = 0; i < lists; i++) {
        u8 listed = (u8)bits(1);
        if (i < 8) {
            m->listed[i] = listed;
            m->deflt[i] = 0;
        }
        if (!listed)
            continue;
        int size = i < 6 ? 16 : 64;
        u8 *out = i < 6 ? m->m4[i] : i < 8 ? m->m8[i - 6] : spare;
        const u8 *zz = i < 6 ? zz4 : zz8;
        s32 last = 8, next = 8;
        for (int j = 0; j < size; j++) {
            if (next) {
                next = (last + se() + 256) & 255;
                if (!j && !next) {      /* useDefaultScalingMatrixFlag */
                    if (i < 8) {
                        m->deflt[i] = 1;
                        default_list(i, out);
                    }
                    break;
                }
            }
            out[zz[j]] = (u8)(next ? next : last);
            last = out[zz[j]];
        }
    }
    for (int i = lists; i < 8; i++)
        m->listed[i] = m->deflt[i] = 0;
}

/* ---- the parameter sets ---- */
struct sps {
    u8 valid, profile, level, chroma, deep, planes, frame_mbs_only, mbaff, direct8x8, poc_type, log2_frame_num,
       log2_poc_lsb, delta_always_zero, refs, cycle_n, has_reorder, reorder;
    u16 mbw, mbh_units, crop_r, crop_b, crop_l, crop_t, sar_w, sar_h;
    s32 off_non_ref, off_top_bottom, delta_per_cycle;
    s16 cycle[256];
    u32 units, scale;                   /* VUI timing: num_units_in_tick, time_scale (0: none) */
    struct matrices mat;
};
struct pps {
    u8 valid, sps_id, cabac, poc_present, l0, l1, wpred, wbipred, deblock_ctl, constrained_intra, redundant, t8x8,
       groups;
    s32 qp, chroma_qp, chroma_qp2;
    struct matrices mat;
};
static struct sps sps_set[32];
static struct pps pps_set[64];

static const u8 sar_table[17][2] = {
    {0, 0}, {1, 1}, {12, 11}, {10, 11}, {16, 11}, {40, 33}, {24, 11}, {20, 11}, {32, 11}, {80, 33}, {18, 11},
    {15, 11}, {64, 33}, {160, 99}, {4, 3}, {3, 2}, {2, 1},
};

static void skip_hrd(void)
{
    u32 n = ue() + 1;
    (void)bits(8);
    for (u32 i = 0; i < n && i < 32; i++) {
        (void)ue();
        (void)ue();
        (void)bits(1);
    }
    (void)bits(20);
}

static void parse_sps(void)
{
    u8 profile = (u8)bits(8);
    (void)bits(8);
    u8 level = (u8)bits(8);
    u32 id = ue();
    if (id >= 32)
        return;
    struct sps *s = &sps_set[id];
    s->valid = 0;
    s->profile = profile;
    s->level = level;
    s->chroma = 1;
    s->deep = s->planes = 0;
    s->mat.present = 0;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 ||
        profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134) {
        s->chroma = (u8)ue();
        if (s->chroma == 3)
            s->planes = (u8)bits(1);
        s->deep = (u8)ue();             /* bit_depth_luma_minus8, then chroma's */
        s->deep = (u8)(s->deep | ue());
        (void)bits(1);
        s->mat.present = (u8)bits(1);
        if (s->mat.present)
            read_matrices(&s->mat, s->chroma == 3 ? 12 : 8);
    }
    s->log2_frame_num = (u8)(ue() + 4);
    s->poc_type = (u8)ue();
    if (s->poc_type == 0)
        s->log2_poc_lsb = (u8)(ue() + 4);
    else if (s->poc_type == 1) {
        s->delta_always_zero = (u8)bits(1);
        s->off_non_ref = se();
        s->off_top_bottom = se();
        u32 n = ue();
        s->cycle_n = (u8)(n > 255 ? 255 : n);
        s->delta_per_cycle = 0;
        for (u32 i = 0; i < n; i++) {
            s32 v = se();
            if (i < 255) {
                s->cycle[i] = (s16)v;
                s->delta_per_cycle += v;
            }
        }
    }
    s->refs = (u8)ue();
    (void)bits(1);                      /* gaps_in_frame_num_value_allowed */
    s->mbw = (u16)(ue() + 1);
    s->mbh_units = (u16)(ue() + 1);
    s->frame_mbs_only = (u8)bits(1);
    s->mbaff = s->frame_mbs_only ? 0 : (u8)bits(1);
    s->direct8x8 = (u8)bits(1);
    s->crop_l = s->crop_r = s->crop_t = s->crop_b = 0;
    if (bits(1)) {
        s->crop_l = (u16)ue();
        s->crop_r = (u16)ue();
        s->crop_t = (u16)ue();
        s->crop_b = (u16)ue();
    }
    s->sar_w = s->sar_h = 1;
    s->units = s->scale = 0;
    s->has_reorder = 0;
    if (bits(1)) {                      /* VUI */
        if (bits(1)) {
            u32 idc = bits(8);
            if (idc == 255) {
                s->sar_w = (u16)bits(16);
                s->sar_h = (u16)bits(16);
            } else if (idc >= 1 && idc <= 16) {
                s->sar_w = sar_table[idc][0];
                s->sar_h = sar_table[idc][1];
            }
            if (!s->sar_w || !s->sar_h)
                s->sar_w = s->sar_h = 1;
        }
        if (bits(1))
            (void)bits(1);              /* overscan */
        if (bits(1)) {                  /* video signal type */
            (void)bits(4);
            if (bits(1))
                (void)bits(24);
        }
        if (bits(1)) {                  /* chroma sample location */
            (void)ue();
            (void)ue();
        }
        if (bits(1)) {                  /* timing */
            s->units = bits(32);
            s->scale = bits(32);
            (void)bits(1);
        }
        u32 nal_hrd = bits(1);
        if (nal_hrd)
            skip_hrd();
        u32 vcl_hrd = bits(1);
        if (vcl_hrd)
            skip_hrd();
        if (nal_hrd || vcl_hrd)
            (void)bits(1);
        (void)bits(1);                  /* pic_struct_present */
        if (bits(1)) {                  /* bitstream restriction */
            (void)bits(1);
            (void)ue();
            (void)ue();
            (void)ue();
            (void)ue();
            u32 reorder = ue();
            s->reorder = (u8)(reorder > 16 ? 16 : reorder);
            s->has_reorder = 1;
            (void)ue();                 /* max_dec_frame_buffering */
        }
    }
    s->valid = 1;
}

static void parse_pps(void)
{
    u32 id = ue();
    if (id >= 64)
        return;
    struct pps *p = &pps_set[id];
    p->valid = 0;
    u32 sid = ue();
    if (sid >= 32)
        return;
    p->sps_id = (u8)sid;
    p->cabac = (u8)bits(1);
    p->poc_present = (u8)bits(1);
    p->groups = (u8)ue();
    if (p->groups) {
        p->valid = 1;                   /* (slice groups: refused at the first slice; the PPS's rest depends on
                                           them, not read) */
        return;
    }
    p->l0 = (u8)ue();
    p->l1 = (u8)ue();
    p->wpred = (u8)bits(1);
    p->wbipred = (u8)bits(2);
    p->qp = 26 + se();
    (void)se();
    p->chroma_qp = se();
    p->deblock_ctl = (u8)bits(1);
    p->constrained_intra = (u8)bits(1);
    p->redundant = (u8)bits(1);
    p->chroma_qp2 = p->chroma_qp;
    p->t8x8 = 0;
    p->mat.present = 0;
    if (more_data()) {
        p->t8x8 = (u8)bits(1);
        p->mat.present = (u8)bits(1);
        if (p->mat.present)
            read_matrices(&p->mat, 6 + 2 * p->t8x8);
        p->chroma_qp2 = se();
    }
    p->valid = p->l0 < 32 && p->l1 < 32;
}

/* the matrices a slice is decoded with (JM's AssignQuantParam): the SPS's lists, a list it leaves out by rule A
   (the default for the first intra / inter list, else the list before); then the PPS's, a list it leaves out by
   rule B (the first ones the SPS's, the others the list before).  1: all flat */
static u8 q4[6][16], q8[2][64];

static void take_list(int i, const struct matrices *m, int rule_b, int sps_has)
{
    u8 *out = i < 6 ? q4[i] : q8[i - 6];
    int size = i < 6 ? 16 : 64;
    const u8 *from;
    if (m->listed[i])
        from = i < 6 ? m->m4[i] : m->m8[i - 6];
    else if (i == 0 || i == 3 || i == 6 || i == 7) {
        if (rule_b && sps_has)
            return;                     /* (rule B: the SPS's, set already) */
        default_list(i, out);
        return;
    } else
        from = i < 6 ? q4[i - 1] : q8[i - 8];
    for (int k = 0; k < size; k++)
        out[k] = from[k];
}

static int assign_matrices(const struct sps *s, const struct pps *p)
{
    if (!s->mat.present && !p->mat.present)
        return 1;
    if (s->mat.present)
        for (int i = 0; i < 8; i++)
            take_list(i, &s->mat, 0, 0);
    if (p->mat.present)
        for (int i = 0; i < 8; i++)
            take_list(i, &p->mat, 1, s->mat.present);
    return 0;
}

/* ---- frames ---- */
struct frame {
    u8 ref;                             /* 0 not a reference, 1 short-term, 2 long-term */
    u8 waiting;                         /* decoded, not yet out in display order */
    u8 held;                            /* out: the player shows it or will */
    u8 type;                            /* its first slice's: 0 P, 1 B, 2 I */
    s32 poc;
    u32 frame_num, long_idx, number;
    s32 wrap;                           /* FrameNumWrap (frames: PicNum) */
    u32 y, c, mv;                       /* physical */
};
static struct frame fr[FRAMES_MAX];
static int nfr;
static u32 ysize, csize, mvsize;
static u32 col_stride;                  /* a frame buffer's region in the "MB col" buffer */
static u16 setup_w, setup_h;            /* the frames' size in macroblocks as they were set up */
static int outq[FRAMES_MAX], outq_n;    /* out, not yet taken by avc_out */
static u32 reorder;                     /* pictures kept back for the display order */
static s32 last_out_poc;
static int out_since_reset;             /* (an IDR / MMCO 5 starts the order over) */

static struct avc_info info;
static struct avc_stats stats;

/* ---- the slice header ---- */
#define MMCO_MAX 32
struct slice_header {
    u32 first_mb, type, pps_id, frame_num, idr, poc_lsb, cabac_init, deblock, direct_spatial, override, redundant;
    s32 delta_bottom, delta[2], qp, alpha, beta;
    u32 active[2];                      /* num_ref_idx_active (not minus 1) */
    u32 mod_n[2];
    struct {
        u8 op;
        u32 value;
    } mod[2][34];
    u32 wp, luma_denom, chroma_denom;
    struct {
        u8 luma, chroma;
        s16 lw, lo, cw[2], co[2];
    } w[2][32];
    u32 long_term_flag, adaptive, mmco_n, mmco5;
    struct {
        u8 op;
        u32 a, b;
    } mmco[MMCO_MAX];
    u32 data_bit;                       /* where its data starts: bits of the RBSP */
};
static struct slice_header sh, pic_sh; /* this slice's; the picture's first slice's (its marking) */
static struct sps *cur_sps;
static struct pps *cur_pps;
static int cur = -1;                   /* the frame being decoded */
static u32 pic_ref;                    /* its nal_ref_idc */
static int started;                    /* an I picture seen: P and B pictures before it are skipped */
static int skipping;                   /* the slices of a skipped picture */
static int answered;                   /* the engine has finished a slice */
static int failed;
static int verbose;                    /* 'play -v': every slice told */
static int describe;                   /* the stream described in a line when it is set up (not 'play -q') */
static u32 file_frame_us;              /* a frame's time as the file says it (an .mp4's track), 0 not said */
static void (*idle_work)(void);        /* the player's, while the engine decodes */
static u32 pic_us;

static int parse_slice(u32 nal_type, u32 ref_idc)
{
    sh.first_mb = ue();
    sh.type = ue() % 5;                 /* 0 P, 1 B, 2 I, 3 SP, 4 SI */
    sh.pps_id = ue();
    if (sh.pps_id >= 64 || !pps_set[sh.pps_id].valid || !sps_set[pps_set[sh.pps_id].sps_id].valid)
        return -1;                      /* (no parameter sets yet: a stream joined in the middle) */
    const struct pps *p = &pps_set[sh.pps_id];
    const struct sps *s = &sps_set[p->sps_id];
    if (sh.type > 2)
        return -2;
    sh.frame_num = bits(s->log2_frame_num);
    if (!s->frame_mbs_only && bits(1))
        return -3;                      /* a field picture */
    sh.idr = nal_type == 5;
    if (sh.idr)
        (void)ue();
    sh.poc_lsb = 0;
    sh.delta_bottom = sh.delta[0] = sh.delta[1] = 0;
    if (s->poc_type == 0) {
        sh.poc_lsb = bits(s->log2_poc_lsb);
        if (p->poc_present)
            sh.delta_bottom = se();
    } else if (s->poc_type == 1 && !s->delta_always_zero) {
        sh.delta[0] = se();
        if (p->poc_present)
            sh.delta[1] = se();
    }
    sh.redundant = p->redundant ? ue() : 0;
    sh.direct_spatial = sh.type == 1 ? bits(1) : 0;
    sh.active[0] = p->l0 + 1u;
    sh.active[1] = p->l1 + 1u;
    sh.override = 0;
    if (sh.type != 2) {
        sh.override = bits(1);
        if (sh.override) {
            sh.active[0] = ue() + 1;
            if (sh.type == 1)
                sh.active[1] = ue() + 1;
        }
    }
    if (sh.type == 2)
        sh.active[0] = sh.active[1] = 0;
    else if (sh.type == 0)
        sh.active[1] = 0;
    if (sh.active[0] > 32 || sh.active[1] > 32)
        return -4;
    sh.mod_n[0] = sh.mod_n[1] = 0;
    for (int l = 0; l < (sh.type == 1 ? 2 : sh.type == 0 ? 1 : 0); l++)
        if (bits(1))
            for (int guard = 0; guard < 64; guard++) {
                u32 op = ue();
                if (op >= 3)
                    break;
                u32 v = ue();
                if (sh.mod_n[l] < 34) {
                    sh.mod[l][sh.mod_n[l]].op = (u8)op;
                    sh.mod[l][sh.mod_n[l]++].value = v;
                }
            }
    sh.wp = 0;
    if ((p->wpred && sh.type == 0) || (p->wbipred == 1 && sh.type == 1)) {
        sh.wp = 1;
        sh.luma_denom = ue() & 7;
        sh.chroma_denom = s->chroma ? ue() & 7 : 0;
        for (int l = 0; l < (sh.type == 1 ? 2 : 1); l++)
            for (u32 i = 0; i < sh.active[l]; i++) {
                sh.w[l][i].luma = (u8)bits(1);
                sh.w[l][i].lw = (s16)(1 << sh.luma_denom);
                sh.w[l][i].lo = 0;
                if (sh.w[l][i].luma) {
                    sh.w[l][i].lw = (s16)se();
                    sh.w[l][i].lo = (s16)se();
                }
                sh.w[l][i].chroma = s->chroma ? (u8)bits(1) : 0;
                for (int c = 0; c < 2; c++) {
                    sh.w[l][i].cw[c] = (s16)(1 << sh.chroma_denom);
                    sh.w[l][i].co[c] = 0;
                    if (sh.w[l][i].chroma) {
                        sh.w[l][i].cw[c] = (s16)se();
                        sh.w[l][i].co[c] = (s16)se();
                    }
                }
            }
    }
    sh.long_term_flag = sh.adaptive = sh.mmco_n = sh.mmco5 = 0;
    if (ref_idc) {
        if (sh.idr) {
            (void)bits(1);              /* no_output_of_prior_pics (the pictures before go out anyway) */
            sh.long_term_flag = bits(1);
        } else {
            sh.adaptive = bits(1);
            if (sh.adaptive)
                for (int guard = 0; guard < 66; guard++) {
                    u32 op = ue(), a = 0, b = 0;
                    if (!op || op > 6)
                        break;
                    if (op == 1 || op == 3 || op == 2 || op == 4)
                        a = ue();
                    if (op == 3 || op == 6)
                        b = ue();
                    if (op == 5)
                        sh.mmco5 = 1;
                    if (sh.mmco_n < MMCO_MAX) {
                        sh.mmco[sh.mmco_n].op = (u8)op;
                        sh.mmco[sh.mmco_n].a = a;
                        sh.mmco[sh.mmco_n++].b = b;
                    }
                }
        }
    }
    sh.cabac_init = p->cabac && sh.type != 2 ? ue() : 0;
    sh.qp = p->qp + se();
    sh.deblock = 0;
    sh.alpha = sh.beta = 0;
    if (p->deblock_ctl) {
        sh.deblock = ue();
        if (sh.deblock != 1) {
            sh.alpha = se();
            sh.beta = se();
        }
    }
    sh.data_bit = pos;
    return pos > rbsp_n * 8 ? -5 : 0;
}

/* ---- the picture order count (8.2.1) ---- */
static s32 prev_msb, prev_lsb, prev_offset;
static u32 prev_frame_num;
static int prev_mmco5;

static s32 picture_order(const struct sps *s, u32 ref_idc)
{
    if (s->poc_type == 0) {
        s32 max_lsb = 1 << s->log2_poc_lsb, lsb = (s32)sh.poc_lsb, msb;
        if (sh.idr || prev_mmco5)
            prev_msb = prev_lsb = 0;
        if (lsb < prev_lsb && prev_lsb - lsb >= max_lsb / 2)
            msb = prev_msb + max_lsb;
        else if (lsb > prev_lsb && lsb - prev_lsb > max_lsb / 2)
            msb = prev_msb - max_lsb;
        else
            msb = prev_msb;
        if (ref_idc) {
            prev_msb = msb;
            prev_lsb = lsb;
        }
        s32 top = msb + lsb, bottom = top + sh.delta_bottom;
        return top < bottom ? top : bottom;
    }
    s32 max_fn = 1 << s->log2_frame_num, offset;
    if (sh.idr)
        offset = 0;                     /* (after an MMCO 5 prev_offset and prev_frame_num are 0: finish_picture) */
    else if (prev_frame_num > sh.frame_num)
        offset = prev_offset + max_fn;
    else
        offset = prev_offset;
    prev_offset = offset;
    prev_frame_num = sh.frame_num;
    if (s->poc_type == 2) {
        if (sh.idr)
            return 0;
        s32 t = 2 * (offset + (s32)sh.frame_num);
        return ref_idc ? t : t - 1;
    }
    s32 abs = s->cycle_n ? offset + (s32)sh.frame_num : 0, expected = 0;
    if (!ref_idc && abs > 0)
        abs--;
    if (abs > 0) {
        s32 cycle = (abs - 1) / s->cycle_n, in = (abs - 1) % s->cycle_n;
        expected = cycle * s->delta_per_cycle;
        for (s32 i = 0; i <= in; i++)
            expected += s->cycle[i];
    }
    if (!ref_idc)
        expected += s->off_non_ref;
    s32 top = expected + sh.delta[0], bottom = top + s->off_top_bottom + sh.delta[1];
    return top < bottom ? top : bottom;
}

/* ---- the reference lists (8.2.4) ---- */
static int list[2][34];
static u32 list_n[2];

static s32 wrap_of(int i, u32 frame_num, u32 max_fn)
{
    return fr[i].frame_num > frame_num ? (s32)fr[i].frame_num - (s32)max_fn : (s32)fr[i].frame_num;
}

/* by 0: FrameNumWrap, 1: POC, 2: LongTermFrameIdx */
static void sort(int *a, u32 n, int by, int descending)
{
    for (u32 i = 1; i < n; i++)
        for (u32 k = i; k > 0; k--) {
            s32 x = by == 1 ? fr[a[k - 1]].poc : by == 2 ? (s32)fr[a[k - 1]].long_idx : fr[a[k - 1]].wrap;
            s32 y = by == 1 ? fr[a[k]].poc : by == 2 ? (s32)fr[a[k]].long_idx : fr[a[k]].wrap;
            if (descending ? x >= y : x <= y)
                break;
            int t = a[k];
            a[k] = a[k - 1];
            a[k - 1] = t;
        }
}

static void build_lists(s32 poc)
{
    u32 max_fn = 1u << cur_sps->log2_frame_num;
    int st[FRAMES_MAX], lt[FRAMES_MAX];
    u32 nst = 0, nlt = 0;
    for (int i = 0; i < nfr; i++) {
        if (i == cur)
            continue;
        if (fr[i].ref == 1) {
            fr[i].wrap = wrap_of(i, sh.frame_num, max_fn);
            st[nst++] = i;
        } else if (fr[i].ref == 2)
            lt[nlt++] = i;
    }
    sort(lt, nlt, 2, 0);
    list_n[0] = list_n[1] = 0;
    if (sh.type == 0) {
        sort(st, nst, 0, 1);
        for (u32 i = 0; i < nst; i++)
            list[0][list_n[0]++] = st[i];
        for (u32 i = 0; i < nlt; i++)
            list[0][list_n[0]++] = lt[i];
    } else if (sh.type == 1) {
        int before[FRAMES_MAX], after[FRAMES_MAX];
        u32 nb = 0, na = 0;
        for (u32 i = 0; i < nst; i++) {
            if (fr[st[i]].poc < poc)
                before[nb++] = st[i];
            else
                after[na++] = st[i];
        }
        sort(before, nb, 1, 1);
        sort(after, na, 1, 0);
        for (u32 i = 0; i < nb; i++)
            list[0][list_n[0]++] = before[i];
        for (u32 i = 0; i < na; i++)
            list[0][list_n[0]++] = after[i];
        for (u32 i = 0; i < na; i++)
            list[1][list_n[1]++] = after[i];
        for (u32 i = 0; i < nb; i++)
            list[1][list_n[1]++] = before[i];
        for (u32 i = 0; i < nlt; i++) {
            list[0][list_n[0]++] = lt[i];
            list[1][list_n[1]++] = lt[i];
        }
        int same = list_n[1] > 1 && list_n[0] == list_n[1];
        for (u32 i = 0; same && i < list_n[1]; i++)
            same = list[0][i] == list[1][i];
        if (same) {
            int t = list[1][0];
            list[1][0] = list[1][1];
            list[1][1] = t;
        }
    }
    /* to the active size (filled up with the first entry), then the modifications (8.2.4.3) */
    for (int l = 0; l < 2; l++) {
        if (!sh.active[l]) {
            list_n[l] = 0;
            continue;
        }
        u32 have = list_n[l];
        for (u32 i = have; i < sh.active[l]; i++)
            list[l][i] = have ? list[l][0] : -1;
        list_n[l] = sh.active[l];
        s32 pred = (s32)sh.frame_num;
        u32 at = 0;
        for (u32 m = 0; m < sh.mod_n[l] && at < list_n[l]; m++) {
            int found = -1;
            if (sh.mod[l][m].op < 2) {
                s32 diff = (s32)sh.mod[l][m].value + 1;
                pred = sh.mod[l][m].op == 0 ? pred - diff : pred + diff;
                if (pred < 0)
                    pred += (s32)max_fn;
                if (pred >= (s32)max_fn)
                    pred -= (s32)max_fn;
                s32 num = pred > (s32)sh.frame_num ? pred - (s32)max_fn : pred;
                for (int i = 0; i < nfr; i++)
                    if (i != cur && fr[i].ref == 1 && fr[i].wrap == num)
                        found = i;
            } else {
                for (int i = 0; i < nfr; i++)
                    if (i != cur && fr[i].ref == 2 && fr[i].long_idx == sh.mod[l][m].value)
                        found = i;
            }
            if (found < 0)
                continue;
            int tmp[35];
            u32 n = 0;
            for (u32 i = 0; i < at; i++)
                tmp[n++] = list[l][i];
            tmp[n++] = found;
            for (u32 i = at; i < list_n[l]; i++)
                if (list[l][i] != found)
                    tmp[n++] = list[l][i];
            for (u32 i = 0; i < list_n[l]; i++)
                list[l][i] = tmp[i];
            at++;
        }
    }
}

/* ---- the marking after a reference picture (8.2.5), with its first slice's header ---- */
static void mark(const struct slice_header *h)
{
    u32 max_fn = 1u << cur_sps->log2_frame_num;
    if (h->idr) {
        for (int i = 0; i < nfr; i++)
            if (i != cur)
                fr[i].ref = 0;
        fr[cur].ref = h->long_term_flag ? 2 : 1;
        fr[cur].long_idx = 0;
        return;
    }
    fr[cur].ref = 1;
    if (!h->adaptive) {                 /* the sliding window */
        u32 n = 0;
        int oldest = -1;
        for (int i = 0; i < nfr; i++)
            if (i != cur && fr[i].ref) {
                n++;
                if (fr[i].ref == 1) {
                    fr[i].wrap = wrap_of(i, h->frame_num, max_fn);
                    if (oldest < 0 || fr[i].wrap < fr[oldest].wrap)
                        oldest = i;
                }
            }
        if (n >= (cur_sps->refs ? cur_sps->refs : 1u) && oldest >= 0)
            fr[oldest].ref = 0;
        return;
    }
    for (u32 m = 0; m < h->mmco_n; m++) {
        u32 op = h->mmco[m].op;
        s32 num = (s32)h->frame_num - ((s32)h->mmco[m].a + 1);
        for (int i = 0; i < nfr; i++) {
            if (i == cur)
                continue;
            if (fr[i].ref == 1)
                fr[i].wrap = wrap_of(i, h->frame_num, max_fn);
            if ((op == 1 && fr[i].ref == 1 && fr[i].wrap == num) ||
                (op == 2 && fr[i].ref == 2 && fr[i].long_idx == h->mmco[m].a) ||
                ((op == 3 || op == 6) && fr[i].ref == 2 && fr[i].long_idx == h->mmco[m].b) ||
                (op == 4 && fr[i].ref == 2 && fr[i].long_idx + 1 > h->mmco[m].a) || op == 5)
                fr[i].ref = 0;
        }
        if (op == 3)
            for (int i = 0; i < nfr; i++)
                if (i != cur && fr[i].ref == 1 && fr[i].wrap == num) {
                    fr[i].ref = 2;
                    fr[i].long_idx = h->mmco[m].b;
                }
        if (op == 6) {
            fr[cur].ref = 2;
            fr[cur].long_idx = h->mmco[m].b;
        }
    }
}

/* ---- the output in display order (C.4.5.3's bumping, with the stream's reorder depth) ---- */
static int smallest_waiting(int *count)
{
    int best = -1, n = 0;
    for (int i = 0; i < nfr; i++)
        if (fr[i].waiting) {
            n++;
            if (best < 0 || fr[i].poc < fr[best].poc)
                best = i;
        }
    if (count)
        *count = n;
    return best;
}

static void put_out(int f)
{
    fr[f].waiting = 0;
    fr[f].held = 1;
    last_out_poc = fr[f].poc;
    out_since_reset = 1;
    outq[outq_n++] = f;
}

static void out_all(void)
{
    int f;
    while ((f = smallest_waiting(0)) >= 0)
        put_out(f);
    out_since_reset = 0;
}

/* the picture decoded: marked, waiting for its turn */
static void finish_picture(void)
{
    if (cur < 0)
        return;
    if (pic_ref)
        mark(&pic_sh);
    prev_mmco5 = 0;
    if (pic_sh.mmco5) {                 /* (8.2.1, C.4.4: the pictures before go out first; this one's POC and
                                           frame_num count as 0 from now on) */
        out_all();
        fr[cur].poc = 0;
        fr[cur].frame_num = 0;
        prev_mmco5 = 1;
        prev_offset = 0;
        prev_frame_num = 0;
    }
    /* a picture that should have gone out before one already out: the stream reorders more than it said */
    if (out_since_reset && fr[cur].poc < last_out_poc && reorder < 16)
        reorder++;
    fr[cur].waiting = 1;
    stats.total_us += pic_us;
    if (pic_us > stats.slowest_us)
        stats.slowest_us = pic_us;
    cur = -1;
    int n, f = smallest_waiting(&n);
    if (n > (int)reorder && f >= 0)
        put_out(f);
}

/* ---- the engine ---- */
static void engine_reset(void)
{
    u8 r = SYS8(0x80);
    SYS8(0x80) = (u8)(r | 0x02);
    delay_ms(2);
    SYS8(0x80) = (u8)(r & ~0x02);
    SYS8(0x28) = (u8)(SYS8(0x28) | 0x02);
    CON(0) = 1;                         /* H.264 (H264DecoderEnable) */
    CON(4) = 0;
}

static void fb_item(int f, u32 item, u32 value)
{
    VE(0x44) = 0x80000000u | (u32)(f + 1) << 8 | item;
    VE(0x48) = value;
}

static void fb_send(int f)
{
    u32 kind = f == cur ? (pic_ref ? 0x00u : 0x22u) : fr[f].ref == 2 ? 0x11u : fr[f].ref == 1 ? 0x00u : 0x22u;
    fb_item(f, 3, KSEG1(fr[f].y));
    fb_item(f, 4, KSEG1(fr[f].c));
    fb_item(f, 5, KSEG1(fr[f].mv));
    fb_item(f, 6, KSEG1(fr[f].mv + mvsize / 2));
    fb_item(f, 0, (u32)fr[f].poc);
    fb_item(f, 1, (u32)fr[f].poc);
    fb_item(f, 2, kind);
}

static void send_matrices(void)
{
    if (assign_matrices(cur_sps, cur_pps)) {
        VE(0x18) = 0x00800000u;
        return;
    }
    for (u32 l = 0; l < 8; l++) {
        const u8 *m = l < 6 ? q4[l] : q8[l - 6];
        u32 size = l < 6 ? 16 : 64;
        for (u32 k = 0; k < size; k += 2)
            VE(0x18) = (u32)m[k] | (u32)m[k + 1] << 8 | l << 16 | (k == 0 && chip_rev() ? 0x00100000u : 0);
    }
}

/* one slice (the NAL unit at nal, n bytes, `avail` readable): 0, -1 the engine did not answer */
static int decode_slice(const u8 *nal, u32 n, u32 avail, u32 ref_idc)
{
    const struct sps *s = cur_sps;
    const struct pps *p = cur_pps;
    u32 w_sps = (u32)(s->level & 0xFF) << 21 | (u32)(s->chroma & 3) << 19 | (u32)s->frame_mbs_only << 18 |
                (u32)s->mbaff << 17 | (u32)s->direct8x8 << 16 | ((s->mbw - 1u) & 0x7F) << 8 |
                ((s->mbh_units - 1u) & 0x7F);
    u32 w_pps = (u32)p->cabac << 15 | (u32)(p->l0 & 31) << 10 | (u32)(p->l1 & 31) << 5 | (u32)p->wpred << 4 |
                (u32)p->wbipred << 2 | (u32)p->constrained_intra << 1 | p->t8x8 | (u32)p->redundant << 31;
    u32 mbx = sh.first_mb % s->mbw, mby = sh.first_mb / s->mbw;
    u32 w_shs1 = mbx << 24 | mby << 16 | (ref_idc ? 1u : 0) << 12 | sh.type << 8 | (sh.first_mb == 0) << 5 |
                 sh.direct_spatial << 2 | (sh.cabac_init & 3);
    u32 a0 = sh.active[0] ? sh.active[0] - 1 : 0, a1 = sh.active[1] ? sh.active[1] - 1 : 0;
    u32 w_shs2 = (a0 & 31) << 24 | (a1 & 31) << 16 | sh.override << 12 | (sh.deblock & 3) << 8 |
                 ((u32)sh.alpha & 15) << 4 | ((u32)sh.beta & 15);
    u32 w_qp = ((u32)sh.qp & 63) | ((u32)p->chroma_qp & 63) << 8 | ((u32)p->chroma_qp2 & 63) << 16;
    /* the input: from the 256-byte boundary before the NAL unit, its slice data on to the start code after it */
    u32 phys = (u32)nal & 0x1FFFFFFFu, base = phys & ~0xFFu;
    u32 give = n + READ_AHEAD < avail ? n + READ_AHEAD : avail;
    av_cache_flush(nal, give);
    u32 header = sh.data_bit + 8u * gone[sh.data_bit >> 3 < rbsp_n ? sh.data_bit >> 3 : rbsp_n - 1];
    if (p->cabac && (header & 7))
        header = (header + 7) & ~7u;
    u32 first_bit = (phys + 1 - base) * 8 + header, end = (phys + give + 255) & ~0xFFu;
    VE(r_stat) = 7;
    if (VE(r_stat) & 3)
        return -1;                      /* (a status that does not clear: no H.264 engine here) */
    VE(0x00) = w_sps;
    VE(0x04) = w_pps;
    VE(0x08) = w_shs1;
    VE(0x0C) = w_shs2;
    VE(0x10) = sh.wp ? sh.luma_denom << 28 | sh.chroma_denom << 24 : 0;
    if (sh.wp) {
        /* every third entry -- the luma ones -- first, as dump.bin's H264SetSliceWeight */
        static u32 tab[192];
        u32 k = 0;
        for (u32 l = 0; l < (sh.type == 1 ? 2u : 1u); l++)
            for (u32 i = 0; i < sh.active[l]; i++) {
                tab[k++] = (sh.w[l][i].luma ? 0x80000000u : 0) | (l * 96 + i) << 16 | (u32)(u8)sh.w[l][i].lo << 8 |
                           (u8)sh.w[l][i].lw;
                if (s->chroma)
                    for (u32 c = 0; c < 2; c++)
                        tab[k++] = (sh.w[l][i].chroma ? 0x80000000u : 0) | (32 + l * 96 + 2 * i + c) << 16 |
                                   (u32)(u8)sh.w[l][i].co[c] << 8 | (u8)sh.w[l][i].cw[c];
            }
        for (u32 i = 0; i < k; i += 3)
            VE(0x14) = tab[i];
        for (u32 i = 1; i < k; i++)
            if (i % 3)
                VE(0x14) = tab[i];
        if (!k)
            VE(0x14) = 0;
    } else
        VE(0x14) = 0;
    send_matrices();
    VE(0x1C) = w_qp;
    VE(0xE8) = (VE(0xE8) & ~0x7Fu) | 20;   /* (the macroblock hang threshold, as the SDK at every slice) */
    VE(r_ctrl) = 0x00006000u | (u32)(s->mbw & 0xFF) << 16;
    if (sh.first_mb == 0)
        VE(r_mbnum) = r_mbnum == 0x8C ? VE(0x8C) & 0xFFFF0000u : 0;
    VE(0x50) = KSEG1(MBCOL_PHYS);
    VE(0x54) = KSEG1(MBNEI_PHYS);
    /* the lists: list 1's half names a real reference in a P slice too */
    u32 nmap = list_n[0] > list_n[1] ? list_n[0] : list_n[1];
    int l1 = list_n[1] ? list[1][0] : list_n[0] ? list[0][0] : cur;
    for (u32 i = 0; i < nmap; i++) {
        int e0 = i < list_n[0] ? list[0][i] : 0, e1 = i < list_n[1] ? list[1][i] : l1;
        VE(0x40) = 0x80000000u | i << 16 | (u32)(e0 + 1) << 1 | (u32)(e1 + 1) << 9;
    }
    fb_send(cur);
    for (int i = 0; i < nfr; i++)
        if (i != cur && fr[i].ref)
            fb_send(i);
    VE(0x4C) = (u32)(cur + 1);
    VE(0x34) = first_bit;
    VE(0x38) = (give - 1) * 8 - header;
    VE(0x3C) = end & 0x0FFFFF00u;
    VE(0x30) = (base & 0x0FFFFF00u) | 0x70000000u;
    u64 t0 = now_ticks();
    u32 limit = ticks_per_ms() * TIMEOUT_MS, st = 0;
    VE(r_start) = 0xF;
    /* (the player's work meanwhile: a 1080p picture takes the engine 21-29 ms, the card's reading and the sound
       took 40 % of a 24 frames/s frame's time besides -- done one after the other, 7 frames of the Sintel trailer
       were late) */
    while (!((st = VE(r_stat)) & 3)) {
        if ((u32)(now_ticks() - t0) > limit)
            break;
        if (idle_work)
            idle_work();
    }
    u32 us = (u32)(now_ticks() - t0) / (ticks_per_ms() / 1000u);
    stats.last_status = st;
    stats.last_mb = VE(r_mba);
    stats.slices++;
    pic_us += us;
    VE(r_stat) = 7;
    if (!(st & 3)) {
        stats.timeouts++;
        engine_reset();
        return -1;
    }
    answered = 1;
    if (st & 2)
        stats.errors++;
    if (verbose || ((st & 2) && stats.errors <= 3)) {
        uprintf("picture %u %c%s POC %d slice at MB %u -> frame %d: %u us, status 0x%08x, MB 0x%08x, error case "
                "0x%08x; L0", fr[cur].number, "PBI"[sh.type], ref_idc ? "" : " (no reference)", fr[cur].poc,
                sh.first_mb, cur + 1, us, st, stats.last_mb, VE(0x58));
        for (u32 i = 0; i < list_n[0]; i++)
            uprintf(" %d", fr[list[0][i]].poc);
        uprintf(" L1");
        for (u32 i = 0; i < list_n[1]; i++)
            uprintf(" %d", fr[list[1][i]].poc);
        uprintf("%s\n", sh.wp ? " (explicit weights)" : "");
    }
    return 0;
}

/* ---- frame buffers ---- */
static int frame_free(int i)
{
    return i != cur && !fr[i].ref && !fr[i].waiting && !fr[i].held;
}

/* frames for this stream's size: 0, -1 too big (said) */
static int frames_setup(const struct sps *s)
{
    u32 mbh = s->mbh_units, rows = (mbh + 1) / 2;
    ysize = s->mbw * rows * 512;
    csize = s->mbw * ((rows + 1) / 2) * 512;
    mvsize = (s->mbw * mbh * 64 + 511) & ~511u;
    col_stride = (32u * s->mbw * mbh + 0x3000u + 255u) & ~255u;
    u32 each = ysize + csize, want = (u32)(s->refs ? s->refs : 1) + reorder + 6;
    if (want > FRAMES_MAX)
        want = FRAMES_MAX;
    int n = 0;
    for (u32 a = IMAGE_WORK_PHYS; a + each <= IMAGE_WORK_PHYS + IMAGE_WORK_SIZE && (u32)n < want; a += each)
        fr[n++].y = a;
    for (u32 a = MEDIA_PHYS; a + each <= MEDIA_PHYS + MEDIA_MAX && (u32)n < want; a += each)
        fr[n++].y = a;
    for (int i = 0; i < n; i++) {
        fr[i].c = fr[i].y + ysize;
        fr[i].mv = MV_PHYS + (u32)i * mvsize;
        /* (its region in the "MB col" buffer -- buffer i + 1's, at 0x1000 + (i + 1) x the stride -- fits too) */
        if (fr[i].mv + mvsize > MV_END || MBCOL_PHYS + 0x1000u + (u32)(i + 2) * col_stride > MBCOL_END) {
            n = i;
            break;
        }
        fr[i].ref = fr[i].waiting = fr[i].held = 0;     /* (no frame is shown before it is decoded) */
        fr[i].number = 0;               /* (none decoded into it yet: 'av shot' says so) */
        fr[i].type = 0;
    }
    nfr = n;
    if ((u32)n < (u32)(s->refs ? s->refs : 1) + 4) {
        uprintf("play: H.264 %u x %u with %u reference frames: too big for this player's memory (%d frames)\n",
                s->mbw * 16, mbh * 16, s->refs, n);
        return -1;
    }
    setup_w = s->mbw;
    setup_h = s->mbh_units;
    return 0;
}

/* the stream's SPS in use (its first slice, or a new size): 0, -1 not played (said) */
static int activate(struct sps *s)
{
    const char *why = !s->frame_mbs_only ? "interlaced (field pictures / MBAFF)" :
                      s->chroma != 1 ? "not 4:2:0" : s->deep ? "more than 8 bits" : s->planes ? "separate colour planes" :
                      s->mbw > 120 || s->mbh_units > 68 ? "bigger than 1920 x 1088" : 0;
    if (why) {
        uprintf("play: H.264 %u x %u, %s: not played here\n", s->mbw * 16u, s->mbh_units * 16u, why);
        return -1;
    }
    if (setup_w == s->mbw && setup_h == s->mbh_units && nfr) {
        cur_sps = s;
        return 0;
    }
    out_all();
    cur_sps = s;
    reorder = s->has_reorder ? s->reorder : s->profile == 66 ? 0u : (u32)(s->refs ? s->refs : 1);
    if (frames_setup(s) < 0)
        return -1;
    info.mbw = s->mbw;
    info.mbh = s->mbh_units;
    info.w = (u16)(s->mbw * 16 - 2 * (s->crop_l + s->crop_r));
    info.h = (u16)(s->mbh_units * 16 - 2 * (s->crop_t + s->crop_b));
    if (info.w < 16 || info.h < 16) {
        info.w = (u16)(s->mbw * 16);
        info.h = (u16)(s->mbh_units * 16);
    }
    info.sar_w = s->sar_w;
    info.sar_h = s->sar_h;
    info.frame_us = file_frame_us ? file_frame_us : s->units && s->scale ? muldiv(2 * s->units, 1000000u, s->scale)
                                                                         : 0;
    if (info.frame_us < 5000 || info.frame_us > 200000)
        info.frame_us = 0;              /* (a field rate, or nonsense: the player's default) */
    info.profile = s->profile;
    info.level = s->level;
    info.refs = s->refs;
    info.reorder = (u8)reorder;
    info.frames = (u8)nfr;
    if (describe) {                     /* (before its first slice is decoded: said even if that fails) */
        u32 us = info.frame_us ? info.frame_us : 40000;
        u32 p = info.profile;
        uprintf("video: H.264 %s %u.%u, %u x %u, %u.%02u frames/s%s, pixels %u:%u, %u reference frame%s, %u kept "
                "back for the display order, %u frame buffers\n", p == 66 ? "Baseline" : p == 77 ? "Main" : p == 88 ?
                "Extended" : p == 100 ? "High" : p == 110 ? "High 10" : p == 122 ? "High 4:2:2" : p == 244 ?
                "High 4:4:4" : "profile ?", info.level / 10u, info.level % 10u, info.w, info.h, 100000000u / us / 100,
                100000000u / us % 100, info.frame_us ? "" : " (the stream does not say: 25 taken)", info.sar_w,
                info.sar_h, info.refs, info.refs == 1 ? "" : "s", info.reorder, info.frames);
    }
    return 0;
}

/* a picture's first slice: its frame; AVC_DONE, AVC_FULL */
static int start_picture(u32 ref_idc)
{
    if (sh.idr) {                       /* the pictures before go out first; none is a reference from here on */
        out_all();
        for (int i = 0; i < nfr; i++)
            fr[i].ref = 0;
    }
    int f = -1;
    for (int i = 0; i < nfr && f < 0; i++)
        if (frame_free(i))
            f = i;
    if (f < 0) {
        int w = smallest_waiting(0);   /* (the one that would go out next goes now: the player shows and releases) */
        if (w >= 0)
            put_out(w);
        return AVC_FULL;
    }
    cur = f;
    fr[f].poc = picture_order(cur_sps, ref_idc);
    fr[f].frame_num = sh.frame_num;
    fr[f].type = (u8)sh.type;
    fr[f].number = ++stats.pictures;
    stats.types[sh.type]++;
    fr[f].ref = 0;
    pic_sh = sh;
    pic_ref = ref_idc;
    pic_us = 0;
    return AVC_DONE;
}

static int slice(const u8 *nal, u32 n, u32 avail, u32 type, u32 ref_idc)
{
    unescape(nal, n);
    int r = parse_slice(type, ref_idc);
    if (r == -1)
        return AVC_DONE;                /* (no parameter sets yet) */
    if (r == -2 || r == -3) {
        uprintf("play: H.264 with %s: not played here\n", r == -2 ? "SP / SI slices" : "field pictures");
        failed = 1;
        return AVC_FAIL;
    }
    if (r < 0 || sh.redundant) {
        stats.errors += r < 0;
        return AVC_DONE;                /* (a damaged slice header; a redundant slice) */
    }
    struct pps *p = &pps_set[sh.pps_id];
    struct sps *s = &sps_set[p->sps_id];
    if (p->groups) {
        uprintf("play: H.264 with slice groups (FMO): not played here\n");
        failed = 1;
        return AVC_FAIL;
    }
    if (sh.first_mb >= (u32)s->mbw * s->mbh_units)
        return AVC_DONE;
    if (sh.first_mb == 0) {
        finish_picture();
        skipping = 0;
        if (s != cur_sps || s->mbw != setup_w || s->mbh_units != setup_h) {
            if (activate(s) < 0) {
                failed = 1;
                return AVC_FAIL;
            }
        }
        cur_pps = p;
        if (!started && sh.type != 2) { /* (before the first I picture: nothing to predict from) */
            stats.skipped++;
            skipping = 1;
            return AVC_DONE;
        }
        started = 1;
        int got = start_picture(ref_idc);
        if (got != AVC_DONE)
            return got;
    } else if (cur < 0 || skipping)
        return AVC_DONE;
    cur_pps = p;
    build_lists(fr[cur].poc);
    if ((sh.active[0] && list[0][0] < 0) || (sh.active[1] && list[1][0] < 0)) {
        stats.errors++;                 /* (no reference to predict from: the slice is left out) */
        return AVC_DONE;
    }
    if (decode_slice(nal, n, avail, ref_idc) < 0 && !answered) {
        uprintf("play: the H.264 decoder does not answer (status 0x%08x)\n", VE(r_stat));
        stats.pictures--;               /* (the first picture, begun: not decoded after all) */
        stats.types[sh.type]--;
        fr[cur].number = 0;
        failed = 1;
        return AVC_FAIL;
    }
    return AVC_DONE;
}

/* ---- the interface ---- */
void avc_begin(int say, void (*idle)(void), u32 tick, u32 scale)
{
    int rev1 = chip_rev() != 0;
    verbose = (say & AVC_VERBOSE) != 0;
    describe = (say & AVC_DESCRIBE) != 0;
    file_frame_us = tick && scale ? muldiv(tick, 1000000u, scale) : 0;
    idle_work = idle;
    r_ctrl = rev1 ? 0x80 : 0x20;
    r_start = rev1 ? 0x84 : 0x24;
    r_stat = rev1 ? 0x88 : 0x28;
    r_mbnum = rev1 ? 0x8C : 0x2C;
    r_mba = rev1 ? 0x90 : 0x60;
    for (int i = 0; i < 32; i++)
        sps_set[i].valid = 0;
    for (int i = 0; i < 64; i++)
        pps_set[i].valid = 0;
    for (int i = 0; i < FRAMES_MAX; i++)
        fr[i].ref = fr[i].waiting = fr[i].held = 0;
    nfr = 0;
    setup_w = setup_h = 0;
    cur = -1;
    cur_sps = 0;
    cur_pps = 0;
    outq_n = 0;
    started = skipping = answered = failed = 0;
    out_since_reset = 0;
    prev_msb = prev_lsb = prev_offset = 0;
    prev_frame_num = 0;
    prev_mmco5 = 0;
    u32 *z = (u32 *)&stats;
    for (u32 i = 0; i < sizeof(stats) / 4; i++)
        z[i] = 0;
    z = (u32 *)&info;
    for (u32 i = 0; i < sizeof(info) / 4; i++)
        z[i] = 0;
    engine_reset();
    for (u32 i = 0; i < 0x1000; i += 4)
        REG32(KSEG1(MBCOL_PHYS + i)) = 0;
    for (u32 i = 0; i < MBNEI_SIZE; i += 4)
        REG32(KSEG1(MBNEI_PHYS + i)) = 0;
}

int avc_nal(const u8 *nal, u32 n, u32 avail)
{
    if (failed)
        return AVC_FAIL;
    if (!n || (nal[0] & 0x80))
        return AVC_DONE;
    u32 type = nal[0] & 0x1F, ref = (u32)(nal[0] >> 5) & 3;
    switch (type) {
    case 1:
    case 5:
        return slice(nal, n, avail, type, ref);
    case 2:
    case 3:
    case 4:
        uprintf("play: H.264 with data partitioning (the Extended profile): not played here\n");
        failed = 1;
        return AVC_FAIL;
    case 7:
        finish_picture();
        unescape(nal, n);
        parse_sps();
        return AVC_DONE;
    case 8:
        finish_picture();
        unescape(nal, n);
        parse_pps();
        return AVC_DONE;
    case 6:
    case 9:
    case 10:
    case 11:
        finish_picture();               /* (an SEI, an access unit delimiter, an end: a new access unit) */
        return AVC_DONE;
    default:
        return AVC_DONE;
    }
}

void avc_end(void)
{
    finish_picture();
    out_all();
}

int avc_out(void)
{
    if (!outq_n)
        return -1;
    int f = outq[0];
    for (int i = 1; i < outq_n; i++)
        outq[i - 1] = outq[i];
    outq_n--;
    return f;
}

void avc_release(int f)
{
    if (f >= 0 && f < nfr)
        fr[f].held = 0;
}

u32 avc_luma(int f)
{
    return fr[f].y;
}

u32 avc_chroma(int f)
{
    return fr[f].c;
}

u32 avc_number(int f)
{
    return fr[f].number;
}

char avc_type(int f)
{
    return "PBI"[fr[f].type % 3];
}

const struct avc_info *avc_info(void)
{
    return &info;
}

const struct avc_stats *avc_stats(void)
{
    return &stats;
}

void avc_status(void)
{
    uprintf("H.264 decoder 0xb8004200 (console +0 %u): status 0x%08x, macroblock 0x%08x, count 0x%08x, error case "
            "0x%08x, control 0x%08x\n", CON(0), VE(r_stat), VE(r_mba), VE(r_mbnum), VE(0x58), VE(r_ctrl));
    uprintf("  last stream: %u x %u (%u x %u macroblocks), %u frame buffers, %u pictures decoded (%u slices, %u with "
            "the error bit, %u timed out)\n", info.w, info.h, info.mbw, info.mbh, info.frames, stats.pictures,
            stats.slices, stats.errors, stats.timeouts);
    for (int i = 0; i < nfr; i++)
        if (!fr[i].number)
            uprintf("  frame %2d at 0x%08x: not used\n", i + 1, fr[i].y);
        else
            uprintf("  frame %2d at 0x%08x: picture %u (%c), POC %d%s%s%s\n", i + 1, fr[i].y, fr[i].number,
                    avc_type(i), fr[i].poc, fr[i].ref == 2 ? ", long-term reference" : fr[i].ref ? ", reference" : "",
                    fr[i].waiting ? ", waiting" : "", fr[i].held ? ", out" : "");
}
