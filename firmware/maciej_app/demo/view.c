/* The picture viewer ('view <file>', or Enter on a picture in the explorer):
   the file read whole into RAM, decoded (image.c) a row at a time straight
   into a picture of the size it is shown at -- made smaller by averaging
   the pixels that fall together, made larger only by whole times (a small
   picture's pixels stay sharp) -- and put in the middle of the test
   screen's picture, turned as a phone photo's EXIF orientation says.

   Keys: Right / Down / PgDn / Space the next picture in the same directory,
   Left / Up / PgUp the one before (the remote: its arrows, PAGE+ / PAGE-),
   i its details again; Esc, q, Ctrl-C or the remote's EXIT end. */
#include "hw.h"
#include "image.h"

#define FIT_MAX (SCREEN_W * SCREEN_H)

struct fit {
    u32 w, h;                           /* the file's picture */
    u32 fw, fh;                         /* the picture made (before turning) */
    u16 *out;                           /* fw x fh */
    u32 *acc, *cols;                    /* a made row's sums; how many columns of the file fall in each */
    u16 *xmap;                          /* the file's column -> the made one */
    u32 oy, rows;                       /* the made row being summed; the file's rows in it so far */
};

static int view_mode;                   /* 0 the video engine first for a baseline JPEG, 1 image.c's decoder only
                                           ('view -s'), 2 both compared ('view -c') */

void view_set_mode(int mode)
{
    view_mode = mode;
}

/* the fit made ready for a picture of w x h coming a row at a time (its out, acc, cols and xmap in place) */
static void fit_start(struct fit *f, u32 w, u32 h)
{
    f->w = w, f->h = h;
    for (u32 x = 0; x < f->fw; x++)
        f->cols[x] = 0;
    for (u32 x = 0; x < f->fw * 3; x++)
        f->acc[x] = 0;
    for (u32 x = 0; x < w; x++) {
        f->xmap[x] = (u16)(x * f->fw / w);
        f->cols[f->xmap[x]]++;
    }
    for (u32 i = 0; i < f->fw * f->fh; i++)
        f->out[i] = RGB(0, 0, 0);
    f->oy = 0, f->rows = 0;
}

/* 2^24 / n rounded up, n 2..256: an average of n bytes' sum as a multiply and a shift -- the same as the
   division's (exact while 255 n (n - 1) < 2^24) -- instead of three divisions a pixel, a third of a 1080p
   picture's fitting time */
static u32 recip[257];

static void fit_flush(struct fit *f)
{
    if (!f->rows || f->oy >= f->fh)
        return;
    if (!recip[2])
        for (u32 n = 2; n <= 256; n++)
            recip[n] = ((1u << 24) + n - 1) / n;
    u16 *o = f->out + f->oy * f->fw;
    u32 *acc = f->acc, rows = f->rows, fw = f->fw;
    const u32 *cols = f->cols;
    for (u32 x = 0; x < fw; x++, acc += 3) {
        u32 n = cols[x] * rows, r = acc[0], g = acc[1], b = acc[2];
        if (n > 256) {
            r /= n, g /= n, b /= n;
        } else if (n > 1) {
            u32 m = recip[n];
            r = r * m >> 24, g = g * m >> 24, b = b * m >> 24;
        }
        o[x] = RGB(r, g, b);
        acc[0] = acc[1] = acc[2] = 0;
    }
    f->rows = 0;
}

static void fit_row(void *ctx, u32 y, const u8 *rgb)
{
    struct fit *f = ctx;
    u32 oy = y * f->fh / f->h;
    if (oy != f->oy) {
        fit_flush(f);
        f->oy = oy;
    }
    u32 *acc = f->acc, w = f->w;        /* (in locals: the stores to acc could be to f, as far as the compiler
                                           knows, and f's fields were read again every pixel) */
    const u16 *xmap = f->xmap;
    for (u32 x = 0; x < w; x++, rgb += 3) {
        u32 *a = acc + xmap[x] * 3;
        a[0] += rgb[0];
        a[1] += rgb[1];
        a[2] += rgb[2];
    }
    f->rows++;
    if ((y & 63) == 0)
        console_service();              /* (a big photo takes seconds: the TV console's text keeps up) */
}

