/* a26.c: an Atari 2600 (VCS) -- its 6507, its picture chip (the TIA), its
   RIOT (RAM, timer, joystick and console switch ports) and the cartridge
   boards most games came on.  Nothing in it knows the box: a cartridge image
   in RAM goes in, pictures of 160 x 228 palette entries come out (play.c
   puts them on the TV and reads the buttons; a PC harness runs the same
   file). */
#ifndef DEMO_A26_H
#define DEMO_A26_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed int s32;

/* The picture: a colour clock is a pixel, 160 of them a line; 228 lines, the first one 23 lines after the line
   the game ends its vertical sync on -- Stella's default window: a game keeping to the usual 3 + 37 lines of
   sync and blanking shows its 192 lines as rows 14..205, and games with longer frames still fit.  A 2600 pixel
   is about 1.7 times as wide as it is tall on a TV: on a 1280 x 720 screen 5 x 3 (800 x 684) looks like the TV
   did, 6 x 3 (960 x 684) like Stella's default. */
#define A26_W 160
#define A26_H 228
#define A26_TOP 23

/* a frame of the NTSC console: 262 lines of 228 colour clocks at 3.579545 MHz (the game decides where a frame
   really ends; one that keeps to the standard takes this long) */
#define A26_FRAME_US 16688

/* the buttons, as a26_frame takes them (the same bits as nes.h's): A and B are the joystick's button, START the
   console's Reset switch, SELECT its Select switch */
#define A26_A      0x01
#define A26_B      0x02
#define A26_SELECT 0x04
#define A26_START  0x08
#define A26_UP     0x10
#define A26_DOWN   0x20
#define A26_LEFT   0x40
#define A26_RIGHT  0x80

const char *a26_load(u8 *image, u32 size);      /* a cartridge image in RAM (used where it is, never written): 0, or
                                                   why it cannot run */
void a26_reset(void);
void a26_frame(u8 pad, u8 *picture);            /* one frame, until the game's next vertical sync: A26_W x A26_H
                                                   entries of a26_rgb (0: no picture kept) */
const char *a26_info(void);                     /* "F8 + Superchip: 8 KB in 4 KB banks, 128 bytes of RAM" */
u8 *a26_save_ram(u32 *size, int *battery);      /* (none: size 0; there for play.c's table) */
int a26_save_changed(void);
u32 a26_cpu_cycles(void);                       /* 6507 cycles since the reset (1.19 MHz) */
int a26_jammed(void);                           /* the program ran into an instruction that stops the 6507 */
extern const u8 a26_rgb[128][3];                /* the NTSC palette: a picture entry is COLUxx >> 1 */

/* for tests: lines in the last frame */
u32 a26_frame_lines(void);

#ifdef A26_CPU_TEST
/* the 6507 alone over 64 KB of flat memory (Klaus Dormann's tests): a26.c built with -DA26_CPU_TEST */
extern u8 a26_test_memory[65536];
void a26_test_start(u16 pc);
void a26_test_step(u16 *pc, u8 *a, u8 *p, u32 *cycles);
void a26_test_registers(int set, u16 *pc, u8 *a, u8 *x, u8 *y, u8 *s, u8 *p);   /* set them, or read them */
#endif

#endif
