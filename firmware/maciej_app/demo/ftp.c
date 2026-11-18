/* An FTP server on port 21 (RFC 959 and the usual extensions: PASV / EPSV,
   PORT / EPRT, MLSD / MLST, SIZE, MDTM, REST, FEAT) without a password: its
   root lists every disk's volumes as directories -- "4 PHONE CARD", "9
   Windows 10": the number as 'cd' takes it ("4.2" a disk's second
   partition) and the label -- and everything on them can be read, FAT and
   NTFS, and written (STOR, APPE, DELE, MKD, RMD, RNFR / RNTO): FAT volumes
   always, an NTFS one when Windows left it clean (disk.c's gate: else it
   refuses, and the reply says why).  Anyone on the network gets all of
   it: a home network's server.

   Two clients at a time (FileZilla opens a second connection for its
   transfers), each with its own passive data port (50021, 50022), its data
   connection taken only from its own address; PORT / EPRT connect to the
   client's own address only.  One transfer at a time: another client's gets
   "450 try again" (clients do).  A transfer moves in the main loop, a few
   4 KB pieces a pass (net_poll: never inside a command, whose disk reads
   would share disk.c's buffers), so the pointer, the keyboard and the
   console go on meanwhile; its file is read by disk.c's reader for ftp.c
   (its own NTFS runs) and written by its own writer.  A transfer that moves
   nothing for a minute ends, and a client silent for five minutes is let
   go (tcp.c has no keepalive: a laptop that went to sleep would hold its
   place for ever).

   Known limits: a STOR over a file empties it first (as most servers do);
   a directory listed shows its first 7000 entries (451 when cut); disk.c's
   paths are 95 characters at most and its names 127 (longer ones are
   refused, not cut); writing the same disk from the shell while an upload
   runs is not guarded. */
#include "hw.h"

#define FTP_PORT    21
#define DATA_PORT   50021               /* + the session's number */
#define SESSIONS    2
#define CHUNK       4096
#define READS       8                   /* pieces read and sent each pass */
#define WRITES      4                   /* pieces received and written each pass (the card: 450 KB/s) */
#define ENTRIES     ((int)(0x100000 / sizeof(struct disk_entry)))  /* a listing's entries (FTP_ENTRIES_PHYS) */
#define TEXT_MAX    0x100000u           /* its text (FTP_TEXT_PHYS) */
#define WAIT_MS     15000               /* for a data connection */
#define STALL_MS    60000               /* a transfer that moved nothing for this long ends */
#define IDLE_MS     300000              /* a client that sent nothing for this long is let go */
#define DISK_PATH   95                  /* disk.c's longest path (PATH_MAX 96) */
#define DISK_NAME   127                 /* its longest name (NAME_MAX 128) */

struct session {
    int fc;                             /* the control connection (-1: none) */
    u8 ip[4];
    char in[600];                       /* a command line coming */
    int in_len, in_long;
    u8 in_iac;                          /* Telnet command bytes to skip (IAC IP before ABOR) */
    char cwd[340];                      /* "/" or "/4 PHONE CARD/DIR" */
    char rnfr[340];                     /* RNFR's disk path ("" none) */
    u64 rest;                           /* REST: for the command right after it only */
    int mode;                           /* 0 no data connection asked for, 1 passive, 2 PORT */
    u8 act_ip[4];
    u16 act_port;
    int dc;                             /* the data connection (-1: none) */
    u64 heard;                          /* when it last sent something */
};

static struct session sessions[SESSIONS];
static int started;

/* the transfer (one at a time) */
enum { X_NONE, X_LIST, X_RETR, X_STOR };
static int xfer;
static struct session *xs;              /* whose */
static u64 xfer_since, xfer_size, moved, moved_at, skip;
static int xfer_eof, list_cut;
static char xfer_path[340];
static struct disk_entry *const ents = (struct disk_entry *)KSEG0(FTP_ENTRIES_PHYS);
static char *const text = (char *)KSEG0(FTP_TEXT_PHYS);
static u32 text_len, text_pos;
static u8 chunk[CHUNK];
static char said[200];                  /* what a disk call printed (its first line) */
static char line[160];                  /* a line for the console */
static u32 sent_files, got_files;

