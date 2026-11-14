/* Wired Ethernet through a USB adapter's CDC ECM function -- the RTL8153's
   second configuration (usb.c chooses it), or any CDC ECM device: Ethernet
   frames as they are, over a bulk IN and a bulk OUT endpoint (a frame ends
   with a short packet).  On the box an RTL8153 behind a hub got its link
   1.6 s after the configuration and a DHCP offer from the router at once.

   A small IPv4 stack on it: DHCP (the address, mask, router and DNS server
   from the network's server, at the start), ARP (answered for our address;
   asked for a host or the router), ICMP echo (answered: the box can be
   pinged; 'ping [-c n] <host>' sends them like Linux's ping), DNS (an A
   record asked of the DHCP's DNS server for the names ping, wget and ntp
   get).  The frames are read in the main loop every 20 ms (eth_poll),
   through endpoint B -- which the keyboard's IN shares: usb.c keeps a
   report that came while a frame was read.  TCP segments go to tcp.c, UDP
   datagrams for other ports to net.c (NTP); while a TCP connection is busy
   the frames are read on every main loop pass (on the box: netload took
   296 KB in 0.11 s, wget 1 MB from the internet in 1.3 s).

   Not re-entrant: a frame is read into the one rx[] and handled from there,
   so the code that reads frames (take, eth_poll) counts itself in `depth`
   and eth_service refuses to run inside it (the telnet console's output
   could otherwise poll the network from a uprintf in the middle of it). */
#include "hw.h"

#define FRAME_MAX 1514
#define POLL_MS   20
#define POLL_MOUSE_MS 250       /* (while a mouse read through endpoint B moves: each read costs it a toggle set-up) */

static struct {
    u8 used, device, ctrl_if, in_ep, out_ep;
    u16 maxp;
    u8 mac[6];
    u8 ip[4], mask[4], router[4], dns[4], server[4], offered[4];
    u32 lease;
    int dhcp;                           /* 0 none, 1 discovering, 2 requesting, 3 bound, 4 given up */
    int dhcp_tries;
    u32 xid;
    u64 dhcp_next, last_poll;
    u32 rx, tx, rx_ignored;
    u32 rx_ours, rx_multi;              /* frames to our MAC; broadcasts and multicasts */
    u32 dhcp_ours, dhcp_multi;          /* DHCP answers: sent to our MAC, broadcast */
    u8 again;                           /* the packet filter was set again while DHCP had no answer */
    u16 ip_id;
    u32 bound;                          /* DHCP acknowledgements so far (net.c sets the clock after the first) */
    const char *pending;                /* a line from a frame handled outside eth_poll, for its next pass */
} eth;
static int depth;                       /* frames being read / handled (rx[] in use) */
static u8 revived, reviving;            /* the adapter was set up again while DHCP had no answer (once, until
                                           it has one or another is plugged in); that is under way */

static u8 rx[FRAME_MAX + 512], tx[FRAME_MAX + 2];
static char said[96];

#define ARP_ENTRIES 8
static struct {
    u8 used, ip[4], mac[6];
} arp_table[ARP_ENTRIES];
static u8 arp_next;

/* ping's state: the reply eth_poll saw; an ICMP error about one of our requests (its type, code, sequence) */
static u16 ping_id, ping_seq_seen, ping_error_seq;
static u64 ping_seen_at;
static u8 ping_from[4], ping_ttl, ping_error_type, ping_error_code, ping_error_from[4];

/* a DNS question's answer: its id, the address (got 1), or no such name (got -1) */
#define DNS_PORT 49153
static u16 dns_id;
static int dns_got;
static u8 dns_answer[4];

