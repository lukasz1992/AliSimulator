/* ssdp.c: SSDP, UPnP's discovery (UDP 1900, multicast group 239.255.255.250) -- the box shows up in Windows'
   Network folder ("Other devices"; double-click: its web page), in UPnP browsers and in home-automation scans.

   A basic UPnP root device (urn:schemas-upnp-org:device:Basic:1) whose UUID comes from the link's MAC (the
   same every start).  Each time DHCP gives (or renews) the address: NOTIFY ssdp:alive for its three names
   (upnp:rootdevice, its uuid, its device type) three times 2 s apart, then every 10 minutes (max-age 30
   minutes); an IGMPv2 membership report for the group every minute, so a switch that snoops IGMP sends it the
   searches; an M-SEARCH for
   ssdp:all, upnp:rootdevice, its uuid or its type -- to the group or to the box -- answered to its sender
   (after up to 0.3 s: several devices at once don't flood it).  Its description, /ssdp/device.xml, comes from
   the web page's server (http.c): the name, the model, the presentation URL (the web page).

   The Ethernet adapter's filter takes all multicast frames for it (eth.c: CDC ECM's ALL_MULTICAST, RNDIS's);
   eth.c hands over the group's UDP 1900 datagrams, net.c those sent to the box's own address.  Nothing is
   sent from inside eth.c's frame handling: answers wait in a small queue for ssdp_poll (the main loop). */
#include "hw.h"

#define SSDP_PORT       1900
#define MAX_AGE         1800
#define EVERY_MS        600000u                 /* the announcements: under half of max-age */
#define QUEUE           6
#define DEVICE_TYPE     "urn:schemas-upnp-org:device:Basic:1"
#define SERVER          "M3801/" APP_VERSION " UPnP/1.0 m3801-demo/" APP_VERSION

static const u8 group[4] = {239, 255, 255, 250};
static u8 frame[14 + 20 + 8 + 1024 + 64];
static char uuid[48];                           /* "uuid:4d333830-3176-3400-8000-001122334455" */
static u8 announced[4];                         /* the address the announcements gave */
static u32 bound = ~0u;                         /* eth_bound_count() then (a link that came back, a lease renewed:
                                                   announced again even with the same address) */
static u64 next_at, igmp_at;
static int rounds;                              /* announcements left of the first three */
static u32 notifies, searches, answers, ignored;
static u8 last_from[4];

static struct answer {
    u8 used, has_mac;
    u8 ip[4], mac[6];
    u16 port;
    u8 which;                                   /* bits: 1 rootdevice, 2 uuid, 4 device type */
    u64 due;
} queue[QUEUE];

static int same4(const u8 *a, const u8 *b)
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* does line `l` (up to its end) start with `p` (any case)? the rest after it, or 0 */
static const char *field(const char *l, const char *end, const char *p)
{
    for (; *p; p++, l++)
        if (l >= end || lower(*l) != lower(*p))
            return 0;
    while (l < end && (*l == ' ' || *l == '\t'))
        l++;
    return l;
}

static int same_text(const char *a, const char *a_end, const char *b)
{
    for (; *b; a++, b++)
        if (a >= a_end || lower(*a) != lower(*b))
            return 0;
    while (a < a_end && (*a == ' ' || *a == '\t' || *a == '\r'))
        a++;
    return a == a_end;
}

static void make_uuid(void)
{
    const u8 *m = eth_mac();
    usnprintf(uuid, sizeof(uuid), "uuid:4d333830-3176-3400-8000-%02x%02x%02x%02x%02x%02x", m[0], m[1], m[2], m[3],
              m[4], m[5]);
}

/* eth.c (the group's datagrams: mac, the sender's) and net.c (to the box's own address: mac 0): an M-SEARCH
   is queued for an answer; anything else (other devices' NOTIFYs) is left */
