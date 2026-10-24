#!/usr/bin/env python3
"""The scenario `aim_at` step's look: the look_px that turns an actor onto a target.

A scenario step {"tick": t, "aim_at": "0x0019"} turns the actor onto that pool-0
entity with one look step that run_parity_topology.ps1 computes on each side from
that side's own reads (the actor's heading and position, the target's position)
and its own mouse sensitivity word S, as retail computes the look yaw:

- One tick's look_px dx turns the heading by round(dx * S / 32) units of 65536 BAM,
  rounding half up: Input_ProcessMouseAxisBindings @0x499680 takes sens = S << 11
  (@0x4996DD) and the axis value (dx * sens + 0x8000) >> 16 (@0x49972E) for an
  unscoped view (a scoped one divides sens by the zoom, @0x499714), and the look
  action, Input_HandleActionBinding_0 case 166, shifts it left 16 (@0x4E10A4) and
  subtracts it from the body's +0x10 heading (@0x4E10A7) and g_LocalPlayerLookYaw
  (@0x4E10AA): mouse right turns the heading negative. Both drivers write a
  look_px into that axis word past the main frame's mouse pump, and the look steps
  of one tick sum into one word first (onHook's tick.lookDx, ScriptedInput's
  take_look), so a tick's turn rounds once.
- The bearing onto the target is in the heading's own convention:
  trunc(atan2(dy, dx) * dbl_7C19D8) over the Position words' differences, as the AI
  target scan measures a candidate against its +0x10 heading (AI_FindBestTargetB
  @0x466F60: fpatan @0x4671A9, the scale @0x4671BB, the chopped fistp @0x4671C5,
  the difference with +0x10 @0x4671CD).
- S on retail is g_MouseSensitivity, the current profile record's +0x590 word,
  which the session start copies unclamped (Game_ApplySessionSettingsToGlobals
  @0x55161E; the [1, 0x1FF] clamp belongs to the mousescale adjust,
  @0x49B19B..0x49B1B9). The current record is g_PlayerProfiles + slot * 0x3C80
  (PlayerProfile_LoadAllFromDisk @0x54F670..0x54F682), the slot game.cfg's
  player_index, 0 when it lies outside 0..5 (@0x54F659..0x54F66B); player.sav holds
  the records after its 16-byte header (engine/formats/playersav/player_sav.h).
  player.sav holds five records, so retail's bound keeps a slot (5, jle @0x54F667)
  whose record lies past the array, and the session start then reads past it. The
  script refuses that slot on purpose (profile_mouse_sensitivity finds no record 5 in
  the file): a test instrument does not model the read past the array.

The look is the px whose rounded turn lands nearest the bearing (the inverse of the
half-up rounding above, a tie to the smaller |px|). onHook takes at most 32767 px
per step, so a larger look is split into same-tick steps, which sum to the one word.

The next aim's read checks that the previous look turned the actor (`turned`): case
166 subtracts the tick's whole turn from the heading in one store, so a look that
reached the body moves it by its turn, give or take the body's own drift over the
part (x9's revive RR: 10,034 BAM against a 279,183,360 BAM look), and a look the
dispatcher dropped moves it by that drift alone (the same cell's dead medic: -10,035
BAM against a -379,584,512 BAM look). The check asks for at least half the expected
turn, in its direction.

A steps_tick scenario's first aim resolves before its staging (R-LX-s29-1 (b')), its
actor's words from the setup pose as both drivers write them (`pose`): the Position
words trunc(v * 65536) and the +0x10 heading of mission yaw Y, the wrapped
trunc(((90 - Y) mod 360) * 2^32 / 360) (devtools::script_pose_from_mission, the
onHook bridge's ApplyPose of the same snapshot: a yaw-0 pose reads 0x40000000, x7's
revive pre-flight).

usage:
  scenario_aim.py look --heading-bam=H --actor=X,Y --target=X,Y --sensitivity=S
  scenario_aim.py pose --x=X --y=Y --yaw-deg=Y
  scenario_aim.py turned --before-bam=H0 --after-bam=H1 --expected-bam=HE
  scenario_aim.py profile-word --copy=DIR
Each prints one JSON object.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import struct
import sys
from pathlib import Path

# dbl_7C19D8, 2^32 / 2pi bit for bit (0x41C45F306DC9C883).
BAM_PER_RADIAN = struct.unpack("<d", bytes.fromhex("83C8C96D305FC441"))[0]
LOOK_PX_STEP_MAX = 32767   # onhook_play_input's look_px bound per step
PROFILE_HEADER_BYTES = 16
PROFILE_RECORD_BYTES = 0x3C80
MOUSE_SENSITIVITY_OFFSET = 0x590
PLAYER_INDEX_RE = re.compile(r'(?m)^[ \t]*player_index[ \t]*=[ \t]*"?(\d+)"?')


def wrap32(value: int) -> int:
    """The 32-bit two's-complement wrap of retail's register math."""
    return ((value + 0x80000000) & 0xFFFFFFFF) - 0x80000000


def look_turn_units(px: int, sensitivity: int) -> int:
    """The heading units one tick's look of `px` subtracts: (px * (S << 11) + 0x8000) >> 16,
    the 64-bit product shifted arithmetically (the low word of the unsigned shift)."""
    return wrap32((px * wrap32(sensitivity << 11) + 0x8000) >> 16)


def heading_after(heading_bam: int, px: int, sensitivity: int) -> int:
    """The +0x10 heading after one tick's look of `px`."""
    return wrap32(heading_bam - (look_turn_units(px, sensitivity) << 16))