static int ends_with(const char *s, const char *tail)
{
    int n = 0, m = 0;
    while (s[n])
        n++;
    while (tail[m])
        m++;
    if (m > n)
        return 0;
    for (int i = 0; i < m; i++) {
        char c = s[n - m + i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c + 32);
        if (c != tail[i])
            return 0;
    }
    return 1;
}

int view_is_picture(const char *name)
{
    return ends_with(name, ".jpg") || ends_with(name, ".jpeg") || ends_with(name, ".png") || ends_with(name, ".gif")
        || ends_with(name, ".bmp") || ends_with(name, ".jpe") || ends_with(name, ".jfif");
}

/* while a picture is decoded (image.c asks now and then): the clock read -- which feeds the watchdog --, the
   TV console kept up, and Ctrl-C or Esc (UART, telnet, USB keyboard) stop it */
static int stop_keyed;                  /* the decode was stopped by a key (not by what the file holds) */

static int stop_wanted(void)
{
    static u64 last;
    u64 now = now_ms();
    if (now - last < 50)
        return 0;
    last = now;
    console_service();
    int c;
    while ((c = uart_getc()) >= 0)
        if (c == 3 || c == 27)
            return stop_keyed = 1;
    usb_int_poll();
    (void)kbd_poll();
    while ((c = kbd_getc()) >= 0)
        if (c == 3)
            return stop_keyed = 1;
    return 0;
}

/* ---- what the TV says while a file is opened ---- */
static const char *base_name(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':')
            b = p + 1;
    return *b ? b : path;
}

/* `s` at the largest scale up to `max` that fits the screen's width (less a margin): its x, and the scale */
static int fit_text(const char *s, int max, int *scale)
{
    int k = max, w;
    while ((w = gfx_text_width(s, k)) > SCREEN_W - 80 && k > 1)
        k--;
    *scale = k;
    return w < SCREEN_W ? (SCREEN_W - w) / 2 : 0;
}

#define BAR_H 40

/* `second`: 0 -- "Opening NAME ..." -- or the reason under (in the strip: after) "Could not open NAME" */
static void say(const char *path, const char *first, const char *second, u16 colour, int bar)
{
    static char line[200];
    usnprintf(line, sizeof(line), "%s%s%s", first, base_name(path), second ? "" : " ...");
    gfx_target(0);
    int k;
    if (bar) {                          /* (over the picture shown now: a strip at the top) */
        if (second && *second)
            usnprintf(line, sizeof(line), "%s%s: %s", first, base_name(path), second);
        gfx_fill(0, 0, SCREEN_W, BAR_H, RGB(0, 0, 0));
        int x = fit_text(line, 3, &k);
        (void)x;
        gfx_text(20, (BAR_H - 8 * k) / 2, line, k, colour);
    } else {
        gfx_fill(0, 0, SCREEN_W, SCREEN_H, RGB(0, 0, 0));
        int x = fit_text(line, 5, &k);
        gfx_text(x, SCREEN_H / 2 - 40, line, k, colour);
        if (second && *second) {
            x = fit_text(second, 3, &k);
            gfx_text(x, SCREEN_H / 2 + 20, second, k, RGB(220, 220, 220));
        }
    }
    gfx_view(0);
}

void tv_opening(const char *path, int bar)
{
    say(path, "Opening ", 0, RGB(255, 255, 255), bar);
}

void tv_failed(const char *path, int bar, int wait)
{
    const char *why = console_last_line();
    say(path, "Could not open ", *why ? why : "(the console says why)", RGB(255, 96, 96), bar);
    if (!wait)
        return;
    u64 start = now_ms();               /* (long enough to read it; a key ends it sooner) */
    while (now_ms() - start < 4000) {
        usb_int_poll();
        (void)kbd_poll();
        if (kbd_getc() >= 0 || uart_getc() >= 0) {
            delay_ms(30);               /* (an arrow key is three bytes: the rest is not for the command line) */
            usb_int_poll();
            (void)kbd_poll();
            while (kbd_getc() >= 0 || uart_getc() >= 0)
                ;
            break;
        }
        console_service();
    }
}

