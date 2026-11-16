/* The game players on the box ('nes <file>', 'gb <file>'): a cartridge file
   from a disk into RAM, the machine (nes.c, gb.c) run one frame at its own
   rate (the NES's 60.1 a second, the Game Boy's 59.7), its picture enlarged
   to the screen's whole height -- the NES's 256 x 240 three times (768 x
   720), the Game Boy's 160 x 144 five times (800 x 720) -- in the middle of
   the test screen's picture.  The NES's picture comes as its 64 colours'
   numbers, the Game Boy's (the Color's too) as 15-bit colours.

   The buttons, the same eight on both: a USB keyboard's keys as long as
   they are held (arrows or W A S D, X or K = A, Z or J = B, Enter = Start,
   Tab or Space = Select); the UART and the telnet console, where a key is
   only known when it is typed, so each counts as a short press (the same
   keys; the terminal's auto-repeat keeps a direction going); the remote
   (arrows, OK = A, 0 = B, MENU = Start, INFO = Select, EXIT ends); on a
   board with two front buttons, UP = Start and DOWN = A.  Ctrl-C or Esc
   ends.

   A cartridge with a battery keeps its RAM in a file beside it (game.nes
   -> game.sav), read at the start and written at the end when the game
   changed it.  A machine that makes sound (the NES) plays it through snd.c
   (HDMI and SCART), 48 kHz. */
#include "hw.h"
#include "nes.h"
#include "gb.h"
#include "sms.h"
#include "zx.h"
#include "a26.h"
#include "c64.h"
#include "amiga.h"
#include "dos.h"
#include "snd.h"

/* a machine, as the player sees it */
struct machine {
    int w, h, xs, ys;                   /* the picture's size; how many times wider and higher on the TV */
    u32 frame_us;                       /* a frame's time */
    const char *(*load)(u8 *image, u32 size, const char *path);
    const char *(*info)(void);
    void (*frame)(u8 pad, void *picture);
    u8 *(*save_ram)(u32 *size, int *battery);
    int (*save_changed)(void);
    int (*jammed)(void);
    const u8 (*rgb)[3];
    int colours;                        /* the picture: numbers of rgb's colours, a byte each; 0: 15-bit colours
                                           (0bbbbbgggggrrrrr), two bytes each */
    const char *cpu;
    /* a machine with a computer's keyboard (the Spectrum): every key held on the USB keyboard as HID usages
       (modifiers 0xE0..0xE7) before each frame, and a character typed on the UART / telnet console; its
       USB keys then make no buttons but the arrows, and only Esc ends (Ctrl is a key of the machine's) */
    void (*keyboard)(const u8 *usages, int n);
    void (*typed)(char c);
    const char *keys;                   /* the keys, said when it starts (0: the NES's) */
    void (*size)(int *w, int *h);       /* a picture whose size the game changes (the Master System's 192 or
                                           224 lines): asked after each frame */
    /* sound (0: none): the rate to make it at (0: none made), and after each frame the samples made since the
       last call (mono, 16-bit) -- they go to the TV's HDMI sound and the SCART's (snd.c) */
    void (*audio_rate)(u32 hz);
    int (*audio)(s16 *out, int max);
};
static const struct machine *m;

static const char *nes_open(u8 *image, u32 size, const char *path)
{
    (void)path;
    return nes_load(image, size);
}

static const char *gb_open(u8 *image, u32 size, const char *path)
{
    (void)path;
    return gb_load(image, size);
}

static void nes_one(u8 pad, void *picture)
{
    nes_frame(pad, 0, (u8 *)picture);
}

static void gb_one(u8 pad, void *picture)
{
    gb_frame(pad, (u16 *)picture);
}

static const struct machine nes_machine = {NES_W, NES_H, 3, 3, 16639, nes_open, nes_info, nes_one, nes_save_ram,
                                           nes_save_changed, nes_jammed, nes_rgb, 64, "the 6502", 0, 0, 0, 0,
                                           nes_audio_rate, nes_audio};
static const struct machine gb_machine = {GB_W, GB_H, 5, 5, 16743, gb_open, gb_info, gb_one, gb_save_ram,
                                          gb_save_changed, gb_jammed, 0, 0, "the Game Boy's CPU", 0, 0, 0, 0, 0, 0};