def bearing_bam(dx: float, dy: float) -> int:
    """trunc(atan2(dy, dx) * dbl_7C19D8), wrapped to the signed word fistp stores."""
    return wrap32(int(math.atan2(dy, dx) * BAM_PER_RADIAN))


def aim_look(heading_bam: int, actor: tuple[float, float], target: tuple[float, float],
             sensitivity: int) -> dict:
    """The look that turns an actor at `actor` facing `heading_bam` onto `target`."""
    if sensitivity <= 0:
        raise ValueError(f"mouse sensitivity {sensitivity} turns no look (S must be positive)")
    dx, dy = target[0] - actor[0], target[1] - actor[1]
    if dx == 0 and dy == 0:
        raise ValueError("the target stands on the actor: no bearing")
    bearing = bearing_bam(dx, dy)
    want = wrap32(heading_bam - bearing) / 65536.0   # the turn to subtract, in units
    base = math.floor(want * 32 / sensitivity)
    px = min(range(base - 1, base + 3),
             key=lambda p: (abs(look_turn_units(p, sensitivity) - want), abs(p)))
    steps = []
    rest = px
    while abs(rest) > LOOK_PX_STEP_MAX:
        chunk = LOOK_PX_STEP_MAX if rest > 0 else -LOOK_PX_STEP_MAX
        steps.append(chunk)
        rest -= chunk
    steps.append(rest)
    after = heading_after(heading_bam, px, sensitivity)
    return {
        "look_px": px,
        "look_steps": steps,
        "turn_units": look_turn_units(px, sensitivity),
        "bearing_bam": bearing,
        "heading_bam": wrap32(heading_bam),
        "heading_after_bam": after,
        "residual_bam": wrap32(after - bearing),
    }


def pose_words(x: float, y: float, yaw_deg: float) -> dict:
    """A setup pose's Position words (x, y) and +0x10 heading as the drivers write them."""
    if not all(math.isfinite(v) for v in (x, y, yaw_deg)):
        raise ValueError("a setup pose's x, y and yaw must be finite")
    normalized = math.fmod(90.0 - yaw_deg, 360.0)
    if normalized < 0.0:
        normalized += 360.0
    heading = wrap32(int(normalized * (4294967296.0 / 360.0)) & 0xFFFFFFFF)
    return {"x_q16": int(math.trunc(x * 65536.0)), "y_q16": int(math.trunc(y * 65536.0)),
            "heading_bam": heading}


def look_turned(before_bam: int, after_bam: int, expected_after_bam: int) -> dict:
    """Whether a look turned the actor: the heading read after it moved at least half
    the look's expected turn, in its direction (a look of no turn passes)."""
    expected = wrap32(expected_after_bam - before_bam)
    turned = wrap32(after_bam - before_bam)
    ok = expected == 0 or ((turned > 0) == (expected > 0) and turned != 0 and
                           2 * abs(turned) >= abs(expected))
    return {"turned_bam": turned, "expected_bam": expected, "ok": ok}


def profile_slot(game_cfg_text: str) -> int:
    """The current profile record game.cfg's player_index names (0 outside 0..5)."""
    found = PLAYER_INDEX_RE.findall(game_cfg_text)
    if len(found) != 1:
        raise ValueError(f"game.cfg holds {len(found)} player_index lines, not one")
    slot = int(found[0])
    return slot if 0 <= slot <= 5 else 0


def profile_mouse_sensitivity(player_sav: bytes, slot: int) -> int:
    """The record's +0x590 word, the g_MouseSensitivity the session start copies. A slot
    whose record the file does not hold (retail's slot 5, past its five) is refused."""
    at = PROFILE_HEADER_BYTES + slot * PROFILE_RECORD_BYTES + MOUSE_SENSITIVITY_OFFSET
    if at + 4 > len(player_sav):
        raise ValueError(f"player.sav ({len(player_sav)} bytes) holds no record {slot}")
    return struct.unpack_from("<i", player_sav, at)[0]


def pair(text: str) -> tuple[float, float]:
    parts = text.split(",")
    if len(parts) != 2:
        raise argparse.ArgumentTypeError(f"expected X,Y, got {text!r}")
    return float(parts[0]), float(parts[1])


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = parser.add_subparsers(dest="command", required=True)
    look = sub.add_parser("look")
    look.add_argument("--heading-bam", type=int, required=True)
    look.add_argument("--actor", type=pair, required=True)
    look.add_argument("--target", type=pair, required=True)
    look.add_argument("--sensitivity", type=int, required=True)
    turned = sub.add_parser("turned")
    turned.add_argument("--before-bam", type=int, required=True)
    turned.add_argument("--after-bam", type=int, required=True)
    turned.add_argument("--expected-bam", type=int, required=True)
    word = sub.add_parser("profile-word")
    word.add_argument("--copy", type=Path, required=True)
    pose = sub.add_parser("pose")
    pose.add_argument("--x", type=float, required=True)
    pose.add_argument("--y", type=float, required=True)
    pose.add_argument("--yaw-deg", type=float, required=True)
    args = parser.parse_args(argv)
    if args.command == "look":
        out = aim_look(args.heading_bam, args.actor, args.target, args.sensitivity)
    elif args.command == "pose":
        out = pose_words(args.x, args.y, args.yaw_deg)
    elif args.command == "turned":
        out = look_turned(args.before_bam, args.after_bam, args.expected_bam)
    else:
        slot = profile_slot((args.copy / "game.cfg").read_bytes().decode("latin-1"))
        out = {"slot": slot,
               "mouse_sensitivity": profile_mouse_sensitivity(
                   (args.copy / "player.sav").read_bytes(), slot)}
    print(json.dumps(out))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
