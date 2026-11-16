/* USB serial ports -- what Linux makes /dev/ttyUSB<n> and ttyACM<n> of: a
   GSM / LTE module's vendor-class interfaces with a bulk IN and a bulk OUT
   endpoint (its AT, modem and diagnostic ports, as Linux's option driver
   takes them), or a CDC ACM data interface, or a USB-serial chip's port -- WCH's CH340 /
   CH341, Silicon Labs' CP210x, FTDI's FT232 / FT2232 / FT4232 / FT232H /
   FT-X, Prolific's PL2303 (HX), each set up the way Linux's driver for it
   does.  usb.c hands them over (serial_attach) as it sets a device up.
   They start at 115200 baud, 8 data bits, no parity, 1 stop bit, DTR and
   RTS on; 'stty [n] [baud]' shows or sets the baud rate (an LTE module's
   own ports have none).

   'tty' lists them.  'cu [n]' joins the terminal -- the UART and the USB
   keyboard -- to port n (without n: the first one named "AT"): what is typed
   goes out to it, what comes in is shown, until "~." at a line's start or
   Ctrl-] (as Linux's cu and telnet).  'at <command>' sends one AT command to
   that port ("AT" put in front when it is not there: 'at +csq', 'at i') and
   shows the answer up to its final result (OK, ERROR, +CME / +CMS ERROR, NO
   CARRIER, or the "> " of AT+CMGS) and what follows it at once (AT+HTTPREAD's
   data); lines that came before the command (+HTTPACTION, RING) first.  A
   token spoilt on the bus is tried again (usb.c); 'cu' goes on through a
   failed transfer while the device is still set up.

   The answers come in packets shorter than asked for: usb_bulk_in_packet
   reads them (usb.c says how the controller is made to). */
#include "hw.h"

#define PORTS SERIAL_PORTS

struct port {
    u8 used, acm, device, iface, in_ep, out_ep;
    u8 chip, control;                   /* SERIAL_* (hw.h); the interface with the modem control */
    u8 version;                         /* a CH34x's (its line control's registers differ below 0x30) */
    u8 limited;                         /* a CH34x clone without the fast prescaler (Linux's
                                           CH341_QUIRK_LIMITED_PRESCALER: its break register stalls) */
    u16 maxp;
    u32 baud;                           /* 0: none to set (an LTE module's own ports) */
    char name[32];
};

static struct port ports[PORTS];
static char said[96];
static u8 in[512];                      /* what a port sent ('cu' and 'at' are never at the same time) */

static const char *tty(const struct port *p)
{
    return p->acm ? "ttyACM" : "ttyUSB";
}

/* ---- the chips ---- */
/* which USB-serial chip a device is (its vendor-class interfaces are serial ports): SERIAL_*, 0 none */
int serial_chip(u16 vid, u16 pid, u16 bcd)
{
    if (vid == 0x0403 && pid == 0x6010 && bcd < 0x0700)
        return SERIAL_FTDI_X;                   /* (an FT2232C/D: the full-speed chip, 48 MHz) */
    static const struct {
        u16 vid, pid;
        u8 chip;
    } known[] = {
        {0x1A86, 0x7523, SERIAL_CH34X}, {0x1A86, 0x5523, SERIAL_CH34X}, {0x1A86, 0x7522, SERIAL_CH34X},
        {0x1A86, 0xE523, SERIAL_CH34X},
        {0x10C4, 0xEA60, SERIAL_CP210X}, {0x10C4, 0xEA61, SERIAL_CP210X}, {0x10C4, 0xEA63, SERIAL_CP210X},
        {0x10C4, 0xEA70, SERIAL_CP210X}, {0x10C4, 0xEA71, SERIAL_CP210X},
        {0x0403, 0x6001, SERIAL_FTDI}, {0x0403, 0x6010, SERIAL_FTDI_H}, {0x0403, 0x6011, SERIAL_FTDI_H},
        {0x0403, 0x6014, SERIAL_FTDI_H}, {0x0403, 0x6015, SERIAL_FTDI_X},
        {0x067B, 0x2303, SERIAL_PL2303},
    };
    for (unsigned i = 0; i < sizeof(known) / sizeof(known[0]); i++)
        if (known[i].vid == vid && known[i].pid == pid)
            return known[i].chip;
    return 0;
}

