/* zx.c: a ZX Spectrum 48K -- the Z80 (z80.c), the ULA (the picture with
   its border, the keyboard, the memory it slows down), a Kempston
   joystick, the tape as files: snapshots (.sna, .z80) and tapes (.tap,
   .tzx's standard blocks, loaded at once through the ROM's LD-BYTES).
   Nothing in it knows the box: the ROM and a file in RAM go in, pictures of
   15-bit colours come out (play.c puts them on the TV and reads the keys; a
   PC harness runs the same file). */
#ifndef DEMO_ZX_H
#define DEMO_ZX_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed int s32;

#define ZX_W 320                        /* the 256 x 192 picture with 32 pixels of border left and right, */
#define ZX_H 240                        /* 24 lines above and below */
#define ZX_FRAME_US 19968               /* 69888 T-states at 3.5 MHz (50.08 frames a second) */

/* the pad's bits (nes.h's): the Kempston joystick (A or B = fire); START = ENTER, SELECT = SPACE */
#define ZX_A      0x01
#define ZX_B      0x02
#define ZX_SELECT 0x04
#define ZX_START  0x08
#define ZX_UP     0x10
#define ZX_DOWN   0x20
#define ZX_LEFT   0x40
#define ZX_RIGHT  0x80

/* the 48K ROM (16 KB, used where it is) and a file: a snapshot (.sna, .z80: 48K ones) runs at once; a tape
   (.tap, .tzx) is loaded the way LOAD "" would, typed once the ROM has started (file 0: BASIC alone).
   0, or why it cannot run */
const char *zx_load(const u8 *rom16k, u8 *file, u32 size, const char *name);
void zx_reset(void);                            /* the machine as at power-on (the tape back at its start) */
/* one frame: ZX_W x ZX_H colours (0bbbbbgggggrrrrr; 0: no picture kept).  matrix[row]: bit k set when key k
   of half-row `row` is held -- row 0 is port 0xFEFE (CAPS SHIFT, Z, X, C, V), 1 0xFDFE (A S D F G), 2 0xFBFE
   (Q W E R T), 3 0xF7FE (1 2 3 4 5), 4 0xEFFE (0 9 8 7 6), 5 0xDFFE (P O I U Y), 6 0xBFFE (ENTER L K J H),
   7 0x7FFE (SPACE, SYMBOL SHIFT, M, N, B); 0: none held */
void zx_frame(u8 pad, const u8 matrix[8], u16 *picture);
/* a USB keyboard's held keys (HID usages; the modifiers as 0xE0-0xE7) as the Spectrum's: letters, digits,
   Enter, Space, Backspace = DELETE, the arrows = CAPS SHIFT + 5 6 7 8, Shift = CAPS SHIFT, Ctrl / Alt =
   SYMBOL SHIFT, punctuation (Shift's too) = SYMBOL SHIFT + the key that has it */
void zx_keys_from_hid(const u8 *usages, int n, u8 matrix[8]);
void zx_type(const char *text);                 /* characters to type, each pressed for a few frames (from a
                                                   serial console, where only typed characters are known) */
int zx_typing(void);                            /* characters still to type */
const char *zx_info(void);                      /* "a tape, 12 blocks (6 KB)" / "a .z80 snapshot (version 3)" */
u8 *zx_save_ram(u32 *size, int *battery);       /* (no cartridge RAM: size 0) */
int zx_save_changed(void);
u32 zx_cpu_cycles(void);                        /* T-states since the reset */
int zx_jammed(void);                            /* halted with interrupts off: nothing can wake it */
extern const u8 zx_rgb[16][3];

/* for tests: the CPU, a byte of memory */
struct z80 *zx_cpu(void);
u8 zx_peek(u16 address);

#endif
