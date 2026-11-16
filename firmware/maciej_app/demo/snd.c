/* snd.c: sound output (a prototype) -- the S3811's sound engine "SND_S3601"
   as the Comsat TE 1050 HD's original firmware (dump.bin) drives it.

   Where it comes from: ALi's M36 SDK library libcore_hd.a (refs/MainCode/
   ddk) has the driver with symbols (snd_m36.o, snd_m36_reg.o: one accessor
   per register field); dump.bin carries a newer build of the same driver
   with S3811 branches (chip ID 0x10048): its accessors match the library's
   byte for byte (0x8038C6F0..0x8038D200), the S3811-only ones follow
   (0x8038D200..0x8038DAB0).  dump.bin run in the emulator with every access
   logged (and the box's own reset values preset) gave the set-up below; the
   start, the frame format and the DAC's power-up come from its code
   (addresses are dump.bin's, RAM image base 0x80000000).

   Registers (base 0xB8002000; bytes unless said)
     +0x00  b0 I2S on, b1 S/PDIF on, b2-3 MCLK select, b4-6 clock gates
            (1 = off), b7 the S3811's third output on    (reset 0x83)
     +0x01  b0..3 interrupt enables (I2S frame, AVS, S/PDIF, resume after
            underrun), b4..7 their status (write 1 to clear); IRQ 13
     +0x02  b0-2 master clock, b3-4 bit clock, b6-7 I2S format (0 = I2S)
     +0x03 frame info, +0x04 PTS (32), +0x08 frame count (16), +0x0A frame
            length (16) -- read back from the frames' headers
     +0x0C  last index (16): the DMA plays up to here (the driver's write
            position); reaching it is an underrun
     +0x0E  100, +0x10 / +0x22 / +0x15C samples a frame (960)
     +0x12  I2S sample count (16)
     +0x14  b0 underrun (1; write 1 clears), b1 fade on underrun, b2 resume
            by itself
     +0x15  b0 I2S DMA, b1 (S3811) third output's DMA, b4 S/PDIF DMA,
            b5 24-bit samples, b6 8 channels
     +0x18  the PCM ring (physical, 32), +0x1C the DMA's index (16, read),
            +0x1E the ring's length in units (16)
     +0x20..+0x27 S/PDIF control and channel status, +0x60 / +0x64 / +0x66
            its ring, index, length
     +0x68 / +0x6A post-processing, +0x6D fade speed, +0x6E volume
            (0..255, the 'music volume'), +0x6F the volume now (fades to +0x6E)
     +0xE4..+0xF3 the audio STC (not used here)
     +0xF4..+0xFD the chip's own audio DAC (the SCART's analog L/R): +0xF6
            b1 / +0xFC b7 power, +0xFB b0-2 a ramp (7 -> 0 on, 0 -> 7 off)
     +0x150..+0x162 the S3811's third output (+0x154 ring, +0x158 length,
            +0x15A index, +0x15E last index)
   Outside: 0xB8000080 b5 the engine's reset; 0xB80000B0 its clocks (b0 the
   third output, b1 S/PDIF, b2 / b5 / b6 I2S: 0 for 48 kHz); GPIO 14 the
   board's mute (dump.bin's board config: mute_num 14, polarity 0 -- low is
   muted; it drives it high when a channel plays); the HDMI transmitter
   0xB802A00F b0 lets the audio through (snd start sets it, stop clears it --
   the rest of HDMI audio, N = 6144 / CTS, the I2S input +0x0A b1 = 0, the
   audio infoframe, is set at the HDMI start and in video_init.h's tables).

   The ring (S3811): units of 16 bytes = two stereo samples, each a 24-bit
   value in the top of a 32-bit word: L0, R0, L1, R1.  A frame is 2 header
   units (the first word's low half = the frame's units, 2 + samples / 2;
   the rest 0) and 480 sample units; the ring holds 8 frames (3856 units,
   +0x1E = 0x0F10 as dump.bin).  The driver writes whole frames and moves
   the last index (+0x0C) past them; the DMA follows (24100 units a second).

   Set-up order (dump.bin's snd open 0x8038BDB8, reset 0x8037FC20, config
   0x8037F6D8 with cfg_i2so 0x8037E6A4 / S/PDIF 0x8037EB18 / third output
   0x8037F05C, the DAC's init 0x8037D604) and start (0x8037FEA0, the DAC's
   start 0x8037D700), stop (0x803800B0, the DAC's stop 0x8037D894) are
   followed below; the S/PDIF and third output rings stay zero (they carry
   only a compressed bitstream; for PCM those outputs take the I2S data). */
