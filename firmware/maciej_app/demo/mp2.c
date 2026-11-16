/* mp2.c: MPEG audio Layer II decoding (ISO 11172-3 and 13818-3's low sampling frequencies), integers only:
   the frame's bit allocation, scale factors and samples requantised (Q28), the two channels mixed, then the
   standard's polyphase synthesis (matrixing into the 1024-sample V buffer, the 512-tap window D) for 32
   samples at a time.  See mp2.h. */
#include "mp2.h"

/* ---- tables (ISO 11172-3 Annex B) ---- */

/* bit rates (kbit/s) by bitrate_index: MPEG-1 Layer II, MPEG-2 low sampling frequencies (Layers II and III) */
static const u16 rates_mpeg1[15] = {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384};
static const u16 rates_lsf[15] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
static const u16 freqs[3] = {44100, 48000, 32000};

/* the quantisation classes: levels, grouped (three samples in one codeword), the codeword's bits */
static const struct {
    u16 levels;
    u8 grouped, bits;
} classes[18] = {
    {0, 0, 0}, {3, 1, 5}, {5, 1, 7}, {7, 0, 3}, {9, 1, 10}, {15, 0, 4}, {31, 0, 5}, {63, 0, 6}, {127, 0, 7},
    {255, 0, 8}, {511, 0, 9}, {1023, 0, 10}, {2047, 0, 11}, {4095, 0, 12}, {8191, 0, 13}, {16383, 0, 14},
    {32767, 0, 15}, {65535, 0, 16},
};

/* the allocation tables 3-B.2a..d and 13818-3's B.1: per subband its allocation field's bits (high nibble)
   and which row of `classes_of` turns the field into a class (low nibble) */
static const u8 alloc_low[12] = {0x44, 0x44, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34};
static const u8 alloc_high[30] = {0x43, 0x43, 0x43, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x31, 0x31,
                                  0x31, 0x31, 0x31, 0x31, 0x31, 0x31, 0x31, 0x31, 0x31, 0x31, 0x20, 0x20, 0x20,
                                  0x20, 0x20, 0x20, 0x20};
static const u8 alloc_lsf[30] = {0x45, 0x45, 0x45, 0x45, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34, 0x34, 0x24, 0x24,
                                 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24, 0x24,
                                 0x24, 0x24, 0x24, 0x24};
static const u8 classes_of[6][16] = {
    {0, 1, 2, 17},
    {0, 1, 2, 3, 4, 5, 6, 17},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 17},
    {0, 1, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17},
    {0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16},
    {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
};

/* the synthesis window D[0..256] x 65536 (Table 3-B.3; D[512 - i] = -D[i], or +D[i] where i is a multiple of
   64) */
