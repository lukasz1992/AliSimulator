"""The A54's virtual USB stick for the GUIs (tv_gui.py, gui_simulator.py).

The A54's EHCI (ehci_host.py) exists on the C3503 family only; elsewhere these helpers do
nothing.  A stick is a FAT32 (default, 128 MB), FAT16 or raw-image disk, writable in memory.
"""
import os

import ehci_host
import fat_image
from usb_host import VirtualStick

FILES = {"FILM.TXT": b"AliSimulator virtual stick\n", "MUZYKA/SONG.MP3": b"\0" * 5000}
KINDS = ("FAT32", "Disk image file")


def make_stick(kind="FAT32", image_path=None):
    if kind == "Disk image file" and image_path and os.path.isfile(image_path):
        with open(image_path, "rb") as fh:
            return VirtualStick(bytearray(fh.read()), writable=True)
    return VirtualStick(fat_image.build(FILES, fat=32, size_mb=128, label="ALISIM32"), writable=True)


def has_ehci(sim):
    return type(sim.chip).__name__ in ["C3503", "C3505"]


def attach(sim, plugged=True, kind="FAT32", image_path=None):
    """Attach the EHCI (C350* only) and plug a stick in.  Returns the stick or None."""
    if not has_ehci(sim):
        return None
    ehci = ehci_host.attach(sim)
    if not plugged:
        return None
    stick = make_stick(kind, image_path)
    ehci.attach(stick)
    return stick


def replug(sim, plugged, kind="FAT32", image_path=None):
    """Unplug the stick, plug a new one when `plugged`.  Returns (stick, note)."""
    ehci = getattr(sim, "ehci", None)
    if ehci is None:
        return None, "no A54 EHCI on this chip"
    if ehci.connected:
        ehci.detach()
    if not plugged:
        return None, "unplugged"
    stick = make_stick(kind, image_path)
    ehci.attach(stick)
    return stick, f"{kind} plugged"


class UsbRow:
    """The 'USB stick' row of gui_simulator.py: file system, image file, Plugged.  The choices take
    effect when the emulator is (re)loaded: unplugging a disk under a running firmware is not
    supported yet (its USB stack crashes on the disconnect), so replug() is not wired to the controls."""

    def __init__(self, parent, get_sim, log):
        import tkinter as tk
        from tkinter import ttk
        self._get_sim, self._log, self.image_path, self.stick = get_sim, log, None, None
        row = tk.Frame(parent)
        row.pack(fill=tk.X, pady=(4, 0))
        tk.Label(row, text="USB stick:").pack(side=tk.LEFT)
        self.kind = tk.StringVar(value=KINDS[0])
        box = ttk.Combobox(row, textvariable=self.kind, width=14, state="readonly", values=list(KINDS))
        box.pack(side=tk.LEFT, padx=(4, 8))
        tk.Button(row, text="Image...", command=self.browse, width=8).pack(side=tk.LEFT, padx=(0, 8))
        self.plugged = tk.BooleanVar(value=not os.environ.get("ALISIM_NO_USB_STICK"))
        tk.Checkbutton(row, text="Plugged (at reset)", variable=self.plugged).pack(side=tk.LEFT, padx=4)
        self.status = tk.StringVar(value="")
        tk.Label(row, textvariable=self.status, fg="#555555").pack(side=tk.LEFT, padx=8)

    def attach(self, sim):
        """After the emulator is (re)loaded: attach the EHCI and plug per the controls."""
        self.stick = attach(sim, self.plugged.get(), self.kind.get(), self.image_path)
        self.status.set(f"{self.kind.get()} plugged" if self.stick else "no stick")

    def replug(self):
        try:
            self.stick, note = replug(self._get_sim(), self.plugged.get(), self.kind.get(), self.image_path)
            self.status.set(note)
            self._log("USB: " + note)
        except Exception as exc:                      # a GUI control must not take the emulator down
            self.status.set(f"error: {exc}")

    def browse(self):
        from tkinter import filedialog
        path = filedialog.askopenfilename(title="Raw disk image for the virtual USB stick")
        if path:
            self.image_path = path
            self.kind.set("Disk image file")
            self.plugged.set(True)
            self.status.set("applies after Reset")
