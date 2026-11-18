/* The RF front end of the Comsat TE 1050 HD (dump.bin's board, MC6379-
   VER1.0): antenna -> MaxLinear MxL603 silicon tuner (I2C 0x60 on the
   hardware I2C master SCB0) -> 4.57 MHz IF -> the S3811's own COFDM (DVB-T)
   demodulator, memory-mapped at 0xB803E000 -> its parallel transport stream
   -> the transport-stream input TSI (0xB801A000) -> the demultiplexer DMX0
   (0xB8022000), which writes the packets its PID filters pass into a ring of
   188-byte packets in RAM.

   Everything here is what dump.bin's application (DVB-T HD V1.1.5, Libcore
   8.1c, decompressed to RAM at 0x80000200) does, read from its code and from
   running it in the emulator with every access logged (the addresses quoted
   are in that RAM image):

     SCB I2C driver      0x801B0A0C init, 0x801B0B04 wait idle, 0x801B0B98 wait
                         start, 0x801B0C88 wait done, 0x801B0CF8 read,
                         0x801B0F.. write
     MxLWare603 (MaxLinear's driver, compiled in) 0x8022DC00..0x80231000:
                         0x8022E700 soft reset, 0x8022E724 overwrite defaults
                         (table 0x8073282C), 0x8022E804 crystal, 0x8022F558 IF
                         out, 0x8022EF6C AGC, 0x8022EB3C tuner mode (DVB-T
                         table 0x80732908), 0x8022F134 channel tune,
                         0x8022F714 lock status, 0x8022EA28 version, 0x8022E8DC
                         power mode; ALi's wrapper 0x8022E374 (init) /
                         0x8022E27C (control) / 0x8022E208 (status)
     NIM_S3811 driver    0x8039CD24 attach, 0x8039D214 open, 0x803A0974 set-up
                         (IF, mode), 0x8039D528 channel change, 0x8039ECA0
                         get_lock, 0x8039FB14 signal level, 0x803A0020 error
                         counters, 0x8039FD64 quality, 0x8039ED30 / 0x8039EFB0
                         TPS; register read / write helpers 0x8039C7AC /
                         0x8039C95C
     TSI                 0x8030CD00..0x8030D400 (mode, select, DMX source)
     DMX HAL             0x8038DCC0..0x8038EAE0; its task 0x80303xxx

   The console stays alive in every wait: console_service() keeps the TV's
   console current, now_ms() / delay_ms() feed the watchdog and empty the
   UART (time.c), and Ctrl-C (uart_getc() == 3) stops scans, captures and
   waits. */
#include "hw.h"
#include "rf.h"

/* ------------------------------------------------------------------ */
/* small helpers (no libc)                                            */
/* ------------------------------------------------------------------ */

static int s_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* "0x1f" or "31": 1, 0 not a number */
static int parse_num(const char *s, u32 *out)
{
    u32 v = 0;
    int base = 10, digits = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    for (; *s; s++, digits++) {
        u32 d;
        if (*s >= '0' && *s <= '9')
            d = (u32)(*s - '0');
        else if (base == 16 && *s >= 'a' && *s <= 'f')
            d = (u32)(*s - 'a' + 10);
        else if (base == 16 && *s >= 'A' && *s <= 'F')
            d = (u32)(*s - 'A' + 10);
        else
            return 0;
        v = v * (u32)base + d;
    }
    if (!digits)
        return 0;
    *out = v;
    return 1;
}

/* a frequency: "474" / "474.5" / "177.500" (MHz), "474000" (kHz, from 2000
   on), or a channel number: 21..69 UHF (8 MHz raster, 474 MHz + 8 x (n - 21)),
   5..12 VHF band III (7 MHz raster, 177.5 MHz + 7 x (n - 5)).  *bw gets the
   channel's bandwidth when a channel number was given: 1, 0 not one */
static int parse_khz(const char *s, u32 *khz, u32 *bw)
{
    u32 whole = 0, frac = 0, scale = 1000;
    int digits = 0, dot = 0;
    for (; *s; s++) {
        if (*s == '.' && !dot) {
            dot = 1;
            continue;
        }
        if (*s < '0' || *s > '9')
            return 0;
        if (!dot) {
            whole = whole * 10 + (u32)(*s - '0');
            digits++;
        } else if (scale > 1) {
            scale /= 10;
            frac += (u32)(*s - '0') * scale;
        }
    }
    if (!digits)
        return 0;
    if (!dot && whole >= 21 && whole <= 69) {
        *khz = 474000 + (whole - 21) * 8000;
        *bw = 8;
    } else if (!dot && whole >= 5 && whole <= 12) {
        *khz = 177500 + (whole - 5) * 7000;
        *bw = 7;
    } else if (!dot && whole >= 2000) {
        *khz = whole;                           /* kHz */
    } else {
        *khz = whole * 1000 + frac;
    }
    return 1;
}

/* a string left-aligned in `width` columns (uprintf's %Ns pads on the left) */
static void put_left(const char *s, int width)
{
    int n = 0;
    while (s[n])
        n++;
    uprintf("%s", s);
    while (n++ < width)
        uprintf(" ");
}

/* "474.000" */
static void put_mhz(u32 khz)
{
    uprintf("%u.%03u", khz / 1000, khz % 1000);
}

/* every wait: the TV's console kept current, Ctrl-C asked for?  (now_ms /
   delay_ms feed the watchdog and empty the UART's FIFO themselves, time.c) */
static int stop_asked(void)
{
    console_service();
    if (uart_getc() == 3) {
        uprintf("^C\n");
        return 1;
    }
    return 0;
}

/* ms milliseconds, in 1 ms steps: 1 when Ctrl-C ended it early */
static int wait_ms(u32 ms)
{
    u64 end = now_ms() + ms;
    while (now_ms() < end) {
        delay_us(500);
        if (stop_asked())
            return 1;
    }
    return 0;
}

/* word copy (both 4-byte aligned, n a multiple of 4) */
static void copy_words(u32 *to, const volatile u32 *from, u32 n)
{
    for (n >>= 2; n; n--)
        *to++ = *from++;
}

/* ------------------------------------------------------------------ */
/* SCB: the hardware I2C master (i2c_scb.py in the emulator)          */
/* ------------------------------------------------------------------ */

/* dump.bin's driver (0x801B0A0C..0x801B11BC) as it runs on the S3811:
     init   +2 &= 0xF0 (interrupt enables off), +3 = 0x0F (flags cleared),
            +6..+B = 6 MHz / bit rate (0x3C = 60: 100 kHz for both masters),
            +C = 0x80 (FIFO on), +0 = 0x80 (master enabled)
     write  wait until +1 bit 5 clears (99 x 1 ms; then +0x14 & 7 nonzero =
            the bus is stuck, -34); +0 &= 0x80, +1 = 0, +2 &= 0xF0, +3 = 0x0F,
            +4 = the 8-bit slave address, +5 = the first byte, +0x23 = length
            >> 5, +C = 0x80 | length & 0x1F, every byte into the FIFO +0x10
            (+1 bit 1: FIFO full), +0 = 0xC1 (0x80 | 0x41 = write); then
            wait while +1 & 0xE0 (a transfer under way; +3 & 0x0E = no ACK,
            -9), tried again up to ten times a millisecond apart, and wait for
            +3 bit 0 (done; & 0x0E no ACK) -- 99 x 1 ms each
     read   the same set-up without +5, +0 = 0xC5 (0x80 | 0x45 = read), the
            same wait, then until +C & 0x1F (bytes in the FIFO) is the length
            (16 at most), and each byte from +0x10
   The MxL603 is read as MaxLinear's protocol wants it: a write of 0xFB, reg,
   then a 1-byte read. */
#define SCB0_BASE   0xB8018200u
#define SCB1_BASE   0xB8018700u
#define SCB_DIV     60                  /* 6 MHz / 60 = 100 kHz, as dump.bin (0x5B8D80 / 100000) */
#define I2C_NOACK   -9                  /* the driver's error codes */
#define I2C_TIMEOUT -34

static u32 scb = SCB0_BASE;             /* the master in use */
#define SCB(o) REG8(scb + (o))

static void scb_init(u32 base)
{
    REG8(base + 2) &= 0xF0;
    REG8(base + 3) = 0x0F;
    for (u32 o = 6; o <= 0xB; o++)
        REG8(base + o) = SCB_DIV;
    REG8(base + 0xC) = 0x80;
    REG8(base + 0) = 0x80;
}

static int scb_wait_idle(void)
{
    for (int i = 0; i < 99; i++) {
        if (!(SCB(1) & 0x20))
            return 0;
        delay_ms(1);
    }
    return (SCB(0x14) & 7) ? I2C_TIMEOUT : 0;
}

static void scb_setup(u8 addr8, int n)
{
    SCB(0) &= 0x80;
    SCB(1) = 0;
    SCB(2) &= 0xF0;
    SCB(3) = 0x0F;
    SCB(4) = addr8;
    (void)n;
}

static int scb_wait_start(void)
{
    for (int i = 0; i < 99; i++) {
        if (!(SCB(1) & 0xE0))
            return 0;
        if (SCB(3) & 0x0E)
            return I2C_NOACK;
        delay_ms(1);
    }
    return I2C_TIMEOUT;
}

static int scb_wait_done(void)
{
    for (int i = 0; i < 99; i++) {
        u8 f = SCB(3);
        if (f & 1)
            return 0;
        if (f & 0x0E)
            return I2C_NOACK;
        delay_ms(1);
    }
    return I2C_TIMEOUT;
}

/* n bytes (1..31) to the slave with 8-bit address addr8: 0, -9 no ACK, -34 */
static int scb_write(u8 addr8, const u8 *d, int n)
{
    if (scb_wait_idle() < 0)
        return I2C_TIMEOUT;
    for (int tries = 10;;) {
        scb_setup(addr8, n);
        SCB(5) = d[0];
        SCB(0x23) = (u8)(n >> 5);
        SCB(0xC) = (u8)(0x80 | (n & 0x1F));
        for (int i = 0; i < n; i++) {
            if (SCB(1) & 2)
                return I2C_NOACK;
            SCB(0x10) = d[i];
        }
        SCB(0) = (u8)((SCB(0) & 0x80) | 0x41);
        if (scb_wait_start() == 0)
            break;
        if (--tries <= 0)
            return I2C_TIMEOUT;
        delay_ms(1);
    }
    return scb_wait_done();
}

/* n bytes (1..16) from the slave: 0, -9, -34 */
static int scb_read(u8 addr8, u8 *d, int n)
{
    int r;
    if (scb_wait_idle() < 0)
        return I2C_TIMEOUT;
    for (int tries = 10;;) {
        scb_setup(addr8, n);
        SCB(0x23) = (u8)(n >> 5);
        SCB(0xC) = (u8)(0x80 | (n & 0x1F));
        SCB(0) = (u8)((SCB(0) & 0x80) | 0x45);
        r = scb_wait_start();
        if (r == 0)
            break;
        if (--tries <= 0)
            return r;
        delay_ms(1);
    }
    int want = n > 16 ? 16 : n, i;
    for (i = 0; i < 99 && (SCB(0xC) & 0x1F) != want; i++)
        delay_ms(1);
    if (i == 99)
        return (SCB(3) & 0x0E) ? I2C_NOACK : I2C_TIMEOUT;
    for (int k = 0; k < n; k++) {
        for (i = 0; i < 100 && !(SCB(0xC) & 0x1F); i++)
            delay_ms(1);
        d[k] = SCB(0x10);
    }
    return 0;
}

static const char *i2c_error(int r)
{
    return r == I2C_NOACK ? "no ACK" : r == I2C_TIMEOUT ? "timeout (bus busy?)" : "error";
}

/* ------------------------------------------------------------------ */
/* the MxL603 tuner                                                   */
/* ------------------------------------------------------------------ */

/* I2C address 0x60 (8-bit 0xC0).  A write is (register, value) pairs, one
   pair per transfer as the driver sends them; a read is the pair (0xFB, reg)
   then one byte.  Register 0x00 selects the page (1 for 0x31, 0x96, 0x5F,
   0x60 here).  The board's tuner set-up (the ALi wrapper's
   COFDM_TUNER_CONFIG_EXT): 16 MHz crystal (20 pF load, no clock out), IF out
   4.57 MHz (MxLWare's IF code 5), IF gain code 11, self AGC with set point
   0x42, single 3.3 V supply, DVB-T mode, loop-through on. */
#define MXL_ADDR8   0xC0
#define MXL_IF_KHZ  4570

static int mxl_err;                     /* the first I2C error of a sequence */

static void mxl_w(u8 reg, u8 val)
{
    u8 b[2] = {reg, val};
    int r = scb_write(MXL_ADDR8, b, 2);
    if (r && !mxl_err)
        mxl_err = r;
}

static u8 mxl_r(u8 reg)
{
    u8 b[2] = {0xFB, reg}, v = 0;
    int r = scb_write(MXL_ADDR8, b, 2);
    if (!r)
        r = scb_read(MXL_ADDR8, &v, 1);
    if (r && !mxl_err)
        mxl_err = r;
    return v;
}

