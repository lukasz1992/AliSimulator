/* What the v4 demo does with TCP and UDP (tcp.c, eth.c):
     - 'wget [-O n:path] <http://host[:port]/path>': an HTTP/1.0 GET, the
       body printed (or saved to a FAT volume through disk_write_file);
     - a telnet console on port 23 (one client): everything printed goes to
       it as well (uart_putc -> telnet_putc), what it types reaches the
       command line as if typed on the UART (uart.c's injection queue, which
       uart_getc reads after the UART: ping's and cat's Ctrl-C see it too);
     - network loading on port 6464: a new build into RAM and run, instead
       of a flash (tools/maciej_netload.py);
     - the time of day from NTP: once after DHCP, and with 'ntp [server]';
       'date' shows it in UTC and in Polish time (CET / CEST);
     - the FTP server (ftp.c) and the web page (http.c).
   eth_poll calls net_poll on its passes (the main loop): the servers run
   there; a command's waits call eth_service.
   On the box: a telnet client's typed character is echoed in 2 ms; a
   netload of the 296 KB demo is received in 0.11 s, the new demo's banner
   is on the UART 0.55 s after the command, its DHCP address 6.3 s after
   (the display set-up and the USB enumeration come first). */
#include "hw.h"

#define TELNET_PORT  23
#define NETLOAD_PORT 6464
#define NTP_PORT     49154              /* our UDP port for NTP's answers (DNS has 49153) */
#define NTP_DEFAULT  "pool.ntp.org"
#define NETLOAD_MAGIC 0x444C344Du       /* "M4LD" read little-endian */
#define LOAD_ADDRESS 0x80000200u        /* link.ld's: where the bootloader puts the demo, and its entry */

static int started;                     /* the listeners are set up */
static int in_command;                  /* main.c runs a command (not the main loop between them) */

