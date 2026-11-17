/* avc.c: H.264 video on the S3811's own H.264 decoder (the SDK's DECV_AVC: the block of the MPEG-2 decoder, at
   0xB8004000 / 0xB8004200).  av.c's player reads the file, takes it apart and shows the frames; it gives the
   video's NAL units here one at a time and takes the decoded frames back in display order.  The engine, its
   registers and what was found on the box are in avc.c's header. */
#ifndef DEMO_AVC_H
#define DEMO_AVC_H

#include "hw.h"

/* avc_nal's answers */
#define AVC_DONE 0                      /* the NAL unit was taken */
#define AVC_FULL 1                      /* it starts a picture and no frame buffer is free: give the same NAL unit
                                           again once the player has shown and released frames (avc_release) */
#define AVC_FAIL (-1)                   /* the stream cannot be played here (said) */

struct avc_info {                       /* the stream, as its sequence parameter set describes it */
    u16 w, h;                           /* the picture: luma pixels, after the cropping */
    u16 mbw, mbh;                       /* its frames, in macroblocks */
    u16 sar_w, sar_h;                   /* the shape of its pixels (1:1 when the stream does not say) */
    u32 frame_us;                       /* a frame's time (0: the stream does not say) */
    u8 profile, level, refs, reorder, frames;   /* (frames: the frame buffers it was given) */
};

struct avc_stats {
    u32 pictures, types[3];             /* decoded: all, then P / B / I (by a picture's first slice) */
    u32 skipped;                        /* pictures not decoded (before the first I picture, no references) */
    u32 slices, errors, timeouts;       /* slices: the engine's error bit; no answer in 300 ms */
    u32 total_us, slowest_us;           /* the engine's time, a picture's */
    u32 last_status, last_mb;
};

/* a new stream: the engine reset, everything forgotten.  say: AVC_DESCRIBE the stream described in a line when
   its first picture sets it up ('video: H.264 ...'; not with 'play -q'), AVC_VERBOSE every slice told too
   ('play -v').  idle: called again and again while the engine decodes a slice -- the player reads the file and
   decodes the sound meanwhile; it must not move the stream's bytes the engine reads */
#define AVC_DESCRIBE 1
#define AVC_VERBOSE 2
void avc_begin(int say, void (*idle)(void));
/* one NAL unit: `nal` its header byte (in RAM the engine reads, KSEG0), `n` bytes; the stream's bytes go on
   to `avail` bytes from nal -- a start code must follow the NAL unit within them (the engine's start code
   detector ends a slice there): AVC_DONE, AVC_FULL, AVC_FAIL */
int avc_nal(const u8 *nal, u32 n, u32 avail);
void avc_end(void);                     /* the stream's end: every decoded picture to the output */
int avc_out(void);                      /* the next frame in display order (its number), -1 none yet */
void avc_release(int f);                /* the player has done with a frame avc_out gave (it is off the screen) */
u32 avc_luma(int f);                    /* a frame's luma / chroma: physical addresses */
u32 avc_chroma(int f);
u32 avc_number(int f);                  /* the picture it holds was the n-th decoded (from 1; 0: none yet) */
char avc_type(int f);                   /* 'I', 'P', 'B' (its first slice's) */
const struct avc_info *avc_info(void);
const struct avc_stats *avc_stats(void);
void avc_status(void);                  /* 'av status': the engine's registers, the frames */

#endif