/* MxLWare603_API_CfgDevOverwriteDefaults' table (dump.bin 0x8073282C:
   {register, mask 0xFF, value}, written in order, the last three on page 1
   and back) */
static const u8 mxl_defaults[][2] = {
    {0x14, 0x13}, {0x6D, 0x8A}, {0x6D, 0x0A}, {0xDF, 0x19}, {0x45, 0x1B}, {0xA9, 0x59},
    {0xAA, 0x6A}, {0xBE, 0x4C}, {0xCF, 0x25}, {0xD0, 0x34}, {0x77, 0xE7}, {0x78, 0xE3},
    {0x6F, 0x51}, {0x7B, 0x84}, {0x7C, 0x9F}, {0x56, 0x41}, {0xCD, 0x64}, {0xC3, 0x2C},
    {0x9D, 0x61}, {0xF7, 0x52}, {0x58, 0x81}, {0x00, 0x01}, {0x62, 0x02}, {0x00, 0x00},
};

/* MxLWare603_API_CfgTunerMode's table for DVB-T (signal mode 2; dump.bin
   0x80732908) */
static const u8 mxl_dvbt[][2] = {
    {0x0C, 0x00}, {0x13, 0x04}, {0x53, 0xFE}, {0x57, 0x91}, {0x62, 0xC2}, {0x6E, 0x01},
    {0x6F, 0x51}, {0x87, 0x77}, {0x88, 0x55}, {0x93, 0x22}, {0x97, 0x02}, {0xBA, 0x30},
    {0x98, 0xAF}, {0x9B, 0x20}, {0x9C, 0x1E}, {0xA0, 0x18}, {0xA5, 0x09}, {0xC2, 0xA9},
    {0xC5, 0x7C}, {0xD5, 0x03}, {0xD9, 0x04},
};

/* MxLWare603_API_CfgTunerChanTune (0x8022F134) for DVB-T: the RF in 1/64 MHz
   at 0x10/0x11, the bandwidth code at 0x0F (0x20 / 0x21 / 0x22 = 6 / 7 / 8
   MHz terrestrial), 0x12 = 1 starts the tune; around it the LNA / spur
   settings of page 1 (0x7C, 0x31 by the frequency: below / above 700 MHz;
   0x60, 0x5F and 0xB6 by page-1 register 0x96 bit 4, the loop-through) */
static void mxl_chan_tune(u32 khz, u32 bw_mhz)
{
    u8 bwcode = bw_mhz == 6 ? 0x20 : bw_mhz == 7 ? 0x21 : 0x22;
    u32 word = khz * 64 / 1000;         /* = (Hz << 6) / 1000000, as the driver's 64-bit division */
    u8 r31;
    mxl_w(0x12, 0x00);                  /* tune stopped */
    mxl_w(0x0E, 0x14);                  /* (the single-supply flag's write, every tune) */
    if (khz <= 700000) {
        mxl_w(0x7C, 0x1F);
        r31 = 0x81;                     /* DVB-T (DVB-C would be 0xC1) */
    } else {
        mxl_w(0x7C, 0x9F);
        r31 = 0x91;                     /* (DVB-C 0xD1) */
    }
    mxl_w(0x00, 0x01);
    mxl_w(0x31, r31);
    mxl_w(0x00, 0x00);
    mxl_w(0x0F, bwcode);
    mxl_w(0x10, (u8)word);
    mxl_w(0x11, (u8)(word >> 8));
    mxl_w(0x0B, 0x01);
    mxl_w(0x00, 0x01);
    u8 r96 = mxl_r(0x96);
    mxl_w(0x00, 0x00);
    u8 rb6 = mxl_r(0xB6);
    mxl_w(0x00, 0x01);
    u8 r60 = mxl_r(0x60), r5f = mxl_r(0x5F);
    if (r96 & 0x10) {
        rb6 = (u8)((rb6 & 0xBF) | 0x0E);
        r60 = (u8)((r60 & 0xC0) | 0x0E);
        r5f = (u8)((r5f & 0xC0) | 0x0E);
    } else {
        rb6 = (u8)((rb6 | 0x40) & 0xC0);
        r60 = (u8)((r60 & 0xC0) | 0x37);
        r5f = (u8)((r5f & 0xC0) | 0x37);
    }
    mxl_w(0x60, r60);
    mxl_w(0x5F, r5f);
    mxl_w(0x00, 0x00);
    mxl_w(0xB6, rb6);
    mxl_w(0x12, 0x01);                  /* start the tune */
    mxl_w(0x0E, 0x14);
    delay_ms(15);
    mxl_w(0xB6, (u8)(rb6 | 0x40));
}

/* the whole start of the tuner as dump.bin's first channel change does it
   (ALi's wrapper 0x8022E374), after the power-mode write its open makes */
static int mxl_init(void)
{
    mxl_err = 0;
    mxl_w(0x0B, 0x01);                  /* CfgDevPowerMode(active): 0x0B = 1, 0x12 = 1 (ioctl 0x8001, 0) */
    mxl_w(0x12, 0x01);
    mxl_w(0xFF, 0x00);                  /* CfgDevSoftReset */
    for (unsigned i = 0; i < sizeof(mxl_defaults) / sizeof(mxl_defaults[0]); i++)
        mxl_w(mxl_defaults[i][0], mxl_defaults[i][1]);
    mxl_w(0x00, 0x01);                  /* page 1: 0x31 |= 0xD0 */
    mxl_w(0x31, (u8)(mxl_r(0x31) | 0xD0));
    mxl_w(0x00, 0x00);
    mxl_w(0x0E, 0x04);                  /* single 3.3 V supply */
    mxl_w(0x01, 0x14);                  /* CfgDevXtal: 16 MHz (code 0 << 5), cap 20 pF; clock out off */
    mxl_w(0x02, 0x00);
    mxl_w(0x0E, 0x14);
    mxl_w(0x04, (u8)((mxl_r(0x04) & 0xC0) | 0x05));     /* CfgTunerIFOutParam: IF 4.57 MHz, not inverted */
    mxl_w(0x05, 0x2B);                  /* IF gain (code 11) */
    mxl_w(0x08, (u8)((mxl_r(0x08) & 0xF2) | 0x05));     /* CfgTunerAGC: self AGC */
    mxl_w(0x09, (u8)((mxl_r(0x09) & 0x80) | 0x42));     /* AGC set point 0x42 */
    mxl_w(0x5E, (u8)(mxl_r(0x5E) & 0xEF));
    for (unsigned i = 0; i < sizeof(mxl_dvbt) / sizeof(mxl_dvbt[0]); i++)
        mxl_w(mxl_dvbt[i][0], mxl_dvbt[i][1]);
    mxl_w(0x5A, 0xFE);                  /* the IF filter for 4.57 MHz */
    mxl_w(0x5B, 0x18);
    mxl_w(0x5C, 0xF1);
    mxl_w(0xEA, 0x0D);                  /* (16 MHz crystal) */
    mxl_w(0xDC, 0x00);
    mxl_w(0x03, 0x00);                  /* the digital part restarted */
    mxl_w(0x03, 0x01);
    delay_ms(50);
    mxl_chan_tune(474000, 8);           /* the wrapper's first tune, 474 MHz, then ... */
    delay_ms(15);
    mxl_w(0x00, 0x01);                  /* ... page-1 0x96 = 0x10 (0x8022F090(1): loop-through on) */
    mxl_w(0x96, 0x10);
    mxl_w(0x00, 0x00);
    return mxl_err;
}

/* register 0x2B: bit 1 RF synthesizer locked, bit 0 reference locked (the
   wrapper wants both) */
static u8 mxl_lock(void)
{
    return (u8)(mxl_r(0x2B) & 3);
}

/* ------------------------------------------------------------------ */
/* the S3811 COFDM demodulator                                        */
/* ------------------------------------------------------------------ */

/* Its registers are bytes at 0xB803E000 + register (0x000..0x147 used).  The
   driver writes a byte, waits 1 us, reads register 0x3B and waits 1 us again
   (0x8039C95C); it reads a byte and waits 1 us (0x8039C7AC).  A write of
   register 0x10 with bit 7 CLEAR puts the demodulator to sleep -- the driver
   then also changes 0xB8000088 (bits 0x710 -> 0x200), 0xB800008C (|=
   0x41000000) and 0xB8000430 (= 0x80000000) and restores them on the next
   write with bit 7 set: here 0x10 is only ever written 0x80.

     0x00  b7, then b6: acquisition restart (|= 0x80, then |= 0x40)
     0x01  |= 0x48 at set-up             0x02  events (written 0 at a tune)
     0x03  0                             0x09..0x0D error counters (below)
     0x10  0x80 (awake)                  0x12  b7 = DVB-T (set), read back
     0x13  b5..4 bandwidth 1/2/3 = 6/7/8 MHz, b3..0 the IF range (2: 3.5-4.6 MHz)
     0x14..0x16  the IF: frac(IF / 1.6875 MHz) x 2^24, big-endian (0xB54932 for
           4.57 MHz; the driver's table also has 3.57, 4.0, 36 and 44 MHz)
     0x17, 0xC4  0 / 0x60 when 0x3B == 1 and 0x147 != 0 (else 0x19 &= ~0x40,
           0xF0 |= 0x10) -- 0x3B and 0x147 tell the silicon apart
     0x1B  b4 = decode the high-priority stream
     0x1D  b5 = OFDM / TPS lock ("get_lock"), b6 = FEC lock (stream out)
     0x1E  b5..4 FFT (0 2K, 1 8K, 2 4K), b3 spectral inversion, b2..0 + 0x1F:
           the carrier offset (11-bit signed)
     0x20  b1..0 guard interval (0 1/32, 1 1/16, 2 1/8, 3 1/4)
     0x28/0x29  0xFF / 0x00, 0x2A |= 2 (b5 clear: the board's AGC set-up)
     0x30/0x31  IF AGC (10 bits, offset binary: + 0x200)
     0x73  b5..3 code rate HP, b2..0 LP (0 1/2, 1 2/3, 2 3/4, 3 5/6, 4 7/8)
     0x74  b3..2 constellation (0 QPSK, 1 16-QAM, 2 64-QAM), b1..0 hierarchy
     0xF8  &= 0x7F                     */
#define DEMOD 0xB803E000u

static u8 dm_r(u32 reg)
{
    u8 v = REG8(DEMOD + reg);
    delay_us(1);
    return v;
}

static void dm_w(u32 reg, u8 v)
{
    REG8(DEMOD + reg) = v;
    delay_us(1);
    (void)REG8(DEMOD + 0x3B);
    delay_us(1);
}

/* the IF word for 0x14..0x16 (the driver's table entry for 4.52..4.62 MHz is
   B5 49 32: this formula gives the same) */
static u32 dm_if_word(u32 if_khz)
{
    /* frac(if / 1687.5 kHz) * 2^24 = ((2 * if) mod 3375) * 2^24 / 3375 */
    u32 rem = (2 * if_khz) % 3375;
    u32 q = 0;
    /* long division of rem * 2^24 by 3375 without 64-bit arithmetic */
    for (int bit = 23; bit >= 0; bit--) {
        rem <<= 1;
        if (rem >= 3375) {
            rem -= 3375;
            q |= 1u << bit;
        }
    }
    return q;
}

/* the set-up the driver repeats before each acquisition (0x803A0974), for this
   board: DVB-T, the tuner's 4.57 MHz IF, no external AGC options */
static void dm_setup(void)
{
    u32 ifw = dm_if_word(MXL_IF_KHZ);
    dm_w(0x12, (u8)((dm_r(0x12) & 0x7F) | 0x80));      /* DVB-T */
    dm_w(0xF8, (u8)(dm_r(0xF8) & 0x7F));
    dm_w(0x14, (u8)(ifw >> 16));
    dm_w(0x15, (u8)(ifw >> 8));
    dm_w(0x16, (u8)ifw);
    dm_w(0x13, (u8)((dm_r(0x13) & 0xF0) | 0x02));      /* IF range 3.5..4.6 MHz */
    dm_w(0x2A, (u8)(dm_r(0x2A) & 0xDF));
    dm_w(0x2A, (u8)(dm_r(0x2A) | 0x02));
    dm_w(0x28, 0xFF);
    dm_w(0x29, 0x00);
    dm_w(0x01, (u8)(dm_r(0x01) | 0x48));
    dm_w(0x03, 0x00);
    if (dm_r(0x3B) == 1) {
        if (dm_r(0x147)) {
            dm_w(0x17, 0x00);
            dm_w(0xC4, 0x60);
        } else {
            dm_w(0x19, (u8)(dm_r(0x19) & 0xBF));
            dm_w(0xF0, (u8)(dm_r(0xF0) | 0x10));
        }
    }
    dm_w(0x10, 0x80);                   /* awake */
}

/* the acquisition (0x8039DBA4..): restart, events cleared, the bandwidth, the
   HP stream, start */
