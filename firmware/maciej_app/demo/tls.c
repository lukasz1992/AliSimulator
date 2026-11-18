/* tls.c: TLS for wget's https:// -- BearSSL 0.6 (../bearssl: Thomas Pornin's TLS library, MIT licence; the files
   a client needs) over one of tcp.c's connections, inside the wget command (it waits there as wget does: the
   frames read meanwhile, Ctrl-C stops it, 15 s without data ends it).

   What BearSSL does: TLS 1.0 to 1.2 (no 1.3: a server that speaks only 1.3 refuses the handshake), every
   cipher suite of its "full" client (ECDHE or RSA key exchange; AES-GCM, ChaCha20-Poly1305, AES-CCM, AES-CBC,
   3DES; RSA and ECDSA certificates), the server's name sent (SNI) and checked against its certificate, the
   chain checked against Mozilla's roots (tls_anchors.bin, built in by tls_anchors.S; tools/tls_anchors.py
   makes it from curl's cacert.pem and lists them in tls_anchors.txt) with the date NTP set -- a box whose clock
   is not set cannot check a certificate's dates and says so.  'wget --no-check-certificate' takes the server's
   key from its certificate and checks nothing (as wget's own option): a man in the middle could read it all.

   Randomness: the box has no random number generator.  TLS's (the client's random, its ECDHE key) is seeded
   from a pool of CP0 Count values taken as Ethernet frames arrive (eth.c calls tls_stir for each), Count again
   around 64 reads of the network at the handshake, the clock and the previous seed, hashed (SHA-256) --
   unpredictable to someone who cannot see the box's timing to the cycle, not a hardware RNG.

   Memory: the record buffer (BR_SSL_BUFSIZE_BIDI, 33 KB) and the contexts are static; one connection at a
   time (wget is a command: nothing else uses TLS meanwhile).  On the box (2026-11-18) a handshake with an
   ECDHE_RSA / ECDSA server took about 0.1 s of computing. */
#include "hw.h"
#include "bearssl.h"

#define ANCHORS_MAX     160
#define IDLE_MS         15000

/* ---- what BearSSL needs of a C library (the demo has no libc; disk.c has memcpy, memset, memmove) ---- */
int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

/* (GCC's 64-bit shifts by a variable count, __lshrdi3 that BearSSL's HMAC needs: disk.c has them) */

/* ---- the roots (tls_anchors.S: tls_anchors.bin) ---- */
extern const u8 tls_anchors_bin[], tls_anchors_bin_end[];
static br_x509_trust_anchor anchors[ANCHORS_MAX];
static int anchors_n = -1;

static u32 get16(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8;
}

static int anchors_load(void)
{
    if (anchors_n >= 0)
        return anchors_n;
    const u8 *p = tls_anchors_bin, *end = tls_anchors_bin_end;
    anchors_n = 0;
    if (end - p < 8 || p[0] != 'T' || p[1] != 'A' || p[2] != '0' || p[3] != '1')
        return 0;
    u32 count = get16(p + 4) | get16(p + 6) << 16;
    p += 8;
    while (count-- && anchors_n < ANCHORS_MAX && end - p >= 8) {
        u32 kind = p[0], curve = p[1], dn = get16(p + 2), a = get16(p + 4), b = get16(p + 6);
        p += 8;
        if ((u32)(end - p) < dn + a + b)
            break;
        br_x509_trust_anchor *t = &anchors[anchors_n];
        t->dn.data = (unsigned char *)p;
        t->dn.len = dn;
        t->flags = BR_X509_TA_CA;
        if (kind == 1) {
            t->pkey.key_type = BR_KEYTYPE_RSA;
            t->pkey.key.rsa.n = (unsigned char *)p + dn;
            t->pkey.key.rsa.nlen = a;
            t->pkey.key.rsa.e = (unsigned char *)p + dn + a;
            t->pkey.key.rsa.elen = b;
        } else {
            t->pkey.key_type = BR_KEYTYPE_EC;
            t->pkey.key.ec.curve = (int)curve;
            t->pkey.key.ec.q = (unsigned char *)p + dn;
            t->pkey.key.ec.qlen = a;
        }
        p += dn + a + b;
        anchors_n++;
    }
    return anchors_n;
}

/* ---- --no-check-certificate: an X.509 "engine" that takes the first certificate's key and checks nothing ---- */
struct nocheck {
    const br_x509_class *vtable;
    br_x509_decoder_context dc;
    int n, ok;
};