/* ---- small helpers ---- */
static int slen(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

static void scopy(char *to, const char *from, int size)
{
    int i = 0;
    for (; i < size - 1 && from[i]; i++)
        to[i] = from[i];
    to[i] = 0;
}

static int upper(int c)
{
    return c >= 'a' && c <= 'z' ? c - 32 : c;
}

static int verb_is(const char *v, const char *w)
{
    while (*v && *w && *v == *w) {
        v++;
        w++;
    }
    return !*v && !*w;
}

static int same_ip(const u8 *a, const u8 *b)
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

/* a 64-bit number in decimal (no 64-bit division in the demo: by ten with shifts) */
static const char *u64_text(char *buf, u64 v)
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

static u64 size_of(const struct disk_entry *e)
{
    return (u64)e->size_hi << 32 | e->size;
}

/* ---- the control connection ---- */
static void reply(struct session *s, const char *text_line)
{
    if (s->fc < 0)
        return;
    tcp_send(s->fc, text_line, slen(text_line));
    tcp_send(s->fc, "\r\n", 2);
    tcp_push(s->fc);
}

static void replyf(struct session *s, const char *fmt, const char *a, const char *b)
{
    char t[480];
    usnprintf(t, sizeof(t), fmt, a, b);
    reply(s, t);
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

/* `arg` (absolute, or relative to cwd) as a clean path into out: "/" or "/a/b"; -1 too long */
static int resolve(const struct session *s, const char *arg, char *out, int size)
{
    int n = 0;
    const char *from[2] = {arg[0] == '/' || arg[0] == '\\' ? "" : s->cwd, arg};
    for (int k = 0; k < 2; k++) {
        const char *p = from[k];
        while (*p) {
            while (*p == '/' || *p == '\\')
                p++;
            const char *q = p;
            while (*q && *q != '/' && *q != '\\')
                q++;
            int len = (int)(q - p);
            if (len == 2 && p[0] == '.' && p[1] == '.') {
                while (n > 0 && out[n - 1] != '/')
                    n--;
                if (n > 0)
                    n--;
            } else if (len && !(len == 1 && p[0] == '.')) {
                if (n + 1 + len >= size)
                    return -1;
                out[n++] = '/';
                for (int i = 0; i < len; i++)
                    out[n++] = p[i];
            }
            p = q;
        }
    }
    if (!n)
        out[n++] = '/';
    out[n] = 0;
    return 0;
}

/* a clean path as disk.c's: "/4 PHONE CARD/DIR/x" -> "4:/DIR/x": 1; the root ("/"): 0; -1 not a volume's
   name (it starts with the volume's number); -2 longer than disk.c takes (refused: never cut) */
static int disk_path(const char *v, char *out, int size)
{
    if (!v[1])
        return 0;
    const char *p = v + 1;
    int n = 0;
    while (((*p >= '0' && *p <= '9') || *p == '.') && n < 8)
        out[n++] = *p++;
    if (!n || (*p && *p != ' ' && *p != '/'))
        return -1;
    while (*p && *p != '/')
        p++;                            /* (its label) */
    out[n++] = ':';
    if (!*p)
        p = "/";
    int start = n, part = 0;            /* (the path after "n:": "/DIR/x") */
    while (*p && n < size - 1) {
        part = *p == '/' ? 0 : part + 1;
        if (part > DISK_NAME)
            return -2;
        out[n++] = *p++;
    }
    out[n] = 0;
    return *p || n - start > DISK_PATH ? -2 : 1;
}

static const char *path_problem(int r)
{
    return r == -2 ? "The path is longer than the demo takes (95 characters on a disk, names 127)"
         : r == -1 ? "No such volume"
                   : "The root: it holds the volumes";
}

/* the volumes as the root's directories: "4 PHONE CARD" from disk_volumes' "4:  FAT32 'PHONE CARD', ..." */
static int volumes(void)
{
    int n = disk_volumes(ents, ENTRIES), k = 0;
    for (int i = 0; i < n; i++) {
        struct disk_entry *e = &ents[i];
        if (e->attr & 0x02)
            continue;                   /* (its files are not read) */
        char name[DISK_ENTRY_NAME];
        int j = 0, at = 0;
        while (e->name[j] && e->name[j] != ':' && at < 8)
            name[at++] = e->name[j++];
        const char *q = e->name + j;
        while (*q && *q != '\'')
            q++;
        if (*q == '\'') {
            name[at++] = ' ';
            for (q++; *q && *q != '\'' && at < DISK_ENTRY_NAME - 1; q++)
                name[at++] = *q == '/' ? '_' : *q;
        }
        name[at] = 0;
        ents[k] = *e;
        scopy(ents[k].name, name, DISK_ENTRY_NAME);
        ents[k].kind = DISK_DIR;
        ents[k].size = ents[k].size_hi = 0;
        ents[k].date = (1 << 5) | 1;    /* (1980-01-01) */
        ents[k].time = 0;
        k++;
    }
    return k;
}

static const char *const months[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct",
                                       "Nov", "Dec"};

/* an entry as a line of a listing: LIST's (ls -l), MLSD's facts, NLST's name */
static int entry_line(char *b, int room, const struct disk_entry *e, int how)
{
    u32 y = 1980 + (e->date >> 9), m = (e->date >> 5) & 15, d = e->date & 31;
    u32 hh = e->time >> 11, mm = (e->time >> 5) & 63, ss = (e->time & 31) * 2;
    if (m < 1 || m > 12)
        m = 1;
    char size[24];
    u64_text(size, size_of(e));
    if (how == 'N')
        return usnprintf(b, room, "%s\r\n", e->name);
    if (how == 'M')
        return usnprintf(b, room, "type=%s;size=%s;modify=%04u%02u%02u%02u%02u%02u; %s\r\n",
                         e->kind == DISK_DIR ? "dir" : "file", size, y, m, d, hh, mm, ss, e->name);
    return usnprintf(b, room, "%s 1 m3801 m3801 %12s %s %2u %5u %s\r\n",
                     e->kind == DISK_DIR ? "drwxr-xr-x" : "-rw-r--r--", size, months[m - 1], d, y, e->name);
}

/* ---- transfers ---- */
static void data_drop(struct session *s, int abort)
{
    if (s->dc >= 0) {
        if (abort)
            tcp_abort(s->dc);
        else
            tcp_close(s->dc);
    }
    s->dc = -1;
    s->mode = 0;
}

/* the transfer ended: an upload's file closed (what came is kept), the reader let go */
static void xfer_end(void)
{
    if (xfer == X_STOR)
        (void)disk_upload_close();
    else if (xfer == X_RETR)
        disk_stream_close();
    xfer = X_NONE;
    if (xs)
        xs->heard = now_ms();           /* (no command came while its file moved: after one of over five minutes
                                           the session was told "421 No command for five minutes" at once) */
    xs = 0;
    skip = 0;
}

/* someone else's transfer under way: 450 (clients try again) */
static int busy(struct session *s)
{
    if (!xfer)
        return 0;
    reply(s, xs == s ? "503 A transfer is under way (ABOR stops it)"
                     : "450 The box moves one file at a time: try again in a moment");
    return 1;
}

/* a transfer's data connection asked for: 150, and (PORT) the connection to the client begun: 0, -1 */
static int xfer_begin(struct session *s, int kind, const char *what)
{
    if (!s->mode) {
        reply(s, "425 Use PASV, EPSV, PORT or EPRT first");
        return -1;
    }
    if (s->mode == 2 && s->dc < 0 && (s->dc = tcp_connect(s->act_ip, s->act_port)) < 0) {
        reply(s, "425 Can't open a data connection to that port");
        s->mode = 0;
        return -1;
    }
    xfer = kind;
    xs = s;
    xfer_since = moved_at = now_ms();
    moved = 0;
    xfer_eof = 0;
    replyf(s, "150 %s", what, "");
    return 0;
}

static void listing(struct session *s, const char *arg, int how)
{
    char v[340], p[340];
    while (*arg == '-') {                       /* (LIST -la: its options left out) */
        while (*arg && *arg != ' ')
            arg++;
        while (*arg == ' ')
            arg++;
    }
    if (resolve(s, *arg ? arg : ".", v, sizeof(v)) < 0) {
        reply(s, "550 The path is too long");
        return;
    }
    int r = disk_path(v, p, sizeof(p)), n;
    u32 total = 0;
    if (r < 0) {
        replyf(s, "550 %s", path_problem(r), "");
        return;
    }
    if (!r) {
        n = volumes();
        total = (u32)n;
    } else {
        cap_begin();
        n = disk_dir_read(p, ents, ENTRIES, &total);
        const char *why = cap_end();
        if (n < 0) {
            replyf(s, "550 %s", why ? why : "Not read", "");
            return;
        }
    }
    text_len = text_pos = 0;
    list_cut = total > (u32)n;
    for (int i = 0; i < n; i++) {
        int k = entry_line(text + text_len, (int)(TEXT_MAX - text_len), &ents[i], how);
        if (k <= 0 || text_len + (u32)k >= TEXT_MAX) {
            list_cut = 1;
            break;
        }
        text_len += (u32)k;
    }
    xfer_begin(s, X_LIST, how == 'M' ? "Here comes the listing (MLSD)" : "Here comes the listing");
}

static void retr(struct session *s, const char *arg, u64 from)
{
    char v[340];
    int r = resolve(s, arg, v, sizeof(v)) < 0 ? -2 : disk_path(v, xfer_path, sizeof(xfer_path));
    if (r <= 0) {
        replyf(s, "550 %s", r ? path_problem(r) : "Not a file", "");
        return;
    }
    if (!s->mode) {
        reply(s, "425 Use PASV, EPSV, PORT or EPRT first");
        return;
    }
    cap_begin();
    int o = disk_stream_open(xfer_path, &xfer_size);
    const char *why = cap_end();
    if (o < 0) {
        replyf(s, "550 %s", why ? why : "Not read", "");
        return;
    }
    char t[24];
    char what[80];
    usnprintf(what, sizeof(what), "Opening BINARY mode data connection (%s bytes)", u64_text(t, xfer_size));
    if (xfer_begin(s, X_RETR, what) < 0) {
        disk_stream_close();
        return;
    }
    skip = from < xfer_size ? from : xfer_size;
    if (skip)
        skip -= disk_stream_seek(skip);         /* (not read from the file's start: a gigabyte took ten minutes
                                                   before the first byte went) */
}

static void stor(struct session *s, const char *arg, int append, u64 from)
{
    char v[340];
    int r = resolve(s, arg, v, sizeof(v)) < 0 ? -2 : disk_path(v, xfer_path, sizeof(xfer_path));
    if (r <= 0) {
        replyf(s, "553 %s", r ? path_problem(r) : "Not a file name on a volume", "");
        return;
    }
    if (from) {
        reply(s, "501 REST with STOR: not supported (APPE adds to a file)");
        return;
    }
    if (!s->mode) {
        reply(s, "425 Use PASV, EPSV, PORT or EPRT first");
        return;
    }
    cap_begin();
    int o = disk_upload_open(xfer_path, append);
    const char *why = cap_end();
    if (o < 0) {
        replyf(s, "553 %s", why ? why : "Not written", "");
        return;
    }
    if (xfer_begin(s, X_STOR, "Ok to send data") < 0)
        (void)disk_upload_close();
}

/* the transfer moved on: a line for the console when a file's ended */
static const char *pump(void)
{
    if (!xfer)
        return 0;
    struct session *s = xs;
    u64 now = now_ms();
    /* (this runs between commands only: a command of over a minute -- a game, a video -- left moved_at that old
       though the client never stopped, and the transfer was ended with 426 at the first look after it.  After
       such a gap both clocks start again) */
    static u64 pumped_at;
    if (now - pumped_at > 2000)
        xfer_since = moved_at = now;
    pumped_at = now;
    if (s->dc < 0) {
        if (now - xfer_since > WAIT_MS) {
            reply(s, "425 No data connection came");
            xfer_end();
            s->mode = 0;
        }
        return 0;
    }
    int st = tcp_status(s->dc);
    if (st == TCP_CONNECTING) {
        if (now - xfer_since > WAIT_MS) {
            reply(s, "425 The data connection did not open");
            xfer_end();
            data_drop(s, 1);
        }
        return 0;
    }
    if (now - moved_at > STALL_MS) {            /* (the client went: no keepalive in tcp.c) */
        reply(s, "426 Nothing moved for a minute: transfer ended");
        xfer_end();
        data_drop(s, 1);
        return 0;
    }
    if (st == TCP_FAILED || (xfer != X_STOR && st != TCP_OPEN && st != TCP_PEER_DONE)) {
        replyf(s, "426 The data connection broke (%s)", st == TCP_FAILED ? tcp_why(s->dc) : "closed", "");
        xfer_end();                             /* (an upload: what came is kept) */
        data_drop(s, 1);
        return 0;
    }
    if (xfer == X_STOR) {
        for (int k = 0; k < WRITES; k++) {
            int n = tcp_recv(s->dc, chunk, CHUNK);
            if (n > 0) {
                cap_begin();
                int r = disk_upload_put(chunk, (u32)n);
                const char *why = cap_end();
                if (r < 0) {
                    replyf(s, "451 %s", why ? why : "Not written", "");
                    xfer = X_NONE;              /* (disk_upload_put let the file go) */
                    xfer_end();
                    data_drop(s, 1);
                    return 0;
                }
                moved += (u32)n;
                moved_at = now;
                continue;
            }
            if (st == TCP_PEER_DONE && !tcp_unread(s->dc)) {
                cap_begin();
                int r = disk_upload_close();
                const char *why = cap_end();
                char t[24];
                xfer = X_NONE;                  /* (closed already) */
                xfer_end();
                data_drop(s, 0);
                if (r < 0) {
                    replyf(s, "451 %s", why ? why : "Not written", "");
                    return 0;
                }
                replyf(s, "226 Transfer complete (%s bytes)", u64_text(t, moved), "");
                got_files++;
                usnprintf(line, sizeof(line), "ftp: %s received (%s bytes)", xfer_path, t);
                return line;
            }
            break;
        }
        return 0;
    }
    for (int k = 0; k < READS && !xfer_eof && tcp_room(s->dc) >= CHUNK; k++) {
        int n;
        if (xfer == X_LIST) {
            n = text_len - text_pos < CHUNK ? (int)(text_len - text_pos) : CHUNK;
            for (int i = 0; i < n; i++)
                chunk[i] = (u8)text[text_pos + i];
            text_pos += (u32)n;
        } else {
            cap_begin();
            n = disk_stream_read(chunk, CHUNK);
            const char *why = cap_end();
            if (n < 0) {
                replyf(s, "451 %s", why ? why : "A sector did not read", "");
                xfer_end();
                data_drop(s, 1);
                return 0;
            }
        }
        if (n <= 0) {
            xfer_eof = 1;
            break;
        }
        int from = 0;
        if (skip) {                             /* (REST: the bytes before it read and left out) */
            u32 sk = skip < (u64)n ? (u32)skip : (u32)n;
            skip -= sk;
            from = (int)sk;
        }
        if (n > from)
            tcp_send(s->dc, chunk + from, n - from);
        moved += (u32)(n - from);
        moved_at = now;
    }
    if (!xfer_eof)
        return 0;
    char t[24];
    int kind = xfer;
    if (kind == X_LIST && list_cut)
        reply(s, "451 The listing was cut: the demo lists a directory's first 7000 entries");
    else
        replyf(s, "226 Transfer complete (%s bytes)", u64_text(t, moved), "");
    xfer_end();
    data_drop(s, 0);
    if (kind != X_RETR)
        return 0;
    sent_files++;
    usnprintf(line, sizeof(line), "ftp: %s sent (%s bytes)", xfer_path, t);
    return line;
}

/* ---- the commands ---- */
static void passive(struct session *s, int extended)
{
    if (s->dc >= 0)
        data_drop(s, 1);
    s->mode = 1;
    char t[96];
    const u8 *ip = eth_address();
    u32 port = DATA_PORT + (u32)(s - sessions);
    if (extended)
        usnprintf(t, sizeof(t), "229 Entering Extended Passive Mode (|||%u|)", port);
    else
        usnprintf(t, sizeof(t), "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u)", ip[0], ip[1], ip[2], ip[3],
                  port >> 8, port & 255);
    reply(s, t);
}

static int number(const char **p, u32 *v, u32 max)
{
    u32 n = 0;
    int digits = 0;
    while (**p >= '0' && **p <= '9' && digits < 5) {
        n = n * 10 + (u32)(*(*p)++ - '0');
        digits++;
    }
    *v = n;
    return digits && n <= max && !(**p >= '0' && **p <= '9');
}

/* PORT a,b,c,d,p1,p2 / EPRT |1|a.b.c.d|port|: the data connection made to the client -- its own address
   only (no "bounce" to another host); a refused one leaves no data connection asked for */
static void port(struct session *s, const char *a, int extended)
{
    u32 v[6];
    const char *p = a;
    int ok = 1;
    if (extended) {
        char d = *p;
        ok = d && *++p == '1' && *++p == d;
        p++;
        for (int i = 0; ok && i < 4; i++)
            ok = number(&p, &v[i], 255) && *p++ == (i < 3 ? '.' : d);
        u32 pt = 0;
        ok = ok && number(&p, &pt, 65535) && *p == d;
        v[4] = pt >> 8;
        v[5] = pt & 255;
    } else {
        for (int i = 0; ok && i < 6; i++)
            ok = number(&p, &v[i], 255) && (i == 5 || *p++ == ',');
    }
    if (s->dc >= 0)
        data_drop(s, 1);
    s->mode = 0;
    if (!ok) {
        reply(s, extended ? "522 Only IPv4 (|1|a.b.c.d|port|)" : "501 PORT a,b,c,d,p1,p2");
        return;
    }
    u8 ip[4] = {(u8)v[0], (u8)v[1], (u8)v[2], (u8)v[3]};
    if (!same_ip(ip, s->ip)) {
        reply(s, "504 Data connections go to the client's own address only");
        return;
    }
    for (int i = 0; i < 4; i++)
        s->act_ip[i] = ip[i];
    s->act_port = (u16)(v[4] << 8 | v[5]);
    s->mode = 2;
    reply(s, "200 PORT command successful");
}

/* a change through one of disk.c's commands (they print only when something is wrong) */
static void change(struct session *s, void (*fn)(const char *), const char *arg, const char *ok)
{
    char v[340], p[340];
    int r = resolve(s, arg, v, sizeof(v)) < 0 ? -2 : disk_path(v, p, sizeof(p));
    if (r <= 0) {
        replyf(s, "550 %s", path_problem(r), "");
        return;
    }
    cap_begin();
    fn(p);
    const char *why = cap_end();
    if (why)
        replyf(s, "550 %s", why, "");
    else
        replyf(s, ok, v, "");
}

static void info(struct session *s, const char *verb, const char *a)
{
    static struct disk_entry e;
    char v[340], p[340];
    if (resolve(s, *a ? a : ".", v, sizeof(v)) < 0) {
        reply(s, "550 The path is too long");
        return;
    }
    int r = disk_path(v, p, sizeof(p)), mlst = verb[0] == 'M' && verb[1] == 'L';
    const char *why = r < 0 ? path_problem(r) : 0;
    if (r > 0) {
        cap_begin();
        int st = disk_stat(p, &e);
        const char *said_why = cap_end();
        if (st < 0)
            why = said_why ? said_why : "Not found";
        else if (e.kind == DISK_DIR && verb[0] == 'S')
            why = "A directory";
    } else if (!r) {
        if (!mlst)
            why = path_problem(0);
        e.kind = DISK_DIR;
        e.size = e.size_hi = 0;
        e.date = (1 << 5) | 1;
        e.time = 0;
    }
    if (why) {
        replyf(s, "550 %s", why, "");
        return;
    }
    char t[24];
    if (verb[0] == 'S') {
        replyf(s, "213 %s", u64_text(t, size_of(&e)), "");
    } else if (!mlst) {
        char d[24];
        usnprintf(d, sizeof(d), "%04u%02u%02u%02u%02u%02u", 1980 + (e.date >> 9), (e.date >> 5) & 15, e.date & 31,
                  e.time >> 11, (e.time >> 5) & 63, (e.time & 31) * 2);
        replyf(s, "213 %s", d, "");
    } else {
        static char b[520], t2[1100];
        scopy(e.name, v, DISK_ENTRY_NAME);
        int k = entry_line(b, sizeof(b) - 1, &e, 'M');
        if (k > 2 && k < (int)sizeof(b))
            b[k - 2] = 0;               /* (its CR LF: reply adds one) */
        usnprintf(t2, sizeof(t2), "250-Listing %s\r\n %s\r\n250 End", v, b);
        reply(s, t2);
    }
}

static void session_reset(struct session *s);

static void command(struct session *s, char *l)
{
    char verb[8];
    int i = 0;
    while (l[i] && l[i] != ' ' && i < 7) {
        verb[i] = (char)upper(l[i]);
        i++;
    }
    verb[i] = 0;
    while (l[i] && l[i] != ' ')
        i++;
    const char *a = l[i] ? l + i + 1 : "";
    u64 from = 0;
    if (!verb_is(verb, "REST")) {               /* (REST counts for the command right after it only) */
        from = s->rest;
        s->rest = 0;
    }
    char v[340], p[340];
    int moves = verb_is(verb, "LIST") || verb_is(verb, "NLST") || verb_is(verb, "MLSD") || verb_is(verb, "RETR")
                || verb_is(verb, "STOR") || verb_is(verb, "APPE");
    if ((verb_is(verb, "RETR") || verb_is(verb, "STOR") || verb_is(verb, "APPE")) && (http_disk_busy() ||
        smb_disk_busy())) {
        reply(s, "450 The box's web page or SMB server moves a file now: try again in a moment");
        return;
    }
    if (xfer && (xs == s ? !verb_is(verb, "ABOR") && !verb_is(verb, "STAT") && !verb_is(verb, "NOOP")
                               && !verb_is(verb, "QUIT")
                         : moves)
        && busy(s))
        return;
    if (verb_is(verb, "USER") || verb_is(verb, "PASS")) {
        reply(s, "230 Logged in: no password (every disk, read and write)");
    } else if (verb_is(verb, "SYST")) {
        reply(s, "215 UNIX Type: L8");
    } else if (verb_is(verb, "FEAT")) {
        reply(s, "211-Features:\r\n MDTM\r\n MLST type*;size*;modify*;\r\n SIZE\r\n REST STREAM\r\n EPSV\r\n"
                 "211 End");
    } else if (verb_is(verb, "OPTS")) {
        reply(s, "200 OK");
    } else if (verb_is(verb, "PWD") || verb_is(verb, "XPWD")) {
        replyf(s, "257 \"%s\" is the current directory", s->cwd, "");
    } else if (verb_is(verb, "CWD") || verb_is(verb, "XCWD") || verb_is(verb, "CDUP") || verb_is(verb, "XCUP")) {
        const char *to = verb[1] == 'D' || verb[2] == 'U' ? ".." : a;
        static struct disk_entry e;
        if (resolve(s, *to ? to : "/", v, sizeof(v)) < 0) {
            reply(s, "550 The path is too long");
            return;
        }
        int r = disk_path(v, p, sizeof(p));
        const char *why = r < 0 ? path_problem(r) : 0;
        if (r > 0) {
            cap_begin();
            int st = disk_stat(p, &e);
            why = cap_end();
            if (st == 0 && e.kind != DISK_DIR)
                why = "Not a directory";
            else if (st < 0 && !why)
                why = "No such directory";
        }
        if (why) {
            replyf(s, "550 %s", why, "");
            return;
        }
        scopy(s->cwd, v, sizeof(s->cwd));
        replyf(s, "250 The current directory is \"%s\"", s->cwd, "");
    } else if (verb_is(verb, "TYPE") || verb_is(verb, "MODE") || verb_is(verb, "STRU") || verb_is(verb, "ALLO")) {
        reply(s, "200 OK (everything is sent as it is: binary)");
    } else if (verb_is(verb, "PASV")) {
        passive(s, 0);
    } else if (verb_is(verb, "EPSV")) {
        passive(s, 1);
    } else if (verb_is(verb, "PORT")) {
        port(s, a, 0);
    } else if (verb_is(verb, "EPRT")) {
        port(s, a, 1);
    } else if (verb_is(verb, "LIST")) {
        listing(s, a, 'L');
    } else if (verb_is(verb, "NLST")) {
        listing(s, a, 'N');
    } else if (verb_is(verb, "MLSD")) {
        listing(s, a, 'M');
    } else if (verb_is(verb, "RETR")) {
        retr(s, a, from);
    } else if (verb_is(verb, "STOR")) {
        stor(s, a, 0, from);
    } else if (verb_is(verb, "APPE")) {
        stor(s, a, 1, 0);
    } else if (verb_is(verb, "REST")) {
        const char *q = a;
        u64 at = 0;
        int digits = 0;
        while (*q >= '0' && *q <= '9' && digits < 15) {
            at = at * 10 + (u64)(*q++ - '0');
            digits++;
        }
        if (!digits || *q) {
            reply(s, "501 REST <bytes>");
            return;
        }
        s->rest = at;
        char t[24];
        replyf(s, "350 Restarting at %s", u64_text(t, at), "");
    } else if (verb_is(verb, "SIZE") || verb_is(verb, "MDTM") || verb_is(verb, "MLST")) {
        info(s, verb, a);
    } else if (verb_is(verb, "DELE")) {
        change(s, disk_rm, a, "250 \"%s\" deleted");
    } else if (verb_is(verb, "MKD") || verb_is(verb, "XMKD")) {
        change(s, disk_mkdir, a, "257 \"%s\" created");
    } else if (verb_is(verb, "RMD") || verb_is(verb, "XRMD")) {
        change(s, disk_rmdir, a, "250 \"%s\" removed");
    } else if (verb_is(verb, "RNFR")) {
        int r = resolve(s, a, v, sizeof(v)) < 0 ? -2 : disk_path(v, s->rnfr, sizeof(s->rnfr));
        if (r <= 0) {
            s->rnfr[0] = 0;
            replyf(s, "550 %s", path_problem(r), "");
            return;
        }
        static struct disk_entry e;
        cap_begin();
        int st = disk_stat(s->rnfr, &e);
        const char *why = cap_end();
        if (st < 0) {
            s->rnfr[0] = 0;
            replyf(s, "550 %s", why ? why : "Not found", "");
            return;
        }
        reply(s, "350 Ready for RNTO");
    } else if (verb_is(verb, "RNTO")) {
        if (!s->rnfr[0]) {
            reply(s, "503 RNFR first");
            return;
        }
        int r = resolve(s, a, v, sizeof(v)) < 0 ? -2 : disk_path(v, p, sizeof(p));
        if (r <= 0) {
            replyf(s, "553 %s", path_problem(r), "");
            return;
        }
        cap_begin();
        disk_mv(s->rnfr, p);
        const char *why = cap_end();
        s->rnfr[0] = 0;
        if (why)
            replyf(s, "553 %s", why, "");
        else
            reply(s, "250 Renamed");
    } else if (verb_is(verb, "ABOR")) {
        if (xfer && xs == s) {
            xfer_end();
            data_drop(s, 1);
            reply(s, "426 Transfer aborted");
        }
        reply(s, "226 ABOR done");
    } else if (verb_is(verb, "STAT")) {
        char t[24];
        if (xfer && xs == s)
            replyf(s, "213 A transfer is under way: %s bytes so far", u64_text(t, moved), "");
        else
            reply(s, "211 The box's FTP server: no transfer of yours under way");
    } else if (verb_is(verb, "NOOP")) {
        reply(s, "200 OK");
    } else if (verb_is(verb, "HELP")) {
        reply(s, "214 USER PASS SYST FEAT OPTS PWD CWD CDUP TYPE PASV EPSV PORT EPRT LIST NLST MLSD MLST RETR "
                 "STOR APPE REST SIZE MDTM DELE MKD RMD RNFR RNTO ABOR STAT NOOP QUIT");
    } else if (verb_is(verb, "QUIT")) {
        reply(s, "221 Goodbye");
        session_reset(s);
    } else {
        replyf(s, "502 %s: not implemented", verb, "");
    }
}

/* the client's bytes: its command lines, one at a time (Telnet commands -- IAC IP before ABOR -- left out) */
static void control_input(struct session *s)
{
    u8 b[256];
    int n;
    while (s->fc >= 0 && (n = tcp_recv(s->fc, b, sizeof(b))) > 0) {
        s->heard = now_ms();
        for (int i = 0; i < n && s->fc >= 0; i++) {
            u8 c = b[i];
            if (s->in_iac) {
                s->in_iac = c >= 0xFB && c <= 0xFE ? 1 : 0;     /* (WILL / WONT / DO / DONT: its option too) */
                continue;
            }
            if (c == 0xFF) {
                s->in_iac = 1;
                continue;
            }
            if (c == '\n') {
                if (s->in_len && s->in[s->in_len - 1] == '\r')
                    s->in_len--;
                s->in[s->in_len] = 0;
                if (s->in_long)
                    reply(s, "500 The line is too long");
                else if (s->in_len)
                    command(s, s->in);
                s->in_len = s->in_long = 0;
            } else if (s->in_len < (int)sizeof(s->in) - 1) {
                s->in[s->in_len++] = (char)c;
            } else {
                s->in_long = 1;
            }
        }
    }
}

/* the session over: its transfer ended, its connections let go */
static void session_reset(struct session *s)
{
    if (xfer && xs == s)
        xfer_end();
    data_drop(s, 1);
    if (s->fc >= 0)
        tcp_close(s->fc);
    s->fc = -1;
}

static const char *session_end(struct session *s, const char *why)
{
    session_reset(s);
    usnprintf(line, sizeof(line), "ftp: %u.%u.%u.%u left (%s)", s->ip[0], s->ip[1], s->ip[2], s->ip[3], why);
    return line;
}

const char *ftp_poll(void)
{
    if (!started) {
        started = 1;
        tcp_listen(FTP_PORT);
        for (int i = 0; i < SESSIONS; i++) {
            sessions[i].fc = sessions[i].dc = -1;
            tcp_listen((u16)(DATA_PORT + i));
        }
    }
    const char *say = 0;
    int c = tcp_accept(FTP_PORT);
    if (c >= 0) {
        struct session *s = 0;
        for (int i = 0; i < SESSIONS && !s; i++)
            if (sessions[i].fc < 0)
                s = &sessions[i];
        u8 ip[4];
        u16 pt;
        tcp_peer(c, ip, &pt);
        if (!s) {
            static const char full[] = "421 The box's FTP server has two clients already\r\n";
            tcp_send(c, full, sizeof(full) - 1);
            tcp_close(c);
        } else {
            s->fc = c;
            s->dc = -1;
            for (int i = 0; i < 4; i++)
                s->ip[i] = ip[i];
            scopy(s->cwd, "/", sizeof(s->cwd));
            s->rnfr[0] = 0;
            s->rest = 0;
            s->mode = 0;
            s->in_len = s->in_long = s->in_iac = 0;
            s->heard = now_ms();
            reply(s, "220 ELEKTRODA ALI M3801 demo: FTP (no password; the root lists every disk)");
            usnprintf(line, sizeof(line), "ftp: %u.%u.%u.%u connected", ip[0], ip[1], ip[2], ip[3]);
            say = line;
        }
    }
    for (int i = 0; i < SESSIONS; i++) {
        struct session *s = &sessions[i];
        c = tcp_accept((u16)(DATA_PORT + i));   /* (passive: from its client's own address, when wanted) */
        if (c >= 0) {
            u8 ip[4];
            u16 pt;
            tcp_peer(c, ip, &pt);
            if (s->fc < 0 || s->mode != 1 || s->dc >= 0 || !same_ip(ip, s->ip))
                tcp_abort(c);
            else
                s->dc = c;
        }
        if (s->fc < 0 || say)
            continue;
        int st = tcp_status(s->fc);
        if (st == TCP_FAILED)
            return session_end(s, tcp_why(s->fc));
        if (st == TCP_DONE)
            return session_end(s, "closed");
        control_input(s);
        if (s->fc < 0) {
            usnprintf(line, sizeof(line), "ftp: %u.%u.%u.%u left (QUIT)", s->ip[0], s->ip[1], s->ip[2], s->ip[3]);
            return line;
        }
        if (st == TCP_PEER_DONE && !tcp_unread(s->fc) && !(xfer && xs == s))
            return session_end(s, "closed");
        if (!(xfer && xs == s) && now_ms() - s->heard > IDLE_MS) {
            reply(s, "421 No command for five minutes: goodbye");
            return session_end(s, "idle for five minutes");
        }
    }
    if (say)
        return say;
    return pump();
}

int ftp_disk_busy(void)
{
    return xfer == X_RETR || xfer == X_STOR;
}

void ftp_status(void)
{
    char t[24];
    int any = 0;
    for (int i = 0; i < SESSIONS; i++) {
        const struct session *s = &sessions[i];
        if (!started || s->fc < 0)
            continue;
        any = 1;
        uprintf("FTP client %u.%u.%u.%u (data port %u) in %s\n", s->ip[0], s->ip[1], s->ip[2], s->ip[3],
                DATA_PORT + i, s->cwd);
    }
    uprintf("FTP server (port %u%s): %s; %u files sent, %u received\n", FTP_PORT, any ? "" : ", passive 50021/2",
            !xfer ? "no transfer" : xfer == X_RETR ? "sending a file" : xfer == X_STOR ? "receiving a file"
                                                                       : "sending a listing",
            sent_files, got_files);
    if (xfer)
        uprintf("  %s: %s bytes so far\n", xfer_path, u64_text(t, moved));
}