static const s32 window_half[257] = {
    0, -1, -1, -1, -1, -1, -1, -2, -2, -2,
    -2, -3, -3, -4, -4, -5, -5, -6, -7, -7,
    -8, -9, -10, -11, -13, -14, -16, -17, -19, -21,
    -24, -26, -29, -31, -35, -38, -41, -45, -49, -53,
    -58, -63, -68, -73, -79, -85, -91, -97, -104, -111,
    -117, -125, -132, -139, -147, -154, -161, -169, -176, -183,
    -190, -196, -202, -208, 213, 218, 222, 225, 227, 228,
    228, 227, 224, 221, 215, 208, 200, 189, 177, 163,
    146, 127, 106, 83, 57, 29, -2, -36, -72, -111,
    -153, -197, -244, -294, -347, -401, -459, -519, -581, -645,
    -711, -779, -848, -919, -991, -1064, -1137, -1210, -1283, -1356,
    -1428, -1498, -1567, -1634, -1698, -1759, -1817, -1870, -1919, -1962,
    -2001, -2032, -2057, -2075, -2085, -2087, -2080, -2063, 2037, 2000,
    1952, 1893, 1822, 1739, 1644, 1535, 1414, 1280, 1131, 970,
    794, 605, 402, 185, -45, -288, -545, -814, -1095, -1388,
    -1692, -2006, -2330, -2663, -3004, -3351, -3705, -4063, -4425, -4788,
    -5153, -5517, -5879, -6237, -6589, -6935, -7271, -7597, -7910, -8209,
    -8491, -8755, -8998, -9219, -9416, -9585, -9727, -9838, -9916, -9959,
    -9966, -9935, -9863, -9750, -9592, -9389, -9139, -8840, -8492, -8092,
    -7640, -7134, 6574, 5959, 5288, 4561, 3776, 2935, 2037, 1082,
    70, -998, -2122, -3300, -4533, -5818, -7154, -8540, -9975, -11455,
    -12980, -14548, -16155, -17799, -19478, -21189, -22929, -24694, -26482, -28289,
    -30112, -31947, -33791, -35640, -37489, -39336, -41176, -43006, -44821, -46617,
    -48390, -50137, -51853, -53534, -55178, -56778, -58333, -59838, -61289, -62684,
    -64019, -65290, -66494, -67629, -68692, -69679, -70590, -71420, -72169, -72835,
    -73415, -73908, -74313, -74630, -74856, -74992, 75038,
};

/* cos(j * pi / 64) x 32767, j = 0..32 (the matrixing's cosines come from these) */
static const s16 cos_quarter[33] = {
    32767, 32728, 32609, 32412, 32137, 31785, 31356, 30852, 30273, 29621, 28898, 28105, 27245, 26319, 25329,
    24279, 23170, 22005, 20787, 19519, 18204, 16846, 15446, 14010, 12539, 11039, 9512, 7962, 6393, 4808, 3212,
    1608, 0,
};

/* 2^(-k/3) x 2^30, k = 0..2: a scale factor index i is 2^(1 - i/3) = base[i % 3] >> (i / 3) in Q29 */
static const u32 sf_base[3] = {1073741824u, 852229450u, 676414963u};

/* ---- state ---- */
static s32 window[512];                 /* D[i] x 65536 */
static s16 matrix[64][32];              /* N[i][k] = cos((16 + i)(2k + 1) pi / 64) x 32767 */
static s32 inv_levels[18];              /* 2^30 / levels */
static s32 v[1024];                     /* the synthesis's V history (Q24), a ring */
static int v_at, ready;

static s16 cos64(int m)                 /* cos(m pi / 64), any m */
{
    m &= 127;
    if (m <= 32)
        return cos_quarter[m];
    if (m <= 64)
        return (s16)-cos_quarter[64 - m];
    if (m <= 96)
        return (s16)-cos_quarter[m - 64];
    return cos_quarter[128 - m];
}

static void setup(void)
{
    for (int i = 0; i <= 256; i++) {
        window[i] = window_half[i];
        if (i && i < 256)
            window[512 - i] = (i & 63) ? -window_half[i] : window_half[i];
    }
    for (int i = 0; i < 64; i++)
        for (int k = 0; k < 32; k++)
            matrix[i][k] = cos64((16 + i) * (2 * k + 1));
    for (int c = 1; c < 18; c++)
        inv_levels[c] = (s32)((1u << 30) / classes[c].levels);
    ready = 1;
}

void mp2_reset(void)
{
    for (int i = 0; i < 1024; i++)
        v[i] = 0;
    v_at = 0;
}

int mp2_header(const u8 *h, struct mp2_header *hd)
{
    if (h[0] != 0xFF || (h[1] & 0xF6) != 0xF4)          /* sync (12 bits; MPEG-2.5 not taken), Layer II */
        return 0;
    int lsf = !(h[1] & 0x08), br = h[2] >> 4, fr = (h[2] >> 2) & 3;
    if (br == 0 || br == 15 || fr == 3)                 /* (free format, not taken) */
        return 0;
    hd->lsf = lsf;
    hd->bitrate = lsf ? rates_lsf[br] : rates_mpeg1[br];
    hd->rate = lsf ? freqs[fr] / 2 : freqs[fr];
    hd->channels = (h[3] >> 6) == 3 ? 1 : 2;
    hd->samples = 1152;
    hd->bytes = 144000u * hd->bitrate / hd->rate + ((h[2] >> 1) & 1);
    return 1;
}

