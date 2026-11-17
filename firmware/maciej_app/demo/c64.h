/* c64.c: a Commodore 64 (PAL) -- its 6510, the VIC-II (6569) picture chip,
   the two CIAs (keyboard, joysticks, timers, the VIC's bank), the SID's
   registers a program reads back (no sound), and its files: programs
   (.prg), tapes (.t64) and disks (.d64) loaded through a trap in the
   KERNAL's LOAD, and cartridges (.crt).  Nothing in it knows the box: the three ROMs and a file in
   RAM go in, pictures of 15-bit colours come out (play.c puts them on the TV
   and reads the keys; a PC harness runs the same file). */
#ifndef DEMO_C64_H
#define DEMO_C64_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed int s32;

#define C64_W 352                       /* the 320 x 200 screen with 16 pixels of border left and right, */
#define C64_H 240                       /* 20 lines above and below (on a 1280 x 720 TV: three times, 1056 x 720) */
#define C64_FRAME_US 19950              /* 312 lines of 63 cycles at 985248 Hz (50.12 frames a second) */

/* the pad's bits (nes.h's): a joystick in port 2 (A or B = fire); START = SPACE, SELECT = F7 (what title
   screens and trainer menus mostly ask for, besides fire) */
#define C64_A      0x01
#define C64_B      0x02
#define C64_SELECT 0x04
#define C64_START  0x08
#define C64_UP     0x10
#define C64_DOWN   0x20
#define C64_LEFT   0x40
#define C64_RIGHT  0x80

/* The ROMs (KERNAL and BASIC 8 KB, the characters 4 KB: used where they are, never written) and a file:
   a program (.prg, .p00), a tape (.t64) or a disk (.d64, 35 or 40 tracks; with error bytes too).  The
   KERNAL starts as on the real machine; once BASIC waits for keys, LOAD"*",8,1 is typed for a file (its
   first program), then RUN -- or SYS to its address when it does not load where BASIC programs go.  A LOAD
   from device 8 (or 1) finds the file's programs by name (* and ? as the 1541 takes them; "$" is the
   directory), so a game that loads more parts through the KERNAL loads them too.  file 0: BASIC alone.
   Or a cartridge (.crt, known by its header "C64 CARTRIDGE   ", whatever its name): plugged in, the
   machine starts in it at bank 0 -- the normal 8K, 16K and Ultimax ones, Final Cartridge III, Simons'
   BASIC, Ocean type 1, Fun Play / Power Play, Super Games, C64 Game System / System 3, Dinamic, Magic Desk
   / Domark / HES Australia, EasyFlash (its RAM too; writes to its flash are lost) and RGCD.  The file is
   used where it is (kept until the next c64_load).  0, or why it cannot run */
const char *c64_load(const u8 *kernal8k, const u8 *basic8k, const u8 *chargen4k, const u8 *file, u32 size,
                     const char *name);
void c64_reset(void);                           /* the machine as at power-on (the file typed for again; a
                                                   cartridge at its bank 0) */
/* one frame: C64_W x C64_H colours (0bbbbbgggggrrrrr; 0: no picture kept).  matrix[a] bit b: the key at
   the keyboard's column a (CIA 1 port A bit a, driven low to scan) and row b (port B bit b) is held --
   column 0: DEL RETURN CRSR-RIGHT F7 F1 F3 F5 CRSR-DOWN; 1: 3 W A 4 Z S E LEFT-SHIFT; 2: 5 R D 6 C F T X;
   3: 7 Y G 8 B H U V; 4: 9 I J 0 M K O N; 5: + P L - . : @ ,; 6: POUND * ; HOME RIGHT-SHIFT = UP-ARROW /;
   7: 1 LEFT-ARROW CTRL 2 SPACE C= Q RUN/STOP.  0: none held */
void c64_frame(u8 pad, const u8 matrix[8], u16 *picture);
/* a USB keyboard's held keys (HID usages; the modifiers as 0xE0-0xE7) as the C64's, where they are on its
   keyboard: letters, digits, Enter = RETURN, Backspace / Delete = INST/DEL, Home = CLR/HOME, Insert = POUND,
   End = UP-ARROW, the arrows = the cursor keys (SHIFT for up and left), F1-F8 (F2 = SHIFT F1 ...), Tab =
   RUN/STOP, PageUp = RESTORE, Shift = SHIFT, Left Ctrl = CTRL, Left Alt / Windows = C=; ` - = [ ] \ ; ' =
   LEFT-ARROW + - @ * = : ;.  Also a joystick: the keypad (8 4 6 2, 7 9 1 3, 5 / 0 fire), Right Ctrl or
   Right Alt = fire, in port 2 (F9 swaps the joystick -- this one and the pad's -- to port 1 and back).
   (The joystick and RESTORE are kept here and taken by the next c64_frame.) */
void c64_keys_from_hid(const u8 *usages, int n, u8 matrix[8]);
void c64_type(const char *text);                /* characters to type, each pressed for a few frames (from a
                                                   serial console, where only typed characters are known) */
int c64_typing(void);                           /* characters still to type */
const char *c64_info(void);                     /* "a .t64 tape, 2 files: DIZZY 4+ (42 KB)", "a cartridge
                                                   (EasyFlash, 64 banks): NAME (1024 KB)" */
u8 *c64_save_ram(u32 *size, int *battery);      /* (no cartridge RAM: size 0) */
int c64_save_changed(void);
u32 c64_cpu_cycles(void);                       /* 6510 cycles since the reset (985248 a second) */
int c64_jammed(void);                           /* the 6510 ran into an instruction that stops it for good */
extern const u8 c64_rgb[16][3];

/* for tests: memory as the 6510 sees it now, its registers, the raster line, the last LOAD the trap took
   (the name asked for; *found: 1 it was loaded, 0 not on the file, -1 none yet) */
u8 c64_peek(u16 address);
void c64_poke(u16 address, u8 v);
void c64_registers(u16 *pc, u8 *a, u8 *x, u8 *y, u8 *s, u8 *p);
u32 c64_raster(void);
const char *c64_last_load(int *found);

#ifdef C64_CPU_TEST
/* the 6510 alone over 64 KB of flat memory (Klaus Dormann's tests): c64.c built with -DC64_CPU_TEST */
extern u8 c64_test_memory[65536];
void c64_test_start(u16 pc);
void c64_test_step(u16 *pc, u32 *cycles);
#endif

#endif
