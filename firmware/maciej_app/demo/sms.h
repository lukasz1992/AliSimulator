/* sms.c: a Sega Master System (NTSC), a Game Gear and an SG-1000 / SC-3000
   -- the Z80 (z80.c), the picture chip (mode 4 and the TMS9918A's modes),
   the cartridge mappers, the controllers.
   Nothing in it knows the box: a cartridge image in RAM goes in, pictures
   of 15-bit colours come out (play.c puts them on the TV and reads the
   buttons; a PC harness runs the same file against test cartridges). */
#ifndef DEMO_SMS_H
#define DEMO_SMS_H

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef signed int s32;

#define SMS_W 256                       /* the Master System's picture: 256 x 192, or x 224 in that mode (the
                                           SG-1000's: 256 x 192) */
#define SMS_H_MAX 224
#define GG_W 160                        /* the Game Gear's screen: the middle 160 x 144 of it */
#define GG_H 144
#define SMS_FRAME_US 16688              /* 262 lines of 228 T-states at 3.579545 MHz (59.92 frames a second) */

/* the buttons, as sms_frame takes them (the same bits as nes.h's and gb.h's) */
#define SMS_2      0x01                 /* (the A button: X on the keyboard) button 2 */
#define SMS_1      0x02                 /* (B: Z) button 1 */
#define SMS_START  0x08                 /* the Master System's Pause (an NMI), the Game Gear's Start */
#define SMS_UP     0x10
#define SMS_DOWN   0x20
#define SMS_LEFT   0x40
#define SMS_RIGHT  0x80

/* the machines sms_load runs a cartridge in */
#define SMS_MASTER_SYSTEM 0             /* .sms */
#define SMS_GAME_GEAR     1             /* .gg */
#define SMS_SG1000        2             /* .sg: 1 KB of RAM */
#define SMS_SC3000        3             /* .sc: the same with 2 KB (pads only, no keyboard) */

const char *sms_load(u8 *image, u32 size, int kind);       /* a cartridge file in RAM (used where it is; a
                                                   copier's 512-byte header skipped) as one of the SMS_
                                                   machines above: 0, or why it cannot run */
void sms_reset(void);
void sms_frame(u8 pad, u16 *picture);           /* one frame: sms_width() x sms_height() 15-bit colours
                                                   (0bbbbbgggggrrrrr; 0: no picture kept) */
void sms_pad2(u8 pad);                          /* the second controller's buttons (the Master System's) */
int sms_width(void);                            /* 256, or the Game Gear's 160 */
int sms_height(void);                           /* 192 or 224 (the mode the game set), the Game Gear's 144 */
const char *sms_info(void);                     /* "Master System, ROM 256 KB, Sega mapper" */
u8 *sms_save_ram(u32 *size, int *battery);      /* the cartridge's 32 KB of RAM (a header does not say
                                                   whether a cartridge has it: offered to every one; an
                                                   SG-1000's 8 KB, no battery) */
int sms_save_changed(void);                     /* written since the last call */
u32 sms_cpu_cycles(void);                       /* T-states since the reset */
int sms_jammed(void);                           /* halted with interrupts off and nothing to wake it */

/* for tests: the CPU */
struct z80 *sms_cpu(void);

#endif