static u32 be32(const u8 *p)
{
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

static u32 le32(const u8 *p)
{
    return (u32)p[3] << 24 | (u32)p[2] << 16 | (u32)p[1] << 8 | p[0];
}

static void put16(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}

static void put32(u8 *p, u32 v)
{
    put16(p, v >> 16);
    put16(p + 2, v);
}

static int same(const u8 *a, const u8 *b, int n)
{
    for (int i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static int str_len(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

static int prefixed(const char *s, const char *p)
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

void net_command(int running)
{
    in_command = running;
}

/* ================================================================== */
/* the telnet console                                                  */
/* ================================================================== */
enum { IAC = 255, DONT = 254, DO = 253, WONT = 252, WILL = 251, SB = 250, IP = 244, SE = 240 };
#define OPT_ECHO 1
#define OPT_SGA  3

static int tn = -1;                     /* the client's connection, -1: none */
static u8 tn_ip[4];
static u8 tn_state, tn_verb, tn_cr, tn_out_cr, tn_flushing;
static u8 tn_answered[32];              /* the options answered once already (no negotiation loops) */
static u8 tn_stuck, tn_given_up;        /* flushes in a row that moved nothing; the client was dropped for it */
static u32 tn_dropped;
static u64 tn_stalled_until, tn_polled;

/* room for n bytes in the client's send buffer: made by sending -- polling the network until the client ACKs --
   when that is safe (not inside eth / TCP code, whose frame is in use; not while a flush runs); up to 3 s, then
   (the client does not take its output) not tried again for 2 s.  A client that took nothing in three such
   flushes in a row is dropped: it would hold every command up */
static int make_room(u32 n)
{
    if (tcp_room(tn) >= n)
        return 1;
    int st = tcp_status(tn);
    if ((st != TCP_OPEN && st != TCP_PEER_DONE) || eth_inside() || tn_flushing || now_ms() < tn_stalled_until)
        return 0;
    tn_flushing = 1;
    u32 before = tcp_room(tn);
    u64 t = now_ms();
    while (tcp_room(tn) < n && now_ms() - t < 3000) {
        eth_service();
        st = tcp_status(tn);
        if (st != TCP_OPEN && st != TCP_PEER_DONE)
            break;
    }
    tn_flushing = 0;
    if (tcp_room(tn) >= n) {
        tn_stuck = 0;
        return 1;
    }
    tn_stuck = tcp_room(tn) == before ? (u8)(tn_stuck + 1) : 0;
    if (tn_stuck >= 3) {
        int c = tn;
        tn = -1;
        tcp_abort(c);
        tn_given_up = 1;                        /* (tn_poll says so) */
        return 0;
    }
    tn_stalled_until = now_ms() + 2000;
    return 0;
}

/* bytes for the client; what does not fit is dropped and a marker says so once there is room again */
static void tn_write(const void *data, u32 n)
{
    static const char mark[] = "\r\n[telnet: output was dropped here]\r\n";
    if (tn < 0)
        return;
    if (tn_dropped) {
        if (!make_room(sizeof(mark) - 1 + n)) {
            tn_dropped += n;
            return;
        }
        tcp_send(tn, mark, sizeof(mark) - 1);
        tn_dropped = 0;
    }
    if (!make_room(n)) {
        tn_dropped += n;
        return;
    }
    tcp_send(tn, data, (int)n);
}

/* uart.c: every character printed.  The box prints CR LF already; a CR alone becomes CR NUL (as telnet's network
   virtual terminal has it), a 0xFF byte IAC IAC */
void telnet_putc(char c)
{
    if (tn < 0)
        return;
    u8 b[3];
    u32 n = 0;
    if (tn_out_cr && c != '\n')
        b[n++] = 0;
    tn_out_cr = c == '\r';
    if ((u8)c == IAC)
        b[n++] = IAC;
    b[n++] = (u8)c;
    tn_write(b, n);
}

/* the client's WILL / DO: SUPPRESS-GO-AHEAD and ECHO are ours (offered at the start), anything else refused --
   each option answered once, so a client that answers answers does not start a loop */
static void tn_answer(u8 verb, u8 opt)
{
    u8 r[3] = {IAC, 0, opt};
    if (verb == WILL)
        r[1] = opt == OPT_SGA ? DO : DONT;
    else if (verb == DO && opt != OPT_ECHO && opt != OPT_SGA)
        r[1] = WONT;
    else
        return;
    if (tn_answered[opt >> 3] & (1u << (opt & 7)))
        return;
    tn_answered[opt >> 3] |= (u8)(1u << (opt & 7));
    tn_write(r, 3);
}

/* one byte from the client: telnet's commands taken out, CR LF / CR NUL / LF as one Enter, IAC IP as Ctrl-C */
static void tn_byte(u8 b)
{
    switch (tn_state) {
    case 1:                                     /* after IAC */
        tn_state = 0;
        if (b >= WILL && b <= DONT) {
            tn_verb = b;
            tn_state = 2;
        } else if (b == SB) {
            tn_state = 3;
        } else if (b == IP) {
            uart_inject(3);
        }
        return;                                 /* (IAC IAC, a 0xFF byte: not typeable here) */
    case 2:
        tn_state = 0;
        tn_answer(tn_verb, b);
        return;
    case 3:                                     /* a subnegotiation, up to IAC SE */
        if (b == IAC)
            tn_state = 4;
        return;
    case 4:
        tn_state = b == SE ? 0 : 3;
        return;
    }
    if (b == IAC) {
        tn_state = 1;
        return;
    }
    if (tn_cr) {
        tn_cr = 0;
        if (b == '\n' || b == 0)
            return;
    }
    if (b == '\r' || b == '\n') {
        tn_cr = b == '\r';
        uart_inject('\r');
        return;
    }
    uart_inject(b);
}

/* the client's bytes into the injection queue, as many as it has room for (the rest waits in TCP's buffer: its
   window closes) */
static void tn_input(void)
{
    u8 b[64];
    for (;;) {
        int room = uart_inject_room();
        if (room <= 0)
            return;
        int n = tcp_recv(tn, b, room < (int)sizeof(b) ? room : (int)sizeof(b));
        if (n <= 0)
            return;
        for (int i = 0; i < n; i++)
            tn_byte(b[i]);
    }
}

/* uart_getc, when the UART has nothing: what the client typed.  Inside a command (its own loop, which does not
   come back to the main loop) the network is polled here too, every 10 ms */
void net_uart_poll(void)
{
    int web = started && http_clients();        /* (the web page's requests go on while a command runs too) */
    if (tn < 0 && !web)
        return;
    if (in_command && !eth_inside() && !tn_flushing && now_ms() - tn_polled >= 10) {
        tn_polled = now_ms();
        eth_service();
        if (web && !eth_inside())
            (void)http_poll(1);
    }
    if (tn >= 0)
        tn_input();
}

static void tn_start(int c)
{
    static const u8 offer[] = {IAC, WILL, OPT_ECHO, IAC, WILL, OPT_SGA};
    static const char hello[] = "\r\n" APP_BANNER " " APP_VERSION ": the command line over telnet (the UART "
                                "shows the same; Ctrl-C works; close the window to leave)\r\n";
    u16 port;
    tn = c;
    tcp_peer(c, tn_ip, &port);
    tn_state = tn_verb = tn_cr = tn_out_cr = tn_stuck = 0;
    tn_dropped = 0;
    tn_stalled_until = 0;
    for (u32 i = 0; i < sizeof(tn_answered); i++)
        tn_answered[i] = 0;
    tcp_send(c, offer, sizeof(offer));
    tcp_send(c, hello, sizeof(hello) - 1);
}

/* net_poll: a client that left, a new one; a line to show */
static const char *tn_poll(void)
{
    static char said[80];
    if (tn_given_up) {
        tn_given_up = 0;
        usnprintf(said, sizeof(said), "telnet: %u.%u.%u.%u dropped: it took no output for over 10 s", tn_ip[0],
                  tn_ip[1], tn_ip[2], tn_ip[3]);
        return said;
    }
    if (tn >= 0) {
        int st = tcp_status(tn);
        if (st == TCP_PEER_DONE)
            tn_input();                         /* (what it sent before it closed still runs) */
        /* (a client that sent its command and closed its side -- printf 'ls\r\n' | nc -N box 23 -- keeps the
           console until the command line took what it sent and the command ran: its output goes back first) */
        if (st == TCP_FAILED || st == TCP_DONE
            || (st == TCP_PEER_DONE && !tcp_unread(tn) && !tn_flushing && uart_inject_room() == INJECT_ROOM
                && !in_command)) {
            int c = tn;
            tn = -1;
            if (st == TCP_FAILED)
                tcp_abort(c);
            else
                tcp_close(c);
            usnprintf(said, sizeof(said), "telnet: %u.%u.%u.%u left (%s)", tn_ip[0], tn_ip[1], tn_ip[2], tn_ip[3],
                      st == TCP_FAILED ? tcp_why(c) : "closed");
            return said;
        }
    }
    int c = tcp_accept(TELNET_PORT);
    if (c < 0)
        return 0;
    u8 ip[4];
    u16 port;
    tcp_peer(c, ip, &port);
    if (tn >= 0) {
        static const char busy[] = "the box's telnet console has a client already\r\n";
        tcp_send(c, busy, sizeof(busy) - 1);
        tcp_close(c);
        usnprintf(said, sizeof(said), "telnet: %u.%u.%u.%u refused (a client is connected)", ip[0], ip[1], ip[2],
                  ip[3]);
        return said;
    }
    tn_start(c);
    usnprintf(said, sizeof(said), "telnet: %u.%u.%u.%u connected", ip[0], ip[1], ip[2], ip[3]);
    return said;
}

/* ================================================================== */
/* netload: a new build into RAM, run                                  */
/* ================================================================== */
/* The protocol (tools/maciej_netload.py): the client connects to port 6464 and sends "M4LD", the image's length
   and its CRC-32 (zlib's), both 32-bit little-endian, then the image -- app.bin, linked at 0x80000200.  The box
   answers one line, "OK ..." or "ERR ...", and closes; after OK it runs the image as the bootloader would: copied
   to 0x80000200, the caches made to agree, a jump to its start.  The copy overwrites the running demo, so a
   small position-independent stub does it from NETLOAD_STUB_PHYS, uncached (KSEG1: no I-cache lines to worry
   about), then writes back and drops EVERY line of the D-cache and drops every line of the I-cache, by index,
   then jumps.  (Only the new image's own lines is not enough: the old demo's dirty lines elsewhere -- its BSS,
   buffers the USB controller reads or fills by DMA -- stayed in the cache and were written back over the new
   demo's data later, or read instead of what a DMA had put in RAM: web uploads right after a netload of a
   build laid out differently came out with runs of a few hundred wrong bytes.  A boot from flash never had
   it: the bootloader starts the caches afresh.)  The watchdog keeps running: the new image starts it again;
   one that hangs is reset by it, into the flashed demo. */
__asm__(".section .text\n"
        ".align 2\n"
        ".set push\n"
        ".set noreorder\n"
        ".set noat\n"
        ".globl netload_stub\n"
        ".globl netload_stub_end\n"
        "netload_stub:\n"                       /* a0 destination, a1 source, a2 bytes (4 * n, n > 0), a3 entry */
        "    move  $t1, $a0\n"
        "    addu  $t3, $a0, $a2\n"
        "1:  lw    $t0, 0($a1)\n"
        "    addiu $a1, $a1, 4\n"
        "    addiu $a2, $a2, -4\n"
        "    sw    $t0, 0($a0)\n"
        "    bnez  $a2, 1b\n"
        "    addiu $a0, $a0, 4\n"
        "    lui   $t1, 0x8000\n"               /* every line of both caches, by index: KSEG0 0x80000000.. */
        "    lui   $t3, 0x8001\n"               /* ..0x80010000 (64 KB, 16-byte steps: at least any cache's size */
        "2:  cache 0x01, 0($t1)\n"              /* and ways, at most its line) -- Index_Writeback_Inv_D */
        "    cache 0x00, 0($t1)\n"              /* Index_Invalidate_I */
        "    addiu $t1, $t1, 16\n"
        "    sltu  $t0, $t1, $t3\n"
        "    bnez  $t0, 2b\n"
        "    nop\n"
        "    sync\n"
        "    .word 0x000000c0\n"                /* ehb (sll $0, $0, 3: a nop before MIPS32 R2) */
        "    .word 0x00e00408\n"                /* jr.hb $a3: the instruction hazard barrier after the cache ops
                                                   (-march=mips32 does not know it; the 24K is MIPS32 R2) */
        "    nop\n"
        "netload_stub_end:\n"
        ".set pop\n");
extern const u32 netload_stub[], netload_stub_end[];

static u32 crc_table[256];

static u32 crc32(const u8 *p, u32 n)            /* zlib's CRC-32 (reflected 0xEDB88320) */
{
    if (!crc_table[1])
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    u32 c = 0xFFFFFFFFu;
    for (u32 i = 0; i < n; i++)
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

static void run_image(u32 len) __attribute__((noreturn));
static void run_image(u32 len)
{
    if (tn >= 0) {                              /* the telnet client told, and let go */
        tcp_close(tn);
        tn = -1;
        u64 t = now_ms();
        while (now_ms() - t < 300)
            eth_service();
    }
    uart_flush();
    usb_in_stop();                              /* no transfer under way: endpoint B's IN (keyboard / mouse), */
    usb_epd_close();                            /* endpoint D's polling (the mouse) */
    dog_feed();
    volatile u32 *to = (volatile u32 *)KSEG1(NETLOAD_STUB_PHYS);
    for (const u32 *p = netload_stub; p < netload_stub_end; p++)
        *to++ = *p;
    __asm__ volatile("sync" ::: "memory");      /* (the stub's uncached writes done before it runs) */
    u32 status;
    __asm__ volatile("mfc0 %0, $12" : "=r"(status));
    __asm__ volatile("mtc0 %0, $12\n\tnop\n\tnop\n\tnop" : : "r"(status & ~1u));   /* (interrupts off: they are) */
    ((void (*)(u32, u32, u32, u32))KSEG1(NETLOAD_STUB_PHYS))(LOAD_ADDRESS, KSEG0(NETLOAD_PHYS), (len + 3) & ~3u,
                                                             LOAD_ADDRESS);
    for (;;) { }
}

static void nl_answer(int c, const char *text)
{
    tcp_send(c, text, str_len(text));
    tcp_push(c);
}

/* n bytes from the connection into `to`: 1, or 0 (*why) -- it closed, failed, sent nothing for 5 s, or was not
   done by `deadline` (the main loop waits meanwhile: a client trickling a byte every 4 s must not hold it) */
static int nl_read(int c, u8 *to, u32 n, u64 deadline, const char **why)
{
    u32 got = 0;
    u64 last = now_ms();
    while (got < n) {
        if (now_ms() > deadline) {
            *why = "too slow (under 100 KB/s)";
            return 0;
        }
        int k = tcp_recv(c, to + got, (int)(n - got));
        if (k > 0) {
            got += (u32)k;
            last = now_ms();
            continue;
        }
        int st = tcp_status(c);
        if (st != TCP_OPEN) {
            *why = st == TCP_FAILED ? tcp_why(c) : "the client closed early";
            return 0;
        }
        if (now_ms() - last > 5000) {
            *why = "nothing came for 5 s";
            return 0;
        }
        eth_service();
    }
    return 1;
}

/* a client on the netload port (net_poll, the main loop): the image received, checked, run -- or a line saying
   why not */
static const char *netload(int c)
{
    static char said[112];
    u8 head[12], ip[4];
    u16 port;
    const char *why = 0;
    u8 *image = (u8 *)KSEG0(NETLOAD_PHYS);
    tcp_peer(c, ip, &port);
    u64 t0 = now_ms();
    uprintf("\r\x1b[K[eth] netload: %u.%u.%u.%u connected, receiving...\n", ip[0], ip[1], ip[2], ip[3]);
    u32 len = 0, crc = 0;
    if (nl_read(c, head, sizeof(head), t0 + 10000, &why)) {
        len = le32(head + 4);
        crc = le32(head + 8);
        if (le32(head) != NETLOAD_MAGIC)
            why = "not a netload header (\"M4LD\", length, CRC-32)";
        else if (len < 64 || len > NETLOAD_MAX)
            why = "the length is not that of an image (64 bytes .. 7 MB)";
    }
    if (!why && nl_read(c, image, len, now_ms() + 10000 + len / 100, &why)) {      /* (100 bytes a ms) */
        u32 got = crc32(image, len);
        if (got != crc)
            why = "the CRC-32 does not match (the transfer spoilt it?)";
        else if (le32(image) != 0x40086000u)
            why = "it does not start as start.S does (mfc0 $t0, $12): not a demo image";
    }
    u32 ms = (u32)(now_ms() - t0);
    if (why) {
        char line[96];
        usnprintf(line, sizeof(line), "ERR %s\n", why);
        nl_answer(c, line);
        tcp_close(c);
        usnprintf(said, sizeof(said), "netload: %s (%u.%u.%u.%u)", why, ip[0], ip[1], ip[2], ip[3]);
        return said;
    }
    char line[96];
    usnprintf(line, sizeof(line), "OK %u bytes, CRC-32 %08x, received in %u ms: starting it\n", len, crc, ms);
    nl_answer(c, line);
    tcp_shutdown(c);
    u64 t = now_ms();
    while (now_ms() - t < 1500 && tcp_status(c) != TCP_DONE && tcp_status(c) != TCP_FAILED)
        eth_service();                          /* (the answer ACKed and the connection closed, both ways) */
    uprintf("[eth] netload: %u bytes (CRC-32 %08x) in %u ms (%u KB/s): starting the new image at 0x%08x\n", len,
            crc, ms, ms ? len / ms * 1000 / 1024 : 0, LOAD_ADDRESS);
    run_image(len);
}

/* ================================================================== */
/* NTP and the time of day                                             */
/* ================================================================== */
static u8 ntp_frame[14 + 20 + 8 + 48 + 2];
static u8 ntp_server[4];
static u32 ntp_cookie;                  /* our transmit time stamp's first word: the answer's originate */
static int ntp_got;
static u32 ntp_tx_s, ntp_tx_frac;
static u8 ntp_stratum;
static u64 ntp_sent_at, ntp_got_at;     /* CP0 Count ticks */

static int synced;                      /* the clock is set: UTC = uptime + off_s s + off_ms ms */
static u32 off_s, off_ms, synced_at;
static char synced_from[40];

/* a request to `ip` (ARP first): 0 sent, -1 not */
static int ntp_send(const u8 ip[4])
{
    u8 mac[6];
    if (!eth_route(ip, mac))
        return -1;
    u8 *u = ntp_frame + 34, *n = u + 8;
    for (int i = 0; i < 48; i++)
        n[i] = 0;
    n[0] = 0x23;                                /* no leap warning, version 4, mode 3 (client) */
    ntp_cookie = cp0_count() | 1;
    put32(n + 40, ntp_cookie);                  /* (SNTP: the server copies it into its originate time stamp) */
    put16(u, NTP_PORT);
    put16(u + 2, 123);
    put16(u + 4, 8 + 48);
    put16(u + 6, 0);
    for (int i = 0; i < 4; i++)
        ntp_server[i] = ip[i];
    ntp_got = 0;
    ntp_sent_at = now_ticks();
    return eth_send_ipv4(ntp_frame, mac, ip, 17, 8 + 48);
}

/* eth.c: a UDP datagram to our address (not DHCP's, not DNS's): an NTP server's answer to our request is kept
   (called inside eth's handle: nothing printed, nothing sent) */
void net_udp_input(const u8 *src, u16 sport, u16 dport, const u8 *data, int len)
{
    if (dport != NTP_PORT || sport != 123 || len < 48 || !same(src, ntp_server, 4))
        return;
    if ((data[0] & 7) != 4 || data[1] == 0 || be32(data + 24) != ntp_cookie)
        return;                                 /* (not a server's answer to it; stratum 0: a kiss-o'-death) */
    ntp_tx_s = be32(data + 40);
    ntp_tx_frac = be32(data + 44);
    ntp_stratum = data[1];
    ntp_got_at = now_ticks();
    ntp_got = 1;
}

/* the uptime as whole seconds and the milliseconds after them (time.c keeps both from the same count) */
static void uptime_now(u32 *s, u32 *ms)
{
    u32 a, b;
    do {
        a = (u32)now_ms();
        *s = uptime_s();
        b = (u32)now_ms();
    } while (a != b);
    *ms = a - *s * 1000;
}

static u32 clock_now(void)              /* UTC, seconds since 1970 */
{
    u32 s, ms;
    uptime_now(&s, &ms);
    return s + off_s + (ms + off_ms) / 1000;
}

/* the answer sets the clock: its transmit time, plus half the round trip, plus the time since it came.  NTP's
   seconds count from 1900 and wrap in 2036: with the top bit clear they are the next era's (RFC 4330) */
static u32 ntp_set(const char *server)
{
    u32 tpm = ticks_per_ms();
    u32 rtt = (u32)(ntp_got_at - ntp_sent_at) / tpm, since = (u32)(now_ticks() - ntp_got_at) / tpm;
    u32 unix_s = ntp_tx_s & 0x80000000u ? ntp_tx_s - 2208988800u : ntp_tx_s + 2085978496u;
    u32 ms = ((ntp_tx_frac >> 16) * 1000 >> 16) + rtt / 2 + since;
    unix_s += ms / 1000;
    ms %= 1000;
    u32 s, lms;
    uptime_now(&s, &lms);
    off_s = unix_s - s;
    if (ms >= lms) {
        off_ms = ms - lms;
    } else {
        off_ms = ms + 1000 - lms;
        off_s--;
    }
    synced = 1;
    synced_at = s;
    int i = 0;
    for (; server[i] && i < (int)sizeof(synced_from) - 1; i++)
        synced_from[i] = server[i];
    synced_from[i] = 0;
    return rtt;
}

/* ---- the calendar (proleptic Gregorian, from 1970; H. Hinnant's days <-> civil date) ---- */
static void civil(u32 days, u32 *y, u32 *m, u32 *d)
{
    u32 z = days + 719468, era = z / 146097, doe = z - era * 146097;
    u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    u32 doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yoe + era * 400 + (*m <= 2);
}

static u32 days_of(u32 y, u32 m, u32 d)
{
    y -= m <= 2;
    u32 era = y / 400, yoe = y - era * 400;
    u32 doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

/* Europe/Warsaw: CEST (UTC+2) from the last Sunday of March 01:00 UTC to the last Sunday of October 01:00 UTC,
   CET (UTC+1) otherwise -- the EU's rule */
static int summer_time(u32 t)
{
    u32 y, m, d;
    civil(t / 86400, &y, &m, &d);
    u32 march = days_of(y, 3, 31), october = days_of(y, 10, 31);
    march -= (march + 4) % 7;                   /* the last Sundays (day 0, 1970-01-01, was a Thursday) */
    october -= (october + 4) % 7;
    return t >= march * 86400 + 3600 && t < october * 86400 + 3600;
}

static int time_text(char *buf, int size, u32 t, const char *zone, int weekday)
{
    static const char *const days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    u32 y, m, d, s = t % 86400;
    civil(t / 86400, &y, &m, &d);
    return usnprintf(buf, size, "%s%s%04u-%02u-%02u %02u:%02u:%02u %s", weekday ? days[(t / 86400 + 4) % 7] : "",
                     weekday ? " " : "", y, m, d, s / 3600, s / 60 % 60, s % 60, zone);
}

static int warsaw_text(char *buf, int size, u32 t, int weekday)
{
    int summer = summer_time(t);
    return time_text(buf, size, t + (summer ? 7200 : 3600), summer ? "CEST" : "CET", weekday);
}

u32 net_clock_utc(void)                 /* disk.c: the time what it makes is dated with */
{
    return synced ? clock_now() : 0;
}

int ntp_clock_text(char *buf, int size)          /* the TV's SYSTEM box */
{
    if (!synced)
        return 0;
    warsaw_text(buf, size, clock_now(), 0);
    return 1;
}

/* the clock set after DHCP's first address, without holding the main loop up: the DNS question, then the NTP
   request, each answer looked for on the next passes; three tries */
static int auto_state, auto_tries;      /* 0 waiting for DHCP, 1 due, 2 DNS asked, 3 NTP asked, 4 done */
static u64 auto_at;
static u8 auto_ip[4];

static const char *auto_retry(u64 now)
{
    if (++auto_tries >= 3) {
        auto_state = 4;
        return "ntp: no answer from " NTP_DEFAULT ": the clock is not set ('ntp' asks again)";
    }
    auto_state = 1;
    auto_at = now + 3000;
    return 0;
}

static const char *ntp_auto(void)
{
    static char said[96];
    u64 now = now_ms();
    if (synced && auto_state != 4)
        auto_state = 4;                         /* ('ntp' set it first) */
    switch (auto_state) {
    case 0:
        if (eth_bound_count()) {
            auto_state = 1;
            auto_at = now + 300;
        }
        return 0;
    case 1:
        if (now < auto_at)
            return 0;
        if (eth_dns_start(NTP_DEFAULT) < 0)
            return auto_retry(now);
        auto_state = 2;
        auto_at = now + 2000;
        return 0;
    case 2: {
        int r = eth_dns_result(auto_ip);
        if (r == 1 && ntp_send(auto_ip) == 0) {
            auto_state = 3;
            auto_at = now + 2000;
        } else if (r || now >= auto_at) {
            return auto_retry(now);
        }
        return 0;
    }
    case 3:
        if (ntp_got) {
            u32 rtt = ntp_set(NTP_DEFAULT);
            auto_state = 4;
            int n = usnprintf(said, sizeof(said), "ntp: the clock is set from " NTP_DEFAULT " (%u ms): ", rtt);
            warsaw_text(said + n, (int)sizeof(said) - n, clock_now(), 1);
            return said;
        }
        if (now >= auto_at)
            return auto_retry(now);
        return 0;
    }
    return 0;
}

static void show_date(void)
{
    char a[48];
    u32 t = clock_now();
    time_text(a, sizeof(a), t, "UTC", 1);
    uprintf("%s\n", a);
    warsaw_text(a, sizeof(a), t, 1);
    uprintf("%s (Europe/Warsaw)\n", a);
}

void cmd_ntp(int argc, char **argv)
{
    const char *server = argc > 1 ? argv[1] : NTP_DEFAULT;
    u8 ip[4];
    if (!eth_lookup(server, ip, "ntp"))
        return;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (ntp_send(ip) < 0) {
            uprintf("ntp: %s: no ARP answer on the way there\n", server);
            return;
        }
        u64 t = now_ms();
        while (!ntp_got && now_ms() - t < 1500) {
            eth_service();
            if (uart_getc() == 3) {
                uprintf("^C\n");
                return;
            }
        }
        if (ntp_got) {
            u32 rtt = ntp_set(server);
            uprintf("ntp: %s (%u.%u.%u.%u), stratum %u, round trip %u ms: the clock is set\n", server, ip[0], ip[1],
                    ip[2], ip[3], ntp_stratum, rtt);
            show_date();
            return;
        }
    }
    uprintf("ntp: no answer from %s (%u.%u.%u.%u)\n", server, ip[0], ip[1], ip[2], ip[3]);
}

void cmd_date(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (!synced) {
        uprintf("date: not synced -- no NTP answer yet ('ntp [server]' asks; up %u s)\n", uptime_s());
        return;
    }
    show_date();
    u32 ago = uptime_s() - synced_at;
    uprintf("(set from %s by NTP %u min %u s ago)\n", synced_from, ago / 60, ago % 60);
}

/* ================================================================== */
/* wget                                                                */
/* ================================================================== */
static struct {
    char head[512];                     /* the response's header (its start, as much as fits) */
    u32 head_len;
    int newlines, in_body, cr;
    u8 *store;                          /* -O: the body goes here (WGET_PHYS), else to the console */
    u32 body, stored;
} wg;

/* a header line's value ("content-length" -> "1256"), 0 when the header has none */
static const char *header_value(const char *name, char *out, int size)
{
    int nl = str_len(name);
    for (u32 i = 0; i + (u32)nl < wg.head_len; i++) {
        if (i && wg.head[i - 1] != '\n')
            continue;
        int k = 0;
        while (k < nl && lower(wg.head[i + k]) == name[k])
            k++;
        if (k < nl || wg.head[i + k] != ':')
            continue;
        u32 p = i + (u32)k + 1;
        while (p < wg.head_len && wg.head[p] == ' ')
            p++;
        int n = 0;
        while (p < wg.head_len && wg.head[p] != '\r' && wg.head[p] != '\n' && n < size - 1)
            out[n++] = wg.head[p++];
        out[n] = 0;
        return out;
    }
    return 0;
}

/* the header is over: its status line ("HTTP/1.1 200 OK" -> "200 OK"), the length and type */
static void header_done(const char *out)
{
    u32 i = 0;
    while (i < wg.head_len && wg.head[i] != ' ' && wg.head[i] != '\n')
        i++;
    char status[48];
    int n = 0;
    while (++i < wg.head_len && wg.head[i] != '\r' && wg.head[i] != '\n' && n < (int)sizeof(status) - 1)
        status[n++] = wg.head[i];
    status[n] = 0;
    uprintf("%s\n", n ? status : "(no status line)");
    char length[16], type[48];
    const char *l = header_value("content-length", length, sizeof(length));
    const char *t = header_value("content-type", type, sizeof(type));
    uprintf("Length: %s%s%s%s\n", l ? l : "unspecified", t ? " [" : "", t ? t : "", t ? "]" : "");
    if (out)
        uprintf("Saving to: '%s'\n", out);
    else
        uprintf("\n");
}

static void body_byte(u8 b)
{
    wg.body++;
    if (wg.store) {
        if (wg.stored < WGET_MAX)
            wg.store[wg.stored++] = b;
        return;
    }
    if (b == '\n') {                            /* (a bare LF as CR LF for the terminal) */
        if (!wg.cr)
            uart_putc('\r');
        uart_putc('\n');
    } else {
        uart_putc(b == '\r' || b == '\t' || (b >= 0x20 && b != 0x7F) ? (char)b : '.');
    }
    wg.cr = b == '\r';
}

/* received bytes: the header until its empty line, then the body */
static void wget_bytes(const u8 *p, int n, const char *out)
{
    for (int i = 0; i < n; i++) {
        if (wg.in_body) {
            body_byte(p[i]);
            continue;
        }
        if (wg.head_len < sizeof(wg.head) - 1)
            wg.head[wg.head_len++] = (char)p[i];
        if (p[i] == '\n') {
            if (++wg.newlines == 2) {
                wg.in_body = 1;
                header_done(out);
            }
        } else if (p[i] != '\r') {
            wg.newlines = 0;
        }
    }
}

/* "http://host[:port]/path" -> host, port, path: 1, or 0 (a line said why) */
static int parse_url(const char *url, char *host, int host_size, u16 *port, const char **path)
{
    if (prefixed(url, "https://")) {
        uprintf("wget: https:// is not supported: there is no TLS on this box (try http://)\n");
        return 0;
    }
    if (prefixed(url, "http://"))
        url += 7;
    for (const char *s = url; *s && *s != '/'; s++)
        if (s[0] == ':' && s[1] == '/' && s[2] == '/') {
            uprintf("wget: only http:// URLs\n");
            return 0;
        }
    int n = 0;
    while (*url && *url != ':' && *url != '/' && n < host_size - 1)
        host[n++] = *url++;
    host[n] = 0;
    u32 p = 80;
    if (*url == ':') {
        p = 0;
        url++;
        while (*url >= '0' && *url <= '9' && p <= 65535)
            p = p * 10 + (u32)(*url++ - '0');
    }
    if (!n || !p || p > 65535 || (*url && *url != '/')) {
        uprintf("wget: not a URL I can read: http://host[:port]/path\n");
        return 0;
    }
    *port = (u16)p;
    *path = *url ? url : "/";
    return 1;
}

/* the connection to the server, open: 1, or 0 (said why) */
static int wget_connect(int c)
{
    u64 t = now_ms();
    for (;;) {
        int st = tcp_status(c);
        if (st == TCP_OPEN)
            return 1;
        if (st != TCP_CONNECTING) {
            uprintf("failed: %s.\n", tcp_why(c));
            return 0;
        }
        if (now_ms() - t > 10000) {
            uprintf("failed: no answer in 10 s.\n");
            return 0;
        }
        if (uart_getc() == 3) {
            uprintf("^C\n");
            return 0;
        }
        eth_service();
    }
}

/* 'wget [-O n:path] <http://host[:port]/path>': HTTP/1.0 GET; the status line, then the body on the console --
   or saved (the body held in RAM at WGET_PHYS, up to 8 MB, then written as 'command > path' writes); Ctrl-C
   stops it, so does 15 s without data; the bytes, the time and the rate at the end */
void cmd_wget(int argc, char **argv)
{
    static const char stopped[] = "stopped";
    const char *url = 0, *out = 0, *path;
    int bad = 0;
    for (int i = 1; i < argc && !bad; i++) {
        if (argv[i][0] == '-' && argv[i][1] == 'O' && !argv[i][2])
            bad = !(i + 1 < argc && (out = argv[++i]));
        else if (argv[i][0] != '-' && !url)
            url = argv[i];
        else
            bad = 1;
    }
    if (bad || !url) {
        uprintf("usage: wget [-O n:path] <http://host[:port]/path>\n");
        return;
    }
    char host[64], ip_text[16];
    u16 port;
    u8 ip[4];
    if (!parse_url(url, host, sizeof(host), &port, &path) || !eth_lookup(host, ip, "wget"))
        return;
    usnprintf(ip_text, sizeof(ip_text), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    uprintf("Connecting to %s (%s):%u... ", host, ip_text, port);
    int c = tcp_connect(ip, port);
    if (c < 0) {
        uprintf("failed: %s.\n", eth_ready() ? "no ARP answer on the way there" : "eth0 has no address");
        return;
    }
    if (!wget_connect(c)) {
        tcp_abort(c);
        return;
    }
    uprintf("connected.\n");
    char req[256];
    int n = port == 80 ? usnprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\n", path, host)
                       : usnprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s:%u\r\n", path, host, port);
    n += usnprintf(req + n, (int)sizeof(req) - n, "User-Agent: m3801-wget/" APP_VERSION "\r\nAccept: */*\r\n"
                   "Connection: close\r\n\r\n");
    if (n >= (int)sizeof(req)) {
        uprintf("wget: the URL is too long\n");
        tcp_abort(c);
        return;
    }
    tcp_send(c, req, n);
    tcp_push(c);
    uprintf("HTTP request sent, awaiting response... ");
    u8 *mem = (u8 *)&wg;
    for (u32 i = 0; i < sizeof(wg); i++)
        mem[i] = 0;
    wg.store = out ? (u8 *)KSEG0(WGET_PHYS) : 0;
    u64 t0 = now_ms(), last = t0, shown = t0;
    const char *why = 0;
    int progress = 0;
    static u8 buf[2048];
    for (;;) {
        int k = tcp_recv(c, buf, sizeof(buf));
        if (k > 0) {
            last = now_ms();
            wget_bytes(buf, k, out);
            if (wg.store && wg.body > WGET_MAX) {
                why = "longer than 8 MB: its first 8 MB kept";
                break;
            }
            if (wg.store && wg.in_body && last - shown >= 1000) {
                shown = last;                   /* (saving: a line that counts, once a second) */
                uprintf("\r  %u KB", wg.body / 1024);
                progress = 1;
            }
        } else {
            int st = tcp_status(c);
            if (st == TCP_FAILED) {
                why = tcp_why(c);
                break;
            }
            if (st != TCP_OPEN)
                break;                          /* the server closed: all there */
            if (now_ms() - last > 15000) {
                why = "no data for 15 s";
                break;
            }
            eth_service();
        }
        if (uart_getc() == 3) {
            uprintf("^C\n");
            why = stopped;
            break;
        }
    }
    u32 ms = (u32)(now_ms() - t0);
    if (why)
        tcp_abort(c);
    else
        tcp_close(c);
    if (progress)
        uprintf("\n");
    if (!wg.in_body)
        uprintf("%s\n", wg.head_len ? "(the header did not end)" : "no answer");
    else if (!wg.store && !wg.cr && wg.body)
        uprintf("\n");
    u32 rate = !ms ? 0 : wg.body >= 4000000 ? wg.body / ms * 1000 / 1024 : wg.body * 1000 / 1024 / ms;
    uprintf("%s%u bytes in %u.%03u s (%u KB/s)%s%s\n", wg.store ? "" : "--\n", wg.body, ms / 1000, ms % 1000, rate,
            why ? ": " : "", why ? why : "");
    if (wg.store && why == stopped) {
        uprintf("nothing saved\n");             /* (Ctrl-C: the file is not wanted; a time-out keeps what came) */
    } else if (wg.store && wg.in_body) {
        uprintf("writing %u bytes to %s...\n", wg.stored, out);
        disk_write_file(out, wg.store, wg.stored, 0);
        uprintf("done\n");
    }
}

/* ================================================================== */
/* the servers, from eth_poll                                          */
/* ================================================================== */
const char *net_poll(void)
{
    if (!eth_ready()) {
        /* no address (the adapter unplugged, DHCP not answered): nothing comes in, but what the network's
           going cut off is still to be ended -- tcp.c failed the connections, and a server that never looked
           kept its state: an upload left open held its volume's writer ("busy") until an address came back */
        static u64 looked;
        const char *s = 0;
        if (started && now_ms() - looked >= 100) {
            looked = now_ms();
            s = tn_poll();
            if (!s)
                s = ftp_poll();
            if (!s)
                s = http_poll(0);
        }
        return s;
    }
    if (!started) {
        started = 1;
        tcp_listen(TELNET_PORT);
        tcp_listen(NETLOAD_PORT);
    }
    const char *s = tn_poll();
    if (s)
        return s;
    int c = tcp_accept(NETLOAD_PORT);
    if (c >= 0)
        return netload(c);
    s = ftp_poll();
    if (s)
        return s;
    s = http_poll(0);
    if (s)
        return s;
    return ntp_auto();
}

void cmd_netstat(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    tcp_list();
    if (tn >= 0)
        uprintf("telnet console (port %u): %u.%u.%u.%u connected\n", TELNET_PORT, tn_ip[0], tn_ip[1], tn_ip[2],
                tn_ip[3]);
    else
        uprintf("telnet console (port %u): no client\n", TELNET_PORT);
    uprintf("netload (port %u): python tools/maciej_netload.py <this box's address> [app.bin]\n", NETLOAD_PORT);
    ftp_status();
    http_status();
}