static void dm_acquire(u32 bw_mhz)
{
    dm_w(0x00, (u8)(dm_r(0x00) | 0x80));
    dm_w(0x02, 0x00);
    u8 b = bw_mhz == 6 ? 0x10 : bw_mhz == 7 ? 0x20 : 0x30;
    dm_w(0x13, (u8)((dm_r(0x13) & 0xCF) | b));
    dm_w(0x1B, (u8)(dm_r(0x1B) | 0x10));
    dm_w(0x00, (u8)(dm_r(0x00) | 0x40));
}

/* the signal level in % from the IF AGC (0x8039FB14 without the board's
   tables, which this one has none of: (agc - 50) * 100 / 1150, clamped to
   1..99, from 100) */
static u32 dm_level(u32 *agc_out)
{
    u8 lo = dm_r(0x30), hi = dm_r(0x31);
    u32 agc = ((((u32)hi & 3) << 8) | lo) + 0x200;
    agc &= 0x3FF;
    if (agc_out)
        *agc_out = agc;
    s32 x = ((s32)agc - 50) * 100 / 1150;
    if (x <= 0)
        x = 1;
    if (x >= 100)
        x = 99;
    return (u32)(100 - x);
}

/* the error counters (0x803A0020; only with the FEC locked, 0x1D bit 6): 0x0B
   b7 = counting (busy); else the bit errors are (0x0B & 0x7F, 0x0C, 0x0D) >>
   1 and 0x0B = 0x80 starts the next period; 0x0A b7 busy, else (0x0A & 0x7F,
   0x09) uncorrected packets and 0x0A = 0x80 starts again.  1 when both were
   read */
static u32 ber_count, per_count;
static int ber_valid, per_valid;

static void dm_errors(void)
{
    u8 b = dm_r(0x0B);
    if (!(b & 0x80)) {
        u8 c = dm_r(0x0C), d = dm_r(0x0D);
        ber_count = ((((u32)b & 0x7F) << 16) | ((u32)c << 8) | d) >> 1;
        ber_valid = 1;
        dm_w(0x0B, 0x80);
    }
    b = dm_r(0x0A);
    if (!(b & 0x80)) {
        per_count = (((u32)b & 0x7F) << 8) | dm_r(0x09);
        per_valid = 1;
        dm_w(0x0A, 0x80);
    }
}

/* the driver's "SNR" (0x8039FD64): a quality figure 25..97 from those counts
   (its constants: 1500 / 500 packets, 8000 bits, 35, 55, 56, 97) */
static u32 dm_quality(void)
{
    if (per_count >= 1500)
        return 25;
    if (per_count > 500)
        return 35 - (per_count - 500) * 10 / 1000;
    if (per_count)
        return 55 - per_count * 20 / 500;
    if (ber_count < 8000)
        return 97 - ber_count * 41 / 8000;
    return 56;
}

static const char *fft_name(u8 r1e)
{
    switch ((r1e >> 4) & 3) {
    case 0: return "2K";
    case 1: return "8K";
    case 2: return "4K";
    }
    return "?";
}

static const char *const gi_names[4] = {"1/32", "1/16", "1/8", "1/4"};
static const char *const qam_names[4] = {"QPSK", "16-QAM", "64-QAM", "?"};
static const char *const cr_names[8] = {"1/2", "2/3", "3/4", "5/6", "7/8", "?", "?", "?"};
static const char *const hier_names[4] = {"none", "a=1", "a=2", "a=4"};

/* ------------------------------------------------------------------ */
/* state, init, tune                                                  */
/* ------------------------------------------------------------------ */

static int rf_ready;                    /* rf init done: the tuner answered */
static u32 cur_khz, cur_bw;             /* the last tune */

#define SYS_REG(o) REG32(0xB8000000u + (o))

/* TSI (0xB801A000) as dump.bin's board set-up leaves it (0x8000DC6C ->
   0x8030CC48 / 0x8030CFDC / 0x8030D22C): +0 = 0x87 the attributes of the
   parallel input SPI_0, which the internal demodulator's stream comes in on
   (b7 on; b2..0 error / sync / valid active high), +8 b6..0 = 0 TS_A's
   source = SPI_0, +0xB b0 = 0 DMX0 takes TS_A */
#define TSI_BASE 0xB801A000u

static void tsi_route(void)
{
    REG8(TSI_BASE + 0x00) = 0x87;
    REG8(TSI_BASE + 0x08) = (u8)(REG8(TSI_BASE + 0x08) & 0x80);
    if (REG8(TSI_BASE + 0x0B) & 1)
        REG8(TSI_BASE + 0x0B) = (u8)(REG8(TSI_BASE + 0x0B) & 0x12);
}

static int rf_init(int verbose)
{
    scb = SCB0_BASE;
    scb_init(SCB0_BASE);
    scb_init(SCB1_BASE);                /* (dump.bin starts both masters the same way) */
    u8 id = 0, ver = 0;
    mxl_err = 0;
    id = mxl_r(0x18);                   /* MxLWare603_API_ReqDevVersionInfo: chip id 0x18, version 0x1A */
    if (mxl_err) {
        uprintf("rf: the tuner at I2C 0x60 (SCB0 0xB8018200) does not answer: %s\n", i2c_error(mxl_err));
        rf_ready = 0;
        return -1;
    }
    ver = mxl_r(0x1A);
    if (verbose)
        uprintf("tuner: MxL603 at I2C 0x60 ACKs; chip id (0x18) = 0x%02x, version (0x1A) = 0x%02x\n", id, ver);
    if (mxl_init()) {
        uprintf("rf: tuner set-up failed: %s\n", i2c_error(mxl_err));
        rf_ready = 0;
        return -1;
    }
    cur_khz = 474000;                   /* (mxl_init's own first tune) */
    cur_bw = 8;
    if (verbose)
        uprintf("tuner: set up (16 MHz crystal, IF 4.57 MHz, DVB-T), first tune 474 MHz: lock 0x2B = %u\n",
                mxl_lock());
    /* the demodulator: the system bits its sleep path changes (see above) as
       they are now, the silicon registers, its set-up */
    u32 s88 = SYS_REG(0x88), s8c = SYS_REG(0x8C), s430 = SYS_REG(0x430);
    if (s430 & 0x80000000u) {
        SYS_REG(0x430) = 0;             /* dump.bin's HAL init writes 0 there (0x8000EB84) */
        if (verbose)
            uprintf("demod: 0xB8000430 was 0x%08x (its sleep value): cleared\n", s430);
    }
    u8 r3b = dm_r(0x3B), r147 = dm_r(0x147);
    if (verbose)
        uprintf("demod: S3811 COFDM at 0xB803E000; 0x3B = 0x%02x, 0x147 = 0x%02x; 0xB8000088 = 0x%08x, "
                "0xB800008C = 0x%08x\n", r3b, r147, s88, s8c);
    dm_setup();
    tsi_route();
    if (verbose)
        uprintf("demod: set up (DVB-T, IF word 0x%06x); TSI: SPI_0 -> TS_A -> DMX0\n", dm_if_word(MXL_IF_KHZ));
    rf_ready = 1;
    return 0;
}

static int need_init(void)
{
    return rf_ready ? 0 : rf_init(0);
}

/* 0 locked (FEC), 1 OFDM only, 2 nothing, 3 the tuner did not lock, -1
   Ctrl-C.  progress: print the lock register as it changes */
#define TUNE_NO_TUNER 3
static int rf_tune(u32 khz, u32 bw, int progress, u32 *took_ms)
{
    u64 t0 = now_ms();
    mxl_err = 0;
    mxl_chan_tune(khz, bw);
    delay_ms(15);                       /* (the wrapper's own 15 ms after the tune) */
    cur_khz = khz;
    cur_bw = bw;
    u8 tl = 0;
    for (int i = 0; i < 10 && (tl = mxl_lock()) != 3; i++)
        delay_ms(10);
    if (tl != 3) {
        if (progress)
            uprintf("tuner: not locked (0x2B = %u)%s\n", tl, mxl_err ? ": no I2C answer" : "");
        if (took_ms)
            *took_ms = (u32)(now_ms() - t0);
        return TUNE_NO_TUNER;
    }
    dm_setup();
    dm_acquire(bw);
    /* the driver: 70 x 10 ms for 0x1D bit 5, then 125 x 10 ms for bit 6 */
    u8 last = 0xFF, r = 0;
    int state = 2;
    u64 start = now_ms();
    for (;;) {
        r = dm_r(0x1D);
        u32 t = (u32)(now_ms() - start);
        if (progress && r != last)
            uprintf("  %4u ms  0x1D = 0x%02x  %s\n", t, r,
                    (r & 0x40) ? "FEC lock" : (r & 0x20) ? "OFDM lock" : "searching");
        last = r;
        if (r & 0x40) {
            state = 0;
            break;
        }
        if (r & 0x20)
            state = 1;
        if ((!(r & 0x20) && t >= 700) || t >= 700 + 1250)
            break;
        if (wait_ms(10)) {
            state = -1;
            break;
        }
    }
    if (took_ms)
        *took_ms = (u32)(now_ms() - t0);
    return state;
}

/* ------------------------------------------------------------------ */
/* DMX0: packets into a ring in RAM                                   */
/* ------------------------------------------------------------------ */

/* dump.bin's DMX HAL (0x8038DCC0..) as its task sets it up (0x80303F90):
     +0 word |= 0x01000000 (+3 b0 reset), until it clears
     +0 b1..0 = 2, b2 = 0; +2 = (+2 & 0xF8) | 4; +1 = 0x47 (the sync byte);
     +0x16 = +0x1A = 0x2C (FIFO thresholds)
     +0x40..+0x45 one enable bit per filter (48), +0x50..+0x55 another set (0)
     +0xB0 |= 0x10000 (one "united" ring for all filters), +0xC0 = its
     physical address (16-byte aligned), +0x140 = its length in packets,
     +0x142 = 0x50 the interrupt threshold
     +0x300 = 0 (threshold interrupt off: the task polls), +0x320 = 1,
     +0x340 = 3; +0x36 = 0 and +0 b7 = 0 (no PCR filter), +0x350 = 0; +2 |= 8
     +0x1C2 + 4 ch = the read pointers (0); +0 b5 = 1: on
   A PID filter (dmx_hal_set_ram 0x8038E268..): +0x64/+0x68 the 8 bytes (TS
   header bytes 1..), +0x60 = type | filter (0x00 mask, 0x40 value), +0x62 =
   2 writes it (reads 0 when done); the driver's PID filter is mask 1F FF 00
   .., value = the PID.  The hardware writes the packet count it has put in
   the ring at +0x1C0 (13 bits); the driver writes how far it has read at
   +0x1C2.  Overflow status at +0x344 (b1..0). */
#define DMX 0xB8022000u
#define TS_PACKET 188

static u32 ring_rd;                     /* our read pointer (packets) */

static void dmx_ram(u8 sel, u32 w0, u32 w1)
{
    REG32(DMX + 0x64) = w0;
    REG32(DMX + 0x68) = w1;
    REG8(DMX + 0x60) = sel;
    REG8(DMX + 0x62) = 2;
    for (int i = 0; i < 10000 && (REG8(DMX + 0x62) & 2); i++) { }
}

/* filter idx passes `pid` (0..0x1FFF), or every packet with pid == 0xFFFF
   (mask all zero) */
static void dmx_filter(int idx, u32 pid)
{
    if (pid == 0xFFFF) {
        dmx_ram((u8)idx, 0, 0);
        dmx_ram((u8)(0x40 | idx), 0, 0);
    } else {
        dmx_ram((u8)idx, 0x0000FF1Fu, 0);
        dmx_ram((u8)(0x40 | idx), ((pid >> 8) & 0x1F) | ((pid & 0xFF) << 8), 0);
    }
    REG8(DMX + 0x40 + (u32)(idx >> 3)) |= (u8)(1u << (idx & 7));
}

static void dmx_setup(void)
{
    REG32(DMX) |= 0x01000000u;          /* reset */
    for (int i = 0; i < 20000 && (REG32(DMX) & 0x01000000u); i++) { }
    REG8(DMX + 0) = (u8)((REG8(DMX + 0) & 0xFC) | 2);
    REG8(DMX + 0) &= (u8)~4;
    REG8(DMX + 2) = (u8)((REG8(DMX + 2) & 0xF8) | 4);
    REG8(DMX + 1) = 0x47;
    REG8(DMX + 0x16) = 0x2C;
    REG8(DMX + 0x1A) = 0x2C;
    for (u32 i = 0; i < 6; i++) {
        REG8(DMX + 0x40 + i) = 0;
        REG8(DMX + 0x50 + i) = 0;
    }
    REG32(DMX + 0xB0) |= 0x10000u;
    REG32(DMX + 0xC0) = RF_RING_PHYS & 0x0FFFFFF0u;
    REG16(DMX + 0x140) = RF_RING_PACKETS;
    REG16(DMX + 0x142) = 0x50;
    REG8(DMX + 0x300) = 0;
    REG8(DMX + 0x320) = 1;
    REG8(DMX + 0x340) = 3;
    REG8(DMX + 0) &= 0x7F;              /* no PCR filter */
    REG8(DMX + 0x36) = 0;
    REG8(DMX + 0x350) = 0;
    REG8(DMX + 2) |= 8;
    for (u32 ch = 1; ch < 48; ch++)
        REG16(DMX + 0x1C2 + 4 * ch) = 0;
    ring_rd = REG16(DMX + 0x1C0) & 0x1FFF;
    if (ring_rd >= RF_RING_PACKETS)
        ring_rd = 0;
    REG16(DMX + 0x1C2) = (u16)ring_rd;
    REG8(DMX + 0) |= 0x20;              /* on */
}