/* one picture on the TV: 0 shown, -1 not (said why) */
static int show_file(const char *path, int quiet)
{
    u8 *data = (u8 *)KSEG0(MEDIA_PHYS);
    u32 size = 0;
    u64 t0 = now_ms();
    int got = disk_read_head(path, data, MEDIA_MAX, &size);
    if (got < 0)
        return -1;
    if (size > MEDIA_MAX) {
        uprintf("%s: %u bytes -- larger than the %u MB a picture file can be here\n", path, size, MEDIA_MAX >> 20);
        return -1;
    }
    struct image_info in;
    const char *why = image_info(data, (u32)got, &in);
    if (why) {
        uprintf("%s: %s\n", path, why);
        return -1;
    }
    if (!in.width || !in.height) {
        uprintf("%s: a picture of no pixels\n", path);
        return -1;
    }
    if ((u64)in.width * in.height > 100000000u) {      /* (a decode of hours, a damaged size) */
        uprintf("%s: %ux%u: larger than 100 megapixels\n", path, in.width, in.height);
        return -1;
    }
    if (in.width > 65535 || in.height > 65535) {        /* (the sizes' arithmetic below is 32-bit) */
        uprintf("%s: %ux%u: too large\n", path, in.width, in.height);
        return -1;
    }
    u64 t1 = now_ms();
    /* the size it is shown at: the screen's, keeping its shape; turned when the photo says so */
    int turned = in.orientation >= 5;
    u32 dw = turned ? in.height : in.width, dh = turned ? in.width : in.height, k = 1;
    u32 sw, sh;
    if (dw <= SCREEN_W && dh <= SCREEN_H) {
        k = SCREEN_W / dw < SCREEN_H / dh ? SCREEN_W / dw : SCREEN_H / dh;
        sw = dw, sh = dh;
    } else if (dw * SCREEN_H > dh * SCREEN_W) {
        sw = SCREEN_W, sh = dh * SCREEN_W / dw;
    } else {
        sh = SCREEN_H, sw = dw * SCREEN_H / dh;
    }
    if (!sw)
        sw = 1;
    if (!sh)
        sh = 1;
    static struct fit f;
    f.w = in.width, f.h = in.height;
    f.fw = turned ? sh : sw, f.fh = turned ? sw : sh;
    u8 *work = (u8 *)KSEG0(IMAGE_WORK_PHYS);
    u32 left = IMAGE_WORK_SIZE;
    f.out = (u16 *)work;
    f.acc = (u32 *)(work + FIT_MAX * 2);
    f.cols = f.acc + SCREEN_W * 3;
    f.xmap = (u16 *)(f.cols + SCREEN_W);
    u32 used = FIT_MAX * 2 + SCREEN_W * 16 + ((in.width * 2 + 7) & ~7u);
    if (in.width > 0x400000 || used > left) {
        uprintf("%s: %ux%u: too wide\n", path, in.width, in.height);
        return -1;
    }
    stop_keyed = 0;
    image_poll = stop_wanted;
    /* a baseline JPEG: the video engine's JPEG mode first (jpeghw.c), pre-scaling it to no less than it is
       shown; image.c's decoder for anything else, and for a JPEG the engine does not take or did not finish */
    char how[80];
    how[0] = 0;
    u32 hw_w, hw_h, hw_ms = 0;
    why = "";
    if (view_mode != 1 && in.kind[0] == 'J' && !jpeghw_plan(data, (u32)got, MEDIA_PHYS, f.fw, f.fh, &hw_w, &hw_h)) {
        fit_start(&f, hw_w, hw_h);
        why = jpeghw_run(IMAGE_WORK_PHYS + used, left - used, fit_row, &f, &hw_ms);
        if (!why)
            usnprintf(how, sizeof(how), " by the video engine (%s: %u ms, %ux%u made)", jpeghw_detail(), hw_ms,
                      hw_w, hw_h);
        else if (!stop_keyed)
            usnprintf(how, sizeof(how), " by the CPU (the video engine: %s)", why);
    }
    if (why && !stop_keyed) {
        fit_start(&f, in.width, in.height);
        why = image_decode(data, (u32)got, work + used, left - used, fit_row, &f);
    }
    image_poll = 0;
    if (why && !stop_keyed && !f.oy && !f.rows) {       /* (not one row of it came: the TV says why, as for a file
                                                           that is no picture -- not a black screen) */
        uprintf("%s: %s %ux%u, %s: %s\n", path, in.kind, in.width, in.height, in.detail, why);
        return -1;
    }
    fit_flush(&f);
    u64 t2 = now_ms();
    /* onto the test screen's picture, in the middle, turned (EXIF: 2..8), each pixel k x k */
    volatile u16 *fb = (volatile u16 *)KSEG1(FB_PHYS);
    for (u32 i = 0; i < (u32)SCREEN_W * SCREEN_H / 2; i++)
        ((volatile u32 *)fb)[i] = (u32)RGB(0, 0, 0) * 0x10001u;
    u32 ow = sw * k, oh = sh * k, x0 = (SCREEN_W - ow) / 2, y0 = (SCREEN_H - oh) / 2;
    u8 o = in.orientation;
    for (u32 dy = 0; dy < sh; dy++) {
        for (u32 dx = 0; dx < sw; dx++) {
            u32 sx, sy;
            switch (o) {
            case 2: sx = f.fw - 1 - dx, sy = dy; break;
            case 3: sx = f.fw - 1 - dx, sy = f.fh - 1 - dy; break;
            case 4: sx = dx, sy = f.fh - 1 - dy; break;
            case 5: sx = dy, sy = dx; break;
            case 6: sx = dy, sy = f.fh - 1 - dx; break;
            case 7: sx = f.fw - 1 - dy, sy = f.fh - 1 - dx; break;
            case 8: sx = f.fw - 1 - dy, sy = dx; break;
            default: sx = dx, sy = dy; break;
            }
            u16 c = f.out[sy * f.fw + sx];
            for (u32 r = 0; r < k; r++)
                for (u32 q = 0; q < k; q++)
                    fb[(y0 + dy * k + r) * SCREEN_W + x0 + dx * k + q] = c;
        }
        if (!(dy & 31))
            console_service();
    }
    gfx_view(0);
    u64 t3 = now_ms();
    if (!quiet || why)
        uprintf("%s: %s %ux%u, %s%s; shown %ux%u%s (read %u ms, decoded %u ms%s, drawn %u ms)%s%s\n", path,
                in.kind, in.width, in.height, in.detail, turned || o > 1 ? ", turned as its EXIF says" : "", ow, oh,
                k > 1 ? " (whole times larger)" : "", (u32)(t1 - t0), (u32)(t2 - t1), how, (u32)(t3 - t2),
                why ? " -- stopped: " : "", why ? why : "");
    return 0;
}