#include "hw.h"
#include "snd.h"

#define SND_BASE        0xB8002000u
#define SND8(o)         REG8(SND_BASE + (o))
#define SND16(o)        REG16(SND_BASE + (o))
#define SND32(o)        REG32(SND_BASE + (o))
#define SYS_RESET       0xB8000080u
#define SYS_AUDIO_CLK   0xB80000B0u
#define HDMI_AUDIO      0xB802A00Fu       /* b0: audio on (snd start / stop) */
#define MUTE_GPIO       14u               /* the Comsat's mute: low = muted */

#define FRAME_UNITS     (SND_FRAME / 2 + 2)                 /* 482 */
#define RING_UNITS      (FRAME_UNITS * SND_FRAMES)          /* 3856 = 0x0F10 */
#define SPO_UNITS       0xC00Cu
#define SPO_BYTES       (SPO_UNITS * 8)

static int inited, running;
static u32 wr;                          /* the next frame's first unit (a multiple of FRAME_UNITS) */
static s16 stage[SND_FRAME];            /* the frame being filled */
static u32 staged;
static u8 volume = 100;                 /* 0..100 (snd vol) */

/* counters for the status (the proof the DMA runs) */
static u32 frames_written, frames_dropped, underruns, frame_irqs;
static u32 units_played, last_hw;
static u64 play_start_ms;

static u32 hw_index(void)
{
    return SND16(0x1C);
}

/* the frames queued (the one being played counts) */
static u32 queued_frames(void)
{
    u32 units = (wr + RING_UNITS - hw_index()) % RING_UNITS;
    return (units + FRAME_UNITS - 1) / FRAME_UNITS;
}

/* bookkeeping on every call: units played, the status bits */
static void service(void)
{
    if (!inited)
        return;
    u32 now = hw_index();
    if (now < RING_UNITS) {
        units_played += (now + RING_UNITS - last_hw) % RING_UNITS;
        last_hw = now;
    }
    u8 st = SND8(0x01);
    if ((st & 0x11) == 0x11) {          /* a frame's interrupt (as dump.bin's ISR: write the bit to clear it) */
        SND8(0x01) = st | 0x10;
        frame_irqs++;
    }
    if ((st & 0x88) == 0x88) {          /* resumed after an underrun (the ISR: +0x01 b7, +0x14 b0 cleared) */
        SND8(0x01) = SND8(0x01) | 0x80;
        SND8(0x14) = SND8(0x14) | 0x01;
        underruns++;
    }
}

/* the volume table of the driver (vol_db_tab, snd_s3601_open): 0..100 -> the register's 0..255 */
static u8 volume_reg(u32 v)
{
    if (v >= 100)
        return 0xFF;
    if (v < 20)
        return (u8)v;
    if (v < 40)
        return (u8)(v * 2 - 20);
    if (v < 60)
        return (u8)(v * 255 / 100 - 42);
    return (u8)(v * 360 / 100 - 105);
}

static void mute_gpio(int muted)
{
    if (panel_present())                /* an Opticum: its mute pin is not known -- left alone */
        return;
    REG32(GPIO0_DIR) |= 1u << MUTE_GPIO;
    if (muted)
        REG32(GPIO0_DO) &= ~(1u << MUTE_GPIO);
    else
        REG32(GPIO0_DO) |= 1u << MUTE_GPIO;
}

static void zero_uncached(u32 phys, u32 bytes)
{
    volatile u32 *p = (volatile u32 *)KSEG1(phys);
    for (u32 i = 0; i < bytes / 4; i++)
        p[i] = 0;
}

u32 snd_rate(void)
{
    return SND_RATE;
}

