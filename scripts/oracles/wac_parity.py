#!/usr/bin/env python3
"""Regenerate the WAC compiler and VM witnesses from the pinned JO executable.

Compile slice: the original Script_Compile @0x4F31F0, with the resolver
WacScript_ResolveParameter @0x4F2920 it calls, compiles synthetic sources in
an x86 emulator. Every first-error write is captured in order (the buffer is
cleared after each write; the compiler reads it only to guard its own
sprintf). The engine lookups outside the compiler answer from small synthetic
catalogs (effects, sound sets, ammo, mission text, net ids, RUN files); the
anim, face and default group tables are the executable's own. The committed
listing writes pool words as values and catalog handles as their names.

VM slice: jo-c's wac_runtime_oracle executes compiled synthetic programs on
the original WacScript_ExecuteBytecode @0x4F58B0 for a few steps.

Corpus slice: the shipped .wac scripts compile with empty catalogs; only the
SHA-256 of each source and of its listing are written, never script bytes.

Requires pefile and unicorn; the native tests use the committed vectors.
Run with PYTHONDONTWRITEBYTECODE=1 so nothing is written into jo-c.
"""
import argparse
import ctypes
import hashlib
import os
import random
import struct
import sys
from pathlib import Path

sys.dont_write_bytecode = True

import pefile
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EIP, UC_X86_REG_ESP

SHA256 = 'b9971c8273b7bbb1c8518a738596d669cd7794e9d307ae63a7a9a530eb802fac'
IMAGE = 0x400000
STACK, STACK_SIZE = 0x10000000, 0x20000
RETURN = 0x11000000
SCRATCH, SCRATCH_SIZE = 0x12000000, 0x80000
IMPORTS = 0x13000000
OUTPUT = SCRATCH + 0x10000
SOURCE = SCRATCH + 0x40000
NAME = SCRATCH + 0x100
CURSOR = SCRATCH + 0x20
THREAD = SCRATCH + 0x1000
FILES = SCRATCH + 0x60000
LIMIT = 0x47A0          # WacScript_InitAndLoad @0x4F93FC: buffer + 0x47A0

POOL, POOL_COUNT = 0xC6AA30, 0xC69A1C
STRINGS, STRING_LENGTH = 0xC69A20, 0xC69A18
EVENT_COUNT, EVENT_DEPTHS = 0xC60E04, 0xC67E18
DO_COUNT, DECL_COUNT, RUN_DEPTH = 0xC60DFC, 0xC60E14, 0xC6EB20
ERROR, SCRATCH_DWORD = 0xC6EB30, 0xC6EAEC
GROUPS, GROUP_COUNT = 0xC6EC40, 0xC6EB1C
BASE_WORD, OUTPUT_END = 0xC60E10, 0xC60E0C
# The instruction after every sprintf into the first-error buffer: the one in
# Script_SetCompileError @0x4EE7C0 and the eleven inline ones.
ERROR_WRITES = (0x4EE7E7, 0x4F2AF1, 0x4F2B58, 0x4F3101, 0x4F3171, 0x4F31C1,
                0x4F364D, 0x4F388C, 0x4F5527, 0x4F559C, 0x4F5666, 0x4F56A5)
SERVICES = {0x5F7310: 'fx', 0x5274F0: 'ss', 0x409870: 'ammo', 0x51ECD0: 'tt', 0x4F0A20: 'ssn',
            0x75AA50: 'exists', 0x75B700: 'read', 0x759E70: 'alloc', 0x759EE0: 'free',
            0x759F10: 'free'}
DEFAULT_GROUPS = ('emptygroup', 'humans', 'blueplayers', 'redplayers', 'ai', 'blueai', 'redai')
TEXT_MISS = 0x74FFFF00  # the one "" every missing key answers (@0x51ED2A)

# The synthetic catalogs; tests/wac/wac_retail_vectors_test.cpp builds the same.
CATALOGS = {
    'fx': ('SMOKE', 'FIRE'),
    'ss': ('BOOM', 'ALARM'),
    'ammo': ('RIFLE', 'AMMO_PISTOL'),
    'tt': {'HELLO': 'Hello there', 'BYE': 'Goodbye'},
}
HANDLE_BASE = {'ammo': 0x71000000, 'ss': 0x72000000, 'fx': 0x73000000, 'tt': 0x74000000}


def u32(value):
    return value & 0xFFFFFFFF


def signed(value):
    return (u32(value) + 2 ** 31) % 2 ** 32 - 2 ** 31


def strcspn_ci(text, charset):
    """SHLWAPI StrCSpnIA; the real API when the host has it."""
    if os.name == 'nt':
        api = ctypes.WinDLL('shlwapi.dll').StrCSpnIA
        api.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        api.restype = ctypes.c_int
        return api(text, charset)
    lowered = set(charset.lower())
    for index, byte in enumerate(text.lower()):
        if byte in lowered:
            return index
    return len(text)


