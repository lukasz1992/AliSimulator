/* Picture files: JPEG, PNG, GIF, BMP (image.h says what each covers).
   Memory: the caller's work area -- a baseline JPEG needs a few rows of
   blocks (its pictures of any size are decoded a row of blocks at a time),
   a progressive JPEG all its coefficients (2 bytes each, about 3 bytes a
   pixel for 4:2:0), an interlaced PNG and a GIF the whole picture; the rest
   need a few rows. */
#include "image.h"

#define FAIL(text) return (text)

static u32 be32(const u8 *p)
{
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

static u32 le32(const u8 *p)
{
    return (u32)p[3] << 24 | (u32)p[2] << 16 | (u32)p[1] << 8 | p[0];
}

static u16 le16(const u8 *p)
{
    return (u16)(p[0] | p[1] << 8);
}

static u16 be16(const u8 *p)
{
    return (u16)(p[0] << 8 | p[1]);
}

static void zero(void *p, u32 n)
{
    u8 *b = p;
    while (n--)
        *b++ = 0;
}

static char *put(char *p, const char *s)
{
    while (*s)
        *p++ = *s++;
    *p = 0;
    return p;
}

static char *putn(char *p, u32 v)
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

/* a piece of the work area, 8-byte aligned: 0 when it is used up */
struct arena {
    u8 *p;
    u32 left;
};

static void *take(struct arena *a, u32 n)
{
    u32 pad = (u32)(-(unsigned long)a->p & 7);
    if (n > 0xFFFFFFF0u - pad || n + pad > a->left)
        return 0;
    u8 *r = a->p + pad;
    a->p += pad + n;
    a->left -= pad + n;
    return r;
}

#define TOO_BIG "too big for the memory there is for it"
#define MAX_SCANS 200                   /* a progressive JPEG's scans (real ones have 10 or so) */

int (*image_poll)(void);

static int stop_asked(void)
{
    return image_poll && image_poll();
}

/* ======================================================================= BMP */
static const char *bmp_info(const u8 *d, u32 size, struct image_info *in, u32 *bpp, u32 *comp)
{
    if (size < 54)
        FAIL("a BMP file too short");
    u32 hs = le32(d + 14);
    if (hs == 12) {
        in->width = le16(d + 18);
        in->height = le16(d + 20);
        *bpp = le16(d + 24);
        *comp = 0;
        if (!in->width || !in->height)
            FAIL("a BMP file with no picture");
    } else if (hs >= 40 && hs <= size - 14) {
        s32 w = (s32)le32(d + 18), h = (s32)le32(d + 22);
        if (w <= 0 || h == 0)
            FAIL("a BMP file with no picture");
        in->width = (u32)w;
        in->height = (u32)(h < 0 ? -h : h);
        *bpp = le16(d + 28);
        *comp = le32(d + 30);
    } else {
        FAIL("a BMP header this does not know");
    }
    if (*comp != 0 && !(*comp == 3 && (*bpp == 16 || *bpp == 32)))
        FAIL("a compressed BMP (RLE): not read");
    if (*bpp != 1 && *bpp != 4 && *bpp != 8 && *bpp != 16 && *bpp != 24 && *bpp != 32)
        FAIL("a BMP depth this does not know");
    char *p = putn(in->detail, *bpp);
    put(p, *bpp <= 8 ? "-bit palette" : "-bit colour");
    return 0;
}

/* a mask's shift and width (BI_BITFIELDS) */
static void mask_shape(u32 m, int *shift, int *bits)
{
    *shift = *bits = 0;
    if (!m)
        return;
    while (!(m & 1)) {
        m >>= 1;
        (*shift)++;
    }
    while (m & 1) {
        m >>= 1;
        (*bits)++;
    }
}

static u8 mask_get(u32 v, int shift, int bits)
{
    if (!bits)
        return 0;
    v = (v >> shift) & ((1u << bits) - 1);
    if (bits >= 8)
        return (u8)(v >> (bits - 8));
    u32 r = v << (8 - bits);            /* the top bits again below: 0x1F -> 0xFF */
    for (int b = bits; b < 8; b += bits)
        r |= v << (8 - bits) >> b;
    return (u8)r;
}

static const char *bmp_decode(const u8 *d, u32 size, struct arena *a, image_row_fn row, void *ctx)
{
    struct image_info in;
    u32 bpp, comp;
    const char *why = bmp_info(d, size, &in, &bpp, &comp);
    if (why)
        return why;
    u32 hs = le32(d + 14), off = le32(d + 10);
    int topdown = hs >= 40 && (s32)le32(d + 22) < 0;
    u32 stride = ((in.width * bpp + 31) / 32) * 4;
    if (off > size || (u64)stride * in.height > size - off)
        FAIL("a BMP file shorter than its picture");
    const u8 *pal = d + 14 + hs;
    u32 pal_entry = hs == 12 ? 3 : 4, colours = hs >= 40 ? le32(d + 46) : 0;
    if (bpp <= 8 && (!colours || colours > 1u << bpp))
        colours = 1u << bpp;
    if (bpp <= 8 && (pal + colours * pal_entry > d + size))
        FAIL("a BMP file shorter than its palette");
    u32 masks[3] = {0x7C00, 0x03E0, 0x001F};
    if (bpp == 32)
        masks[0] = 0xFF0000, masks[1] = 0xFF00, masks[2] = 0xFF;
    if (comp == 3) {
        const u8 *m = hs >= 52 ? d + 54 : d + 14 + hs;
        if (m + 12 > d + size)
            FAIL("a BMP file too short");
        masks[0] = le32(m), masks[1] = le32(m + 4), masks[2] = le32(m + 8);
    }
    int sh[3], bits[3];
    for (int i = 0; i < 3; i++)
        mask_shape(masks[i], &sh[i], &bits[i]);
    u8 *rgb = take(a, in.width * 3);
    if (!rgb)
        FAIL(TOO_BIG);
    for (u32 y = 0; y < in.height; y++) {
        const u8 *s = d + off + (u64)stride * (topdown ? y : in.height - 1 - y);
        u8 *o = rgb;
        for (u32 x = 0; x < in.width; x++, o += 3) {
            u32 v;
            switch (bpp) {
            case 1: case 4: case 8:
                v = (s[x * bpp / 8] >> (8 - bpp - (x * bpp) % 8)) & ((1u << bpp) - 1);
                if (v >= colours)
                    v = 0;
                o[0] = pal[v * pal_entry + 2], o[1] = pal[v * pal_entry + 1], o[2] = pal[v * pal_entry];
                break;
            case 24:
                o[0] = s[x * 3 + 2], o[1] = s[x * 3 + 1], o[2] = s[x * 3];
                break;
            default:
                v = bpp == 16 ? le16(s + x * 2) : le32(s + x * 4);
                for (int i = 0; i < 3; i++)
                    o[i] = mask_get(v, sh[i], bits[i]);
                break;
            }
        }
        row(ctx, y, rgb);
        if (!(y & 15) && stop_asked())
            FAIL(IMAGE_STOPPED);
    }
    return 0;
}

/* =================================================================== inflate */
struct huff {
    u16 count[16], symbol[288];
};

struct inflate {
    const u8 *in, *end;
    u32 bits;
    int nbits;
    u8 *window;                         /* 32 KB, circular */
    u32 wpos;
    void (*out)(void *ctx, const u8 *p, u32 n);
    void *ctx;
    int stop;                           /* the consumer has all it wants, or failed; 2: image_poll asked */
};

static int need(struct inflate *z, int n)
{
    while (z->nbits < n) {
        if (z->in >= z->end)
            return 0;
        z->bits |= (u32)*z->in++ << z->nbits;
        z->nbits += 8;
    }
    return 1;
}

static int zbits(struct inflate *z, int n)
{
    if (!need(z, n))
        return -1;
    int v = (int)(z->bits & ((1u << n) - 1));
    z->bits >>= n;
    z->nbits -= n;
    return v;
}

static void build(struct huff *h, const u8 *lengths, int n)
{
    u16 offs[16];
    zero(h->count, sizeof(h->count));
    for (int i = 0; i < n; i++)
        h->count[lengths[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int len = 1; len < 15; len++)
        offs[len + 1] = (u16)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++)
        if (lengths[i])
            h->symbol[offs[lengths[i]]++] = (u16)i;
}

static int zdecode(struct inflate *z, const struct huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        if (!z->nbits && !need(z, 1))
            return -1;
        code |= (int)(z->bits & 1);
        z->bits >>= 1;
        z->nbits--;
        int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static void emit(struct inflate *z, u8 c)
{
    z->window[z->wpos++ & 0x7FFF] = c;
    if (!(z->wpos & 0x7FFF)) {
        z->out(z->ctx, z->window, 0x8000);
        if (!z->stop && stop_asked())
            z->stop = 2;
    }
}

static void flush(struct inflate *z)
{
    u32 n = z->wpos & 0x7FFF;
    if (n)
        z->out(z->ctx, z->window, n);
}

static const u16 len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99,
                                 115, 131, 163, 195, 227, 258};
static const u8 len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const u16 dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025,
                                  1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const u8 dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12,
                                  12, 13, 13};

