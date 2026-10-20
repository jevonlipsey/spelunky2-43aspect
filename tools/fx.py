#!/usr/bin/env python3
"""spel2 static helpers (fast).

section mapping: .text = rva-0xc00, .rdata = rva-0x1a00 (data section differs!).
.pdata gives function begin/end so an address resolves to its enclosing function.
"""
import struct
import capstone
import numpy as np

TEXT_VA, TEXT_RAW, TEXT_SIZE = 0x1000, 0x400, 0x22d08200
RDATA_VA, RDATA_RAW, RDATA_SIZE = 0x22d0a000, 0x22d08600, 0xe0400
DATA_VA, DATA_RAW, DATA_SIZE = 0x22deb000, 0x22de8a00, 0x3600
PDATA_VA, PDATA_RAW, PDATA_SIZE = 0x22e1f000, 0x22dec000, 0x1cc00
SECTIONS = ((TEXT_VA, TEXT_RAW, TEXT_SIZE), (RDATA_VA, RDATA_RAW, RDATA_SIZE),
            (DATA_VA, DATA_RAW, DATA_SIZE), (PDATA_VA, PDATA_RAW, PDATA_SIZE))

with open('Spel2.exe', 'rb') as f:
    IMG = f.read()

_TLEN = TEXT_SIZE - TEXT_VA
TEXT = IMG[TEXT_RAW:TEXT_RAW + _TLEN]
_B = np.frombuffer(TEXT, dtype=np.uint8)

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True


def rva2off(rva):
    for va, raw, size in SECTIONS:
        if va <= rva < va + size:
            return raw + (rva - va)
    return None


def read_bytes(rva, n):
    o = rva2off(rva)
    return IMG[o:o + n] if o is not None else b''


def read_float(rva):
    b = read_bytes(rva, 4)
    return struct.unpack('<f', b)[0] if len(b) == 4 else None


def read_u32(rva):
    b = read_bytes(rva, 4)
    return struct.unpack('<I', b)[0] if len(b) == 4 else None


def dis(rva, length, floats=True, limit=500):
    code = read_bytes(rva, length)
    n = 0
    for i in md.disasm(code, rva):
        line = f'0x{i.address:x}: {i.bytes.hex():<22} {i.mnemonic} {i.op_str}'
        if floats:
            for op in i.operands:
                if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                    t = i.address + i.size + op.mem.disp
                    line += f'    ; [0x{t:x}]={read_float(t) if rva2off(t) else "?"}'
        print(line)
        n += 1
        if n >= limit or i.mnemonic == 'ret':
            break


# ---- int32 window view over .text (disp32 of a rip-relative insn is its last 4 bytes) ----
_I32 = np.ndarray(shape=(len(_B) - 3,), dtype='<i4', buffer=_B, strides=(1,))
_AR = np.arange(len(_I32), dtype=np.int32)

# ---- .pdata ----
_pd = IMG[PDATA_RAW:PDATA_RAW + PDATA_SIZE]
FUNCS = []
for i in range(0, len(_pd) - 11, 12):
    b, e, _u = struct.unpack_from('<III', _pd, i)
    if e:
        FUNCS.append((b, e))
FUNCS.sort()
_FB = np.fromiter((x[0] for x in FUNCS), dtype=np.int64, count=len(FUNCS))


def func_for(rva, _cache={}):
    if rva in _cache:
        return _cache[rva]
    i = int(np.searchsorted(_FB, rva, side='right') - 1)
    r = FUNCS[i] if 0 <= i < len(FUNCS) and FUNCS[i][0] <= rva < FUNCS[i][1] else None
    _cache[rva] = r
    return r


def rip_refs(target, max_back=15):
    """every site whose rip-relative effective address is `target` (longest decode wins)"""
    want = np.int32((target - TEXT_VA - 4) & 0xffffffff)
    with np.errstate(over='ignore'):
        hits = np.nonzero(_I32 == want - _AR)[0]
    best = {}
    for h in hits[:4000]:
        h = int(h)
        base = h + 4
        for back in range(1, max_back + 1):
            start = base - back
            if start < 0:
                break
            for i in md.disasm(TEXT[start:base], TEXT_VA + start):
                if i.address + i.size != TEXT_VA + base:
                    break
                for op in i.operands:
                    if (op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP
                            and i.address + i.size + op.mem.disp == target):
                        cur = best.get(base)
                        if cur is None or i.size > cur[0]:
                            best[base] = (i.size, TEXT_VA + start, f'{i.mnemonic} {i.op_str}')
                break
    return [v for v in sorted(best.values(), key=lambda x: x[1])]


_CD = np.ndarray(shape=(len(_B) - 4,), dtype='<i4', buffer=_B, strides=(1,))


def direct_callers(target, limit=200):
    """sites of `call rel32` / `jmp rel32` landing exactly on target"""
    e8 = np.nonzero(_B[:len(_B) - 4] == 0xE8)[0]
    e9 = np.nonzero(_B[:len(_B) - 4] == 0xE9)[0]
    out = []
    for opcode, arr in ((0xE8, e8), (0xE9, e9)):
        tgt = TEXT_VA + arr.astype(np.int64) + 5 + _CD[arr + 1].astype(np.int64)
        out += [(TEXT_VA + int(a), opcode) for a in arr[tgt == target]]
    return sorted(out)[:limit]


if __name__ == '__main__':
    import sys
    for a in sys.argv[1:]:
        t = int(a, 16)
        f = func_for(t)
        r = rip_refs(t)
        print(f'=== 0x{t:x} float={read_float(t)} func=0x{f[0]:x}-0x{f[1]:x} : {len(r)} refs ===' if f else f'=== 0x{t:x} : {len(r)} refs ===')
        for size, addr, txt in r:
            print(f'   0x{addr:x}  {txt}')