static const u8 bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static int same(const u8 *a, const u8 *b, int n)
{
    for (int i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static void copy(u8 *to, const u8 *from, int n)
{
    for (int i = 0; i < n; i++)
        to[i] = from[i];
}

static u16 be16(const u8 *p)
{
    return (u16)(p[0] << 8 | p[1]);
}

static void put16(u8 *p, u16 v)
{
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}

static u16 checksum(const u8 *p, int n)
{
    u32 sum = 0;
    for (int i = 0; i + 1 < n; i += 2)
        sum += (u32)(p[i] << 8 | p[i + 1]);
    if (n & 1)
        sum += (u32)(p[n - 1] << 8);
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

static int hexval(u8 c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                                                                                          : -1;
}

/* "a.b.c.d": 1 and the four bytes */
int eth_parse_ip(const char *s, u8 ip[4])
{
    for (int k = 0; k < 4; k++) {
        u32 v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9' && digits < 4) {
            v = v * 10 + (u32)(*s++ - '0');
            digits++;
        }
        if (!digits || v > 255 || (k < 3 && *s++ != '.'))
            return 0;
        ip[k] = (u8)v;
    }
    return !*s;
}

static int ip_text(char *buf, int size, const u8 *ip)
{
    return usnprintf(buf, size, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

/* frames for us and broadcasts (SET_ETHERNET_PACKET_FILTER: directed | broadcast) */
static void set_filter(void)
{
    (void)usb_request(eth.device, 0x21, 0x43, 0x000C, eth.ctrl_if, 0, 0, 500);
}

/* ---- frames ---- */
static int send_frame(u8 *f, int len)
{
    while (len < 60)                            /* (the shortest Ethernet frame, without its CRC) */
        f[len++] = 0;
    if (!(len % eth.maxp))
        f[len++] = 0;                           /* (a whole number of packets would need a zero-length one) */
    if (usb_bulk_out(eth.device, eth.out_ep, eth.maxp, f, (u32)len, 200) < 0)
        return -1;
    eth.tx++;
    return 0;
}

/* one frame: its packets until a short one -- its length, 0 none in time, -1 failed */
static int receive(u32 wait_ms)
{
    int len = 0;
    for (;;) {
        int k = usb_bulk_in_packet(eth.device, eth.in_ep, eth.maxp, rx + len, len ? 20 : wait_ms);
        if (k == -1)
            return -1;
        if (k <= 0)
            return len;                         /* (none in time; a zero-length packet ends a frame too) */
        len += k;
        if (k < eth.maxp || len + eth.maxp > (int)sizeof(rx))
            return len;
    }
}

static u8 *ether(u8 *f, const u8 *to, u16 type)
{
    copy(f, to, 6);
    copy(f + 6, eth.mac, 6);
    put16(f + 12, type);
    return f + 14;
}

/* an IPv4 header (20 bytes) for `len` bytes after it; its end */
static u8 *ipv4(u8 *h, const u8 *from, const u8 *to, u8 proto, int len)
{
    h[0] = 0x45;
    h[1] = 0;
    put16(h + 2, (u16)(20 + len));
    put16(h + 4, ++eth.ip_id);
    h[6] = h[7] = 0;
    h[8] = 64;
    h[9] = proto;
    h[10] = h[11] = 0;
    copy(h + 12, from, 4);
    copy(h + 16, to, 4);
    put16(h + 10, checksum(h, 20));
    return h + 20;
}

/* ---- ARP ---- */
static void arp_learn(const u8 *ip, const u8 *mac)
{
    for (int i = 0; i < ARP_ENTRIES; i++)
        if (arp_table[i].used && same(arp_table[i].ip, ip, 4)) {
            copy(arp_table[i].mac, mac, 6);
            return;
        }
    copy(arp_table[arp_next].ip, ip, 4);
    copy(arp_table[arp_next].mac, mac, 6);
    arp_table[arp_next].used = 1;
    arp_next = (u8)((arp_next + 1) % ARP_ENTRIES);
}

static const u8 *arp_find(const u8 *ip)
{
    for (int i = 0; i < ARP_ENTRIES; i++)
        if (arp_table[i].used && same(arp_table[i].ip, ip, 4))
            return arp_table[i].mac;
    return 0;
}

static void arp_send(u16 op, const u8 *to_mac, const u8 *target_mac, const u8 *target_ip)
{
    u8 *a = ether(tx, to_mac, 0x0806);
    put16(a, 1);                                /* Ethernet */
    put16(a + 2, 0x0800);                       /* IPv4 */
    a[4] = 6;
    a[5] = 4;
    put16(a + 6, op);
    copy(a + 8, eth.mac, 6);
    copy(a + 14, eth.ip, 4);
    copy(a + 18, target_mac, 6);
    copy(a + 24, target_ip, 4);
    send_frame(tx, 14 + 28);
}

/* ---- DHCP ---- */
static void dhcp_send(int type)
{
    static const u8 zero[4];
    u8 *h = ether(tx, bcast, 0x0800);
    u8 *u = h + 20, *b = u + 8;
    for (int i = 0; i < 300; i++)
        b[i] = 0;
    b[0] = 1;                                   /* BOOTREQUEST, Ethernet, 6-byte addresses */
    b[1] = 1;
    b[2] = 6;
    b[4] = (u8)(eth.xid >> 24);
    b[5] = (u8)(eth.xid >> 16);
    b[6] = (u8)(eth.xid >> 8);
    b[7] = (u8)eth.xid;
    b[10] = 0x80;                               /* the answer broadcast: no address yet */
    copy(b + 28, eth.mac, 6);
    b[236] = 99;
    b[237] = 130;
    b[238] = 83;
    b[239] = 99;
    u8 *o = b + 240;
    *o++ = 53;
    *o++ = 1;
    *o++ = (u8)type;                            /* 1 DISCOVER, 3 REQUEST */
    if (type == 3) {
        *o++ = 50;
        *o++ = 4;
        copy(o, eth.offered, 4);
        o += 4;
        *o++ = 54;
        *o++ = 4;
        copy(o, eth.server, 4);
        o += 4;
    }
    *o++ = 55;                                  /* asked for: mask, router, DNS server */
    *o++ = 3;
    *o++ = 1;
    *o++ = 3;
    *o++ = 6;
    *o++ = 12;                                  /* the host name */
    *o++ = 5;
    copy(o, (const u8 *)"m3801", 5);
    o += 5;
    *o = 255;
    u[0] = 0;
    u[1] = 68;
    u[2] = 0;
    u[3] = 67;
    put16(u + 4, 8 + 300);
    u[6] = u[7] = 0;                            /* (no UDP checksum: allowed over IPv4) */
    ipv4(h, zero, bcast, 17, 8 + 300);
    send_frame(tx, 14 + 20 + 8 + 300);
}

static void dhcp_start(void)
{
    eth.dhcp = 1;
    eth.dhcp_tries = 0;
    eth.xid = cp0_count() ^ (u32)(eth.mac[2] << 24 | eth.mac[3] << 16 | eth.mac[4] << 8 | eth.mac[5]);
    eth.dhcp_next = now_ms() + 2000;            /* (the link comes up about 1.6 s after the set-up) */
}

/* a DHCP answer for us: an offer is asked for, an acknowledgement taken (a line to show) */
static const char *dhcp_input(const u8 *b, int len)
{
    if (len < 240 || b[0] != 2 || be16(b + 4) != (u16)(eth.xid >> 16) || be16(b + 6) != (u16)eth.xid)
        return 0;
    int type = 0;
    u8 server[4] = {0}, mask[4] = {0}, router[4] = {0}, dns[4] = {0};
    u32 lease = 0;
    for (const u8 *o = b + 240; o < b + len && *o != 255; o += *o ? 2 + o[1] : 1) {
        if (!*o)
            continue;
        if (o[0] == 53)
            type = o[2];
        else if (o[0] == 54 && o[1] == 4)
            copy(server, o + 2, 4);
        else if (o[0] == 1 && o[1] == 4)
            copy(mask, o + 2, 4);
        else if (o[0] == 3 && o[1] >= 4)
            copy(router, o + 2, 4);
        else if (o[0] == 6 && o[1] >= 4)
            copy(dns, o + 2, 4);
        else if (o[0] == 51 && o[1] == 4)
            lease = (u32)(o[2] << 24 | o[3] << 16 | o[4] << 8 | o[5]);
    }
    if (type == 2 && eth.dhcp == 1) {           /* an offer: asked for */
        copy(eth.offered, b + 16, 4);
        copy(eth.server, server, 4);
        eth.dhcp = 2;
        eth.dhcp_tries = 0;
        dhcp_send(3);
        eth.dhcp_next = now_ms() + 2000;
        return 0;
    }
    if (type == 5 && eth.dhcp == 2) {           /* acknowledged: ours */
        copy(eth.ip, b + 16, 4);
        copy(eth.mask, mask, 4);
        copy(eth.router, router, 4);
        copy(eth.dns, dns, 4);
        eth.lease = lease;
        eth.dhcp = 3;
        eth.bound++;
        revived = 0;
        char a[16], r[16];
        ip_text(a, sizeof(a), eth.ip);
        ip_text(r, sizeof(r), eth.router);
        usnprintf(said, sizeof(said), "eth0: %s (DHCP), router %s, lease %u s", a, r, lease);
        return said;
    }
    if (type == 6 && eth.dhcp == 2) {           /* refused: from the start */
        dhcp_start();
        eth.dhcp_next = now_ms() + 1000;
    }
    return 0;
}

/* ---- DNS: a name's IPv4 address from the server DHCP named ---- */
static int dns_name_len(const u8 *p, const u8 *end)     /* a (maybe compressed) name's bytes; -1 bad */
{
    const u8 *q = p;
    while (q < end) {
        if (!*q)
            return (int)(q + 1 - p);
        if ((*q & 0xC0) == 0xC0)
            return (int)(q + 2 - p);
        q += 1 + *q;
    }
    return -1;
}

static void dns_input(const u8 *d, int len)
{
    const u8 *end = d + len;
    if (be16(d) != dns_id || !(d[2] & 0x80))
        return;                                 /* (not the answer to our question) */
    if ((d[3] & 0x0F) != 0) {
        dns_got = -1;                           /* no such name (or the server failed) */
        return;
    }
    int questions = be16(d + 4), answers = be16(d + 6);
    const u8 *p = d + 12;
    for (int i = 0; i < questions; i++) {
        int k = dns_name_len(p, end);
        if (k < 0)
            return;
        p += k + 4;
    }
    for (int i = 0; i < answers && p < end; i++) {
        int k = dns_name_len(p, end);
        if (k < 0 || p + k + 10 > end)
            return;
        p += k;
        u16 type = be16(p), rdlen = be16(p + 8);
        p += 10;
        if (type == 1 && rdlen == 4 && p + 4 <= end) {  /* an A record (CNAMEs before it skipped) */
            copy(dns_answer, p, 4);
            dns_got = 1;
            return;
        }
        p += rdlen;
    }
    dns_got = -1;
}

static const u8 *resolve(const u8 *ip);
static const char *handle(int len);

/* one frame read (up to wait_ms for it) and handled, for the waits of eth.c's own commands and eth_service: 1
   when one came.  A line handle() returns (a DHCP acknowledgement) waits for eth_poll's next pass */
static int take(u32 wait_ms)
{
    depth++;
    int n = receive(wait_ms);
    if (n >= 14) {
        eth.rx++;
        const char *s = handle(n);
        if (s)
            eth.pending = s;
    }
    depth--;
    return n >= 14;
}

/* the question for `name`'s A record sent to the DNS server DHCP named (dns_got 0 until its answer): 0, or why
   it could not be */
static const char *dns_ask(const char *name)
{
    static const u8 none[4];
    if (same(eth.dns, none, 4))
        return "no DNS server (DHCP named none)";
    const u8 *mac = resolve(eth.dns);
    if (!mac)
        return "the DNS server does not answer ARP";
    u8 to[6];
    copy(to, mac, 6);
    dns_id = (u16)(cp0_count() | 1);
    dns_got = 0;
    u8 *h = ether(tx, to, 0x0800), *u = h + 20, *d = u + 8;
    put16(d, dns_id);
    put16(d + 2, 0x0100);                       /* a standard query, recursion desired */
    put16(d + 4, 1);
    put16(d + 6, 0);
    put16(d + 8, 0);
    put16(d + 10, 0);
    u8 *q = d + 12;
    for (const char *s = name; *s;) {          /* the name as labels */
        u8 *count = q++;
        int k = 0;
        while (*s && *s != '.' && k < 63) {
            *q++ = (u8)*s++;
            k++;
        }
        *count = (u8)k;
        if (*s == '.')
            s++;
        if (q - d > 300)
            return "the name is too long";
    }
    *q++ = 0;
    put16(q, 1);                                /* A, IN */
    put16(q + 2, 1);
    q += 4;
    int dlen = (int)(q - d);
    put16(u, DNS_PORT);
    put16(u + 2, 53);
    put16(u + 4, (u16)(8 + dlen));
    u[6] = u[7] = 0;
    ipv4(h, eth.ip, eth.dns, 17, 8 + dlen);
    if (send_frame(tx, 14 + 20 + 8 + dlen) < 0)
        return usb_error_text();
    return 0;
}

/* a host name's address: 1 found (ip), 0 no such name or no answer (a line, `who`: first, said why) */
static int dns_lookup(const char *name, u8 *ip, const char *who)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        const char *why = dns_ask(name);
        if (why) {
            uprintf("%s: %s: %s\n", who, name, why);
            return 0;
        }
        u64 t = now_ms();
        while (!dns_got && now_ms() - t < 2000)
            take(5);
        if (dns_got == 1) {
            copy(ip, dns_answer, 4);
            return 1;
        }
        if (dns_got < 0) {
            uprintf("%s: %s: no such name\n", who, name);
            return 0;
        }
    }
    uprintf("%s: %s: no answer from the DNS server\n", who, name);
    return 0;
}

/* ---- the frames that come ---- */
static const char *handle(int len)
{
    const u8 *f = rx;
    u16 type = be16(f + 12);
    int to_us = same(f, eth.mac, 6);
    if (to_us)
        eth.rx_ours++;
    else if (f[0] & 1)
        eth.rx_multi++;
    if (type == 0x0806 && len >= 42) {
        const u8 *a = f + 14;
        if (eth.dhcp != 3 || !same(a + 24, eth.ip, 4)) {
            if (be16(a + 6) == 2 && to_us)
                arp_learn(a + 14, a + 8);       /* (an answer to us) */
            return 0;
        }
        arp_learn(a + 14, a + 8);
        if (be16(a + 6) == 1)                   /* who-has our address: we do */
            arp_send(2, a + 8, a + 8, a + 14);
        return 0;
    }
    if (type != 0x0800 || len < 34 || !(to_us || f[0] == 0xFF))
        return 0;
    const u8 *h = f + 14;
    int hl = (h[0] & 15) * 4, total = be16(h + 2);
    if ((h[0] >> 4) != 4 || hl < 20 || total > len - 14 || (be16(h + 6) & 0x3FFF))
        return 0;                               /* (no fragments) */
    const u8 *p = h + hl;
    int plen = total - hl;
    if (h[9] == 17 && plen >= 8 && be16(p + 2) == 68 && be16(p) == 67) {
        if (to_us)
            eth.dhcp_ours++;
        else
            eth.dhcp_multi++;
        return dhcp_input(p + 8, plen - 8);
    }
    if (eth.dhcp != 3 || !same(h + 16, eth.ip, 4))
        return 0;
    if (h[9] == 17 && plen >= 8 + 12 && be16(p) == 53 && be16(p + 2) == DNS_PORT) {
        dns_input(p + 8, plen - 8);
        return 0;
    }
    if (h[9] == 17 && plen >= 8) {              /* another UDP port: net.c's (NTP) */
        int ulen = be16(p + 4);
        if (ulen >= 8 && ulen <= plen)
            net_udp_input(h + 12, be16(p), be16(p + 2), p + 8, ulen - 8);
        return 0;
    }
    if (h[9] == 6 && to_us) {                   /* TCP (never to a broadcast) */
        tcp_input(h + 12, h + 16, f + 6, p, plen);
        return 0;
    }
    if (h[9] == 1 && plen >= 8) {
        if (p[0] == 8) {                        /* an echo request: answered, to where it came from */
            u8 *q = ipv4(ether(tx, f + 6, 0x0800), eth.ip, h + 12, 1, plen);
            copy(q, p, plen);
            q[0] = 0;
            q[2] = q[3] = 0;
            put16(q + 2, checksum(q, plen));
            send_frame(tx, 14 + 20 + plen);
            arp_learn(h + 12, f + 6);
        } else if (p[0] == 0 && be16(p + 4) == ping_id) {   /* our ping's reply */
            ping_seq_seen = be16(p + 6);
            ping_seen_at = now_ticks();
            ping_ttl = h[8];
            copy(ping_from, h + 12, 4);
        } else if ((p[0] == 3 || p[0] == 11) && plen >= 8 + 20 + 8) {     /* unreachable, TTL exceeded */
            const u8 *inner = p + 8, *e = inner + (inner[0] & 15) * 4;
            if (inner[9] == 1 && e + 8 <= p + plen && e[0] == 8 && be16(e + 4) == ping_id) {
                ping_error_type = p[0];
                ping_error_code = p[1];
                ping_error_seq = be16(e + 6);
                copy(ping_error_from, h + 12, 4);
            }
        }
    }
    return 0;
}

/* every main loop pass: the frames waiting (every POLL_MS; on every pass while a TCP connection is busy), DHCP's
   timers, TCP's, then net.c's servers (telnet, netload) and its NTP; a line to show, or 0 */
const char *eth_poll(void)
{
    if (!eth.used || depth)
        return 0;
    u64 now = now_ms();
    int moving = mouse_moving_on_b();
    if (now - eth.last_poll < (u32)(moving ? POLL_MOUSE_MS : tcp_busy() ? 0 : POLL_MS))
        return 0;                               /* (the adapter keeps the frames meanwhile) */
    /* a moving mouse's IN is almost always half way through the hub's split transaction: ending it lost that
       report and the next (on the box 99 of 211 with frames read every 100 ms).  So the frames are read only
       between its reports -- endpoint B is free from a report until the main loop asks for the next.  (TCP's
       segments wait too: a bulk OUT ends that IN as well) */
    if (moving && usb_in_pending())
        return 0;
    eth.last_poll = now;
    const char *line = eth.pending;
    eth.pending = 0;
    depth++;
    for (int k = 0; k < 16; k++) {              /* (a busy network: up to 16 frames a look) */
        int n = receive(1);
        if (n < 14)
            break;
        eth.rx++;
        const char *s = handle(n);
        if (s)
            line = s;
    }
    depth--;
    if ((eth.dhcp == 1 || eth.dhcp == 2) && now_ms() >= eth.dhcp_next) {
        if (++eth.dhcp_tries > 8) {
            eth.dhcp = 4;
            line = "eth0: no answer from a DHCP server ('dhcp' asks again)";
        } else {
            /* No offer: the adapter may not send.  On the box an RTL8153 plugged in while the demo ran took
               its frames (10 DISCOVERs in 3 minutes: each bulk OUT done) and received the network's
               broadcasts, but no offer came -- the router answers by broadcast, so it never got one; after
               a restart of the box the same adapter had its address in 2 s, and plugged in again later it
               had too.  So after the second DISCOVER its packet filter is set again, and after the fourth
               (14 s) the adapter is set up again as if it had been plugged in again: its hub port reset,
               its configuration, eth_attach -- once, until DHCP has an answer */
            if (eth.dhcp == 1 && eth.dhcp_tries == 2) {
                set_filter();
                eth.again = 1;
            } else if (eth.dhcp == 1 && eth.dhcp_tries == 4 && !revived) {
                revived = 1;
                reviving = 1;
                int r = usb_set_up_again(eth.device);
                reviving = 0;
                return r < 0 ? "eth0: no DHCP answer, and the adapter did not come back from its port's reset"
                             : "eth0: no DHCP answer: the adapter set up again (its port reset)";
            }
            dhcp_send(eth.dhcp == 1 ? 1 : 3);
            eth.dhcp_next = now_ms() + 2000u * (u32)(eth.dhcp_tries < 4 ? eth.dhcp_tries : 4);
        }
    }
    tcp_poll();
    if (line)
        return line;                            /* (net_poll's turn comes in the next pass) */
    return net_poll();
}

/* ---- for tcp.c and net.c ---- */
int eth_ready(void)                             /* an address from DHCP: IPv4 can be sent */
{
    return eth.used && eth.dhcp == 3;
}

const u8 *eth_address(void)
{
    return eth.ip;
}

u32 eth_bound_count(void)
{
    return eth.bound;
}

int eth_inside(void)                            /* frames are being read / handled now: not a time to read more */
{
    return depth;
}

/* the frames waiting now (up to 16) and TCP's timers -- for a command's own waits (wget, ntp) and the telnet
   console; nothing inside eth code (rx[] in use) */
void eth_service(void)
{
    if (!eth.used || depth)
        return;
    for (int k = 0; k < 16 && take(1); k++)
        ;
    tcp_poll();
}

/* the MAC address packets to `ip` go to: the host's on our network, else the router's (ARP, up to 1 s): 1, 0 no
   answer */
int eth_route(const u8 *ip, u8 mac[6])
{
    if (!eth_ready() || depth)
        return 0;
    const u8 *m = resolve(ip);
    if (!m)
        return 0;
    copy(mac, m, 6);
    return 1;
}

/* an IPv4 packet in the caller's own frame (`len` bytes of payload at frame + 34, room before it for the headers
   and FRAME_MAX + 2 bytes in all) to `ip` through `mac`: 0 sent, -1 not */
int eth_send_ipv4(u8 *frame, const u8 *mac, const u8 *ip, u8 proto, int len)
{
    if (!eth_ready() || len > FRAME_MAX - 34)
        return -1;
    ipv4(ether(frame, mac, 0x0800), eth.ip, ip, proto, len);
    return send_frame(frame, 14 + 20 + len);
}

/* "a.b.c.d" or a name (DNS): 1 and its address, 0 (a line starting with `who` said why) */
int eth_lookup(const char *host, u8 ip[4], const char *who)
{
    if (eth_parse_ip(host, ip))
        return 1;
    if (!eth_ready()) {
        uprintf("%s: eth0 has no address yet ('ifconfig')\n", who);
        return 0;
    }
    return dns_lookup(host, ip, who);
}

/* a DNS question sent without waiting for its answer (the clock's setting after DHCP): 0, -1 not sent */
int eth_dns_start(const char *name)
{
    if (!eth_ready() || depth)
        return -1;
    return dns_ask(name) ? -1 : 0;
}

int eth_dns_result(u8 ip[4])                    /* its answer: 1 (ip), 0 none yet, -1 no such name */
{
    if (dns_got == 1)
        copy(ip, dns_answer, 4);
    return dns_got;
}

/* ---- the commands ---- */
const char *eth_attach(u8 device, u8 ctrl_if, u8 data_if, u8 data_alt, u8 in_ep, u8 out_ep, u16 maxp, u8 int_ep,
                       u8 mac_string)
{
    (void)int_ep;
    if (eth.used && eth.device != device)
        return "an Ethernet port, but one is in use already: left alone";
    if (usb_set_interface(device, data_if, data_alt) < 0)
        return "an Ethernet port that refused its data interface's setting";
    u8 s[64];
    int n = usb_request(device, 0x80, 0x06, (u16)(0x0300 | mac_string), 0x0409, s, sizeof(s), 500);
    for (int i = 0; i < 6; i++) {
        int hi = n >= 2 + 24 ? hexval(s[2 + 4 * i]) : -1, lo = n >= 2 + 24 ? hexval(s[4 + 4 * i]) : -1;
        if (hi < 0 || lo < 0)
            return "an Ethernet port without a MAC address";
        eth.mac[i] = (u8)(hi << 4 | lo);
    }
    eth.used = 1;
    eth.device = device;
    eth.ctrl_if = ctrl_if;
    set_filter();
    if (!reviving)
        revived = 0;
    eth.in_ep = in_ep;
    eth.out_ep = out_ep;
    eth.maxp = maxp;
    eth.rx = eth.tx = eth.rx_ignored = eth.rx_ours = eth.rx_multi = eth.dhcp_ours = eth.dhcp_multi = 0;
    eth.again = 0;
    for (int i = 0; i < 4; i++)
        eth.ip[i] = 0;
    for (int i = 0; i < ARP_ENTRIES; i++)
        arp_table[i].used = 0;
    tcp_drop_all();                             /* (an address of its own again: the old connections are gone) */
    dhcp_start();
    usnprintf(said, sizeof(said), "eth0: Ethernet (CDC ECM), MAC %02x:%02x:%02x:%02x:%02x:%02x; DHCP asks for an "
              "address", eth.mac[0], eth.mac[1], eth.mac[2], eth.mac[3], eth.mac[4], eth.mac[5]);
    return said;
}

void eth_detach(u8 device)
{
    if (eth.used && (!device || eth.device == device)) {
        eth.used = 0;
        tcp_drop_all();
    }
}

void eth_show(void)
{
    if (!eth.used) {
        uprintf("no Ethernet port (a USB Ethernet adapter with CDC ECM: an RTL8153's, ...)\n");
        return;
    }
    char a[16], m[16], r[16], d[16];
    ip_text(a, sizeof(a), eth.ip);
    ip_text(m, sizeof(m), eth.mask);
    ip_text(r, sizeof(r), eth.router);
    ip_text(d, sizeof(d), eth.dns);
    uprintf("eth0: device %u, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", eth.device, eth.mac[0], eth.mac[1], eth.mac[2],
            eth.mac[3], eth.mac[4], eth.mac[5]);
    if (eth.dhcp == 3)
        uprintf("  inet %s, mask %s, router %s, DNS %s (DHCP, lease %u s)\n", a, m, r, d, eth.lease);
    else
        uprintf("  no address yet (DHCP: %s)\n", eth.dhcp == 4 ? "no answer" : eth.dhcp == 2 ? "requesting" : "asking");
    uprintf("  %u frames in (%u to this MAC, %u broadcast / multicast), %u out\n", eth.rx, eth.rx_ours,
            eth.rx_multi, eth.tx);
    if (eth.dhcp != 3 || eth.again || revived)
        uprintf("  DHCP answers seen: %u to this MAC, %u broadcast%s%s\n", eth.dhcp_ours, eth.dhcp_multi,
                eth.again ? "; the packet filter was set again" : "",
                revived ? "; the adapter was set up again" : "");
}

void eth_dhcp(void)
{
    if (!eth.used) {
        eth_show();
        return;
    }
    dhcp_start();
    eth.dhcp_next = now_ms();
    uprintf("DHCP: asking for an address\n");
}

void eth_arp_list(void)
{
    int n = 0;
    for (int i = 0; i < ARP_ENTRIES; i++)
        if (arp_table[i].used) {
            char a[16];
            ip_text(a, sizeof(a), arp_table[i].ip);
            const u8 *m = arp_table[i].mac;
            uprintf("%s  %02x:%02x:%02x:%02x:%02x:%02x\n", a, m[0], m[1], m[2], m[3], m[4], m[5]);
            n++;
        }
    if (!n)
        uprintf("the ARP table is empty\n");
}

static int on_link(const u8 *ip)                /* (our network's: else through the router) */
{
    for (int i = 0; i < 4; i++)
        if ((ip[i] & eth.mask[i]) != (eth.ip[i] & eth.mask[i]))
            return 0;
    return 1;
}

/* the MAC address to send to `ip` at (ARP, up to 1 s): 0 none */
static const u8 *resolve(const u8 *ip)
{
    const u8 *via = on_link(ip) ? ip : eth.router;
    for (int tries = 0; tries < 3; tries++) {
        const u8 *mac = arp_find(via);
        if (mac)
            return mac;
        static const u8 none[6];
        arp_send(1, bcast, none, via);
        u64 t = now_ms();
        while (now_ms() - t < 333) {
            take(5);
            if (arp_find(via))
                break;
        }
    }
    return arp_find(via);
}

/* a round trip in microseconds as Linux's ping shows it: three digits (0.612, 12.3, 103) */
static void ms_text(char *buf, int size, u32 us)
{
    if (us >= 100000)
        usnprintf(buf, size, "%u", us / 1000);
    else if (us >= 10000)
        usnprintf(buf, size, "%u.%u", us / 1000, us % 1000 / 100);
    else if (us >= 1000)
        usnprintf(buf, size, "%u.%02u", us / 1000, us % 1000 / 10);
    else
        usnprintf(buf, size, "0.%03u", us);
}

static const char *icmp_error_text(u8 type, u8 code)
{
    if (type == 11)
        return "Time to live exceeded";
    static const char *const unreachable[] = {"Destination Net Unreachable", "Destination Host Unreachable",
                                              "Destination Protocol Unreachable", "Destination Port Unreachable",
                                              "Frag needed and DF set", "Source Route Failed"};
    return code < sizeof(unreachable) / sizeof(unreachable[0]) ? unreachable[code] : "Destination Unreachable";
}

/* 'ping [-c count] <host>' like Linux's: echo requests a second apart until Ctrl-C (or count of them), each
   reply's TTL and time, then the statistics; a host name is asked of the DNS server DHCP named */
void eth_ping(const char *host, int count)
{
    if (!eth.used || eth.dhcp != 3) {
        uprintf(eth.used ? "ping: eth0 has no address yet ('ifconfig')\n" : "ping: no Ethernet port\n");
        return;
    }
    u8 ip[4];
    if (!eth_parse_ip(host, ip) && !dns_lookup(host, ip, "ping"))
        return;
    char a[16];
    ip_text(a, sizeof(a), ip);
    const u8 *mac = resolve(ip);
    if (!mac) {
        uprintf("ping: %s: no ARP answer from %s\n", host, on_link(ip) ? a : "the router");
        return;
    }
    u8 to[6];
    copy(to, mac, 6);
    ping_id = (u16)(cp0_count() | 1);
    uprintf("PING %s (%s) 56(84) bytes of data.\n", host, a);
    int sent = 0, got = 0, errors = 0, stop = 0;
    u32 best = 0xFFFFFFFFu, worst = 0, sum = 0;
    u64 start = now_ms();
    for (int seq = 1; !stop && (!count || seq <= count); seq++) {
        u8 *q = ipv4(ether(tx, to, 0x0800), eth.ip, ip, 1, 8 + 56);
        q[0] = 8;
        q[1] = 0;
        q[2] = q[3] = 0;
        put16(q + 4, ping_id);
        put16(q + 6, (u16)seq);
        for (int i = 0; i < 56; i++)
            q[8 + i] = (u8)(0x10 + i);
        put16(q + 2, checksum(q, 64));
        ping_seq_seen = ping_error_seq = 0;
        u64 t0 = now_ticks(), ms0 = now_ms();
        if (send_frame(tx, 14 + 20 + 64) < 0) {
            uprintf("ping: sendmsg: %s\n", usb_error_text());
            errors++;
        } else {
            sent++;
        }
        int answered = 0, last = count && seq == count;
        while (now_ms() - ms0 < 1000) {
            take(5);
            tcp_poll();                         /* (the telnet console's output and its connection's timers) */
            if (!answered && ping_seq_seen == seq && same(ping_from, ip, 4)) {
                u32 us = (u32)(ping_seen_at - t0) / (ticks_per_ms() / 1000);    /* (32-bit: under a second) */
                char t[16];
                ms_text(t, sizeof(t), us);
                uprintf("64 bytes from %s: icmp_seq=%d ttl=%u time=%s ms\n", a, seq, ping_ttl, t);
                answered = 1;
                got++;
                sum += us;
                best = us < best ? us : best;
                worst = us > worst ? us : worst;
                if (last)
                    break;
            }
            if (!answered && ping_error_seq == seq) {
                char from[16];
                ip_text(from, sizeof(from), ping_error_from);
                uprintf("From %s icmp_seq=%d %s\n", from, seq, icmp_error_text(ping_error_type, ping_error_code));
                answered = 1;
                errors++;
                if (last)
                    break;
            }
            if (uart_getc() == 3) {
                uprintf("^C\n");
                stop = 1;
                break;
            }
        }
    }
    u32 took = (u32)(now_ms() - start);
    uprintf("\n--- %s ping statistics ---\n", host);
    uprintf("%d packets transmitted, %d received", sent, got);
    if (errors)
        uprintf(", +%d errors", errors);
    uprintf(", %d%% packet loss, time %ums\n", sent ? (sent - got) * 100 / sent : 0, took);
    if (got) {
        u32 avg = sum / (u32)got;
        uprintf("rtt min/avg/max = %u.%03u/%u.%03u/%u.%03u ms\n", best / 1000, best % 1000, avg / 1000, avg % 1000,
                worst / 1000, worst % 1000);
    }
}