static void dmx_stop(void)
{
    REG8(DMX + 0) &= (u8)~0x20;
    for (u32 i = 0; i < 6; i++)
        REG8(DMX + 0x40 + i) = 0;
}

/* packets waiting in the ring */
static u32 dmx_waiting(void)
{
    u32 wr = REG16(DMX + 0x1C0) & 0x1FFF;
    if (wr >= RF_RING_PACKETS)
        return 0;
    return (wr + RF_RING_PACKETS - ring_rd) % RF_RING_PACKETS;
}

static const volatile u8 *dmx_packet(u32 i)
{
    return (const volatile u8 *)KSEG1(RF_RING_PHYS + i * TS_PACKET);
}

static void dmx_consumed(u32 n)
{
    ring_rd = (ring_rd + n) % RF_RING_PACKETS;
    REG16(DMX + 0x1C2) = (u16)ring_rd;
}

/* ------------------------------------------------------------------ */
/* commands                                                           */
/* ------------------------------------------------------------------ */

static void show_status(void)
{
    u8 r1d = dm_r(0x1D), r1e = dm_r(0x1E), r20 = dm_r(0x20), r73 = dm_r(0x73), r74 = dm_r(0x74);
    u32 agc, level = dm_level(&agc);
    uprintf("frequency ");
    put_mhz(cur_khz);
    uprintf(" MHz, %u MHz; tuner %s; demod 0x1D = 0x%02x: %s\n", cur_bw, mxl_lock() == 3 ? "locked" : "NOT locked",
            r1d, (r1d & 0x40) ? "locked (FEC: the stream comes out)" : (r1d & 0x20) ? "OFDM lock only" : "no lock");
    uprintf("level %u%% (IF AGC 0x%03x)", level, agc);
    if (r1d & 0x20)
        uprintf("; %s, guard %s, %s, code rate %s (LP %s), hierarchy %s%s", fft_name(r1e), gi_names[r20 & 3],
                qam_names[(r74 >> 2) & 3], cr_names[(r73 >> 3) & 7], cr_names[r73 & 7], hier_names[r74 & 3],
                (r1e & 8) ? ", inverted" : "");
    uprintf("\n");
    if (!(r1d & 0x40))
        return;
    /* a fresh count: the one under way finished, the next one waited for */
    ber_valid = per_valid = 0;
    dm_errors();
    ber_valid = per_valid = 0;
    u64 t = now_ms();
    while ((!ber_valid || !per_valid) && now_ms() - t < 2000) {
        dm_errors();
        if (stop_asked())
            return;
        delay_ms(5);
    }
    if (ber_valid || per_valid)
        uprintf("quality %u%% (bit errors %u, uncorrected packets %u in the last period)\n", dm_quality(),
                ber_count, per_count);
    else
        uprintf("quality: the error counters did not finish a period in 2 s (0x0B = 0x%02x, 0x0A = 0x%02x)\n",
                dm_r(0x0B), dm_r(0x0A));
}

static int cmd_tune(int argc, char **argv)
{
    u32 khz, bw = 8;
    if (argc < 3 || !parse_khz(argv[2], &khz, &bw) || (argc > 3 && (!parse_num(argv[3], &bw)))) {
        uprintf("usage: rf tune <MHz | kHz | channel 21..69 / 5..12> [6|7|8 MHz]\n");
        return -1;
    }
    if (bw < 6 || bw > 8 || khz < 44000 || khz > 1002000) {
        uprintf("rf: 44..1002 MHz, bandwidth 6, 7 or 8 MHz\n");
        return -1;
    }
    if (need_init())
        return -1;
    uprintf("tuning ");
    put_mhz(khz);
    uprintf(" MHz, %u MHz\n", bw);
    u32 ms;
    int st = rf_tune(khz, bw, 1, &ms);
    if (st < 0)
        return 0;
    uprintf("%s after %u ms\n", st == 0 ? "LOCKED" : st == 1 ? "OFDM lock only (no FEC lock)" :
            st == TUNE_NO_TUNER ? "tuner not locked" : "no signal", ms);
    if (st != TUNE_NO_TUNER)
        show_status();
    return 0;
}

static int cmd_scan(int argc, char **argv)
{
    u32 from = 474000, to = 858000, step = 8000, bw = 8;
    int vhf = 0, uhf = 1;
    if (argc > 2 && (s_eq(argv[2], "vhf") || s_eq(argv[2], "all"))) {
        vhf = 1;
        uhf = s_eq(argv[2], "all");
    } else if (argc > 3) {
        u32 b;
        if (!parse_khz(argv[2], &from, &b) || !parse_khz(argv[3], &to, &b) || from > to) {
            uprintf("usage: rf scan [vhf | all | <from MHz> <to MHz>]\n");
            return -1;
        }
        if (to < 300000) {
            step = 7000;
            bw = 7;
        }
    } else if (argc > 2) {
        uprintf("usage: rf scan [vhf | all | <from MHz> <to MHz>]\n");
        return -1;
    }
    if (need_init())
        return -1;
    static u32 found[64], found_lvl[64], found_q[64];
    static u8 found_bw[64];
    int nf = 0;
    uprintf("scanning%s%s (Ctrl-C stops)\n", vhf ? " VHF band III 177.5..226.5 MHz (7 MHz)" : "",
            uhf ? (vhf ? " and UHF 474..858 MHz (8 MHz)" : "") : "");
    for (int band = 0; band < 2; band++) {
        u32 f0 = from, f1 = to, st = step, b = bw;
        if (band == 0) {
            if (!vhf)
                continue;
            f0 = 177500, f1 = 226500, st = 7000, b = 7;
        } else if (!uhf) {
            continue;
        }
        for (u32 f = f0; f <= f1; f += st) {
            u32 ms;
            int s = rf_tune(f, b, 0, &ms);
            if (s < 0)
                goto done;
            u32 lvl = dm_level(0);
            char ch[8];
            int c = b == 8 && f >= 474000 && f <= 858000 && (f - 474000) % 8000 == 0 ? (int)((f - 474000) / 8000 + 21)
                    : b == 7 && f >= 177500 && f <= 226500 && (f - 177500) % 7000 == 0 ? (int)((f - 177500) / 7000 + 5)
                    : 0;
            usnprintf(ch, sizeof(ch), c ? "ch %2d" : "     ", c);
            uprintf("%s  ", ch);
            put_mhz(f);
            uprintf(" MHz  level %2u%%  %s", lvl, s == 0 ? "LOCK" : s == 1 ? "ofdm" : s == TUNE_NO_TUNER ? "tuner?" : "-");
            if (s == 0) {
                ber_valid = per_valid = 0;
                dm_errors();
                ber_valid = per_valid = 0;
                u64 t = now_ms();
                while ((!ber_valid || !per_valid) && now_ms() - t < 1000) {
                    delay_ms(5);
                    dm_errors();
                }
                u32 q = (ber_valid || per_valid) ? dm_quality() : 0;
                if (q)
                    uprintf("  quality %u%%", q);
                if (nf < 64) {
                    found[nf] = f;
                    found_lvl[nf] = lvl;
                    found_q[nf] = q;
                    found_bw[nf++] = (u8)b;
                }
            }
            uprintf("\n");
            if (stop_asked())
                goto done;
        }
    }
done:
    uprintf("locked: %d\n", nf);
    for (int i = 0; i < nf; i++) {
        uprintf("  ");
        put_mhz(found[i]);
        uprintf(" MHz (%u MHz)  level %u%%", found_bw[i], found_lvl[i]);
        if (found_q[i])
            uprintf("  quality %u%%", found_q[i]);
        uprintf("\n");
    }
    return 0;
}

/* ---- rf spectrum ---- */

/* The aerial's level across a band, as a bar chart on the TV: the tuner tuned
   every `step` (8 MHz wide filter), the demodulator's acquisition restarted
   so its IF AGC settles on what the filter lets through, the level read after
   `settle` ms.  No lock is waited for -- it shows any signal, DVB-T2
   multiplexes and mobile-phone bands too, which the S3811 cannot receive. */
#define SPEC_MAX 1024
static u8 spec[SPEC_MAX];

static void spec_draw(u32 from, u32 step, int n, u32 floor, u32 top)
{
    const int x0 = 60, y0 = 110, w = SCREEN_W - 2 * x0, h = 480;
    char buf[64];
    gfx_target(0);
    gfx_fill(0, 0, SCREEN_W, SCREEN_H, RGB(12, 16, 28));
    usnprintf(buf, sizeof(buf), "Aerial level %u..%u MHz", from / 1000, (from + step * (u32)(n - 1)) / 1000);
    gfx_text(x0, 30, buf, 3, RGB(240, 240, 240));
    usnprintf(buf, sizeof(buf), "MxL603 + S3811 IF AGC, every %u kHz, 8 MHz filter", step);
    gfx_text(x0, 76, buf, 2, RGB(150, 160, 180));
    gfx_fill(x0, y0, w, h, RGB(24, 30, 48));
    /* the level axis: from a little under the floor to well over the highest (a weak aerial's whole range
       would be a sliver of 0..100 %), a grid line every 5 % */
    u32 lo = floor > 5 ? (floor - 5) / 5 * 5 : 0, hi = (top + 10 > lo + 25 ? top + 10 : lo + 25) / 5 * 5;
    if (hi > 100)
        hi = 100;
    for (u32 g = lo; g <= hi; g += 5) {
        int y = y0 + h - (int)((g - lo) * (u32)h / (hi - lo));
        gfx_fill(x0, y, w, 1, RGB(50, 60, 84));
        usnprintf(buf, sizeof(buf), "%u%%", g);
        gfx_text(x0 - 8 - gfx_text_width(buf, 1), y - 8, buf, 1, RGB(150, 160, 180));
    }
    u32 span = top > floor ? top - floor : 1;
    for (int i = 0; i < n; i++) {
        int xa = x0 + i * w / n, xb = x0 + (i + 1) * w / n;
        int bh = spec[i] > lo ? (int)(((u32)spec[i] - lo) * (u32)h / (hi - lo)) : 1;
        if (bh > h)
            bh = h;
        u32 rel = spec[i] > floor ? ((u32)spec[i] - floor) * 255 / span : 0;
        if (rel > 255)
            rel = 255;
        u16 c = RGB(60 + rel * 195 / 255, 120 + rel / 2, 255 - rel * 200 / 255);
        if (xb - xa > 2)
            xb--;
        gfx_fill(xa, y0 + h - bh, xb - xa, bh, c);
    }
    u32 last = from + step * (u32)(n - 1);
    for (u32 mhz = (from / 1000 + 99) / 100 * 100; mhz * 1000 <= last; mhz += 100) {   /* MHz every 100 */
        int x = x0 + w / (2 * n) + (int)((mhz * 1000 - from) * (u32)w / ((u32)n * step));
        gfx_fill(x, y0 + h, 1, 10, RGB(200, 200, 200));
        usnprintf(buf, sizeof(buf), "%u", mhz);
        gfx_text(x - gfx_text_width(buf, 2) / 2, y0 + h + 14, buf, 2, RGB(200, 200, 200));
    }
    for (u32 ch = 21; ch <= 69; ch++) {                           /* UHF channels: a tick each, a number every 5 */
        u32 f = 474000 + (ch - 21) * 8000;
        if (f < from || f > last)
            continue;
        int x = x0 + w / (2 * n) + (int)((f - from) * (u32)w / ((u32)n * step));
        gfx_fill(x, y0 + h + 46, 1, ch % 5 ? 5 : 10, RGB(150, 200, 150));
        if (ch % 5 == 0) {
            usnprintf(buf, sizeof(buf), "%u", ch);
            gfx_text(x - gfx_text_width(buf, 2) / 2, y0 + h + 60, buf, 2, RGB(150, 200, 150));
        }
    }
    if (from < 474000)
        gfx_text(x0, y0 + h + 60, "UHF channel", 2, RGB(150, 200, 150));
    gfx_text(x0, SCREEN_H - 40, "'console' brings the console back, 'screen demo' the demo screen", 2,
             RGB(150, 160, 180));
    gfx_view(0);
}