/* ---- the bit reader ---- */
struct bits {
    const u8 *p;
    u32 at, end;                        /* bits */
};

static u32 get(struct bits *b, int n)
{
    u32 r = 0;
    while (n > 0) {
        u32 byte = b->at >> 3, left = 8 - (b->at & 7);
        u32 take = (u32)n < left ? (u32)n : left;
        u32 x = byte < (b->end >> 3) ? b->p[byte] : 0;
        r = (r << take) | ((x >> (left - take)) & ((1u << take) - 1));
        b->at += take;
        n -= (int)take;
    }
    return r;
}

/* ---- synthesis: 32 subband samples (Q28) into 32 PCM samples ---- */
static void synth(const s32 *s, s16 *out)
{
    v_at = (v_at - 64) & 1023;
    s32 *vv = v + v_at;                 /* (64 entries from v_at: never wraps, v_at is a multiple of 64) */
    for (int i = 0; i < 64; i++) {
        long long acc = 0;
        const s16 *n = matrix[i];
        for (int k = 0; k < 32; k++)
            acc += (long long)s[k] * n[k];
        vv[i] = (s32)(acc >> 19);       /* (Q28 x Q15 -> Q24: headroom for 128 x full scale) */
    }
    for (int j = 0; j < 32; j++) {
        long long acc = 0;
        for (int i = 0; i < 8; i++) {
            acc += (long long)v[(v_at + i * 128 + j) & 1023] * window[i * 64 + j];
            acc += (long long)v[(v_at + i * 128 + 96 + j) & 1023] * window[i * 64 + 32 + j];
        }
        s32 o = (s32)(acc >> 25);       /* (Q24 x Q16 -> Q15) */
        out[j] = (s16)(o > 32767 ? 32767 : o < -32768 ? -32768 : o);
    }
}