static void nc_start_chain(const br_x509_class **ctx, const char *server_name)
{
    struct nocheck *x = (struct nocheck *)(void *)ctx;
    (void)server_name;
    x->n = 0;
    x->ok = 0;
}

static void nc_start_cert(const br_x509_class **ctx, uint32_t length)
{
    struct nocheck *x = (struct nocheck *)(void *)ctx;
    (void)length;
    if (!x->n)
        br_x509_decoder_init(&x->dc, 0, 0);
}

static void nc_append(const br_x509_class **ctx, const unsigned char *buf, size_t len)
{
    struct nocheck *x = (struct nocheck *)(void *)ctx;
    if (!x->n)
        br_x509_decoder_push(&x->dc, buf, len);
}

static void nc_end_cert(const br_x509_class **ctx)
{
    struct nocheck *x = (struct nocheck *)(void *)ctx;
    if (!x->n)
        x->ok = br_x509_decoder_get_pkey(&x->dc) != 0;
    x->n++;
}

static unsigned nc_end_chain(const br_x509_class **ctx)
{
    struct nocheck *x = (struct nocheck *)(void *)ctx;
    return x->ok ? 0 : BR_ERR_X509_UNSUPPORTED;    /* (no key read from it) */
}

static const br_x509_pkey *nc_get_pkey(const br_x509_class *const *ctx, unsigned *usages)
{
    struct nocheck *x = (struct nocheck *)(void *)ctx;
    if (usages)
        *usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
    return x->ok ? br_x509_decoder_get_pkey(&x->dc) : 0;
}

static const br_x509_class nocheck_class = {
    sizeof(struct nocheck), nc_start_chain, nc_start_cert, nc_append, nc_end_cert, nc_end_chain, nc_get_pkey
};

/* ---- randomness ---- */
static u32 pool[32];
static u32 pool_at, stirs;
static u8 carry[32];                    /* the last seed (hashed into the next) */

void tls_stir(u32 v)
{
    u32 i = pool_at++ & 31, w = pool[i];
    pool[i] = (w << 5 | w >> 27) ^ v ^ cp0_count();
    stirs++;
}

static void seed(br_ssl_engine_context *eng)
{
    br_sha256_context h;
    br_sha256_init(&h);
    br_sha256_update(&h, carry, sizeof(carry));
    br_sha256_update(&h, pool, sizeof(pool));
    u64 t = now_ticks();
    u32 more[6] = {(u32)t, (u32)(t >> 32), net_clock_utc(), stirs, pool_at, 0};
    br_sha256_update(&h, more, sizeof(more));
    br_sha256_update(&h, eth_address(), 4);
    for (int i = 0; i < 64; i++) {      /* (the time the network's frames take to read, to the cycle) */
        eth_service();
        u32 c = cp0_count();
        br_sha256_update(&h, &c, sizeof(c));
    }
    u8 out[32];
    br_sha256_out(&h, out);
    br_ssl_engine_inject_entropy(eng, out, sizeof(out));
    br_sha256_init(&h);                 /* (what is kept for the next is not what was given) */
    br_sha256_update(&h, out, sizeof(out));
    br_sha256_update(&h, "next", 4);
    br_sha256_out(&h, carry);
}

/* ---- the connection ---- */
static br_ssl_client_context cc;
static br_x509_minimal_context xc;
static struct nocheck nc;
static br_sslio_context ioc;
static u8 iobuf[BR_SSL_BUFSIZE_BIDI];
static int conn = -1, peer_closed;
static const char *io_why;
static u64 io_last;

static int stop_asked(void)
{
    if (uart_getc() != 3)
        return 0;
    io_why = "stopped (Ctrl-C)";
    return 1;
}

static int low_read(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    for (;;) {
        int k = tcp_recv(conn, buf, (int)len);
        if (k > 0) {
            io_last = now_ms();
            tls_stir((u32)k);
            return k;
        }
        int st = tcp_status(conn);
        if (st == TCP_FAILED) {
            io_why = tcp_why(conn);
            return -1;
        }
        if (st != TCP_OPEN) {
            peer_closed = 1;
            io_why = "the server closed the connection";
            return -1;
        }
        if (now_ms() - io_last > IDLE_MS) {
            io_why = "no data for 15 s";
            return -1;
        }
        if (stop_asked())
            return -1;
        eth_service();
    }
}