static int cmd_spectrum(int argc, char **argv)
{
    u32 from = 174000, to = 862000, step = 4000, settle = 160, b;
    int a = 2;
    if (argc > a + 1 && argv[a][0] != '-') {
        if (!parse_khz(argv[a], &from, &b) || !parse_khz(argv[a + 1], &to, &b) || from >= to) {
            uprintf("usage: rf spectrum [<from MHz> <to MHz> [step kHz]] [-w settle ms]\n");
            return -1;
        }
        a += 2;
        if (argc > a && argv[a][0] != '-') {
            if (!parse_num(argv[a], &step) || step < 250) {
                uprintf("rf spectrum: a step of 250 kHz or more\n");
                return -1;
            }
            a++;
        }
    }
    if (argc > a + 1 && s_eq(argv[a], "-w") && !parse_num(argv[a + 1], &settle)) {
        uprintf("usage: rf spectrum [<from MHz> <to MHz> [step kHz]] [-w settle ms]\n");
        return -1;
    }
    if ((to - from) / step + 1 > SPEC_MAX)
        step = (to - from) / (SPEC_MAX - 1) + 1;
    if (from < 42000 || to > 1002000) {
        uprintf("rf spectrum: the MxL603 tunes 42..1002 MHz\n");
        return -1;
    }
    if (need_init())
        return -1;
    dm_setup();
    int n = 0;
    u64 t0 = now_ms();
    uprintf("measuring %u..%u MHz every %u kHz (%u ms each; Ctrl-C stops)\n", from / 1000, to / 1000, step, settle);
    for (u32 f = from; f <= to && n < SPEC_MAX; f += step) {
        mxl_err = 0;
        mxl_chan_tune(f, 8);
        dm_acquire(8);
        if (wait_ms(settle))
            break;
        spec[n++] = (u8)dm_level(0);
        if (mxl_err) {
            uprintf("rf spectrum: the tuner does not answer\n");
            return -1;
        }
    }
    if (n < 2)
        return -1;
    u32 floor = 100, top = 0;
    for (int i = 0; i < n; i++) {
        if (spec[i] < floor)
            floor = spec[i];
        if (spec[i] > top)
            top = spec[i];
    }
    uprintf("%d points in %u ms; level %u..%u %%\n", n, (u32)(now_ms() - t0), floor, top);
    /* the peaks: local highs at least 5 points above the floor, the strongest first (up to 12) */
    static u16 peak[64];
    int np = 0;
    for (int i = 0; i < n && np < 64; i++) {
        if (spec[i] < floor + 5 || (i > 0 && spec[i - 1] > spec[i]) || (i + 1 < n && spec[i + 1] >= spec[i]))
            continue;
        peak[np++] = (u16)i;
    }
    for (int i = 1; i < np; i++)                  /* (insertion sort by level) */
        for (int j = i; j > 0 && spec[peak[j]] > spec[peak[j - 1]]; j--) {
            u16 t = peak[j];
            peak[j] = peak[j - 1];
            peak[j - 1] = t;
        }
    for (int i = 0; i < np && i < 12; i++) {
        u32 f = from + step * peak[i];
        uprintf("  peak ");
        put_mhz(f);
        uprintf(" MHz  level %2u%%", spec[peak[i]]);
        if (f + 4000 >= 474000 && f <= 862000)
            uprintf("  (UHF ch %u)", (f + 4000 - 474000) / 8000 + 21);
        uprintf("\n");
    }
    spec_draw(from, step, n, floor, top);
    cur_khz = 0;
    return 0;
}

/* ---- rf live ---- */

/* 'rf spectrum' kept current, with keys: the band swept again and again, each bar drawn as it is measured
   (tuner tuned, acquisition restarted, the IF AGC read after `settle` ms -- so a pass takes its points x
   (settle + the tune's own ~time), not an instant), a marker with its frequency / channel / level, the
   highest level seen kept as a tick over each bar.  Left / Right move the marker (past an edge: the window
   moves), Up / Down halve / double the span around it, PgUp / PgDn move the window half its width, F the
   whole band again, + / - halve / double the settle time (160 ms at first, 40..640), Space pauses, P peak hold on / off (cleared),
   Enter tries a DVB-T lock on the marker's channel, Esc / q / Ctrl-C / the remote's EXIT end it. */
#define LIVE_BARS 145                   /* at most: 8 pixels each */
#define LIVE_MAX  (LIVE_BARS + 3)
#define LX0 60
#define LY0 132
#define LW  (SCREEN_W - 2 * LX0)
#define LH  400
#define LIVE_BG RGB(12, 16, 28)

static struct {
    u32 from, span, step;               /* kHz */
    int n, marker;                      /* points; the marker's */
    u32 lo, hi;                         /* the level axis, tenths of a percent */
    int ranged;                         /* ... set from what was measured */
    u32 settle, pass_ms;                /* ms a point; the last whole pass */
    int paused, hold;
    char note[64];                      /* what Enter found */
} lv;
static u16 live_lvl[LIVE_MAX], live_peak[LIVE_MAX];     /* tenths of a percent; 0 = not measured yet */

enum { LK_NONE, LK_LEFT, LK_RIGHT, LK_UP, LK_DOWN, LK_PGUP, LK_PGDN, LK_ENTER, LK_SPACE, LK_FASTER, LK_SLOWER,
       LK_PEAK, LK_FULL, LK_QUIT };

static int live_same(const char *a, const char *b)     /* (the remote's key names: any case) */
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z')
            x = (char)(x - 32);
        if (y >= 'a' && y <= 'z')
            y = (char)(y - 32);
        if (x != y)
            return 0;
    }
    return *a == *b;
}