void ssdp_input(const u8 *src, const u8 *mac, u16 sport, const u8 *data, int len)
{
    const char *t = (const char *)data, *end = t + len;
    const char *line_end = t;
    while (line_end < end && *line_end != '\n')
        line_end++;
    if (!field(t, line_end, "M-SEARCH * HTTP/1.1") || !eth_ready() || same4(src, eth_address()))
        return;
    searches++;
    if (!uuid[0])
        make_uuid();
    int discover = 0;
    u8 which = 0;
    for (const char *l = line_end + 1; l < end;) {
        const char *e = l;
        while (e < end && *e != '\n')
            e++;
        const char *v;
        if ((v = field(l, e, "MAN:")) && (field(v, e, "\"ssdp:discover\"") || field(v, e, "ssdp:discover")))
            discover = 1;
        else if ((v = field(l, e, "ST:"))) {
            if (same_text(v, e, "ssdp:all"))
                which = 7;
            else if (same_text(v, e, "upnp:rootdevice"))
                which = 1;
            else if (same_text(v, e, uuid))
                which = 2;
            else if (same_text(v, e, DEVICE_TYPE))
                which = 4;
        }
        l = e + 1;
    }
    if (!discover || !which) {
        ignored++;
        return;
    }
    for (int i = 0; i < QUEUE; i++)
        if (!queue[i].used) {
            struct answer *a = &queue[i];
            a->used = 1;
            for (int k = 0; k < 4; k++)
                a->ip[k] = src[k];
            a->has_mac = mac != 0;
            for (int k = 0; mac && k < 6; k++)
                a->mac[k] = mac[k];
            a->port = sport;
            a->which = which;
            a->due = now_ms() + cp0_count() % 300;
            return;
        }
    ignored++;                                  /* (a queue full of searches: this one goes unanswered) */
}

/* the three names: NT / ST, and USN */
static void names(int k, const char **nt, char *usn, int size)
{
    *nt = k == 0 ? "upnp:rootdevice" : k == 1 ? uuid : DEVICE_TYPE;
    if (k == 1)
        usnprintf(usn, size, "%s", uuid);
    else
        usnprintf(usn, size, "%s::%s", uuid, *nt);
}

/* a UDP datagram from port 1900 (its text at frame + 42) to ip:port through mac: 0 sent, -1 not */
static int send_udp(const u8 *mac, const u8 *ip, u16 port, int n, int multicast)
{
    u8 *u = frame + 34;
    u[0] = SSDP_PORT >> 8, u[1] = SSDP_PORT & 0xFF;
    u[2] = (u8)(port >> 8), u[3] = (u8)port;
    u[4] = (u8)((8 + n) >> 8), u[5] = (u8)(8 + n);
    u[6] = u[7] = 0;                            /* (no checksum: IPv4 allows it) */
    return multicast ? eth_send_multicast(frame, ip, 17, 8 + n, 4) : eth_send_ipv4(frame, mac, ip, 17, 8 + n);
}

static void announce(void)
{
    const u8 *ip = eth_address();
    char *t = (char *)frame + 42, usn[96];
    for (int k = 0; k < 3; k++) {
        const char *nt;
        names(k, &nt, usn, sizeof(usn));
        int n = usnprintf(t, 1024, "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nCACHE-CONTROL: max-age=%u\r\n"
                          "LOCATION: http://%u.%u.%u.%u/ssdp/device.xml\r\nNT: %s\r\nNTS: ssdp:alive\r\n"
                          "SERVER: " SERVER "\r\nUSN: %s\r\n\r\n", MAX_AGE, ip[0], ip[1], ip[2], ip[3], nt, usn);
        if (send_udp(0, group, SSDP_PORT, n, 1) == 0)
            notifies++;
    }
}

/* IGMPv2's membership report for the group (TTL 1): a switch that snoops IGMP forwards its traffic here (sent
   every minute: queries are not answered, and such a switch forgets a member after about 4 minutes) */
static void join(void)
{
    u8 *g = frame + 34;
    g[0] = 0x16, g[1] = 0, g[2] = g[3] = 0;
    for (int k = 0; k < 4; k++)
        g[4 + k] = group[k];
    u32 sum = (u32)(g[0] << 8 | g[1]) + (u32)(g[4] << 8 | g[5]) + (u32)(g[6] << 8 | g[7]);
    sum = (sum & 0xFFFF) + (sum >> 16);
    sum = ~((sum & 0xFFFF) + (sum >> 16)) & 0xFFFF;
    g[2] = (u8)(sum >> 8), g[3] = (u8)sum;
    (void)eth_send_multicast(frame, group, 2, 8, 1);
}