int snd_init(u32 rate)
{
    if (rate && rate != SND_RATE) {
        uprintf("snd: %u Hz is not supported -- the engine runs at 48000 Hz (the HDMI audio is set up for it)\n",
                rate);
        return -1;
    }
    if (inited)
        snd_stop();
    zero_uncached(SND_I2S_PHYS, RING_UNITS * 16);
    zero_uncached(SND_SPO_PHYS, SPO_BYTES);
    zero_uncached(SND_OUT3_PHYS, SPO_BYTES);

    /* snd open: the clocks on */
    SND8(0x00) &= ~0x70;
    /* snd reset (config): the engine's reset bit pulsed for 3 us */
    REG8(SYS_RESET) |= 0x20;
    delay_us(3);
    REG8(SYS_RESET) &= ~0x20;
    SND32(0x18) = SND_I2S_PHYS & 0x0FFFFFFFu;
    SND16(0x1E) = 0xC020;               /* (dump.bin's first length; the real one below) */
    SND32(0x60) = SND_SPO_PHYS & 0x0FFFFFFFu;
    SND16(0x66) = 0xC020;
    SND32(0x154) = SND_OUT3_PHYS & 0x0FFFFFFFu;
    SND16(0x158) = 0xC020;
    SND8(0x01) |= 0x01;                 /* I2S frame interrupt (the CPU's interrupts stay off: polled) */
    SND8(0x01) |= 0x08;                 /* resume interrupt */
    SND8(0x6D) = 0x20;                  /* fade speed */
    SND8(0x14) |= 0x02;                 /* fade on underrun */
    SND8(0x14) |= 0x04;                 /* resume by itself */
    SND16(0x0E) = 100;
    SND16(0x0C) = 0;                    /* last index */
    SND8(0x104) = 2;                    /* (S3811) */
    SND8(0x106) = 2;
    SND8(0x68) = 0x00;                  /* (set_IIR_bit 0) */
    SND8(0x02) &= 0x3F;                 /* I2S format 0 */
    /* config: 2 channels, no post-processing */
    SND8(0x15) &= ~0x40;
    SND8(0x6A) = 0x60;
    /* cfg_i2so, 48 kHz (rate table 0x807384B4: 0x300D): clocks, the DAC's format, 24 bits */
    REG32(SYS_AUDIO_CLK) &= ~0x64u;
    SND8(0xF6) = (SND8(0xF6) & 0x0F) | 0xA0;
    SND8(0xF4) &= ~0x30;
    SND8(0x02) = (SND8(0x02) & ~0x07) | 3;      /* master clock */
    SND8(0x02) = (SND8(0x02) & ~0x18) | (1 << 3);       /* bit clock */
    SND8(0x00) &= ~0x0C;                /* MCLK select 0 */
    SND8(0xF5) = 0x80;
    SND8(0x15) |= 0x20;                 /* 24-bit samples */
    SND8(0x00) |= 0x01;                 /* I2S on */
    /* cfg_spdo (PCM, 48 kHz) */
    REG8(SYS_AUDIO_CLK) &= ~0x02;
    SND8(0x20) = 0x02;
    SND8(0x21) = 0x00;
    SND8(0x24) = 0x00;
    SND8(0x25) = 0x00;
    SND8(0x27) = (SND8(0x27) & 0xF0) | 0x02;    /* 48 kHz */
    SND8(0x00) |= 0x02;                 /* S/PDIF on */
    /* the third output (0x8037F05C) */
    REG8(SYS_AUDIO_CLK) &= ~0x01;
    SND8(0x150) = 0x04;
    SND8(0x153) = 0x02;
    SND8(0x161) = 0x42;
    SND8(0x162) = 0x4A;
    SND8(0x00) |= 0x80;                 /* third output on */
    /* frame size and ring lengths */
    SND16(0x10) = SND_FRAME;
    SND16(0x22) = SND_FRAME;
    SND16(0x15C) = SND_FRAME;
    SND16(0x66) = SPO_UNITS;
    SND16(0x1E) = RING_UNITS;
    SND16(0x158) = SPO_UNITS;
    SND16(0x0C) = 0;
    SND16(0x15E) = 0;
    /* the DAC's init (0x8037D604, not the 0x10047 branch) */
    SND8(0xF8) = 0x00;
    SND8(0xF9) = 0x80;
    SND8(0xF7) = 0x08;
    mute_gpio(1);

    wr = 0;
    staged = 0;
    running = 0;
    last_hw = hw_index() % RING_UNITS;
    frames_written = frames_dropped = underruns = frame_irqs = units_played = 0;
    inited = 1;
    return 0;
}