static int low_write(void *ctx, const unsigned char *buf, size_t len)
{
    (void)ctx;
    u64 since = now_ms();
    for (;;) {
        int n = tcp_send(conn, buf, (int)len);
        if (n > 0) {
            tcp_push(conn);
            return n;
        }
        int st = tcp_status(conn);
        if (st != TCP_OPEN) {
            io_why = st == TCP_FAILED ? tcp_why(conn) : "the server closed the connection";
            return -1;
        }
        if (now_ms() - since > IDLE_MS) {
            io_why = "the server took nothing for 15 s";
            return -1;
        }
        if (stop_asked())
            return -1;
        eth_service();
    }
}

/* BearSSL's error code in words */
static const char *error_text(int e)
{
    static char t[80];
    switch (e) {
    case BR_ERR_X509_NOT_TRUSTED:
        return "its certificate is not signed by a root this box knows (--no-check-certificate takes it anyway)";
    case BR_ERR_X509_EXPIRED:
        return "its certificate has expired or is not valid yet (is the clock right? 'date')";
    case BR_ERR_X509_BAD_SERVER_NAME:
        return "its certificate is for another name";
    case BR_ERR_X509_TIME_UNKNOWN:
        return "the clock is not set, so the certificate's dates can't be checked ('ntp' sets it; "
               "--no-check-certificate skips the check)";
    case BR_ERR_BAD_VERSION:
    case BR_ERR_UNSUPPORTED_VERSION:
        return "it wants a TLS version this box does not speak (1.3 only?)";
    case BR_ERR_BAD_CIPHER_SUITE:
        return "no cipher suite in common";
    case BR_ERR_X509_UNSUPPORTED:
        return "its certificate uses something BearSSL does not take";
    case BR_ERR_X509_LIMIT_EXCEEDED:
        return "its certificate chain is too long";
    case BR_ERR_NO_RANDOM:
        return "no randomness (a bug: it is seeded before each connection)";
    }
    if (e >= BR_ERR_RECV_FATAL_ALERT && e < BR_ERR_RECV_FATAL_ALERT + 256) {
        int a = e - BR_ERR_RECV_FATAL_ALERT;
        usnprintf(t, sizeof(t), "the server ended it with alert %d%s", a,
                  a == 40 ? " (handshake failure: nothing in common)" : a == 70 ? " (protocol version: 1.3 only?)"
                  : a == 112 ? " (unrecognized name)" : "");
        return t;
    }
    if (e >= BR_ERR_SEND_FATAL_ALERT && e < BR_ERR_SEND_FATAL_ALERT + 256) {
        usnprintf(t, sizeof(t), "this box ended it with alert %d", e - BR_ERR_SEND_FATAL_ALERT);
        return t;
    }
    if (e >= 32 && e < 64)
        usnprintf(t, sizeof(t), "its certificate was refused (X.509 error %d)", e);
    else
        usnprintf(t, sizeof(t), "TLS error %d", e);
    return t;
}

static const char *failure(void)
{
    int e = br_ssl_engine_last_error(&cc.eng);
    if (e == BR_ERR_IO || !e)
        return io_why ? io_why : "the connection went";
    return error_text(e);
}

/* "TLS 1.2, ECDHE_RSA_WITH_AES_128_GCM_SHA256" */
const char *tls_info(void)
{
    static char t[80];
    static const struct { u16 id; const char *name; } suites[] = {
        {0xC02B, "ECDHE_ECDSA_WITH_AES_128_GCM_SHA256"}, {0xC02C, "ECDHE_ECDSA_WITH_AES_256_GCM_SHA384"},
        {0xC02F, "ECDHE_RSA_WITH_AES_128_GCM_SHA256"}, {0xC030, "ECDHE_RSA_WITH_AES_256_GCM_SHA384"},
        {0xCCA8, "ECDHE_RSA_WITH_CHACHA20_POLY1305"}, {0xCCA9, "ECDHE_ECDSA_WITH_CHACHA20_POLY1305"},
        {0xC023, "ECDHE_ECDSA_WITH_AES_128_CBC_SHA256"}, {0xC027, "ECDHE_RSA_WITH_AES_128_CBC_SHA256"},
        {0xC009, "ECDHE_ECDSA_WITH_AES_128_CBC_SHA"}, {0xC013, "ECDHE_RSA_WITH_AES_128_CBC_SHA"},
        {0xC014, "ECDHE_RSA_WITH_AES_256_CBC_SHA"}, {0x009C, "RSA_WITH_AES_128_GCM_SHA256"},
        {0x009D, "RSA_WITH_AES_256_GCM_SHA384"}, {0x002F, "RSA_WITH_AES_128_CBC_SHA"},
        {0x0035, "RSA_WITH_AES_256_CBC_SHA"}, {0x000A, "RSA_WITH_3DES_EDE_CBC_SHA"},
    };
    u32 v = br_ssl_engine_get_version(&cc.eng), s = cc.eng.session.cipher_suite;
    const char *name = 0;
    for (u32 i = 0; i < sizeof(suites) / sizeof(suites[0]); i++)
        if (suites[i].id == s)
            name = suites[i].name;
    char other[16];
    usnprintf(other, sizeof(other), "suite 0x%04x", s);
    usnprintf(t, sizeof(t), "TLS 1.%u, %s", v >= 0x0301 ? v - 0x0301 : 0, name ? name : other);
    return t;
}

