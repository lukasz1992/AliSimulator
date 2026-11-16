/* snd.c: sound output of the S3811 (the Comsat TE 1050 HD's chip), a
   prototype modelled on the original firmware dump.bin's "SND_S3601" driver
   (ALi's snd_m36 with its S3811 additions): the sound engine at 0xB8002000
   reads PCM frames from a ring in RAM by DMA and plays them at 48 kHz on its
   I2S output (which the HDMI transmitter takes as its audio input) and on the
   chip's own audio DAC (the SCART's analog L/R).  The register map and the
   set-up sequence are in snd.c's header comment.

   Use: snd_init(0) once (or let snd_write do it), then snd_write() mono
   samples at snd_rate(); snd_room() says how many fit without waiting.  The
   engine starts playing once two frames (2 x 960 samples) are queued, and
   waits (silent, fading out and back in) when the ring runs dry. */
#ifndef DEMO_SND_H
#define DEMO_SND_H

typedef signed short s16;

#define SND_RATE         48000u           /* the only rate: dump.bin's own (and the HDMI set-up's N = 6144) */
#define SND_FRAME        960u             /* samples a frame (dump.bin's config: 0x3C0, 20 ms) */
#define SND_FRAMES       8u               /* frames in the ring (as dump.bin: 160 ms) */

/* RAM the sound engine reads (physical, written through KSEG1; clear of everything hw.h and rf.h list:
   RF_STAGE ends at 0x03F00000, NTFS_RUNS_PHYS starts at 0x04000000) */
#define SND_I2S_PHYS     0x03F00000u      /* the PCM ring: SND_FRAMES x 482 units of 16 bytes (61696 bytes) */
#define SND_SPO_PHYS     0x03F10000u      /* the S/PDIF ring (bitstream mode only; zeroed): 0xC00C x 8 bytes */
#define SND_OUT3_PHYS    0x03F80000u      /* the S3811's third output's ring (zeroed): 0xC00C x 8 bytes */

int snd_init(u32 rate);                 /* rate 0 or 48000: 0; anything else: -1 (said why) */
void snd_write(const s16 *mono, int n); /* n samples (played on both channels); waits for room (500 ms at most) */
u32 snd_rate(void);                     /* 48000 */
int snd_room(void);                     /* samples snd_write takes now without waiting (0 before snd_init) */
void snd_stop(void);                    /* silence: muted, DMA off, the DAC powered down; snd_init again to play */
void snd_status(void);                  /* the engine's registers, its DMA position, the measured rate */

/* the console command 'snd': init, beep [Hz] [ms], status, stop, vol [0..100] */
void snd_command(int argc, char **argv);

#endif
