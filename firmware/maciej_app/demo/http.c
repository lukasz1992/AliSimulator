/* A web server on port 80: the box's page (web.html, built in by web.S) for
   any browser on the network -- the disks' files (listed, downloaded,
   uploaded, deleted, renamed, new directories), the TV's picture, the
   command line -- and the small HTTP API it uses:

     GET  /                         the page
     GET  /api/info                 {"banner","version","ip","uptime","command"} (command: one runs now)
     GET  /api/ls?p=4:/DIR          a directory as JSON ({"path","entries":[{"n","d","s","t"}],"cut"});
                                    p empty: the volumes ({"n":"4:","l":"PHONE CARD","i":"FAT32 ..."})
     GET  /api/get?p=4:/a.jpg       the file (&inline=1: shown by the browser, not saved)
     PUT  /api/put?p=4:/DIR/a.jpg   the request's body written as that file (made, or emptied first)
     POST /api/rm?p=, /api/rmdir?p=, /api/mkdir?p=, /api/mv?p=&to=     as the commands of those names
     GET  /api/screen.bmp[?half=1]  what the TV shows now (the test screen or the console), a BMP of 1280 x
                                    720 -- or 640 x 360 -- 16-bit pixels
     GET  /api/out?from=N           what was printed since byte N (the X-Next header: where the next one
                                    starts); waits up to 15 s for something new (a long poll)
     POST /api/in                   the body's bytes typed, as a telnet client's are (uart.c's injection
                                    queue: the command line, a game's buttons, Ctrl-C)

   HTTP/1.1 with keep-alive (the page asks often: a connection a request
   would leave tcp.c's few connections in TIME_WAIT), six browser
   connections at a time, no chunked bodies (a PUT needs Content-Length).
   No password: anyone on the network gets all of it, as with FTP.

   Where it runs: the main loop's net_poll, and -- while a command runs (a
   game, a long copy) -- net_uart_poll, as the telnet console's input does.
   Inside a command the disks are the command's (disk.c's buffers): the
   requests that read or write them get 503 then, and a download or upload
   under way waits (TCP's window holds the client) until the command ends.
   One file moves at a time between this and the FTP server (disk.c has one
   reader and one writer for them): the other gets 503 (the page says so)
   or FTP's 450. */
#include "hw.h"

#define HTTP_PORT   80
#define CLIENTS     6                   /* (a browser opens six connections to a host) */
#define HEAD_MAX    2048
#define CHUNK       4096
#define READS       8                   /* pieces read and sent each pass */
#define WRITES      4                   /* pieces received and written each pass */
#define IDLE_MS     30000               /* a kept-alive connection that asks nothing for this long is closed */
#define STALL_MS    60000               /* a transfer that moved nothing for this long ends */
#define WAIT_OUT_MS 15000               /* /api/out's long poll */
#define RING        65536u              /* what was printed, kept for /api/out */
#define FIRST_OUT   8192u               /* what a first /api/out (no from) gets of it */
#define ENTRIES     ((int)(HTTP_ENTRIES_SIZE / sizeof(struct disk_entry)))
#define PATH_MAX_   340

extern const char web_page[], web_page_end[];          /* web.S: web.html */

enum { S_FREE, S_HEAD, S_BODY, S_SEND };
enum { B_NONE, B_UPLOAD, B_INPUT, B_DISCARD };          /* what a request's body is for */
enum { O_NONE, O_MEM, O_FILE, O_SCREEN, O_WAIT_OUT };   /* what a response's body comes from */

struct client {
    int state;
    int c;
    u8 ip[4];
    char head[HEAD_MAX + 1];
    int head_len, body_at;              /* the request's bytes so far; where its body starts in head[] */
    int keep;                           /* keep-alive after this response */
    u64 heard, moved_at;
    u64 body_left;
    int body;
    int out;
    char hdr[600];                      /* the response's header (a BMP's own header after it) */
    u32 hdr_len, hdr_pos;
    const u8 *mem;                      /* O_MEM: the body */
    u32 mem_len, mem_pos;
    u64 left;                           /* O_FILE: bytes still to send */
    int row, half;                      /* O_SCREEN: the next row; 640 x 360 */
    const volatile u16 *fb;
    u32 from;                           /* O_WAIT_OUT: /api/out's from */
    u64 since;
    char path[PATH_MAX_];               /* a download's or an upload's */
    int ok;                             /* an upload: every piece written */
    u8 stage[CHUNK];                    /* an upload's next 4 KB, gathered: written whole (the bytes that came
                                           with the header would put every piece after them across sectors) */
    u32 staged;
};