/* snd start (0x8037FEA0): the DAC up, the DMA on, the HDMI's audio on, unmuted */
static void start(void)
{
    SND8(0xF8) = 0x00;                  /* the DAC's start (0x8037D700, S3811 branch) */
    SND8(0xF7) = 0x08;
    SND8(0xFC) = 0x80;
    SND8(0xF6) |= 0x02;
    for (int s = 7; s >= 0; s--) {
        SND8(0xFB) = (SND8(0xFB) & ~7) | (u8)s;
        delay_us(1000);
    }
    SND8(0x00) |= 0x01 | 0x02 | 0x80;   /* (set_sub_blk 4, 0x40, 0x80) */
    SND8(0x15) = (SND8(0x15) & 0xEC) | 0x13;    /* DMA: I2S, third output, S/PDIF */
    REG8(HDMI_AUDIO) |= 0x01;
    mute_gpio(0);
    SND8(0x6E) = volume_reg(volume);
    running = 1;
    play_start_ms = now_ms();
    units_played = 0;
    last_hw = hw_index() % RING_UNITS;
}

void snd_stop(void)
{
    if (!inited)
        return;
    REG8(HDMI_AUDIO) &= ~0x01;
    SND8(0x6E) = 0;                     /* fade out (as set_mute) */
    for (int i = 0; i < 100 && SND8(0x6F); i++)
        delay_us(1000);
    mute_gpio(1);
    SND8(0x15) &= 0xEC;                 /* DMA off */
    SND16(0x0C) = 0;
    for (int s = 0; s <= 7; s++) {      /* the DAC's stop (0x8037D894, S3811 branch) */
        SND8(0xFB) = (SND8(0xFB) & ~7) | (u8)s;
        delay_us(1000);
    }
    SND8(0xFC) &= 0x7F;
    delay_us(1000);
    SND8(0xF6) &= ~0x02;
    inited = running = 0;
}

/* one frame from stage[] into the ring at wr (room checked by the caller) */
static void put_frame(void)
{
    volatile u32 *u = (volatile u32 *)KSEG1(SND_I2S_PHYS + wr * 16);
    u[0] = FRAME_UNITS;                 /* header: the frame's units (2 + 960 / 2), the rest 0 */
    u[1] = 0;
    u[2] = 0;
    u[3] = 0;
    u[4] = u[5] = u[6] = u[7] = 0;
    u += 8;
    for (u32 i = 0; i < SND_FRAME; i += 2) {
        u32 a = (u32)(s32)stage[i] << 16, b = (u32)(s32)stage[i + 1] << 16;   /* 24 bits << 8 */
        u[0] = a;
        u[1] = a;
        u[2] = b;
        u[3] = b;
        u += 4;
    }
    wr = (wr + FRAME_UNITS) % RING_UNITS;
    SND16(0x0C) = (u16)wr;              /* the DMA may play up to here */
    frames_written++;
    staged = 0;
    if (!running && queued_frames() >= 2)
        start();
}

int snd_room(void)
{
    if (!inited)
        return 0;
    service();
    u32 q = queued_frames();
    u32 free_frames = q + 1 < SND_FRAMES ? SND_FRAMES - 1 - q : 0;
    return (int)(free_frames * SND_FRAME + (SND_FRAME - staged));
}

