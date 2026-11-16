/* TCP over eth.c's IPv4: a few connections, opened from here (tcp_connect:
   wget) or by a peer through a port listened on (tcp_listen + tcp_accept:
   the telnet console, netload, FTP, the web page).  Kept small and plain:
     - MSS 1460 offered (the peer's taken from its SYN, 536 without one), no
       window scaling, no SACK, no timestamps;
     - in-order receive only: a segment that does not start at rcv_nxt is
       dropped and answered with a duplicate ACK (the peer sends it again);
       every segment with data is ACKed at once (no delayed ACK);
     - a send buffer per connection: the bytes from snd_una on stay until
       ACKed; sent as far as the peer's window allows, in MSS pieces; after a
       time-out everything from snd_una is sent again (go-back-N), the timer
       doubling from 400 ms to 3.2 s, the connection given up after 8 of
       them (unless the peer still answers: a closed window is probed, one
       byte at a time, as long as it does);
     - FIN in both directions (TIME_WAIT kept 2 s: a LAN), RST answered and
       sent -- also for segments to a port nobody listens on;
     - checksums with the pseudo-header, both ways.
   Segments are built in this file's own frame (an ACK can go out from
   inside eth.c's handle, while eth.c's tx[] is half built).  Called only
   from the main loop and from commands (no interrupts); tcp_input from
   eth.c's handle, everything else outside it.
   Measured on the box (RTL8153 behind two hubs): 2.5 MB/s from a PC on the
   LAN, 790 KB/s from a server 34 ms away; from one 118 ms away over a path
   that lost packets only 31 KB/s -- every segment after a lost one is
   dropped here and sent again a round trip later (an out-of-order queue
   or SACK would be the cure). */
#include "hw.h"

#define CONNS        12              /* (telnet, FTP's two control and data, wget or netload, the web page's six) */
#define LISTENS      8
#define MSS          1460
#define SEND_SIZE    32768u             /* each connection's send buffer (the telnet console's output) */
#define RECV_SIZE    32768u             /* its receive buffer: the most the window offers */
#define RTO_FIRST    400
#define RTO_MAX      3200
#define TRIES        8
#define TIME_WAIT_MS 2000
#define LINGER_MS    10000              /* FIN_WAIT_2 without the peer's FIN: given up */
#define BUSY_MS      200                /* data moved this recently: eth_poll keeps reading on every pass */

enum { CLOSED, SYN_SENT, SYN_RCVD, ESTABLISHED, FIN_WAIT_1, FIN_WAIT_2, CLOSE_WAIT, CLOSING, LAST_ACK, TIME_WAIT };
static const char *const state_names[] = {"CLOSED", "SYN_SENT", "SYN_RCVD", "ESTABLISHED", "FIN_WAIT_1",
                                          "FIN_WAIT_2", "CLOSE_WAIT", "CLOSING", "LAST_ACK", "TIME_WAIT"};

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

struct conn {
    u8 state;
    u8 held;                            /* the caller's (connected, or accepted): kept until tcp_close / tcp_abort */
    u8 fin_queued, fin_sent, fin_got, ack_due;
    const char *why;                    /* why it failed (0: it did not) */
    u8 ip[4], mac[6];
    u16 lport, rport, mss;
    u32 iss, snd_una, snd_nxt, snd_max, snd_wnd, fin_seq;
    u32 s_seq, s_len, s_head;           /* the send buffer: s_len bytes from sequence number s_seq, at s_head */
    u32 rcv_nxt, r_head, r_len, r_wnd;  /* the receive buffer: r_len bytes at r_head; the window last offered */
    u32 rto, tries;
    u64 timer;                          /* retransmission / TIME_WAIT / FIN_WAIT_2's end (0: off) */
    u64 heard, moved;                   /* the peer's last segment; data last moved */
    u32 sent, received;                 /* bytes, for netstat */
};
static struct conn conns[CONNS];
static u8 sbuf[CONNS][SEND_SIZE], rbuf[CONNS][RECV_SIZE];
static u16 listening[LISTENS];
static u16 next_port;
static u8 frame[14 + 20 + 24 + MSS + 2];
static u32 bad_sums, resets_sent, segs_in, segs_out, timeouts, dropped_ooo;

