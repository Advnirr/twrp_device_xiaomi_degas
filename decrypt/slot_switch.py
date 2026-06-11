#!/usr/bin/env python3
# slot_switch.py — direct-write A/B slot switcher for MTK degas (Xiaomi 14T).
#
# WHY: TWRP's "Change Slot" hangs forever because this build only ships
# libboot_control_client.so, which calls the android.hardware.boot HAL over
# binder — and in standalone recovery there is no servicemanager / boot HAL
# running, so the binder call blocks and the progress bar freezes.
#
# The bootloader actually selects the slot from a standard `bootloader_control`
# struct stored in the `misc` partition at byte offset 2048 (magic "BCAB" =
# 0x42414342, 32 bytes, crc32_le over the first 28 bytes). This tool does the
# read-modify-write directly, exactly like a proper bootctrl backend — no binder.
#
# Usage:
#   slot_switch.py status            # show current slot metadata (read-only)
#   slot_switch.py set a|b           # make slot A or B active for next boot
#   slot_switch.py set a|b --dry-run # build+validate the new struct, write nothing
#
# Runs on the host and drives the device over `adb` (the wrapper in this dir).
import sys, os, subprocess, struct, zlib, tempfile

ADB = os.path.join(os.path.dirname(os.path.abspath(__file__)), "adb")
MISC = "/dev/block/by-name/misc"
OFF = 2048
SIZE = 32
MAGIC = 0x42414342  # "BCAB"

def sh(*args, binary=False):
    return subprocess.run([ADB, *args], capture_output=True,
                          text=not binary).stdout

def read_struct():
    sh("shell", f"dd if={MISC} bs=1 skip={OFF} count={SIZE} of=/tmp/bc.bin 2>/dev/null")
    tmp = tempfile.mktemp()
    sh("pull", "/tmp/bc.bin", tmp)
    data = open(tmp, "rb").read()
    os.unlink(tmp)
    if len(data) != SIZE:
        sys.exit(f"FATAL: read {len(data)} bytes, expected {SIZE}")
    return bytearray(data)

def parse(d):
    magic = struct.unpack("<I", d[4:8])[0]
    if magic != MAGIC:
        sys.exit(f"FATAL: bad magic 0x{magic:08x}, not a bootloader_control struct")
    stored = struct.unpack("<I", d[28:32])[0]
    calc = zlib.crc32(bytes(d[:28])) & 0xffffffff
    if stored != calc:
        sys.exit(f"FATAL: existing crc mismatch stored=0x{stored:08x} calc=0x{calc:08x}")
    def slot(b): return dict(prio=b & 0xf, tries=(b >> 4) & 7, ok=(b >> 7) & 1)
    return dict(suffix=bytes(d[0:4]).rstrip(b"\0").decode(),
                ver=d[8], slotA=slot(d[12]), slotB=slot(d[14]))

def slot_byte(prio, tries, ok):
    return (prio & 0xf) | ((tries & 7) << 4) | ((ok & 1) << 7)

def build(d, target):  # target: 0=A, 1=B
    nd = bytearray(d)
    suffix = b"_a\0\0" if target == 0 else b"_b\0\0"
    nd[0:4] = suffix
    # target slot: highest priority, full retries, not-yet-successful
    # other slot:  demote to 14 so the target always wins, but keep bootable
    a = slot_byte(15, 7, 0) if target == 0 else slot_byte(14, 7, 0)
    b = slot_byte(15, 7, 0) if target == 1 else slot_byte(14, 7, 0)
    nd[12] = a
    nd[14] = b
    crc = zlib.crc32(bytes(nd[:28])) & 0xffffffff
    nd[28:32] = struct.pack("<I", crc)
    return nd

def write_struct(nd):
    tmp = tempfile.mktemp()
    open(tmp, "wb").write(bytes(nd))
    sh("push", tmp, "/tmp/bc.new")
    os.unlink(tmp)
    # write the 32-byte struct back at offset 2048 (seek, do not truncate)
    sh("shell", f"dd if=/tmp/bc.new of={MISC} bs=1 seek={OFF} count={SIZE} conv=notrunc 2>/dev/null")

def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    d = read_struct()
    info = parse(d)
    print(f"current: suffix={info['suffix']} ver={info['ver']} "
          f"A={info['slotA']} B={info['slotB']}")
    if cmd == "status":
        return
    if cmd == "set":
        tgt = sys.argv[2].lower()
        target = 0 if tgt == "a" else 1 if tgt == "b" else sys.exit("target must be a|b")
        nd = build(d, target)
        # validate the freshly built struct round-trips
        chk = parse(nd)
        print(f"new    : suffix={chk['suffix']} ver={chk['ver']} "
              f"A={chk['slotA']} B={chk['slotB']}  crc=OK")
        if "--dry-run" in sys.argv:
            print("dry-run: nothing written")
            return
        write_struct(nd)
        rb = parse(read_struct())
        ok = rb["suffix"] == ("_a" if target == 0 else "_b")
        print(f"written; readback suffix={rb['suffix']} {'OK' if ok else 'FAILED'}")

if __name__ == "__main__":
    main()
