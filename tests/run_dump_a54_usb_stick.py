#!/usr/bin/env python3
"""
The Ferguson Ariva 54 firmware V1.3B1 (ALi C3503D) with a USB stick in its EHCI port
(ehci_host.py: the C3503 / C3505 / C3507 controller at 0xB803A000, interrupt-controller line 36
here; usb_stick_ui.attach plugs in a 128 MB FAT32 VirtualStick, as the GUIs do).  About 39 s after
power-on the application's USB task starts the controller: the glue writes HCSPARAMS (0x13) and
HCCPARAMS (0), which land, it enables line 36 and resets the OHCI companion, whose reset bit
clears at once (the poll ends after a few reads instead of timing out after 31), the driver
resets the port, gives the stick address 2 and configures it, and the mass-storage driver reads
it (INQUIRY, READ CAPACITY, READ(10) of the MBR and the FAT32 boot sector).  Then the PVR code
runs its disk speed test on the stick: it creates the /ALIDVRS2 directory and TEST_W~n.DVR files
(WRITE(10)), all within 80 s of emulated time and without a reboot.
"""
import os
import sys
import time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

import usb_stick_ui                                       # noqa: E402
from simulator import AliMipsSimulator, flash_size_for   # noqa: E402

DUMP = "A54_20190926_V1.3B1.abs"
VTIME = 80          # emulated seconds: the USB task starts at ~39 s, the speed test ends at ~74 s
EHCI_LINE = 36
INQUIRY, READ_CAPACITY, READ_10, WRITE_10 = 0x12, 0x25, 0x28, 0x2A

failures = []


def check(ok, text):
    print(f"  [{'PASS' if ok else 'FAIL'}] {text}", flush=True)
    if not ok:
        failures.append(text)


print("=== Regression Test: Ferguson Ariva 54 (C3503D) uses a USB stick in its EHCI port ===")
t0 = time.time()
uart = []
sim = AliMipsSimulator(rom_size=flash_size_for(DUMP), log_handler=lambda m: None)
sim.setSPIDump(False)
sim.setI2CDump(False)
sim.setUartHandler(lambda c: uart.append(c if isinstance(c, str) else chr(c)))
sim.loadFile(DUMP)
sim.attach_tuner()
stick = usb_stick_ui.attach(sim)
check(stick is not None and getattr(sim, "ehci", None) is not None, "the EHCI and a stick are attached on the C3503")
if failures:
    sys.exit(1)
ehci = sim.ehci
B = ehci.base
ohci_reads = []


def on_ohci_command_status_read(uc, access, address, size, value, user_data):
    ohci_reads.append(sim._mmio_view[B + 0x1008])


sim._mmio_on('r', on_ohci_command_status_read, B + 0x1008, B + 0x1008)

while sim._vtime() < VTIME:
    before = sim.instruction_count
    sim.run(max_instructions=sim.instruction_count + 10 ** 11, max_vtime=min(VTIME, sim._vtime() + 10))
    if sim.instruction_count <= before:
        break
text = "".join(uart)
view = sim._mmio_view
events = [e for _t, e in ehci.log]
print(f"ran to {sim._vtime():.1f} s of emulated time in {time.time() - t0:.1f} s; EHCI events {events[:8]}")


def word(off):
    return int.from_bytes(bytes(view[off:off + 4]), "little")


en_off = sim._ic_en - 0xB8000000
check(ehci.line == EHCI_LINE, f"the model uses interrupt-controller line {EHCI_LINE} on the C3503 (got {ehci.line})")
check(word(en_off + 4 * (EHCI_LINE // 32)) >> (EHCI_LINE % 32) & 1,
      f"the firmware enabled line {EHCI_LINE} as it started the controller")
check("host controller reset" in events and "run" in events, "the driver reset the controller and set it running")
check(word(B + 0x104) == 0x13 and word(B + 0x108) == 0,
      f"the glue's stores to HCSPARAMS / HCCPARAMS landed (0x{word(B + 0x104):X} / 0x{word(B + 0x108):X})")
check(0 < len(ohci_reads) <= 10 and not ohci_reads[-1] & 1 and not view[B + 0x1008] & 1,
      f"the OHCI companion's reset bit cleared at once ({len(ohci_reads)} reads of HcCommandStatus; 31 = timed out)")
check(stick.address != 0 and stick.configuration == 1,
      f"the stick was given an address and configured (address {stick.address}, configuration {stick.configuration})")
ops = [cdb[0] for _t, cdb in stick.commands]
check(INQUIRY in ops and READ_CAPACITY in ops, "the mass-storage driver sent INQUIRY and READ CAPACITY")
part = int.from_bytes(bytes(stick.image[0x1BE + 8:0x1BE + 12]), "little")
lbas = {int.from_bytes(bytes(cdb[2:6]), "big") for _t, cdb in stick.commands if cdb[0] == READ_10}
check(0 in lbas and part in lbas,
      f"it read the MBR and the FAT32 boot sector (LBA 0 and {part}; {ops.count(READ_10)} READ(10)s)")
image = bytes(stick.image)


def attributes(name11):
    """The attribute byte of the directory entry with this 8.3 name (11 bytes), or None."""
    i = image.find(name11)
    return image[i + 11] if i >= 0 else None


dvr_dir, test_file = attributes(b"ALIDVRS2   "), attributes(b"TEST_W~1DVR")
check(WRITE_10 in ops and dvr_dir is not None and dvr_dir & 0x10 and test_file is not None,
      f"the PVR disk speed test created the /ALIDVRS2 directory and TEST_W~1.DVR on the stick "
      f"({len(stick.writes)} writes)")
check(text.count("MC: APP  init ok") == 1, "the application started once (no reboot)")
print(f"\n{time.time() - t0:.1f} s")
if failures:
    print(f"[FAIL] {len(failures)} check(s) failed")
    sys.exit(1)
print("[PASS] the Ariva 54's firmware enumerates, mounts and writes the EHCI model's USB stick")