static const char *codes(struct inflate *z, const struct huff *lit, const struct huff *dist)
{
    for (;;) {
        if (z->stop)
            return 0;
        int s = zdecode(z, lit);
        if (s < 0)
            FAIL("its compressed data ends early or is damaged");
        if (s < 256) {
            emit(z, (u8)s);
        } else if (s == 256) {
            return 0;
        } else {
            s -= 257;
            if (s >= 29)
                FAIL("its compressed data is damaged");
            int e = zbits(z, len_extra[s]);
            int d = zdecode(z, dist);
            if (e < 0 || d < 0 || d >= 30)
                FAIL("its compressed data is damaged");
            int de = zbits(z, dist_extra[d]);
            if (de < 0)
                FAIL("its compressed data ends early");
            u32 len = len_base[s] + (u32)e, back = dist_base[d] + (u32)de;
            if (back > z->wpos)
                FAIL("its compressed data is damaged");
            while (len--)
                emit(z, z->window[(z->wpos - back) & 0x7FFF]);
        }
    }
}

static const char *inflate_run(struct inflate *z, struct huff *lit, struct huff *dist)
{
    static const u8 order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    u8 lengths[320];
    int last;
    do {
        last = zbits(z, 1);
        int type = zbits(z, 2);
        if (last < 0 || type < 0)
            FAIL("its compressed data ends early");
        if (type == 0) {
            z->bits = 0;
            z->nbits = 0;
            if (z->end - z->in < 4)
                FAIL("its compressed data ends early");
            u32 len = le16(z->in);
            z->in += 4;
            if ((u32)(z->end - z->in) < len)
                FAIL("its compressed data ends early");
            while (len-- && !z->stop)
                emit(z, *z->in++);
        } else if (type == 1) {
            int i = 0;
            for (; i < 144; i++) lengths[i] = 8;
            for (; i < 256; i++) lengths[i] = 9;
            for (; i < 280; i++) lengths[i] = 7;
            for (; i < 288; i++) lengths[i] = 8;
            build(lit, lengths, 288);
            for (i = 0; i < 30; i++)
                lengths[i] = 5;
            build(dist, lengths, 30);
            const char *why = codes(z, lit, dist);
            if (why)
                return why;
        } else if (type == 2) {
            int nlen = zbits(z, 5), ndist = zbits(z, 5), ncode = zbits(z, 4);
            if (nlen < 0 || ndist < 0 || ncode < 0)
                FAIL("its compressed data ends early");
            nlen += 257, ndist += 1, ncode += 4;
            zero(lengths, 19);
            for (int i = 0; i < ncode; i++) {
                int v = zbits(z, 3);
                if (v < 0)
                    FAIL("its compressed data ends early");
                lengths[order[i]] = (u8)v;
            }
            struct huff lens;
            build(&lens, lengths, 19);
            for (int i = 0; i < nlen + ndist;) {
                int s = zdecode(z, &lens), rep = 0, val = 0;
                if (s < 0)
                    FAIL("its compressed data is damaged");
                if (s < 16) {
                    lengths[i++] = (u8)s;
                    continue;
                }
                if (s == 16) {
                    if (!i)
                        FAIL("its compressed data is damaged");
                    val = lengths[i - 1];
                    rep = 3 + zbits(z, 2);
                } else if (s == 17) {
                    rep = 3 + zbits(z, 3);
                } else {
                    rep = 11 + zbits(z, 7);
                }
                if (rep < 3 || i + rep > nlen + ndist)
                    FAIL("its compressed data is damaged");
                while (rep--)
                    lengths[i++] = (u8)val;
            }
            build(lit, lengths, nlen);
            build(dist, lengths + nlen, ndist);
            const char *why = codes(z, lit, dist);
            if (why)
                return why;
        } else {
            FAIL("its compressed data is damaged");
        }
    } while (!last && !z->stop);
    flush(z);
    return 0;
}

/* ======================================================================= PNG */
struct png {
    u32 w, h, rowbytes, bpp;            /* bpp: bytes per complete pixel, at least 1 (the filters') */
    u8 depth, type, interlace;
    u8 pal[256][3], alpha[256];
    int trns_key;                       /* gray / RGB: one colour is transparent */
    u16 key[3];
    u8 *cur, *prev, *rgb, *full;        /* full: an interlaced picture, whole */
    u32 at;                             /* the bytes of this row so far (with its filter byte) */
    u32 y, pass, pass_w, pass_h;
    image_row_fn row;
    void *ctx;
    const char *why;
    int done;
    struct inflate *z;                  /* (told to stop once the picture is complete, or a row failed) */
};

static const u8 adam7[7][4] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2},
                               {0, 1, 1, 2}};

static u32 png_rowbytes(const struct png *g, u32 w)
{
    static const u8 channels[7] = {1, 0, 3, 1, 2, 0, 4};
    return (w * channels[g->type] * g->depth + 7) / 8;
}

static void png_pass_size(struct png *g)
{
    if (!g->interlace) {
        g->pass_w = g->w, g->pass_h = g->h;
        return;
    }
    const u8 *p = adam7[g->pass];
    g->pass_w = g->w > p[0] ? (g->w - p[0] + p[2] - 1) / p[2] : 0;
    g->pass_h = g->h > p[1] ? (g->h - p[1] + p[3] - 1) / p[3] : 0;
}

