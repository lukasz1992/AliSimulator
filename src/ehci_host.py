"""EHCI host controller model of the ALi C3503 / C3505 / C3507, written for the Ferguson Ariva 54
(C3503D); the Ariva 104 and Amiko HD8155 (C3505) and GTMedia V9 Prime (C3507) firmwares drive it
the same way.

Layout (confirmed from A54's own driver, FUN_805cdc58 and the ehci probe):

  0xB803A000..0xB803A0FF   ALi wrapper registers (+0x04 / +0x0C / +0x10 = 0x1803A100 /
                           +0x40 / +0x44 are written by the glue; plain storage here)
  0xB803A100               EHCI capability registers (CAPLENGTH 0x10, HCIVERSION 1.00).  The
                           glue writes HCSPARAMS (0x13) and HCCPARAMS (0: no 64-bit addressing)
                           before it starts the controller, and those stores land
  0xB803A110               EHCI operational registers, PORTSC at 0xB803A154
  0xB803A800, 0xB803B000   OHCI companion for low / full speed: the glue word and register stubs
                           only (revision, root hub descriptor, HcCommandStatus.HCR self-clearing)

Its interrupt-controller line is the bit the driver enables as it starts the controller: 36 on
the C3503 / C3505, 33 on the C3507 (chip ID 0x3507, whose lines are laid out differently).

The controller walks the guest's asynchronous and periodic schedules (QH / qTD
in RAM) between emulation slices and executes the transfers against the
virtual USB devices of usb_host.py (VirtualStick, VirtualHub, ...).
"""
import ctypes

from usb_host import HALTED, VirtualHub, VirtualStick

CAP = 0x100                   # offset of the capability block inside the window
CAPLENGTH = 0x10
OP = CAP + CAPLENGTH          # operational registers

CMD_RS, CMD_HCRESET, CMD_PSE, CMD_ASE, CMD_IAAD = 0x01, 0x02, 0x10, 0x20, 0x40
STS_INT, STS_ERR, STS_PCD, STS_IAA = 0x01, 0x02, 0x04, 0x20
STS_HALTED, STS_PSS, STS_ASS = 0x1000, 0x4000, 0x8000

PS_CCS, PS_CSC, PS_PED, PS_PEDC, PS_PR, PS_PP = 0x01, 0x02, 0x04, 0x08, 0x100, 0x1000

QTD_ACTIVE, QTD_HALT, QTD_DBE, QTD_XACT = 0x80, 0x40, 0x20, 0x08
PID_OUT, PID_IN, PID_SETUP = 0, 1, 2

C3507_CHIP_ID = 0x3507


def default_line(sim):
    """The controller's interrupt-controller line on `sim`'s chip: 36, or 33 on the C3507."""
    return 33 if getattr(getattr(sim, "chip", None), "chip_id", None) == C3507_CHIP_ID else 36


