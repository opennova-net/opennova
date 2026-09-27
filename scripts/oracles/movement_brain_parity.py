#!/usr/bin/env python3
"""Regenerate bounded movement/brain witnesses from the pinned JO executable.

The steering slice executes the original ground/bike motor input and brake branches;
it stops before steering physics/contact. The brain slice executes the original
dispatchers with their original no-op/reset callbacks, including the no-target idle
latch over the ammo dwords, the profile's weapon ammo bytes and the gunner guard,
and the alert edge's occupant/Player gate. No engine calls are mocked.
Requires pefile and unicorn; normal native tests use the committed vectors.
"""
import argparse
import hashlib
import itertools
import struct
from pathlib import Path

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import (
    UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_EBX, UC_X86_REG_ECX,
    UC_X86_REG_EDI, UC_X86_REG_EDX, UC_X86_REG_EFLAGS, UC_X86_REG_EIP,
    UC_X86_REG_ESI, UC_X86_REG_ESP,
)

SHA256 = 'b9971c8273b7bbb1c8518a738596d669cd7794e9d307ae63a7a9a530eb802fac'
P = 0x60000000
VEH, DRIVER, BRAIN, DEF, SLOT, STACK = [P + n * 0x2000 for n in range(1, 7)]
STOP = P + 0x10000


def signed(n):
    return (int(n) + 2**31) % 2**32 - 2**31


def c_int(n):
    """A signed dword as C++ source. INT32_MIN is (-2147483647 - 1):
    2147483648 fits no int, so `-2147483648` negates a wider constant (MSVC:
    unsigned long) that narrows into the int32_t tables."""
    return '(-2147483647 - 1)' if n == -2**31 else str(n)