static u8 paeth(u8 a, u8 b, u8 c)
{
    int p = a + b - c, pa = p - a, pb = p - b, pc = p - c;
    pa = pa < 0 ? -pa : pa, pb = pb < 0 ? -pb : pb, pc = pc < 0 ? -pc : pc;
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* one sample of the row (depth bits) */
static u32 sample(const u8 *r, u32 i, u8 depth)
{
    switch (depth) {
    case 16: return (u32)r[i * 2] << 8 | r[i * 2 + 1];
    case 8: return r[i];
    default: return (u32)(r[i * depth / 8] >> (8 - depth - (i * depth) % 8)) & ((1u << depth) - 1);
    }
}

static u8 scale8(u32 v, u8 depth)
{
    switch (depth) {
    case 16: return (u8)(v >> 8);
    case 8: return (u8)v;
    case 4: return (u8)(v * 17);
    case 2: return (u8)(v * 85);
    default: return (u8)(v * 255);
    }
}

/* the unfiltered row (pass_w pixels) as RGB into out: alpha over black */
static void png_rgb(struct png *g, const u8 *r, u8 *out)
{
    for (u32 x = 0; x < g->pass_w; x++, out += 3) {
        u32 a = 255, c[3];
        switch (g->type) {
        case 0:
            c[0] = c[1] = c[2] = sample(r, x, g->depth);
            if (g->trns_key && c[0] == g->key[0])
                a = 0;
            c[0] = c[1] = c[2] = scale8(c[0], g->depth);
            break;
        case 2:
            for (int i = 0; i < 3; i++)
                c[i] = sample(r, x * 3 + (u32)i, g->depth);
            if (g->trns_key && c[0] == g->key[0] && c[1] == g->key[1] && c[2] == g->key[2])
                a = 0;
            for (int i = 0; i < 3; i++)
                c[i] = scale8(c[i], g->depth);
            break;
        case 3: {
            u32 v = sample(r, x, g->depth);
            c[0] = g->pal[v][0], c[1] = g->pal[v][1], c[2] = g->pal[v][2];
            a = g->alpha[v];
            break;
        }
        case 4:
            c[0] = c[1] = c[2] = scale8(sample(r, x * 2, g->depth), g->depth);
            a = scale8(sample(r, x * 2 + 1, g->depth), g->depth);
            break;
        default:
            for (int i = 0; i < 3; i++)
                c[i] = scale8(sample(r, x * 4 + (u32)i, g->depth), g->depth);
            a = scale8(sample(r, x * 4 + 3, g->depth), g->depth);
            break;
        }
        for (int i = 0; i < 3; i++)
            out[i] = (u8)(c[i] * a / 255);
    }
}

static void png_row_done(struct png *g)
{
    u8 *r = g->cur + 1, *p = g->prev + 1;
    u32 n = png_rowbytes(g, g->pass_w), bpp = g->bpp;
    switch (g->cur[0]) {
    case 0: break;
    case 1: for (u32 i = bpp; i < n; i++) r[i] = (u8)(r[i] + r[i - bpp]); break;
    case 2: for (u32 i = 0; i < n; i++) r[i] = (u8)(r[i] + p[i]); break;
    case 3:
        for (u32 i = 0; i < n; i++)
            r[i] = (u8)(r[i] + (((i >= bpp ? r[i - bpp] : 0) + p[i]) >> 1));
        break;
    case 4:
        for (u32 i = 0; i < n; i++)
            r[i] = (u8)(r[i] + paeth(i >= bpp ? r[i - bpp] : 0, p[i], i >= bpp ? p[i - bpp] : 0));
        break;
    default:
        g->why = "its rows are damaged (an unknown filter)";
        return;
    }
    if (!g->interlace) {
        png_rgb(g, r, g->rgb);
        g->row(g->ctx, g->y, g->rgb);
    } else {
        const u8 *a = adam7[g->pass];
        png_rgb(g, r, g->rgb);
        u8 *line = g->full + ((a[1] + g->y * a[3]) * g->w) * 3;
        for (u32 x = 0; x < g->pass_w; x++)
            for (int i = 0; i < 3; i++)
                line[(a[0] + x * a[2]) * 3 + (u32)i] = g->rgb[x * 3 + (u32)i];
    }
    u8 *t = g->prev;
    g->prev = g->cur;
    g->cur = t;
    g->y++;
}

static void png_next_pass(struct png *g)
{
    while (g->y >= g->pass_h) {
        if (!g->interlace || ++g->pass == 7) {
            g->done = 1;
            return;
        }
        g->y = 0;
        png_pass_size(g);
        zero(g->prev, g->rowbytes + 1);
        if (g->pass_w == 0)
            g->pass_h = 0;
    }
}

static void png_bytes(void *ctx, const u8 *p, u32 n)
{
    struct png *g = ctx;
    while (n && !g->done && !g->why) {
        u32 len = png_rowbytes(g, g->pass_w) + 1;
        u32 k = len - g->at < n ? len - g->at : n;
        for (u32 i = 0; i < k; i++)
            g->cur[g->at + i] = p[i];
        g->at += k, p += k, n -= k;
        if (g->at == len) {
            g->at = 0;
            png_row_done(g);
            png_next_pass(g);
        }
    }
    if ((g->done || g->why) && !g->z->stop)
        g->z->stop = 1;
}

static const char *png_head(const u8 *d, u32 size, struct png *g)
{
    if (size < 33 || be32(d + 8) != 13 || d[12] != 'I' || d[13] != 'H' || d[14] != 'D' || d[15] != 'R')
        FAIL("a PNG file without its header");
    g->w = be32(d + 16), g->h = be32(d + 20);
    g->depth = d[24], g->type = d[25], g->interlace = d[28];
    if (!g->w || !g->h || g->w > 0x8000 || g->h > 0x8000)
        FAIL("a PNG file of a size this does not take");
    int ok = 0;
    switch (g->type) {
    case 0: ok = g->depth == 1 || g->depth == 2 || g->depth == 4 || g->depth == 8 || g->depth == 16; break;
    case 3: ok = g->depth == 1 || g->depth == 2 || g->depth == 4 || g->depth == 8; break;
    case 2: case 4: case 6: ok = g->depth == 8 || g->depth == 16; break;
    }
    if (!ok || d[26] || d[27] || g->interlace > 1)
        FAIL("a PNG kind this does not know");
    return 0;
}

static void png_detail(const struct png *g, char *p)
{
    static const char *const kinds[7] = {"gray", "", "RGB", "palette", "gray + alpha", "", "RGBA"};
    p = putn(p, g->depth);
    p = put(p, "-bit ");
    p = put(p, kinds[g->type]);
    if (g->interlace)
        put(p, ", interlaced");
}

static const char *png_decode(u8 *d, u32 size, struct arena *a, image_row_fn row, void *ctx)
{
    static struct png g;
    zero(&g, sizeof(g));
    const char *why = png_head(d, size, &g);
    if (why)
        return why;
    for (int i = 0; i < 256; i++)
        g.alpha[i] = 255, g.pal[i][0] = g.pal[i][1] = g.pal[i][2] = (u8)i;
    /* the chunks: the palette, transparency, and the IDATs' data gathered in place (it only moves forward) */
    u8 *data = d + 8, *gathered = 0;
    u32 total = 0, at = 8;
    while (at + 12 <= size) {
        u32 len = be32(d + at);
        const u8 *t = d + at + 4;
        if (len > size - at - 12)
            FAIL("a PNG file that ends early");
        const u8 *c = d + at + 8;
        if (t[0] == 'P' && t[1] == 'L' && t[2] == 'T' && t[3] == 'E') {
            for (u32 i = 0; i < len / 3 && i < 256; i++)
                g.pal[i][0] = c[i * 3], g.pal[i][1] = c[i * 3 + 1], g.pal[i][2] = c[i * 3 + 2];
        } else if (t[0] == 't' && t[1] == 'R' && t[2] == 'N' && t[3] == 'S') {
            if (g.type == 3)
                for (u32 i = 0; i < len && i < 256; i++)
                    g.alpha[i] = c[i];
            else if (g.type == 0 && len >= 2)
                g.trns_key = 1, g.key[0] = be16(c);
            else if (g.type == 2 && len >= 6)
                g.trns_key = 1, g.key[0] = be16(c), g.key[1] = be16(c + 2), g.key[2] = be16(c + 4);
        } else if (t[0] == 'I' && t[1] == 'D' && t[2] == 'A' && t[3] == 'T') {
            if (!gathered)
                gathered = data;
            for (u32 i = 0; i < len; i++)
                gathered[total + i] = c[i];
            total += len;
        } else if (t[0] == 'I' && t[1] == 'E' && t[2] == 'N' && t[3] == 'D') {
            break;
        }
        at += len + 12;
    }
    if (!total)
        FAIL("a PNG file without picture data");
    if (total < 2 || (gathered[0] & 0x0F) != 8 || (be16(gathered) % 31))
        FAIL("its compressed data is not zlib's");
    static const u8 channels[7] = {1, 0, 3, 1, 2, 0, 4};
    g.bpp = (u32)channels[g.type] * g.depth / 8;
    if (!g.bpp)
        g.bpp = 1;
    g.rowbytes = png_rowbytes(&g, g.w);
    g.cur = take(a, g.rowbytes + 1);
    g.prev = take(a, g.rowbytes + 1);
    g.rgb = take(a, g.w * 3);
    struct inflate *z = take(a, sizeof(*z));
    struct huff *lit = take(a, sizeof(*lit)), *dist = take(a, sizeof(*dist));
    u8 *window = take(a, 0x8000);
    if (g.interlace)
        g.full = (u64)g.w * g.h * 3 < 0xFFFFFFFFu ? take(a, g.w * g.h * 3) : 0;
    if (!g.cur || !g.prev || !g.rgb || !z || !lit || !dist || !window || (g.interlace && !g.full))
        FAIL(TOO_BIG);
    zero(g.prev, g.rowbytes + 1);
    if (g.full)
        zero(g.full, g.w * g.h * 3);
    g.row = row, g.ctx = ctx;
    png_pass_size(&g);
    png_next_pass(&g);
    zero(z, sizeof(*z));
    z->in = gathered + 2, z->end = gathered + total;
    z->window = window, z->out = png_bytes, z->ctx = &g;
    g.z = z;
    why = inflate_run(z, lit, dist);
    if (g.why)
        return g.why;
    if (z->stop == 2)
        return IMAGE_STOPPED;
    if (!g.done)
        return why ? why : "its picture data ends early";
    if (g.interlace)
        for (u32 y = 0; y < g.h; y++) {
            row(ctx, y, g.full + y * g.w * 3);
            if (!(y & 15) && stop_asked())
                return IMAGE_STOPPED;
        }
    return 0;
}

/* ======================================================================= GIF */
static const char *gif_decode(const u8 *d, u32 size, struct arena *a, image_row_fn row, void *ctx, int info_only,
                              struct image_info *info)
{
    if (size < 13)
        FAIL("a GIF file too short");
    u32 sw = le16(d + 6), sh = le16(d + 8);
    u8 flags = d[10], bg = d[11];
    u8 gpal[256][3], lpal[256][3], (*pal)[3] = gpal;
    u32 at = 13, gcolours = 0;
    zero(gpal, sizeof(gpal));
    zero(lpal, sizeof(lpal));
    if (flags & 0x80) {
        gcolours = 2u << (flags & 7);
        if (at + gcolours * 3 > size)
            FAIL("a GIF file too short");
        for (u32 i = 0; i < gcolours; i++)
            gpal[i][0] = d[at + i * 3], gpal[i][1] = d[at + i * 3 + 1], gpal[i][2] = d[at + i * 3 + 2];
        at += gcolours * 3;
    }
    int transparent = -1;
    while (at < size) {
        u8 b = d[at++];
        if (b == 0x21) {                                /* an extension: a graphic control's transparency */
            if (at + 1 >= size)
                break;
            u8 label = d[at++];
            if (label == 0xF9 && at + 5 < size && d[at] >= 4 && (d[at + 1] & 1))
                transparent = d[at + 4];
            while (at < size && d[at]) {
                at += d[at] + 1u;
            }
            at++;
            continue;
        }
        if (b != 0x2C)
            break;
        if (at + 9 > size)
            FAIL("a GIF file too short");
        u32 left = le16(d + at), top = le16(d + at + 2), w = le16(d + at + 4), h = le16(d + at + 6);
        u8 f = d[at + 8];
        at += 9;
        if (f & 0x80) {
            u32 n = 2u << (f & 7);
            if (at + n * 3 > size)
                FAIL("a GIF file too short");
            for (u32 i = 0; i < n; i++)
                lpal[i][0] = d[at + i * 3], lpal[i][1] = d[at + i * 3 + 1], lpal[i][2] = d[at + i * 3 + 2];
            pal = lpal;
            at += n * 3;
        }
        if (!sw || !sh)
            sw = left + w, sh = top + h;
        if (!sw || !sh || !w || !h)
            FAIL("a GIF file with no picture");
        if (info_only) {
            info->width = sw, info->height = sh;
            char *p = put(info->detail, "palette");
            if (f & 0x40)
                put(p, ", interlaced");
            return 0;
        }
        /* the picture's pixels, and which of them it drew (the rest -- outside its frame, or transparent -- is
           the background: the global palette's background colour, black when the picture has transparency
           or there is no global palette) */
        u8 *canvas = (u64)sw * sh < 0x7FFFFFF0u ? take(a, sw * sh) : 0;
        u8 *drawn = canvas ? take(a, (sw * sh + 7) / 8) : 0;
        u16 *prefix = take(a, 4096 * 2);
        u8 *suffix = take(a, 4096), *stack = take(a, 4097), *rgb = take(a, sw * 3);
        if (!canvas || !drawn || !prefix || !suffix || !stack || !rgb)
            FAIL(TOO_BIG);
        zero(drawn, (sw * sh + 7) / 8);
        u8 back[3] = {0, 0, 0};
        if (transparent < 0 && gcolours)
            back[0] = gpal[bg][0], back[1] = gpal[bg][1], back[2] = gpal[bg][2];
        if (at >= size)
            FAIL("a GIF file too short");
        u32 min = d[at++];
        if (min < 2 || min > 11)
            FAIL("its picture data is damaged");
        u32 clear = 1u << min, end = clear + 1, next = clear + 2, csize = min + 1, old = 0xFFFF;
        u32 bits = 0, nbits = 0, block = 0, x = 0, y = 0, pass = 0, sp = 0;
        u8 firstc = 0;
        int finished = 0;
        static const u8 istart[4] = {0, 4, 2, 1}, istep[4] = {8, 8, 4, 2};
        while (!finished) {
            while (nbits < csize) {
                if (!block) {
                    if (at >= size || !(block = d[at++])) {
                        finished = 1;
                        break;
                    }
                }
                if (at >= size) {
                    finished = 1;
                    break;
                }
                bits |= (u32)d[at++] << nbits;
                nbits += 8;
                block--;
            }
            if (finished)
                break;
            u32 code = bits & ((1u << csize) - 1);
            bits >>= csize;
            nbits -= csize;
            if (code == clear) {
                csize = min + 1, next = clear + 2, old = 0xFFFF;
                continue;
            }
            if (code == end)
                break;
            u32 in = code;
            if (old == 0xFFFF) {
                if (code >= clear)
                    FAIL("its picture data is damaged");
                stack[sp++] = (u8)code;
                firstc = (u8)code;
            } else {
                if (code > next || (code == next && next >= 4096))
                    FAIL("its picture data is damaged");
                if (code == next) {
                    stack[sp++] = firstc;
                    code = old;
                }
                while (code >= clear) {
                    stack[sp++] = suffix[code];
                    code = prefix[code];
                    if (sp >= 4096)
                        FAIL("its picture data is damaged");
                }
                stack[sp++] = (u8)code;
                firstc = (u8)code;
                if (next < 4096) {
                    prefix[next] = (u16)old;
                    suffix[next] = firstc;
                    next++;
                    if (next == (1u << csize) && csize < 12)
                        csize++;
                }
            }
            old = in;
            while (sp) {
                u8 c = stack[--sp];
                if (y < h) {
                    if (left + x < sw && top + y < sh && (int)c != transparent) {
                        u32 i = (top + y) * sw + left + x;
                        canvas[i] = c;
                        drawn[i >> 3] = (u8)(drawn[i >> 3] | 1u << (i & 7));
                    }
                    if (++x == w) {
                        x = 0;
                        if (!(y & 15) && stop_asked())
                            FAIL(IMAGE_STOPPED);
                        if (f & 0x40) {
                            y += istep[pass];
                            while (y >= h && pass < 3)
                                y = istart[++pass];
                        } else {
                            y++;
                        }
                    }
                }
            }
        }
        for (u32 r = 0; r < sh; r++) {
            for (u32 c = 0; c < sw; c++) {
                u32 i = r * sw + c;
                const u8 *colour = (drawn[i >> 3] >> (i & 7)) & 1 ? pal[canvas[i]] : back;
                rgb[c * 3] = colour[0], rgb[c * 3 + 1] = colour[1], rgb[c * 3 + 2] = colour[2];
            }
            row(ctx, r, rgb);
        }
        return 0;
    }
    FAIL("a GIF file without a picture");
}

/* ====================================================================== JPEG */
static const u8 zz[64] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27,
                          20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58,
                          59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

struct jhuff {
    u8 fast[512];                       /* the 9 bits ahead -> the symbol's index (255: longer code) */
    u8 size[257], values[256];
    u16 code[256];
    s32 mincode[17], maxcode[18], valptr[17];
};

struct jcomp {
    u8 id, h, v, tq, td, ta;
    u32 bw, bh;                         /* its blocks across and down (whole MCUs) */
    s32 dc;
    short *coef;                        /* bw x bh blocks of 64 (progressive / several scans: all of them) */
    u8 *plane;                          /* one MCU row of samples: bw * 8 wide */
};

struct jpeg {
    const u8 *d, *end;
    u32 pos;
    u32 bits;
    int nbits, marker_hit;
    u16 q[4][64];                       /* natural order */
    struct jhuff dc[4], ac[4];
    struct jcomp c[3];
    int nc, progressive, adobe_rgb, rst, hmax, vmax;
    u32 w, h, mcux, mcuy;
    int stored;                         /* all coefficients kept (progressive, or scans of single components) */
    u32 eobrun;
    u8 *rgb;
};

static int jhuff_build(struct jhuff *t, const u8 *counts)
{
    int k = 0;
    for (int len = 1; len <= 16; len++)
        for (int i = 0; i < counts[len - 1]; i++)
            t->size[k++] = (u8)len;
    t->size[k] = 0;
    u32 code = 0;
    k = 0;
    for (int len = 1; len <= 16; len++) {
        t->valptr[len] = k;
        t->mincode[len] = (s32)code;
        for (int i = 0; i < counts[len - 1]; i++)
            t->code[k++] = (u16)code++;
        t->maxcode[len] = counts[len - 1] ? (s32)code - 1 : -1;
        if (code > (1u << len))
            return -1;                  /* (more codes of this length than there can be: a damaged table) */
        code <<= 1;
    }
    t->maxcode[17] = 0x7FFFFFFF;
    for (int i = 0; i < 512; i++)
        t->fast[i] = 255;
    for (int i = 0; i < k; i++) {
        int s = t->size[i];
        if (s <= 9) {
            int c = t->code[i] << (9 - s), m = 1 << (9 - s);
            for (int j = 0; j < m; j++)
                t->fast[c + j] = (u8)i;
        }
    }
    return 0;
}

static void jfill(struct jpeg *j)
{
    while (j->nbits <= 24) {
        u32 b = 0;
        if (!j->marker_hit && j->d + j->pos < j->end) {
            b = j->d[j->pos];
            if (b == 0xFF) {
                u8 n = j->d + j->pos + 1 < j->end ? j->d[j->pos + 1] : 0xD9;
                if (n == 0) {
                    j->pos += 2;
                } else {
                    j->marker_hit = 1;
                    b = 0;
                }
            } else {
                j->pos++;
            }
        }
        j->bits |= b << (24 - j->nbits);
        j->nbits += 8;
    }
}

static int jbit(struct jpeg *j)
{
    if (j->nbits < 1)
        jfill(j);
    int v = (int)(j->bits >> 31);
    j->bits <<= 1;
    j->nbits--;
    return v;
}

static int jget(struct jpeg *j, int n)
{
    if (!n)
        return 0;
    if (j->nbits < n)
        jfill(j);
    int v = (int)(j->bits >> (32 - n));
    j->bits <<= n;
    j->nbits -= n;
    return v;
}

static int extend(int v, int s)
{
    return s && v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
}

static int jdecode(struct jpeg *j, const struct jhuff *t)
{
    if (j->nbits < 16)
        jfill(j);
    int k = t->fast[j->bits >> 23];
    if (k < 255) {
        int s = t->size[k];
        j->bits <<= s;
        j->nbits -= s;
        return t->values[k];
    }
    s32 code = (s32)(j->bits >> 23);
    j->bits <<= 9;
    j->nbits -= 9;
    for (int len = 10; len <= 16; len++) {
        code = (code << 1) | (s32)(j->bits >> 31);
        j->bits <<= 1;
        j->nbits--;
        if (t->maxcode[len] >= 0 && code <= t->maxcode[len] && code >= t->mincode[len])
            return t->values[t->valptr[len] + code - t->mincode[len]];
    }
    return -1;
}

/* a block in a sequential scan (or a progressive scan's first DC-only pass when ss = se = 0, al): its
   coefficients, not dequantized */
static int block_seq(struct jpeg *j, struct jcomp *c, short *b)
{
    zero(b, 128);
    int t = jdecode(j, &j->dc[c->td]);
    if (t < 0 || t > 16)
        return -1;
    c->dc += extend(jget(j, t), t);
    b[0] = (short)c->dc;
    for (int k = 1; k < 64;) {
        int rs = jdecode(j, &j->ac[c->ta]);
        if (rs < 0)
            return -1;
        int r = rs >> 4, s = rs & 15;
        if (!s) {
            if (r != 15)
                break;
            k += 16;
            continue;
        }
        k += r;
        if (k > 63)
            return -1;
        b[zz[k++]] = (short)extend(jget(j, s), s);
    }
    return 0;
}

static int block_dc(struct jpeg *j, struct jcomp *c, short *b, int ah, int al)
{
    if (!ah) {
        int t = jdecode(j, &j->dc[c->td]);
        if (t < 0 || t > 16)
            return -1;
        c->dc += extend(jget(j, t), t);
        b[0] = (short)(c->dc * (1 << al));
    } else if (jbit(j)) {
        b[0] = (short)(b[0] | (1 << al));
    }
    return 0;
}

static int block_ac_first(struct jpeg *j, struct jcomp *c, short *b, int ss, int se, int al)
{
    if (j->eobrun) {
        j->eobrun--;
        return 0;
    }
    for (int k = ss; k <= se;) {
        int rs = jdecode(j, &j->ac[c->ta]);
        if (rs < 0)
            return -1;
        int r = rs >> 4, s = rs & 15;
        if (!s) {
            if (r < 15) {
                j->eobrun = (1u << r) - 1;
                if (r)
                    j->eobrun += (u32)jget(j, r);
                break;
            }
            k += 16;
            continue;
        }
        k += r;
        if (k > 63)
            return -1;
        b[zz[k++]] = (short)(extend(jget(j, s), s) * (1 << al));
    }
    return 0;
}

static int block_ac_refine(struct jpeg *j, struct jcomp *c, short *b, int ss, int se, int al)
{
    short bit = (short)(1 << al);
    int k = ss;
    if (j->eobrun) {
        j->eobrun--;
        for (; k <= se; k++) {
            short *p = &b[zz[k]];
            if (*p && jbit(j) && !(*p & bit))
                *p = (short)(*p > 0 ? *p + bit : *p - bit);
        }
        return 0;
    }
    do {
        int rs = jdecode(j, &j->ac[c->ta]);
        if (rs < 0)
            return -1;
        int s = rs & 15, r = rs >> 4;
        if (!s) {
            if (r < 15) {
                j->eobrun = (1u << r) - 1;
                if (r)
                    j->eobrun += (u32)jget(j, r);
                r = 64;
            }
        } else {
            if (s != 1)
                return -1;
            s = jbit(j) ? bit : -bit;
        }
        while (k <= se) {
            short *p = &b[zz[k++]];
            if (*p) {
                if (jbit(j) && !(*p & bit))
                    *p = (short)(*p > 0 ? *p + bit : *p - bit);
            } else {
                if (!r) {
                    *p = (short)s;
                    break;
                }
                r--;
            }
        }
    } while (k <= se);
    return 0;
}

#define F_IDCT(s0, s1, s2, s3, s4, s5, s6, s7) \
    int t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3; \
    p2 = s2; p3 = s6; \
    p1 = (p2 + p3) * 2217; \
    t2 = p1 + p3 * -7567; \
    t3 = p1 + p2 * 3135; \
    p2 = s0; p3 = s4; \
    t0 = (p2 + p3) * 4096; \
    t1 = (p2 - p3) * 4096; \
    x0 = t0 + t3; x3 = t0 - t3; x1 = t1 + t2; x2 = t1 - t2; \
    t0 = s7; t1 = s5; t2 = s3; t3 = s1; \
    p3 = t0 + t2; p4 = t1 + t3; p1 = t0 + t3; p2 = t1 + t2; \
    p5 = (p3 + p4) * 4816; \
    t0 = t0 * 1223; t1 = t1 * 8410; t2 = t2 * 12586; t3 = t3 * 6149; \
    p1 = p5 + p1 * -3685; p2 = p5 + p2 * -10497; p3 = p3 * -8034; p4 = p4 * -1597; \
    t3 += p1 + p4; t2 += p2 + p3; t1 += p2 + p4; t0 += p1 + p3;

static u8 clamp(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (u8)v;
}

/* a value kept within +-limit: a damaged file's coefficients cannot overflow the IDCT's 32-bit sums (a real
   picture's stay far inside: its dequantized coefficients within +-1024 or so, the first pass's results +-12000) */
static int bound(int v, int limit)
{
    return v < -limit ? -limit : v > limit ? limit : v;
}

/* a block's coefficients (natural order, not dequantized) to 8 x 8 samples at out, `stride` apart */
static void idct(const short *in, const u16 *q, u8 *out, u32 stride)
{
    int v[64];
    for (int i = 0; i < 8; i++) {
        const short *d = in + i;
        const u16 *m = q + i;
        int c[8];
        for (int r = 0; r < 8; r++)
            c[r] = bound(d[r * 8] * (int)m[r * 8], 16384);
        if (!d[8] && !d[16] && !d[24] && !d[32] && !d[40] && !d[48] && !d[56]) {
            int dc = bound(c[0] * 4, 32767);
            for (int r = 0; r < 8; r++)
                v[i + r * 8] = dc;
            continue;
        }
        F_IDCT(c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7])
        x0 += 512; x1 += 512; x2 += 512; x3 += 512;
        v[i] = bound((x0 + t3) >> 10, 32767);
        v[i + 56] = bound((x0 - t3) >> 10, 32767);
        v[i + 8] = bound((x1 + t2) >> 10, 32767);
        v[i + 48] = bound((x1 - t2) >> 10, 32767);
        v[i + 16] = bound((x2 + t1) >> 10, 32767);
        v[i + 40] = bound((x2 - t1) >> 10, 32767);
        v[i + 24] = bound((x3 + t0) >> 10, 32767);
        v[i + 32] = bound((x3 - t0) >> 10, 32767);
    }
    for (int i = 0; i < 8; i++, out += stride) {
        const int *s = v + i * 8;
        F_IDCT(s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7])
        x0 += 65536 + (128 << 17); x1 += 65536 + (128 << 17); x2 += 65536 + (128 << 17); x3 += 65536 + (128 << 17);
        out[0] = clamp((x0 + t3) >> 17);
        out[7] = clamp((x0 - t3) >> 17);
        out[1] = clamp((x1 + t2) >> 17);
        out[6] = clamp((x1 - t2) >> 17);
        out[2] = clamp((x2 + t1) >> 17);
        out[5] = clamp((x2 - t1) >> 17);
        out[3] = clamp((x3 + t0) >> 17);
        out[4] = clamp((x3 - t0) >> 17);
    }
}