static const char *chip_name(int chip)
{
    switch (chip) {
    case SERIAL_CH34X: return "CH340 / CH341";
    case SERIAL_CP210X: return "CP210x";
    case SERIAL_FTDI:
    case SERIAL_FTDI_X: return "FTDI";
    case SERIAL_ACM: return "CDC ACM";
    case SERIAL_FTDI_H: return "FTDI (high-speed)";
    case SERIAL_PL2303: return "PL2303";
    }
    return "";
}

static int out_req(const struct port *p, u8 rtype, u8 req, u16 value, u16 index, const void *data, u16 len)
{
    return usb_request(p->device, rtype, req, value, index, (u8 *)data, len, 500) < 0 ? -1 : 0;
}

/* a CH34x's baud rate register (Linux's ch341_get_divisor): prescaler ps, a base clock 48 MHz / 2^(12 - 3ps
   - fact), the divisor 256 - div; 0 when the rate cannot be made */
static u16 ch34x_divisor(u32 speed, int limited)
{
    static const u32 min_rate[4] = {48000000u / (2048u * 512u), 48000000u / (256u * 512u),
                                    48000000u / (32u * 512u), 48000000u / (4u * 512u)};
    if (speed < 46 || speed > 2000000)
        return 0;
    int ps = 3;
    while (ps >= 0 && speed <= min_rate[ps])
        ps--;
    if (ps < 0)
        return 0;
    u32 fact = 1, clk_div = 1u << (12 - 3 * ps - (int)fact), div = 48000000u / (clk_div * speed);
    if (div < 9 || div > 255 || (limited && ps < 3)) {
        div /= 2;
        clk_div *= 2;
        fact = 0;
    }
    if (div < 2)
        return 0;
    if (16u * 48000000u / (clk_div * div) - 16u * speed >= 16u * speed - 16u * 48000000u / (clk_div * (div + 1)))
        div++;
    if (fact == 1 && !(div % 2)) {
        div /= 2;
        fact = 0;
    }
    return (u16)((0x100 - div) << 8 | fact << 2 | (u32)ps);
}

/* an FTDI's baud rate as its wValue and wIndex (Linux's ftdi_232bm / ftdi_2232h_baud_base_to_divisor) */
static u32 ftdi_divisor(int high, u32 baud)
{
    static const u8 frac[8] = {0, 3, 2, 4, 1, 5, 6, 7};
    u32 d3 = high ? (8u * 120000000u + 5u * baud) / (10u * baud) : (48000000u + baud) / (2u * baud);
    u32 d = d3 >> 3 | (u32)frac[d3 & 7] << 14;
    if (d == 1)
        d = 0;
    else if (d == 0x4001)
        d = 1;
    return high ? d | 0x20000 : d;
}

