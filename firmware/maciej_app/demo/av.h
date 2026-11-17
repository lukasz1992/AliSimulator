/* av.c: a video file player on the S3811's own MPEG-2 decoder ("VE", 0xB8004200) -- and its H.264 engine,
   through avc.c -- and the display engine's video plane (the "MP" layer under the OSD): 'play <file>' (a .ts,
   an .mpg program stream, an .m2v or .264 elementary stream from a USB disk / the card), sound (MPEG audio
   Layer II, decoded by mp2.c) through snd.c.  The registers and how the original firmware (dump.bin) drives
   them are in av.c's and avc.c's headers. */
#ifndef DEMO_AV_H
#define DEMO_AV_H

#include "hw.h"

/* RAM (physical).  The decoder addresses its frame buffers inside one 32 MB window (its +0x78 register
   holds the window's number: bits 25..27 of the addresses), so they and its input buffer sit in the
   0x06000000..0x07FFFFFF window, in the picture viewer's work area (view.c; never used at the same time):
   dump.bin puts them in the same window (0x06C6C200.., VBV 0x07ED4000..). */
#define AV_WINDOW_PHYS   0x06000000u      /* the decoder's 32 MB window */
#define AV_VBV_PHYS      0x06900000u      /* the decoder's input: one picture's slices at a time (2 MB) */
#define AV_VBV_SIZE      0x00200000u
#define AV_FRAMES_PHYS   0x06B00000u      /* the frame buffers (5, as dump.bin's driver: 2 references, the one
                                             being decoded, the one shown and the one shown before it) */
#define AV_FRAMES_END    0x07F00000u      /* (VIEW_LIST_PHYS) */
#define AV_ES_PHYS       0x05800000u      /* the video elementary stream read from the file (4 MB; free RAM
                                             between WGET_PHYS's 8 MB and NETLOAD_PHYS) */
#define AV_ES_SIZE       0x00400000u
#define AV_AUDIO_PHYS    0x05C00000u      /* the audio elementary stream (1 MB) */
#define AV_AUDIO_SIZE    0x00100000u
#define AV_STAGE_PHYS    0x05D00000u      /* a program stream's bytes being taken apart (1 MB less 4 KB) */
#define AV_STAGE_SIZE    0x000FF000u
#define AV_PARAM_PHYS    0x05DFF000u      /* the display engine's parameter block: the scaler's filters (its
                                             +0x1E4 register points here; 0x560 bytes) */

/* 'play [-n frames] [-x] [-k] [-v] [-q] <file>': 0, -1 it could not start */
int av_play(int argc, char **argv);
void play_command(int argc, char **argv);       /* (the same for the command table) */
/* 'av <status|shot <file.bmp|file.yuv>|show|off|reset>' */
void av_command(int argc, char **argv);
/* the picture a 'play -k' / 'av show' left on the TV (the OSD hidden under it) taken away: 1 if there was one */
int av_dismiss(void);
/* the explorer: is this a file 'play' takes (.ts .mpg .mpeg .m2v .m2t .vob .trp .264 .h264)? */
int av_is_video(const char *name);
/* the data cache written back over [p, p + n) (what the CPU wrote, in RAM before a decoder reads it) */
void av_cache_flush(const void *p, u32 n);

#endif