/* the MCU row `my`'s samples into each component's plane, then its picture rows out */
static void jpeg_rows(struct jpeg *j, u32 my, short *rowcoef[3], image_row_fn row, void *ctx)
{
    for (int i = 0; i < j->nc; i++) {
        struct jcomp *c = &j->c[i];
        u32 stride = c->bw * 8;
        for (u32 by = 0; by < c->v; by++)
            for (u32 bx = 0; bx < c->bw; bx++) {
                const short *b = rowcoef[i] + ((by * c->bw) + bx) * 64;
                idct(b, j->q[c->tq], c->plane + by * 8 * stride + bx * 8, stride);
            }
    }
    u32 lines = (u32)j->vmax * 8;
    for (u32 l = 0; l < lines; l++) {
        u32 y = my * lines + l;
        if (y >= j->h)
            break;
        u8 *o = j->rgb;
        const struct jcomp *c0 = &j->c[0], *c1 = &j->c[1], *c2 = &j->c[2];
        const u8 *r0 = c0->plane + (l * c0->v / (u32)j->vmax) * c0->bw * 8;
        if (j->nc == 1) {
            for (u32 x = 0; x < j->w; x++, o += 3)
                o[0] = o[1] = o[2] = r0[x];
        } else {
            const u8 *r1 = c1->plane + (l * c1->v / (u32)j->vmax) * c1->bw * 8;
            const u8 *r2 = c2->plane + (l * c2->v / (u32)j->vmax) * c2->bw * 8;
            int s0 = c0->h == j->hmax, s1 = c1->h == j->hmax, s2 = c2->h == j->hmax;
            for (u32 x = 0; x < j->w; x++, o += 3) {
                int yy = r0[s0 ? x : x * c0->h / (u32)j->hmax];
                int cb = r1[s1 ? x : x * c1->h / (u32)j->hmax];
                int cr = r2[s2 ? x : x * c2->h / (u32)j->hmax];
                if (j->adobe_rgb) {
                    o[0] = (u8)yy, o[1] = (u8)cb, o[2] = (u8)cr;
                    continue;
                }
                cb -= 128, cr -= 128;
                int base = yy * 65536 + 32768;
                o[0] = clamp((base + cr * 91881) >> 16);
                o[1] = clamp((base - cb * 22554 - cr * 46802) >> 16);
                o[2] = clamp((base + cb * 116130) >> 16);
            }
        }
        row(ctx, y, j->rgb);
    }
}