static struct client clients[CLIENTS];
static int started, polling;
static struct client *disk_owner;       /* the client whose download or upload has disk.c's reader / writer */
static u8 chunk[CHUNK];
static char said[200];
static char line[200];
static u32 got_files, sent_files, requests;
static struct disk_entry *const ents = (struct disk_entry *)KSEG0(HTTP_ENTRIES_PHYS);

/* what was printed (uart_putc) */
static u8 ring[RING];
static u32 out_total;

void http_putc(char c)
{
    ring[out_total++ % RING] = (u8)c;
}

/* ---- small helpers ---- */
static int slen(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static int starts(const char *s, const char *p)
{
    while (*p)
        if (*s++ != *p++)
            return 0;
    return 1;
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int starts_nocase(const char *s, const char *p)
{
    while (*p)
        if (lower(*s++) != lower(*p++))
            return 0;
    return 1;
}

static void scopy(char *to, const char *from, int size)
{
    int i = 0;
    for (; from[i] && i < size - 1; i++)
        to[i] = from[i];
    to[i] = 0;
}

static char *u64_text(char *buf, u64 v)                /* (no 64-bit division in the demo: ftp.c's way) */
{
    char t[24];
    int n = 0;
    do {
        u64 q = (v >> 1) + (v >> 2);
        q += q >> 4;
        q += q >> 8;
        q += q >> 16;
        q += q >> 32;
        q >>= 3;
        u32 r = (u32)(v - q * 10);
        if (r > 9) {
            q++;
            r -= 10;
        }
        t[n++] = (char)('0' + r);
        v = q;
    } while (v && n < 22);
    for (int i = 0; i < n; i++)
        buf[i] = t[n - 1 - i];
    buf[n] = 0;
    return buf;
}

static int hexval(int c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                                                                     : -1;
}

/* the query's parameter `name`, %-decoded ('+' a space), into out: 1 there, 0 not (out ""), -1 too long */
static int param(const char *query, const char *name, char *out, int size)
{
    out[0] = 0;
    int nl = slen(name);
    const char *p = query;
    while (p && *p) {
        if (starts(p, name) && p[nl] == '=') {
            p += nl + 1;
            int n = 0;
            while (*p && *p != '&') {
                int c = *p++;
                if (c == '+')
                    c = ' ';
                else if (c == '%' && hexval(p[0]) >= 0 && hexval(p[1]) >= 0) {
                    c = hexval(p[0]) << 4 | hexval(p[1]);
                    p += 2;
                }
                if (n >= size - 1)
                    return -1;
                out[n++] = (char)c;
            }
            out[n] = 0;
            return 1;
        }
        while (*p && *p != '&')
            p++;
        if (*p)
            p++;
    }
    return 0;
}

static u32 number(const char *s)
{
    u32 v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (u32)(*s++ - '0');
    return v;
}

/* what disk calls print goes into `said`: 0 nothing (it worked), else its first line */
static void cap_begin(void)
{
    uart_capture((u8 *)said, sizeof(said) - 1);
}

static const char *cap_end(void)
{
    u32 over;
    u32 n = uart_capture_end(&over);
    said[n] = 0;
    int i = 0;
    while (said[i] && said[i] != '\n')
        i++;
    said[i] = 0;
    return said[0] ? said : 0;
}

static char *text_of(const struct client *cl)          /* the client's own 1 MB for a response's body */
{
    return (char *)KSEG0(HTTP_TEXT_PHYS + (u32)(cl - clients) * HTTP_TEXT_SIZE);
}

/* a JSON string: `s` quoted and escaped, at b (room bytes): its length */
static int json_str(char *b, int room, const char *s)
{
    int n = 0;
    if (room < 3)
        return room;
    b[n++] = '"';
    for (; *s && n < room - 8; s++) {
        u8 c = (u8)*s;
        if (c == '"' || c == '\\') {
            b[n++] = '\\';
            b[n++] = (char)c;
        } else if (c < 0x20 || c >= 0x7F) {
            n += usnprintf(b + n, room - n, "\\u%04x", c);
        } else {
            b[n++] = (char)c;
        }
    }
    b[n++] = '"';
    return n;
}

/* ---- responses ---- */
static const char *reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    }
    return "Error";
}

/* the header of a response with `len` bytes of body (`extra`: more header lines, each ending in CRLF) */
static void header(struct client *cl, int status, const char *type, u64 len, const char *extra)
{
    char t[24];
    cl->hdr_len = (u32)usnprintf(cl->hdr, sizeof(cl->hdr),
                                 "HTTP/1.1 %u %s\r\nContent-Type: %s\r\nContent-Length: %s\r\nCache-Control: no-store\r\n"
                                 "Connection: %s\r\n%s\r\n", status, reason(status), type, u64_text(t, len),
                                 cl->keep ? "keep-alive" : "close", extra ? extra : "");
    if (cl->hdr_len >= sizeof(cl->hdr))
        cl->hdr_len = sizeof(cl->hdr) - 1;
    cl->hdr_pos = 0;
    cl->state = S_SEND;
}