class EhciHost:
    def __init__(self, sim, base=0x3A000, line=None):
        self.sim = sim
        self.base = base & 0xFFFFFF
        self.line = default_line(sim) if line is None else line
        self.device = None
        self.connected = False
        self.usbcmd = 0
        self.usbsts = STS_HALTED
        self.usbintr = 0
        self.frindex = 0
        self.dsseg = 0
        self.periodic_base = 0
        self.async_base = 0
        self.config_flag = 0
        self.portsc = PS_PP
        self.other_ports = [PS_PP, PS_PP]                         # ports 2 and 3: nothing plugged in
        self.frame_t0 = None
        self.last_frame = -1
        self.ctrl = {}                         # (devaddr) -> control transfer state
        self.log = []
        self.transfers = 0
        view = sim._mmio_view
        b = self.base
        view[b + 0x00:b + 0x04] = (0x01000010).to_bytes(4, "little")      # the glue's presence word
        view[b + CAP:b + CAP + 4] = ((0x0100 << 16) | CAPLENGTH).to_bytes(4, "little")
        view[b + CAP + 4:b + CAP + 8] = (0x13).to_bytes(4, "little")      # HCSPARAMS: 3 ports, PPC (u-boot ehci-ali.c)
        view[b + CAP + 8:b + CAP + 12] = (1).to_bytes(4, "little")        # HCCPARAMS: 64-bit; the glue writes 0
        view[b + 0x800:b + 0x804] = (0x01000010).to_bytes(4, "little")      # OHCI wrapper gate
        view[b + 0x1000:b + 0x1004] = (0x10).to_bytes(4, "little")         # OHCI HcRevision (0x1803B000)
        view[b + 0x1048:b + 0x104C] = (0x102).to_bytes(4, "little")        # HcRhDescriptorA: 3 ports, NOCP
        sim._mmio_on('w', self._ohci_write, self.base + 0x1008, self.base + 0x100B)
        sim._mmio_on('r', self._read, self.base + OP, self.base + OP + 0x4F)
        sim._mmio_on('w', self._write, self.base + OP, self.base + OP + 0x4F)
        sim.slice_services.append(self._service)

    def _ohci_write(self, uc, access, address, size, value, user_data):
        """HcCommandStatus: HCR (bit 0, host controller reset) self-clears, so the reset is done at
        once.  What a write handler returns is what lands in the register (a change made to the view
        here would be overwritten by the store)."""
        if (address & 0xFFFFFF) == self.base + 0x1008:
            return value & ~1
        return None

    # ---- the device ----
    def attach(self, device=None, speed="high"):
        self.device = device if device is not None else VirtualStick()
        self.device.clock = self.sim._vtime
        self.connected = True
        self.portsc |= PS_CCS | PS_CSC
        self.usbsts |= STS_PCD
        self._event("device attached")
        self._update_line()

    def detach(self):
        self.connected = False
        self.portsc = (self.portsc & ~(PS_CCS | PS_PED)) | PS_CSC
        self.usbsts |= STS_PCD
        self._event("device removed")
        self._update_line()

    def _event(self, text):
        if len(self.log) < 1000:
            self.log.append((round(self.sim._vtime(), 4), text))

    def _devices(self, device=None):
        if device is None:
            device = self.device if (self.connected and self.portsc & PS_PED) else None
        if device is None:
            return
        yield device
        if isinstance(device, VirtualHub):
            for p in device.ports.values():
                if p.device is not None:
                    yield from self._devices(p.device)

    def _find(self, address):
        for dev in self._devices():
            if dev.address == address:
                return dev
        return None

    # ---- registers ----
    def _update_line(self):
        self.sim._ic_set_line(self.line, bool(self.usbsts & self.usbintr & 0x3F))

    def _put(self, abs_off, size, value):
        self.sim._mmio_view[abs_off:abs_off + size] = (value & ((1 << (8 * size)) - 1)).to_bytes(size, "little")

    def _reg_value(self, off):
        if off == 0x00:
            return self.usbcmd
        if off == 0x04:
            v = self.usbsts & ~(STS_HALTED | STS_PSS | STS_ASS)
            if not self.usbcmd & CMD_RS:
                v |= STS_HALTED
            else:
                if self.usbcmd & CMD_PSE:
                    v |= STS_PSS
                if self.usbcmd & CMD_ASE:
                    v |= STS_ASS
            return v
        if off == 0x08:
            return self.usbintr
        if off == 0x0C:
            return self.frindex
        if off == 0x10:
            return self.dsseg
        if off == 0x14:
            return self.periodic_base
        if off == 0x18:
            return self.async_base
        if off == 0x40:
            return self.config_flag
        if off == 0x44:
            return self.portsc
        if off in (0x48, 0x4C):
            return self.other_ports[(off - 0x48) // 4]
        return 0

    def _read(self, uc, access, address, size, value, user_data):
        self.sim._device_access(uc)
        abs_off = address & 0xFFFFFF
        off = abs_off - self.base - OP
        word = off & ~3
        self._service()
        v = self._reg_value(word) >> (8 * (off & 3))
        self._put(abs_off, size, v)

    def _write(self, uc, access, address, size, value, user_data):
        self.sim._device_access(uc)
        abs_off = address & 0xFFFFFF
        off = abs_off - self.base - OP
        word = off & ~3
        shift = 8 * (off & 3)
        mask = ((1 << (8 * size)) - 1) << shift
        value = (value << shift) & mask
        cur = self._reg_value(word)
        new = (cur & ~mask) | value
        if word == 0x00:
            self._write_cmd(new)
        elif word == 0x04:
            self.usbsts &= ~(value & 0x3F)                    # write 1 to clear
        elif word == 0x08:
            self.usbintr = new & 0x3F
        elif word == 0x0C:
            self.frindex = new & 0x3FFF
        elif word == 0x10:
            self.dsseg = new
        elif word == 0x14:
            self.periodic_base = new & ~0xFFF
        elif word == 0x18:
            self.async_base = new & ~0x1F
        elif word == 0x40:
            self.config_flag = new & 1
        elif word == 0x44:
            self._write_portsc(value, mask)
        elif word in (0x48, 0x4C):
            self.other_ports[(word - 0x48) // 4] = (new & PS_PP) | (self.other_ports[(word - 0x48) // 4] & ~PS_PP)
        self._update_line()
        self._put(abs_off, size, new >> shift)

    def _write_cmd(self, new):
        if new & CMD_HCRESET:
            self.usbcmd = 0
            self.usbsts = STS_HALTED
            self.usbintr = 0
            self.frindex = 0
            self.periodic_base = self.async_base = 0
            self.config_flag = 0
            self.portsc = PS_PP | (self.portsc & PS_CCS)
            self.ctrl.clear()
            self._event("host controller reset")
            return
        was = self.usbcmd
        self.usbcmd = new & ~CMD_IAAD
        if new & CMD_RS and not was & CMD_RS:
            self.frame_t0 = self.sim._vtime()
            self._event("run")
        if new & CMD_IAAD:
            self.usbsts |= STS_IAA                              # the async advance doorbell: answered at once
            self._event("async advance doorbell")

    def _write_portsc(self, value, mask):
        """u-boot's rules: PE is never cleared by software (it writes back a stale PE=0 after a
        reset); a write of PR=0 during a reset ends it at once; W1C change bits; PP writable."""
        ps = self.portsc
        ps &= ~(value & (PS_CSC | PS_PEDC))
        if mask & 0xFF00:
            if value & PS_PR:
                if not ps & PS_PR:
                    ps |= PS_PR
                    ps &= ~PS_PED
                    if self.connected and self.device is not None:
                        self.device.address, self.device.pending_address = 0, None
                        self.device.configuration = 0
                        self.device.in_toggle.clear()
                        self.device.out_toggle.clear()
                    self.ctrl.clear()
            elif ps & PS_PR:
                ps &= ~PS_PR
                if self.connected:
                    ps |= PS_PED                    # (PEC is set only when a port is disabled by an error, not on enable)
                self._event("port reset done, enabled")
            ps = (ps & ~PS_PP) | (value & PS_PP)
        self.portsc = ps

    # ---- RAM access ----
    def _ram_off(self, address):
        return address & 0x1FFFFFFF

    def _rd(self, address, n):
        off = self._ram_off(address)
        if off + n > self.sim.ram_size:
            return bytes(n)
        return ctypes.string_at(ctypes.addressof(self.sim.ram_buffer) + off, n)

    def _wr(self, address, data):
        off = self._ram_off(address)
        if off + len(data) <= self.sim.ram_size:
            ctypes.memmove(ctypes.addressof(self.sim.ram_buffer) + off, bytes(data), len(data))

    def _rd32(self, address):
        return int.from_bytes(self._rd(address, 4), "little")

    def _wr32(self, address, value):
        self._wr(address, (value & 0xFFFFFFFF).to_bytes(4, "little"))

    # ---- schedules ----
    def _service(self):
        if not self.usbcmd & CMD_RS:
            return
        t = self.sim._vtime()
        if self.frame_t0 is None:
            self.frame_t0 = t
        uframes = int((t - self.frame_t0) / 125e-6)
        self.frindex = uframes & 0x3FFF
        if not (self.connected and self.portsc & PS_PED):
            return
        if self.usbcmd & CMD_ASE and self.async_base:
            self._walk_async()
        if self.usbcmd & CMD_PSE and self.periodic_base:
            frame = (self.frindex >> 3) & 0x3FF
            if frame != self.last_frame:
                self.last_frame = frame
                self._walk_periodic(frame)
        self._update_line()

    def _walk_async(self):
        start = cur = self.async_base
        for _ in range(64):
            self._qh(cur)
            nxt = self._rd32(cur)
            link = nxt & ~0x1F
            if nxt & 1 or link == start or link == 0:
                break
            cur = link

    def _walk_periodic(self, frame):
        link = self._rd32(self.periodic_base + 4 * frame)
        for _ in range(64):
            if link & 1:
                break
            kind = (link >> 1) & 3
            addr = link & ~0x1F
            if kind == 1:
                self._qh(addr)
                link = self._rd32(addr)
            else:                                               # iTD / siTD / FSTN: not modelled, skip
                link = self._rd32(addr & ~0x1F)

    def _qh(self, qh):
        info = self._rd32(qh + 4)
        devaddr, ep = info & 0x7F, (info >> 8) & 0xF
        maxp = (info >> 16) & 0x7FF
        for _ in range(16):
            token = self._rd32(qh + 0x18)
            if token & QTD_HALT:
                return
            if not token & QTD_ACTIVE:
                nxt = self._rd32(qh + 0x10)
                if nxt & 1:
                    return
                if not self._rd32((nxt & ~0x1F) + 8) & QTD_ACTIVE:
                    return                      # an inactive qTD (Linux's dummy at the list's end) is not
                                                # loaded into the overlay; it is looked at again later, when
                                                # the driver has filled it in and set Active
                self._fetch_qtd(qh, nxt & ~0x1F)
                token = self._rd32(qh + 0x18)
                if not token & QTD_ACTIVE:
                    continue
            if not self._execute(qh, devaddr, ep, maxp):
                return

    def _fetch_qtd(self, qh, qtd):
        words = [self._rd32(qtd + 4 * i) for i in range(8)]
        self._wr32(qh + 0x0C, qtd)                              # current qTD
        self._wr32(qh + 0x10, words[0])
        self._wr32(qh + 0x14, words[1])
        self._wr32(qh + 0x18, words[2])
        for i in range(5):
            self._wr32(qh + 0x1C + 4 * i, words[3 + i])

    def _span(self, qh, token):
        """Addresses for the bytes still to transfer: [(addr, n)], following the page list."""
        bufs = [self._rd32(qh + 0x1C + 4 * i) for i in range(5)]
        c_page = (token >> 12) & 7
        total = (token >> 16) & 0x7FFF
        parts = []
        page = c_page
        off = bufs[page] & 0xFFF if page < 5 else 0
        remaining = total
        while remaining > 0 and page < 5:
            n = min(remaining, 0x1000 - off)
            parts.append(((bufs[page] & ~0xFFF) + off, n))
            remaining -= n
            page += 1
            off = 0
        return parts

    def _advance(self, qh, token, moved):
        """After `moved` bytes: update the overlay's buffer pointer / C_Page / total."""
        c_page = (token >> 12) & 7
        total = (token >> 16) & 0x7FFF
        bufs = [self._rd32(qh + 0x1C + 4 * i) for i in range(5)]
        left = moved
        page = c_page
        while left > 0 and page < 5:
            off = bufs[page] & 0xFFF
            step = min(left, 0x1000 - off)
            bufs[page] += step
            left -= step
            if (bufs[page] & 0xFFF) == 0 and page < 4:
                page += 1
        total -= moved
        token = (token & ~(0x7FFF << 16) & ~(7 << 12)) | (total << 16) | (page << 12)
        for i in range(5):
            self._wr32(qh + 0x1C + 4 * i, bufs[i])
        return token

    def _complete(self, qh, token, status=0, short=False):
        """Retire the active qTD: write the token back to it, raise IOC / error, follow next / alt."""
        token = (token & ~0xFF) | status
        token &= ~QTD_ACTIVE
        cur = self._rd32(qh + 0x0C) & ~0x1F
        self._wr32(qh + 0x18, token)
        if cur:
            self._wr32(cur + 8, token)
        if status & (QTD_HALT | QTD_DBE | QTD_XACT):
            self.usbsts |= STS_ERR | STS_INT
            if (self._rd32(qh + 4) >> 8) & 0xF == 0:
                self._flush_chain(qh)
        elif token & 0x8000 or short:
            self.usbsts |= STS_INT if token & 0x8000 else 0
        if short and not status & QTD_HALT:
            alt = self._rd32(qh + 0x14)
            if not alt & 1:
                self._wr32(qh + 0x10, alt)
        self._update_line()

    def _flush_chain(self, qh):
        """A STALL on endpoint 0 ends the whole control chain (u-boot would wait on its last qTD)."""
        link = self._rd32(qh + 0x10)
        for _ in range(8):
            if link & 1:
                break
            q = link & ~0x1F
            t = self._rd32(q + 8)
            self._wr32(q + 8, (t & ~QTD_ACTIVE) | QTD_HALT)
            link = self._rd32(q)
        self._wr32(qh + 0x10, 1)

    def _execute(self, qh, devaddr, ep, maxp):
        """One transaction series of the overlay's active qTD. True if it made progress."""
        token = self._rd32(qh + 0x18)
        pid = (token >> 8) & 3
        total = (token >> 16) & 0x7FFF
        dev = self._find(devaddr)
        if dev is None:
            self._complete(qh, token, QTD_HALT | QTD_XACT)
            return True
        maxp = maxp or 64
        self.transfers += 1
        spans = self._span(qh, token)
        if pid == PID_SETUP:
            packet = self._gather(spans, 8)
            self._ctrl_setup(dev, devaddr, packet)
            token = self._advance(qh, token, 8) if total >= 8 else token
            self._complete(qh, token)
            return True
        st = self.ctrl.get(devaddr)
        if pid == PID_IN:
            if ep == 0 and st is not None:
                data = self._ctrl_in(dev, st, total, maxp)
            else:
                data = dev.in_transfer(ep, total, maxp)
            if data is HALTED:
                self._complete(qh, token, QTD_HALT)
                return True
            if data is None:
                return False                                     # NAK: the qTD stays active
            data = bytes(data)[:total]
            self._scatter(spans, data)
            token = self._advance(qh, token, len(data))
            token ^= 0x80000000
            self._complete(qh, token, 0, short=len(data) < total)
            self._after_status(dev, ep, st, in_dir=True, total=total)
            return True
        # OUT
        data = self._gather(spans, total)
        if ep == 0 and st is not None:
            ok = self._ctrl_out(dev, st, data)
        else:
            ok = dev.bulk_out(ep, data)
        if ok is HALTED:
            self._complete(qh, token, QTD_HALT)
            return True
        if ok is None:
            return False
        token = self._advance(qh, token, len(data))
        token ^= 0x80000000
        self._complete(qh, token)
        self._after_status(dev, ep, st, in_dir=False, total=total)
        return True

    def _gather(self, spans, n):
        out = bytearray()
        for addr, count in spans:
            out += self._rd(addr, count)
        return bytes(out[:n])

    def _scatter(self, spans, data):
        pos = 0
        for addr, count in spans:
            chunk = data[pos:pos + count]
            if not chunk:
                break
            self._wr(addr, chunk)
            pos += len(chunk)

    # ---- control transfers on endpoint 0 ----
    def _ctrl_setup(self, dev, devaddr, packet):
        rtype, length = packet[0], packet[6] | packet[7] << 8
        st = {"packet": packet, "in": None, "out": bytearray(), "length": length, "stall": False,
              "dir_in": bool(rtype & 0x80), "pending_addr": None}
        self.ctrl[devaddr] = st
        if length and not rtype & 0x80:
            return                                              # an OUT data stage first: the device sees it at its end
        resp = dev.setup(packet)
        if resp is None:
            st["stall"] = True
        else:
            st["in"] = bytearray(resp)

    def _ctrl_in(self, dev, st, total, maxp):
        if st["stall"]:
            return HALTED
        if st["in"] is None:
            return b""
        if st["dir_in"] is False:
            return b""                                          # status stage of an OUT control transfer
        n = min(total, len(st["in"]))
        data = bytes(st["in"][:n])
        del st["in"][:n]
        return data

    def _ctrl_out(self, dev, st, data):
        if st["stall"]:
            return HALTED
        if st["dir_in"]:
            return True                                         # status stage of an IN control transfer
        st["out"] += data
        if len(st["out"]) >= st["length"] and st["in"] is None:
            resp = dev.setup_data(st["packet"], bytes(st["out"]))
            if resp is None:
                st["stall"] = True
                return HALTED
            st["in"] = bytearray(resp)
        return True

    def _after_status(self, dev, ep, st, in_dir, total):
        """SET_ADDRESS takes effect after its status stage (the zero-length IN)."""
        if ep == 0 and st is not None and total == 0 and getattr(dev, "pending_address", None) is not None:
            dev.address, dev.pending_address = dev.pending_address, None
            self.ctrl.clear()
            self._event("device address %d" % dev.address)


def attach(sim, base=0x3A000, line=None):
    """Put the C350x EHCI on `sim` (once; also sim.ehci).  .attach(device) / .detach() plug a
    virtual USB device in and out.  Call after loadFile: the interrupt line (when not given)
    depends on the chip it found (default_line)."""
    ehci = getattr(sim, "ehci", None)
    if ehci is None or ehci._read not in sim._mmio_rd.get(ehci.base + OP, ()):   # (a family switch dropped it)
        ehci = sim.ehci = EhciHost(sim, base, line)
    return ehci