/* to the next marker after a restart interval: its RSTn passed, the predictions back to 0 */
static void restart(struct jpeg *j)
{
    j->bits = 0, j->nbits = 0, j->marker_hit = 0, j->eobrun = 0;
    while (j->d + j->pos + 1 < j->end && !(j->d[j->pos] == 0xFF && j->d[j->pos + 1] >= 0xD0 && j->d[j->pos + 1] <= 0xD7))
        j->pos++;
    if (j->d + j->pos + 1 < j->end)
        j->pos += 2;
    for (int i = 0; i < j->nc; i++)
        j->c[i].dc = 0;
}

/* the headers up to the frame (info), or every segment with the scans (decoding) */
static const char *jpeg_parse(struct jpeg *j, struct image_info *info, struct arena *a, image_row_fn row, void *ctx)
{
    u32 size = (u32)(j->end - j->d), at = 2;
    int frame = 0, scans = 0;
    if (info)
        info->orientation = 1;
    while (at + 4 <= size) {
        if (j->d[at] != 0xFF) {
            at++;
            continue;
        }
        u8 m = j->d[at + 1];
        if (m == 0xFF) {
            at++;
            continue;
        }
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) {
            at += 2;
            continue;
        }
        if (m == 0xD9)
            break;
        u32 len = be16(j->d + at + 2);
        const u8 *s = j->d + at + 4;
        if (len < 2 || at + 2 + len > size)
            FAIL("a JPEG file that ends early");
        u32 n = len - 2;
        if (m == 0xE1 && info && n > 14 && s[0] == 'E' && s[1] == 'x' && s[2] == 'i' && s[3] == 'f') {
            const u8 *t = s + 6;                        /* EXIF: IFD0's orientation */
            u32 tn = n - 6;
            int le = t[0] == 'I';
            u32 ifd = le ? le32(t + 4) : be32(t + 4);
            if (tn >= 2 && ifd <= tn - 2) {
                u32 count = le ? le16(t + ifd) : be16(t + ifd);
                for (u32 i = 0; i < count && i < 1000 && i * 12 + 14 <= tn - ifd; i++) {
                    const u8 *e = t + ifd + 2 + i * 12;
                    if ((le ? le16(e) : be16(e)) == 0x0112) {
                        u32 o = le ? le16(e + 8) : be16(e + 8);
                        if (o >= 1 && o <= 8)
                            info->orientation = (u8)o;
                    }
                }
            }
        } else if (m == 0xEE && n >= 12 && s[0] == 'A' && s[1] == 'd' && s[2] == 'o' && s[3] == 'b' && s[4] == 'e') {
            j->adobe_rgb = s[11] == 0;
        } else if (m == 0xDB) {
            for (u32 i = 0; i < n;) {
                int prec = s[i] >> 4, id = s[i] & 3;
                if (i + 1 + (prec ? 128u : 64u) > n)
                    FAIL("a JPEG file with a damaged table");
                i++;
                for (int k = 0; k < 64; k++)
                    j->q[id][zz[k]] = prec ? be16(s + i + k * 2) : s[i + k];
                i += prec ? 128 : 64;
            }
        } else if (m == 0xC4) {
            for (u32 i = 0; i + 17 <= n;) {
                int cls = s[i] >> 4, id = s[i] & 3, total = 0;
                struct jhuff *t = cls ? &j->ac[id] : &j->dc[id];
                for (int k = 0; k < 16; k++)
                    total += s[i + 1 + k];
                if (total > 256 || i + 17 + (u32)total > n)
                    FAIL("a JPEG file with a damaged table");
                for (int k = 0; k < total; k++)
                    t->values[k] = s[i + 17 + k];
                if (jhuff_build(t, s + i + 1) < 0)
                    FAIL("a JPEG file with a damaged table");
                i += 17 + (u32)total;
            }
        } else if (m == 0xDD) {
            if (n >= 2)
                j->rst = be16(s);
        } else if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
            if (frame)
                FAIL("a JPEG with two frames");
            if (n < 6 || n < 6 + 3u * s[5])
                FAIL("a JPEG with a damaged frame header");
            if (s[0] != 8)
                FAIL("a 12-bit JPEG: not read");
            j->progressive = m == 0xC2;
            j->h = be16(s + 1), j->w = be16(s + 3), j->nc = s[5];
            if (!j->w || !j->h)
                FAIL("a JPEG without its height (DNL): not read");
            if (j->nc != 1 && j->nc != 3)
                FAIL(j->nc == 4 ? "a CMYK JPEG: not read" : "a JPEG of a kind this does not know");
            j->hmax = j->vmax = 1;
            for (int i = 0; i < j->nc; i++) {
                struct jcomp *c = &j->c[i];
                c->id = s[6 + i * 3], c->h = s[7 + i * 3] >> 4, c->v = s[7 + i * 3] & 15, c->tq = s[8 + i * 3] & 3;
                if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4)
                    FAIL("a JPEG of a kind this does not know");
                if (c->h > j->hmax) j->hmax = c->h;
                if (c->v > j->vmax) j->vmax = c->v;
            }
            if (j->nc == 1)
                j->c[0].h = j->c[0].v = 1, j->hmax = j->vmax = 1;
            j->mcux = (j->w + 8u * j->hmax - 1) / (8u * j->hmax);
            j->mcuy = (j->h + 8u * j->vmax - 1) / (8u * j->vmax);
            for (int i = 0; i < j->nc; i++)
                j->c[i].bw = j->mcux * j->c[i].h, j->c[i].bh = j->mcuy * j->c[i].v;
            frame = 1;
            if (!info) {
                j->rgb = take(a, j->w * 3);
                for (int i = 0; i < j->nc; i++)
                    if (!(j->c[i].plane = take(a, j->c[i].bw * 8 * j->c[i].v * 8)))
                        FAIL(TOO_BIG);
                if (!j->rgb)
                    FAIL(TOO_BIG);
            }
            if (info) {
                info->width = j->w, info->height = j->h;
                char *p = put(info->detail, j->progressive ? "progressive" : "baseline");
                if (j->nc == 1) {
                    put(p, ", gray");
                } else {
                    p = put(p, ", ");
                    const char *sub = j->hmax == 2 && j->vmax == 2 ? "4:2:0" : j->hmax == 2 && j->vmax == 1 ? "4:2:2"
                                    : j->hmax == 1 && j->vmax == 1 ? "4:4:4" : "subsampled";
                    put(p, sub);
                }
            }
        } else if (m >= 0xC3 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            FAIL(m >= 0xC9 ? "an arithmetic-coded JPEG: not read" : "a lossless / hierarchical JPEG: not read");
        } else if (m == 0xDA) {
            if (!frame)
                FAIL("a JPEG with no frame before its scan");
            if (info)
                return 0;
            if (n < 1 || n < 4 + 2u * s[0])
                FAIL("a JPEG with a damaged scan");
            if (++scans > MAX_SCANS)
                FAIL("a JPEG with more scans than a real one has");
            int ns = s[0];
            struct jcomp *sc[4];
            for (int i = 0; i < ns && i < 4; i++) {
                sc[i] = 0;
                for (int k = 0; k < j->nc; k++)
                    if (j->c[k].id == s[1 + i * 2])
                        sc[i] = &j->c[k];
                if (!sc[i])
                    FAIL("a JPEG with a damaged scan");
                sc[i]->td = (s[2 + i * 2] >> 4) & 3;
                sc[i]->ta = s[2 + i * 2] & 3;
            }
            const u8 *p = s + 1 + ns * 2;
            int ss = p[0], se = p[1], ah = p[2] >> 4, al = p[2] & 15;
            if (ns < 1 || ns > 3 || p + 3 > s + n || ss > se || se > 63 || al > 13
                || (j->progressive && ((ss == 0) != (se == 0) || (ss && ns != 1))))
                FAIL("a JPEG with a damaged scan");
            j->pos = at + 2 + len;
            j->bits = 0, j->nbits = 0, j->marker_hit = 0, j->eobrun = 0;
            for (int i = 0; i < j->nc; i++)
                j->c[i].dc = 0;
            if (!j->stored && (j->progressive || ns != j->nc)) {
                j->stored = 1;
                for (int i = 0; i < j->nc; i++) {
                    u64 bytes = (u64)j->c[i].bw * j->c[i].bh * 128;
                    j->c[i].coef = bytes < 0x7FFFFFFF ? take(a, (u32)bytes) : 0;
                    if (!j->c[i].coef)
                        FAIL(TOO_BIG);
                    zero(j->c[i].coef, (u32)bytes);
                }
            }
            short *rowcoef[3] = {0, 0, 0};
            if (!j->stored) {
                for (int i = 0; i < j->nc; i++) {
                    rowcoef[i] = take(a, j->c[i].bw * j->c[i].v * 128);
                    if (!rowcoef[i])
                        FAIL(TOO_BIG);
                }
            }
            u32 count = 0;
            int bad = 0;
            if (ns == 1 && j->stored) {
                /* a scan of one component: its own blocks, row by row, as far as the picture reaches */
                struct jcomp *c = sc[0];
                u32 cw = (j->w * c->h + (u32)j->hmax - 1) / (u32)j->hmax, ch = (j->h * c->v + (u32)j->vmax - 1) / (u32)j->vmax;
                u32 bw = (cw + 7) / 8, bh = (ch + 7) / 8;
                for (u32 by = 0; by < bh && !bad; by++) {
                    if (!(by & 7) && stop_asked())
                        FAIL(IMAGE_STOPPED);
                    for (u32 bx = 0; bx < bw && !bad; bx++) {
                        if (j->rst && count && !(count % (u32)j->rst))
                            restart(j);
                        count++;
                        short *b = c->coef + (by * c->bw + bx) * 64;
                        if (!j->progressive)
                            bad = block_seq(j, c, b);
                        else if (!ss)
                            bad = block_dc(j, c, b, ah, al);
                        else
                            bad = ah ? block_ac_refine(j, c, b, ss, se, al) : block_ac_first(j, c, b, ss, se, al);
                    }
                }
            } else {
                for (u32 my = 0; my < j->mcuy && !bad; my++) {
                    if (stop_asked())
                        FAIL(IMAGE_STOPPED);
                    for (u32 mx = 0; mx < j->mcux && !bad; mx++) {
                        if (j->rst && count && !(count % (u32)j->rst))
                            restart(j);
                        count++;
                        for (int i = 0; i < ns && !bad; i++) {
                            struct jcomp *c = sc[i];
                            for (u32 v = 0; v < c->v && !bad; v++)
                                for (u32 h = 0; h < c->h && !bad; h++) {
                                    short *b;
                                    if (j->stored)
                                        b = c->coef + ((my * c->v + v) * c->bw + mx * c->h + h) * 64;
                                    else
                                        b = rowcoef[c - j->c] + (v * c->bw + mx * c->h + h) * 64;
                                    bad = j->progressive ? block_dc(j, c, b, ah, al) : block_seq(j, c, b);
                                }
                        }
                    }
                    if (!j->stored && !bad)
                        jpeg_rows(j, my, rowcoef, row, ctx);
                }
            }
            if (bad && !j->stored)
                FAIL("its picture data is damaged");
            /* on to the next marker (a damaged progressive scan: the picture as far as it got) */
            at = j->pos;
            if (!j->stored)
                return 0;
            continue;
        }
        at += 2 + len;
    }
    if (info)
        FAIL("a JPEG file without a frame");
    if (!j->stored)
        FAIL("a JPEG file without picture data");
    short *rowcoef[3];
    for (u32 my = 0; my < j->mcuy; my++) {
        if (stop_asked())
            FAIL(IMAGE_STOPPED);
        for (int i = 0; i < j->nc; i++)
            rowcoef[i] = j->c[i].coef + my * j->c[i].v * j->c[i].bw * 64;
        jpeg_rows(j, my, rowcoef, row, ctx);
    }
    return 0;
}