/* the port's line: `baud`, 8 data bits, no parity, 1 stop bit, DTR and RTS on.  0, -1 (said why) */
static int line_set(struct port *p, u32 baud)
{
    u8 coding[7] = {(u8)baud, (u8)(baud >> 8), (u8)(baud >> 16), (u8)(baud >> 24), 0, 0, 8};
    int r = 0;
    switch (p->chip) {
    case SERIAL_CH34X: {
        u16 div = ch34x_divisor(baud, p->limited);
        if (!div) {
            uprintf("a CH34x does not make %u baud (46 .. 2000000)\n", baud);
            return -1;
        }
        u16 val = (u16)(div | (p->version > 0x27 ? 0x80 : 0));
        r = out_req(p, 0x40, 0x9A, 0x1312, val, 0, 0);              /* WRITE_REG: divisor, prescaler */
        if (!r && p->version >= 0x30)
            r = out_req(p, 0x40, 0x9A, 0x2518, 0xC3, 0, 0);         /* line control: RX, TX on, 8 bits */
        if (!r)
            r = out_req(p, 0x40, 0xA4, (u16)~0x60, 0, 0, 0);        /* MODEM_CTRL: DTR and RTS (active low) */
        break;
    }
    case SERIAL_CP210X: {
        u8 b[4] = {(u8)baud, (u8)(baud >> 8), (u8)(baud >> 16), (u8)(baud >> 24)};
        r = out_req(p, 0x41, 0x1E, 0, p->iface, b, 4);              /* SET_BAUDRATE */
        if (!r)
            r = out_req(p, 0x41, 0x03, 0x0800, p->iface, 0, 0);     /* SET_LINE_CTL: 8 bits, N, 1 */
        if (!r)
            r = out_req(p, 0x41, 0x07, 0x0303, p->iface, 0, 0);     /* SET_MHS: DTR, RTS */
        break;
    }
    case SERIAL_FTDI:
    case SERIAL_FTDI_X:
    case SERIAL_FTDI_H: {
        int high = p->chip == SERIAL_FTDI_H;
        if ((high && baud < 1200) || baud < 183 || baud > (high ? 12000000u : 3000000u)) {
            uprintf("this FTDI chip makes %u .. %u baud\n", high ? 1200u : 183u, high ? 12000000u : 3000000u);
            return -1;
        }
        u32 d = ftdi_divisor(high, baud);
        u16 port = p->control;                  /* (0 a single port; 1.. a multi-port chip's A, B, ...) */
        u16 index = p->chip != SERIAL_FTDI ? (u16)((d >> 16) << 8 | port) : (u16)(d >> 16);
        r = out_req(p, 0x40, 0x03, (u16)d, index, 0, 0);            /* SET_BAUD_RATE */
        if (!r)
            r = out_req(p, 0x40, 0x04, 8, port, 0, 0);              /* SET_DATA: 8 bits, N, 1 */
        if (!r)
            r = out_req(p, 0x40, 0x01, 0x0303, port, 0, 0);         /* SET_MODEM_CTRL: DTR, RTS */
        break;
    }
    case SERIAL_PL2303:
    case SERIAL_ACM:
        r = out_req(p, 0x21, 0x20, 0, p->control, coding, 7);       /* SET_LINE_CODING */
        if (!r)
            r = out_req(p, 0x21, 0x22, 3, p->control, 0, 0);        /* SET_CONTROL_LINE_STATE: DTR, RTS */
        break;
    default:
        return 0;
    }
    if (r) {
        uprintf("%s: the line could not be set: %s\n", chip_name(p->chip), usb_error_text());
        return -1;
    }
    p->baud = baud;
    return 0;
}

/* the chip's own start (before its line is set): 0, -1 */
static int chip_start(struct port *p)
{
    u8 b[2];
    switch (p->chip) {
    case SERIAL_CH34X:
        if (usb_request(p->device, 0xC0, 0x5F, 0, 0, b, 2, 500) != 2)     /* READ_VERSION */
            return -1;
        p->version = b[0];
        p->limited = usb_request(p->device, 0xC0, 0x95, 0x0005, 0, b, 2, 500) < 0 && (usb_last_error() & 0x80);
        return out_req(p, 0x40, 0xA1, 0, 0, 0, 0);                         /* SERIAL_INIT */
    case SERIAL_CP210X:
        return out_req(p, 0x41, 0x00, 1, p->iface, 0, 0);                  /* IFC_ENABLE */
    case SERIAL_FTDI:
    case SERIAL_FTDI_X:
    case SERIAL_FTDI_H:
        return out_req(p, 0x40, 0x00, 0, p->control, 0, 0);                /* RESET */
    case SERIAL_PL2303: {
        /* Linux's pl2303_startup for an HX: vendor reads and writes in this order */
        static const struct {
            u8 write;
            u16 value, index;
        } steps[] = {{0, 0x8484, 0}, {1, 0x0404, 0}, {0, 0x8484, 0}, {0, 0x8383, 0}, {0, 0x8484, 0},
                     {1, 0x0404, 1}, {0, 0x8484, 0}, {0, 0x8383, 0}, {1, 0, 1}, {1, 1, 0}, {1, 2, 0x44}};
        for (unsigned i = 0; i < sizeof(steps) / sizeof(steps[0]); i++) {
            int r = steps[i].write ? out_req(p, 0x40, 0x01, steps[i].value, steps[i].index, 0, 0)
                                   : (usb_request(p->device, 0xC0, 0x01, steps[i].value, 0, b, 1, 500) < 0 ? -1 : 0);
            if (r)
                return -1;
        }
        return 0;
    }
    }
    return 0;
}

