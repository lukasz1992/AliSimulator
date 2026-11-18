#!/usr/bin/env python3
"""
The GTMedia V9 Prime firmware V163 (ALi M3507 / C3507, the chip-ID-0x3507 variant of
chips/c3503.py) with a USB stick in its EHCI port: the same controller at 0xB803A000 as the
C3503 / C3505 (ehci_host.py), but on interrupt-controller line 33, not 36 -- the C3507 lays its
lines out differently (its GE is line 76, its IR receiver 12).  About 98 s after power-on the
application's USB task enables line 33 and starts the controller; the driver gives the stick
address 2 and configures it, the mass-storage driver reads it and the PVR code runs its disk
speed test (/ALIDVRS2, TEST_W~n.DVR).  With an empty channel database this firmware shows its
Factory Test Page, whose "USB:" field turns from a red "None" to a green "Insert" (captured and
attached to the report).
"""
import os
import sys
import time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src"))

import numpy as np                                        # noqa: E402

import report_artifacts                                   # noqa: E402
import usb_stick_ui                                       # noqa: E402
from simulator import AliMipsSimulator, flash_size_for   # noqa: E402

DUMP = "GTMEDIA V9 PRIME/GTMedia_V9Prime_V163_20210618.bin"
VTIME = 135         # emulated seconds: the USB task starts at ~98 s, the stick is idle again at ~127 s
EHCI_LINE = 33
USB_FIELD = (slice(515, 548), slice(300, 460))     # the Factory Test Page's "USB:" value (1280 x 720)
INQUIRY, READ_CAPACITY, READ_10, WRITE_10 = 0x12, 0x25, 0x28, 0x2A

failures = []


def check(ok, text):
    print(f"  [{'PASS' if ok else 'FAIL'}] {text}", flush=True)
    if not ok:
        failures.append(text)


print("=== Regression Test: GTMedia V9 Prime V163 (C3507) uses a USB stick in its EHCI port ===")
t0 = time.time()
sim = AliMipsSimulator(rom_size=flash_size_for(DUMP), log_handler=lambda m: None)
sim.setSPIDump(False)
sim.setI2CDump(False)
sim.setUartHandler(lambda c: None)
sim.loadFile(DUMP)
sim.attach_tuner()
stick = usb_stick_ui.attach(sim)
check(stick is not None and getattr(sim, "ehci", None) is not None and sim.chip.chip_id == 0x3507,
      "the EHCI and a stick are attached on the C3507")
if failures:
    sys.exit(1)
ehci = sim.ehci

while sim._vtime() < VTIME:
    before = sim.instruction_count
    sim.run(max_instructions=sim.instruction_count + 10 ** 11, max_vtime=min(VTIME, sim._vtime() + 10))
    if sim.instruction_count <= before:
        break
events = [e for _t, e in ehci.log]
times = [t for t, _cdb in stick.commands]
print(f"ran to {sim._vtime():.1f} s of emulated time in {time.time() - t0:.1f} s; EHCI events {events[:8]}; "
      f"SCSI commands {len(stick.commands)}" + (f" from {times[0]:.1f} to {times[-1]:.1f} s" if times else "")
      + (f"; writes {stick.writes[0][0]:.1f}-{stick.writes[-1][0]:.1f} s" if stick.writes else ""))

view = sim._mmio_view
en_off = sim._ic_en - 0xB8000000
enabled = int.from_bytes(bytes(view[en_off + 4 * (EHCI_LINE // 32):en_off + 4 * (EHCI_LINE // 32) + 4]), "little")
check(ehci.line == EHCI_LINE, f"the model uses interrupt-controller line {EHCI_LINE} on the C3507 (got {ehci.line})")
check(enabled >> (EHCI_LINE % 32) & 1, f"the firmware enabled line {EHCI_LINE} as it started the controller")
check(stick.address != 0 and stick.configuration == 1,
      f"the stick was given an address and configured (address {stick.address}, configuration {stick.configuration})")
ops = [cdb[0] for _t, cdb in stick.commands]
check(INQUIRY in ops and READ_CAPACITY in ops and READ_10 in ops,
      f"the mass-storage driver sent INQUIRY, READ CAPACITY and READ(10) ({ops.count(READ_10)} reads)")
image = bytes(stick.image)
i = image.find(b"ALIDVRS2   ")
check(WRITE_10 in ops and i >= 0 and image[i + 11] & 0x10,
      f"the PVR disk speed test created the /ALIDVRS2 directory on the stick ({len(stick.writes)} writes)")

shot = report_artifacts.path("v9_prime_v163_usb_stick.png")
rgb = sim.capture_screen(shot)
field = np.asarray(rgb)[USB_FIELD].astype(int)
green = int(np.count_nonzero((field[..., 1] > 180) & (field[..., 0] < 120) & (field[..., 2] < 120)))
red = int(np.count_nonzero((field[..., 0] > 180) & (field[..., 1] < 100) & (field[..., 2] < 100)))
report_artifacts.image(shot, f"Factory Test Page at {sim._vtime():.0f} s with the stick plugged in")
check(green > 50 and red == 0,
      f'the Factory Test Page shows the stick: "USB:" is green "Insert", not red "None" '
      f"({green} green / {red} red pixels in the field)")
print(f"\n{time.time() - t0:.1f} s")
if failures:
    print(f"[FAIL] {len(failures)} check(s) failed")
    sys.exit(1)
print("[PASS] the V9 Prime's firmware finds the EHCI model's USB stick on line 33")