/* the Master System (256 x 192, three times: 768 x 576) and the Game Gear (its 160 x 144 screen five times) */
static const char *sms_open(u8 *image, u32 size, const char *path)
{
    (void)path;
    return sms_load(image, size, 0);
}

static const char *gg_open(u8 *image, u32 size, const char *path)
{
    (void)path;
    return sms_load(image, size, 1);
}

static void sms_one(u8 pad, void *picture)
{
    sms_frame(pad, (u16 *)picture);
}

static void sms_size(int *w, int *h)
{
    *w = sms_width();
    *h = sms_height();
}

static const struct machine sms_machine = {SMS_W, 192, 3, 3, SMS_FRAME_US, sms_open, sms_info, sms_one,
                                           sms_save_ram, sms_save_changed, sms_jammed, 0, 0, "the Z80", 0, 0,
                                           "arrows or W A S D, X = button 2, Z = button 1, Enter = Pause; Ctrl-C or "
                                           "Esc ends", sms_size, 0, 0};
static const struct machine gg_machine = {GG_W, GG_H, 5, 5, SMS_FRAME_US, gg_open, sms_info, sms_one,
                                          sms_save_ram, sms_save_changed, sms_jammed, 0, 0, "the Z80", 0, 0,
                                          "arrows or W A S D, X = button 2, Z = button 1, Enter = Start; Ctrl-C or "
                                          "Esc ends", 0, 0, 0};

/* the ZX Spectrum 48K: 320 x 240 with its border, three times (960 x 720).  Its ROM is not in the demo: a
   file 48.rom (16 KB) in the program's directory, or zx/48.rom or 48.rom at its volume's top */
static u8 zx_rom[16384];
static u8 zx_matrix[8];

static void zx_join(char *out, int size, const char *dir, int n, const char *name)     /* dir's first n, name */
{
    int k = 0;
    for (int i = 0; i < n && k < size - 1; i++)
        out[k++] = dir[i];
    while (*name && k < size - 1)
        out[k++] = *name++;
    out[k] = 0;
}

/* a machine's ROM file `name` of `least`..`want` bytes into rom: beside the program, in <sub>/ or at the top of
   its volume, or (no volume in the path) in <sub>/ on any disk; its size, 0 when not found */
static u32 rom_find(const char *path, const char *sub, const char *name, u8 *rom, u32 least, u32 want)
{
    char dir[160], try[200], in_sub[40], top[40];
    usnprintf(in_sub, sizeof(in_sub), "/%s/%s", sub, name);
    usnprintf(top, sizeof(top), "/%s", name);
    int n = 0, cut = 0, colon = -1;
    const char *base = *path ? path : disk_cwd();
    for (; base[n] && n < (int)sizeof(dir) - 1; n++) {
        dir[n] = base[n];
        if (base[n] == '/' || base[n] == '\\')
            cut = n + 1;
        if (base[n] == ':' && colon < 0)
            colon = n;
    }
    dir[n] = 0;
    if (!*path) {                       /* (the current directory itself) */
        if (n && dir[n - 1] != '/')
            dir[n++] = '/', dir[n] = 0;
        cut = n;
    }
    for (int k = 0; k < 3 + 16; k++) {
        if (k == 0) {
            if (!cut)
                continue;
            zx_join(try, sizeof(try), dir, cut, name);
        } else if (k < 3) {
            if (colon < 0)
                continue;
            zx_join(try, sizeof(try), dir, colon + 1, k == 1 ? in_sub : top);
        } else {                        /* (no directory to start from: <sub>/<name> on any disk) */
            if (colon >= 0)
                break;
            usnprintf(try, sizeof(try), "%d:%s", k - 3, in_sub);
        }
        u32 size = 0;
        if (disk_exists(try) && disk_read_head(try, rom, want, &size) >= (int)least && size >= least &&
            size <= want) {
            uprintf("ROM: %s\n", try);
            return size;
        }
    }
    return 0;
}

static int rom_read(const char *path, const char *sub, const char *name, u8 *rom, u32 size)
{
    return rom_find(path, sub, name, rom, size, size) != 0;
}

static int zx_rom_read(const char *path)
{
    return rom_read(path, "zx", "48.rom", zx_rom, sizeof(zx_rom));
}

