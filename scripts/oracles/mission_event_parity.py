#!/usr/bin/env python3
"""Regenerate the BMS event runtime witnesses from the pinned JO executable.

Executes jo-c's mission-event probes in the original PE under Unicorn: the
condition evaluator EventTrigger_EvaluateCondition @0x453620, the chain fold
EventTrigger_EvaluateChain @0x454050, EventAction_Dispatch @0x4542E0, the entry
scheduler EventTrigger_UpdateEntry @0x454C30 and the quarter/pre/post passes
@0x454D50/@0x454DC0/@0x454E00. The extended probes add the signed trigger
count, the raw Berserk fold, the SingleDestroyed/SingleAlive row walk
(Entity_IsAliveByBmsRef @0x43E640) and the authority-gated linked-spawn marks
(EventTrigger_MarkLinkedSpawnPoints @0x452CE0). No event, action, timer or
input predicate is mocked; the inputs are synthetic records and every output
is the original's. The committed C++ test (tests/mission/
mission_event_vectors_test.cpp) replays the vectors through the public
BmsEventSystem seams without Python or the game.

Requires pefile, capstone and unicorn (jo-c's harness). Check mode by default;
--write rewrites the fixture.
"""
import argparse
import hashlib
import struct
import sys
from pathlib import Path

sys.dont_write_bytecode = True

SHA256 = 'b9971c8273b7bbb1c8518a738596d669cd7794e9d307ae63a7a9a530eb802fac'
ROOT_IDS = {'condition': 0, 'chain': 1, 'action': 2, 'entry': 3, 'quarter': 4, 'pre': 5, 'post': 6}
FIXTURE = 'tests/mission/fixtures/mission_event_vectors.inc'


def signed(n):
    return (int(n) + 2**31) % 2**32 - 2**31


def literal(n):
    # INT32_MIN spelled so the C++ literal never narrows from unsigned long.
    v = signed(n)
    return '(-2147483647-1)' if v == -2**31 else str(v)