static void send_mem(struct client *cl, int status, const char *type, const void *body, u32 len, const char *extra)
{
    header(cl, status, type, len, extra);
    cl->out = O_MEM;
    cl->mem = (const u8 *)body;
    cl->mem_len = len;
    cl->mem_pos = 0;
}

static void send_json(struct client *cl, int status, const char *json)
{
    char *t = text_of(cl);
    scopy(t, json, HTTP_TEXT_SIZE);
    send_mem(cl, status, "application/json", t, (u32)slen(t), 0);
}

/* {"error": why} with that status */
static void fail(struct client *cl, int status, const char *why)
{
    char *t = text_of(cl);
    int n = usnprintf(t, 64, "{\"error\":");
    n += json_str(t + n, 400, why);
    t[n++] = '}';
    t[n] = 0;
    send_mem(cl, status, "application/json", t, (u32)n, 0);
}

static void done_ok(struct client *cl)
{
    send_json(cl, 200, "{\"ok\":1}");
}

/* ---- the requests ---- */
static int ends_with(const char *s, const char *ext)
{
    int n = slen(s), e = slen(ext);
    if (n < e)
        return 0;
    for (int i = 0; i < e; i++)
        if (lower(s[n - e + i]) != ext[i])
            return 0;
    return 1;
}

static const char *type_of(const char *path)
{
    static const struct {
        const char *ext, *type;
    } types[] = {{".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".png", "image/png"}, {".gif", "image/gif"},
                 {".bmp", "image/bmp"}, {".txt", "text/plain; charset=utf-8"}, {".log", "text/plain; charset=utf-8"},
                 {".md", "text/plain; charset=utf-8"}, {".c", "text/plain; charset=utf-8"},
                 {".h", "text/plain; charset=utf-8"}, {".py", "text/plain; charset=utf-8"},
                 {".ini", "text/plain; charset=utf-8"}, {".csv", "text/plain; charset=utf-8"},
                 {".json", "application/json"}, {".htm", "text/html"}, {".html", "text/html"},
                 {".pdf", "application/pdf"}, {".mp3", "audio/mpeg"}, {".mp4", "video/mp4"}};
    for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        if (ends_with(path, types[i].ext))
            return types[i].type;
    return "application/octet-stream";
}

/* the disks may be used now: a command does not run, no other file moves.  Else 503, and 0 */
static int disk_free(struct client *cl, int in_cmd, int moving)
{
    if (in_cmd) {
        fail(cl, 503, "A command is running on the box: its disks are its own until it ends");
        return 0;
    }
    if (moving && (disk_owner || ftp_disk_busy())) {
        fail(cl, 503, "The box moves one file at a time (another download or upload, the page's or FTP's): "
                      "try again in a moment");
        return 0;
    }
    return 1;
}

static void list(struct client *cl, const char *path)
{
    char *t = text_of(cl);
    u32 room = HTTP_TEXT_SIZE - 1024, total = 0;      /* (an entry takes 600 bytes at most) */
    int n, len = 0, cut = 0;
    if (!path[0]) {
        n = disk_volumes(ents, ENTRIES);
        total = (u32)n;
    } else {
        cap_begin();
        n = disk_dir_read(path, ents, ENTRIES, &total);
        const char *why = cap_end();
        if (n < 0) {
            fail(cl, 404, why ? why : "Not read");
            return;
        }
    }
    len = usnprintf(t, 64, "{\"path\":");
    len += json_str(t + len, 400, path);
    len += usnprintf(t + len, 64, ",\"entries\":[");
    int first = 1;
    for (int i = 0; i < n; i++) {
        const struct disk_entry *e = &ents[i];
        if ((u32)len > room) {
            cut = 1;
            break;
        }
        if (!path[0]) {                                 /* a volume: "4:  FAT32 'PHONE CARD', 3780 MB, ..." */
            if (e->attr & 0x02)
                continue;                               /* (not readable) */
            char num[12], label[DISK_ENTRY_NAME];
            int j = 0, k = 0;
            while (e->name[j] && e->name[j] != ':' && j < 10)
                num[j] = e->name[j], j++;
            num[j++] = ':';
            num[j] = 0;
            const char *q = e->name;
            while (*q && *q != '\'')
                q++;
            if (*q)
                for (q++; *q && *q != '\'' && k < DISK_ENTRY_NAME - 1; q++)
                    label[k++] = *q;
            label[k] = 0;
            q = e->name;
            while (*q && *q != ':')
                q++;
            while (*q == ':' || *q == ' ')
                q++;
            len += usnprintf(t + len, 16, "%s{\"n\":", first ? "" : ",");
            len += json_str(t + len, 200, num);
            len += usnprintf(t + len, 16, ",\"l\":");
            len += json_str(t + len, 300, label);
            len += usnprintf(t + len, 16, ",\"i\":");
            len += json_str(t + len, 300, q);
            len += usnprintf(t + len, 16, ",\"d\":2}");
        } else {
            if (same(e->name, ".") || same(e->name, ".."))
                continue;
            char size[24];
            u32 y = 1980 + (e->date >> 9), m = (e->date >> 5) & 15, d = e->date & 31;
            u32 hh = e->time >> 11, mm = (e->time >> 5) & 63;
            len += usnprintf(t + len, 16, "%s{\"n\":", first ? "" : ",");
            len += json_str(t + len, 300, e->name);
            len += usnprintf(t + len, 120, ",\"d\":%u,\"s\":%s,\"t\":\"%04u-%02u-%02u %02u:%02u\"}",
                             e->kind == DISK_DIR ? 1 : 0, u64_text(size, (u64)e->size_hi << 32 | e->size), y, m, d,
                             hh, mm);
        }
        first = 0;
    }
    len += usnprintf(t + len, 64, "],\"cut\":%u}", cut || total > (u32)n ? 1 : 0);
    send_mem(cl, 200, "application/json", t, (u32)len, 0);
}

static void download(struct client *cl, const char *path, int inline_)
{
    u64 size;
    cap_begin();
    int o = disk_stream_open(path, &size);
    const char *why = cap_end();
    if (o < 0) {
        fail(cl, 404, why ? why : "Not read");
        return;
    }
    disk_owner = cl;
    scopy(cl->path, path, sizeof(cl->path));
    const char *name = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == ':')
            name = p + 1;
    char extra[300], safe[160];
    int k = 0;
    for (; name[k] && k < (int)sizeof(safe) - 1; k++)
        safe[k] = name[k] == '"' ? '\'' : name[k];
    safe[k] = 0;
    usnprintf(extra, sizeof(extra), "Content-Disposition: %s; filename=\"%s\"\r\n", inline_ ? "inline" : "attachment",
              safe);
    header(cl, 200, type_of(path), size, extra);
    cl->out = O_FILE;
    cl->left = size;
    cl->moved_at = now_ms();
}

