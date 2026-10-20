#!/usr/bin/env python3
# read the live spelunky 2 process straight out of /proc/<pid>/mem.
#
# runs on the handheld (inside the droiddeck container, where the ssh user is
# root), not on the mac. install it there with:
#   ssh droiddeck 'cat > /root/snap.py' < tools/snap.py
#
# why this exists: the mod only logs what someone thought to log, which costs a
# rebuild + relaunch every time a question changes. while the game is running we
# can just read the same addresses from outside. the guest is x86_64 under
# FEX-Emu, which maps guest vas through, so a guest address is readable here at
# the same value.
import os
import struct
import sys

# ### the 2d canvas / display singleton, from mod_43aspect.c
# [0x22e1e798] -> display object; object+0x10 -> the struct holding the 2d
# canvas size and the aspect mode byte. lazily built, so it can be null early.
RVA_DISPLAY_SLOT = 0x22e1e798
OBJ_TO_DISPLAY = 0x10
DISP_CANVAS_W = 0x8
DISP_CANVAS_H = 0xc
DISP_MODE = 0x804e9      # 2 => full rect, else centered 16:9
DISP_PRE_W = 0x81034     # pre-scale mode size
DISP_PRE_H = 0x81038
DISP_PCT = 0x8103c       # scale percent, 40..100

# ### baked 16:9 constants, sanity checks that base is right
CHECK_FLOATS = (
    ('1.7777778 (aspect main)', 0x22d0c4a0),
    ('0.5625000 (aspect inv)', 0x22d0d620),
    ('1.3579950 (stock m00)', 0x22d12410),
)


def find_pid():
    for entry in os.listdir('/proc'):
        if not entry.isdigit():
            continue
        try:
            cmd = open(f'/proc/{entry}/cmdline', 'rb').read().decode('utf-8', 'replace')
        except OSError:
            continue
        if 'Spel2.exe' in cmd:
            return int(entry)
    return None


def module_base(pid, needle='Spel2'):
    """lowest mapping of the exe itself, i.e. the base the mod prints."""
    for line in open(f'/proc/{pid}/maps'):
        parts = line.split()
        if len(parts) < 6 or parts[2] != '00000000':
            continue
        if needle.lower() in parts[-1].lower():
            return int(parts[0].split('-')[0], 16), parts[-1]
    return None, None


def rd(pid, addr, count):
    try:
        fd = os.open(f'/proc/{pid}/mem', os.O_RDONLY)
    except OSError:
        return None
    try:
        return os.preadv(fd, [(addr, count)])
    except OSError:
        return None
    finally:
        os.close(fd)


def u32(pid, addr):
    raw = rd(pid, addr, 4)
    return struct.unpack('<I', raw)[0] if raw and len(raw) == 4 else None


def u64(pid, addr):
    raw = rd(pid, addr, 8)
    return struct.unpack('<Q', raw)[0] if raw and len(raw) == 8 else None


def f32(pid, addr):
    raw = rd(pid, addr, 4)
    return struct.unpack('<f', raw)[0] if raw and len(raw) == 4 else None


pid = find_pid()
if not pid:
    print('Spel2.exe is not running')
    sys.exit(1)

base, path = module_base(pid)
if not base:
    print(f'pid={pid} but no Spel2 module mapping found')
    sys.exit(1)

# the guest va we printed is only trustworthy if it lands on the pe header
print(f'pid={pid} base=0x{base:x}')
print(f'  mapped: {path}')
print(f'  header @base: {rd(pid, base, 2)!r}   (want b"MZ")')

for label, rva in CHECK_FLOATS:
    print(f'  {label}: {f32(pid, base + rva)}')

# ### the actual question: which rect does the 2d layer draw into
obj = u64(pid, base + RVA_DISPLAY_SLOT)
print(f'  display slot @0x{base + RVA_DISPLAY_SLOT:x} = {obj and hex(obj)}')
if not obj:
    print('  -> not built yet; the game is still early in init')
    sys.exit(0)

disp = u64(pid, obj + OBJ_TO_DISPLAY)
print(f'  display struct = {disp and hex(disp)}')
if not disp:
    sys.exit(0)

w = u32(pid, disp + DISP_CANVAS_W)
h = u32(pid, disp + DISP_CANVAS_H)
mode = u32(pid, disp + DISP_MODE)
pw = u32(pid, disp + DISP_PRE_W)
ph = u32(pid, disp + DISP_PRE_H)
pct = u32(pid, disp + DISP_PCT)

print(f'  canvas = {w}x{h}  ar = {(w / h) if (w and h) else 0.0:.5f}')
print(f'  mode byte = {mode}   pre-scale = {pw}x{ph}   pct = {pct}')
print('  4:3 canvas wants element w/h = 0.75; a 16:9 canvas wants 0.5625')
