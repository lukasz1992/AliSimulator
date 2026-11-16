/* image.c: picture files decoded -- JPEG (baseline and progressive), PNG
   (every colour type and depth, interlaced too), GIF (its first picture) and
   BMP (1, 4, 8, 16, 24 and 32 bits).  Nothing in it knows the box: a file in
   RAM goes in, rows of red, green and blue bytes come out, top to bottom
   (view.c fits them to the TV; the same code runs on a PC for tests). */
#ifndef DEMO_IMAGE_H
#define DEMO_IMAGE_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed int s32;
typedef unsigned long long u64;

struct image_info {
    u32 width, height;
    const char *kind;                   /* "JPEG", "PNG", "GIF", "BMP" */
    char detail[48];                    /* "progressive, 4:2:0", "8-bit palette, interlaced", ... */
    u8 orientation;                     /* a JPEG's EXIF orientation, 1..8 (1: as stored) */
};

typedef void (*image_row_fn)(void *ctx, u32 y, const u8 *rgb);

extern int (*image_poll)(void);         /* when set: called now and then during a long decode (every 32 KB
                                           of a PNG's data, every row of blocks of a JPEG scan, every 16 rows of a
                                           GIF / BMP); nonzero stops it ("stopped") */
#define IMAGE_STOPPED "stopped"

const char *image_info(const u8 *data, u32 size, struct image_info *info);
                                        /* what the file is: 0, or why it is no picture this reads */
const char *image_decode(u8 *data, u32 size, u8 *work, u32 work_size, image_row_fn row, void *ctx);
                                        /* its rows through row(), y = 0 .. height - 1 in order, each width x 3
                                           bytes: 0, or why it stopped (the rows so far were given).  A PNG's
                                           data is gathered in place: the file in RAM is changed */

#endif
