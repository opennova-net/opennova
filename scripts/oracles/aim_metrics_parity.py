#!/usr/bin/env python3
"""Regenerate the relative-position metrics witnesses from the pinned JO executable.

Executes the original Entity_ComputeRelativePositionMetrics @0x545710 (the frame
transpose, the Q22 point transform, both fild/fsqrt/fistp distances and the two
fpatan angles) under the game's nearest-even control word on synthetic frames
and points. Each frame is a quarter-turn yaw placement, whose Q22 rotation the
port's collision_matrix_from_euler builds exactly, written straight into the
matrix the function reads. No engine calls are mocked. Requires pefile and
unicorn; the native test consumes the committed vectors.
"""
import argparse
import hashlib
import itertools
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import (
    UC_X86_REG_EIP, UC_X86_REG_ESP, UC_X86_REG_FPCW, UC_X86_REG_FPSW, UC_X86_REG_FPTAG,
)

SHA256 = 'b9971c8273b7bbb1c8518a738596d669cd7794e9d307ae63a7a9a530eb802fac'
METRICS = 0x545710
P = 0x60000000
MATRIX, POINT, OUT, STACK = P + 0x1000, P + 0x2000, P + 0x3000, P + 0xF000
STOP = P
Q22 = 0x400000
U = 65536
# cos, sin per quarter turn of yaw (heading 0, 0x40000000, 0x80000000, 0xC0000000).
QUARTERS = [(Q22, 0), (0, Q22), (-Q22, 0), (0, -Q22)]


def signed(n):
    return (int(n) + 2**31) % 2**32 - 2**31


class Machine:
    def __init__(self, path):
        raw = path.read_bytes()
        assert hashlib.sha256(raw).hexdigest() == SHA256
        pe = pefile.PE(data=raw)
        self.u = Uc(UC_ARCH_X86, UC_MODE_32)
        self.u.mem_map(0x400000, pe.OPTIONAL_HEADER.SizeOfImage)
        self.u.mem_write(0x400000, pe.get_memory_mapped_image())
        self.u.mem_map(P, 0x10000)

    def wr(self, addr, *values):
        self.u.mem_write(addr, struct.pack('<%dI' % len(values), *(v & 0xffffffff for v in values)))

    def rd(self, addr):
        return struct.unpack('<i', self.u.mem_read(addr, 4))[0]

    def metrics(self, quarter, translation, point):
        c, s = QUARTERS[quarter]
        tx, ty, tz = translation
        # Rows of Rz(yaw) with the placement in the fourth column (+0x0C/+0x1C/+0x2C).
        self.wr(MATRIX, c, -s, 0, tx, s, c, 0, ty, 0, 0, Q22, tz, 0, 0, 0, U)
        self.wr(POINT, *point)
        self.wr(OUT, *([0x5A5A5A5A] * 6))
        self.wr(STACK, STOP, OUT, MATRIX, POINT)
        self.u.reg_write(UC_X86_REG_ESP, STACK)
        self.u.reg_write(UC_X86_REG_FPCW, 0x027F)
        self.u.reg_write(UC_X86_REG_FPSW, 0)
        self.u.reg_write(UC_X86_REG_FPTAG, 0xFFFF)
        self.u.emu_start(METRICS, STOP, count=100000)
        assert self.u.reg_read(UC_X86_REG_EIP) == STOP
        # The function writes the horizontal distance, the distance, the yaw
        # and the pitch; words 1 and 5 keep the sentinel.
        out = [self.rd(OUT + 4 * n) for n in range(6)]
        assert out[1] == out[5] == 0x5A5A5A5A
        return [out[0], out[2], out[3], out[4]]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--retail-exe', type=Path, required=True)
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    machine = Machine(args.retail_exe)
    root = Path(__file__).resolve().parents[2] / 'tests/world/fixtures'

    # Frame-local offsets in 1/16 u: sqrt fractions below, above and near one
    # half, sub-unit and vertical cases, the axes, and a squared sum that
    # wraps the 32-bit accumulator.
    locals16 = [
        (32, 48, 0), (48, 32, 16), (16, 32, 0), (160, 80, 0), (112, 112, 112),
        (1600, 16, 0), (11, 3, 2), (24, 0, 0), (0, 0, 80), (-48, -64, 0),
        (-40, 72, -24), (8, -8, 200), (400, 0, -400), (-8000, 4000, 1000),
        (512000, 512000, 512000),
    ]
    translations = [(0, 0, 0), (10 * U, -20 * U, 3 * U)]
    rows = []
    for quarter, translation, local in itertools.product(range(4), translations, locals16):
        c, s = QUARTERS[quarter]
        lx, ly, lz = (v * U // 16 for v in local)
        # The world point whose frame-local offset is `local`: R * local + t.
        px = translation[0] + (c * lx - s * ly) // Q22
        py = translation[1] + (s * lx + c * ly) // Q22
        pz = translation[2] + lz
        point = (px, py, pz)
        rows.append([quarter, *translation, *point] + machine.metrics(quarter, translation, point))

    text = '// Original JO x86 @0x545710; regenerate with scripts/oracles/aim_metrics_parity.py.\n'
    text += ''.join('{' + ','.join(str(signed(x)) for x in row) + '},\n' for row in rows)
    path = root / 'aim_metrics_vectors.inc'
    if args.write:
        path.write_text(text, encoding='utf-8')
    else:
        assert path.read_text(encoding='utf-8') == text, str(path) + ' differs'
    print(path.name, len(rows), 'original-instruction cases passed')


if __name__ == '__main__':
    main()
