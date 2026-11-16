/* rf.c: the RF front end of the Comsat TE 1050 HD (board MC6379-VER1.0, the
   original firmware dump.bin): a MaxLinear MxL603 silicon tuner on the
   chip's hardware I2C master SCB0, the S3811's own COFDM (DVB-T)
   demodulator, the transport-stream input (TSI) and the demultiplexer DMX0
   that writes the stream's packets into a ring in RAM.  The console command
   'rf' (rf init / tune / status / scan / ts / services, and register peeks
   for testing on the box). */
#ifndef DEMO_RF_H
#define DEMO_RF_H

/* the console command: argv[0] is "rf"; 0 done, -1 a usage or hardware error
   (said on the UART) */
int rf_command(int argc, char **argv);

/* RAM the RF driver uses (physical; clear of everything hw.h lists: MEDIA_PHYS
   ends at 0x03400000, NTFS_RUNS_PHYS starts at 0x04000000) */
#define RF_RING_PHYS     0x03400000u      /* DMX0's packet ring: RF_RING_PACKETS x 188 bytes (1.1 MB) */
#define RF_RING_PACKETS  0x1800u          /* as dump.bin's own ring (+0x140 = 0x1800) */
#define RF_STAGE_PHYS    0x03600000u      /* 'rf ts <s> file': packets gathered here (up to RF_STAGE_SIZE)
                                             before each write to the disk */
#define RF_STAGE_SIZE    0x00900000u      /* 9 MB: ~3 s of a 24 Mbit/s multiplex, up to 0x03F00000 */

#endif