static void answer(struct answer *a)
{
    u8 mac[6];
    if (a->has_mac) {
        for (int k = 0; k < 6; k++)
            mac[k] = a->mac[k];
    } else if (!eth_route(a->ip, mac)) {
        return;
    }
    const u8 *ip = eth_address();
    char *t = (char *)frame + 42, usn[96];
    for (int k = 0; k < 3; k++) {
        if (!(a->which & (1 << k)))
            continue;
        const char *st;
        names(k, &st, usn, sizeof(usn));
        int n = usnprintf(t, 1024, "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=%u\r\nEXT:\r\n"
                          "LOCATION: http://%u.%u.%u.%u/ssdp/device.xml\r\nSERVER: " SERVER "\r\nST: %s\r\n"
                          "USN: %s\r\n\r\n", MAX_AGE, ip[0], ip[1], ip[2], ip[3], st, usn);
        if (send_udp(mac, a->ip, a->port, n, 0) == 0)
            answers++;
    }
    for (int k = 0; k < 4; k++)
        last_from[k] = a->ip[k];
}

/* net_poll (the main loop): the announcements when they are due, the answers waiting; nothing to show (0) */
const char *ssdp_poll(void)
{
    if (!eth_ready() || eth_inside())
        return 0;
    u64 now = now_ms();
    if (bound != eth_bound_count() || !same4(announced, eth_address())) {
        bound = eth_bound_count();      /* (an address from DHCP: announced at once, three times; the link's MAC --
                                           the UUID's -- may be another's; searches from before it dropped) */
        for (int k = 0; k < 4; k++)
            announced[k] = eth_address()[k];
        make_uuid();
        rounds = 3;
        next_at = igmp_at = now;
        for (int i = 0; i < QUEUE; i++)
            queue[i].used = 0;
    }
    if (now >= igmp_at) {
        join();
        igmp_at = now + 60000;
    }
    if (now >= next_at) {
        announce();
        if (rounds > 0)
            rounds--;
        next_at = now + (rounds > 0 ? 2000 : EVERY_MS);
    }
    for (int i = 0; i < QUEUE; i++)
        if (queue[i].used && now >= queue[i].due) {
            answer(&queue[i]);
            queue[i].used = 0;
        }
    return 0;
}

/* http.c: /ssdp/device.xml (the device's description): its length */
int ssdp_description(char *b, int size)
{
    const u8 *ip = eth_address();
    if (!uuid[0])
        make_uuid();
    return usnprintf(b, size,
                     "<?xml version=\"1.0\"?>\r\n"
                     "<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\r\n"
                     "<specVersion><major>1</major><minor>0</minor></specVersion>\r\n"
                     "<device>\r\n"
                     "<deviceType>" DEVICE_TYPE "</deviceType>\r\n"
                     "<friendlyName>M3801 " APP_VERSION " demo (%u.%u.%u.%u)</friendlyName>\r\n"
                     "<manufacturer>M3801 " APP_VERSION " demo</manufacturer>\r\n"
                     "<modelDescription>A set-top box's demo: web page, FTP and SMB file servers "
                     "(\\\\%u.%u.%u.%u), telnet console</modelDescription>\r\n"
                     "<modelName>Set-top box (ALi M3801 / S3811)</modelName>\r\n"
                     "<modelNumber>" APP_VERSION "</modelNumber>\r\n"
                     "<UDN>%s</UDN>\r\n"
                     "<presentationURL>http://%u.%u.%u.%u/</presentationURL>\r\n"
                     "</device>\r\n"
                     "</root>\r\n",
                     ip[0], ip[1], ip[2], ip[3], ip[0], ip[1], ip[2], ip[3], uuid, ip[0], ip[1], ip[2], ip[3]);
}

/* 'netstat' */
void ssdp_status(void)
{
    if (!eth_ready()) {
        uprintf("SSDP (UPnP discovery, UDP 1900): waits for an address\n");
        return;
    }
    uprintf("SSDP (UPnP discovery, UDP 1900): %s, %u NOTIFYs sent, %u searches seen, %u answers sent", uuid,
            notifies, searches, answers);
    if (answers)
        uprintf(" (the last to %u.%u.%u.%u)", last_from[0], last_from[1], last_from[2], last_from[3]);
    uprintf("\n");
}