def load_harness(jo_c):
    sys.path.insert(0, str(jo_c / 'tools'))
    import mission_events_oracle as meo  # noqa: E402  (jo-c harness)
    from lan_discovery_oracle import P, SP, STOP, SAVED, pack, u32  # noqa: E402
    from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EIP, UC_X86_REG_ESP  # noqa: E402

    E, T, A = P + 0x1000, P + 0x4000, P + 0x8000
    ROWS, AIRT, WAYPOINTS = P + 0x20000, P + 0x60000, P + 0xB000
    POOL_LIST, NAPI_AUTHORITY, LOCAL_PLAYER = 0xA892E0, 0xB5CBC8 + 0x60, 0xB75FC8

    class Machine(meo.Machine):
        """jo-c's mission-event machine plus the extended fixture fields: pool-0
        rows (DcbId +0x7C, Flags +0x24), a local player whose aiRuntime word 1
        carries the behavior bits, and the authority byte."""

        def run_case(self, root, v):
            u = self.u
            u.context_restore(self.initial)
            u.mem_write(P, bytes(0x100000))
            for _, n, q in self.globals:
                u.mem_write(q, bytes(n))
            events = v.get('events', [{}])
            self.wr(0xAE0700, len(events))
            self.wr(0xAE0704, E)
            self.wr(0x815174, 1)
            self.wr(0xB3B738, v.get('input', 0) & 0xFFFFFFFF)
            self.wr(0xAE06F8, v.get('mirror', 0) & 0xFFFFFFFF)
            self.wr(0xC6B240, *[x & 0xFFFFFFFF for x in v.get('vars', [0])])
            for i, t in enumerate(v.get('triggers', [(0, 4, 1, 0, 0, 0, 0, 0)])):
                u.mem_write(T + 32 * i, pack(*[x & 0xFFFFFFFF for x in t]))
            for i, a in enumerate(v.get('actions', [(0, 5, 4, 0, 0, 0, 0, 0)])):
                u.mem_write(A + 32 * i, pack(*[x & 0xFFFFFFFF for x in a]))
            for i, e in enumerate(events):
                u.mem_write(E + 24 * i, struct.pack(
                    '<IIIHHHHBBBB', e.get('flags', 0), T + 32 * e.get('trigger_start', 0),
                    A + 32 * e.get('action_start', 0), e.get('cooldown', 0) & 65535,
                    e.get('reload', 0) & 65535, e.get('delay', 0) & 65535,
                    e.get('delay_reload', 0) & 65535, e.get('active', 0), e.get('triggers', 0),
                    e.get('actions', 1), 0))
            waypoints = v.get('waypoints', [])
            self.wr(0xB76568, len(waypoints))
            self.wr(0xB7656C, 0)
            for i, (event, backchain) in enumerate(waypoints):
                q = WAYPOINTS + i * 0x300
                self.wr(0xB76570 + 4 * i, q)
                u.mem_write(q + 528, struct.pack('<H', event & 0xFFFF))
                u.mem_write(q + 535, bytes([backchain]))
            rows = v.get('rows', [])
            if rows:
                self.wr(POOL_LIST, ROWS, 0x2B4, len(rows), 64)
                for i, (dcb, flags) in enumerate(rows):
                    u.mem_write(ROWS + i * 0x2B4 + 0x7C, pack(dcb & 0xFFFFFFFF))
                    u.mem_write(ROWS + i * 0x2B4 + 0x24, pack(flags & 0xFFFFFFFF))
            if v.get('local_behavior', -1) >= 0:
                self.wr(LOCAL_PLAYER, ROWS + 63 * 0x2B4)
                u.mem_write(ROWS + 63 * 0x2B4 + 0x68, pack(AIRT))
                u.mem_write(AIRT + 4, pack(v['local_behavior']))
            if v.get('authority', 0):
                self.wr(NAPI_AUTHORITY, 1)
            snapshots = []
            for _ in range(v.get('steps', 1)):
                event = E + 24 * v.get('entry_index', 0)
                args = () if root in ('quarter', 'pre', 'post') else (
                    {'condition': T, 'chain': event, 'action': A, 'entry': event}[root],)
                u.mem_write(SP, pack(STOP, *args))
                for r in SAVED:
                    u.reg_write(r, 0x13572468)
                u.reg_write(UC_X86_REG_ESP, SP)
                u.emu_start(self.entries[root], STOP, count=200000)
                assert u.reg_read(UC_X86_REG_EIP) == STOP, (root, v)
                assert u.reg_read(UC_X86_REG_ESP) == SP + 4, (root, v)
                assert all(u.reg_read(r) == 0x13572468 for r in SAVED), (root, v)
                raw = bytes(u.mem_read(E, 24 * len(events)))
                words = []
                for i in range(len(events)):
                    cd, rl, dl, drl, act = struct.unpack_from('<HHHHB', raw, 24 * i + 12)
                    words += [cd, rl, dl, drl, act]
                snapshots.append(dict(
                    result=u.reg_read(UC_X86_REG_EAX) if root in ('condition', 'chain') else 0,
                    events=words,
                    vars=list(struct.unpack('<8i', self.read(0xC6B240, 32))),
                    input=u32(self.read(0xB3B738, 4)), mirror=u32(self.read(0xAE06F8, 4)),
                    marks=[u.mem_read(WAYPOINTS + i * 0x300 + 536, 1)[0] for i in range(len(waypoints))]))
            return snapshots

    return meo, Machine