static void upload(struct client *cl, const char *path, u64 length)
{
    cap_begin();
    int o = disk_upload_open(path, 0);
    const char *why = cap_end();
    if (o < 0) {
        cl->keep = 0;                                   /* (its body is not read: the connection ends) */
        fail(cl, 409, why ? why : "Not written");
        return;
    }
    disk_owner = cl;
    scopy(cl->path, path, sizeof(cl->path));
    cl->state = S_BODY;
    cl->body = B_UPLOAD;
    cl->body_left = length;
    cl->staged = 0;
    cl->ok = 1;
    cl->moved_at = now_ms();
}

static void change(struct client *cl, void (*fn)(const char *), const char *path)
{
    cap_begin();
    fn(path);
    const char *why = cap_end();
    if (why)
        fail(cl, 409, why);
    else
        done_ok(cl);
}

static void screen(struct client *cl, int half)
{
    int w = half ? SCREEN_W / 2 : SCREEN_W, h = half ? SCREEN_H / 2 : SCREEN_H;
    u32 image = (u32)w * (u32)h * 2, file = 66 + image;
    header(cl, 200, "image/bmp", file, 0);
    /* the BMP's own header after the HTTP one: a top-down (negative height) 16-bit picture with bit fields --
       ARGB1555 as the display reads it, the A bit left out */
    u8 *b = (u8 *)cl->hdr + cl->hdr_len;
    static const u8 fixed[66] = {'B', 'M', 0, 0, 0, 0, 0, 0, 0, 0, 66, 0, 0, 0, 40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                 1, 0, 16, 0, 3, 0, 0, 0, 0, 0, 0, 0, 0x13, 0x0B, 0, 0, 0x13, 0x0B, 0, 0, 0, 0, 0, 0,
                                 0, 0, 0, 0, 0x00, 0x7C, 0, 0, 0xE0, 0x03, 0, 0, 0x1F, 0, 0, 0};
    for (int i = 0; i < 66; i++)
        b[i] = fixed[i];
    u32 nh = (u32)-h;
    for (int i = 0; i < 4; i++) {
        b[2 + i] = (u8)(file >> (8 * i));
        b[18 + i] = (u8)((u32)w >> (8 * i));
        b[22 + i] = (u8)(nh >> (8 * i));
        b[34 + i] = (u8)(image >> (8 * i));
    }
    cl->hdr_len += 66;
    cl->out = O_SCREEN;
    cl->row = 0;
    cl->half = half;
    cl->fb = (const volatile u16 *)KSEG1(gfx_view_console() ? CON_FB_PHYS : FB_PHYS);
}