/* `kind`: SERIAL_ACM, a chip's SERIAL_*, or 0 a module's own port (no line to set) */
const char *serial_attach(u8 device, u8 iface, u8 control, int kind, u8 in_ep, u8 out_ep, u16 maxp,
                          const char *name)
{
    for (int i = 0; i < PORTS; i++) {
        struct port *p = &ports[i];
        if (p->used)
            continue;
        p->used = 1;
        p->acm = kind == SERIAL_ACM;
        p->chip = (u8)kind;
        p->control = control;
        p->device = device;
        p->iface = iface;
        p->in_ep = in_ep;
        p->out_ep = out_ep;
        p->maxp = maxp;
        p->baud = 0;
        p->version = 0;
        usnprintf(p->name, sizeof(p->name), "%s", name[0] ? name : chip_name(kind));
        if (kind && kind != SERIAL_ACM) {
            if (chip_start(p) < 0 || line_set(p, 115200) < 0) {
                usnprintf(said, sizeof(said), "a %s port that does not answer its set-up: %s -- left alone",
                          chip_name(kind), usb_error_text());
                p->used = 0;
                return said;
            }
            usnprintf(said, sizeof(said), "%s%d: a %s serial port, 115200 baud 8N1: 'cu %d', 'stty %d <baud>'",
                      tty(p), i, chip_name(kind), i, i);
            return said;
        }
        if (kind == SERIAL_ACM)
            (void)line_set(p, 115200);
        /* DTR and RTS on (SET_CONTROL_LINE_STATE, to the interface with the modem control: CDC ACM's
           communications interface), as Linux does when it opens the port; one without may refuse it */
        (void)usb_request(device, 0x21, 0x22, 3, control, 0, 0, 100);
        usnprintf(said, sizeof(said), "%s%d: a serial port (interface %u%s%s): 'cu %d'", tty(p), i, iface,
                  p->name[0] ? ", " : "", p->name, i);
        return said;
    }
    usnprintf(said, sizeof(said), "a serial port, but %d are known already: left alone", PORTS);
    return said;
}

void serial_detach(u8 device)
{
    for (int i = 0; i < PORTS; i++)
        if (!device || ports[i].device == device)
            ports[i].used = 0;
}

int serial_count(void)
{
    int n = 0;
    for (int i = 0; i < PORTS; i++)
        n += ports[i].used;
    return n;
}

void serial_list(void)
{
    for (int i = 0; i < PORTS; i++) {
        const struct port *p = &ports[i];
        if (!p->used)
            continue;
        uprintf("%s%d  device %u interface %u: %s (bulk in %02x, out %02x, %u-byte packets)", tty(p), i,
                p->device, p->iface, p->name[0] ? p->name : "a serial port", 0x80 | p->in_ep, p->out_ep, p->maxp);
        if (p->baud)
            uprintf(", %u baud 8N1", p->baud);
        uprintf("\n");
    }
    if (!serial_count())
        uprintf("no USB serial ports (a GSM / LTE module, a CDC ACM device: 'lsusb' shows what is there)\n");
}

/* the port 'at' and 'cu' without a number use: the first named "... AT ...", else the first */
static int at_port(void)
{
    int first = -1;
    for (int i = 0; i < PORTS; i++) {
        if (!ports[i].used)
            continue;
        if (first < 0)
            first = i;
        const char *s = ports[i].name;
        for (int k = 0; s[k] && s[k + 1]; k++)
            if (s[k] == 'A' && s[k + 1] == 'T' && (k == 0 || s[k - 1] == ' ') && (s[k + 2] == ' ' || !s[k + 2]))
                return i;
    }
    return first;
}

static int port_ok(int n)
{
    if (n >= 0 && n < PORTS && ports[n].used)
        return 1;
    if (!serial_count())
        uprintf("no USB serial port ('tty' lists them)\n");
    else
        uprintf("no serial port %d ('tty' lists them)\n", n);
    return 0;
}

/* a transfer on port n failed (usb.c tried a token spoilt on the bus again already).  0: its device is
   still set up: go on.  Else it lost its set-up -- an LTE module that restarted: the A7670E after AT+CRESET
   came back twice and the second time had lost its configuration without the port's connection changing --
   and it is set up again (usb_set_up_again: behind a hub that port alone): 1 when port n is back (the same
   interface), -1 it is not */
static int recover(int n)
{
    u8 iface = ports[n].iface;
    if (usb_still_set_up(ports[n].device))
        return 0;
    uprintf("(the device is not set up any more -- restarted? -- setting it up again)\n");
    if (usb_set_up_again(ports[n].device) < 0) {
        uprintf("(it does not answer: %s)\n", usb_error_text());
        return -1;
    }
    return ports[n].used && ports[n].iface == iface ? 1 : -1;
}