static int before(u32 a, u32 b)         /* sequence numbers: a comes before b */
{
    return (s32)(a - b) < 0;
}

static u16 be16(const u8 *p)
{
    return (u16)(p[0] << 8 | p[1]);
}

static u32 be32(const u8 *p)
{
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
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

static void copy(u8 *to, const u8 *from, int n)
{
    for (int i = 0; i < n; i++)
        to[i] = from[i];
}

static u32 sum_add(u32 sum, const u8 *p, int n)
{
    for (int i = 0; i + 1 < n; i += 2)
        sum += (u32)(p[i] << 8 | p[i + 1]);
    if (n & 1)
        sum += (u32)(p[n - 1] << 8);
    return sum;
}

/* the checksum over the pseudo-header (addresses, protocol 6, length) and the segment; 0 for a received
   segment that is whole */
static u16 tcp_sum(const u8 *src, const u8 *dst, const u8 *seg, int len)
{
    u32 sum = sum_add(sum_add(0, src, 4), dst, 4) + 6 + (u32)len;
    sum = sum_add(sum, seg, len);
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

static int index_of(const struct conn *c)
{
    return (int)(c - conns);
}

static struct conn *get(int c)
{
    return c >= 0 && c < CONNS ? &conns[c] : 0;
}

/* ---- sending ---- */
/* one segment from `frame`, its data (n bytes) already at frame + 54; SYN segments carry the MSS option and no
   data */
static void emit(const u8 *ip, const u8 *mac, u16 lport, u16 rport, u32 seq, u32 ack, u8 flags, u32 wnd, int n)
{
    u8 *t = frame + 34;
    int hl = 20;
    put16(t, lport);
    put16(t + 2, rport);
    put32(t + 4, seq);
    put32(t + 8, ack);
    if (flags & F_SYN) {
        t[20] = 2;                              /* the MSS option */
        t[21] = 4;
        put16(t + 22, MSS);
        hl = 24;
        n = 0;
    }
    t[12] = (u8)(hl / 4 << 4);
    t[13] = flags;
    put16(t + 14, wnd > 0xFFFF ? 0xFFFF : wnd);
    put16(t + 16, 0);
    put16(t + 18, 0);
    put16(t + 16, tcp_sum(eth_address(), ip, t, hl + n));
    eth_send_ipv4(frame, mac, ip, 6, hl + n);
    segs_out++;
}

static u32 window(const struct conn *c)
{
    return RECV_SIZE - c->r_len;
}

/* a segment of the connection: n bytes of its send buffer from `off` (bytes after s_seq) */
static void send_seg(struct conn *c, u32 seq, u8 flags, u32 off, u32 n)
{
    const u8 *b = sbuf[index_of(c)];
    u32 at = (c->s_head + off) % SEND_SIZE;
    for (u32 i = 0; i < n; i++) {
        frame[54 + i] = b[at];
        at = (at + 1) % SEND_SIZE;
    }
    c->r_wnd = window(c);
    emit(c->ip, c->mac, c->lport, c->rport, seq, c->rcv_nxt, flags, c->r_wnd, (int)n);
    if (flags & F_ACK)
        c->ack_due = 0;
    if (before(c->snd_max, seq + n))
        c->snd_max = seq + n;
}

static void arm(struct conn *c)
{
    if (!c->timer)
        c->timer = now_ms() + c->rto;
}

/* what may go now: the SYN (again), data as far as the peer's window and the buffer allow, then the FIN */
static void output(struct conn *c)
{
    if (c->state == SYN_SENT || c->state == SYN_RCVD) {
        if (c->snd_nxt == c->iss) {
            send_seg(c, c->iss, c->state == SYN_RCVD ? F_SYN | F_ACK : F_SYN, 0, 0);
            c->snd_nxt = c->iss + 1;
            if (before(c->snd_max, c->snd_nxt))
                c->snd_max = c->snd_nxt;
            arm(c);
        }
        return;
    }
    if (c->state != ESTABLISHED && c->state != CLOSE_WAIT && c->state != FIN_WAIT_1 && c->state != CLOSING
        && c->state != LAST_ACK)
        return;
    for (;;) {
        u32 off = c->snd_nxt - c->s_seq;
        if (off >= c->s_len)
            break;                              /* (everything sent, or the FIN is) */
        u32 flight = c->snd_nxt - c->snd_una, usable = c->snd_wnd > flight ? c->snd_wnd - flight : 0;
        u32 n = c->s_len - off;
        if (n > usable)
            n = usable;
        if (n > c->mss)
            n = c->mss;
        if (!n) {
            if (!flight)
                arm(c);                         /* a closed window: probed when the timer runs out */
            break;
        }
        send_seg(c, c->snd_nxt, F_ACK | F_PSH, off, n);
        c->snd_nxt += n;
        c->sent += n;
        arm(c);
    }
    if (c->fin_sent && c->snd_nxt == c->fin_seq && c->snd_nxt == c->s_seq + c->s_len) {
        send_seg(c, c->snd_nxt, F_FIN | F_ACK, 0, 0);   /* (after a time-out: the FIN again) */
        c->snd_nxt++;
        arm(c);
    } else if (c->fin_queued && !c->fin_sent && c->snd_nxt == c->s_seq + c->s_len) {
        send_seg(c, c->snd_nxt, F_FIN | F_ACK, 0, 0);
        c->fin_seq = c->snd_nxt++;
        c->fin_sent = 1;
        if (before(c->snd_max, c->snd_nxt))
            c->snd_max = c->snd_nxt;
        if (c->state == ESTABLISHED)
            c->state = FIN_WAIT_1;
        else if (c->state == CLOSE_WAIT)
            c->state = LAST_ACK;
        arm(c);
    }
}

static void send_ack(struct conn *c)
{
    send_seg(c, c->snd_nxt, F_ACK, 0, 0);
}

static void set_closed(struct conn *c, const char *why)
{
    c->state = CLOSED;
    c->timer = 0;
    if (why && !c->why)
        c->why = why;
}

static void reset(struct conn *c, const char *why)     /* RST to the peer, closed */
{
    if (c->state != CLOSED && c->state != SYN_SENT && c->state != TIME_WAIT) {
        emit(c->ip, c->mac, c->lport, c->rport, c->snd_nxt, c->rcv_nxt, F_RST | F_ACK, 0, 0);
        resets_sent++;
    }
    set_closed(c, why);
}

/* the retransmission timer ran out: everything from snd_una again (or a probe of a closed window), the timer
   doubled; given up after TRIES unless the peer still answers.  TIME_WAIT and FIN_WAIT_2 end on it too */
static void timeout(struct conn *c, u64 now)
{
    c->timer = 0;
    if (c->state == TIME_WAIT || c->state == FIN_WAIT_2) {
        set_closed(c, 0);
        return;
    }
    u32 flight = c->snd_nxt - c->snd_una, off = c->snd_nxt - c->s_seq;
    if (!flight && c->snd_wnd == 0 && off < c->s_len) {
        send_seg(c, c->snd_nxt, F_ACK, off, 1);                         /* the window probe: one byte */
        c->snd_nxt++;
        c->rto = c->rto * 2 > RTO_MAX ? RTO_MAX : c->rto * 2;
        arm(c);
        return;
    }
    if (!flight)
        return;
    int alive = c->snd_wnd == 0 && now - c->heard < 2u * c->rto;       /* (a closed window, answered) */
    if (!alive && ++c->tries > TRIES) {
        reset(c, "timed out");
        return;
    }
    timeouts++;
    c->rto = c->rto * 2 > RTO_MAX ? RTO_MAX : c->rto * 2;
    c->snd_nxt = c->snd_una;                    /* go back: the SYN, the data, the FIN, whatever is not ACKed (a FIN
                                                   stays fin_sent: output sends it again when the data is out --
                                                   cleared, a late ACK of data + FIN would not see it ACKed and
                                                   LAST_ACK / CLOSING would hold the connection for ever) */
    if (c->state == SYN_SENT || c->state == SYN_RCVD)
        c->snd_nxt = c->iss;
    output(c);
    arm(c);
}

/* ---- receiving ---- */
static struct conn *find(const u8 *ip, u16 lport, u16 rport)
{
    for (int i = 0; i < CONNS; i++) {
        struct conn *c = &conns[i];
        if (c->state != CLOSED && c->lport == lport && c->rport == rport && same(c->ip, ip, 4))
            return c;
    }
    return 0;
}

/* a slot for a new connection: a closed one, else the oldest in TIME_WAIT (cut short: on a LAN a late
   segment of an old connection is rare, and an FTP client fetching a folder of small files leaves a data
   connection in TIME_WAIT for 2 s after each file -- with every slot so, the next was refused) */
static struct conn *free_conn(void)
{
    struct conn *oldest = 0;
    for (int i = 0; i < CONNS; i++) {
        struct conn *c = &conns[i];
        if (c->state == CLOSED && !c->held)
            return c;
        if (c->state == TIME_WAIT && !c->held && (!oldest || c->timer < oldest->timer))
            oldest = c;
    }
    if (oldest)
        set_closed(oldest, 0);
    return oldest;
}

static void start(struct conn *c, const u8 *ip, const u8 *mac, u16 lport, u16 rport)
{
    u8 held = c->held;
    u8 *p = (u8 *)c;
    for (u32 i = 0; i < sizeof(*c); i++)
        p[i] = 0;
    c->held = held;
    copy(c->ip, ip, 4);
    copy(c->mac, mac, 6);
    c->lport = lport;
    c->rport = rport;
    c->mss = 536;
    c->iss = cp0_count() * 2654435761u ^ (u32)rport << 16 ^ lport;
    c->snd_una = c->snd_nxt = c->snd_max = c->iss;
    c->s_seq = c->iss + 1;
    c->rto = RTO_FIRST;
    c->heard = c->moved = now_ms();
}

static u16 peer_mss(const u8 *seg, int hl)     /* the MSS option of a SYN (536 without one) */
{
    for (int i = 20; i + 1 < hl;) {
        u8 kind = seg[i];
        if (kind == 0)
            break;
        if (kind == 1) {
            i++;
            continue;
        }
        if (kind == 2 && seg[i + 1] == 4 && i + 4 <= hl) {
            u16 m = be16(seg + i + 2);
            return m < 64 ? 64 : m > MSS ? MSS : m;
        }
        if (seg[i + 1] < 2)
            break;
        i += seg[i + 1];
    }
    return 536;
}

/* a segment for no connection (or one that is not ours to take): RST, as RFC 793 answers it (never to a RST) */
static void refuse(const u8 *ip, const u8 *mac, const u8 *seg, int dlen)
{
    u8 flags = seg[13];
    if (flags & F_RST)
        return;
    u32 seq = be32(seg + 4), ack = be32(seg + 8);
    if (flags & F_ACK)
        emit(ip, mac, be16(seg + 2), be16(seg), ack, 0, F_RST, 0, 0);
    else
        emit(ip, mac, be16(seg + 2), be16(seg), 0, seq + (u32)dlen + !!(flags & F_SYN) + !!(flags & F_FIN),
             F_RST | F_ACK, 0, 0);
    resets_sent++;
}

static int listened(u16 port)
{
    for (int i = 0; i < LISTENS; i++)
        if (listening[i] == port)
            return 1;
    return 0;
}

/* the bytes the peer ACKed leave the send buffer; the SYN and the FIN ACKed move the state on */
static void acked(struct conn *c, u32 ack, u64 now)
{
    if (before(c->snd_una, ack) && !before(c->snd_max, ack)) {
        if (!before(ack, c->s_seq)) {
            u32 n = ack - c->s_seq;
            if (n > c->s_len)
                n = c->s_len;                   /* (the rest: the FIN) */
            c->s_head = (c->s_head + n) % SEND_SIZE;
            c->s_len -= n;
            c->s_seq += n;
            if (n)
                c->moved = now;
        }
        c->snd_una = ack;
        if (before(c->snd_nxt, ack))
            c->snd_nxt = ack;
        c->rto = RTO_FIRST;
        c->tries = 0;
        c->timer = 0;
        if (c->snd_nxt != c->snd_una)
            arm(c);
    }
    if (c->fin_sent && !before(c->snd_una, c->fin_seq + 1)) {      /* our FIN is ACKed */
        if (c->state == FIN_WAIT_1) {
            c->state = FIN_WAIT_2;
            c->timer = now + LINGER_MS;
        } else if (c->state == CLOSING) {
            c->state = TIME_WAIT;
            c->timer = now + TIME_WAIT_MS;
        } else if (c->state == LAST_ACK) {
            set_closed(c, 0);
        }
    }
}

/* data in order into the receive buffer (as much as it holds), the FIN after it; 1 when an ACK is due */
static int take_data(struct conn *c, u32 seq, const u8 *data, int dlen, int fin, u64 now)
{
    if (c->state != ESTABLISHED && c->state != FIN_WAIT_1 && c->state != FIN_WAIT_2)
        return dlen > 0 || fin;                 /* (after its FIN: nothing more taken, ACKed again) */
    if (seq != c->rcv_nxt) {
        u32 old = c->rcv_nxt - seq;             /* (unsigned: a segment 2^31 back must not make it negative) */
        if (!before(seq, c->rcv_nxt) || old >= (u32)dlen + (u32)fin) {
            dropped_ooo++;
            return 1;                           /* out of order, or all old: dropped, a duplicate ACK */
        }
        if (old > (u32)dlen)
            old = (u32)dlen;
        data += old;                            /* (partly old: the new part taken) */
        dlen -= (int)old;
    }
    u32 room = RECV_SIZE - c->r_len, n = (u32)dlen < room ? (u32)dlen : room;
    if (!c->held && c->fin_queued) {
        n = (u32)dlen;                          /* given up by its caller: ACKed, nobody reads it */
    } else {
        u8 *b = rbuf[index_of(c)];
        u32 at = (c->r_head + c->r_len) % RECV_SIZE;
        for (u32 i = 0; i < n; i++) {
            b[at] = data[i];
            at = (at + 1) % RECV_SIZE;
        }
        c->r_len += n;
    }
    c->rcv_nxt += n;
    c->received += n;
    if (n)
        c->moved = now;
    if (fin && n == (u32)dlen && !c->fin_got) {
        c->fin_got = 1;
        c->rcv_nxt++;
        if (c->state == ESTABLISHED) {
            c->state = CLOSE_WAIT;
        } else if (c->state == FIN_WAIT_2 || (c->state == FIN_WAIT_1 && !before(c->snd_una, c->fin_seq + 1))) {
            c->state = TIME_WAIT;
            c->timer = now + TIME_WAIT_MS;
        } else {
            c->state = CLOSING;
        }
    }
    return dlen > 0 || fin;
}

/* a segment for an open or opening connection */
static void segment(struct conn *c, const u8 *seg, int hl, int dlen, u64 now)
{
    u8 flags = seg[13];
    u32 seq = be32(seg + 4), ack = be32(seg + 8), wnd = be16(seg + 14);
    c->heard = now;
    if (c->state == SYN_SENT) {
        if ((flags & F_ACK) && ack != c->iss + 1) {
            if (!(flags & F_RST))
                emit(c->ip, c->mac, c->lport, c->rport, ack, 0, F_RST, 0, 0);
            return;
        }
        if (flags & F_RST) {
            if (flags & F_ACK)
                set_closed(c, "connection refused");
            return;
        }
        if (!(flags & F_SYN) || !(flags & F_ACK))
            return;                             /* (a simultaneous open: not done here) */
        c->rcv_nxt = seq + 1;
        c->mss = peer_mss(seg, hl);
        c->snd_una = ack;
        c->snd_wnd = wnd;
        c->state = ESTABLISHED;
        c->timer = 0;
        c->tries = 0;
        c->rto = RTO_FIRST;
        send_ack(c);
        output(c);
        return;
    }
    if (flags & F_RST) {
        if (seq - c->rcv_nxt < RECV_SIZE || c->state == SYN_RCVD)  /* (in the window: not a stray one) */
            set_closed(c, "connection reset by the peer");
        return;
    }
    if (flags & F_SYN) {
        if (c->state == SYN_RCVD && seq + 1 == c->rcv_nxt) {
            c->snd_nxt = c->iss;                /* its SYN again: our SYN-ACK was lost */
            output(c);
        } else {
            send_ack(c);                        /* (RFC 5961's challenge ACK) */
        }
        return;
    }
    if (!(flags & F_ACK))
        return;
    if (c->state == SYN_RCVD) {
        if (ack != c->iss + 1) {
            emit(c->ip, c->mac, c->lport, c->rport, ack, 0, F_RST, 0, 0);
            return;
        }
        c->state = ESTABLISHED;
    }
    if (before(c->snd_max, ack)) {
        send_ack(c);                            /* it ACKs what was never sent */
        return;
    }
    acked(c, ack, now);
    if (!before(ack, c->snd_una))
        c->snd_wnd = wnd;
    if (c->state == CLOSED)
        return;
    if (take_data(c, seq, seg + hl, dlen, flags & F_FIN, now))
        c->ack_due = 1;
    output(c);
    if (c->ack_due && c->state != CLOSED)
        send_ack(c);
}

/* eth.c: a TCP segment for our address (src, dst: the IPv4 header's addresses; src_mac: where it came from, the
   way back) */
void tcp_input(const u8 *src, const u8 *dst, const u8 *src_mac, const u8 *seg, int len)
{
    if (len < 20)
        return;
    int hl = (seg[12] >> 4) * 4;
    if (hl < 20 || hl > len)
        return;
    if (tcp_sum(src, dst, seg, len) != 0) {
        bad_sums++;
        return;
    }
    segs_in++;
    u64 now = now_ms();
    u16 sport = be16(seg), dport = be16(seg + 2);
    u8 flags = seg[13];
    int dlen = len - hl;
    struct conn *c = find(src, dport, sport);
    if (c && c->state == TIME_WAIT && (flags & F_SYN) && !(flags & F_ACK)) {
        set_closed(c, 0);                       /* (the same pair again: a new connection) */
        c = 0;
    }
    if (c) {
        segment(c, seg, hl, dlen, now);
        return;
    }
    if ((flags & (F_SYN | F_ACK | F_RST)) == F_SYN && listened(dport) && (c = free_conn())) {
        start(c, src, src_mac, dport, sport);
        c->state = SYN_RCVD;
        c->rcv_nxt = be32(seg + 4) + 1;
        c->mss = peer_mss(seg, hl);
        c->snd_wnd = be16(seg + 14);
        output(c);                              /* the SYN-ACK */
        return;
    }
    refuse(src, src_mac, seg, dlen);
}

/* ---- the callers' side ---- */
void tcp_poll(void)
{
    u64 now = now_ms();
    for (int i = 0; i < CONNS; i++) {
        struct conn *c = &conns[i];
        if (c->state == CLOSED)
            continue;
        if (!eth_ready()) {
            set_closed(c, "the network went away");
            continue;
        }
        if (c->timer && now >= c->timer)
            timeout(c, now);
        if (c->state == CLOSED)
            continue;
        output(c);
        if (c->ack_due)
            send_ack(c);
    }
}

int tcp_busy(void)
{
    u64 now = now_ms();
    for (int i = 0; i < CONNS; i++) {
        const struct conn *c = &conns[i];
        if (c->state == CLOSED || c->state == TIME_WAIT)
            continue;
        if (c->state != ESTABLISHED && c->state != CLOSE_WAIT && c->state != FIN_WAIT_2)
            return 1;                           /* opening or closing */
        if (c->snd_nxt != c->snd_una || c->snd_nxt - c->s_seq < c->s_len || c->ack_due || now - c->moved < BUSY_MS)
            return 1;
    }
    return 0;
}

void tcp_drop_all(void)
{
    for (int i = 0; i < CONNS; i++)
        if (conns[i].state != CLOSED)
            set_closed(&conns[i], "the network went away");
}

int tcp_listen(u16 port)
{
    if (listened(port))
        return 0;
    for (int i = 0; i < LISTENS; i++)
        if (!listening[i]) {
            listening[i] = port;
            return 0;
        }
    return -1;
}

int tcp_accept(u16 port)
{
    for (int i = 0; i < CONNS; i++) {
        struct conn *c = &conns[i];
        if (!c->held && !c->fin_queued && c->lport == port && (c->state == ESTABLISHED || c->state == CLOSE_WAIT)) {
            /* (fin_queued: one its caller closed, its FIN waiting for the peer's window -- not a new one) */
            c->held = 1;
            return i;
        }
    }
    return -1;
}

int tcp_connect(const u8 ip[4], u16 port)
{
    u8 mac[6];
    struct conn *c = free_conn();
    if (!c)
        return -1;
    c->held = 1;                                /* (eth_route's ARP handles frames for up to a second: a SYN to
                                                   the telnet port then must not take this slot) */
    if (!eth_route(ip, mac)) {
        c->held = 0;
        return -1;
    }
    if (next_port < 49200 || next_port >= 65000)
        next_port = (u16)(49200 + cp0_count() % 8000);
    c->held = 1;
    start(c, ip, mac, next_port++, port);
    c->state = SYN_SENT;
    output(c);
    return index_of(c);
}

int tcp_status(int i)
{
    const struct conn *c = get(i);
    if (!c)
        return TCP_FAILED;
    switch (c->state) {
    case SYN_SENT:
    case SYN_RCVD:
        return TCP_CONNECTING;
    case ESTABLISHED:
    case FIN_WAIT_1:
    case FIN_WAIT_2:
        return TCP_OPEN;
    case CLOSE_WAIT:
    case CLOSING:
    case LAST_ACK:
        return TCP_PEER_DONE;
    case TIME_WAIT:
        return TCP_DONE;
    }
    return c->why ? TCP_FAILED : TCP_DONE;
}

const char *tcp_why(int i)
{
    const struct conn *c = get(i);
    return c && c->why ? c->why : "closed";
}

int tcp_send(int i, const void *data, int len)
{
    struct conn *c = get(i);
    if (!c || len <= 0 || c->fin_queued
        || (c->state != ESTABLISHED && c->state != CLOSE_WAIT && c->state != SYN_SENT && c->state != SYN_RCVD))
        return 0;
    u32 room = SEND_SIZE - c->s_len, n = (u32)len < room ? (u32)len : room;
    const u8 *d = data;
    u8 *b = sbuf[i];
    u32 at = (c->s_head + c->s_len) % SEND_SIZE;
    for (u32 k = 0; k < n; k++) {
        b[at] = d[k];
        at = (at + 1) % SEND_SIZE;
    }
    c->s_len += n;
    return (int)n;
}

u32 tcp_room(int i)
{
    const struct conn *c = get(i);
    return c && !c->fin_queued && (c->state == ESTABLISHED || c->state == CLOSE_WAIT) ? SEND_SIZE - c->s_len : 0;
}

void tcp_push(int i)
{
    struct conn *c = get(i);
    if (c && c->state != CLOSED && !eth_inside())
        output(c);
}

/* the bytes received; the window offered again when reading opened it far (else the peer, seeing a closed
   window, would wait for the probe) */
int tcp_recv(int i, void *data, int max)
{
    struct conn *c = get(i);
    if (!c || max <= 0 || !c->r_len)
        return 0;
    u32 n = c->r_len < (u32)max ? c->r_len : (u32)max;
    const u8 *b = rbuf[i];
    u8 *d = data;
    for (u32 k = 0; k < n; k++) {
        d[k] = b[c->r_head];
        c->r_head = (c->r_head + 1) % RECV_SIZE;
    }
    c->r_len -= n;
    u32 now_open = window(c);
    if (c->state != CLOSED && now_open > c->r_wnd && now_open - c->r_wnd >= RECV_SIZE / 4) {
        c->ack_due = 1;
        if (!eth_inside())
            send_ack(c);
    }
    return (int)n;
}

u32 tcp_unread(int i)
{
    const struct conn *c = get(i);
    return c ? c->r_len : 0;
}

void tcp_shutdown(int i)
{
    struct conn *c = get(i);
    if (!c || c->fin_queued)
        return;
    if (c->state == SYN_SENT || c->state == SYN_RCVD) {
        reset(c, 0);
        return;
    }
    c->fin_queued = 1;
    if (!eth_inside())
        output(c);
}

void tcp_close(int i)
{
    struct conn *c = get(i);
    if (!c)
        return;
    tcp_shutdown(i);
    c->held = 0;
    if (c->state == CLOSE_WAIT || c->state == ESTABLISHED || c->state == FIN_WAIT_1 || c->state == FIN_WAIT_2) {
        c->r_len = 0;                           /* (what the peer sends from now on is not read: ACKed and dropped) */
        c->r_head = 0;
    }
}

void tcp_abort(int i)
{
    struct conn *c = get(i);
    if (!c)
        return;
    if (c->state != CLOSED)
        reset(c, 0);
    c->held = 0;
}

void tcp_peer(int i, u8 ip[4], u16 *port)
{
    const struct conn *c = get(i);
    if (c) {
        copy(ip, c->ip, 4);
        *port = c->rport;
    }
}

void tcp_list(void)
{
    uprintf("TCP listening on port(s):");
    for (int i = 0; i < LISTENS; i++)
        if (listening[i])
            uprintf(" %u", listening[i]);
    uprintf("\n");
    int n = 0;
    for (int i = 0; i < CONNS; i++) {
        const struct conn *c = &conns[i];
        if (c->state == CLOSED)
            continue;
        u32 off = c->snd_nxt - c->s_seq;
        uprintf("  %d: :%u <-> %u.%u.%u.%u:%u %s, %u bytes sent (%u in flight, %u waiting), %u received (%u not "
                "read), the peer's window %u\n", i, c->lport, c->ip[0], c->ip[1], c->ip[2], c->ip[3], c->rport,
                state_names[c->state], c->sent, c->snd_nxt - c->snd_una, off < c->s_len ? c->s_len - off : 0,
                c->received, c->r_len, c->snd_wnd);
        n++;
    }
    if (!n)
        uprintf("  no connections\n");
    uprintf("  segments: %u in, %u out; %u time-outs (sent again), %u out of order or old (dropped), %u with a bad "
            "checksum, %u resets sent\n", segs_in, segs_out, timeouts, dropped_ooo, bad_sums, resets_sent);
}