/* /api/out's answer: what was printed from `from` on */
static void out_answer(struct client *cl, u32 from)
{
    if (from > out_total)
        from = 0;                                       /* (the box started again since) */
    if (out_total - from > RING)
        from = out_total - RING;                        /* (older than the ring keeps) */
    char *t = text_of(cl);
    u32 n = out_total - from;
    for (u32 i = 0; i < n; i++)
        t[i] = (char)ring[(from + i) % RING];
    char extra[64];
    usnprintf(extra, sizeof(extra), "X-Next: %u\r\n", out_total);
    send_mem(cl, 200, "text/plain; charset=utf-8", t, n, extra);
}

static void info(struct client *cl, int in_cmd)
{
    char *t = text_of(cl);
    const u8 *ip = eth_address();
    int n = usnprintf(t, 400, "{\"banner\":\"%s\",\"version\":\"%s\",\"ip\":\"%u.%u.%u.%u\",\"uptime\":%u,"
                              "\"command\":%u}", APP_BANNER, APP_VERSION, ip[0], ip[1], ip[2], ip[3], uptime_s(),
                      in_cmd ? 1 : 0);
    send_mem(cl, 200, "application/json", t, (u32)n, 0);
}

/* the request in head[] (up to body_at) acted on */
static void request(struct client *cl, int in_cmd)
{
    requests++;
    char *h = cl->head;
    h[cl->body_at - 2] = 0;                             /* (the blank line's CR LF) */
    char *method = h, *target = h, *version = 0;
    while (*target && *target != ' ')
        target++;
    if (*target)
        *target++ = 0;
    version = target;
    while (*version && *version != ' ')
        version++;
    if (*version)
        *version++ = 0;
    char *query = target;
    while (*query && *query != '?')
        query++;
    if (*query)
        *query++ = 0;
    /* the header lines */
    u64 length = 0;
    int has_length = 0, chunked = 0;
    cl->keep = starts(version, "HTTP/1.1");
    for (char *p = version; *p; p++) {
        if (p[0] == '\r' && p[1] == '\n') {
            char *l = p + 2;
            if (starts_nocase(l, "content-length:")) {
                char *v = l + 15;
                while (*v == ' ')
                    v++;
                length = 0;
                while (*v >= '0' && *v <= '9')
                    length = length * 10 + (u64)(*v++ - '0');
                has_length = 1;
            } else if (starts_nocase(l, "connection:")) {
                char *v = l + 11;
                while (*v == ' ')
                    v++;
                if (starts_nocase(v, "close"))
                    cl->keep = 0;
                else if (starts_nocase(v, "keep-alive"))
                    cl->keep = 1;
            } else if (starts_nocase(l, "transfer-encoding:")) {
                chunked = 1;
            }
        }
    }
    /* the request line's own end (the version: up to its CR) */
    for (char *p = version; *p; p++)
        if (*p == '\r') {
            *p = 0;
            break;
        }
    int get = same(method, "GET") || same(method, "HEAD"), post = same(method, "POST"), put = same(method, "PUT");
    cl->body = B_DISCARD;                               /* (a body nobody uses: read and dropped) */
    cl->body_left = has_length ? length : 0;
    if (chunked) {
        cl->keep = 0;
        fail(cl, 411, "Send a Content-Length (no chunked bodies)");
        return;
    }
    char path[PATH_MAX_], to[PATH_MAX_], v[16];
    if (param(query, "p", path, sizeof(path)) < 0 || param(query, "to", to, sizeof(to)) < 0) {
        fail(cl, 400, "The path is too long");
        return;
    }
    if (get && (same(target, "/") || same(target, "/index.html"))) {
        send_mem(cl, 200, "text/html; charset=utf-8", web_page, (u32)(web_page_end - web_page), 0);
    } else if (get && same(target, "/api/info")) {
        info(cl, in_cmd);
    } else if (get && same(target, "/api/out")) {
        param(query, "from", v, sizeof(v));
        u32 from = v[0] ? number(v) : out_total > FIRST_OUT ? out_total - FIRST_OUT : 0;
        if (v[0] && from == out_total) {
            cl->state = S_SEND;                         /* (nothing new: wait for it, up to WAIT_OUT_MS) */
            cl->out = O_WAIT_OUT;
            cl->hdr_len = cl->hdr_pos = 0;
            cl->from = from;
            cl->since = now_ms();
        } else {
            out_answer(cl, from);
        }
    } else if (post && same(target, "/api/in")) {
        cl->body = B_INPUT;
        done_ok(cl);
        cl->state = S_BODY;                             /* (the answer after the body) */
    } else if (get && same(target, "/api/screen.bmp")) {
        param(query, "half", v, sizeof(v));
        screen(cl, v[0] == '1');
    } else if (get && same(target, "/api/ls")) {
        if (disk_free(cl, in_cmd, 0))
            list(cl, path);
    } else if (get && same(target, "/api/get")) {
        param(query, "inline", v, sizeof(v));
        if (!path[0])
            fail(cl, 400, "Which file? (p=4:/name)");
        else if (disk_free(cl, in_cmd, 1))
            download(cl, path, v[0] == '1');
    } else if (put && same(target, "/api/put")) {
        if (!has_length) {
            cl->keep = 0;
            fail(cl, 411, "Send a Content-Length");
        } else if (!path[0]) {
            cl->keep = 0;
            fail(cl, 400, "Which file? (p=4:/name)");
        } else if (!disk_free(cl, in_cmd, 1)) {
            cl->keep = 0;                               /* (its body is not read: the connection ends) */
        } else {
            upload(cl, path, length);
        }
    } else if (post && (same(target, "/api/rm") || same(target, "/api/rmdir") || same(target, "/api/mkdir"))) {
        if (!path[0])
            fail(cl, 400, "Which one? (p=4:/name)");
        else if (disk_free(cl, in_cmd, 0))
            change(cl, same(target, "/api/rm") ? disk_rm : same(target, "/api/rmdir") ? disk_rmdir : disk_mkdir, path);
    } else if (post && same(target, "/api/mv")) {
        if (!path[0] || !to[0]) {
            fail(cl, 400, "From where to where? (p=4:/a&to=4:/b)");
        } else if (disk_free(cl, in_cmd, 0)) {
            cap_begin();
            disk_mv(path, to);
            const char *why = cap_end();
            if (why)
                fail(cl, 409, why);
            else
                done_ok(cl);
        }
    } else if (starts(target, "/api/") || !get) {
        fail(cl, starts(target, "/api/") ? 404 : 405, "No such request");
    } else {
        fail(cl, 404, "No such page");
    }
    if (same(method, "HEAD") && cl->state == S_SEND && cl->out != O_WAIT_OUT) {
        cl->out = O_NONE;                               /* (the header alone) */
        if (disk_owner == cl) {
            disk_stream_close();
            disk_owner = 0;
        }
    }
    /* a body that came with a request not reading it (and not one to wait for: an error ends the connection) */
    if (cl->body == B_DISCARD && cl->body_left && cl->state == S_SEND)
        cl->keep = 0;
}

