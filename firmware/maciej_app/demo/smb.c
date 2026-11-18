/* smb.c: an SMB2 file server on port 445 -- Windows' "\\192.168.0.161\4 PHONE CARD" (or just "\4"): every USB
   disk's volume a share, named as the FTP server's root names them (the number 'cd' takes, then the label), read
   and written as FTP does it (disk.c's reader and writer; FAT always, NTFS when Windows left it clean).  No
   password: whatever user and password Windows sends is taken, and the session is a guest's (a home network's
   server, as the FTP server and the web page).  'net view \\192.168.0.161' and Explorer list the shares
   (srvsvc's NetShareEnum over DCE/RPC on the IPC$ share's pipe).

   The protocol (MS-SMB2), as far as Windows' client needs it: NEGOTIATE (dialect 2.1, else 2.0.2; an SMB1
   negotiate offering "SMB 2.???" answered with SMB2's 0x02FF), SESSION_SETUP (SPNEGO with NTLMSSP only; the
   AUTHENTICATE message accepted unchecked: a guest session, so nothing is signed), TREE_CONNECT / DISCONNECT,
   CREATE (dispositions, file / directory options, delete on close; no oplocks, no create contexts), CLOSE, READ,
   WRITE, FLUSH, LOCK (granted), ECHO, QUERY_DIRECTORY (FileDirectory, FileFullDirectory, FileBothDirectory,
   FileNames, FileIdBothDirectory, FileIdFullDirectory information; wildcards), QUERY_INFO (the file and file
   system classes Explorer asks for; a security descriptor giving Everyone everything), SET_INFO (rename, delete,
   end of file and allocation -- 0 empties the file --; times and attributes taken and left), IOCTL (the pipe's
   transceive, FSCTL_VALIDATE_NEGOTIATE_INFO; DFS referrals answered "not found"), compounded and related
   requests.  CHANGE_NOTIFY is "not supported" (Explorer then refreshes by itself).

   Each message is read whole (its 4-byte NetBIOS length, then up to MSG_MAX bytes) into the client's buffer
   in RAM, its answer built whole and sent as the connection's send buffer takes it, in the main loop only
   (never inside a command: disk.c's buffers are shared).  Reads: disk.c's one reader, kept on a file across
   READs (a 4 KB piece and its place remembered: Windows reads in order) and let go after 2 s unused, so FTP,
   the web page and 'play' get it; writes: disk.c's one writer, from a file's CREATE (overwrite) or its first
   WRITE at 0 / at its end to its CLOSE.  Writes elsewhere in a file are refused (the writer only appends).  The
   reader and the writer may be on two files at once (a copy within the card), never on one.

   Tried from Windows 10 (2026-11-18): net view, dir, Copy-Item / robocopy both ways (byte-exact), Set-Content /
   Add-Content, rename and move, a copy within the share, delete, mkdir / rmdir -r; about 250 KB/s up and
   700 KB/s down.

   Known limits: names in ASCII only (disk.c's: a character it cannot keep reads '?'); one file written at a
   time (another gets "sharing violation"); guest sessions only -- Windows 10 Home / Pro allow them, while
   Windows 10/11 Enterprise and Education, and Windows 11 24H2 Pro, refuse guest logons and want signing; no
   oplocks / leases, no SMB 3. */
#include "hw.h"

#define SMB_PORT        445
#define CLIENTS         2
#define HANDLES         48
#define TREES           8
#define MSG_MAX         0x1F000u                /* a message (after its 4-byte header): a 64 KB WRITE and more */
#define IO_MAX          65536u                  /* MaxReadSize / MaxWriteSize / MaxTransactSize */
#define PIPES           4
#define PIPE_MAX        8192u
#define ENTRIES         ((int)(0x100000 / sizeof(struct disk_entry)))
#define IDLE_MS         600000                  /* a client silent for ten minutes is let go */
#define READER_IDLE_MS  2000                    /* disk.c's reader let go after this long unused */
#define DISK_PATH       95                      /* disk.c's longest path, its longest name */
#define DISK_NAME       127

#define RAM_IN(i)       (SMB_RAM_PHYS + 0x100000u + (u32)(i) * 0x40000u)
#define RAM_OUT(i)      (RAM_IN(i) + 0x20000u)
#define RAM_PIPE(i)     (SMB_RAM_PHYS + 0x180000u + (u32)(i) * PIPE_MAX)

/* status codes */
#define S_OK                    0x00000000u
#define S_BUFFER_OVERFLOW       0x80000005u
#define S_NO_MORE_FILES         0x80000006u
#define S_NOT_IMPLEMENTED       0xC0000002u
#define S_INVALID_INFO_CLASS    0xC0000003u
#define S_INFO_LENGTH_MISMATCH  0xC0000004u
#define S_INVALID_HANDLE        0xC0000008u
#define S_INVALID_PARAMETER     0xC000000Du
#define S_NO_SUCH_FILE          0xC000000Fu
#define S_INVALID_DEVICE_REQ    0xC0000010u
#define S_END_OF_FILE           0xC0000011u
#define S_MORE_PROCESSING       0xC0000016u
#define S_ACCESS_DENIED         0xC0000022u
#define S_NAME_INVALID          0xC0000033u
#define S_NAME_NOT_FOUND        0xC0000034u
#define S_NAME_COLLISION        0xC0000035u
#define S_PATH_NOT_FOUND        0xC000003Au
#define S_SHARING_VIOLATION     0xC0000043u
#define S_LOGON_FAILURE         0xC000006Du
#define S_DISK_FULL             0xC000007Fu
#define S_INSUFFICIENT_RES      0xC000009Au
#define S_WRITE_PROTECTED       0xC00000A2u
#define S_FILE_IS_A_DIRECTORY   0xC00000BAu
#define S_NOT_SUPPORTED         0xC00000BBu
#define S_NETWORK_NAME_DELETED  0xC00000C9u
#define S_BAD_NETWORK_NAME      0xC00000CCu
#define S_DIRECTORY_NOT_EMPTY   0xC0000101u
#define S_NOT_A_DIRECTORY       0xC0000103u
#define S_TOO_MANY_OPENED       0xC000011Fu
#define S_FILE_CLOSED           0xC0000128u
#define S_IO_DEVICE_ERROR       0xC0000185u
#define S_USER_SESSION_DELETED  0xC0000203u
#define S_NOT_FOUND             0xC0000225u

/* commands */
enum { C_NEGOTIATE, C_SESSION_SETUP, C_LOGOFF, C_TREE_CONNECT, C_TREE_DISCONNECT, C_CREATE, C_CLOSE, C_FLUSH,
       C_READ, C_WRITE, C_LOCK, C_IOCTL, C_CANCEL, C_ECHO, C_QUERY_DIRECTORY, C_CHANGE_NOTIFY, C_QUERY_INFO,
       C_SET_INFO, C_OPLOCK_BREAK };

#define F_REPLY         0x00000001u             /* SMB2_FLAGS_SERVER_TO_REDIR */
#define F_RELATED       0x00000004u

#define A_READONLY      0x01u
#define A_DIRECTORY     0x10u
#define A_ARCHIVE       0x20u
#define A_NORMAL        0x80u

struct tree {
    u32 id;
    u8 used, ipc;
    char vol[12];                       /* "4", "4.2" */
};

struct client {
    int c;                              /* the TCP connection (-1: none) */
    u8 ip[4];
    u8 *in, *out;
    u32 in_len, need;                   /* the message coming: bytes so far, its whole length (with its header) */
    u32 out_len, out_pos;               /* the answer going */
    u64 session;                        /* 0: none yet */
    u8 authed, anonymous;
    struct tree trees[TREES];
    u32 next_tree;
    u64 heard;
    u32 requests;
    char user[40];
    u16 dialect;                        /* NEGOTIATE's choice (FSCTL_VALIDATE_NEGOTIATE_INFO repeats it) */
};

static u8 server_guid[16];

enum { H_FILE = 1, H_DIR, H_PIPE };
struct handle {
    u8 used, kind, client;
    u8 delete_pending, writing, root;
    u32 tree;
    u64 id;                             /* the FileId's halves (both the same) */
    char path[DISK_PATH + 12];          /* disk.c's: "4:/DIR/x" */
    char rel[DISK_PATH + 12];           /* the share's: "\DIR\x" (FileNameInformation) */
    struct disk_entry e;                /* as it was when opened (size, times, attributes) */
    u64 size;
    u64 written;                        /* the bytes the writer has taken (writing) */
    u32 pos;                            /* QUERY_DIRECTORY: its next entry (0, 1: ".", "..") */
    u8 listed;                          /* that listing read */
    char pattern[64];
    u8 pipe;                            /* its answer buffer (RAM_PIPE) */
    u32 pipe_len;                       /* the answer waiting for a READ */
};

static struct client clients[CLIENTS];
static struct handle handles[HANDLES];
static u8 pipe_used[PIPES];
static u64 next_id = 1, next_session = 0x10001;
static int started;
static struct disk_entry *const ents = (struct disk_entry *)KSEG0(SMB_RAM_PHYS);
static int ents_n;
static u64 ents_owner;                  /* the handle whose directory the listing holds */
static u32 ents_changes;
/* disk.c's reader and writer while the SMB server has them */
static u64 reader_owner, writer_owner;  /* handle ids (0: none) */
static char reader_path[DISK_PATH + 12], writer_path[DISK_PATH + 12];   /* their files (one file is never both:
                                           the writer empties it; two files are -- a copy within the card) */
static u64 rd_pos, rd_size, piece_pos, reader_used;
static u32 piece_len, reader_opens;     /* (disk_stream_opens when it was opened: someone else's since if not) */
static u8 piece[4096];
static char said[200];
static char line[120];
static u32 n_reads, n_writes, n_connects;
static u64 bytes_out, bytes_in;
static u8 ntlm_challenge_bytes[8];

/* ---- small helpers ---- */
static u16 le16(const u8 *p)
{
    return (u16)(p[0] | p[1] << 8);
}

static u32 le32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static u64 le64(const u8 *p)
{
    return (u64)le32(p) | (u64)le32(p + 4) << 32;
}