void snd_write(const s16 *mono, int n)
{
    if (!inited && snd_init(0))
        return;
    while (n > 0) {
        u32 take = SND_FRAME - staged;
        if ((u32)n < take)
            take = (u32)n;
        for (u32 i = 0; i < take; i++)
            stage[staged + i] = mono[i];
        staged += take;
        mono += take;
        n -= (int)take;
        if (staged < SND_FRAME)
            break;
        /* a whole frame: wait for a free one (500 ms at most: the DMA stopped?) */
        u64 end = now_ms() + 500;
        service();
        while (queued_frames() + 1 >= SND_FRAMES && now_ms() < end) {
            delay_us(200);
            service();
        }
        if (queued_frames() + 1 >= SND_FRAMES) {
            frames_dropped++;
            staged = 0;
            continue;
        }
        put_frame();
    }
}

/* ------------------------------------------------------------------ */
/* the console command                                                */
/* ------------------------------------------------------------------ */

static int s_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int num(const char *s, u32 *out)
{
    u32 v = 0;
    if (!*s)
        return 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return 0;
        v = v * 10 + (u32)(*s - '0');
    }
    *out = v;
    return 1;
}

static int stop_asked(void)
{
    console_service();
    if (uart_getc() == 3) {
        uprintf("^C\n");
        return 1;
    }
    return 0;
}

static const s16 quarter_sine[65] = {
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179,
    7962, 8739, 9512, 10278, 11039, 11793, 12539, 13279, 14010, 14732,
    15446, 16151, 16846, 17530, 18204, 18868, 19519, 20159, 20787, 21403,
    22005, 22594, 23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790,
    27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956, 30273, 30571,
    30852, 31113, 31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521,
    32609, 32678, 32728, 32757, 32767,
};

static int sine(u32 phase)              /* phase: a full turn = 2^32 */
{
    u32 i = phase >> 24;                /* 0..255 */
    u32 q = i & 63;
    int v;
    switch (i >> 6) {
    case 0: v = quarter_sine[q]; break;
    case 1: v = quarter_sine[64 - q]; break;
    case 2: v = -quarter_sine[q]; break;
    default: v = -quarter_sine[64 - q]; break;
    }
    return v;
}