/* ---- moving the bytes ---- */
static void release(struct client *cl, int broken)
{
    if (disk_owner != cl)
        return;
    if (cl->out == O_FILE) {
        disk_stream_close();
    } else if (cl->body == B_UPLOAD) {
        cap_begin();
        int r = disk_upload_close();
        (void)cap_end();
        if (broken || r < 0 || !cl->ok) {               /* (an upload cut short is not kept) */
            cap_begin();
            disk_rm(cl->path);
            (void)cap_end();
        }
    }
    disk_owner = 0;
}

static void drop(struct client *cl, int abort)
{
    release(cl, 1);
    if (abort)
        tcp_abort(cl->c);
    else
        tcp_close(cl->c);
    cl->state = S_FREE;
    cl->c = -1;
}

/* the bytes of the request's body already read with its head, then the connection's: up to max */
static int body_bytes(struct client *cl, u8 *to, int max)
{
    int pre = cl->head_len - cl->body_at;
    if (pre > 0) {
        int n = pre < max ? pre : max;
        for (int i = 0; i < n; i++)
            to[i] = (u8)cl->head[cl->body_at + i];
        cl->body_at += n;
        return n;
    }
    return tcp_recv(cl->c, to, max);
}

/* a request's body read: written, typed or dropped.  A line for the console when an upload ended */
static const char *body_pump(struct client *cl, int in_cmd)
{
    if (cl->body == B_UPLOAD && in_cmd)
        return 0;                                       /* (the disks are the command's) */
    u64 now = now_ms();
    if (cl->body == B_UPLOAD) {
        for (int k = 0; k < WRITES && cl->body_left;) {
            u32 want = CHUNK - cl->staged;
            if (want > cl->body_left)
                want = (u32)cl->body_left;
            int n = body_bytes(cl, cl->stage + cl->staged, (int)want);
            if (n <= 0)
                break;
            cl->staged += (u32)n;
            cl->body_left -= (u32)n;
            cl->moved_at = now;
            if (cl->staged < CHUNK && cl->body_left)
                continue;
            k++;
            u32 len = cl->staged;
            cl->staged = 0;
            if (!cl->ok)
                continue;
            cap_begin();
            int r = disk_upload_put(cl->stage, len);
            const char *why = cap_end();
            if (r < 0) {                                /* (disk_upload_put let the file go) */
                cl->ok = 0;
                disk_owner = 0;
                cl->keep = 0;
                fail(cl, 409, why ? why : "Not written");
                return 0;
            }
        }
    }
    for (int k = 0; k < 8 && cl->body != B_UPLOAD && cl->body_left; k++) {
        int max = cl->body_left < CHUNK ? (int)cl->body_left : CHUNK;
        if (cl->body == B_INPUT && max > uart_inject_room())
            max = uart_inject_room();
        if (max <= 0)
            break;
        int n = body_bytes(cl, chunk, max);
        if (n <= 0)
            break;
        cl->body_left -= (u32)n;
        cl->moved_at = now;
        if (cl->body == B_INPUT)
            for (int i = 0; i < n; i++)
                uart_inject(chunk[i]);
    }
    if (cl->body_left) {
        if (cl->body == B_UPLOAD && now - cl->moved_at > STALL_MS) {
            release(cl, 1);
            cl->keep = 0;
            fail(cl, 408, "Nothing came for a minute");
        }
        return 0;
    }
    if (cl->body == B_UPLOAD) {
        cap_begin();
        int r = disk_upload_close();
        const char *why = cap_end();
        disk_owner = 0;
        if (r < 0) {
            fail(cl, 409, why ? why : "Not written");
            return 0;
        }
        got_files++;
        done_ok(cl);
        u8 *ip = cl->ip;
        usnprintf(line, sizeof(line), "web: %s received from %u.%u.%u.%u", cl->path, ip[0], ip[1], ip[2], ip[3]);
        return line;
    }
    cl->state = S_SEND;                                 /* (B_INPUT: its answer is ready; B_DISCARD: the response) */
    return 0;
}