static const char *jpeg_decode(const u8 *d, u32 size, struct arena *a, image_row_fn row, void *ctx)
{
    static struct jpeg j;
    zero(&j, sizeof(j));
    j.d = d, j.end = d + size;
    return jpeg_parse(&j, 0, a, row, ctx);
}

/* ===================================================================== both */
static int kind_of(const u8 *d, u32 size)
{
    if (size >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF)
        return 'J';
    if (size >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G')
        return 'P';
    if (size >= 6 && d[0] == 'G' && d[1] == 'I' && d[2] == 'F' && d[3] == '8')
        return 'G';
    if (size >= 2 && d[0] == 'B' && d[1] == 'M')
        return 'B';
    return 0;
}

const char *image_info(const u8 *data, u32 size, struct image_info *info)
{
    zero(info, sizeof(*info));
    info->orientation = 1;
    switch (kind_of(data, size)) {
    case 'J': {
        static struct jpeg j;
        zero(&j, sizeof(j));
        j.d = data, j.end = data + size;
        info->kind = "JPEG";
        return jpeg_parse(&j, info, 0, 0, 0);
    }
    case 'P': {
        static struct png g;
        zero(&g, sizeof(g));
        info->kind = "PNG";
        const char *why = png_head(data, size, &g);
        if (why)
            return why;
        info->width = g.w, info->height = g.h;
        png_detail(&g, info->detail);
        return 0;
    }
    case 'G':
        info->kind = "GIF";
        return gif_decode(data, size, 0, 0, 0, 1, info);
    case 'B': {
        u32 bpp, comp;
        info->kind = "BMP";
        return bmp_info(data, size, info, &bpp, &comp);
    }
    }
    FAIL("not a picture this knows (JPEG, PNG, GIF, BMP)");
}

const char *image_decode(u8 *data, u32 size, u8 *work, u32 work_size, image_row_fn row, void *ctx)
{
    struct arena a = {work, work_size};
    switch (kind_of(data, size)) {
    case 'J':
        return jpeg_decode(data, size, &a, row, ctx);
    case 'P':
        return png_decode(data, size, &a, row, ctx);
    case 'G':
        return gif_decode(data, size, &a, row, ctx, 0, 0);
    case 'B':
        return bmp_decode(data, size, &a, row, ctx);
    }
    FAIL("not a picture this knows (JPEG, PNG, GIF, BMP)");
}