class Machine:
    def __init__(self, path):
        raw = path.read_bytes()
        assert hashlib.sha256(raw).hexdigest() == SHA256
        pe = pefile.PE(data=raw)
        self.u = Uc(UC_ARCH_X86, UC_MODE_32)
        self.u.mem_map(0x400000, pe.OPTIONAL_HEADER.SizeOfImage)
        self.u.mem_write(0x400000, pe.get_memory_mapped_image())
        self.u.mem_map(P, 0x20000)

    def wr(self, addr, *values):
        self.u.mem_write(addr, struct.pack('<%dI' % len(values), *(v & 0xffffffff for v in values)))

    def rd(self, addr):
        return struct.unpack('<i', self.u.mem_read(addr, 4))[0]

    def reset(self):
        self.u.mem_write(P, bytes(0x20000))
        for reg in (UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_EBX,
                    UC_X86_REG_ECX, UC_X86_REG_EDI, UC_X86_REG_EDX, UC_X86_REG_ESI):
            self.u.reg_write(reg, 0)
        self.u.reg_write(UC_X86_REG_EFLAGS, 2)
        self.u.reg_write(UC_X86_REG_ESP, STACK)
        self.wr(0xB75FC8, 0)  # authority serving a remote driver
        self.wr(0xB5CC28, 1)  # g_NapiNPCtx.is_authority

    def run(self, start, stop):
        self.u.emu_start(start, stop, count=10000)
        assert self.u.reg_read(UC_X86_REG_EIP) == stop

    def steering(self, v):
        family, order, ax, ay, az, look, yaw, steer, ramp, brake, brake_dir = v
        self.reset()
        self.wr(VEH + 0x10, yaw)
        self.wr(VEH + 0x20, DEF)
        self.wr(VEH + 0x170, DRIVER)
        self.wr(VEH + 0x3C8, brake_dir)
        self.wr(VEH + 0x2F4, 1)  # bike has both wheels grounded
        self.u.mem_write(VEH + 0x3CD, bytes([brake]))
        self.wr(DRIVER + 0x10, look)
        self.wr(DRIVER + 0x12C, order)
        self.u.mem_write(DRIVER + 0x130, bytes(x & 255 for x in (ax, ay, az)))
        self.wr(DEF + 0x8E8, 27542)
        self.wr(DEF + 0x944, 1)
        self.wr(BRAIN + 0x210, steer)
        self.wr(BRAIN + 0x224, ramp)
        self.wr(STACK + 0x60, BRAIN)  # original local moveMode at ESP+0x60
        self.u.reg_write(UC_X86_REG_ESI, VEH)
        self.u.reg_write(UC_X86_REG_ECX, DRIVER)
        # MoveOrder's freelook merge precedes the claimant/sound services.
        if family == 0:
            self.run(0x48B847, 0x48B89E)
            self.u.reg_write(UC_X86_REG_EBP, 0)
            self.run(0x48B9B2, 0x48C095)
        else:
            self.run(0x48496D, 0x4849C4)
            self.u.reg_write(UC_X86_REG_EDI, 0)
            self.run(0x484AD8, 0x48526F)
        return [self.rd(BRAIN + n) for n in (0x220, 0x210, 0x224)] + [
            self.rd(DRIVER + 0x10), self.rd(DRIVER + 0x12C), self.rd(VEH + 0x24),
            self.u.mem_read(VEH + 0x3CD, 1)[0], self.rd(VEH + 0x3C8),
        ]

    def brain(self, v):
        (air, authority, event, current, pending, ammo_a, ammo_b, weap_a, weap_b, guard,
         occupant, flags96) = v
        self.reset()
        self.wr(0xB5CC28, authority)
        self.wr(VEH + 100, BRAIN, SLOT)
        self.wr(VEH + 684, 37)
        self.wr(BRAIN, VEH, DEF)
        self.wr(BRAIN + 16, current, pending, 0, 31)
        self.wr(BRAIN + 184, 3, 5)
        # The no-target idle latch inputs: the two ammo dwords, the profile's
        # resolved weapon ammo bytes, and the gunner-attachment guard.
        self.wr(BRAIN + 0xD4, ammo_a, ammo_b)
        self.u.mem_write(DEF + 0x94, bytes([weap_a]))
        self.u.mem_write(DEF + 0xB4, bytes([weap_b]))
        self.wr(BRAIN + 0x240, guard)
        # The alert-edge inputs: the entity+0x170 occupant (1 an NPC, 2 a Player
        # with Flags 0x100) and the profile's +0x60 flags (bit 2 holds the pend).
        if occupant:
            self.wr(VEH + 0x170, DRIVER)
            self.wr(DRIVER + 0x24, 0x100 if occupant == 2 else 0)
        self.wr(DEF + 0x60, flags96)
        self.wr(STACK, STOP, VEH, event)
        self.run(0x4581B0 if air else 0x4583C0, STOP)
        assert self.u.reg_read(UC_X86_REG_ESP) == STACK + 4
        return [self.rd(BRAIN + n) for n in (16, 20, 28, 40, 184, 188)] + [
            self.rd(VEH + 684), self.rd(BRAIN + 0xC0)]


    def recovery(self, v):
        phase, step, flags = v
        self.reset()
        self.wr(VEH + 16, 123)
        self.wr(VEH + 100, BRAIN)
        self.wr(BRAIN, VEH, DEF, SLOT)
        self.wr(BRAIN + 24, 17, step)
        self.wr(BRAIN + 49 * 4, 7, 0, 1000)
        self.wr(DEF + 100, flags)
        self.wr(DEF + 216, 2000, 3000)
        self.wr(SLOT + 16, phase)
        self.wr(STACK, STOP, VEH)
        self.run(0x457B40, STOP)
        return [self.u.reg_read(UC_X86_REG_EAX), self.rd(SLOT + 16), self.rd(BRAIN + 20)] + [
            self.rd(BRAIN + n * 4) for n in (128, 131, 132, 133, 134, 138)
        ]

    def class_walk(self, keys):
        # The profile loader's four {class index, priority key} pairs through the
        # CRT qsort with CompareFunction, read back in the loader's reversed store
        # order (+0x28 = the last pair ... +0x34 = the first).
        self.reset()
        for i, key in enumerate(keys):
            self.wr(BRAIN + 8 * i, i, key)
        self.wr(STACK, STOP, BRAIN, 4, 8, 0x455D90)
        self.run(0x76D6A0, STOP)
        return [self.rd(BRAIN + 8 * i) for i in (3, 2, 1, 0)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--retail-exe', type=Path, required=True)
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    machine = Machine(args.retail_exe)
    root = Path(__file__).resolve().parents[2] / 'tests/world/fixtures'

    def fixture(name, rows):
        text = '// Original JO x86; regenerate with scripts/oracles/movement_brain_parity.py.\n'
        text += ''.join('{' + ','.join(c_int(signed(x)) for x in row) + '},\n' for row in rows)
        path = root / name
        if args.write:
            path.write_text(text, encoding='utf-8')
        else:
            assert path.read_text(encoding='utf-8') == text, str(path) + ' differs'
        print(name, len(rows), 'original-instruction cases passed')

    steering = []
    axes = [(0, 0, 0), (-128, 0, 0), (127, -63, 0), (0, -64, 64), (32, -64, 0)]
    for family, order, axis, brake, brake_dir in itertools.product(
            (0, 1), (0, 8, 9, 10, 11, 12, 13, 14, 15, 6, 0x18, 0x28, 0x48, 0x88, 0x108, 0x208), axes, (0, 1), (0, 3)):
        v = [family, order, *axis, 0x12345678, 0x3FFFFFC0, 0x23456789, 0x10000000, brake, brake_dir]
        steering.append(v + machine.steering(v))
    fixture('vehicle_input_vectors.inc', steering)

    brains = []
    for air, authority, event, current, pending in itertools.product(
            (0, 1), (0, 1), (2, 3, 6), (0, 14, 22), (0, 7, 11, 14, 16, 19, 22)):
        v = [air, authority, event, current, pending, 0, 0, 0, 0, 0, 0, 0]
        brains.append(v + machine.brain(v))
    for air, authority, ammo_a, ammo_b, weap_a, weap_b, guard in itertools.product(
            (0, 1), (0, 1), (0, 5), (0, 5), (0, 3), (0, 3), (0, 1)):
        v = [air, authority, 2, 0, 0, ammo_a, ammo_b, weap_a, weap_b, guard, 0, 0]
        brains.append(v + machine.brain(v))
    # The alert edge without a committed transition: the hold state or the
    # profile's +0x60 bit 2 keeps the pend, so prev records the gate's verdict.
    for air, authority, occupant, (held, flags96) in itertools.product(
            (0, 1), (0, 1), (0, 1, 2), ((1, 0), (0, 2), (1, 2))):
        current = (14 if air else 22) if held else 0
        v = [air, authority, 2, current, current, 0, 0, 0, 0, 0, occupant, flags96]
        brains.append(v + machine.brain(v))
    fixture('brain_dispatch_vectors.inc', brains)

    recovery = []
    for phase, step, flags in itertools.product((-1, 0, 495, 496, 497, 500), (16, 64), (0, 2)):
        v = [phase, step, flags]
        recovery.append(v + machine.recovery(v))
    fixture('aircraft_recovery_vectors.inc', recovery)

    walks = []
    extra = [(0x7FFFFFFF, -1, 0, 5), (-0x80000000, 0x7FFFFFFF, 1, 1), (-5, -5, 10, -5),
             (200, 10, 100, 0), (10, 200, 100, 0), (1000000, -1000000, 3, 3)]
    for keys in list(itertools.product((0, 1, 2, 3), repeat=4)) + extra:
        v = list(keys)
        walks.append(v + machine.class_walk(v))
    fixture('ai_class_walk_vectors.inc', walks)


if __name__ == '__main__':
    main()