class Compiler:
    """The original compiler, one fresh emulator per compile."""

    def __init__(self, exe):
        raw = exe.read_bytes()
        assert hashlib.sha256(raw).hexdigest() == SHA256
        self.pe = pefile.PE(data=raw)
        self.image = self.pe.get_memory_mapped_image()
        base = 0x82D290  # the 165 x 44-byte command table
        self.argc = []
        for index in range(165):
            record = self.image[base - IMAGE + 44 * index: base - IMAGE + 44 * (index + 1)]
            self.argc.append(sum(1 for k in range(4) if struct.unpack_from('<I', record, 0x18 + 4 * k)[0]))
        self.named = {}
        for index in range(24):  # the named-value rows @0x82EEF0
            record = self.image[0x82EEF0 - IMAGE + 24 * index: 0x82EEF0 - IMAGE + 24 * (index + 1)]
            pointer = struct.unpack_from('<I', record, 20)[0]
            self.named.setdefault(pointer, record[:19].split(b'\0', 1)[0].decode('latin-1').lower())

    def compile(self, source, files=None, catalogs=True):
        files = {name.upper(): data for name, data in (files or {}).items()}
        uc = Uc(UC_ARCH_X86, UC_MODE_32)
        uc.mem_map(IMAGE, (self.pe.OPTIONAL_HEADER.SizeOfImage + 0xFFF) & ~0xFFF)
        uc.mem_write(IMAGE, self.image)
        for address, size in ((STACK, STACK_SIZE), (RETURN, 0x1000), (SCRATCH, SCRATCH_SIZE), (IMPORTS, 0x10000)):
            uc.mem_map(address, size)

        def w32(address, value):
            uc.mem_write(address, struct.pack('<I', u32(value)))

        def r32(address):
            return struct.unpack('<I', uc.mem_read(address, 4))[0]

        def cstr(address, limit=0x2000):
            out = bytearray()
            while len(out) < limit:
                byte = uc.mem_read(address + len(out), 1)[0]
                if not byte:
                    break
                out.append(byte)
            return bytes(out)

        # \x04 separates the layers WacScript_InitAndLoad compiles into one
        # buffer (game.wac, server.wac, then the mission, @0x4F94A8 /
        # @0x4F950E / @0x4F9597); one part is a lone script.
        parts = source.split(b'\x04')
        names = [b'script.wac'] if len(parts) == 1 else [b'game.wac', b'server.wac', b'mission.wac'][:len(parts)]
        assert len(source) < 0x1FFFF
        # WacScript_InitAndLoad's state before its first compile
        # (@0x4F93F1..0x4F9448); the group table holds the seven defaults.
        w32(BASE_WORD, OUTPUT)
        w32(OUTPUT_END, OUTPUT + LIMIT)  # maxOutputEnd, the limit a RUN compile gets (@0x4F9411)
        for address in (EVENT_COUNT, STRING_LENGTH, DECL_COUNT, RUN_DEPTH, POOL, DO_COUNT):
            w32(address, 0)
        w32(POOL_COUNT, 1)
        w32(CURSOR, OUTPUT)
        w32(GROUP_COUNT, len(DEFAULT_GROUPS))
        for index, group in enumerate(DEFAULT_GROUPS):
            uc.mem_write(GROUPS + 44 * index + 12, group.encode() + b'\0')
        uc.mem_write(ERROR, b'\0')
        w32(0x334A444, 1)  # __sse2_available, as sub_7887AF @0x7887B4 sets it
        imports = {}
        for library in self.pe.DIRECTORY_ENTRY_IMPORT:
            for entry in library.imports:
                trap = IMPORTS + 16 * len(imports)
                imports[trap] = library.dll.decode() + '!' + (entry.name.decode() if entry.name else '#%d' % entry.ordinal)
                w32(entry.address, trap)
                uc.mem_write(trap, b'\xC3')
        # An initialized CRT thread in the PE's default locale (the jo-c oracle's fixture).
        w32(THREAD + 0x6C, r32(0x85B780))
        w32(THREAD + 0x68, r32(0x85B5A0))
        w32(0x85B170, 0)
        w32(0x85B174, 1)
        tls_get = next(a for a, n in imports.items() if n == 'KERNEL32.dll!TlsGetValue')
        tls = {0: THREAD, 1: tls_get}
        state = {'last_error': 0, 'heap': FILES}
        errors = []
        seen = {kind: {} for kind in HANDLE_BASE}

        def ret(sp, value, pop=0):
            uc.reg_write(UC_X86_REG_EAX, u32(value))
            uc.reg_write(UC_X86_REG_EIP, r32(sp))
            uc.reg_write(UC_X86_REG_ESP, sp + 4 + pop)

        def on_import(uc_, address, size, data):
            name = imports.get(address)
            if name is None:
                return
            sp = uc.reg_read(UC_X86_REG_ESP)
            if name == 'SHLWAPI.dll!StrCSpnIA':
                return ret(sp, strcspn_ci(cstr(r32(sp + 4)), cstr(r32(sp + 8))), 8)
            if name == 'KERNEL32.dll!TlsGetValue':
                state['last_error'] = 0
                return ret(sp, tls[r32(sp + 4)], 4)
            if name == 'KERNEL32.dll!GetLastError':
                return ret(sp, state['last_error'])
            if name == 'KERNEL32.dll!SetLastError':
                state['last_error'] = r32(sp + 4)
                return ret(sp, state['last_error'], 4)
            raise RuntimeError('unsupported import ' + name)

        def on_error(uc_, address, size, data):
            text = cstr(ERROR)
            if text:
                errors.append(text.decode('latin-1'))
                uc.mem_write(ERROR, b'\0')

        def handle(kind, name):
            key = name.upper()
            if kind == 'tt':
                if not catalogs or key not in CATALOGS['tt']:
                    return TEXT_MISS
            elif not catalogs or key not in CATALOGS[kind]:
                return 0
            table = seen[kind]
            table.setdefault(key, HANDLE_BASE[kind] + len(table))
            return table[key]

        def on_service(uc_, address, size, data):
            kind = SERVICES[address]
            sp = uc.reg_read(UC_X86_REG_ESP)
            if kind == 'ssn':
                out, net = r32(sp + 4), r32(sp + 8)
                uc.mem_write(out, struct.pack('<H', net & 0xFFFF))
                return ret(sp, out)
            if kind == 'exists':
                return ret(sp, 1 if cstr(r32(sp + 4)).decode('latin-1').upper() in files else 0)
            if kind == 'read':
                data_ = files.get(cstr(r32(sp + 4)).decode('latin-1').upper())
                if data_ is None:
                    return ret(sp, 0xFFFFFFFF)
                if r32(sp + 8):
                    uc.mem_write(r32(sp + 8), data_)
                return ret(sp, len(data_))
            if kind == 'alloc':
                block = state['heap']
                state['heap'] += (r32(sp + 4) + 0x1F) & ~0xF
                return ret(sp, block)
            if kind == 'free':
                return ret(sp, 0)
            return ret(sp, handle(kind, cstr(r32(sp + 4)).decode('latin-1')))

        uc.hook_add(UC_HOOK_CODE, on_import, begin=IMPORTS, end=IMPORTS + 0x10000)
        for address in ERROR_WRITES:
            uc.hook_add(UC_HOOK_CODE, on_error, begin=address, end=address)
        for address in SERVICES:
            uc.hook_add(UC_HOOK_CODE, on_service, begin=address, end=address)
        for part, name in zip(parts, names):
            uc.mem_write(SOURCE, part + b'\0')
            uc.mem_write(NAME, name + b'\0')
            sp = STACK + STACK_SIZE - 0x100
            uc.mem_write(sp, struct.pack('<6I', RETURN, NAME, CURSOR, OUTPUT + LIMIT, SOURCE, len(part)))
            uc.reg_write(UC_X86_REG_ESP, sp)
            try:
                uc.emu_start(0x4F31F0, RETURN, count=200_000_000)
            except UcError as error:
                raise RuntimeError('%s at %08X' % (error, uc.reg_read(UC_X86_REG_EIP)))
            assert uc.reg_read(UC_X86_REG_EIP) == RETURN, 'the compiler did not return'
        cursor = r32(CURSOR)
        words = [w for (w,) in struct.iter_unpack('<I', bytes(uc.mem_read(OUTPUT, cursor - OUTPUT)))]
        reverse = {}
        for kind, table in seen.items():
            for key, value in table.items():
                reverse[value] = kind.upper() + ':' + key
        reverse[TEXT_MISS] = 'TT:'
        return {
            'words': words,
            'pool': [r32(POOL + 4 * i) for i in range(512)],
            'pool_count': r32(POOL_COUNT),
            'strings': bytes(uc.mem_read(STRINGS, 0x1010)),
            'errors': errors,
            'depths': list(uc.mem_read(EVENT_DEPTHS, r32(EVENT_COUNT))) if r32(EVENT_COUNT) <= 0x400 else [],
            'events': r32(EVENT_COUNT),
            'loops': r32(DO_COUNT),
            'reverse': reverse,
        }

    def operand(self, result, word):
        if POOL <= word < POOL + 0x800 and (word - POOL) % 4 == 0:
            value = result['pool'][(word - POOL) // 4]
            return result['reverse'].get(value, '#%d' % signed(value))
        if 0xC6B240 <= word < 0xC6BA40 and word % 4 == 0:
            return 'V%d' % ((word - 0xC6B240) // 4)
        if 0xC6BA40 <= word < 0xC6BE40 and word % 4 == 0:
            return 'G%d' % ((word - 0xC6BA40) // 4)
        if 0xC6CE40 <= word < 0xC6DE40 and word % 4 == 0:
            return 'E%d' % ((word - 0xC6CE40) // 4)
        if STRINGS <= word < POOL:
            text = result['strings'][word - STRINGS:].split(b'\0', 1)[0]
            return "S'" + text.decode('latin-1') + "'"
        if word == SCRATCH_DWORD:
            return 'SCRATCH'
        if word in self.named:
            return 'N:' + self.named[word]
        return 'A%08X' % word

    def document(self, result):
        """The listing both sides write: one line per word, then the errors,
        the event depth bytes and the DO count."""
        words, lines, i = result['words'], [], 0
        while i < len(words):
            word = words[i]
            op = (word >> 24) & 0x3F
            lines.append('%08X' % word)
            if op == 0 or 0x0F <= op <= 0x1C:
                # A ')' can pop a frame while parameters are pending: that POP
                # word sits between the call and its operands.
                index = word & 0xFFFF
                count = self.argc[index] if index < len(self.argc) else 0
                i += 1
                while count and i < len(words):
                    if words[i] >> 24 == 7:
                        lines.append('%08X' % words[i])
                    else:
                        lines.append(self.operand(result, words[i]))
                        count -= 1
                    i += 1
            elif op == 8:
                if i + 1 < len(words):
                    lines.append(self.operand(result, words[i + 1]))
                i += 2
            elif op in (4, 6):
                if i + 1 < len(words):
                    lines.append('%08X' % words[i + 1])
                i += 2
            else:
                i += 1
        lines.append('ERRORS')
        lines.extend(result['errors'])
        lines.append('EVENTS ' + ','.join(str(d) for d in result['depths']))
        lines.append('LOOPS %d' % result['loops'])
        return '\n'.join(lines)


def expand(source):
    """\\x02COUNT\\x02TEXT\\x03 repeats TEXT; the native test expands the same way."""
    out = bytearray()
    i = 0
    while i < len(source):
        if source[i] == 2:
            j = source.index(2, i + 1)
            k = source.index(3, j + 1)
            out += source[j + 1:k] * int(source[i + 1:j])
            i = k + 1
        else:
            out.append(source[i])
            i += 1
    return bytes(out)


def c_literal(data):
    """A C string literal; every byte a C++ source would read differently is octal."""
    out = []
    for byte in data:
        if byte == 0x5C:
            out.append('\\\\')
        elif byte == 0x22:
            out.append('\\"')
        elif byte == 0x3F:
            out.append('\\077')
        elif 0x20 <= byte < 0x7F:
            out.append(chr(byte))
        else:
            out.append('\\%03o' % byte)
    return '"' + ''.join(out) + '"'


def directed_cases():
    """(label, source, run files) witnesses for every compiler path the port
    translates; sources use CR LF like the shipped scripts."""
    L = '\r\n'
    rep = lambda n, text: b'\x02%d\x02' % n + text.encode('latin-1') + b'\x03'
    cases = [
        # The tokenizer: comments, CR-only line counts, strings, words, pairs.
        ('comments', 'v1 = 5 ; set it' + L + '// whole line' + L + 'v2 = 10 / 2 // half' + L + 'v3 = 7' + L, None),
        ('lf-only lines', 'v1 = 1\nbogus\nv2 = 2\n', None),
        ('cr-only lines', 'v1 = 1\rbogus\rv2 = 2\r', None),
        ('string', 'text("hello, world; really")' + L, None),
        ('string long', 'text("' + 'x' * 70 + '")' + L + 'text(' + 'Y' * 70 + ')' + L, None),
        ('string unterminated', 'text("abc' + L + 'v1 = 2' + L, None),
        ('string eof', 'text("', None),
        ('word long', 'v1 = ' + 'A' * 66 + L, None),
        ('lowercase high bytes', 'text(caf\xe9) text("caf\xe9")' + L, None),
        ('pairs', 'if 1 <= 2 and 2 >= 1 and 1 <> 2 and 1 != 2 and 1 ~= 2 and 1 == 1 || 0 && 1 then inc(v1) endif' + L, None),
        ('singles', 'v1 = 7 % 3 + 2 * 3 - 4 / 2 ^ 1' + L + 'if 1 < 2 and 2 > 1 then inc(v2) endif' + L, None),
        ('minus words', 'v1 = 5 - -3' + L + 'set(v2, -7)' + L + 'v3 = load(-40)' + L + 'v4 = -5' + L, None),
        ('keyword prefixes', 'if true(1) thence inc(v1) endif' + L + 'if true(0) then inc(v2) elsewhere inc(v3) enddo' + L, None),
        ('elsie', 'if true(0) then inc(v1) elsie true(1) then inc(v2) endif' + L, None),
        ('end family', 'if true(1) then doseq inc(v1) next inc(v2) enddo endif' + L + 'gloop humans inc(v3) endloop' + L + 'if true(1) then endp' + L + 'if true(1) then endx' + L, None),
        # Values and the resolver legs.
        ('named values', 'set(v1, ticks) set(v2, result) set(v3, health) set(v4, rnd) set(v5, curtod)' + L +
         'set(player, 3) set(item, 4) set(auto, 5) set(squadssn, 1) set(squadwho, 2)' + L +
         'set(night, 1) set(seatbelt, 1) set(wind, 2) set(breathtime, 3) set(fallmps, 4)' + L +
         'set(accuracyspread, 5) set(autogain, 1) set(mana, 3) set(bluekills, 1) set(greenkills, 2)' + L +
         'set(humans, 1) set(gameover, 1) set(winvar, 1) set(losevar, 1)' + L, None),
        ('banks', 'set(v0, 1) set(v255, 2) set(v256, 3) set(g0, 4) set(g255, 5) set(g300, 6) set(m1, 7) set(v9x, 8)' + L, None),
        ('declarations', 'var alpha' + L + 'cheat beta' + L + 'var alpha' + L + 'var ticks' + L + 'var 5' + L + 'var g_humans' + L +
         'set(alpha, 3) set(beta, 4)' + L + 'alpha = alpha + beta' + L, None),
        ('declaration overflow', 'var zz' + L + ' '.join('var q%d' % i for i in range(258)) + L + 'set(q0, 1) set(q257, 2)' + L, None),
        ('if names', 'if [first] true(1) then inc(v1) endif' + L + 'if [first] true(1) then inc(v2) endif' + L +
         'if true(1) [second] [third] then inc(v3) endif' + L + '[orphan]' + L +
         'reset(first) reset(second) reset(nosuch) set(v4, first) set(first, 1)' + L +
         'if true(first) and true(second) then inc(v5) endif' + L, None),
        ('ifname collisions', 'var taken' + L + 'if [taken] true(1) then inc(v1) endif' + L +
         'if [ticks] true(1) then inc(v2) endif' + L, None),
        ('variable slots', 'set(5, 1) inc(first) store(ticks) dec("v1") add(v1, 2) sub(v1)' + L, None),
        ('groups', 'gkill(g_humans) gkill(humans) gkill(g_nosuch) gkill(redai) gremove(5) gkill("ai")' + L, None),
        ('effects', 'fx2tgt(fx_smoke, 1) fx2tgt(fire, 2) fx2tgt(fx_nosuch, 3) fxrain(smoke) load(fx_fire)' + L, None),
        ('faces', 'face(face_normal) face(smile) face(face_nosuch) ssnface(5, face_angry) load(face_normal)' + L, None),
        ('sound sets', 'sound(ss_boom, 10, 20) sound(alarm, 1, 2) sound(ss_nosuch, 1, 2) ss2ssn(boom, 5) load(ss_alarm)' + L, None),
        ('text tokens', 'ssnname(5, tt_hello) ssnname(6, bye) ssnname(7, tt_nosuch) ssnname(8, nothing) ssnname(9, "quoted")' + L +
         'load(tt_hello) set(v1, tt_bye)' + L, None),
        ('anims', 'anim(anim_walk_forward) anim(idle) anim(anim_nosuch) anim(5) forceanim(reset) ssnanim(5, anim_idle) load(anim_run_2)' + L, None),
        ('ssns', 'killssn(ssn_12) killssn(34) killssn(ssn_70000) killssn(nosuch) killssn("5") load(ssn_9) ssnonssn(ssn_1, 2)' + L, None),
        ('ammo', 'ammo2tgt(ammo_rifle, 1) ammo2tgt(pistol, 2) ammo2tgt(ammo_nosuch, 3) ammorain(rifle) load(ammo_pistol)' + L, None),
        ('wrong parameter', 'set(v1, fx_smoke) set(v2, g_humans) set(v3, anim_idle) eq(face_normal, 1) true(ss_boom)' + L, None),
        ('numbers', 'load(5) load(-5) load(1.5) load(.5) load(1.5M) load(2F) load(12:30) load(1D3) load(1E3) load(3E) load(2.5d1)' + L +
         'load(32767.99M) load(65536M) load(3000000000) load(-40000M) load(100000F) load(1:2:3) load(:5)' + L, None),
        ('scaled slots', 'fogdist(5) movefog(2, 3) tod(12) tod(6:30) tod(1.5) ssnnearssn(1, 2, 3) groupmin(1, 2.5)' + L, None),
        ('text slots', 'text(hello) text("mixed Case") wave(file.wav) text(v1) text(ticks) text(12) text(first)' + L, None),
        ('null legs', 'set(v1, nosuchname) eq(1, "quoted") true(nosuch) load(nosuch) killssn()' + L, None),
        ('refused tokens refeed', 'eq(bogus 3 4)' + L + 'set(bogus v1 5)' + L + 'eq(1 2 3)' + L, None),
        ('unknown words', 'nosuchcommand' + L + '= 3' + L + '5 = 3' + L + ']' + L, None),
        ('pool dedup', 'load(7) load(7) load(0) load(-1) load(4294967295) load(7.9)' + L, None),
        ('pool overflow', ' '.join('load(%d)' % i for i in range(1, 516)) + L + 'load(3) load(600) load(601)' + L, None),
        ('string overflow', rep(70, 'text(' + 'S' * 60 + ') ').decode('latin-1') + L, None),
        # Expressions, parens and assignment.
        ('precedence', 'v1 = 1 + 2 * 3' + L + 'v2 = 2 * 3 + 1' + L + 'v3 = 1 + 2 * 3 ^ 2 - 4' + L + 'v4 = 1 or 2 == 3 + 4 * 5' + L, None),
        ('parens', 'v1 = (1 + 2) * 3' + L + 'v2 = 2 * (3 + 4)' + L + 'v3 = ((1))' + L + 'v4 = (1 + 2 * 3)' + L + 'v5 = not (1)' + L, None),
        ('auto paren close', 'v1 = 2 * (3 + 4 * 5) + 1' + L + 'v2 = (1 + 2 * 3) * 4' + L, None),
        ('not', 'if not true(0) then inc(v1) endif' + L + 'if ! true(1) then inc(v2) endif' + L + 'v3 = not not 1' + L, None),
        ('unexpected operators', 'v1 = 1 + * 2' + L + 'v2 = 1 and or 2' + L + 'v3 = not + 1' + L + 'v4 = 1 == != 2' + L +
         'v5 = 1 < <= 2' + L + 'v6 = 1 ~= ^ 2' + L + 'v7 = 1 % / 2' + L + 'v8 = 1 - > 2' + L + 'v9 = 1 <> && 2' + L + 'v10 = 1 >= || 2' + L, None),
        ('paren errors', 'v1 = 1 )' + L + 'v2 = (((((((((((((((((1)))))))))))))))))' + L + 'if (true(1) then inc(v3) endif' + L, None),
        ('auto paren too deep', 'v1 = (((((((((((((((1 + 2 * 3 ^ 4)))))))))))))))' + L + 'v2 = 5' + L, None),
        ('frame overflow', 'v1 = ((((((((((((((((1 + 2 * 3 ^ 4 * 5 + 6))))))))))))))))' + L + 'v2 = 7 + 8' + L + 'text("{}")' + L, None),
        ('assignment', 'v1 = 5' + L + 'v2 = v1 + 1' + L + 'v3 = v4 = 2' + L + 'v5 =' + L + 'v6 = 1 v7 = 2' + L, None),
        ('assignment errors', 'v1 + v2 = 3' + L + '(v3 = 4)' + L + 'v4 = (1 + 2' + L + 'v5 = 3 then' + L, None),
        ('store at eof', 'v1 = 2 + 3', None),
        ('store before keyword', 'v1 = 2 if true(v1) then inc(v2) endif' + L, None),
        # Blocks.
        ('if chain', 'if true(0) then inc(v1) elseif true(1) then inc(v2) else inc(v3) endif' + L, None),
        ('else if', 'if true(0) then inc(v1) else if true(1) then inc(v2) endif endif' + L +
         'if never then set(v3, 1) else if never then set(v3, 2) endif' + L + 'if never then set(v4, 9) endif' + L, None),
        ('enter leave', 'if true(v1) enter inc(v2) endif' + L + 'if true(v1) leave inc(v3) else inc(v4) endif' + L, None),
        ('block errors', 'then' + L + 'enter' + L + 'leave' + L + 'else' + L + 'elseif true(1) then' + L + 'endif' + L +
         'if if true(1) then endif' + L + 'if true(1) else endif' + L + 'if true(1) endif' + L, None),
        ('not before then', 'if true(1) not then inc(v1) endif' + L + 'if true(1) ! enter inc(v2) endif' + L + 'if true(1) not leave endif' + L, None),
        ('and before then', 'if true(1) and then inc(v1) endif' + L + 'if true(1) + then inc(v2) endif' + L, None),
        ('enif', 'if true(1) then' + L + 'inc(v1)' + L + 'enif' + L + 'inc(v2)' + L, None),
        ('missing end', 'if true(1) then inc(v1)' + L + 'doseq inc(v2) next inc(v3)' + L + 'gloop humans inc(v4)' + L, None),
        ('eof in condition', 'if true(1) and', None),
        ('eof params', 'set(v1,', None),
        ('do blocks', 'doseq inc(v1) next inc(v2) next inc(v3) enddo' + L + 'dornd inc(v4) next inc(v5) enddo' + L +
         'doseq doseq inc(v6) next inc(v7) enddo next inc(v8) enddo' + L, None),
        ('do errors', 'next' + L + 'if doseq then endif' + L + 'if true(1) dornd' + L + 'if true(1) next' + L, None),
        ('do in if', 'if true(1) then doseq inc(v1) next inc(v2) enddo else dornd inc(v3) next inc(v4) enddo endif' + L, None),
        ('loops', 'gloop g_humans inc(v1) endloop' + L + 'ploop inc(v2) endloop' + L + 'gloop ai gloop redai inc(v3) end end' + L +
         'if gloop then endif' + L + 'if ploop then endif' + L, None),
        ('gloop operands', 'gloop 5 inc(v1) end' + L + 'gloop v1 inc(v2) end' + L + 'gloop nosuch inc(v3) end' + L + 'gloop "humans" inc(v4) end' + L, None),
        ('ifname keyword errors', '[a]' + L + 'if true(1) then [b] endif' + L, None),
        ('keyword abandons', 'if never() and 1 + 2 then set(v1,1) endif' + L + 'if never() and 1 + 2 then -5 endif' + L, None),
        ('drain before keyword', 'v1 = (1 + 2' + L + 'if true(1) then inc(v2) endif' + L, None),
        ('out of if space', rep(1025, 'if true(1) then endif ').decode('latin-1') + L, None),
        ('out of do space', rep(1025, 'doseq enddo ').decode('latin-1') + L, None),
        ('over compile buffersize', rep(2400, 'inc(v1) ').decode('latin-1') + L + 'inc(v2)' + L, None),
        # RUN.
        ('run', 'inc(v1)' + L + 'run other.txt' + L + 'inc(v2)' + L, {'OTHER.WAC': 'var shared' + L + 'set(shared, 3) inc(v3)' + L}),
        ('run nesting', 'run a' + L + 'inc(v1)' + L, {'A.WAC': 'run b' + L + 'inc(v2)', 'B.WAC': 'run c' + L + 'inc(v3)', 'C.WAC': 'inc(v4)'}),
        ('run errors', 'run missing' + L + 'if true(1) then run other endif' + L + 'run "other"' + L + 'run' , {'OTHER.WAC': 'inc(v5)'}),
        ('run tail', 'run other', {'OTHER.WAC': 'if true(1) then inc(v1)'}),
        # The layered load: one buffer, shared tables, each file's own name.
        ('layers', 'var shared' + L + 'if [gate] true(1) then inc(v1) endif' + L + '\x04' +
         'set(shared, 2) reset(gate) doseq inc(v2) next inc(v3) enddo' + L + '\x04' +
         'if true(gate) then set(v4, shared) endif' + L + 'bogus' + L + 'if true(1) then' + L, None),
        ('layers empty', '\x04\x04inc(v1)' + L, None),
        ('layers overflow', rep(1400, 'inc(v1) ').decode('latin-1') + '\x04' + rep(1400, 'inc(v2) ').decode('latin-1') +
         '\x04var late' + L + 'inc(v3)' + L, None),
        # Corpus shapes found by the review (synthetic).
        ('killssn before endif', 'if true(1) then' + L + 'killssn()' + L + 'endif' + L + 'inc(v1)' + L, None),
        ('foglevel args', 'fogdist(100M, 5)' + L + 'movefog 3000 10' + L, None),
        ('colon value', 'if past(0:30) then inc(v1) endif' + L + 'load(1:30)' + L, None),
        ('comment after then', 'if never() and then //ChemStacks Blow' + L + 'killssn(1)' + L + 'killssn(2)' + L +
         'endif' + L + 'If never() then inc(v1) endif' + L, None),
        ('unknown command args', 'foglevel (250)' + L + ':' + L + 'inc(v1)' + L, None),
        ('string control bytes', 'text("a\tb") text("c' + '\x01' + 'd")' + L, None),
        ('word bytes', 'load($5) load(@5) load(?5) set(v1, `x) load(a\\b)' + L +
         'v2 = 1 // lf comment\nv3 = 3\r\n', None),
        ('no such keywords', 'array a' + L + 'do inc(v1) enddo' + L + 'v2 = 1 xor 2' + L + 'cheat c' + L + 'set(c, 4)' + L, None),
        ('number forms', 'load(1.5:30) load(1:30M) load(1:30F) load(0x10) load(1e400) load(-1e400) load(1.) load(-.5)' + L +
         'tod(1.5:30) fogdist(1:30) load(2d-1) load(12:30:45)' + L, None),
    ]
    return [(label, source.encode('latin-1') if isinstance(source, str) else source,
             {k: v.encode('latin-1') for k, v in (files or {}).items()}) for label, source, files in cases]


def fuzz_cases(count, seed=20260923):
    """Seeded token soups over the language's whole surface."""
    rng = random.Random(seed)
    atoms = ['1', '2', '0', '10', '300', '-5', '1.5', '2M', '3F', '1:30', 'v1', 'v2', 'v9', 'g1', 'm1', 'ticks', 'result',
             'health', 'auto', 'rnd', 'alpha', 'first', '"str"', 'nosuch', 'fx_smoke', 'ss_boom', 'tt_hello', 'anim_idle',
             'face_normal', 'ssn_5', 'ammo_rifle', 'g_humans']
    calls = ['true(%s)', 'false(%s)', 'eq(%s, %s)', 'set(v%d, %s)', 'inc(v%d)', 'load(%s)', 'random(3)', 'past(%s)',
             'never', 'reset(first)', 'text(%s)', 'store(v%d)', 'add(v%d, %s)', 'killssn(%s)', 'anim(%s)', 'gkill(%s)',
             'fx2tgt(%s, 1)', 'sound(%s, 1, 2)', 'ssnname(1, %s)', 'ammo2tgt(%s, 2)', 'tod(%s)', 'fogdist(%s)']
    words = ['if', 'then', 'else', 'elseif', 'endif', 'end', 'enter', 'leave', 'doseq', 'dornd', 'next', 'enddo', 'gloop',
             'ploop', 'endloop', 'var alpha', 'cheat beta', '[first]', ']', 'not', '!', '(', ')', '=', '+', '-', '*', '/',
             '%', '^', '==', '!=', '<>', '~=', '<', '>', '<=', '>=', '&&', '||', 'and', 'or', ',', 'run other']
    separators = [' ', ' ', ' ', '\r\n', '\t', ',', ' ; note\r\n', ' // note\r\n', '\n']

    def call():
        form = rng.choice(calls)
        args = []
        for _ in range(form.count('%')):
            args.append(str(rng.randint(0, 9)) if form.find('v%d') >= 0 and not args else rng.choice(atoms))
        try:
            return form % tuple(args)
        except TypeError:
            return form

    def expression(depth=0):
        parts = []
        for k in range(rng.randint(1, 4)):
            if k:
                parts.append(rng.choice(['+', '-', '*', '/', '%', '^', '==', '!=', '<', '>', '<=', '>=', 'and', 'or', '&&', '||']))
            if rng.random() < 0.15:
                parts.append(rng.choice(['not', '!']))
            if depth < 2 and rng.random() < 0.2:
                parts.append('(' + expression(depth + 1) + ')')
            else:
                parts.append(rng.choice(atoms + [call(), call()]))
        return ' '.join(parts)

    def statement(depth=0):
        r = rng.random()
        if r < 0.25:
            return 'v%d = %s' % (rng.randint(1, 9), expression())
        if r < 0.45 and depth < 3:
            body = ' '.join(statement(depth + 1) for _ in range(rng.randint(0, 2)))
            tail = rng.choice(['endif', 'endif', 'else ' + statement(depth + 1) + ' endif', 'elseif %s then %s endif' % (expression(), statement(depth + 1)), ''])
            return 'if %s %s %s %s' % (rng.choice(['', '[first]', '']), expression(), rng.choice(['then', 'then', 'enter', 'leave']), body + ' ' + tail)
        if r < 0.55 and depth < 3:
            return '%s %s next %s enddo' % (rng.choice(['doseq', 'dornd']), statement(depth + 1), statement(depth + 1))
        if r < 0.6 and depth < 3:
            return rng.choice(['gloop g_humans', 'ploop', 'gloop ai']) + ' ' + statement(depth + 1) + ' endloop'
        if r < 0.85:
            return call()
        return ' '.join(rng.choice(words + atoms) for _ in range(rng.randint(1, 6)))

    out = []
    for index in range(count):
        parts = []
        for _ in range(rng.randint(1, 5)):
            parts.append(statement())
            parts.append(rng.choice(separators))
        source = ''.join(parts)
        if rng.random() < 0.3:
            source = ''.join(ch.upper() if rng.random() < 0.3 else ch for ch in source)
        out.append(('fuzz %d' % index, source.encode('latin-1'), {'OTHER.WAC': b'inc(v7) var shared\r\n'}))
    return out


def soup_cases(count, seed=9232026):
    """Seeded byte soups for the tokenizer: every operator byte, quotes,
    separators, comment starts, high bytes and fragments of real words."""
    rng = random.Random(seed)
    alphabet = (list('{}()[]+-*/|&^%<>=!~') + ['"', ';', ',', ' ', ' ', '\t', '\r', '\n', '\r\n', '//', ':', '.', '_'] +
                list('0123456789') + list('aeimrstvxzAGMSVT') + ['\xe9', '\x80', '\x7f', '`'] +
                ['if', 'then', 'else', 'elseif', 'end', 'endif', 'doseq', 'dornd', 'next', 'gloop', 'ploop', 'var',
                 'cheat', 'run', 'not', 'and', 'or', 'enter', 'leave', 'inc', 'set', 'load', 'text', 'v1', 'g2', 'm3',
                 'eq', 'true', 'ticks', 'tt_', 'fx_', 'ss_', 'ssn_', 'anim_', 'face_', 'ammo_', 'g_'])
    out = []
    for index in range(count):
        text = ''.join(rng.choice(alphabet) for _ in range(rng.randint(1, 60 if index % 4 else 240)))
        out.append(('soup %d' % index, text.encode('latin-1'), {'OTHER.WAC': b'inc(v7)'}))
    return out


VM_SOURCES = [
    ('power', 'if 2 ^ 3 then store(v1) endif\nif (-2) ^ 3 then store(v2) endif\nif 2 ^ (-3) then store(v3) endif\nif 3 ^ 0 then store(v4) endif\n'),
    ('variables', 'set(v1,2147483647)\ninc(v1)\nstore(v2)\nadd(v2,7)\nsub(v2,9)\ndec(v2)\nload(v2)\n'),
    ('globals', 'set(g1,-7)\nadd(g1,11)\nload(g1)\nstore(v2)\n'),
    ('doseq', 'doseq inc(v1) next inc(v2) next inc(v3) enddo\n'),
    ('nested do', 'doseq inc(v1) next doseq inc(v2) next inc(v3) enddo next inc(v4) enddo\ndornd inc(v5) next inc(v6) enddo\n'),
    ('clock', 'if past(3) then inc(v1) endif\nif before(3) then inc(v2) endif\nif ontick(3) then inc(v3) endif\n'),
    ('random', 'random(7)\nstore(v1)\nrandom(0)\nstore(v2)\nrandom(-1)\nstore(v3)\n'),
    ('if state', 'if true(v1) then inc(v2) endif\ninc(v1)\nif elapse(3) then inc(v3) endif\n'),
    ('expressions', 'v1 = 1 + 2 * 3\nv2 = (1 + 2) * 3\nv3 = 2 * (3 + 4 * 5) + 1\nv4 = 300 + 2 * 3\nv5 = 7 % 3 - 10 / 4\nv6 = not 0 + 1\n'),
    ('comparisons', 'v1 = 1 < 2\nv2 = 2 <= 1\nv3 = 3 == 3 and 1\nv4 = 1 != 1 or 0\nv5 = -1 > 1\nv6 = 5 >= 5\n'),
    ('else if chain', 'if never then\r\n\t\tset(v1,1)\r\nelse if never then\r\n\t\tset(v1,2)\r\nendif\r\nif never then\r\n\t\tset(v2,9)\r\nendif\r\n'),
    ('elseif', 'if true(0) then set(v1,5) elseif true(2) then set(v1,9) else set(v1,7) endif\n'),
    ('enter leave', 'if eq(v1,1) enter inc(v2) endif\nif eq(v1,1) leave inc(v3) endif\nif past(1) then set(v1,1) endif\nif past(2) then set(v1,0) endif\n'),
    ('named reset', 'var counter\nif [root] never then inc(counter)\n if never [child] then inc(v6) endif\nendif\n'
     'if [adjacent] never then inc(v3) endif\nif ontick(1) then reset(root) endif\nset(v1,counter)\nif true(root) then inc(v4) endif\n'),
    ('enif', 'if true(1) then\r\ninc(v1)\r\nenif\r\ninc(v2)\r\n'),
    ('and before then', 'if true(1) and then inc(v1) endif\r\ninc(v2)\r\n'),
    ('assign chains', 'v1 = 5\nv2 = v1 + 1\nv3 = v4 = 2\nv5 = 1 v6 = 2\n'),
    ('keyword abandons', 'if never() and 1 + 2 then set(v1,1) endif\nif never() and 1 + 2 then -5 endif\ninc(v2)\n'),
    ('missing end', 'if true(v1) then inc(v2)\ndoseq inc(v3) next inc(v4)\n'),
    ('previous chain', 'if true(1) then inc(v1) endif\nif previous then inc(v2) endif\nif chain(1) then inc(v3) endif\n'),
]


def vm_vectors(compiler, jo_c, exe, steps=4):
    sys.path.insert(0, str(jo_c / 'tools'))
    import wac_runtime_oracle as oracle
    oracle.configure()
    machine = oracle.Machine(exe)
    rows = []
    for label, source in VM_SOURCES:
        result = compiler.compile(source.encode('latin-1'))
        compiled = {'words': ['%08X' % w for w in result['words']], 'value_pool': result['pool'][:max(result['pool_count'], 1)]}

        def compile_state(vm, result=result):
            # The compile's event count and depth bytes that reset() walks
            # (WacCmd_Reset @0x4ED304 / @0x4ED31B); the harness zeroes them.
            vm.wr(EVENT_COUNT, result['events'])
            vm.u.mem_write(vm.locate(EVENT_DEPTHS), bytes(result['depths']))
        snapshots = machine.run(compiled, steps=steps, setup=compile_state)
        values = []
        for snap in snapshots:
            values += [snap['result']] + snap['vars'] + snap['globals'] + snap['cycles'] + snap['loops'] + [snap['rng']]
            values += snap['if_ticks'] + snap['if_active'] + snap['if_state']
        rows.append((label, source.encode('latin-1'), [signed(v) for v in values]))
    return rows


def pff_wac_entries(path):
    with open(path, 'rb') as stream:
        header, magic, count, stride, table = struct.unpack('<5I', stream.read(20))
        stream.seek(table)
        records = [stream.read(stride) for _ in range(count)]
        out = []
        for record in records:
            flags, offset, size = struct.unpack_from('<3I', record)
            name = record[16:32].split(b'\0', 1)[0].decode('latin-1')
            if name.lower().endswith('.wac') and flags == 0:
                stream.seek(offset)
                out.append((name, stream.read(size)))
        return out


def corpus_rows(compiler, exe):
    game = exe.parent
    entries = []
    for archive in (game / 'localres.pff', game / 'expansion' / 'revx02' / 'RevX02.pff'):
        if archive.exists():
            entries += pff_wac_entries(archive)
    files = {name.upper(): data for name, data in entries}
    rows = {}
    for name, data in entries:
        document = compiler.document(compiler.compile(data, files, catalogs=False))
        rows[hashlib.sha256(data).hexdigest()] = hashlib.sha256(document.encode('latin-1')).hexdigest()
    return sorted(rows.items())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jo-c', type=Path, required=True)
    parser.add_argument('--retail-exe', type=Path, required=True)
    parser.add_argument('--write', action='store_true')
    parser.add_argument('--fuzz', type=int, default=120, help='seeded statement cases (the committed set uses 120)')
    parser.add_argument('--soup', type=int, default=60, help='seeded byte-soup cases (the committed set uses 60)')
    parser.add_argument('--seed', type=int, default=0, help='offsets both fuzz seeds (the committed set uses 0)')
    args = parser.parse_args()
    compiler = Compiler(args.retail_exe)
    root = Path(__file__).resolve().parents[2] / 'tests/wac/fixtures'
    header = '// Original JO x86; regenerate with scripts/oracles/wac_parity.py.\n'

    def fixture(name, text):
        path = root / name
        if args.write:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding='utf-8', newline='\n')
        else:
            assert path.read_text(encoding='utf-8') == text, str(path) + ' differs'

    compile_text = [header]
    cases = directed_cases() + fuzz_cases(args.fuzz, 20260923 + args.seed) + soup_cases(args.soup, 9232026 + args.seed)
    for label, source, files in cases:
        result = compiler.compile(expand(source), files)
        document = compiler.document(result)
        packed = b''.join(name.encode() + b'\x01' + data + b'\x01' for name, data in sorted(files.items()))
        if len(document) > 6000:
            document = 'sha256:' + hashlib.sha256(document.encode('latin-1')).hexdigest()
        compile_text.append('{// %s\n %s,\n %s,\n' % (label, c_literal(source), c_literal(packed)))
        lines = document.split('\n')
        compile_text.append(''.join('  %s\n' % c_literal((line + ('\n' if k + 1 < len(lines) else '')).encode('latin-1'))
                                    for k, line in enumerate(lines)))
        compile_text.append('},\n')
    fixture('wac_retail_compile_vectors.inc', ''.join(compile_text))
    print(len(cases), 'original-compiler cases')

    vm_text = [header]
    rows = vm_vectors(compiler, args.jo_c.resolve(), args.retail_exe.resolve())
    for label, source, values in rows:
        vm_text.append('{// %s\n %s,\n {%s}},\n' % (label, c_literal(source), ','.join(
            str(v) if v != -2 ** 31 else '-2147483647 - 1' for v in values)))
    fixture('wac_retail_vm_vectors.inc', ''.join(vm_text))
    print(len(rows), 'original-VM programs')

    corpus = corpus_rows(compiler, args.retail_exe.resolve())
    fixture('wac_retail_corpus_vectors.inc', header + ''.join('{"%s", "%s"},\n' % row for row in corpus))
    print(len(corpus), 'shipped scripts hashed')


if __name__ == '__main__':
    main()