/* 'view -c': a JPEG decoded by image.c's decoder (its rows kept at WGET_PHYS, as many as 8 MB holds) and by the
   video engine at full size, compared value by value; nothing shown */
static u8 *cmp_rgb;
static u32 cmp_w, cmp_rows, cmp_max, cmp_off, cmp_x, cmp_y, cmp_seen;
static u32 cmp_sum;                     /* (8 MB of values at most 255 off: under 2^31) */

static void cmp_store(void *ctx, u32 y, const u8 *rgb)
{
    (void)ctx;
    if (y < cmp_rows)
        for (u32 i = 0; i < cmp_w * 3; i++)
            cmp_rgb[y * cmp_w * 3 + i] = rgb[i];
    if ((y & 63) == 0)
        console_service();
}

static void cmp_check(void *ctx, u32 y, const u8 *rgb)
{
    (void)ctx;
    if (y >= cmp_rows)
        return;
    const u8 *s = cmp_rgb + y * cmp_w * 3;
    for (u32 i = 0; i < cmp_w * 3; i++) {
        u32 d = rgb[i] > s[i] ? rgb[i] - s[i] : s[i] - rgb[i];
        cmp_sum += d;
        if (d > cmp_max)
            cmp_max = d;
        if (d > 8 && !cmp_off++)
            cmp_x = i / 3, cmp_y = y;
    }
    cmp_seen++;
}