static void put16(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static void put32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

static void put64(u8 *p, u64 v)
{
    put32(p, (u32)v);
    put32(p + 4, (u32)(v >> 32));
}

static void zero(u8 *p, u32 n)
{
    while (n--)
        *p++ = 0;
}

static void copy(u8 *to, const u8 *from, u32 n)
{
    while (n--)
        *to++ = *from++;
}

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

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static int same_nocase(const char *a, const char *b)
{
    while (*a && *b && lower(*a) == lower(*b)) {
        a++;
        b++;
    }
    return !*a && !*b;
}

/* UTF-16LE (n bytes) to ASCII: the characters' count, -1 one outside 32..126 or too long */
static int from16(const u8 *p, u32 n, char *out, int size)
{
    int k = 0;
    for (u32 i = 0; i + 1 < n; i += 2) {
        u16 ch = le16(p + i);
        if (!ch)
            break;
        if (ch < 32 || ch > 126 || k >= size - 1)
            return -1;
        out[k++] = (char)ch;
    }
    out[k] = 0;
    return k;
}

/* ASCII to UTF-16LE: the bytes written */
static u32 to16(u8 *out, const char *s)
{
    u32 n = 0;
    for (; *s; s++, n += 2)
        put16(out + n, (u8)*s);
    return n;
}

/* a FAT date / time as a FILETIME (100 ns since 1601; the box's clock is taken as UTC) */
static u64 filetime(u16 date, u16 time)
{
    static const u16 before[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    u32 y = 1980 + (date >> 9), m = (date >> 5) & 15, d = date & 31;
    if (m < 1 || m > 12)
        m = 1;
    if (d < 1)
        d = 1;
    u32 days = (y - 1980) * 365 + (y - 1977) / 4 + before[m - 1] + (m > 2 && !(y & 3) ? 1 : 0) + d - 1;
    u32 secs = (u32)(time >> 11) * 3600 + ((time >> 5) & 63) * 60 + (time & 31) * 2;
    return ((u64)(138426 + days) * 86400 + secs) * 10000000u;  /* (1601-01-01 to 1980-01-01: 138426 days) */
}

static u64 now_filetime(void)
{
    u32 utc = net_clock_utc();
    if (!utc)
        utc = 1735689600;               /* (no NTP yet: 2025-01-01) */
    return ((u64)utc + 11644473600ull) * 10000000u;
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

static int has(const char *s, const char *w)
{
    for (; *s; s++) {
        int i = 0;
        while (w[i] && lower(s[i]) == lower(w[i]))
            i++;
        if (!w[i])
            return 1;
    }
    return 0;
}

/* a disk call's complaint as a status */
static u32 status_of(const char *why)
{
    if (!why)
        return S_OK;
    if (has(why, "not empty"))
        return S_DIRECTORY_NOT_EMPTY;
    if (has(why, "full"))
        return S_DISK_FULL;
    if (has(why, "protect"))
        return S_WRITE_PROTECTED;
    if (has(why, "no such") || has(why, "not found") || has(why, "no file"))
        return S_NAME_NOT_FOUND;
    if (has(why, "exists") || has(why, "already"))
        return S_NAME_COLLISION;
    if (has(why, "read") && has(why, "fail"))
        return S_IO_DEVICE_ERROR;
    return S_ACCESS_DENIED;
}

/* ---- shares: the volumes (as the FTP server's root names them), IPC$ ---- */
static int volume_share(const struct disk_entry *e, char *name, int size)
{
    if (e->attr & 0x02)
        return 0;                       /* (its files are not read) */
    int j = 0, at = 0;
    while (e->name[j] && e->name[j] != ':' && at < 8 && at < size - 1)
        name[at++] = e->name[j++];
    const char *q = e->name + j;
    while (*q && *q != '\'')
        q++;
    if (*q == '\'' && at < size - 2) {
        name[at++] = ' ';
        for (q++; *q && *q != '\'' && at < size - 1; q++)
            name[at++] = *q == '/' || *q == '\\' ? '_' : *q;
    }
    name[at] = 0;
    return at > 0;
}

/* a share's name to its volume's number ("4 PHONE CARD" or "4" -> "4"): 1, 0 not a volume of a disk now */
static int share_volume(const char *share, char *vol, int size)
{
    int n = 0;
    while (((share[n] >= '0' && share[n] <= '9') || share[n] == '.') && n < size - 1) {
        vol[n] = share[n];
        n++;
    }
    vol[n] = 0;
    if (!n || (share[n] && share[n] != ' '))
        return 0;
    char root[16];
    usnprintf(root, sizeof(root), "%s:/", vol);
    struct disk_entry e;
    cap_begin();
    int st = disk_stat(root, &e);
    (void)cap_end();
    return st == 0;
}

/* ---- handles ---- */
static struct handle *handle_new(struct client *cl)
{
    for (int i = 0; i < HANDLES; i++)
        if (!handles[i].used) {
            struct handle *h = &handles[i];
            zero((u8 *)h, sizeof(*h));
            h->used = 1;
            h->client = (u8)(cl - clients);
            h->id = next_id++;
            return h;
        }
    return 0;
}

static struct handle *handle_find(struct client *cl, u64 id)
{
    for (int i = 0; i < HANDLES; i++)
        if (handles[i].used && handles[i].id == id && handles[i].client == (u8)(cl - clients))
            return &handles[i];
    return 0;
}

/* the reader let go (closed only when it is still ours: a command or the web page may have taken it) */
static void reader_release(void)
{
    if (reader_owner && disk_stream_opens() == reader_opens)
        disk_stream_close();
    reader_owner = 0;
    piece_len = 0;
}

/* a handle let go: its upload ended (what came is kept), its deletion done, the reader / listing let go */
static void handle_close(struct handle *h)
{
    if (!h->used)
        return;
    if (h->writing && writer_owner == h->id) {
        cap_begin();
        (void)disk_upload_close();
        (void)cap_end();
        writer_owner = 0;
    }
    if (reader_owner == h->id)
        reader_release();
    if (ents_owner == h->id)
        ents_owner = 0;
    if (h->delete_pending && h->kind != H_PIPE && !(writer_owner && same_nocase(writer_path, h->path))) {
        if (reader_owner && same_nocase(reader_path, h->path))
            reader_release();
        cap_begin();
        if (h->kind == H_DIR)
            disk_rmdir(h->path);
        else
            disk_rm(h->path);
        (void)cap_end();
    }
    if (h->kind == H_PIPE && h->pipe < PIPES)
        pipe_used[h->pipe] = 0;
    h->used = 0;
}

static void client_drop(struct client *cl, int abort)
{
    for (int i = 0; i < HANDLES; i++)
        if (handles[i].used && handles[i].client == (u8)(cl - clients))
            handle_close(&handles[i]);
    if (cl->c >= 0) {
        if (abort)
            tcp_abort(cl->c);
        else
            tcp_close(cl->c);
    }
    cl->c = -1;
    cl->session = 0;
    cl->authed = 0;
    for (int t = 0; t < TREES; t++)
        cl->trees[t].used = 0;
}

static struct tree *tree_find(struct client *cl, u32 id)
{
    for (int t = 0; t < TREES; t++)
        if (cl->trees[t].used && cl->trees[t].id == id)
            return &cl->trees[t];
    return 0;
}

/* ---- disk.c's reader and writer ---- */
static int others_busy(void)
{
    return ftp_disk_busy() || http_disk_busy();
}

int smb_disk_busy(void)
{
    return writer_owner != 0;
}

/* n bytes of a file from `at` into `to` (its end: fewer): the bytes, -1 not read, -2 the reader is someone
   else's now */
static int file_read_at(struct handle *h, u64 at, u8 *to, u32 n)
{
    if (reader_owner && disk_stream_opens() != reader_opens)
        reader_owner = 0;               /* (taken meanwhile: opened again) */
    if (reader_owner != h->id) {
        if ((writer_owner && same_nocase(writer_path, h->path)) || others_busy())
            return -2;
        reader_release();
        u64 size;
        cap_begin();
        int o = disk_stream_open(h->path, &size);
        (void)cap_end();
        if (o < 0)
            return -1;
        reader_owner = h->id;
        scopy(reader_path, h->path, sizeof(reader_path));
        reader_opens = disk_stream_opens();
        rd_pos = 0;
        rd_size = size;
        piece_len = 0;
    }
    reader_used = now_ms();
    u32 got = 0;
    while (got < n && at + got < rd_size) {
        u64 want = at + got;
        if (piece_len && want >= piece_pos && want < piece_pos + piece_len) {
            u32 off = (u32)(want - piece_pos), k = piece_len - off;
            if (k > n - got)
                k = n - got;
            copy(to + got, piece + off, k);
            got += k;
            continue;
        }
        if (want < rd_pos || want >= rd_pos + sizeof(piece)) {
            u64 p = disk_stream_seek(want);
            if (p > want)
                return -1;
            rd_pos = p;
        }
        cap_begin();
        int k = disk_stream_read(piece, sizeof(piece));
        (void)cap_end();
        if (k <= 0) {
            reader_release();
            return got ? (int)got : -1;
        }
        piece_pos = rd_pos;
        piece_len = (u32)k;
        rd_pos += (u32)k;
    }
    return (int)got;
}

/* the writer taken for a handle: its file emptied (append 0) or added to: a status */
static u32 writer_take(struct handle *h, int append)
{
    if (writer_owner && writer_owner != h->id)
        return S_SHARING_VIOLATION;
    if (others_busy())
        return S_SHARING_VIOLATION;
    if (reader_owner && same_nocase(reader_path, h->path))
        reader_release();
    cap_begin();
    int o = disk_upload_open(h->path, append);
    const char *why = cap_end();
    if (o < 0)
        return why ? status_of(why) : S_ACCESS_DENIED;
    writer_owner = h->id;
    scopy(writer_path, h->path, sizeof(writer_path));
    h->writing = 1;
    h->written = append ? h->size : 0;
    if (!append)
        h->size = 0;
    return S_OK;
}

/* ---- SPNEGO / NTLMSSP ---- */
static const u8 oid_ntlmssp[12] = {0x06, 0x0A, 0x2B, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x02, 0x0A};

/* a DER element: tag, length, `n` bytes of contents from `in` (in may lie inside out's room: moved): its size */
static u32 der(u8 *out, u8 tag, const u8 *in, u32 n)
{
    u32 h = n < 128 ? 2 : n < 256 ? 3 : 4;
    for (u32 i = n; i-- > 0;)
        out[h + i] = in[i];             /* (from the end: `in` may be out + something) */
    out[0] = tag;
    if (n < 128) {
        out[1] = (u8)n;
    } else if (n < 256) {
        out[1] = 0x81;
        out[2] = (u8)n;
    } else {
        out[1] = 0x82;
        out[2] = (u8)(n >> 8);
        out[3] = (u8)n;
    }
    return h + n;
}

/* NEGOTIATE's security buffer: NegTokenInit2 offering NTLMSSP only */
static u32 spnego_init(u8 *out)
{
    static const u8 spnego_oid[8] = {0x06, 0x06, 0x2B, 0x06, 0x01, 0x05, 0x05, 0x02};
    u8 t[64];
    u32 n = der(t, 0x30, oid_ntlmssp, sizeof(oid_ntlmssp));     /* mechTypes: SEQUENCE OF OID */
    n = der(t, 0xA0, t, n);
    n = der(t, 0x30, t, n);
    n = der(t, 0xA0, t, n);
    u8 u[80];
    copy(u, spnego_oid, sizeof(spnego_oid));
    copy(u + sizeof(spnego_oid), t, n);
    return der(out, 0x60, u, sizeof(spnego_oid) + n);
}

/* NegTokenResp: negState (0 accept-completed, 1 accept-incomplete), NTLMSSP named when `mech`, a token */
static u32 spnego_resp(u8 *out, int state, int mech, const u8 *token, u32 tn)
{
    static u8 t[1024];
    u32 n = 0;
    u8 st[3] = {0x0A, 0x01, (u8)state};
    n += der(t + n, 0xA0, st, 3);
    if (mech)
        n += der(t + n, 0xA1, oid_ntlmssp, sizeof(oid_ntlmssp));
    if (token && tn + 8 < sizeof(t) - n) {
        u32 k = der(t + n, 0x04, token, tn);
        n += der(t + n, 0xA2, t + n, k);
    }
    u32 m = der(t, 0x30, t, n);
    return der(out, 0xA1, t, m);
}

/* the NTLMSSP CHALLENGE message (answering a NEGOTIATE with these flags): its size */
static u32 ntlm_challenge(u8 *p, u32 cflags)
{
    static const char *const name = "M3801", *const domain = "WORKGROUP";
    /* UNICODE, NTLM, TARGET_TYPE_SERVER, TARGET_INFO, and what the client asked for of 56/KEY_EXCH/128/VERSION/
       EXTENDED_SESSIONSECURITY/ALWAYS_SIGN/LM_KEY/SEAL/SIGN/REQUEST_TARGET/UNICODE -- LM_KEY not with extended
       session security (MS-NLMP: the latter alone is returned) */
    u32 flags = (cflags & 0xE20882B5u) | 0x00000001u | 0x00000200u | 0x00020000u | 0x00800000u;
    if (flags & 0x00080000u)
        flags &= ~0x00000080u;
    zero(p, 56);
    copy(p, (const u8 *)"NTLMSSP", 8);
    put32(p + 8, 2);
    u32 at = 56;
    u32 tn = to16(p + at, name);        /* TargetName: the server's name (TARGET_TYPE_SERVER) */
    put16(p + 12, tn);
    put16(p + 14, tn);
    put32(p + 16, at);
    at += tn;
    put32(p + 20, flags);
    u64 t = now_ticks();
    for (int i = 0; i < 8; i++)
        ntlm_challenge_bytes[i] = (u8)(t >> (8 * (i & 3))) ^ (u8)(i * 37 + (u8)(t >> 40));
    copy(p + 24, ntlm_challenge_bytes, 8);
    u32 info = at;                      /* TargetInfo: AV pairs */
    static const u16 ids[4] = {2, 1, 4, 3};
    const char *vals[4] = {domain, name, "", "m3801"};
    for (int i = 0; i < 4; i++) {
        put16(p + at, ids[i]);
        u32 k = to16(p + at + 4, vals[i]);
        put16(p + at + 2, k);
        at += 4 + k;
    }
    put32(p + at, 0);                   /* MsvAvEOL */
    at += 4;
    put16(p + 40, at - info);
    put16(p + 42, at - info);
    put32(p + 44, info);
    static const u8 version[8] = {6, 1, 0xB1, 0x1D, 0, 0, 0, 0x0F};
    copy(p + 48, version, 8);
    return at;
}

/* ---- the answer being built ---- */
static u8 *rb;                          /* the current answer's body (after its 64-byte header) */
static u32 rb_len;

static u8 *body_room(u32 n)
{
    u8 *p = rb + rb_len;
    zero(p, n);
    rb_len += n;
    return p;
}

/* ---- commands ---- */
static u32 do_negotiate(struct client *cl, const u8 *b, u32 n)
{
    if (n < 36)
        return S_INVALID_PARAMETER;
    u32 count = le16(b + 2), dialect = 0;
    for (u32 i = 0; i < count && 36 + 2 * i + 2 <= n; i++) {
        u32 d = le16(b + 36 + 2 * i);
        if (d == 0x0210 || (d == 0x0202 && dialect != 0x0210))
            dialect = d;
    }
    if (!dialect)
        return S_NOT_SUPPORTED;
    cl->dialect = (u16)dialect;
    u8 *r = body_room(64);
    put16(r, 65);
    put16(r + 2, 0x0001);               /* signing enabled, not required */
    put16(r + 4, dialect);
    for (int i = 0; i < 16; i++)
        server_guid[i] = (u8)(0xA5 ^ (i * 29));
    copy(r + 8, server_guid, 16);
    put32(r + 24, 0);                   /* no DFS, leases, large MTU */
    put32(r + 28, IO_MAX);
    put32(r + 32, IO_MAX);
    put32(r + 36, IO_MAX);
    put64(r + 40, now_filetime());
    put64(r + 48, 0);
    u32 sl = spnego_init(rb + rb_len);
    rb_len += sl;
    put16(r + 56, 64 + 64);
    put16(r + 58, sl);
    return S_OK;
}

/* SMB1's NEGOTIATE (a client that starts with it): answered with SMB2's, dialect 0x02FF ("SMB 2.???") or 0x0202 */
static int smb1_upgrade(const u8 *m, u32 n, u32 *dialect)
{
    if (n < 35 || m[4] != 0x72)
        return 0;
    u32 bc = le16(m + 33), at = 35;
    *dialect = 0;
    while (at < 35 + bc && at < n) {
        if (m[at] != 0x02)
            break;
        const char *s = (const char *)m + at + 1;
        if (same_nocase(s, "SMB 2.???"))
            *dialect = 0x02FF;
        else if (same_nocase(s, "SMB 2.002") && !*dialect)
            *dialect = 0x0202;
        at += 2 + (u32)slen(s);
    }
    return *dialect != 0;
}

static u32 do_session_setup(struct client *cl, const u8 *h, const u8 *b, u32 n, u64 *sid, u16 *sflags)
{
    if (n < 24)
        return S_INVALID_PARAMETER;
    u32 off = le16(b + 12), len = le16(b + 14);
    const u8 *sec = h + off;
    if (off < 64 + 24 || off + len > 64 + n)
        return S_INVALID_PARAMETER;
    const u8 *ntlm = 0;
    for (u32 i = 0; i + 12 <= len; i++)
        if (sec[i] == 'N' && sec[i + 1] == 'T' && sec[i + 2] == 'L' && sec[i + 3] == 'M' && sec[i + 4] == 'S' &&
            sec[i + 5] == 'S' && sec[i + 6] == 'P' && !sec[i + 7]) {
            ntlm = sec + i;
            break;
        }
    u8 *r = body_room(8);
    put16(r, 9);
    put16(r + 4, 64 + 8);
    if (!ntlm) {                        /* a token of another mechanism first: NTLMSSP named, its turn next */
        if (!*sid)
            *sid = next_session++;
        cl->session = *sid;
        cl->authed = 0;
        u32 sl = spnego_resp(rb + rb_len, 1, 1, 0, 0);
        rb_len += sl;
        put16(r + 6, sl);
        return S_MORE_PROCESSING;
    }
    u32 type = le32(ntlm + 8), rest = len - (u32)(ntlm - sec);
    if (type == 1) {                    /* NEGOTIATE -> CHALLENGE */
        if (!*sid)
            *sid = next_session++;
        cl->session = *sid;
        cl->authed = 0;
        static u8 ch[400];
        u32 cn = ntlm_challenge(ch, rest >= 16 ? le32(ntlm + 12) : 0);
        u32 sl = spnego_resp(rb + rb_len, 1, 1, ch, cn);
        rb_len += sl;
        put16(r + 6, sl);
        return S_MORE_PROCESSING;
    }
    if (type != 3 || rest < 52 || *sid != cl->session || !cl->session)
        return S_LOGON_FAILURE;
    /* AUTHENTICATE: taken as it is (the user's name kept for 'netstat') */
    u32 nt_len = le16(ntlm + 20), user_len = le16(ntlm + 36), user_off = le32(ntlm + 40);
    cl->user[0] = 0;
    if (user_off + user_len <= rest && user_len)
        (void)from16(ntlm + user_off, user_len, cl->user, sizeof(cl->user));
    cl->anonymous = !user_len && nt_len <= 1;
    cl->authed = 1;
    /* IS_NULL for an anonymous one, IS_GUEST for everybody else: the password is never checked, so there's no
       session key to sign with, and a session that isn't a guest's must sign (Windows logged off and went when
       FSCTL_VALIDATE_NEGOTIATE_INFO came back unsigned) */
    *sflags = cl->anonymous ? 0x0002 : 0x0001;
    u32 sl = spnego_resp(rb + rb_len, 0, 0, 0, 0);
    rb_len += sl;
    put16(r + 6, sl);
    return S_OK;
}

static u32 do_tree_connect(struct client *cl, const u8 *h, const u8 *b, u32 n, u32 *tid)
{
    if (n < 8)
        return S_INVALID_PARAMETER;
    u32 off = le16(b + 4), len = le16(b + 6);
    if (off < 64 || off + len > 64 + n)
        return S_INVALID_PARAMETER;
    char path[160];
    if (from16(h + off, len, path, sizeof(path)) < 0)
        return S_BAD_NETWORK_NAME;
    const char *share = path;
    for (const char *p = path; *p; p++)
        if (*p == '\\' || *p == '/')
            share = p + 1;
    struct tree t;
    zero((u8 *)&t, sizeof(t));
    if (same_nocase(share, "IPC$"))
        t.ipc = 1;
    else if (!share_volume(share, t.vol, sizeof(t.vol)))
        return S_BAD_NETWORK_NAME;
    int k = 0;
    while (k < TREES && cl->trees[k].used)
        k++;
    if (k == TREES)
        return S_INSUFFICIENT_RES;
    t.used = 1;
    t.id = ++cl->next_tree;
    cl->trees[k] = t;
    *tid = t.id;
    u8 *r = body_room(16);
    put16(r, 16);
    r[2] = t.ipc ? 2 : 1;               /* ShareType: pipe, disk */
    put32(r + 4, 0x00000030);           /* SMB2_SHAREFLAG_NO_CACHING: no offline copies */
    put32(r + 8, 0);
    put32(r + 12, 0x001F01FF);          /* MaximalAccess: everything */
    return S_OK;
}

/* the share-relative name ("DIR\x", UTF-16) of a CREATE as disk.c's path and the share's: 0, a status */
static u32 make_path(const struct tree *t, const u8 *name, u32 len, struct handle *h)
{
    char rel[DISK_PATH + 8];
    int n = from16(name, len, rel, sizeof(rel));
    if (n < 0)
        return S_NAME_INVALID;
    /* a stream: only the file's own ("x::$DATA") */
    for (int i = 0; rel[i]; i++)
        if (rel[i] == ':') {
            if (!same_nocase(rel + i, "::$DATA"))
                return S_NAME_NOT_FOUND;
            rel[i] = 0;
            break;
        }
    int part = 0, at = 0;
    char *p = h->path;
    at = usnprintf(p, (int)sizeof(h->path), "%s:", t->vol);
    h->rel[0] = '\\';
    int ra = 1;
    const char *s = rel;
    while (*s == '\\' || *s == '/')
        s++;
    p[at++] = '/';
    for (; *s; s++) {
        char c = *s == '\\' ? '/' : *s;
        if (c == '*' || c == '?' || c == '<' || c == '>' || c == '"' || c == '|')
            return S_NAME_INVALID;
        if (c == '/' && (at > 0 && p[at - 1] == '/'))
            continue;
        part = c == '/' ? 0 : part + 1;
        if (part > DISK_NAME || at >= (int)sizeof(h->path) - 2 || ra >= (int)sizeof(h->rel) - 2)
            return S_NAME_INVALID;
        p[at++] = c;
        h->rel[ra++] = c == '/' ? '\\' : c;
    }
    while (at > 3 && p[at - 1] == '/')  /* ("4:/DIR/" -> "4:/DIR") */
        at--;
    p[at] = 0;
    while (ra > 1 && h->rel[ra - 1] == '\\')
        ra--;
    h->rel[ra] = 0;
    if (at - slen(t->vol) - 1 > DISK_PATH)
        return S_NAME_INVALID;
    h->root = at == slen(t->vol) + 2;
    return S_OK;
}

/* a handle's times / sizes / attributes into a 4-time block and the rest as CREATE / CLOSE / network-open
   information lay them out: creation, access, write, change, allocation, end of file, attributes */
static u32 attrs_of(const struct handle *h)
{
    if (h->kind == H_DIR)
        return A_DIRECTORY | (h->e.attr & 0x06);
    u32 a = h->e.attr & 0x27;
    return a ? a : A_NORMAL;
}

static u64 alloc_of(u64 size)
{
    return (size + 4095) & ~(u64)4095;
}

static void put_times(u8 *p, const struct handle *h)
{
    u64 t = h->root ? filetime((1 << 5) | 1, 0) : filetime(h->e.date, h->e.time);
    put64(p, t);
    put64(p + 8, t);
    put64(p + 16, t);
    put64(p + 24, t);
}

static u32 do_create(struct client *cl, struct tree *t, const u8 *h, const u8 *b, u32 n, u64 *fid)
{
    if (n < 56)
        return S_INVALID_PARAMETER;
    u32 disp = le32(b + 36), options = le32(b + 40), off = le16(b + 44), len = le16(b + 46);
    if (len && (off < 64 || off + len > 64 + n))
        return S_INVALID_PARAMETER;
    struct handle *f = handle_new(cl);
    if (!f)
        return S_TOO_MANY_OPENED;
    f->tree = t->id;
    u32 action = 1;                     /* FILE_OPENED */
    if (t->ipc) {                       /* the named pipes: srvsvc only */
        char name[40];
        if (from16(h + off, len, name, sizeof(name)) < 0 ||
            !(same_nocase(name, "srvsvc") || same_nocase(name, "\\srvsvc") || same_nocase(name, "\\PIPE\\srvsvc"))) {
            f->used = 0;
            return S_NAME_NOT_FOUND;
        }
        int p = 0;
        while (p < PIPES && pipe_used[p])
            p++;
        if (p == PIPES) {
            f->used = 0;
            return S_INSUFFICIENT_RES;
        }
        pipe_used[p] = 1;
        f->kind = H_PIPE;
        f->pipe = (u8)p;
        f->e.attr = 0;
    } else {
        u32 st = make_path(t, h + off, len, f);
        if (st) {
            f->used = 0;
            return st;
        }
        cap_begin();
        int there = disk_stat(f->path, &f->e) == 0;
        (void)cap_end();
        int is_dir = there && f->e.kind != DISK_FILE;
        if (there && (disp == 2)) {                         /* FILE_CREATE */
            f->used = 0;
            return S_NAME_COLLISION;
        }
        if (there && is_dir && (options & 0x40)) {          /* FILE_NON_DIRECTORY_FILE */
            f->used = 0;
            return S_FILE_IS_A_DIRECTORY;
        }
        if (there && !is_dir && (options & 0x01)) {         /* FILE_DIRECTORY_FILE */
            f->used = 0;
            return S_NOT_A_DIRECTORY;
        }
        if (!there && (disp == 1 || disp == 4)) {           /* FILE_OPEN, FILE_OVERWRITE */
            f->used = 0;
            return S_NAME_NOT_FOUND;
        }
        if (!there) {                                       /* made: a directory, or a file (the writer) */
            if (f->root) {
                f->used = 0;
                return S_ACCESS_DENIED;
            }
            if (options & 0x01) {
                cap_begin();
                disk_mkdir(f->path);
                const char *why = cap_end();
                if (why) {
                    f->used = 0;
                    return has(why, "no such") ? S_PATH_NOT_FOUND : status_of(why);
                }
                f->kind = H_DIR;
            } else {
                f->kind = H_FILE;
                st = writer_take(f, 0);
                if (st) {
                    f->used = 0;
                    return st == S_NAME_NOT_FOUND ? S_PATH_NOT_FOUND : st;
                }
            }
            cap_begin();
            (void)disk_stat(f->path, &f->e);
            (void)cap_end();
            if (f->kind == H_DIR)
                f->e.kind = DISK_DIR;
            action = 2;                 /* FILE_CREATED */
        } else {
            f->kind = is_dir ? H_DIR : H_FILE;
            f->size = (u64)f->e.size_hi << 32 | f->e.size;
            if (!is_dir && (disp == 0 || disp == 4 || disp == 5)) {    /* superseded / overwritten: emptied */
                st = writer_take(f, 0);
                if (st) {
                    f->used = 0;
                    return st;
                }
                action = disp == 0 ? 0 : 3;
            }
        }
        if (options & 0x1000) {         /* FILE_DELETE_ON_CLOSE (not while another handle writes it) */
            if (!f->writing && writer_owner && same_nocase(writer_path, f->path)) {
                f->used = 0;
                return S_SHARING_VIOLATION;
            }
            f->delete_pending = 1;
        }
    }
    *fid = f->id;
    u8 *r = body_room(89);
    put16(r, 89);
    put32(r + 4, action);
    if (f->kind != H_PIPE) {
        put_times(r + 8, f);
        put64(r + 40, f->kind == H_FILE ? alloc_of(f->size) : 0);
        put64(r + 48, f->kind == H_FILE ? f->size : 0);
        put32(r + 56, attrs_of(f));
    } else {
        put32(r + 56, A_NORMAL);
    }
    put64(r + 64, f->id);
    put64(r + 72, f->id);
    return S_OK;
}

static u32 do_close(struct client *cl, const u8 *b, u32 n, u64 fid)
{
    if (n < 24)
        return S_INVALID_PARAMETER;
    struct handle *f = handle_find(cl, fid);
    if (!f)
        return S_FILE_CLOSED;
    u32 flags = le16(b + 2);
    u8 *r = body_room(60);
    put16(r, 60);
    if ((flags & 1) && f->kind != H_PIPE) {     /* SMB2_CLOSE_FLAG_POSTQUERY_ATTRIB */
        if (f->writing)
            f->size = f->written;
        put16(r + 2, 1);
        put_times(r + 8, f);
        put64(r + 40, f->kind == H_FILE ? alloc_of(f->size) : 0);
        put64(r + 48, f->kind == H_FILE ? f->size : 0);
        put32(r + 56, attrs_of(f));
    }
    handle_close(f);
    return S_OK;
}

/* ---- srvsvc over DCE/RPC (the IPC$ pipe): the shares for 'net view' and Explorer ---- */
static const u8 uuid_srvsvc[20] = {0xC8, 0x4F, 0x32, 0x4B, 0x70, 0x16, 0xD3, 0x01, 0x12, 0x78, 0x5A, 0x47, 0xBF,
                                   0x6E, 0xE1, 0x88, 3, 0, 0, 0};
static const u8 uuid_ndr[20] = {0x04, 0x5D, 0x88, 0x8A, 0xEB, 0x1C, 0xC9, 0x11, 0x9F, 0xE8, 0x08, 0x00, 0x2B, 0x10,
                                0x48, 0x60, 2, 0, 0, 0};

static int same_bytes(const u8 *a, const u8 *b, u32 n)
{
    while (n--)
        if (*a++ != *b++)
            return 0;
    return 1;
}

/* NDR: a conformant varying string (with its NUL), padded to 4 */
static u32 ndr_string(u8 *p, const char *s)
{
    u32 n = (u32)slen(s) + 1;
    put32(p, n);
    put32(p + 4, 0);
    put32(p + 8, n);
    u32 at = 12;
    for (u32 i = 0; i < n; i++, at += 2)
        put16(p + at, i + 1 < n ? (u8)s[i] : 0);
    while (at & 3)
        p[at++] = 0;
    return at;
}

struct share {
    char name[40], remark[48];
    u32 type;
};

static int shares_list(struct share *sh, int max)
{
    static struct disk_entry v[16];
    int n = disk_volumes(v, 16), k = 0;
    for (int i = 0; i < n && k < max - 1; i++) {
        if (!volume_share(&v[i], sh[k].name, sizeof(sh[k].name)))
            continue;
        /* the remark: "FAT32, 30 GB" from "4:  FAT32 'PHONE CARD', 30 GB" */
        const char *q = v[i].name;
        while (*q && *q != ' ')
            q++;
        while (*q == ' ')
            q++;
        int at = 0;
        for (; *q && *q != ' ' && *q != '\'' && at < 10; q++)
            sh[k].remark[at++] = *q;
        sh[k].remark[at] = 0;
        sh[k].type = 0;                 /* STYPE_DISKTREE */
        k++;
    }
    scopy(sh[k].name, "IPC$", sizeof(sh[k].name));
    scopy(sh[k].remark, "IPC", sizeof(sh[k].remark));
    sh[k].type = 0x80000003u;           /* STYPE_IPC | STYPE_SPECIAL */
    return k + 1;
}

/* a request's stub for opnum 15 (NetrShareEnum) / 16 (NetrShareGetInfo) answered: the response stub's length */
static u32 rpc_shares(u32 opnum, const u8 *in, u32 n, u8 *out)
{
    static struct share sh[17];
    int count = shares_list(sh, 17);
    u32 at = 0, ref = 0x00020000;
    /* the request: ServerName (a unique pointer to a string) first */
    u32 r = 0;
    if (n >= 4) {
        u32 ptr = le32(in);
        r = 4;
        if (ptr && n >= r + 12)
            r += 12 + ((le32(in + r + 8) * 2 + 3) & ~3u);
    }
    if (opnum == 15) {
        u32 level = n >= r + 4 ? le32(in + r) : 1;
        if (level != 0 && level != 1)
            level = 1;
        put32(out + at, level);
        put32(out + at + 4, level);
        put32(out + at + 8, ref);
        ref += 4;
        put32(out + at + 12, (u32)count);
        put32(out + at + 16, ref);
        ref += 4;
        put32(out + at + 20, (u32)count);
        at += 24;
        for (int i = 0; i < count; i++) {
            put32(out + at, ref);
            ref += 4;
            at += 4;
            if (level == 1) {
                put32(out + at, sh[i].type);
                put32(out + at + 4, ref);
                ref += 4;
                at += 8;
            }
        }
        for (int i = 0; i < count; i++) {
            at += ndr_string(out + at, sh[i].name);
            if (level == 1)
                at += ndr_string(out + at, sh[i].remark);
        }
        put32(out + at, (u32)count);    /* TotalEntries */
        put32(out + at + 4, ref);       /* ResumeHandle: a pointer, 0 */
        put32(out + at + 8, 0);
        put32(out + at + 12, 0);        /* WERROR_OK */
        return at + 16;
    }
    /* 16: NetName (a string, not a pointer), Level */
    char want[40];
    want[0] = 0;
    if (n >= r + 12) {
        u32 len = le32(in + r + 8);
        (void)from16(in + r + 12, len * 2, want, sizeof(want));
        r += 12 + ((len * 2 + 3) & ~3u);
    }
    u32 level = n >= r + 4 ? le32(in + r) : 1;
    int k = -1;
    for (int i = 0; i < count; i++)
        if (same_nocase(sh[i].name, want) ||
            (sh[i].type == 0 && want[0] >= '0' && want[0] <= '9' && sh[i].name[0] == want[0] &&
             (sh[i].name[1] == ' ' || !sh[i].name[1]) && !want[1]))
            k = i;
    if (k < 0 || (level != 0 && level != 1 && level != 501 && level != 1005)) {
        put32(out, level);
        put32(out + 4, 0);              /* (a null pointer) */
        put32(out + 8, k < 0 ? 2310u : 124u);   /* NERR_NetNameNotFound, ERROR_INVALID_LEVEL */
        return 12;
    }
    put32(out + at, level);
    put32(out + at + 4, ref);
    ref += 4;
    at += 8;
    if (level == 1005) {
        put32(out + at, 0x0030);        /* shi1005_flags: CSC_CACHE_NONE */
        at += 4;
    } else {
        put32(out + at, ref);
        ref += 4;
        at += 4;
        if (level >= 1) {
            put32(out + at, sh[k].type);
            put32(out + at + 4, ref);
            ref += 4;
            at += 8;
            if (level == 501) {
                put32(out + at, 0x0030);
                at += 4;
            }
        }
        at += ndr_string(out + at, sh[k].name);
        if (level >= 1)
            at += ndr_string(out + at, sh[k].remark);
    }
    put32(out + at, 0);                 /* WERROR_OK */
    return at + 4;
}

/* a DCE/RPC PDU written to the pipe: its answer into `out`: the answer's length (0 none) */
static u32 rpc(const u8 *in, u32 n, u8 *out, u32 room)
{
    if (n < 16 || in[0] != 5 || room < 512)
        return 0;
    u32 type = in[2], call = le32(in + 12);
    zero(out, 16);
    out[0] = 5;
    out[3] = 0x03;                      /* first and last fragment */
    out[4] = 0x10;                      /* little-endian, ASCII, IEEE */
    put32(out + 12, call);
    u32 at = 16;
    if (type == 11 || type == 14) {     /* bind / alter context */
        out[2] = type == 11 ? 12 : 15;
        put16(out + 16, 4280);
        put16(out + 18, 4280);
        put32(out + 20, 0x53F0);
        at = 24;
        if (type == 11) {
            static const char port[] = "\\PIPE\\srvsvc";
            put16(out + at, sizeof(port));
            copy(out + at + 2, (const u8 *)port, sizeof(port));
            at += 2 + sizeof(port);
        } else {
            put16(out + at, 0);
            at += 2;
        }
        while (at & 3)
            out[at++] = 0;
        u32 contexts = n > 24 ? in[24] : 0, ci = 28;
        out[at] = (u8)contexts;
        at += 4;
        for (u32 c = 0; c < contexts && ci + 24 <= n && at + 24 <= room; c++) {
            u32 nts = in[ci + 2];
            const u8 *abstract = in + ci + 4;
            int ok = same_bytes(abstract, uuid_srvsvc, 20), ndr = 0, feature = 0;
            for (u32 t = 0; t < nts && ci + 24 + 20 * t + 20 <= n; t++) {
                const u8 *ts = in + ci + 24 + 20 * t;
                if (same_bytes(ts, uuid_ndr, 20))
                    ndr = 1;
                if (ts[0] == 0x2C && ts[1] == 0x1C && ts[2] == 0xB7 && ts[3] == 0x6C)
                    feature = 1;        /* (bind time feature negotiation) */
            }
            zero(out + at, 24);
            if (ok && ndr) {
                copy(out + at + 4, uuid_ndr, 20);
            } else if (feature) {
                put16(out + at, 3);     /* negotiate_ack */
            } else {
                put16(out + at, 2);     /* provider rejection */
                put16(out + at + 2, 2); /* proposed transfer syntaxes not supported */
            }
            at += 24;
            ci += 24 + 20 * nts;
        }
    } else if (type == 0) {             /* request */
        u32 opnum = n >= 24 ? le16(in + 22) : 0;
        if (opnum == 15 || opnum == 16) {
            out[2] = 2;                 /* response */
            u32 len = rpc_shares(opnum, in + 24, n - 24, out + 24);
            put32(out + 16, len);
            put16(out + 20, n >= 24 ? le16(in + 20) : 0);
            at = 24 + len;
        } else {
            out[2] = 3;                 /* fault: nca_s_op_rng_error */
            put32(out + 16, 0);
            put32(out + 24, 0x1C010002u);
            put32(out + 28, 0);
            at = 32;
        }
    } else {
        return 0;
    }
    put16(out + 8, at);
    return at;
}

/* ---- READ / WRITE ---- */
static u32 do_read(struct client *cl, const u8 *b, u32 n, u64 fid)
{
    if (n < 48)
        return S_INVALID_PARAMETER;
    u32 len = le32(b + 4);
    u64 at = le64(b + 8);
    struct handle *f = handle_find(cl, fid);
    if (!f)
        return S_FILE_CLOSED;
    if (len > IO_MAX)
        len = IO_MAX;
    u8 *r = body_room(16);
    put16(r, 17);
    r[2] = 64 + 16;
    u8 *data = rb + rb_len;
    int got;
    if (f->kind == H_PIPE) {
        u8 *pb = (u8 *)KSEG0(RAM_PIPE(f->pipe));
        got = (int)(f->pipe_len < len ? f->pipe_len : len);
        copy(data, pb, (u32)got);
        for (u32 i = (u32)got; i < f->pipe_len; i++)
            pb[i - (u32)got] = pb[i];
        f->pipe_len -= (u32)got;
        if (!got) {
            rb_len -= 16;
            return S_END_OF_FILE;
        }
    } else {
        if (f->kind == H_DIR) {
            rb_len -= 16;
            return S_INVALID_DEVICE_REQ;
        }
        /* (a file being written can't be read back -- the writer has it -- except past what's written: its end.
           PowerShell's Set-Content reads the file it has just made, to see its encoding) */
        if (f->writing ? at >= f->written : at >= f->size) {
            rb_len -= 16;
            return S_END_OF_FILE;
        }
        if (f->writing) {
            rb_len -= 16;
            return S_SHARING_VIOLATION;
        }
        got = file_read_at(f, at, data, len);
        if (got < 0) {
            rb_len -= 16;
            return got == -2 ? S_SHARING_VIOLATION : S_IO_DEVICE_ERROR;
        }
        if (!got) {
            rb_len -= 16;
            return S_END_OF_FILE;
        }
        n_reads++;
        bytes_out += (u32)got;
    }
    put32(r + 4, (u32)got);
    rb_len += (u32)got;
    return S_OK;
}

static u32 do_write(struct client *cl, const u8 *h, const u8 *b, u32 n, u64 fid)
{
    if (n < 48)
        return S_INVALID_PARAMETER;
    u32 off = le16(b + 2), len = le32(b + 4);
    u64 at = le64(b + 8);
    if (off < 64 || len > n || off - 64 > n - len)
        return S_INVALID_PARAMETER;     /* (len is 32 bits: off + len could wrap and pass) */
    struct handle *f = handle_find(cl, fid);
    if (!f)
        return S_FILE_CLOSED;
    if (f->kind == H_PIPE) {
        u8 *pb = (u8 *)KSEG0(RAM_PIPE(f->pipe));
        f->pipe_len = rpc(h + off, len, pb, PIPE_MAX);
    } else {
        if (f->kind == H_DIR)
            return S_INVALID_DEVICE_REQ;
        if (!f->writing) {              /* its first WRITE: at 0 (rewritten from the start) or at its end */
            if (at != 0 && at != f->size)
                return S_NOT_SUPPORTED;
            u32 st = writer_take(f, at != 0);
            if (st)
                return st;
        }
        if (at != f->written)
            return S_NOT_SUPPORTED;     /* (only in order: the writer appends) */
        cap_begin();
        int w = disk_upload_put(h + off, len);
        const char *why = cap_end();
        if (w < 0) {                    /* (the writer let the file go: its clusters freed) */
            f->writing = 0;
            writer_owner = 0;
            return why ? status_of(why) : S_IO_DEVICE_ERROR;
        }
        f->written += len;
        f->size = f->written;
        n_writes++;
        bytes_in += len;
    }
    u8 *r = body_room(17);
    put16(r, 17);
    put32(r + 4, len);
    return S_OK;
}

/* ---- QUERY_DIRECTORY ---- */
static int wild(const char *p, const char *s)
{
    for (; *p; p++, s++) {
        if (*p == '*' || *p == '<') {
            while (p[1] == '*' || p[1] == '<')
                p++;
            if (!p[1])
                return 1;
            for (; *s; s++)
                if (wild(p + 1, s))
                    return 1;
            return wild(p + 1, s);
        }
        if (!*s)
            return (*p == '"' || *p == '>') && !p[1];
        if (*p == '?' || *p == '>')
            continue;
        if (*p == '"' ? *s != '.' : lower(*p) != lower(*s))
            return 0;
    }
    return !*s;
}

/* an entry of `cls` for a name / entry at p (room bytes): its size, 0 it does not fit (-1 the class unknown) */
static int dir_entry(u8 *p, u32 room, int cls, const char *name, const struct disk_entry *e, u32 index)
{
    u32 nl = (u32)slen(name) * 2, fixed;
    switch (cls) {
    case 1: fixed = 64; break;          /* FileDirectoryInformation */
    case 2: fixed = 68; break;          /* FileFullDirectoryInformation */
    case 3: fixed = 94; break;          /* FileBothDirectoryInformation */
    case 12: fixed = 12; break;         /* FileNamesInformation */
    case 37: fixed = 104; break;        /* FileIdBothDirectoryInformation */
    case 38: fixed = 80; break;         /* FileIdFullDirectoryInformation */
    default: return -1;
    }
    if (fixed + nl > room)
        return 0;
    zero(p, fixed);
    put32(p + 4, index);
    if (cls == 12) {
        put32(p + 8, nl);
        to16(p + 12, name);
        return (int)(fixed + nl);
    }
    u64 t = filetime(e->date, e->time);
    put64(p + 8, t);
    put64(p + 16, t);
    put64(p + 24, t);
    put64(p + 32, t);
    u64 size = e->kind == DISK_FILE ? (u64)e->size_hi << 32 | e->size : 0;
    put64(p + 40, size);
    put64(p + 48, alloc_of(size));
    u32 a = e->kind != DISK_FILE ? A_DIRECTORY | (e->attr & 0x06) : (e->attr & 0x27);
    put32(p + 56, a ? a : A_NORMAL);
    put32(p + 60, nl);
    if (cls == 38)
        put64(p + 72, index + 1);
    if (cls == 37)
        put64(p + 96, index + 1);
    to16(p + fixed, name);
    return (int)(fixed + nl);
}

static u32 do_query_directory(struct client *cl, const u8 *h, const u8 *b, u32 n, u64 fid)
{
    if (n < 32)
        return S_INVALID_PARAMETER;
    int cls = b[2];
    u32 flags = b[3], off = le16(b + 24), len = le16(b + 26), room = le32(b + 28);
    struct handle *f = handle_find(cl, fid);
    if (!f)
        return S_FILE_CLOSED;
    if (f->kind != H_DIR)
        return S_INVALID_PARAMETER;
    if (room > IO_MAX)
        room = IO_MAX;
    if (flags & 0x11) {                 /* RESTART_SCANS, REOPEN */
        f->pos = 0;
        f->listed = 0;
    }
    if (len && off >= 64 && off + len <= 64 + n) {
        char pat[64];
        if (from16(h + off, len, pat, sizeof(pat)) >= 0 && !(f->listed && same_nocase(pat, f->pattern))) {
            scopy(f->pattern, pat, sizeof(f->pattern));
            if (f->listed && !(flags & 0x11))
                f->pos = 0;
        }
    }
    if (!f->pattern[0])
        scopy(f->pattern, "*", sizeof(f->pattern));
    if (!f->listed || ents_owner != f->id || ents_changes != disk_changes()) {
        u32 total = 0;
        cap_begin();
        int k = disk_dir_read(f->path, ents, ENTRIES, &total);
        const char *why = cap_end();
        if (k < 0) {
            ents_owner = 0;
            return why ? status_of(why) : S_IO_DEVICE_ERROR;
        }
        ents_n = k;
        ents_owner = f->id;
        ents_changes = disk_changes();
        f->listed = 1;
    }
    u8 *r = body_room(8);
    put16(r, 9);
    put16(r + 2, 64 + 8);
    u8 *out = rb + rb_len;
    u32 used = 0, last = 0, count = 0;
    while (f->pos < (u32)ents_n + 2) {
        static struct disk_entry dot;
        const struct disk_entry *e;
        const char *name;
        if (f->pos < 2) {
            dot.kind = DISK_DIR;
            dot.attr = 0;
            dot.date = f->e.date ? f->e.date : (1 << 5) | 1;
            dot.time = f->e.time;
            e = &dot;
            name = f->pos ? ".." : ".";
        } else {
            e = &ents[f->pos - 2];
            name = e->name;
        }
        if (!wild(f->pattern, name)) {
            f->pos++;
            continue;
        }
        u32 at = (used + 7) & ~7u;
        if (at >= room)
            break;
        int k = dir_entry(out + at, room - at, cls, name, e, f->pos);
        if (k < 0) {
            rb_len -= 8;
            return S_INVALID_INFO_CLASS;
        }
        if (!k)
            break;
        if (count)
            put32(out + last, at - last);
        last = at;
        used = at + (u32)k;
        count++;
        f->pos++;
        if (flags & 0x02)               /* RETURN_SINGLE_ENTRY */
            break;
    }
    if (!count) {
        rb_len -= 8;
        return f->pos <= 2 && !same_nocase(f->pattern, "*") ? S_NO_SUCH_FILE : S_NO_MORE_FILES;
    }
    put32(r + 4, used);
    rb_len += used;
    return S_OK;
}

/* ---- QUERY_INFO / SET_INFO ---- */
static u32 file_info(struct handle *f, int cls, u8 *p, u32 room, u32 *len)
{
    u32 a = attrs_of(f);
    u64 size = f->kind == H_FILE ? (f->writing ? f->written : f->size) : 0;
    u32 nl = (u32)slen(f->rel) * 2;
    switch (cls) {
    case 4:                             /* FileBasicInformation */
        *len = 40;
        if (room < 40)
            return S_INFO_LENGTH_MISMATCH;
        zero(p, 40);
        put_times(p, f);
        put32(p + 32, a);
        return S_OK;
    case 5:                             /* FileStandardInformation */
        *len = 24;
        if (room < 24)
            return S_INFO_LENGTH_MISMATCH;
        zero(p, 24);
        put64(p, alloc_of(size));
        put64(p + 8, size);
        put32(p + 16, 1);
        p[20] = f->delete_pending;
        p[21] = f->kind == H_DIR;
        return S_OK;
    case 6:                             /* FileInternalInformation */
        *len = 8;
        if (room < 8)
            return S_INFO_LENGTH_MISMATCH;
        put64(p, f->id);
        return S_OK;
    case 7:                             /* FileEaInformation */
    case 16:                            /* FileModeInformation */
    case 17:                            /* FileAlignmentInformation */
        *len = 4;
        if (room < 4)
            return S_INFO_LENGTH_MISMATCH;
        put32(p, 0);
        return S_OK;
    case 8:                             /* FileAccessInformation */
        *len = 4;
        if (room < 4)
            return S_INFO_LENGTH_MISMATCH;
        put32(p, 0x001F01FF);
        return S_OK;
    case 14:                            /* FilePositionInformation */
        *len = 8;
        if (room < 8)
            return S_INFO_LENGTH_MISMATCH;
        put64(p, 0);
        return S_OK;
    case 9:                             /* FileNameInformation */
    case 48:                            /* FileNormalizedNameInformation */
        *len = 4 + nl;
        if (room < 4)
            return S_INFO_LENGTH_MISMATCH;
        put32(p, nl);
        if (room < 4 + nl) {
            *len = room;
            return S_BUFFER_OVERFLOW;
        }
        to16(p + 4, f->rel);
        return S_OK;
    case 18:                            /* FileAllInformation */
        *len = 100 + nl;
        if (room < 100)
            return S_INFO_LENGTH_MISMATCH;
        zero(p, 100);
        put_times(p, f);
        put32(p + 32, a);
        put64(p + 40, alloc_of(size));
        put64(p + 48, size);
        put32(p + 56, 1);
        p[60] = f->delete_pending;
        p[61] = f->kind == H_DIR;
        put64(p + 64, f->id);
        put32(p + 76, 0x001F01FF);
        put32(p + 96, nl);
        if (room < 100 + nl) {
            *len = room;
            return S_BUFFER_OVERFLOW;
        }
        to16(p + 100, f->rel);
        return S_OK;
    case 22:                            /* FileStreamInformation: a file's one stream */
        if (f->kind != H_FILE) {
            *len = 0;
            return S_OK;
        }
        *len = 24 + 14;
        if (room < 38)
            return S_BUFFER_OVERFLOW;
        zero(p, 24);
        put32(p + 4, 14);
        put64(p + 8, size);
        put64(p + 16, alloc_of(size));
        to16(p + 24, "::$DATA");
        return S_OK;
    case 34:                            /* FileNetworkOpenInformation */
        *len = 56;
        if (room < 56)
            return S_INFO_LENGTH_MISMATCH;
        zero(p, 56);
        put_times(p, f);
        put64(p + 32, alloc_of(size));
        put64(p + 40, size);
        put32(p + 48, a);
        return S_OK;
    case 35:                            /* FileAttributeTagInformation */
        *len = 8;
        if (room < 8)
            return S_INFO_LENGTH_MISMATCH;
        put32(p, a);
        put32(p + 4, 0);
        return S_OK;
    default:
        return S_INVALID_INFO_CLASS;
    }
}

static u32 fs_info(struct handle *f, struct tree *t, int cls, u8 *p, u32 room, u32 *len)
{
    struct disk_space sp;
    char root[16];
    usnprintf(root, sizeof(root), "%s:/", t->vol);
    zero((u8 *)&sp, sizeof(sp));
    if (cls == 1 || cls == 3 || cls == 5 || cls == 7 || cls == 11) {
        cap_begin();
        int st = disk_space(root, &sp);
        (void)cap_end();
        if (st < 0) {
            sp.total = sp.avail = 0;
            sp.cluster = 4096;
            scopy(sp.fs, "FAT", sizeof(sp.fs));
        }
    }
    (void)f;
    u32 cl = sp.cluster ? sp.cluster : 4096, spc = cl / 512 ? cl / 512 : 1, shift = 0;
    while ((1u << shift) < cl && shift < 31)
        shift++;                        /* (a cluster's bytes: a power of two -- no 64-bit division here) */
    sp.total >>= shift;
    sp.avail >>= shift;
    cl = 1;
    switch (cls) {
    case 1: {                           /* FileFsVolumeInformation */
        u32 ln = (u32)slen(sp.label) * 2;
        *len = 18 + ln;
        if (room < 18)
            return S_INFO_LENGTH_MISMATCH;
        zero(p, 18);
        put64(p, filetime((1 << 5) | 1, 0));
        put32(p + 8, sp.serial);
        put32(p + 12, ln);
        if (room < 18 + ln) {
            *len = room;
            return S_BUFFER_OVERFLOW;
        }
        to16(p + 18, sp.label);
        return S_OK;
    }
    case 3:                             /* FileFsSizeInformation */
        *len = 24;
        if (room < 24)
            return S_INFO_LENGTH_MISMATCH;
        put64(p, sp.total / cl);
        put64(p + 8, sp.avail / cl);
        put32(p + 16, spc);
        put32(p + 20, 512);
        return S_OK;
    case 4:                             /* FileFsDeviceInformation */
        *len = 8;
        if (room < 8)
            return S_INFO_LENGTH_MISMATCH;
        put32(p, 7);                    /* FILE_DEVICE_DISK */
        put32(p + 4, 0x20);             /* FILE_DEVICE_IS_MOUNTED */
        return S_OK;
    case 5: {                           /* FileFsAttributeInformation */
        u32 fl = (u32)slen(sp.fs) * 2;
        *len = 12 + fl;
        if (room < 12)
            return S_INFO_LENGTH_MISMATCH;
        put32(p, 0x00000003);           /* case sensitive search off: case preserved names, Unicode on disk */
        put32(p + 4, 255);
        put32(p + 8, fl);
        if (room < 12 + fl) {
            *len = room;
            return S_BUFFER_OVERFLOW;
        }
        to16(p + 12, sp.fs);
        return S_OK;
    }
    case 7:                             /* FileFsFullSizeInformation */
        *len = 32;
        if (room < 32)
            return S_INFO_LENGTH_MISMATCH;
        put64(p, sp.total / cl);
        put64(p + 8, sp.avail / cl);
        put64(p + 16, sp.avail / cl);
        put32(p + 24, spc);
        put32(p + 28, 512);
        return S_OK;
    case 11:                            /* FileFsSectorSizeInformation */
        *len = 28;
        if (room < 28)
            return S_INFO_LENGTH_MISMATCH;
        put32(p, 512);
        put32(p + 4, 512);
        put32(p + 8, 512);
        put32(p + 12, 512);
        put32(p + 16, 0);
        put32(p + 20, 0);
        put32(p + 24, 0);
        return S_OK;
    default:
        return S_INVALID_INFO_CLASS;
    }
}

/* a security descriptor: Everyone owns it and may do everything */
static u32 security(u8 *p, u32 room, u32 what, u32 *len)
{
    static const u8 everyone[12] = {1, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0};    /* S-1-1-0 */
    u32 at = 20;
    u8 t[100];
    zero(t, sizeof(t));
    t[0] = 1;                           /* revision */
    put16(t + 2, 0x8004);               /* self-relative, DACL present */
    if (what & 1) {
        put32(t + 4, at);
        copy(t + at, everyone, 12);
        at += 12;
    }
    if (what & 2) {
        put32(t + 8, at);
        copy(t + at, everyone, 12);
        at += 12;
    }
    put32(t + 16, at);                  /* the DACL: one ACE, ACCESS_ALLOWED to Everyone */
    t[at] = 2;
    put16(t + at + 2, 8 + 20);
    put16(t + at + 4, 1);
    t[at + 8] = 0;                      /* ACCESS_ALLOWED_ACE_TYPE */
    t[at + 9] = 0x03;                   /* inherited by files and directories */
    put16(t + at + 10, 20);
    put32(t + at + 12, 0x001F01FF);
    copy(t + at + 16, everyone, 12);
    at += 28;
    *len = at;
    if (room < at)
        return S_BUFFER_OVERFLOW;
    copy(p, t, at);
    return S_OK;
}

static u32 do_query_info(struct client *cl, struct tree *t, const u8 *b, u32 n, u64 fid)
{
    if (n < 40)
        return S_INVALID_PARAMETER;
    int type = b[2], cls = b[3];
    u32 room = le32(b + 4), what = le32(b + 16);
    struct handle *f = handle_find(cl, fid);
    if (!f)
        return S_FILE_CLOSED;
    if (room > IO_MAX)
        room = IO_MAX;
    u8 *r = body_room(8);
    put16(r, 9);
    put16(r + 2, 64 + 8);
    u8 *p = rb + rb_len;
    u32 len = 0, st;
    if (f->kind == H_PIPE) {
        st = type == 1 && cls == 5 ? file_info(f, cls, p, room, &len) : S_NOT_SUPPORTED;
    } else if (type == 1) {
        st = file_info(f, cls, p, room, &len);
    } else if (type == 2) {
        st = t->ipc ? S_NOT_SUPPORTED : fs_info(f, t, cls, p, room, &len);
    } else if (type == 3) {
        st = security(p, room, what, &len);
    } else {
        st = S_NOT_SUPPORTED;
    }
    if (st && st != S_BUFFER_OVERFLOW) {
        rb_len -= 8;
        if (st == S_INFO_LENGTH_MISMATCH || (type == 3 && st == S_BUFFER_OVERFLOW)) {
            /* (too small: the size it needs, as an error's data) */
        }
        return st;
    }
    if (len > room)
        len = room;
    put32(r + 4, len);
    rb_len += len;
    return st;
}

static u32 do_set_info(struct client *cl, struct tree *t, const u8 *h, const u8 *b, u32 n, u64 fid)
{
    if (n < 32)
        return S_INVALID_PARAMETER;
    int type = b[2], cls = b[3];
    u32 len = le32(b + 4), off = le16(b + 8);
    if (off < 64 || len > n || off - 64 > n - len)
        return S_INVALID_PARAMETER;     /* (len is 32 bits: off + len could wrap and pass) */
    const u8 *p = h + off;
    struct handle *f = handle_find(cl, fid);
    if (!f)
        return S_FILE_CLOSED;
    u32 st = S_OK;
    if (type == 3) {
        st = S_OK;                      /* (a security descriptor: taken and left) */
    } else if (type != 1 || f->kind == H_PIPE) {
        st = S_NOT_SUPPORTED;
    } else if (cls == 4 || cls == 14 || cls == 16) {
        st = S_OK;                      /* basic (times, attributes), position, mode: left */
    } else if (cls == 19 || cls == 20) {        /* FileAllocationInformation, FileEndOfFileInformation */
        if (len < 8 || f->kind != H_FILE)
            return S_INVALID_PARAMETER;
        /* an allocation below the end cuts the file there too: PowerShell's Set-Content empties a file so
           (allocation 0), then writes after what it read of it -- not emptied, the write landed at 1024 */
        u64 to = le64(p), end = f->writing ? f->written : f->size;
        if (to >= end) {
            /* (a size announced before the data -- the writer goes on appending -- or an allocation) */
        } else if (to == 0) {
            if (f->writing) {
                cap_begin();
                (void)disk_upload_close();
                (void)cap_end();
                writer_owner = 0;
                f->writing = 0;
            }
            st = writer_take(f, 0);
        } else {
            st = S_NOT_SUPPORTED;       /* (cut part way: the writer only empties or appends) */
        }
    } else if (cls == 13 || cls == 64) {        /* FileDispositionInformation (Ex) */
        if (len < 1)
            return S_INVALID_PARAMETER;
        if (f->root)
            return S_ACCESS_DENIED;
        f->delete_pending = (cls == 13 ? p[0] : (p[0] & 1)) ? 1 : 0;
        if (f->delete_pending && !f->writing && writer_owner && same_nocase(writer_path, f->path)) {
            f->delete_pending = 0;      /* (another handle writes it: its clusters would be freed under it) */
            return S_SHARING_VIOLATION;
        }
    } else if (cls == 10) {             /* FileRenameInformation: a new name in the same share */
        if (len < 20 || f->root)
            return S_INVALID_PARAMETER;
        u32 nl = le32(p + 16);
        if (20 + nl > len)
            return S_INVALID_PARAMETER;
        static struct handle to;
        zero((u8 *)&to, sizeof(to));
        st = make_path(t, p + 20, nl, &to);
        if (st)
            return st;
        if (f->writing || (writer_owner && same_nocase(writer_path, f->path)))
            return S_SHARING_VIOLATION; /* (the writer's entry would move under it) */
        if (reader_owner && same_nocase(reader_path, f->path))
            reader_release();
        struct disk_entry e;
        cap_begin();
        int there = disk_stat(to.path, &e) == 0;
        (void)cap_end();
        if (there && !same_nocase(to.path, f->path)) {
            if (!p[0])
                return S_NAME_COLLISION;
            cap_begin();
            disk_rm(to.path);
            const char *why = cap_end();
            if (why)
                return status_of(why);
        }
        cap_begin();
        disk_mv(f->path, to.path);
        const char *why = cap_end();
        if (why)
            return status_of(why);
        scopy(f->path, to.path, sizeof(f->path));
        scopy(f->rel, to.rel, sizeof(f->rel));
    } else {
        st = S_NOT_SUPPORTED;
    }
    if (st)
        return st;
    u8 *r = body_room(2);
    put16(r, 2);
    return S_OK;
}

static u32 do_ioctl(struct client *cl, const u8 *h, const u8 *b, u32 n, u64 fid)
{
    if (n < 56)
        return S_INVALID_PARAMETER;
    u32 code = le32(b + 4), in_off = le32(b + 24), in_len = le32(b + 28), max_out = le32(b + 44);
    if (in_len && (in_off < 64 || in_len > n || in_off - 64 > n - in_len))
        return S_INVALID_PARAMETER;     /* (32-bit offset and length: compared so that neither sum wraps) */
    if (code == 0x00060194u || code == 0x000601B0u)     /* FSCTL_DFS_GET_REFERRALS (_EX): no DFS */
        return S_NOT_FOUND;
    if (code == 0x00140204u) {          /* FSCTL_VALIDATE_NEGOTIATE_INFO: NEGOTIATE's answer again (Windows asks
                                           after its first TREE_CONNECT; refused, it logged off and went) */
        u8 *r = body_room(48);
        put16(r, 49);
        put32(r + 4, code);
        put64(r + 8, ~(u64)0);
        put64(r + 16, ~(u64)0);
        put32(r + 24, 64 + 48);
        put32(r + 32, 64 + 48);
        put32(r + 36, 24);
        u8 *o = body_room(24);
        put32(o, 0);                    /* Capabilities */
        copy(o + 4, server_guid, 16);
        put16(o + 20, 0x0001);          /* SecurityMode */
        put16(o + 22, cl->dialect);
        return S_OK;
    }
    if (code != 0x0011C017u)            /* FSCTL_PIPE_TRANSCEIVE only (Windows' FSCTL_QUERY_FILE_REGIONS, its
                                           server-side copy: refused, and it reads and writes instead) */
        return S_NOT_SUPPORTED;
    struct handle *f = handle_find(cl, fid);
    if (!f)
        return S_FILE_CLOSED;
    if (f->kind != H_PIPE)
        return S_INVALID_DEVICE_REQ;
    u8 *r = body_room(48);
    put16(r, 49);
    put32(r + 4, code);
    put64(r + 8, f->id);
    put64(r + 16, f->id);
    put32(r + 24, 64 + 48);
    put32(r + 32, 64 + 48);
    u8 *out = rb + rb_len;
    u32 len = rpc(h + in_off, in_len, out, PIPE_MAX);
    if (len > max_out)
        len = max_out;
    put32(r + 36, len);
    rb_len += len;
    return S_OK;
}

/* ---- one message ---- */
static u32 command(struct client *cl, u32 cmd, const u8 *h, u32 len, u64 *sid, u32 *tid, u64 *fid,
                   u16 *sflags, int related, u64 last_fid)
{
    const u8 *b = h + 64;
    u32 n = len - 64;
    if (cmd == C_NEGOTIATE)
        return do_negotiate(cl, b, n);
    if (cmd == C_SESSION_SETUP)
        return do_session_setup(cl, h, b, n, sid, sflags);
    if (cmd == C_ECHO) {
        u8 *r = body_room(4);
        put16(r, 4);
        return S_OK;
    }
    if (!cl->authed || *sid != cl->session)
        return S_USER_SESSION_DELETED;
    if (cmd == C_LOGOFF) {
        for (int i = 0; i < HANDLES; i++)
            if (handles[i].used && handles[i].client == (u8)(cl - clients))
                handle_close(&handles[i]);
        cl->authed = 0;
        for (int t = 0; t < TREES; t++)
            cl->trees[t].used = 0;
        u8 *r = body_room(4);
        put16(r, 4);
        return S_OK;
    }
    if (cmd == C_TREE_CONNECT)
        return do_tree_connect(cl, h, b, n, tid);
    struct tree *t = tree_find(cl, *tid);
    if (!t)
        return S_NETWORK_NAME_DELETED;
    /* the FileId: its own, or (related, all ones) the CREATE's before it in the same message */
    u64 f = n >= 32 ? 0 : 0;
    if (cmd == C_CLOSE || cmd == C_FLUSH || cmd == C_QUERY_DIRECTORY || cmd == C_CHANGE_NOTIFY ||
        cmd == C_QUERY_INFO || cmd == C_SET_INFO || cmd == C_LOCK) {
        u32 at = cmd == C_CLOSE ? 8 : cmd == C_FLUSH ? 8 : cmd == C_QUERY_DIRECTORY ? 8 : cmd == C_CHANGE_NOTIFY ? 8
               : cmd == C_QUERY_INFO ? 24 : cmd == C_SET_INFO ? 16 : 8;
        if (n >= at + 16)
            f = le64(b + at + 8);
    } else if (cmd == C_READ || cmd == C_WRITE) {
        if (n >= 32)
            f = le64(b + 24);
    } else if (cmd == C_IOCTL) {
        if (n >= 24)
            f = le64(b + 16);
    }
    if (related && f == ~(u64)0)
        f = last_fid;
    switch (cmd) {
    case C_TREE_DISCONNECT: {
        for (int i = 0; i < HANDLES; i++)
            if (handles[i].used && handles[i].client == (u8)(cl - clients) && handles[i].tree == t->id)
                handle_close(&handles[i]);
        t->used = 0;
        u8 *r = body_room(4);
        put16(r, 4);
        return S_OK;
    }
    case C_CREATE:
        return do_create(cl, t, h, b, n, fid);
    case C_CLOSE:
        return do_close(cl, b, n, f);
    case C_FLUSH:
    case C_LOCK: {
        if (!handle_find(cl, f))
            return S_FILE_CLOSED;
        u8 *r = body_room(4);
        put16(r, 4);
        return S_OK;
    }
    case C_READ:
        return do_read(cl, b, n, f);
    case C_WRITE:
        return do_write(cl, h, b, n, f);
    case C_IOCTL:
        return do_ioctl(cl, h, b, n, f);
    case C_QUERY_DIRECTORY:
        return do_query_directory(cl, h, b, n, f);
    case C_QUERY_INFO:
        return do_query_info(cl, t, b, n, f);
    case C_SET_INFO:
        return do_set_info(cl, t, h, b, n, f);
    default:
        return S_NOT_SUPPORTED;     /* CHANGE_NOTIFY, OPLOCK_BREAK */
    }
}

static void process(struct client *cl)
{
    u8 *m = cl->in + 4, *out = cl->out;
    u32 n = cl->need - 4, at = 0, oat = 4, prev = 0;
    cl->out_len = cl->out_pos = 0;
    cl->requests++;
    if (n >= 4 && m[0] == 0xFF && m[1] == 'S' && m[2] == 'M' && m[3] == 'B') {
        u32 dialect;
        if (!smb1_upgrade(m, n, &dialect))
            return;
        static u8 fake[64 + 38];
        zero(fake, sizeof(fake));
        put16(fake + 64, 36);
        put16(fake + 66, 1);
        put16(fake + 64 + 36, dialect == 0x02FF ? 0x0210 : 0x0202);
        u8 *rh = out + oat;
        zero(rh, 64);
        put32(rh, 0x424D53FEu);
        put16(rh + 4, 64);
        put16(rh + 14, 1);
        put32(rh + 16, F_REPLY);
        rb = rh + 64;
        rb_len = 0;
        (void)do_negotiate(cl, fake + 64, 38);
        put16(rb + 4, dialect);
        oat += 64 + rb_len;
    } else if (n >= 64 && m[0] == 0xFE && m[1] == 'S' && m[2] == 'M' && m[3] == 'B') {
        u64 sid = 0, fid_last = ~(u64)0;
        u32 tid = 0, last_status = 0;
        for (;;) {
            const u8 *h = m + at;
            if (at + 64 > n)
                break;
            u32 next = le32(h + 20), len = next ? next : n - at;
            if (len < 64 || len > n - at)
                break;                  /* (not at + len > n: a NextCommand near 4 GB wrapped past it -- a length
                                           the commands then trusted, or a step back and round for ever) */
            u32 cmd = le16(h + 12), flags = le32(h + 16);
            int related = (flags & F_RELATED) != 0;
            if (!related) {
                sid = le64(h + 40);
                tid = le32(h + 36);
            }
            if (cmd == C_CANCEL) {
                if (!next)
                    break;
                at += next;
                continue;
            }
            oat = (oat - 4 + 7) / 8 * 8 + 4;
            if (oat + 64 + 0x10200 > 0x20000)
                break;                  /* (no room for another answer) */
            u8 *rh = out + oat;
            zero(rh, 64);
            rb = rh + 64;
            rb_len = 0;
            u64 fid = 0;
            u16 sflags = 0;
            u32 st;
            if (related && last_status && last_status != S_MORE_PROCESSING)
                st = last_status;       /* (a related request after a failed one fails the same) */
            else
                st = command(cl, cmd, h, len, &sid, &tid, &fid, &sflags, related, fid_last);
            if (cmd == C_SESSION_SETUP && st != S_OK && st != S_MORE_PROCESSING)
                rb_len = 0;
            if (cmd == C_SESSION_SETUP && rb_len >= 4)
                put16(rb + 2, sflags);
            if (st != S_OK && st != S_BUFFER_OVERFLOW && st != S_MORE_PROCESSING && !(cmd == C_SESSION_SETUP &&
                rb_len)) {
                rb_len = 0;
                u8 *r = body_room(9);
                put16(r, 9);
            }
            if (cmd == C_CREATE && st == S_OK)
                fid_last = fid;
            last_status = st;
            put32(rh, 0x424D53FEu);
            put16(rh + 4, 64);
            put16(rh + 6, le16(h + 6));
            put32(rh + 8, st);
            put16(rh + 12, cmd);
            u32 credits = le16(h + 14);
            put16(rh + 14, credits < 1 ? 1 : credits > 64 ? 64 : credits);
            put32(rh + 16, F_REPLY | (flags & F_RELATED));
            copy(rh + 24, h + 24, 8);   /* MessageId */
            copy(rh + 32, h + 32, 4);
            put32(rh + 36, tid);
            put64(rh + 40, sid);
            if (prev)
                put32(out + prev + 20, oat - prev);
            prev = oat;
            oat += 64 + rb_len;
            if (!next)
                break;
            at += next;
        }
    }
    if (oat <= 4)
        return;
    out[0] = 0;
    out[1] = (u8)((oat - 4) >> 16);
    out[2] = (u8)((oat - 4) >> 8);
    out[3] = (u8)(oat - 4);
    cl->out_len = oat;
}

/* ---- the connections ---- */
static void service(struct client *cl)
{
    int st = tcp_status(cl->c);
    if (st == TCP_FAILED || st == TCP_DONE || (st == TCP_PEER_DONE && !tcp_unread(cl->c))) {
        usnprintf(line, sizeof(line), "smb: %u.%u.%u.%u left", cl->ip[0], cl->ip[1], cl->ip[2], cl->ip[3]);
        client_drop(cl, st == TCP_FAILED);
        return;
    }
    if (st != TCP_OPEN && st != TCP_PEER_DONE)
        return;
    u64 now = now_ms();
    if (cl->out_pos < cl->out_len) {    /* the answer going out as the send buffer takes it */
        u32 room = tcp_room(cl->c), left = cl->out_len - cl->out_pos;
        if (room) {
            int k = tcp_send(cl->c, cl->out + cl->out_pos, (int)(left < room ? left : room));
            if (k > 0)
                cl->out_pos += (u32)k;
            tcp_push(cl->c);
        }
        if (cl->out_pos < cl->out_len)
            return;
    }
    for (int pass = 0; pass < 64; pass++) {
        u32 want = cl->in_len < 4 ? 4 - cl->in_len : cl->need - cl->in_len;
        if (!want)
            break;
        int k = tcp_recv(cl->c, cl->in + cl->in_len, (int)want);
        if (k <= 0)
            break;
        cl->in_len += (u32)k;
        cl->heard = now;
        if (cl->in_len == 4) {
            u32 len = (u32)cl->in[1] << 16 | (u32)cl->in[2] << 8 | cl->in[3];
            if (cl->in[0] || len > MSG_MAX) {
                if (cl->in[0] == 0x85 && !len) {        /* (a NetBIOS keepalive) */
                    cl->in_len = 0;
                    continue;
                }
                client_drop(cl, 1);
                return;
            }
            cl->need = 4 + len;
        }
    }
    if (cl->in_len >= 4 && cl->in_len == cl->need) {
        process(cl);
        cl->in_len = cl->need = 0;
        if (cl->out_len) {
            u32 room = tcp_room(cl->c);
            int k = tcp_send(cl->c, cl->out, (int)(cl->out_len < room ? cl->out_len : room));
            if (k > 0)
                cl->out_pos = (u32)k;
            tcp_push(cl->c);
        }
    } else if (now - cl->heard > IDLE_MS) {
        client_drop(cl, 0);
    }
}

const char *smb_poll(void)
{
    line[0] = 0;
    if (!started) {
        if (!eth_ready())
            return 0;
        if (tcp_listen(SMB_PORT) < 0)
            return 0;
        for (int i = 0; i < CLIENTS; i++) {
            clients[i].c = -1;
            clients[i].in = (u8 *)KSEG0(RAM_IN(i));
            clients[i].out = (u8 *)KSEG0(RAM_OUT(i));
        }
        started = 1;
    }
    int c = tcp_accept(SMB_PORT);
    if (c >= 0) {
        int i = 0;
        while (i < CLIENTS && clients[i].c >= 0)
            i++;
        if (i == CLIENTS) {
            tcp_abort(c);
        } else {
            struct client *cl = &clients[i];
            u8 *in = cl->in, *out = cl->out;
            zero((u8 *)cl, sizeof(*cl));
            cl->in = in;
            cl->out = out;
            cl->c = c;
            u16 port;
            tcp_peer(c, cl->ip, &port);
            cl->heard = now_ms();
            n_connects++;
            usnprintf(line, sizeof(line), "smb: %u.%u.%u.%u connected", cl->ip[0], cl->ip[1], cl->ip[2], cl->ip[3]);
        }
    }
    for (int i = 0; i < CLIENTS; i++)
        if (clients[i].c >= 0)
            service(&clients[i]);
    if (reader_owner && now_ms() - reader_used > READER_IDLE_MS)
        reader_release();
    return line[0] ? line : 0;
}

void smb_status(void)
{
    int n = 0, open = 0;
    for (int i = 0; i < CLIENTS; i++)
        n += clients[i].c >= 0;
    for (int i = 0; i < HANDLES; i++)
        open += handles[i].used;
    uprintf("SMB server (port 445): %s; %d client%s", started ? "\\\\<this box's address>\\<share>" : "not started",
            n, n == 1 ? "" : "s");
    for (int i = 0; i < CLIENTS; i++)
        if (clients[i].c >= 0)
            uprintf(" (%u.%u.%u.%u%s%s, %u requests)", clients[i].ip[0], clients[i].ip[1], clients[i].ip[2],
                    clients[i].ip[3], clients[i].user[0] ? " as " : "", clients[i].user, clients[i].requests);
    uprintf(", %d files open; %u reads (%u KB), %u writes (%u KB), %u connections\n", open, n_reads,
            (u32)(bytes_out >> 10), n_writes, (u32)(bytes_in >> 10), n_connects);
}