static int live_len(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

/* the next key (USB keyboard, UART / telnet, the remote), LK_NONE: none */
static int live_key(void)
{
    static u8 esc;
    static int num;
    static u64 esc_since;
    int c;
    usb_int_poll();
    (void)kbd_poll();
    while ((c = kbd_getc()) >= 0 || (c = uart_getc()) >= 0) {
        if (esc == 2) {
            if (c >= '0' && c <= '9') {
                num = num * 10 + c - '0';
                continue;
            }
            esc = 0;
            if (c == 'C')
                return LK_RIGHT;
            if (c == 'D')
                return LK_LEFT;
            if (c == 'A')
                return LK_UP;
            if (c == 'B')
                return LK_DOWN;
            if (c == '~' && num == 5)
                return LK_PGUP;
            if (c == '~' && num == 6)
                return LK_PGDN;
            continue;
        }
        if (esc == 1) {
            esc = c == '[' ? 2 : 0;
            num = 0;
            if (!esc)
                return LK_QUIT;         /* (a lone ESC, then something else) */
            continue;
        }
        if (c == 27) {
            esc = 1;
            esc_since = now_ms();
            continue;
        }
        if (c == 3 || c == 'q' || c == 'Q')
            return LK_QUIT;
        if (c == '\r' || c == '\n')
            return LK_ENTER;
        if (c == ' ')
            return LK_SPACE;
        if (c == '+' || c == '=')
            return LK_FASTER;
        if (c == '-' || c == '_')
            return LK_SLOWER;
        if (c == 'p' || c == 'P')
            return LK_PEAK;
        if (c == 'f' || c == 'F')
            return LK_FULL;
    }
    if (esc == 1 && now_ms() - esc_since > 150) {       /* ESC alone (the UART's; the keyboard's is Ctrl-C) */
        esc = 0;
        return LK_QUIT;
    }
    struct ir_event ev;
    while (ir_poll(&ev)) {
        if (!ev.valid)
            continue;
        const char *n = ir_key_name(ev.code);
        if (!n)
            continue;
        if (live_same(n, "LEFT"))       /* (the marker's keys repeat) */
            return LK_LEFT;
        if (live_same(n, "RIGHT"))
            return LK_RIGHT;
        if (ev.repeat)
            continue;
        if (live_same(n, "UP"))
            return LK_UP;
        if (live_same(n, "DOWN"))
            return LK_DOWN;
        if (live_same(n, "PAGE+"))
            return LK_PGDN;
        if (live_same(n, "PAGE-"))
            return LK_PGUP;
        if (live_same(n, "OK"))
            return LK_ENTER;
        if (live_same(n, "EXIT"))
            return LK_QUIT;
    }
    return LK_NONE;
}

/* the level in tenths of a percent (dm_level's formula, finer) */
static u32 live_level(void)
{
    u32 agc;
    (void)dm_level(&agc);
    s32 x = ((s32)agc - 50) * 1000 / 1150;
    if (x < 10)
        x = 10;
    if (x > 990)
        x = 990;
    return (u32)(1000 - x);
}

/* the window: `span` kHz from `from` (kept inside the tuner's 42..1002 MHz), the marker on `mark` kHz */
static void live_layout(u32 from, u32 span, u32 mark)
{
    if (span < 8000)
        span = 8000;
    if (span > 960000)
        span = 960000;
    if (from < 42000 || from > 0x7FFFFFFFu)
        from = 42000;
    if (from + span > 1002000)
        from = 1002000 - span;
    u32 step = (span / LIVE_BARS + 249) / 250 * 250;
    if (step < 250)
        step = 250;
    lv.from = from;
    lv.span = span;
    lv.step = step;
    lv.n = (int)(span / step) + 1;
    if (lv.n > LIVE_MAX)
        lv.n = LIVE_MAX;
    int m = mark > from ? (int)((mark - from + step / 2) / step) : 0;
    lv.marker = m >= lv.n ? lv.n - 1 : m;
    lv.lo = 0;
    lv.hi = 1000;
    lv.ranged = 0;
    lv.pass_ms = 0;
    for (int i = 0; i < LIVE_MAX; i++)
        live_lvl[i] = live_peak[i] = 0;
}

/* the level axis from what is measured: from a little under the lowest to well over the highest.  During a
   pass it only grows (shrink 0), with room to spare so that it seldom has to; after a pass it shrinks when it
   is well too wide.  1 when it changed (the bars and its labels are to be drawn again: live_rescale) */
static int live_range(int shrink)
{
    u32 floor = 1000, top = 0;
    for (int i = 0; i < lv.n; i++) {
        u32 v = live_lvl[i];
        if (!v)
            continue;
        if (v < floor)
            floor = v;
        if (v > top)
            top = v;
        if (lv.hold && live_peak[i] > top)
            top = live_peak[i];
    }
    u32 lo = 0, hi = 1000;
    if (top) {
        lo = floor > 50 ? (floor - 50) / 50 * 50 : 0;
        hi = (top + 100 > lo + 250 ? top + 100 : lo + 250) / 50 * 50;
        if (hi > 1000)
            hi = 1000;
        if (lv.ranged) {
            u32 want_lo = lo, want_hi = hi;
            lo = lv.lo;
            hi = lv.hi;
            if (want_lo < lo)
                lo = want_lo >= 50 ? want_lo - 50 : 0;
            if (want_hi > hi)
                hi = want_hi + 50 > 1000 ? 1000 : want_hi + 50;
            if (shrink && want_lo >= lo + 150)
                lo = want_lo;
            if (shrink && hi >= want_hi + 150)
                hi = want_hi;
        }
        lv.ranged = 1;
    }
    if (lo == lv.lo && hi == lv.hi)
        return 0;
    lv.lo = lo;
    lv.hi = hi;
    return 1;
}

static int live_height(u32 v)
{
    if (v <= lv.lo)
        return 1;
    int h = (int)((v - lv.lo) * (u32)LH / (lv.hi - lv.lo));
    return h < 1 ? 1 : h > LH ? LH : h;
}

static int live_x(u32 khz)              /* a frequency's column (its bar's middle) */
{
    return LX0 + LW / (2 * lv.n) + (int)((khz - lv.from) * (u32)LW / ((u32)lv.n * lv.step));
}

/* one bar: its background and grid lines, the bar, the peak's tick, the marker's line */
static void live_col(int i)
{
    if (i < 0 || i >= lv.n)
        return;
    gfx_target(0);
    int xa = LX0 + i * LW / lv.n, xb = LX0 + (i + 1) * LW / lv.n, bw = xb - xa > 3 ? xb - xa - 1 : xb - xa;
    gfx_fill(xa, LY0, xb - xa, LH, RGB(24, 30, 48));
    u32 grid = lv.hi - lv.lo > 500 ? 100 : 50;
    for (u32 g = lv.lo; g <= lv.hi; g += grid)
        gfx_fill(xa, LY0 + LH - (int)((g - lv.lo) * (u32)LH / (lv.hi - lv.lo)) - (g == lv.lo), xb - xa, 1,
                 RGB(50, 60, 84));
    u32 v = live_lvl[i];
    if (v) {
        int bh = live_height(v);
        u32 rel = v > lv.lo ? (v - lv.lo) * 255 / (lv.hi - lv.lo) : 0;
        if (rel > 255)
            rel = 255;
        gfx_fill(xa, LY0 + LH - bh, bw, bh, RGB(60 + rel * 195 / 255, 120 + rel / 2, 255 - rel * 200 / 255));
    }
    if (lv.hold && live_peak[i] > v)
        gfx_fill(xa, LY0 + LH - live_height(live_peak[i]), bw, 2, RGB(255, 220, 120));
    if (i == lv.marker)
        gfx_fill((xa + xb) / 2, LY0, 1, LH, RGB(255, 255, 255));
}

static void live_tick(int i, int on)    /* where the sweep is: a mark under its bar */
{
    if (i < 0 || i >= lv.n)
        return;
    gfx_target(0);
    int xa = LX0 + i * LW / lv.n, xb = LX0 + (i + 1) * LW / lv.n;
    gfx_fill(xa, LY0 + LH + 2, xb - xa, 4, on ? RGB(255, 230, 80) : LIVE_BG);
}

static void live_info(void)             /* the settings' line */
{
    char buf[128];
    gfx_target(0);
    gfx_fill(LX0, 68, LW, 20, LIVE_BG);
    usnprintf(buf, sizeof(buf), "%u..%u MHz   every %u kHz   %u ms a point", lv.from / 1000,
              (lv.from + lv.step * (u32)(lv.n - 1)) / 1000, lv.step, lv.settle);
    if (lv.pass_ms)
        usnprintf(buf + live_len(buf), (int)sizeof(buf) - live_len(buf), "   a pass %u.%u s", lv.pass_ms / 1000,
                  lv.pass_ms % 1000 / 100);
    if (lv.hold)
        usnprintf(buf + live_len(buf), (int)sizeof(buf) - live_len(buf), "   peak hold");
    gfx_text(LX0, 70, buf, 2, RGB(150, 160, 180));
    if (lv.paused)
        gfx_text(LX0 + LW - gfx_text_width("PAUSED", 2), 70, "PAUSED", 2, RGB(255, 200, 80));
}

static void live_readout(void)          /* the marker's line */
{
    char buf[128];
    u32 f = lv.from + lv.step * (u32)lv.marker, v = live_lvl[lv.marker], pk = live_peak[lv.marker];
    gfx_target(0);
    gfx_fill(LX0, 96, LW, 20, LIVE_BG);
    usnprintf(buf, sizeof(buf), "Marker %u.%03u MHz", f / 1000, f % 1000);
    if (f >= 470000 && f < 862000)
        usnprintf(buf + live_len(buf), (int)sizeof(buf) - live_len(buf), "  UHF ch %u", (f - 470000) / 8000 + 21);
    else if (f >= 174000 && f < 230000)
        usnprintf(buf + live_len(buf), (int)sizeof(buf) - live_len(buf), "  VHF ch %u", (f - 174000) / 7000 + 5);
    if (v)
        usnprintf(buf + live_len(buf), (int)sizeof(buf) - live_len(buf), "   %u.%u %%", v / 10, v % 10);
    if (lv.hold && pk > v)
        usnprintf(buf + live_len(buf), (int)sizeof(buf) - live_len(buf), " (peak %u.%u %%)", pk / 10, pk % 10);
    gfx_text(LX0, 98, buf, 2, RGB(255, 255, 255));
    if (lv.note[0])
        gfx_text(LX0 + gfx_text_width(buf, 2) + 36, 98, lv.note, 2, RGB(255, 220, 120));
}

static void live_rescale(void)          /* a new level axis: its labels and every bar */
{
    char buf[16];
    gfx_target(0);
    gfx_fill(0, LY0 - 12, LX0, LH + 24, LIVE_BG);
    u32 grid = lv.hi - lv.lo > 500 ? 100 : 50;
    for (u32 g = lv.lo; g <= lv.hi; g += grid) {
        usnprintf(buf, sizeof(buf), "%u%%", g / 10);
        gfx_text(LX0 - 8 - gfx_text_width(buf, 1), LY0 + LH - (int)((g - lv.lo) * (u32)LH / (lv.hi - lv.lo)) - 8, buf,
                 1, RGB(150, 160, 180));
    }
    for (int i = 0; i < lv.n; i++)
        live_col(i);
}

static void live_all(void)              /* the whole picture: at the start and for a new window */
{
    char buf[64];
    gfx_target(0);
    gfx_fill(0, 0, SCREEN_W, SCREEN_H, LIVE_BG);
    gfx_text(LX0, 22, "Aerial level, live", 3, RGB(240, 240, 240));
    live_info();
    live_readout();
    live_rescale();
    /* the frequency axis: MHz at a spacing that fits the span, the UHF channels under it */
    u32 last = lv.from + lv.step * (u32)(lv.n - 1), span = last - lv.from;
    u32 every = span > 400000 ? 100 : span > 160000 ? 50 : span > 64000 ? 20 : span > 32000 ? 10 : span > 12000 ? 5 : 2;
    for (u32 mhz = (lv.from / 1000 + every - 1) / every * every; mhz * 1000 <= last; mhz += every) {
        int x = live_x(mhz * 1000);
        gfx_fill(x, LY0 + LH + 8, 1, 10, RGB(200, 200, 200));
        usnprintf(buf, sizeof(buf), "%u", mhz);
        gfx_text(x - gfx_text_width(buf, 2) / 2, LY0 + LH + 22, buf, 2, RGB(200, 200, 200));
    }
    u32 label = span > 320000 ? 5 : span > 120000 ? 2 : 1;
    int any = 0;
    for (u32 ch = 21; ch <= 69; ch++) {
        u32 f = 474000 + (ch - 21) * 8000;
        if (f < lv.from || f > last)
            continue;
        int x = live_x(f);
        any = 1;
        gfx_fill(x, LY0 + LH + 48, 1, ch % label ? 5 : 10, RGB(150, 200, 150));
        if (ch % label == 0) {
            usnprintf(buf, sizeof(buf), "%u", ch);
            gfx_text(x - gfx_text_width(buf, 2) / 2, LY0 + LH + 62, buf, 2, RGB(150, 200, 150));
        }
    }
    if (any && lv.from < 470000)
        gfx_text(LX0, LY0 + LH + 62, "UHF channel", 2, RGB(150, 200, 150));
    gfx_text(LX0, SCREEN_H - 92, "Left / Right  marker      Up / Down  zoom in / out      PgUp / PgDn  move      "
             "F  whole band", 2, RGB(150, 160, 180));
    gfx_text(LX0, SCREEN_H - 68, "Enter  try DVB-T at the marker      + / -  faster / slower      Space  pause", 2,
             RGB(150, 160, 180));
    gfx_text(LX0, SCREEN_H - 44, "P  peak hold      Esc  back", 2, RGB(150, 160, 180));
}

static int cmd_live(int argc, char **argv)
{
    u32 from = 174000, to = 862000, b;
    if (argc > 2 && (argc < 4 || !parse_khz(argv[2], &from, &b) || !parse_khz(argv[3], &to, &b) || from >= to)) {
        uprintf("usage: rf live [<from MHz> <to MHz>]\n");
        return -1;
    }
    if (from < 42000 || to > 1002000) {
        uprintf("rf live: the MxL603 tunes 42..1002 MHz\n");
        return -1;
    }
    if (need_init())
        return -1;
    dm_setup();
    lv.settle = 160;                    /* as 'rf spectrum': sooner the AGC has not settled (+ trades that for speed) */
    lv.paused = 0;
    lv.hold = 1;
    lv.note[0] = 0;
    live_layout(from, to - from, (from + to) / 2);
    uprintf("rf live: the chart is on the TV, swept again and again.  Left / Right the marker, Up / Down zoom, "
            "PgUp / PgDn move, F the whole band, + / - faster / slower, Space pause, P peak hold, Enter a DVB-T "
            "lock tried at the marker, Esc / q / Ctrl-C end it\n");
    live_all();
    gfx_view(0);
    int i = 0, rc = 0;
    u32 passes = 0;
    u64 pass_start = now_ms();
    for (;;) {
        int k = LK_NONE;
        if (lv.paused) {
            delay_ms(5);
            k = live_key();
        } else {
            live_tick(i, 1);
            mxl_err = 0;
            mxl_chan_tune(lv.from + lv.step * (u32)i, 8);
            dm_acquire(8);
            if (mxl_err) {
                uprintf("rf live: the tuner does not answer\n");
                rc = -1;
                break;
            }
            u64 end = now_ms() + lv.settle;
            while (now_ms() < end && (k = live_key()) == LK_NONE)
                delay_us(500);
            if (k == LK_NONE) {
                u32 v = live_level();
                live_lvl[i] = (u16)v;
                if (v > live_peak[i])
                    live_peak[i] = (u16)v;
                if (live_range(0))
                    live_rescale();
                else
                    live_col(i);
                if (i == lv.marker)
                    live_readout();
                live_tick(i, 0);
                if (++i >= lv.n) {
                    i = 0;
                    passes++;
                    lv.pass_ms = (u32)(now_ms() - pass_start);
                    pass_start = now_ms();
                    if (live_range(1))
                        live_rescale();
                    live_info();
                }
                continue;
            }
            live_tick(i, 0);
        }
        if (k == LK_NONE)
            continue;
        if (k == LK_QUIT)
            break;
        u32 mark = lv.from + lv.step * (u32)lv.marker;
        int again = 0;                  /* a new window: everything from its first point */
        if (k == LK_LEFT || k == LK_RIGHT) {
            int m = lv.marker + (k == LK_RIGHT ? 1 : -1);
            lv.note[0] = 0;
            if (m < 0 || m >= lv.n) {   /* past the edge: the window a quarter of its width on */
                u32 by = lv.span / 4, f = lv.from;
                if (k == LK_RIGHT)
                    f = f + by + lv.span <= 1002000 ? f + by : 1002000 - lv.span;
                else
                    f = f >= 42000 + by ? f - by : 42000;
                if (f != lv.from) {
                    live_layout(f, lv.span, k == LK_RIGHT ? mark + lv.step : mark > lv.step ? mark - lv.step : mark);
                    again = 1;
                }
            } else {
                int old = lv.marker;
                lv.marker = m;
                live_col(old);
                live_col(m);
                live_readout();
            }
        } else if (k == LK_UP || k == LK_DOWN) {
            u32 span = k == LK_UP ? lv.span / 2 : lv.span * 2;
            if (span < 8000)
                span = 8000;
            if (span > 960000)
                span = 960000;
            if (span != lv.span) {
                lv.note[0] = 0;
                live_layout(mark > 42000 + span / 2 ? mark - span / 2 : 42000, span, mark);
                again = 1;
            }
        } else if (k == LK_PGUP || k == LK_PGDN) {
            u32 by = lv.span / 2, f = lv.from;
            if (k == LK_PGDN)
                f = f + by + lv.span <= 1002000 ? f + by : 1002000 - lv.span;
            else
                f = f >= 42000 + by ? f - by : 42000;
            if (f != lv.from) {
                lv.note[0] = 0;
                live_layout(f, lv.span, f + (mark - lv.from));
                again = 1;
            }
        } else if (k == LK_FULL) {
            lv.note[0] = 0;
            live_layout(174000, 688000, mark);
            again = 1;
        } else if (k == LK_FASTER || k == LK_SLOWER) {
            u32 s = k == LK_FASTER ? lv.settle / 2 : lv.settle * 2;
            lv.settle = s < 40 ? 40 : s > 640 ? 640 : s;
            lv.pass_ms = 0;
            live_info();
        } else if (k == LK_SPACE) {
            lv.paused = !lv.paused;
            live_info();
        } else if (k == LK_PEAK) {
            lv.hold = !lv.hold;
            for (int j = 0; j < LIVE_MAX; j++)
                live_peak[j] = live_lvl[j];
            (void)live_range(1);
            live_rescale();
            live_info();
        } else if (k == LK_ENTER) {     /* a DVB-T lock tried on the marker's channel */
            u32 f = mark, bw = 8, took = 0, ch = 0;
            if (f >= 470000 && f < 862000) {
                ch = (f - 470000) / 8000 + 21;
                f = 474000 + (ch - 21) * 8000;
            } else if (f >= 174000 && f < 230000) {
                ch = (f - 174000) / 7000 + 5;
                f = 177500 + (ch - 5) * 7000;
                bw = 7;
            }
            usnprintf(lv.note, sizeof(lv.note), "trying %u.%03u MHz ...", f / 1000, f % 1000);
            live_readout();
            int r = rf_tune(f, bw, 0, &took);
            const char *what = r == 0 ? "DVB-T LOCKED" : r == 1 ? "a DVB-T carrier, no data lock"
                               : r == TUNE_NO_TUNER ? "the tuner did not lock" : "no DVB-T (DVB-T2 or nothing)";
            if (ch)
                usnprintf(lv.note, sizeof(lv.note), "ch %u: %s", ch, what);
            else
                usnprintf(lv.note, sizeof(lv.note), "%u.%03u MHz: %s", f / 1000, f % 1000, what);
            uprintf("rf live: %u.%03u MHz, %u MHz wide: %s (%u ms)\n", f / 1000, f % 1000, bw, what, took);
            dm_setup();
            live_readout();
        }
        if (again) {
            i = 0;
            pass_start = now_ms();
            live_all();
        }
    }
    cur_khz = 0;
    uprintf("rf live: ended after %u whole passes\n", passes);
    return rc;
}

/* ---- rf ts ---- */
#define MAX_PIDS 64

struct pid_stat {
    u16 pid;
    u8 cc;                              /* the last continuity counter, 0xFF none yet */
    u8 pad;
    u32 packets, cc_errors, scrambled;
};

static struct pid_stat pids[MAX_PIDS];
static int npids;
static u32 pid_overflow, tei_count, sync_errors;

static void count_packet(const volatile u8 *p)
{
    if (p[0] != 0x47) {
        sync_errors++;
        return;
    }
    u8 b1 = p[1], b3 = p[3];
    u16 pid = (u16)(((b1 & 0x1F) << 8) | p[2]);
    if (b1 & 0x80)
        tei_count++;
    int i;
    for (i = 0; i < npids && pids[i].pid != pid; i++) { }
    if (i == npids) {
        if (npids == MAX_PIDS) {
            pid_overflow++;
            return;
        }
        pids[npids].pid = pid;
        pids[npids].cc = 0xFF;
        pids[npids].packets = pids[npids].cc_errors = pids[npids].scrambled = 0;
        npids++;
    }
    struct pid_stat *s = &pids[i];
    s->packets++;
    if (b3 & 0xC0)
        s->scrambled++;
    if (pid != 0x1FFF && (b3 & 0x10)) {     /* a payload: the counter steps (a repeat is allowed) */
        u8 cc = b3 & 0x0F;
        if (s->cc != 0xFF && cc != s->cc && cc != ((s->cc + 1) & 0x0F))
            s->cc_errors++;
        s->cc = cc;
    }
}

/* the staging buffer for a recording: a circular buffer in cached RAM */
static u32 stage_head, stage_tail;      /* bytes written in / taken out (positions mod RF_STAGE_SIZE) */

/* one write of what is staged (up to 256 KB: the hardware ring, 1.1 MB, has
   to hold what comes meanwhile): 0, -1 the disk failed (said why) */
#define STAGE_CHUNK 0x40000u
static int stage_write(u32 *written)
{
    u32 off = stage_tail % RF_STAGE_SIZE;
    u32 n = stage_head - stage_tail;
    if (n > RF_STAGE_SIZE - off)
        n = RF_STAGE_SIZE - off;
    if (n > STAGE_CHUNK)
        n = STAGE_CHUNK;
    if (n && disk_upload_put((const u8 *)KSEG0(RF_STAGE_PHYS + off), n) < 0)
        return -1;
    stage_tail += n;
    *written += n;
    return 0;
}

/* kbit/s of `packets` 188-byte packets in `ms` (bits per millisecond), in 32 bits */
static u32 rate_kbps(u32 packets, u32 ms)
{
    if (!ms)
        return 0;
    while (packets > 2000000 || ms > 1000000) {
        packets >>= 1;
        ms >>= 1;
    }
    return ms ? packets * (TS_PACKET * 8) / ms : 0;
}

static int cmd_ts(int argc, char **argv)
{
    u32 secs = 0;
    const char *file = 0;
    static u32 only[16];
    int nonly = 0;
    for (int i = 2; i < argc; i++) {
        u32 v;
        if (s_eq(argv[i], "-p") && i + 1 < argc) {
            const char *s = argv[++i];
            while (*s && nonly < 16) {
                char tmp[12];
                int n = 0;
                while (*s && *s != ',' && n < 11)
                    tmp[n++] = *s++;
                tmp[n] = 0;
                if (*s == ',')
                    s++;
                if (!parse_num(tmp, &v) || v > 0x1FFF) {
                    uprintf("rf ts: bad PID '%s'\n", tmp);
                    return -1;
                }
                only[nonly++] = v;
            }
        } else if (!secs && parse_num(argv[i], &v) && v) {
            secs = v;
        } else if (!file) {
            file = argv[i];
        } else {
            secs = 0;
            break;
        }
    }
    if (!secs) {
        uprintf("usage: rf ts <seconds> [file] [-p pid,pid,...]   (without -p: every packet; with a file: a raw\n"
                "       .ts written as it comes, else a PID table)\n");
        return -1;
    }
    if (need_init())
        return -1;
    if (!(dm_r(0x1D) & 0x40)) {
        uprintf("rf ts: no FEC lock (0x1D = 0x%02x): 'rf tune' a multiplex first\n", dm_r(0x1D));
        return -1;
    }
    if (file && (ftp_disk_busy() || http_disk_busy() || smb_disk_busy())) {
        uprintf("rf ts: the FTP server, the web page or the SMB server is moving a file now; try again\n");
        return -1;
    }
    if (file && disk_upload_open(file, 0) < 0)
        return -1;
    tsi_route();
    dmx_setup();
    if (nonly) {
        for (int i = 0; i < nonly; i++)
            dmx_filter(i, only[i]);
    } else {
        dmx_filter(0, 0xFFFF);          /* mask all zero: every packet (see FINDINGS: unverified on hardware) */
    }
    npids = 0;
    pid_overflow = tei_count = sync_errors = 0;
    stage_head = stage_tail = 0;
    u32 total = 0, written = 0, full = 0, said = 0;
    int failed = 0;
    u64 t0 = now_ms(), last_print = t0;
    uprintf("receiving %u s%s%s (Ctrl-C stops)\n", secs, file ? " into " : "", file ? file : "");
    while (now_ms() - t0 < (u64)secs * 1000) {
        u32 n = dmx_waiting();
        if (n >= RF_RING_PACKETS - 1)
            full++;
        while (n) {
            u32 run = RF_RING_PACKETS - ring_rd;       /* up to the ring's end */
            if (run > n)
                run = n;
            for (u32 k = 0; k < run; k++)
                count_packet(dmx_packet(ring_rd + k));
            if (file) {
                u32 bytes = run * TS_PACKET;
                if (RF_STAGE_SIZE - (stage_head - stage_tail) < bytes) {
                    full++;                             /* the disk is slower than the stream: dropped */
                } else {
                    u32 off = stage_head % RF_STAGE_SIZE, first = bytes;
                    if (first > RF_STAGE_SIZE - off)
                        first = RF_STAGE_SIZE - off;
                    copy_words((u32 *)KSEG0(RF_STAGE_PHYS + off), (const volatile u32 *)dmx_packet(ring_rd), first);
                    if (first < bytes)
                        copy_words((u32 *)KSEG0(RF_STAGE_PHYS),
                                   (const volatile u32 *)((const volatile u8 *)dmx_packet(ring_rd) + first),
                                   bytes - first);
                    stage_head += bytes;
                }
            }
            dmx_consumed(run);
            total += run;
            n -= run;
        }
        if (file && stage_head - stage_tail >= STAGE_CHUNK && stage_write(&written) < 0) {
            failed = 1;
            break;
        }
        if (now_ms() - last_print >= 1000) {
            last_print = now_ms();
            u32 ms = (u32)(last_print - t0);
            uprintf("  %u s: %u packets, %u kbit/s%s\n", ms / 1000, total, rate_kbps(total, ms),
                    full > said ? ", ring overflowed" : "");
            said = full;
        }
        if (stop_asked())
            break;
    }
    u32 ms = (u32)(now_ms() - t0);
    u8 ovf = (u8)(REG32(DMX + 0x344) & 3);
    dmx_stop();
    if (file) {
        while (!failed && stage_head != stage_tail)
            if (stage_write(&written) < 0)
                failed = 1;
        if (!failed && disk_upload_close() < 0)
            failed = 1;
        if (failed)
            uprintf("rf ts: writing %s failed after %u bytes\n", file, written);
        else
            uprintf("%s: %u bytes (%u packets)\n", file, written, written / TS_PACKET);
    }
    u32 kbps = rate_kbps(total, ms);
    uprintf("%u packets in %u.%03u s = %u kbit/s; sync errors %u, transport errors %u%s%s\n", total, ms / 1000,
            ms % 1000, kbps, sync_errors, tei_count, full ? ", THE RING OVERFLOWED (packets lost)" : "",
            ovf ? " (DMX overflow status set)" : "");
    /* the PID table, by PID */
    for (int i = 1; i < npids; i++)
        for (int j = i; j > 0 && pids[j - 1].pid > pids[j].pid; j--) {
            struct pid_stat t = pids[j];
            pids[j] = pids[j - 1];
            pids[j - 1] = t;
        }
    uprintf("  PID        packets   kbit/s  cc errors  scrambled\n");
    for (int i = 0; i < npids; i++) {
        u32 p = pids[i].packets;
        uprintf("  0x%04x %12u %8u %10u %10u%s\n", pids[i].pid, p, rate_kbps(p, ms), pids[i].cc_errors,
                pids[i].scrambled,
                pids[i].pid == 0 ? "  PAT" : pids[i].pid == 0x10 ? "  NIT" : pids[i].pid == 0x11 ? "  SDT" :
                pids[i].pid == 0x12 ? "  EIT" : pids[i].pid == 0x14 ? "  TDT/TOT" : pids[i].pid == 0x1FFF ? "  null" : "");
    }
    if (pid_overflow)
        uprintf("  (%u packets of more than %d PIDs not counted)\n", pid_overflow, MAX_PIDS);
    if (!total && !nonly)
        uprintf("nothing came: try 'rf ts %u -p 0,0x11' (PID filters as the original firmware sets them)\n", secs);
    return failed ? -1 : 0;
}

/* ---- rf services: PAT, SDT (and the PMTs) ---- */

/* MPEG-2's CRC-32 (polynomial 0x04C11DB7, not reflected, start 0xFFFFFFFF):
   0 over a whole section with its CRC */
static u32 crc32_mpeg(const u8 *p, u32 n)
{
    u32 c = 0xFFFFFFFFu;
    while (n--) {
        c ^= (u32)*p++ << 24;
        for (int i = 0; i < 8; i++)
            c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : c << 1;
    }
    return c;
}

#define SEC_MAX 1024
struct section_rx {
    u16 pid;
    u16 len;                            /* bytes gathered */
    u8 buf[SEC_MAX + 188];
};

static struct section_rx rx_pat, rx_sdt;
static struct section_rx rx_pmt[16];

/* every complete section gathered: fn(section, length) for each with a good
   CRC; what is left is the start of the next one (or nothing) */
static void section_take(struct section_rx *r, void (*fn)(const u8 *, u32))
{
    for (;;) {
        if (r->len && r->buf[0] == 0xFF) {
            r->len = 0;                 /* stuffing: nothing more in this packet */
            return;
        }
        if (r->len < 3)
            return;
        u32 sl = 3 + ((((u32)r->buf[1] & 0x0F) << 8) | r->buf[2]);
        if (sl > SEC_MAX) {
            r->len = 0;                 /* not a section: wait for the next start */
            return;
        }
        if (r->len < sl)
            return;
        if (sl >= 8 && crc32_mpeg(r->buf, sl) == 0)
            fn(r->buf, sl);
        for (u32 k = sl; k < r->len; k++)
            r->buf[k - sl] = r->buf[k];
        r->len = (u16)(r->len - sl);
    }
}

/* one packet of the PID's payload: the pointer field when a section starts in
   it (payload_unit_start_indicator), the bytes before it ending the section
   under way */
static void section_feed(struct section_rx *r, const volatile u8 *pk, void (*fn)(const u8 *, u32))
{
    u8 b3 = pk[3];
    if (!(b3 & 0x10))
        return;                         /* no payload */
    u32 i = 4;
    if (b3 & 0x20)
        i += 1 + (u32)pk[4];            /* the adaptation field */
    if (i >= TS_PACKET)
        return;
    if (pk[1] & 0x40) {
        u32 ptr = pk[i++];
        if (r->len) {                   /* the end of the section under way */
            for (u32 k = 0; k < ptr && i + k < TS_PACKET && r->len < sizeof(r->buf); k++)
                r->buf[r->len++] = pk[i + k];
            section_take(r, fn);
        }
        r->len = 0;
        i += ptr;
    } else if (!r->len) {
        return;                         /* the middle of a section we did not see start */
    }
    for (; i < TS_PACKET && r->len < sizeof(r->buf); i++)
        r->buf[r->len++] = pk[i];
    section_take(r, fn);
}

static u16 pat_prog[16], pat_pmt[16];
static int pat_n, pat_done;
static u32 sdt_seen;                    /* section numbers got (bits) */
static int sdt_last = -1, sdt_count;
static u8 pmt_done[16];

static void on_pat(const u8 *s, u32 n)
{
    if (s[0] != 0x00 || pat_done)
        return;
    uprintf("PAT: transport stream id %u (0x%04x), version %u\n", (s[3] << 8) | s[4], (s[3] << 8) | s[4],
            (s[5] >> 1) & 0x1F);
    for (u32 i = 8; i + 4 <= n - 4; i += 4) {
        u16 prog = (u16)((s[i] << 8) | s[i + 1]), pid = (u16)(((s[i + 2] & 0x1F) << 8) | s[i + 3]);
        if (prog == 0) {
            uprintf("  NIT on PID 0x%04x\n", pid);
            continue;
        }
        uprintf("  programme %5u (0x%04x): PMT on PID 0x%04x\n", prog, prog, pid);
        if (pat_n < 16) {
            pat_prog[pat_n] = prog;
            pat_pmt[pat_n++] = pid;
        }
    }
    pat_done = 1;
}

/* a DVB string: its character table byte(s) skipped, printable ASCII kept */
static void put_dvb_text(const u8 *p, u32 n, int width)
{
    char out[64];
    u32 o = 0, i = 0;
    if (n && p[0] < 0x20)
        i = p[0] == 0x10 ? 3 : 1;
    for (; i < n && o < sizeof(out) - 1; i++) {
        u8 c = p[i];
        if (c == 0x86 || c == 0x87)
            continue;                   /* emphasis on / off */
        out[o++] = c >= 0x20 && c < 0x7F ? (char)c : c == 0x8A ? ' ' : '?';
    }
    out[o] = 0;
    put_left(out, width);
}

static const char *service_type(u8 t)
{
    switch (t) {
    case 0x01: return "TV";
    case 0x02: return "radio";
    case 0x0C: return "data";
    case 0x11: return "MPEG-2 HD TV";
    case 0x16: return "H.264 SD TV";
    case 0x19: return "H.264 HD TV";
    case 0x1F: return "HEVC TV";
    }
    return "other";
}

static void on_sdt(const u8 *s, u32 n)
{
    if (s[0] != 0x42)                   /* SDT actual */
        return;
    u32 secno = s[6], last = s[7];
    if (secno > 31 || (sdt_seen & (1u << secno)))
        return;
    sdt_seen |= 1u << secno;
    sdt_last = (int)last;
    if (!sdt_count++)
        uprintf("SDT: transport stream id %u, original network id %u (0x%04x)\n", (s[3] << 8) | s[4],
                (s[8] << 8) | s[9], (s[8] << 8) | s[9]);
    for (u32 i = 11; i + 5 <= n - 4;) {
        u16 sid = (u16)((s[i] << 8) | s[i + 1]);
        u32 dl = (((u32)s[i + 3] & 0x0F) << 8) | s[i + 4];
        int ca = (s[i + 3] >> 4) & 1;
        u32 d = i + 5, end = d + dl;
        if (end > n - 4)
            break;
        int shown = 0;
        while (d + 2 <= end) {
            u8 tag = s[d], len = s[d + 1];
            if (d + 2 + len > end)
                break;
            if (tag == 0x48 && len >= 3) {      /* service_descriptor */
                const u8 *q = s + d + 2;
                u8 type = q[0], pl = q[1];
                if (2u + pl + 1 <= len) {
                    u8 nl = q[2 + pl];
                    if (3u + pl + nl <= len) {
                        uprintf("  service %5u (0x%04x)  ", sid, sid);
                        put_left(service_type(type), 13);
                        put_dvb_text(q + 2, pl, 16);
                        uprintf("  ");
                        put_dvb_text(q + 3 + pl, nl, 0);
                        uprintf("%s\n", ca ? "  (scrambled)" : "");
                        shown = 1;
                    }
                }
            }
            d += 2u + len;
        }
        if (!shown)
            uprintf("  service %5u (0x%04x)  (no service descriptor)\n", sid, sid);
        i = end;
    }
}

static int pmt_index;                   /* the PMT being parsed (on_pmt has no context argument) */

static const char *stream_type(u8 t)
{
    switch (t) {
    case 0x01: return "MPEG-1 video";
    case 0x02: return "MPEG-2 video";
    case 0x03: return "MPEG-1 audio";
    case 0x04: return "MPEG-2 audio";
    case 0x06: return "private (AC-3 / teletext / subtitles)";
    case 0x0B: return "DSM-CC";
    case 0x0F: return "AAC audio";
    case 0x11: return "LATM AAC audio";
    case 0x1B: return "H.264 video";
    case 0x24: return "HEVC video";
    case 0x81: return "AC-3 audio";
    }
    return "other";
}

static void on_pmt(const u8 *s, u32 n)
{
    if (s[0] != 0x02 || pmt_done[pmt_index])
        return;
    u16 prog = (u16)((s[3] << 8) | s[4]);
    if (prog != pat_prog[pmt_index])
        return;
    pmt_done[pmt_index] = 1;
    u32 pil = (((u32)s[10] & 0x0F) << 8) | s[11];
    uprintf("PMT of programme %u: PCR PID 0x%04x\n", prog, ((s[8] & 0x1F) << 8) | s[9]);
    for (u32 i = 12 + pil; i + 5 <= n - 4;) {
        u8 t = s[i];
        u16 pid = (u16)(((s[i + 1] & 0x1F) << 8) | s[i + 2]);
        u32 el = (((u32)s[i + 3] & 0x0F) << 8) | s[i + 4];
        uprintf("  PID 0x%04x  type 0x%02x  %s\n", pid, t, stream_type(t));
        i += 5 + el;
    }
}

static void feed_pat(const u8 *s, u32 n) { on_pat(s, n); }
static void feed_sdt(const u8 *s, u32 n) { on_sdt(s, n); }

static int cmd_services(int argc, char **argv)
{
    int want_pmt = argc > 2 && s_eq(argv[2], "pmt");
    if (need_init())
        return -1;
    if (!(dm_r(0x1D) & 0x40)) {
        uprintf("rf services: no FEC lock (0x1D = 0x%02x): 'rf tune' a multiplex first\n", dm_r(0x1D));
        return -1;
    }
    tsi_route();
    dmx_setup();
    dmx_filter(0, 0x0000);              /* PAT */
    dmx_filter(1, 0x0011);              /* SDT / BAT */
    rx_pat.pid = 0;
    rx_pat.len = 0;
    rx_sdt.pid = 0x11;
    rx_sdt.len = 0;
    pat_n = pat_done = 0;
    sdt_seen = 0;
    sdt_last = -1;
    sdt_count = 0;
    int pmt_filters = 0;
    for (int i = 0; i < 16; i++) {
        pmt_done[i] = 0;
        rx_pmt[i].len = 0;
    }
    u64 t0 = now_ms();
    u32 packets = 0;
    int ok = 0;
    while (now_ms() - t0 < 8000) {
        u32 n = dmx_waiting();
        for (u32 k = 0; k < n; k++) {
            const volatile u8 *pk = dmx_packet((ring_rd + k) % RF_RING_PACKETS);
            u16 pid = (u16)(((pk[1] & 0x1F) << 8) | pk[2]);
            packets++;
            if (pk[0] != 0x47)
                continue;
            if (pid == 0)
                section_feed(&rx_pat, pk, feed_pat);
            else if (pid == 0x11)
                section_feed(&rx_sdt, pk, feed_sdt);
            else
                for (int j = 0; j < pmt_filters; j++)
                    if (pid == pat_pmt[j]) {
                        pmt_index = j;
                        section_feed(&rx_pmt[j], pk, on_pmt);
                    }
        }
        dmx_consumed(n);
        if (want_pmt && pat_done && !pmt_filters) {
            for (int j = 0; j < pat_n; j++) {
                rx_pmt[j].pid = pat_pmt[j];
                dmx_filter(2 + j, pat_pmt[j]);
            }
            pmt_filters = pat_n;
        }
        int sdt_complete = sdt_last >= 0 && sdt_seen == (sdt_last >= 31 ? 0xFFFFFFFFu : (2u << sdt_last) - 1);
        int pmts = 1;
        for (int j = 0; j < pmt_filters; j++)
            pmts &= pmt_done[j];
        if (pat_done && sdt_complete && (!want_pmt || (pmt_filters && pmts))) {
            ok = 1;
            break;
        }
        if (stop_asked())
            break;
        delay_ms(2);
    }
    dmx_stop();
    if (!ok)
        uprintf("(after %u ms: %u packets on the filtered PIDs; PAT %s, SDT sections %s)\n",
                (u32)(now_ms() - t0), packets, pat_done ? "found" : "missing",
                sdt_count ? "incomplete" : "missing");
    return 0;
}

/* ---- register access for testing on the box ---- */

static int cmd_regs(int argc, char **argv)
{
    u32 reg, val, n = 1;
    const char *c = argv[1];
    int tuner = c[0] == 't', write = c[1] == 'w';
    if (argc < 3 || !parse_num(argv[2], &reg) || (write && (argc < 4 || !parse_num(argv[3], &val))) ||
        (!write && argc > 3 && !parse_num(argv[3], &n))) {
        uprintf("usage: rf dr <reg> [n] | rf dw <reg> <val> (demodulator 0x000..0x1FFF);\n"
                "       rf tr <reg> | rf tw <reg> <val> (tuner, I2C 0x60)\n");
        return -1;
    }
    if (tuner) {
        scb = SCB0_BASE;
        mxl_err = 0;
        if (write) {
            mxl_w((u8)reg, (u8)val);
            uprintf("tuner 0x%02x <- 0x%02x%s\n", reg, val, mxl_err ? " (no ACK)" : "");
        } else {
            u8 v = mxl_r((u8)reg);
            if (mxl_err)
                uprintf("tuner: %s\n", i2c_error(mxl_err));
            else
                uprintf("tuner 0x%02x = 0x%02x\n", reg, v);
        }
        return 0;
    }
    if (reg > 0x1FFF || n > 0x200) {
        uprintf("rf: demodulator registers 0x000..0x1FFF\n");
        return -1;
    }
    if (write) {
        if (reg == 0x10 && !(val & 0x80))
            uprintf("(0x10 without bit 7 is the demodulator's sleep: the original also changes 0xB8000088/8C/430)\n");
        dm_w(reg, (u8)val);
        uprintf("demod 0x%03x <- 0x%02x\n", reg, val);
        return 0;
    }
    for (u32 i = 0; i < n; i++) {
        if (i % 16 == 0)
            uprintf("%s0x%03x:", i ? "\n" : "", reg + i);
        uprintf(" %02x", dm_r(reg + i));
    }
    uprintf("\n");
    return 0;
}

/* every 7-bit address on SCB0 / SCB1 that answers a 1-byte read */
static int cmd_i2cscan(int argc, char **argv)
{
    u32 bus = 0;
    if (argc > 2 && (!parse_num(argv[2], &bus) || bus > 1)) {
        uprintf("usage: rf i2cscan [0|1]\n");
        return -1;
    }
    u32 base = bus ? SCB1_BASE : SCB0_BASE;
    scb_init(base);
    scb = base;
    uprintf("SCB%u (0x%08x):", bus, base);
    int found = 0;
    for (u32 a = 0x08; a < 0x78; a++) {
        u8 v;
        if (scb_read((u8)(a << 1), &v, 1) == 0) {
            uprintf(" 0x%02x", a);
            found++;
        }
        if (stop_asked())
            break;
    }
    uprintf(found ? "\n" : " nothing answers\n");
    scb = SCB0_BASE;
    return 0;
}

int rf_command(int argc, char **argv)
{
    const char *c = argc > 1 ? argv[1] : "";
    if (s_eq(c, "init"))
        return rf_init(1);
    if (s_eq(c, "tune"))
        return cmd_tune(argc, argv);
    if (s_eq(c, "status")) {
        if (need_init())
            return -1;
        show_status();
        return 0;
    }
    if (s_eq(c, "scan"))
        return cmd_scan(argc, argv);
    if (s_eq(c, "ts"))
        return cmd_ts(argc, argv);
    if (s_eq(c, "spectrum"))
        return cmd_spectrum(argc, argv);
    if (s_eq(c, "live"))
        return cmd_live(argc, argv);
    if (s_eq(c, "services"))
        return cmd_services(argc, argv);
    if (s_eq(c, "dr") || s_eq(c, "dw") || s_eq(c, "tr") || s_eq(c, "tw"))
        return cmd_regs(argc, argv);
    if (s_eq(c, "i2cscan"))
        return cmd_i2cscan(argc, argv);
    uprintf("rf init                       the tuner (MxL603, I2C 0x60) and the demodulator set up\n"
            "rf tune <MHz|ch> [6|7|8]      tune (default 8 MHz; 21..69 / 5..12 = a UHF / VHF channel)\n"
            "rf status                     lock, level, quality, errors, the stream's parameters\n"
            "rf scan [vhf|all|<from> <to>] every channel 474..858 MHz (vhf: 177.5..226.5 MHz, 7 MHz)\n"
            "rf ts <s> [file] [-p pids]    the stream: a PID table with rates, or a .ts file\n"
            "rf spectrum [from to [kHz]]   the aerial's level across 174..862 MHz as a chart on the TV\n"
            "rf live [from to]             that chart kept current, with keys: marker, zoom, move, pause, peaks\n"
            "rf services [pmt]             PAT and SDT: the services' ids, providers, names\n"
            "rf dr|dw|tr|tw <reg> [val]    demodulator / tuner registers; rf i2cscan [0|1]\n");
    return c[0] ? -1 : 0;
}