/* the response sent: 1 when all of it went */
static int send_pump(struct client *cl, int in_cmd, const char **say)
{
    int c = cl->c;
    if (cl->out == O_WAIT_OUT) {
        if (out_total == cl->from && now_ms() - cl->since < WAIT_OUT_MS)
            return 0;
        out_answer(cl, cl->from);
    }
    if (cl->hdr_pos < cl->hdr_len) {
        u32 room = tcp_room(c), n = cl->hdr_len - cl->hdr_pos;
        if (room < n)
            return 0;
        tcp_send(c, cl->hdr + cl->hdr_pos, (int)n);
        cl->hdr_pos = cl->hdr_len;
    }
    switch (cl->out) {
    case O_MEM:
        while (cl->mem_pos < cl->mem_len) {
            u32 room = tcp_room(c), n = cl->mem_len - cl->mem_pos;
            if (!room)
                return 0;
            if (n > room)
                n = room;
            cl->mem_pos += (u32)tcp_send(c, cl->mem + cl->mem_pos, (int)n);
        }
        return 1;
    case O_FILE: {
        if (in_cmd)
            return 0;                                   /* (the disks are the command's) */
        u64 now = now_ms();
        for (int k = 0; k < READS && cl->left && tcp_room(c) >= CHUNK; k++) {
            cap_begin();
            int n = disk_stream_read(chunk, CHUNK);
            (void)cap_end();
            if (n <= 0) {                               /* (a sector that did not read: the connection ends) */
                cl->keep = 0;
                drop(cl, 1);
                return 0;
            }
            if ((u64)n > cl->left)
                n = (int)cl->left;
            tcp_send(c, chunk, n);
            cl->left -= (u32)n;
            cl->moved_at = now;
        }
        if (cl->left) {
            if (now - cl->moved_at > STALL_MS)
                drop(cl, 1);
            return 0;
        }
        disk_stream_close();
        disk_owner = 0;
        sent_files++;
        u8 *ip = cl->ip;
        usnprintf(line, sizeof(line), "web: %s sent to %u.%u.%u.%u", cl->path, ip[0], ip[1], ip[2], ip[3]);
        *say = line;
        return 1;
    }
    case O_SCREEN: {
        int w = cl->half ? SCREEN_W / 2 : SCREEN_W, h = cl->half ? SCREEN_H / 2 : SCREEN_H;
        u32 bytes = (u32)w * 2;
        while (cl->row < h && tcp_room(c) >= bytes) {
            const volatile u16 *src = cl->fb + (u32)cl->row * (cl->half ? 2u : 1u) * SCREEN_W;
            u16 *to = (u16 *)chunk;
            if (cl->half)
                for (int x = 0; x < w; x++)
                    to[x] = src[x * 2];
            else
                for (int x = 0; x < w; x++)
                    to[x] = src[x];
            tcp_send(c, chunk, (int)bytes);
            cl->row++;
        }
        return cl->row >= h;
    }
    }
    return 1;
}