/* the handshake on connection c (open) with `host` (its name: SNI, and the certificate's); check 0: nothing
   checked.  1, or 0 (said why) */
int tls_begin(int c, const char *host, int check)
{
    conn = c;
    peer_closed = 0;
    io_why = 0;
    io_last = now_ms();
    int n = anchors_load();
    br_ssl_client_init_full(&cc, &xc, anchors, (size_t)n);
    u32 utc = net_clock_utc();
    if (utc)                            /* days since 1 January of year 0 (719528 to 1970), seconds */
        br_x509_minimal_set_time(&xc, 719528u + utc / 86400u, utc % 86400u);
    if (!check) {
        nc.vtable = &nocheck_class;
        br_ssl_engine_set_x509(&cc.eng, &nc.vtable);
    }
    br_ssl_engine_set_buffer(&cc.eng, iobuf, sizeof(iobuf), 1);
    seed(&cc.eng);
    if (!br_ssl_client_reset(&cc, host, 0)) {
        uprintf("TLS: %s\n", failure());
        return 0;
    }
    br_sslio_init(&ioc, &cc.eng, low_read, 0, low_write, 0);
    u64 t0 = now_ms();
    /* (flushing nothing runs the handshake to its end: until application data can go) */
    if (br_sslio_flush(&ioc) < 0 || !(br_ssl_engine_current_state(&cc.eng) & BR_SSL_SENDAPP)) {
        uprintf("TLS handshake failed: %s.\n", failure());
        return 0;
    }
    uprintf("%s, certificate %s (%u ms).\n", tls_info(), check ? "checked" : "NOT checked", (u32)(now_ms() - t0));
    return 1;
}

/* all n bytes, sent: n, or -1 (said why) */
int tls_write(const void *data, int n)
{
    if (br_sslio_write_all(&ioc, data, (size_t)n) < 0 || br_sslio_flush(&ioc) < 0) {
        uprintf("TLS: %s\n", failure());
        return -1;
    }
    return n;
}

/* the next bytes (up to max): how many; 0 at the end (the server's close_notify, or its TCP close -- many end
   so); -1 (why: tls_why) */
int tls_read(u8 *buf, int max)
{
    int k = br_sslio_read(&ioc, buf, (size_t)max);
    if (k > 0)
        return k;
    int e = br_ssl_engine_last_error(&cc.eng);
    if (e == BR_ERR_OK || (e == BR_ERR_IO && peer_closed))
        return 0;
    return -1;
}

const char *tls_why(void)
{
    return failure();
}

/* the connection ended: close_notify sent when the engine still can, without waiting for the server's */
void tls_end(void)
{
    if (conn < 0)
        return;
    if (!(br_ssl_engine_current_state(&cc.eng) & BR_SSL_CLOSED)) {
        br_ssl_engine_close(&cc.eng);
        for (int i = 0; i < 4 && (br_ssl_engine_current_state(&cc.eng) & BR_SSL_SENDREC); i++) {
            size_t len;
            unsigned char *buf = br_ssl_engine_sendrec_buf(&cc.eng, &len);
            int w = tcp_send(conn, buf, (int)len);
            if (w <= 0)
                break;
            tcp_push(conn);
            br_ssl_engine_sendrec_ack(&cc.eng, (size_t)w);
        }
    }
    conn = -1;
}