static const char *zx_open(u8 *image, u32 size, const char *path)
{
    if (!zx_rom_read(path))
        return "no 48.rom (the Spectrum 48K's ROM, 16 KB) beside it, in zx/ or at the top of its disk";
    for (int i = 0; i < 8; i++)
        zx_matrix[i] = 0;
    return zx_load(zx_rom, size ? image : 0, size, path);
}

static void zx_one(u8 pad, void *picture)
{
    zx_frame(pad, zx_matrix, (u16 *)picture);
}

static void zx_keyboard(const u8 *usages, int n)
{
    zx_keys_from_hid(usages, n, zx_matrix);
}

static void zx_typed(char c)
{
    char s[2] = {c, 0};
    zx_type(s);
}

static const struct machine zx_machine = {ZX_W, ZX_H, 3, 3, ZX_FRAME_US, zx_open, zx_info, zx_one, zx_save_ram,
                                          zx_save_changed, zx_jammed, 0, 0, "the Z80", zx_keyboard, zx_typed,
                                          "the USB keyboard is the Spectrum's (Shift = CAPS SHIFT, Ctrl / Alt = "
                                          "SYMBOL SHIFT), its arrows a Kempston joystick too; what is typed on this "
                                          "console is typed on it; Esc ends", 0, 0, 0};

#define PAD_A      0x01                 /* (nes.h's and gb.h's bits) */
#define PAD_B      0x02
#define PAD_SELECT 0x04
#define PAD_START  0x08
#define PAD_UP     0x10
#define PAD_DOWN   0x20
#define PAD_LEFT   0x40
#define PAD_RIGHT  0x80
#define TAP_FRAMES 6                    /* a key typed on the UART: the button held this many frames */
#define IR_FRAMES  9                    /* a remote's key: until its repeat frame would have come (108 ms) */

#define PICTURE_MAX (352 * 272)          /* the largest picture: the Amiga's with its border */
static u16 picture[PICTURE_MAX], before[PICTURE_MAX];         /* (u8 for the NES, u16 for the others) */
static u16 colours[128];
static int pw, ph;                      /* the picture's size now (m->w, m->h, or what m->size says) */
static u8 taps[8];                      /* frames left of each button's short press */