/* a sine tone of hz for ms at -12 dBFS, 5 ms fades; then the numbers that say the DMA played it */
static void beep(u32 hz, u32 ms)
{
    if (!inited && snd_init(0))
        return;
    u32 total = ms * (SND_RATE / 1000), done = 0, phase = 0;
    u32 inc = hz * 89478u + hz * 485u / 1000u;  /* 2^32 / 48000 = 89478.485 */
    u32 fade = SND_RATE / 200;          /* 5 ms */
    s16 buf[240];
    u32 f0 = frames_written, u0 = underruns, i0 = frame_irqs, d0 = frames_dropped;
    u16 sc0 = SND16(0x12), fc0 = SND16(0x08);
    int stopped = 0;
    u32 vol_min = 255, vol_max = 0, t1 = 0, u1 = 0, t2 = 0, u2 = 0, have1 = 0;
    if (running) {                      /* (playing on: this tone's numbers from now) */
        service();
        units_played = 0;
        play_start_ms = now_ms();
    }
    while (done < total) {
        if (stop_asked()) {
            stopped = 1;
            break;
        }
        if (running) {                  /* the volume as it fades, and a steady window for the rate */
            u32 v = SND8(0x6F), el = (u32)(now_ms() - play_start_ms);
            vol_min = v < vol_min ? v : vol_min;
            vol_max = v > vol_max ? v : vol_max;
            service();
            if (!have1 && el >= 200) {
                have1 = 1;
                t1 = el;
                u1 = units_played;
            }
            if (have1) {
                t2 = el;
                u2 = units_played;
            }
        }
        int room = snd_room();
        if (room < 240) {
            delay_us(500);
            continue;
        }
        u32 n = total - done < 240 ? total - done : 240;
        for (u32 i = 0; i < n; i++, done++) {
            int v = sine(phase) >> 2;
            phase += inc;
            u32 g = done < fade ? done : total - done < fade ? total - done : fade;
            buf[i] = (s16)(v * (int)g / (int)fade);
        }
        snd_write(buf, (int)n);
    }
    if (staged) {                       /* the last part frame: padded with silence */
        for (u32 i = staged; i < SND_FRAME; i++)
            stage[i] = 0;
        s16 none = 0;
        staged = SND_FRAME - 1;
        snd_write(&none, 1);
    }
    if (!running && frames_written > f0)
        start();                        /* (a tone shorter than two frames) */
    /* until the DMA reached the last frame's end */
    u64 end = now_ms() + 400;
    while (now_ms() < end && hw_index() != wr && !stop_asked())
        service();
    service();
    u32 el = (u32)(now_ms() - play_start_ms);
    uprintf("snd beep %u Hz %u ms%s: %u frames written (%u dropped), DMA at %u = last %u: %s\n", hz, ms,
            stopped ? " (stopped)" : "", frames_written - f0, frames_dropped - d0, hw_index(), wr,
            hw_index() == wr ? "all played" : "NOT all played");
    if (el && units_played) {           /* (32-bit divisions only: no libgcc) */
        u32 ups = units_played / el * 1000 + units_played % el * 1000 / el;
        u32 f10 = units_played * 10 / FRAME_UNITS;      /* frames played x 10 */
        u32 fps100 = f10 / el * 10000 + f10 % el * 10000 / el;
        uprintf("  the DMA moved %u units in %u ms: %u units/s = %u samples/s (expected 24100 = 48000), "
                "%u.%02u frames/s (expected 50.00)\n", units_played, el, ups, ups * SND_FRAME / FRAME_UNITS,
                fps100 / 100, fps100 % 100);
    }
    if (t2 > t1 + 100) {                /* from 0.2 s after the start to the last write: the steady rate */
        u32 d = u2 - u1, w = t2 - t1;
        u32 ups = d / w * 1000 + d % w * 1000 / w;
        uprintf("  steady window %u ms: %u units = %u units/s = %u samples/s\n", w, d, ups,
                ups * SND_FRAME / FRAME_UNITS);
    }
    if (vol_max)
        uprintf("  the volume now (+0x6F) during the tone: %u..%u (+0x6E = %u)\n", vol_min, vol_max, SND8(0x6E));
    else
        uprintf("  the volume now (+0x6F) stayed 0 during the tone (+0x6E = %u)\n", SND8(0x6E));
    uprintf("  frame interrupts (+0x01 b4) %u, underrun resumes %u, sample count +0x12 %u -> %u, frame "
            "count +0x08 %u -> %u, underrun flag +0x14 b0 = %u\n", frame_irqs - i0, underruns - u0, sc0,
            SND16(0x12), fc0, SND16(0x08), SND8(0x14) & 1);
}