def extended_cases():
    out = []

    def add(root, **v):
        out.append((root, v))

    # (a) The trigger-count byte is signed in the chain: 128..255 evaluate
    # trigger 0 only. [orig: EventTrigger_EvaluateChain @0x45405F/@0x45408C/@0x4540C7]
    for count in (2, 127, 128, 200, 255):
        records = dict(vars=[0], events=[dict(triggers=count, actions=1)],
                       triggers=[(0, 4, 1, 0, 0, 0, 0, 0), (0, 4, 1, 0, 1, 0, 0, 0)],
                       actions=[(0, 5, 4, 1, 0, 0, 0, 0)])
        add('chain', **records)
        add('entry', **records)
    # (b) The Berserk read returns aiRuntime[1] & 0x200 raw; the chain folds raw
    # ints with `xor 1` negation. [orig: @0x453B85..0x453B99; @0x454084..0x4540C5]
    folds = [
        [(0, 7, 18, 0, 0, 0, 0, 0)],
        [(1, 7, 18, 0, 0, 0, 0, 0)],
        [(0, 7, 18, 0, 0, 0, 0, 0), (0, 4, 1, 0, 0, 0, 0, 0)],
        [(0, 4, 1, 0, 0, 0, 0, 0), (0, 7, 18, 0, 0, 0, 0, 0)],
        [(4, 7, 18, 0, 0, 0, 0, 0), (0, 4, 1, 0, 0, 0, 0, 0)],
        [(2, 7, 18, 0, 0, 0, 0, 0), (0, 4, 1, 0, 1, 0, 0, 0)],
    ]
    for bits in (0x200, 0x201, 0):
        for triggers in folds:
            add('chain', vars=[0], local_behavior=bits,
                events=[dict(triggers=len(triggers), actions=1)], triggers=triggers)
    # (c) SingleDestroyed (sub 4) / SingleAlive (sub 5) walk the pool rows by
    # the full DcbId dword and answer !(Flags & 2); SSN 0 and no row read not
    # alive. [orig: Entity_IsAliveByBmsRef @0x43E640]
    for ssn, rows in ((0, []), (101, []), (101, [(101, 0)]), (101, [(101, 2)]),
                      (101, [(0, 0)]), (101, [(7, 0), (101, 2), (101, 0)])):
        for sub in (4, 5):
            add('condition', rows=rows, triggers=[(0, 2, sub, ssn, 0, 0, 0, 0)])
    # (d) The linked-spawn marks after a fire, on the authority: the fired
    # event's linked records, chained backward through +535.
    # [orig: EventTrigger_MarkLinkedSpawnPoints @0x452CE0]
    for words in ([2, 2, 1, 0x8002], [0, 2, 2, 2], [3, 2, 2, 5]):
        for chain in ([1, 1, 0, 0], [0, 1, 1, 1], [1, 0, 1, 1]):
            # The third event fires, so its index (2) is what the scan matches.
            add('entry', authority=1, events=[dict(actions=0)] * 3,
                waypoints=list(zip(words, chain)), steps=1, entry_index=2)
    return out


def encode(root, v, snapshots):
    events = v.get('events', [{}])
    triggers = v.get('triggers', [(0, 4, 1, 0, 0, 0, 0, 0)])
    actions = v.get('actions', [(0, 5, 4, 0, 0, 0, 0, 0)])
    waypoints = v.get('waypoints', [])
    rows = v.get('rows', [])
    varz = v.get('vars', [0])
    row = [ROOT_IDS[root], v.get('steps', 1), v.get('entry_index', 0), v.get('authority', 0),
           v.get('input', 0), v.get('mirror', 0), v.get('local_behavior', -1), len(varz), *varz,
           len(rows)]
    for dcb, flags in rows:
        row += [dcb, flags]
    row.append(len(events))
    for e in events:
        row += [e.get('flags', 0), e.get('trigger_start', 0), e.get('action_start', 0),
                e.get('cooldown', 0) & 65535, e.get('reload', 0) & 65535,
                e.get('delay', 0) & 65535, e.get('delay_reload', 0) & 65535,
                e.get('active', 0), e.get('triggers', 0), e.get('actions', 1)]
    row.append(len(triggers))
    for t in triggers:
        row += list(t)
    row.append(len(actions))
    for a in actions:
        row += list(a)
    row.append(len(waypoints))
    for event, backchain in waypoints:
        row += [event, backchain]
    for s in snapshots:
        row += [s['result'], *s['events'], *s['vars'], s['input'], s['mirror'], *s['marks']]
    return [len(row) + 1] + row


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jo-c', type=Path, required=True)
    parser.add_argument('--retail-exe', type=Path, required=True)
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    retail = args.retail_exe.resolve()
    assert hashlib.sha256(retail.read_bytes()).hexdigest() == SHA256
    meo, Machine = load_harness(args.jo_c.resolve())
    meo.configure()
    machine = Machine(retail)

    lines = []
    cases = [(root, v, 'jo-c %d' % i) for i, (root, v) in enumerate(meo.cases())]
    cases += [(root, v, 'extended %d' % i) for i, (root, v) in enumerate(extended_cases())]
    for root, v, label in cases:
        row = encode(root, v, machine.run_case(root, v))
        lines.append(','.join(literal(x) for x in row) + ', // ' + root + ' ' + label + '\n')
    text = ('// Original JO x86 BMS event witnesses; regenerate with\n'
            '// scripts/oracles/mission_event_parity.py --jo-c <jo-c> --retail-exe <Jointops.exe> --write.\n'
            + ''.join(lines))
    path = Path(__file__).resolve().parents[2] / FIXTURE
    if args.write:
        path.write_text(text, encoding='utf-8')
    else:
        assert path.read_text(encoding='utf-8') == text, str(path) + ' differs'
    print(FIXTURE, len(lines), 'original-instruction cases passed')



if __name__ == '__main__':
    main()