/* 'stty [n] [baud]': the ports' line, or port n's baud rate set */
void serial_stty(int n, u32 baud)
{
    if (n < 0) {
        serial_list();
        return;
    }
    if (!port_ok(n))
        return;
    struct port *p = &ports[n];
    if (!baud) {
        if (p->baud)
            uprintf("%s%d: %u baud, 8 data bits, no parity, 1 stop bit\n", tty(p), n, p->baud);
        else
            uprintf("%s%d: a port without a line to set (a module's own)\n", tty(p), n);
        return;
    }
    if (!p->chip) {
        uprintf("%s%d: a module's own port has no baud rate\n", tty(p), n);
        return;
    }
    if (line_set(p, baud) == 0)
        uprintf("%s%d: %u baud 8N1\n", tty(p), n, baud);
}

/* one packet from the port: what usb_bulk_in_packet gives, without an FTDI chip's two status bytes at the
   start of every packet */
static int port_read(const struct port *p, u8 *buf, u32 timeout_ms)
{
    int k = usb_bulk_in_packet(p->device, p->in_ep, p->maxp, buf, timeout_ms);
    if (k > 0 && (p->chip == SERIAL_FTDI || p->chip == SERIAL_FTDI_X || p->chip == SERIAL_FTDI_H)) {
        for (int i = 2; i < k; i++)
            buf[i - 2] = buf[i];
        k = k > 2 ? k - 2 : 0;
    }
    return k;
}

/* the next character typed (the UART, then the USB keyboard), -1 none */
static int typed(void)
{
    int c = uart_getc();
    if (c >= 0)
        return c;
    usb_int_poll();
    (void)kbd_poll();
    return kbd_getc();
}

void serial_cu(int n)
{
    if (n < 0)
        n = at_port();
    if (!port_ok(n))
        return;
    struct port *p = &ports[n];
    uprintf("Connected to %s%d (device %u interface %u%s%s). What you type goes to it; \"~.\" at a line's "
            "start or Ctrl-] ends.\n", tty(p), n, p->device, p->iface, p->name[0] ? ", " : "", p->name);
    u8 out[64];
    int no = 0;                                 /* typed, not sent yet (after a failed send: sent again) */
    int line_start = 1, tilde = 0, done = 0, after_cr = 0, recoveries = 0, errors = 0;
    while (!done) {
        int c;
        while (no < (int)sizeof(out) - 1 && (c = typed()) >= 0) {
            if (c == 0x1D) {                    /* Ctrl-] */
                done = 1;
                break;
            }
            /* Enter as CR, the end of an AT command, whatever the terminal sends for it (CR, LF, CR LF) */
            int was_cr = after_cr;
            after_cr = c == '\r';
            if (c == '\n') {
                if (was_cr)
                    continue;
                c = '\r';
            }
            if (tilde) {
                tilde = 0;
                if (c == '.') {
                    done = 1;
                    break;
                }
                out[no++] = '~';
            }
            if (line_start && c == '~') {
                tilde = 1;
                continue;
            }
            out[no++] = (u8)c;
            line_start = c == '\r';
        }
        /* a failed transfer: with the device still set up the session goes on -- what was typed goes out
           the next time round, 50 failures in a row end it --; a device that lost its set-up is set up again
           (three times at most); one that is gone ends it (unplugged, restarting: the main loop sets it up
           when it is back) */
        int failed = no && usb_bulk_out(p->device, p->out_ep, p->maxp, out, (u32)no, 1000) < 0;
        if (!failed) {
            no = 0;
            int k = port_read(p, in, 20);
            for (int i = 0; i < k; i++)
                uart_putc((char)in[i]);
            failed = k == -1;
        }
        if (failed) {
            int r = recover(n);
            if (r < 0 || (r == 0 && ++errors == 50) || (r > 0 && ++recoveries > 3)) {
                uprintf("\n(the port failed: %s)", usb_error_text());
                break;
            }
            if (r == 0 && errors == 1)
                uprintf("\n(a USB transfer failed: %s -- going on)\n", usb_error_text());
            if (r > 0)
                uprintf("(%s%d is back: type on)\n", tty(p), n);
        } else {
            errors = 0;
        }
        console_service();
    }
    uprintf("\nDisconnected from %s%d.\n", tty(p), n);
}