/* ---- the server ---- */
static struct client *slot(void)
{
    for (int i = 0; i < CLIENTS; i++)
        if (clients[i].state == S_FREE)
            return &clients[i];
    return 0;
}

int http_clients(void)
{
    int n = 0;
    for (int i = 0; i < CLIENTS; i++)
        n += clients[i].state != S_FREE;
    return n;
}

int http_disk_busy(void)
{
    return disk_owner != 0;
}

/* net_poll (in_cmd 0) and net_uart_poll while a command runs (1): a line for the console, or 0 */
const char *http_poll(int in_cmd)
{
    if (polling)
        return 0;                                       /* (not into itself: a disk call's wait reads the UART) */
    polling = 1;
    if (!started) {
        started = 1;
        tcp_listen(HTTP_PORT);
        for (int i = 0; i < CLIENTS; i++)
            clients[i].state = S_FREE, clients[i].c = -1;
    }
    const char *say = 0;
    struct client *free_one = slot();
    int c = free_one ? tcp_accept(HTTP_PORT) : -1;      /* (none free: the connection waits in tcp.c) */
    if (c >= 0) {
        struct client *cl = free_one;
        u16 port;
        cl->c = c;
        tcp_peer(c, cl->ip, &port);
        cl->state = S_HEAD;
        cl->head_len = cl->body_at = 0;
        cl->heard = now_ms();
        cl->out = O_NONE;
        cl->body = B_NONE;
    }
    for (int i = 0; i < CLIENTS; i++) {
        struct client *cl = &clients[i];
        if (cl->state == S_FREE)
            continue;
        int st = tcp_status(cl->c);
        if (st == TCP_FAILED || st == TCP_DONE) {
            drop(cl, st == TCP_FAILED);
            continue;
        }
        if (cl->state == S_HEAD) {
            int n = tcp_recv(cl->c, cl->head + cl->head_len, HEAD_MAX - cl->head_len);
            if (n > 0) {
                cl->head_len += n;
                cl->heard = now_ms();
                cl->head[cl->head_len] = 0;
                int end = -1;
                for (int k = 3; k < cl->head_len && end < 0; k++)
                    if (cl->head[k - 3] == '\r' && cl->head[k - 2] == '\n' && cl->head[k - 1] == '\r'
                        && cl->head[k] == '\n')
                        end = k + 1;
                if (end >= 0) {
                    cl->body_at = end;
                    request(cl, in_cmd);
                } else if (cl->head_len >= HEAD_MAX) {
                    cl->keep = 0;
                    cl->body_left = 0;
                    fail(cl, 431, "The request's header is too long");
                }
            } else if (st == TCP_PEER_DONE || now_ms() - cl->heard > IDLE_MS) {
                drop(cl, 0);                            /* (the browser let a kept-alive connection go) */
                continue;
            }
        }
        if (cl->state == S_BODY) {
            const char *s = body_pump(cl, in_cmd);
            if (s)
                say = s;
        }
        if (cl->state == S_SEND) {
            if (cl->body == B_DISCARD && cl->body_left)
                body_pump(cl, in_cmd);                  /* (dropped: the error answers before it all came) */
            if (!send_pump(cl, in_cmd, &say) || cl->state != S_SEND)
                continue;
            cl->out = O_NONE;
            cl->body = B_NONE;
            if (!cl->keep) {
                tcp_close(cl->c);
                cl->state = S_FREE;
                cl->c = -1;
            } else {
                cl->state = S_HEAD;                     /* (the next request on the same connection) */
                cl->head_len = cl->body_at = 0;
                cl->heard = now_ms();
            }
        }
    }
    polling = 0;
    return in_cmd ? 0 : say;
}

void http_status(void)
{
    int n = http_clients();
    uprintf("web page (port %u): http://%u.%u.%u.%u/ -- %u connection%s; %u requests, %u files sent, %u received%s\n",
            HTTP_PORT, eth_address()[0], eth_address()[1], eth_address()[2], eth_address()[3], n, n == 1 ? "" : "s",
            requests, sent_files, got_files, disk_owner ? "; a file is moving" : "");
}