static u32 crc32(const u8 *p, u32 n)
{
    u32 c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

/* the lines of the picture that changed, xs times as wide and ys times as high, into the middle of the test
   screen's picture (`all`: every line) */
static void show(int all)
{
    /* (uncached; through the cache with its lines written back after each line is no faster: the whole 800 x 720
       takes 9 to 10 ms either way) */
    volatile u32 *fb = (volatile u32 *)KSEG1(FB_PHYS);
    static union {
        u32 pair[SCREEN_W / 2];
        u16 pixel[SCREEN_W];
    } row;
    int out_w = pw * m->xs, out_x = (SCREEN_W - out_w) / 2 & ~1, out_y = (SCREEN_H - ph * m->ys) / 2;
    for (int y = 0; y < ph; y++) {
        if (!m->colours) {                              /* 15-bit colours (the Game Boy's, ...) */
            const u16 *line = picture + y * pw;
            u16 *old = before + y * pw;
            int same = !all;
            for (int x = 0; x < pw && same; x++)
                same = line[x] == old[x];
            if (same)
                continue;
            for (int x = 0, k = 0; x < pw; x++) {
                u16 c = old[x] = line[x];
                u16 p = (u16)(0x8000u | (c & 0x1Fu) << 10 | (c & 0x3E0u) | (c >> 10 & 0x1Fu));
                for (int i = 0; i < m->xs; i++)
                    row.pixel[k++] = p;
            }
            goto out;
        }
        const u8 *line = (const u8 *)picture + y * pw;
        u8 *old = (u8 *)before + y * pw;
        int same = !all;
        for (int x = 0; x < pw && same; x++)
            same = line[x] == old[x];
        if (same)
            continue;
        if (m->xs == 3) {
            for (int x = 0, w = 0; x < pw; x += 2) {
                u32 p = colours[old[x] = line[x]], q = colours[old[x + 1] = line[x + 1]];
                row.pair[w++] = p | p << 16;
                row.pair[w++] = p | q << 16;
                row.pair[w++] = q | q << 16;
            }
        } else {
            for (int x = 0, k = 0; x < pw; x++) {
                u16 p = colours[old[x] = line[x]];
                for (int i = 0; i < m->xs; i++)
                    row.pixel[k++] = p;
            }
        }
    out:
        for (int r = 0; r < m->ys; r++) {
            volatile u32 *to = fb + ((u32)(out_y + y * m->ys + r) * SCREEN_W + (u32)out_x) / 2;
            for (int w = 0; w < out_w / 2; w++)
                to[w] = row.pair[w];
        }
    }
}

static void tap(u8 button, int frames)
{
    for (int i = 0; i < 8; i++)
        if (button & (1u << i))
            taps[i] = (u8)frames;
}

/* the buttons held now; -1 when the player asked to end.  `start`: a game begins -- the USB keyboard's keys
   held now (the Enter that opened it in the explorer) count only once they were let go */
static int buttons(int start)
{
    static u8 esc;                      /* an escape sequence's bytes so far: 1 ESC, 2 ESC [ */
    static u64 esc_at;
    static u16 stale;                   /* keys[] held since the start, not let go yet */
    u8 pad = 0;
    if (start)
        esc = 0;
    int c;
    usb_int_poll();
    (void)kbd_poll();
    while ((c = kbd_getc()) >= 0)
        if (c == 3 && !m->keyboard)
            return -1;
    if (m->keyboard) {                  /* a computer's keyboard: every key to it, the arrows also as buttons */
        u8 usages[16];
        int n = kbd_held_all(usages, 16);
        for (int i = 0; i < n; i++)
            if (usages[i] == 0x29)      /* Esc ends */
                return -1;
        m->keyboard(usages, start ? 0 : n);
    }
    static const struct {
        u8 usage, button;
    } keys[] = {{0x52, PAD_UP}, {0x1A, PAD_UP}, {0x51, PAD_DOWN}, {0x16, PAD_DOWN}, {0x50, PAD_LEFT},
                {0x04, PAD_LEFT}, {0x4F, PAD_RIGHT}, {0x07, PAD_RIGHT}, {0x1B, PAD_A}, {0x0E, PAD_A},
                {0x1D, PAD_B}, {0x0D, PAD_B}, {0x28, PAD_START}, {0x2B, PAD_SELECT}, {0x2C, PAD_SELECT}};
    for (unsigned i = 0; i < (m->keyboard ? 4u : sizeof(keys) / sizeof(keys[0])); i++) {
        unsigned k = m->keyboard ? 2 * i : i;           /* (a computer's keyboard: the arrows alone) */
        int down = kbd_held(keys[k].usage);
        if (start && down)
            stale |= (u16)(1u << i);
        else if (!down)
            stale &= (u16)~(1u << i);
        else if (!(stale & (1u << i)))
            pad |= keys[k].button;
    }
    while ((c = uart_getc()) >= 0) {
        if (c == 3)
            return -1;
        if (esc == 2) {
            esc = 0;
            tap(c == 'A' ? PAD_UP : c == 'B' ? PAD_DOWN : c == 'C' ? PAD_RIGHT : c == 'D' ? PAD_LEFT : 0, TAP_FRAMES);
            continue;
        }
        if (esc == 1) {
            esc = c == '[' ? 2 : 0;
            continue;
        }
        if (m->typed && c != 27) {      /* a computer: the character typed on it */
            m->typed((char)c);
            continue;
        }
        switch (c) {
        case 27: esc = 1, esc_at = now_ms(); break;
        case 'w': tap(PAD_UP, TAP_FRAMES); break;
        case 's': tap(PAD_DOWN, TAP_FRAMES); break;
        case 'a': tap(PAD_LEFT, TAP_FRAMES); break;
        case 'd': tap(PAD_RIGHT, TAP_FRAMES); break;
        case 'x': case 'k': tap(PAD_A, TAP_FRAMES); break;
        case 'z': case 'j': tap(PAD_B, TAP_FRAMES); break;
        case '\r': case '\n': tap(PAD_START, TAP_FRAMES); break;
        case '\t': case ' ': tap(PAD_SELECT, TAP_FRAMES); break;
        }
    }
    if (esc == 1 && now_ms() - esc_at > 150)
        return -1;                      /* (ESC alone, nothing after it: the Esc key) */
    struct ir_event ev;
    static u8 ir_button;
    while (ir_poll(&ev)) {
        if (ev.valid && !ev.repeat) {
            const char *name = ir_key_name(ev.code);
            static const struct {
                const char *name;
                u8 button;
            } remote[] = {{"UP", PAD_UP}, {"DOWN", PAD_DOWN}, {"LEFT", PAD_LEFT}, {"RIGHT", PAD_RIGHT},
                          {"OK", PAD_A}, {"0", PAD_B}, {"MENU", PAD_START}, {"INFO", PAD_SELECT}};
            ir_button = 0;
            for (unsigned i = 0; name && i < sizeof(remote) / sizeof(remote[0]); i++) {
                const char *a = name, *b = remote[i].name;
                while (*a && *a == *b)
                    a++, b++;
                if (!*a && !*b)
                    ir_button = remote[i].button;
            }
            if (name && name[0] == 'E' && name[1] == 'X')      /* EXIT */
                return -1;
        }
        if (ev.valid)
            tap(ir_button, IR_FRAMES);                          /* (a repeat frame: the same key still held) */
    }
    if (!panel_present()) {
        int held = panel_buttons();
        if (held & PANEL_BUTTON_UP)
            pad |= PAD_START;
        if (held & PANEL_BUTTON_DOWN)
            pad |= PAD_A;
    }
    for (int i = 0; i < 8; i++)
        if (taps[i]) {
            taps[i]--;
            pad |= (u8)(1u << i);
        }
    return pad;
}

/* "frame:buttons,frame:buttons,...": the hex buttons held from that frame on (a test's script) */
static u8 scripted(const char **script, u32 frame, u8 pad)
{
    const char *s = *script;
    while (*s) {
        u32 at = 0, v = 0;
        const char *p = s;
        while (*p >= '0' && *p <= '9')
            at = at * 10 + (u32)(*p++ - '0');
        if (*p != ':' || at > frame)
            break;
        for (p++; (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'); p++)
            v = v * 16 + (u32)(*p <= '9' ? *p - '0' : *p - 'a' + 10);
        pad = (u8)v;
        s = *p == ',' ? p + 1 : p;
        if (*p != ',')
            break;
    }
    *script = s;
    return pad;
}

/* the cartridge's file name with another ending: "4:/games/a.nes" -> "4:/games/a.sav" */
static int save_path(const char *path, char *out, int size)
{
    int n = 0, dot = -1;
    for (; path[n]; n++)
        if (path[n] == '.')
            dot = n;
        else if (path[n] == '/' || path[n] == '\\')
            dot = -1;
    if (dot < 0)
        dot = n;
    if (dot + 5 > size)
        return 0;
    for (int i = 0; i < dot; i++)
        out[i] = path[i];
    out[dot] = '.';
    out[dot + 1] = 's';
    out[dot + 2] = 'a';
    out[dot + 3] = 'v';
    out[dot + 4] = 0;
    return 1;
}

/* 'nes' / 'gb' [-f frames] [-k script] <file>: frames 0 = until the player ends it.  0, -1 it could not
   start */
static int play(const struct machine *machine, const char *path, u32 frames, const char *script)
{
    m = machine;
    u8 *image = (u8 *)KSEG0(MEDIA_PHYS);
    u32 size = 0;
    tv_opening(path, 0);                        /* (on the TV at once: reading a game takes a moment) */
    int got = *path ? disk_read_head(path, image, MEDIA_MAX, &size) : 0;      /* ("": no file -- BASIC) */
    const char *why = 0;
    if (got >= 0 && size > MEDIA_MAX) {
        uprintf("%s: %u bytes -- more than a cartridge file can be here (%u MB)\n", path, size, MEDIA_MAX >> 20);
        got = -1;
    }
    if (got >= 0 && (why = m->load(image, (u32)got, path)) != 0) {
        uprintf("%s: %s\n", path, why);
        got = -1;
    }
    if (got < 0) {
        tv_failed(path, 0, !frames);            /* (a test's run: no waiting) */
        return -1;
    }
    uprintf("%s: %s\n", path, m->info());
    char sav[128];
    u32 ram_size;
    int battery, have_sav = save_path(path, sav, sizeof(sav));
    u8 *ram = m->save_ram(&ram_size, &battery);
    if (battery && have_sav && disk_exists(sav)) {
        u32 n = 0;
        if (disk_read_head(sav, ram, ram_size, &n) > 0)
            uprintf("its battery RAM read from %s\n", sav);
        (void)m->save_changed();
    }
    for (int i = 0; i < m->colours; i++)
        colours[i] = RGB(m->rgb[i][0], m->rgb[i][1], m->rgb[i][2]);
    for (int i = 0; i < 8; i++)
        taps[i] = 0;
    pw = m->w;
    ph = m->h;
    gfx_target(0);
    gfx_fill(0, 0, SCREEN_W, SCREEN_H, RGB(0, 0, 0));
    gfx_view(0);
    uprintf("playing: %s\n", m->keys ? m->keys : "arrows or W A S D, X = A, Z = B, Enter = Start, Tab = Select; "
            "Ctrl-C or Esc ends");
    u32 frame_ticks = ticks_per_ms() * (m->frame_us / 1000u) + ticks_per_ms() * (m->frame_us % 1000u) / 1000u;
    u64 next = now_ticks();
    u32 n = 0, late = 0, emu = 0, draw = 0, per = ticks_per_ms() / 1000u;   /* (emu, draw: microseconds) */
    u32 slowest = 0, slowest_at = 0, slowest_draw = 0, over = 0;     /* (the longest frame's run and draw
                                                                       together; frames longer than their time) */
    u8 pad = 0;
    int first = 1, by_script = *script != 0;    /* (a test's buttons: the script's alone) */
    /* sound, played as it is made (a test's run of -f frames makes none); what does not fit in the sound's ring
       (a game ahead of it) is dropped */
    static s16 samples[4096];
    u32 dropped = 0;
    int sound = !frames && m->audio && snd_init(0) == 0;
    if (sound)
        m->audio_rate(snd_rate());
    u64 started_ms = now_ms();
    for (;; n++) {
        if (frames && n == frames)
            break;
        int held = buttons(n == 0);
        if (held < 0)
            break;
        pad = by_script ? scripted(&script, n, pad) : (u8)held;
        u64 t0 = now_ticks();
        m->frame(pad, picture);
        if (sound) {
            int k = m->audio(samples, (int)(sizeof(samples) / sizeof(samples[0]))), room = snd_room();
            if (k > room)
                dropped += (u32)(k - room), k = room;
            if (k > 0)
                snd_write(samples, k);
        }
        if (m->size) {                          /* (the game changed its picture's size: drawn again whole) */
            int w, h;
            m->size(&w, &h);
            if (w != pw || h != ph) {
                pw = w;
                ph = h;
                gfx_fill(0, 0, SCREEN_W, SCREEN_H, RGB(0, 0, 0));
                first = 1;
            }
        }
        u64 t1 = now_ticks();
        show(first);
        first = 0;
        u64 t2 = now_ticks();
        emu += (u32)(t1 - t0) / per;
        draw += (u32)(t2 - t1) / per;
        u32 took = (u32)(t2 - t0) / per;
        if (took > slowest)
            slowest = took, slowest_at = n, slowest_draw = (u32)(t2 - t1) / per;
        if (took > m->frame_us)
            over++;
        if (m->jammed()) {
            uprintf("the game's program stopped %s (an instruction that halts it) at frame %u\n", m->cpu, n);
            break;
        }
        if (!frames) {                          /* (a test's frames run as fast as they go) */
            next += frame_ticks;
            if (now_ticks() > next + 4u * frame_ticks) {
                next = now_ticks();             /* too far behind: no catching up */
                late++;
            }
            while (now_ticks() < next)
                console_service();
        }
    }
    u32 ms = (u32)(now_ms() - started_ms);
    uprintf("%u frames in %u.%u s; a frame took %u us to run and %u us to draw (the slowest %u us of %u, frame %u, "
            "%u of it drawing; %u longer)%s; picture CRC-32 %08x\n", n, ms / 1000, ms % 1000 / 100, n ? emu / n : 0,
            n ? draw / n : 0,
            slowest, m->frame_us, slowest_at, slowest_draw, over, late ? " (some frames came late)" : "",
            crc32((const u8 *)picture, (u32)(pw * ph * (m->colours ? 1 : 2))));
    if (sound) {
        m->audio_rate(0);
        snd_stop();
        if (dropped)
            uprintf("sound: %u samples did not fit in the sound's ring\n", dropped);
    }
    if (battery && have_sav && m->save_changed())
        disk_write_file(sav, ram, ram_size, 0);
    return 0;
}

int nes_play(const char *path, u32 frames, const char *script)
{
    return play(&nes_machine, path, frames, script);
}

int gb_play(const char *path, u32 frames, const char *script)
{
    return play(&gb_machine, path, frames, script);
}

int sms_play(const char *path, u32 frames, const char *script)
{
    return play(&sms_machine, path, frames, script);
}

int gg_play(const char *path, u32 frames, const char *script)
{
    return play(&gg_machine, path, frames, script);
}

/* the Atari 2600: 160 x 228 of its 128 colours' numbers, five times as wide and three times as high (800 x
   684: its pixels were wide on a TV) */
static const char *a26_open(u8 *image, u32 size, const char *path)
{
    (void)path;
    return a26_load(image, size);
}

static void a26_one(u8 pad, void *picture)
{
    a26_frame(pad, (u8 *)picture);
}

static const struct machine a26_machine = {A26_W, A26_H, 5, 3, A26_FRAME_US, a26_open, a26_info, a26_one,
                                           a26_save_ram, a26_save_changed, a26_jammed, a26_rgb, 128, "the 6507",
                                           0, 0, "arrows or W A S D, X or Z = fire, Enter = the console's Reset, "
                                           "Tab = Select; Ctrl-C or Esc ends", 0, 0, 0};

int a26_play(const char *path, u32 frames, const char *script)
{
    return play(&a26_machine, path, frames, script);
}

/* the Commodore 64 (PAL): 352 x 240 (the 320 x 200 screen with 16 pixels of border left and right, 20 lines
   above and below), three times (1056 x 720).  Its ROMs are not in the demo: kernal.rom (8 KB), basic.rom
   (8 KB) and chargen.rom (4 KB) beside the program, in c64/ or at the top of its disk (VICE's files work,
   renamed) */
static u8 c64_kernal[8192], c64_basic[8192], c64_chargen[4096];
static u8 c64_matrix[8];

static const char *c64_open(u8 *image, u32 size, const char *path)
{
    if (!rom_read(path, "c64", "kernal.rom", c64_kernal, sizeof(c64_kernal)) ||
        !rom_read(path, "c64", "basic.rom", c64_basic, sizeof(c64_basic)) ||
        !rom_read(path, "c64", "chargen.rom", c64_chargen, sizeof(c64_chargen)))
        return "no kernal.rom, basic.rom and chargen.rom (the C64's ROMs: 8, 8 and 4 KB) beside it, in c64/ or "
               "at the top of its disk";
    for (int i = 0; i < 8; i++)
        c64_matrix[i] = 0;
    return c64_load(c64_kernal, c64_basic, c64_chargen, size ? image : 0, size, path);
}

static void c64_one(u8 pad, void *picture)
{
    c64_frame(pad, c64_matrix, (u16 *)picture);
}

static void c64_keyboard(const u8 *usages, int n)
{
    c64_keys_from_hid(usages, n, c64_matrix);
}

static void c64_typed(char c)
{
    char s[2] = {c, 0};
    c64_type(s);
}

static const struct machine c64_machine = {C64_W, C64_H, 3, 3, C64_FRAME_US, c64_open, c64_info, c64_one,
                                           c64_save_ram, c64_save_changed, c64_jammed, 0, 0, "the 6510",
                                           c64_keyboard, c64_typed,
                                           "the USB keyboard is the C64's (Tab = RUN/STOP, PageUp = RESTORE, Alt = "
                                           "C=), its arrows joystick 2 (fire: right Ctrl, the remote's OK; F9 "
                                           "swaps the ports); the remote's MENU = SPACE, INFO = F7; what is typed "
                                           "on this console is typed on it; Esc ends", 0, 0, 0};

int c64_play(const char *path, u32 frames, const char *script)
{
    return play(&c64_machine, path, frames, script);
}

/* the Amiga 500 (PAL): 352 x 272 (PAL's 320 x 256 screen with 16 pixels of border left and right, 8 lines
   above and below), twice as large (704 x 544).  Its Kickstart is not in the demo: kick.rom (256 or 512 KB, the
   user's own), or AROS's free replacement aros-rom.bin with aros-ext.bin, beside the floppy image, in amiga/ or
   at the top of its disk.  amiga_load copies them: read into the top of the media area, above the .adf */
#define AMIGA_ROM_AT  (MEDIA_PHYS + MEDIA_MAX - 0x00100000u)
static u8 amiga_keys[16];

static const char *amiga_open(u8 *image, u32 size, const char *path)
{
    u8 *kick = (u8 *)KSEG0(AMIGA_ROM_AT), *ext = kick + 0x80000;
    u32 kick_size, ext_size = 0;
    if (size > AMIGA_ROM_AT - MEDIA_PHYS)
        return "too large for a floppy image";
    kick_size = rom_find(path, "amiga", "kick.rom", kick, 0x40000, 0x80000);
    if (!kick_size) {
        kick_size = rom_find(path, "amiga", "aros-rom.bin", kick, 0x80000, 0x80000);
        if (kick_size)
            ext_size = rom_find(path, "amiga", "aros-ext.bin", ext, 0x80000, 0x80000);
        if (!kick_size || !ext_size)
            return "no kick.rom (a Kickstart, 256 or 512 KB) and no aros-rom.bin with aros-ext.bin (AROS's free "
                   "one) beside it, in amiga/ or at the top of its disk";
    }
    for (int i = 0; i < 16; i++)
        amiga_keys[i] = 0;
    return amiga_load(kick, kick_size, ext_size ? ext : 0, ext_size, size ? image : 0, size, path);
}

static void amiga_one(u8 pad, void *picture)
{
    amiga_frame(pad, amiga_keys, (u16 *)picture);
}

static void amiga_keyboard(const u8 *usages, int n)
{
    amiga_keys_from_hid(usages, n, amiga_keys);
}

static void amiga_typed(char c)
{
    char s[2] = {c, 0};
    amiga_type(s);
}

static const struct machine amiga_machine = {AMIGA_W, AMIGA_H, 2, 2, AMIGA_FRAME_US, amiga_open, amiga_info,
                                             amiga_one, amiga_save_ram, amiga_save_changed, amiga_jammed, 0, 0,
                                             "the 68000", amiga_keyboard, amiga_typed,
                                             "the USB keyboard is the Amiga's, its arrows the joystick (fire: right "
                                             "Ctrl, the remote's OK); the mouse button: Page Up / Page Down, the "
                                             "remote's 0; the remote's MENU = Return, INFO = Space; what is typed "
                                             "on this console is typed on it; Esc ends", 0, 0, 0};

int amiga_play(const char *path, u32 frames, const char *script)
{
    return play(&amiga_machine, path, frames, script);
}

/* a DOS PC (an AT with a VGA): the 320 x 200 picture three times as large (960 x 600).  One program (.exe or
   .com) runs, with no data files beside it (Magicland Dizzy needs none) */
static u8 dos_keys[32];

static const char *dos_open(u8 *image, u32 size, const char *path)
{
    for (int i = 0; i < 32; i++)
        dos_keys[i] = 0;
    dos_clear_files();
    dos_args("");
    return dos_load(image, size, path);
}

static void dos_one(u8 pad, void *picture)
{
    dos_frame(pad, dos_keys, (u16 *)picture);
}

static void dos_keyboard(const u8 *usages, int n)
{
    dos_keys_from_hid(usages, n, dos_keys);
}

static void dos_typed(char c)
{
    char s[2] = {c, 0};
    dos_type(s);
}

static const struct machine dos_machine = {DOS_W, DOS_H, 3, 3, DOS_FRAME_US, dos_open, dos_info, dos_one,
                                           dos_save_ram, dos_save_changed, dos_jammed, 0, 0, "the 286",
                                           dos_keyboard, dos_typed,
                                           "the USB keyboard is the PC's (but Esc, which ends); the remote: arrows, "
                                           "OK = Up, 0 = Space, MENU = Enter, INFO = Esc; what is typed on this "
                                           "console is typed on it", 0, 0, 0};

int dos_play(const char *path, u32 frames, const char *script)
{
    return play(&dos_machine, path, frames, script);
}

int zx_play(const char *path, u32 frames, const char *script)
{
    return play(&zx_machine, path, frames, script);
}
