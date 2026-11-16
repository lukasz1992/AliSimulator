/* amiga.c: an Amiga 500 (OCS chips, PAL) -- its 68000 at 7.09 MHz, 512 KB
   of chip RAM and 512 KB of slow RAM at 0xC00000, the Kickstart ROM at
   0xF80000 (and AROS's extension ROM at 0xE00000), Agnus (the copper, the
   blitter, the bitplane and sprite DMA, the beam), Denise (bitplanes in low
   and high resolution, dual playfield, HAM, extra half-brite, the scroll,
   the priorities, eight sprites), Paula (the interrupts, the floppy's DMA
   from an .adf made into MFM tracks as AmigaDOS writes them, the sound's
   interrupts without the sound), the two CIAs (timers, TOD, the keyboard's
   serial line, the floppy's lines, the joystick's fire button).  Nothing in
   it knows the box: the ROMs and a floppy image in RAM go in, pictures of
   15-bit colours come out (play.c puts them on the TV and reads the keys; a
   PC harness runs the same file). */
#ifndef DEMO_AMIGA_H
#define DEMO_AMIGA_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed int s32;

/* The picture: 352 x 272 low-resolution pixels -- PAL's usual 320 x 256 screen (DIWSTRT 0x2C81, DIWSTOP
   0x2CC1) with 16 pixels of border left and right and 8 lines above and below, where overscanned screens go
   (Magicland Dizzy's game is 320 x 259, its TRSi intro 352 wide); a high-resolution line shows each pair of its
   pixels as their average.  AMIGA_X0, AMIGA_Y0: where it starts, in the chips' own coordinates (the display
   window's).  On a 1280 x 720 screen: twice as wide and high (704 x 544). */
#define AMIGA_W  352
#define AMIGA_H  272
#define AMIGA_X0 0x71
#define AMIGA_Y0 0x24
#define AMIGA_FRAME_US 20032            /* 313 lines of 227 colour clocks at 3.546895 MHz (49.92 frames a second) */

/* the pad's bits (nes.h's): the joystick in port 2 (A = its fire button), B = the mouse's left button (crack
   screens and title pages wait for it); START = Return, SELECT = Space */
#define AMIGA_A      0x01
#define AMIGA_B      0x02
#define AMIGA_SELECT 0x04
#define AMIGA_START  0x08
#define AMIGA_UP     0x10
#define AMIGA_DOWN   0x20
#define AMIGA_LEFT   0x40
#define AMIGA_RIGHT  0x80

/* the Kickstart ROM (256 or 512 KB; copied): a real one (kick.rom) or AROS's free replacement (aros-rom.bin, 512
   KB, with its extension aros-ext.bin: 512 KB at 0xE00000; copied too, or 0); a floppy image (.adf, 901120
   bytes, used where it is and never written; or 0: no disk in the drive).  0, or why it cannot run */
const char *amiga_load(const u8 *kick, u32 kick_size, const u8 *ext, u32 ext_size, const u8 *adf, u32 adf_size,
                       const char *name);
void amiga_reset(void);                         /* the machine as at power-on (the floppy still in) */
/* one frame: AMIGA_W x AMIGA_H colours (0bbbbbgggggrrrrr; 0: no picture kept, the frame still run).  A line
   made from the same registers, colours, sprites and bitplane words as in the frame before is not drawn again:
   the same picture buffer every frame, kept between them.  keys: the Amiga's 128 key codes held (bit k & 7 of
   byte k >> 3), from amiga_keys_from_hid; 0: none */
void amiga_frame(u8 pad, const u8 keys[16], u16 *picture);
/* a USB keyboard's held keys (HID usages; the modifiers as 0xE0-0xE7) as the Amiga's: letters, digits,
   punctuation where a US keyboard has it, F1-F10, F11 / Insert / Home = Help, Delete, the arrows, the keypad,
   Shift, Ctrl, Alt, the Windows keys = the Amiga keys */
void amiga_keys_from_hid(const u8 *usages, int n, u8 keys[16]);
void amiga_type(const char *text);              /* characters to type, each pressed for a few frames */
int amiga_typing(void);                         /* characters still to type */
const char *amiga_info(void);                   /* "Kickstart 34.5, a floppy in DF0:" */
u8 *amiga_save_ram(u32 *size, int *battery);    /* (nothing kept: size 0) */
int amiga_save_changed(void);
u32 amiga_cpu_cycles(void);                     /* 68000 cycles since the reset */
int amiga_jammed(void);                         /* STOPped with every interrupt masked: nothing can wake it */

/* for tests: a byte of the 68000's memory; a custom chip register as last written; the 68000's registers D0-D7,
   A0-A7, PC, SR */
u8 amiga_peek(u32 address);
u16 amiga_custom(u32 offset);
void amiga_registers(u32 r[18]);

#ifdef AMIGA_CPU_TEST
/* the 68000 alone over 16 MB of flat memory (the single-step tests): amiga.c built with -DAMIGA_CPU_TEST */
void amiga_test_init(void);
void amiga_test_poke(u32 a, u8 v);
u8 amiga_test_peek(u32 a);
void amiga_test_registers(int set, u32 r[19]);  /* D0-D7, A0-A6, USP, SSP, SR, PC */
int amiga_test_step(void);                      /* one instruction: its cycles */
#endif

#endif
