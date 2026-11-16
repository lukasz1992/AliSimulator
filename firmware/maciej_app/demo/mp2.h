/* mp2.c: an MPEG-1 / MPEG-2 (low sampling frequencies) audio Layer II decoder in integer arithmetic -- the
   sound of DVB broadcasts and of most .ts / .mpg files.  The original firmware (dump.bin) decodes it in
   software too (ALi's libmpg, "mpeg1_decode_audio_packet": the S3811 has no audio decoding hardware; its
   sound engine, snd.c, plays PCM), so the player (av.c) does the same.

   A frame is decoded whole into mono samples (the two channels mixed: snd.c plays mono on both sides) at the
   stream's own rate; mp2_resample() brings them to 48 kHz when the stream has another rate. */
#ifndef DEMO_MP2_H
#define DEMO_MP2_H

#include "hw.h"

typedef signed short s16;

struct mp2_header {
    u32 rate;                   /* samples a second (32000, 44100, 48000; 16000, 22050, 24000) */
    u32 bitrate;                /* kbit/s */
    u32 bytes;                  /* the whole frame's length */
    u32 samples;                /* samples a channel in the frame (Layer II: always 1152) */
    int channels;               /* 1, 2 */
    int lsf;                    /* MPEG-2's low sampling frequencies */
};

/* the frame header at h (4 bytes): 1 when it is a Layer II header this decoder takes (*hd filled), 0 */
int mp2_header(const u8 *h, struct mp2_header *hd);
/* one whole frame (hd->bytes at frame) into hd->samples mono samples: 0, -1 broken */
int mp2_decode(const u8 *frame, const struct mp2_header *hd, s16 *out);
/* the synthesis filters' history forgotten (a new stream) */
void mp2_reset(void);
/* n samples at `rate` into 48 kHz (out: room for n * 48000 / rate + 2): how many came out; the fraction
   carried over from call to call */
int mp2_resample(const s16 *in, int n, u32 rate, s16 *out);

#endif