/* a final result code: the answer to an AT command is complete */
static int final_line(const char *l)
{
    static const char *const finals[] = {"OK", "ERROR", "+CME ERROR", "+CMS ERROR", "NO CARRIER", "BUSY",
                                         "NO ANSWER", "NO DIALTONE", "CONNECT"};
    for (unsigned i = 0; i < sizeof(finals) / sizeof(finals[0]); i++) {
        const char *f = finals[i];
        int k = 0;
        while (f[k] && l[k] == f[k])
            k++;
        if (!f[k] && (!l[k] || l[k] == ':' || l[k] == ' '))
            return 1;
    }
    return 0;
}

/* the module's output a line at a time, as it comes (CRs dropped, other control bytes as '.'); a long line
   in pieces of 79 -- the page AT+HTTPREAD gives can be one line of hundreds */
struct lines {
    char l[80];
    int n, cont;                        /* its characters so far; a piece of it printed already */
};

/* one character; 1 when it ended a line that is a final result */
static int line_char(struct lines *s, char c)
{
    if (c == '\n') {
        s->l[s->n] = 0;
        uprintf("%s\n", s->l);
        int final = !s->cont && final_line(s->l);
        s->n = s->cont = 0;
        return final;
    }
    if (c == '\r')
        return 0;
    if (s->n == (int)sizeof(s->l) - 1) {
        s->l[s->n] = 0;
        uprintf("%s", s->l);
        s->n = 0;
        s->cont = 1;
    }
    s->l[s->n++] = c >= 32 && c < 127 ? c : '.';
    return 0;
}

/* a line without its end yet: printed and ended */
static void line_end(struct lines *s)
{
    if (s->n || s->cont)
        (void)line_char(s, '\n');
}

/* "AT..." in any case: an AT command ('at' puts "AT" in front of anything else) */
int at_prefixed(const char *s)
{
    return (s[0] == 'A' || s[0] == 'a') && (s[1] == 'T' || s[1] == 't');
}

/* After the final result the answer is read on for 250 ms of quiet: some come after their OK (AT+HTTPREAD's
   data).  Lines that came before the command -- unsolicited ones: +HTTPACTION, RING, +CMTI -- are shown
   first, as they are.  A command that did not go out is sent again when the device is still set up (or was
   set up again). */
#define AT_AFTER_MS 250

void serial_at(const char *command)
{
    int n = at_port();
    if (!port_ok(n))
        return;
    struct port *p = &ports[n];
    u8 cmd[100];
    int len = 0;
    if (!at_prefixed(command)) {
        cmd[len++] = 'A';
        cmd[len++] = 'T';
    }
    while (*command && len < (int)sizeof(cmd) - 1)
        cmd[len++] = (u8)*command++;
    cmd[len++] = '\r';
    static struct lines s;
    s.n = s.cont = 0;
    for (int attempt = 0;; attempt++) {
        int k;
        while ((k = port_read(p, in, 1)) > 0)
            for (int i = 0; i < k; i++)
                (void)line_char(&s, (char)in[i]);       /* (what came before the command: shown) */
        line_end(&s);
        if (usb_bulk_out(p->device, p->out_ep, p->maxp, cmd, (u32)len, 1000) == 0)
            break;
        uprintf("not sent to %s%d: %s\n", tty(p), n, usb_error_text());
        int r = attempt < 2 ? recover(n) : -1;
        if (r < 0)
            return;
        uprintf("(%s%d %s: the command again)\n", tty(p), n, r ? "is back" : "still set up");
    }
    int got = 0, done = 0;
    u64 start = now_ms(), last = start;
    for (;;) {
        int k = port_read(p, in, 50);
        u64 now = now_ms();
        if (k > 0) {
            last = now;
            got = 1;
        }
        for (int i = 0; i < k; i++)
            done |= line_char(&s, (char)in[i]);
        if (!done && s.n == 2 && !s.cont && s.l[0] == '>' && s.l[1] == ' ') {
            uprintf("> \n");                    /* AT+CMGS waits for the text (Ctrl-Z ends it: 'cu') */
            return;
        }
        if (k == -1) {                          /* (a token spoilt on the bus was tried again in usb.c) */
            line_end(&s);
            uprintf("%s%d: %s\n", tty(p), n, usb_error_text());
            return;
        }
        if (done && now - last >= AT_AFTER_MS)
            break;
        if (uart_getc() == 3) {
            line_end(&s);
            uprintf("^C\n");
            return;
        }
        if (now - start > 120000 || (!done && now - last > 10000)) {
            line_end(&s);
            if (!done)
                uprintf("%s\n", got ? "(no final result within 10 s)" : "(no answer within 10 s)");
            return;
        }
    }
    line_end(&s);
}