void snd_status(void)
{
    service();
    uprintf("SND 0xB8002000: %s%s; +00 %02x +01 %02x +02 %02x +14 %02x +15 %02x (DMA %s, %u bits, %u ch)\n",
            inited ? "set up" : "not set up", running ? ", playing" : "", SND8(0x00), SND8(0x01), SND8(0x02),
            SND8(0x14), SND8(0x15), SND8(0x15) & 1 ? "on" : "off", SND8(0x15) & 0x20 ? 24 : 16,
            SND8(0x15) & 0x40 ? 8 : 2);
    uprintf("  ring %08x x %u units (+0x1E), DMA index +0x1C %u, last +0x0C %u, frame samples +0x10 %u, "
            "sample count +0x12 %u, frames +0x08 %u, frame length +0x0A %u, info +0x03 %02x, PTS +0x04 %08x\n",
            SND32(0x18), SND16(0x1E), SND16(0x1C), SND16(0x0C), SND16(0x10), SND16(0x12), SND16(0x08),
            SND16(0x0A), SND8(0x03), SND32(0x04));
    uprintf("  S/PDIF +0x20 %02x +0x21 %02x +0x27 %02x ring %08x x %u, index %u; third output +0x150 %02x "
            "+0x161 %02x +0x162 %02x ring %08x x %u, index %u\n", SND8(0x20), SND8(0x21), SND8(0x27),
            SND32(0x60), SND16(0x66), SND16(0x64), SND8(0x150), SND8(0x161), SND8(0x162), SND32(0x154),
            SND16(0x158), SND16(0x15A));
    uprintf("  volume +0x6E %u (now +0x6F %u); DAC +0xF4..+0xFD:", SND8(0x6E), SND8(0x6F));
    for (u32 o = 0xF4; o <= 0xFD; o++)
        uprintf(" %02x", SND8(o));
    uprintf("; clocks 0xB80000B0 %08x; mute GPIO %u %s\n", REG32(SYS_AUDIO_CLK), MUTE_GPIO,
            panel_present() ? "(not this board's)" : (REG32(GPIO0_DO) >> MUTE_GPIO) & 1 ? "high (sound)"
                                                                                   : "low (muted)");
    uprintf("  HDMI: +07 %02x +09 %02x +0A %02x (input %s) +0F %02x (audio %s) +55 %02x +58 %02x, CTS read "
            "%u, N/CTS set at the HDMI start\n", REG8(0xB802A007), REG8(0xB802A009), REG8(0xB802A00A),
            REG8(0xB802A00A) & 2 ? "S/PDIF" : "I2S", REG8(0xB802A00F), REG8(0xB802A00F) & 1 ? "on" : "off",
            REG8(0xB802A055), REG8(0xB802A058),
            REG8(0xB802A059) | REG8(0xB802A05A) << 8 | (REG8(0xB802A05B) & 0x0F) << 16);
    uprintf("  driver: %u frames written, %u dropped, %u queued, %u frame interrupts, %u underrun resumes\n",
            frames_written, frames_dropped, inited ? queued_frames() : 0, frame_irqs, underruns);
    if (SND8(0x15) & 1) {               /* the DMA's speed, measured over 100 ms */
        u32 a = hw_index();
        u64 t = now_ticks();
        delay_ms(100);
        u32 b = hw_index();
        u32 us = (u32)(now_ticks() - t) / (ticks_per_ms() / 1000);
        u32 d = (b + RING_UNITS - a) % RING_UNITS;
        uprintf("  DMA index %u -> %u in %u us: %u units/s%s\n", a, b, us, us ? d * 1000000u / us : 0,
                d ? "" : " (not moving: underrun or stopped)");
    }
}

void snd_command(int argc, char **argv)
{
    const char *c = argc > 1 ? argv[1] : "";
    if (s_eq(c, "init")) {
        if (snd_init(0) == 0)
            uprintf("snd: set up, 48000 Hz, ring %08x (%u frames of %u samples); 'snd beep' plays\n",
                    SND_I2S_PHYS, SND_FRAMES, SND_FRAME);
        return;
    }
    if (s_eq(c, "beep")) {
        u32 hz = 1000, ms = 1000;
        if ((argc > 2 && (!num(argv[2], &hz) || hz < 20 || hz > 20000)) ||
            (argc > 3 && (!num(argv[3], &ms) || !ms || ms > 600000))) {
            uprintf("usage: snd beep [Hz 20..20000] [ms]\n");
            return;
        }
        beep(hz, ms);
        return;
    }
    if (s_eq(c, "status")) {
        snd_status();
        return;
    }
    if (s_eq(c, "stop")) {
        snd_stop();
        uprintf("snd: stopped (muted, DMA off, DAC down)\n");
        return;
    }
    if (s_eq(c, "vol")) {
        u32 v;
        if (argc > 2) {
            if (!num(argv[2], &v) || v > 100) {
                uprintf("usage: snd vol [0..100]\n");
                return;
            }
            volume = (u8)v;
            if (running)
                SND8(0x6E) = volume_reg(volume);
        }
        uprintf("snd: volume %u (register +0x6E = %u)\n", volume, volume_reg(volume));
        return;
    }
    uprintf("snd init                  the sound engine set up (48 kHz; HDMI audio needs 'video init' first)\n"
            "snd beep [Hz] [ms]        a sine tone (1000 Hz, 1000 ms) on HDMI and the SCART's L/R, then what\n"
            "                          the DMA did (Ctrl-C stops it)\n"
            "snd status                the engine's registers, DMA index and speed, the DAC, the HDMI's audio\n"
            "snd stop                  silence: muted, DMA off, the DAC powered down\n"
            "snd vol [0..100]          the volume (100: register 255)\n");
}