/* ---- a frame ---- */
int mp2_decode(const u8 *frame, const struct mp2_header *hd, s16 *out)
{
    if (!ready) {
        setup();
        mp2_reset();
    }
    struct bits b = {frame, 32, hd->bytes * 8};
    if (!(frame[1] & 1))
        b.at += 16;                     /* (the CRC: not checked) */
    int mode = frame[3] >> 6, ext = (frame[3] >> 4) & 3, nch = hd->channels;
    const u8 *table;
    int sblimit;
    if (hd->lsf) {
        table = alloc_lsf;
        sblimit = 30;
    } else {
        u32 per = hd->bitrate / (u32)nch;
        if (per <= 48) {
            table = alloc_low;
            sblimit = hd->rate == 32000 ? 12 : 8;
        } else {
            table = alloc_high;
            sblimit = per <= 80 || hd->rate == 48000 ? 27 : 30;
        }
    }
    int bound = mode == 1 ? (ext + 1) * 4 : sblimit;
    if (nch == 1 || bound > sblimit)
        bound = sblimit;
    u8 cls[2][32], scfsi[2][32], sf[2][32][3];
    for (int sb = 0; sb < sblimit; sb++) {
        int nbal = table[sb] >> 4, row = table[sb] & 15;
        for (int ch = 0; ch < nch; ch++) {
            if (sb < bound || ch == 0)
                cls[ch][sb] = classes_of[row][get(&b, nbal)];
            else
                cls[ch][sb] = cls[0][sb];
        }
    }
    for (int sb = 0; sb < sblimit; sb++)
        for (int ch = 0; ch < nch; ch++)
            scfsi[ch][sb] = cls[ch][sb] ? (u8)get(&b, 2) : 0;
    for (int sb = 0; sb < sblimit; sb++)
        for (int ch = 0; ch < nch; ch++) {
            if (!cls[ch][sb])
                continue;
            u8 *f = sf[ch][sb];
            switch (scfsi[ch][sb]) {
            case 0: f[0] = (u8)get(&b, 6); f[1] = (u8)get(&b, 6); f[2] = (u8)get(&b, 6); break;
            case 1: f[0] = f[1] = (u8)get(&b, 6); f[2] = (u8)get(&b, 6); break;
            case 2: f[0] = f[1] = f[2] = (u8)get(&b, 6); break;
            default: f[0] = (u8)get(&b, 6); f[1] = f[2] = (u8)get(&b, 6); break;
            }
        }
    if (b.at > b.end)
        return -1;
    for (int gr = 0; gr < 12; gr++) {
        int part = gr >> 2;
        s32 mix[3][32];                 /* the three samples of each subband, both channels mixed (Q28) */
        for (int sb = 0; sb < 32; sb++)
            mix[0][sb] = mix[1][sb] = mix[2][sb] = 0;
        for (int sb = 0; sb < sblimit; sb++) {
            s32 frac[2][3];             /* each channel's three samples before their scale factor (Q30) */
            for (int ch = 0; ch < nch; ch++) {
                int c = cls[ch][sb];
                if (!c) {
                    frac[ch][0] = frac[ch][1] = frac[ch][2] = 0;
                    continue;
                }
                if (sb >= bound && ch == 1) {           /* (joint stereo: the samples are the first channel's) */
                    frac[1][0] = frac[0][0], frac[1][1] = frac[0][1], frac[1][2] = frac[0][2];
                    continue;
                }
                u32 levels = classes[c].levels, code[3];
                if (classes[c].grouped) {
                    u32 w = get(&b, classes[c].bits);
                    code[0] = w % levels, w /= levels;
                    code[1] = w % levels;
                    code[2] = w / levels;
                } else {
                    code[0] = get(&b, classes[c].bits);
                    code[1] = get(&b, classes[c].bits);
                    code[2] = get(&b, classes[c].bits);
                }
                for (int i = 0; i < 3; i++)
                    frac[ch][i] = (s32)(2 * code[i] - (levels - 1)) * inv_levels[c];
            }
            for (int ch = 0; ch < nch; ch++) {
                if (!cls[ch][sb])
                    continue;
                int idx = sf[ch][sb][part];
                if (idx >= 63)
                    continue;
                s32 scale = (s32)(sf_base[idx % 3] >> (idx / 3));      /* Q29 */
                for (int i = 0; i < 3; i++) {
                    s32 x = (s32)(((long long)frac[ch][i] * scale) >> 31);      /* Q28 */
                    mix[i][sb] += nch == 2 ? x / 2 : x;
                }
            }
        }
        for (int i = 0; i < 3; i++)
            synth(mix[i], out + (gr * 3 + i) * 32);
    }
    return b.at > b.end ? -1 : 0;
}

/* ---- linear resampling to 48 kHz ---- */
int mp2_resample(const s16 *in, int n, u32 rate, s16 *out)
{
    static u32 phase;                   /* Q16 position between `last` (0) and in[0] (1.0) */
    static s16 last;
    u32 step = (rate << 16) / 48000u;   /* (rate <= 48000: no overflow) */
    int k = 0;
    for (;;) {
        u32 at = phase >> 16;           /* 0: between last and in[0]; i: between in[i - 1] and in[i] */
        if ((int)at >= n)
            break;
        s32 a = at ? in[at - 1] : last, c = in[at];
        u32 f = phase & 0xFFFF;
        out[k++] = (s16)(a + (((c - a) * (s32)f) >> 16));
        phase += step;
    }
    phase -= (u32)n << 16;
    last = n ? in[n - 1] : last;
    return k;
}