static int compare_file(const char *path)
{
    u8 *data = (u8 *)KSEG0(MEDIA_PHYS);
    u32 size = 0;
    int got = disk_read_head(path, data, MEDIA_MAX, &size);
    if (got < 0)
        return -1;
    struct image_info in;
    const char *why = image_info(data, (u32)got, &in);
    u32 w, h, hw_ms = 0;
    if (!why)
        why = in.kind[0] != 'J' ? "not a JPEG" : jpeghw_plan(data, (u32)got, MEDIA_PHYS, in.width, in.height, &w, &h);
    if (why) {
        uprintf("%s: the video engine does not take it: %s\n", path, why);
        return -1;
    }
    cmp_rgb = (u8 *)KSEG0(WGET_PHYS);
    cmp_w = w;
    cmp_rows = WGET_MAX / (w * 3) < h ? WGET_MAX / (w * 3) : h;
    stop_keyed = 0;
    image_poll = stop_wanted;
    u64 t0 = now_ms();
    why = image_decode(data, (u32)got, (u8 *)KSEG0(IMAGE_WORK_PHYS), IMAGE_WORK_SIZE, cmp_store, 0);
    u64 t1 = now_ms();
    if (!why) {
        (void)jpeghw_plan(data, (u32)got, MEDIA_PHYS, in.width, in.height, &w, &h);
        cmp_max = cmp_off = cmp_seen = 0, cmp_sum = 0;
        why = jpeghw_run(IMAGE_WORK_PHYS, IMAGE_WORK_SIZE, cmp_check, 0, &hw_ms);
    }
    u64 t2 = now_ms();
    image_poll = 0;
    if (why) {
        uprintf("%s: %s\n", path, why);
        return -1;
    }
    u32 n = cmp_seen * cmp_w * 3;
    u32 mean = n ? cmp_sum / n * 100 + cmp_sum % n * 100 / n : 0;
    uprintf("%s: JPEG %ux%u, %s: the CPU %u ms, the video engine %u ms (%u ms its decoding, the rest to RGB); %u of "
            "%u rows compared: largest difference %u, mean %u.%02u, %u values off by more than 8", path, in.width,
            in.height, jpeghw_detail(), (u32)(t1 - t0), (u32)(t2 - t1), hw_ms, cmp_seen, h, cmp_max, mean / 100,
            mean % 100, cmp_off);
    if (cmp_off)
        uprintf(" (the first at %u, %u)", cmp_x, cmp_y);
    uprintf("\n");
    return 0;
}

/* the directory part of a path ("4:/a/b.jpg" -> "4:/a"), "" for a bare name */
static void dir_of(const char *path, char *dir, int size)
{
    int cut = -1;
    for (int i = 0; path[i]; i++)
        if (path[i] == '/' || path[i] == '\\' || path[i] == ':')
            cut = i;
    int n = 0;
    if (cut >= 0) {
        n = path[cut] == ':' || cut == 0 || path[cut - 1] == ':' ? cut + 1 : cut;
        if (n >= size)
            n = size - 1;
        for (int i = 0; i < n; i++)
            dir[i] = path[i];
    }
    dir[n] = 0;
}

static const char *base_of(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ':')
            b = p + 1;
    return b;
}

static int same_name(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z')
            x = (char)(x + 32);
        if (y >= 'A' && y <= 'Z')
            y = (char)(y + 32);
        if (x != y)
            return 0;
    }
    return *a == *b;
}

/* the next key: 1 next, -1 before, 2 details, 3 end, 0 none */
static int key(void)
{
    static u8 esc;
    static int num;
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
            if (c == 'C' || c == 'B' || (c == '~' && num == 6))
                return 1;
            if (c == 'D' || c == 'A' || (c == '~' && num == 5))
                return -1;
            continue;
        }
        if (esc == 1) {
            esc = c == '[' ? 2 : 0;
            num = 0;
            if (!esc)
                return 3;               /* (a lone ESC, then something else) */
            continue;
        }
        if (c == 27) {
            esc = 1;
            continue;
        }
        if (c == 3 || c == 'q')
            return 3;
        if (c == ' ' || c == 'n')
            return 1;
        if (c == 'p' || c == 8 || c == 127)
            return -1;
        if (c == 'i')
            return 2;
    }
    static u64 since;                   /* when the ESC waited for was first seen waiting (0: none is) */
    if (esc != 1) {
        since = 0;                      /* (it began an arrow's sequence, or there is none: the next ESC's
                                           150 ms start afresh -- kept, the second arrow ended the viewer) */
    } else {                            /* ESC alone (the USB keyboard's Esc comes as Ctrl-C) */
        if (!since)
            since = now_ms();
        if (now_ms() - since > 150) {
            since = 0;
            esc = 0;
            return 3;
        }
    }
    struct ir_event ev;
    while (ir_poll(&ev)) {
        if (!ev.valid || ev.repeat)
            continue;
        const char *n = ir_key_name(ev.code);
        if (!n)
            continue;
        if (same_name(n, "RIGHT") || same_name(n, "DOWN") || same_name(n, "PAGE+") || same_name(n, "NEXT"))
            return 1;
        if (same_name(n, "LEFT") || same_name(n, "UP") || same_name(n, "PAGE-") || same_name(n, "PREV"))
            return -1;
        if (same_name(n, "INFO"))
            return 2;
        if (same_name(n, "EXIT") || same_name(n, "MENU"))
            return 3;
    }
    return 0;
}

/* the picture before or after `name` in its directory (wrapping round): into path, 1; 0 there is none */
static int neighbour(char *path, int size, int step)
{
    char dir[128];
    dir_of(path, dir, sizeof(dir));
    struct disk_entry *list = (struct disk_entry *)KSEG0(VIEW_LIST_PHYS);
    u32 total;
    int n = disk_dir_read(dir[0] ? dir : ".", list, (int)(VIEW_LIST_SIZE / sizeof(*list)), &total);
    if (n <= 0)
        return 0;
    const char *me = base_of(path);
    int at = -1;
    for (int i = 0; i < n; i++)
        if (list[i].kind == DISK_FILE && same_name(list[i].name, me))
            at = i;
    for (int tries = 0, i = at; tries < n; tries++) {
        i = (i + step + n) % n;
        if (list[i].kind == DISK_FILE && view_is_picture(list[i].name) && i != at) {
            int d = 0;
            while (dir[d])
                d++;
            int need = d + 1;
            for (const char *q = list[i].name; *q; q++)
                need++;
            if (need >= size)
                return 0;
            usnprintf(path, size, "%s%s%s", dir, d && dir[d - 1] != '/' && dir[d - 1] != ':' ? "/" : "",
                      list[i].name);
            return 1;
        }
    }
    return 0;
}

/* 'view <file>': the picture, then the others of its directory, until Esc.  `once`: show it and return
   (a test) */
static char cur[160];                   /* the picture shown last */

const char *view_last(void)
{
    return cur;
}

/* the same with the TV told at once: "Opening NAME ..." (bar: over the picture shown now), and why it failed */
static int show(const char *path, int bar, int wait)
{
    tv_opening(path, bar);
    int r = show_file(path, 0);
    if (r < 0)
        tv_failed(path, bar, wait);
    return r;
}

void view_file(const char *path, int once)
{
    if (view_mode == 2) {
        (void)compare_file(path);
        return;
    }
    usnprintf(cur, sizeof(cur), "%s", path);
    if (show(cur, 0, !once) < 0)
        return;
    if (once)
        return;
    uprintf("view: Right / Space the next picture, Left the one before, i its details, Esc or q ends\n");
    for (;;) {
        int k = key();
        if (k == 3)
            break;
        if (k == 2)
            (void)show(cur, 1, 0);
        if (k == 1 || k == -1) {
            if (!neighbour(cur, sizeof(cur), k))
                uprintf("view: no other picture in that directory\n");
            else
                (void)show(cur, 1, 0);
        }
        console_service();
        if (eth_ready())
            eth_service();
    }
}
