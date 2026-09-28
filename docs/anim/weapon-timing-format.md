# The weapon timing request and edits texts

`opennova-3di weapon timing <timing.txt> -o <edits.txt>` compiles a first-person
rig's authored action windows, the `weapon.def` entries that play its clips and
the eyes an author placed into the `weapon.def` keys each entry sets, and
measures every entry with the engine's own weapon FSM
(`engine/runtime/world/weapon_fsm.h`; [ADR 0047](../adr/0047-blender-3di-exporter.md)
decision 14). `opennova-3di weapon merge <weapon.def> <edits.txt> -o <out.def>`
sets those keys in a copy of a `weapon.def` and leaves every other byte as it
was. The Blender add-on (`tools/blender/opennova_3di`) writes the request; any
other front end may. No imported weapon or animation is an input.

## Conventions

- Both texts share the `.o3d` and `.o3a` tokenizer
  ([o3d-scene-format.md](../threedi/o3d-scene-format.md);
  `apps/threedi_cli/scene_text.h`): one record per line, fields separated by
  whitespace, `#` starting a comment at the start of a line or after
  whitespace, numbers finite. A record takes exactly its fields.
- An active time is seconds of clip: a channel steps one sixty-second of a
  clip second per logic tick [orig: `AnimChannel_InitFromData @ 0x410560`]. A
  recovery time and a shot period are seconds of logic time, 62.5 ticks a
  second [orig: `Game_MainLoop @ 0x52b630`].

## The request

The first record is `weapon_timing 2`; the others come in any order.

| Record | Fields | Meaning |
| --- | --- | --- |
| `action` | suffix, active, recovery | one authored action: its suffix, the time from its entry to the marked pose, and the recovery after it |
| `entry` | name, mode, period | a `weapon.def` entry that plays the clips: its name, its fire mode (`semi`, `auto` or `burst`) and the requested shot-to-shot period |
| `view` | `pos` or `tpos`, x, y, z | an eye the author placed: the hip view (`pos`) or the aimed one (`tpos`), in metres, the model's mission axes (x forward, y left, z up), relative to the model's origin |

- The suffixes are the weapon actions that have an anim slot of their own:
  `idle emptyidle fire recoil reload empty switchto switchfrom switchrank
  scopeup scopedown`, in any case. Each one's ANIM is that slot's key,
  `anim_wpn_<suffix>` and `anim_wpn_empty_idle` for `emptyidle` (the
  `weaponaction <suffix> <slot> <key>` rows `opennova-3di catalog` prints)
  [orig: `g_AnimStateNameTable @ 0x8135F0` 241..251]. `overheated` is refused: nothing ever queues it, and it has no
  slot of its own [orig: the heat-glow leg reads its row @ 0x5410A7..0x541108].
  An action is authored once; `fire` is required.
- DELAYSTART is `round(active * 62) + 1` for a positive active time and 0 for
  none: the entry tick shows frame 0 and the tick that drains the counter does
  not step the clip. DELAYEND is `round(recovery * 62.5)`.
- `fire` takes a recovery of 0: every entry's period solves the fire DELAYEND
  against the FSM, each in its own mode (a semi press waits for idle, a burst
  measures its within-burst rate). The FSM fires at most every 2 ticks auto or
  burst (1875 rounds a minute) and every 3 ticks semi (1250).
- `switchto` and `switchfrom` take `0 0`: the handlers pace themselves on their
  switch timer, and the rows get DELAYSTART 0, DELAYEND 2.
- The firing cycle runs through the recoil decision, so the recoil's delays
  are always set: a recoil nobody authored is a zero window whose ANIM the
  entry keeps. Held auto fire re-arms only in the recoil's last recovery tick
  (or on a zero-length recoil), so an auto entry refuses a recoil with an
  active phase and no recovery [orig: `WeaponAction_Recoil @ 0x542dd0`, the
  deferred refire @ 0x542e7f..0x542e9d].
- An entry name is 1 to 31 letters, digits, `_`, `-` or `.` (the def's name
  field [orig: `WeaponDefs_ParseLineCallback`, strncpy 32 @ 0x543737]), named
  once. Its mode must be what its FLAGS say (`merge` checks).
- An eye becomes the def's `pos` or `tpos` position as `-eye * 256`: the
  viewmodel stands at the eye plus the position over 256 in the view frame,
  which is the model's mission axes [orig:
  `Player_UpdateFirstPersonCamera @ 0x4dd380`; the parser's `* 256` @ 0x544770].

## The edits file

`timing` writes, and `merge` reads, only the keys to set: per entry its
`pos`/`tpos` positions when an eye was given, and per action its ANIM
(authored actions only), DELAYSTART and DELAYEND. It never names FUNCTION,
SOUNDSET, SOUNDSETEND, PARTICLE, PARTICLEUSERPOINT or FLAGS.

| Record | Fields | Meaning |
| --- | --- | --- |
| `weapon_edits` | `1` | the first record |
| `entry` | name, mode | the entry the following records edit, and the mode its timing was measured in |
| `pos`, `tpos` | x, y, z | the view position, in the def's own units |
| `action` | suffix, key, value | one key of one ACTION block: `anim` (an identifier), `delaystart` or `delayend` (whole ticks or `auto`) |

## Merging

`merge` walks the def the way the game reads it: lines split at a CR LF pair
and nowhere else, each line cut by the retail tokenizer (quotes optional, `//`
and `;` end a line) [orig: `File_ParseASCIIFile @ 0x53D810`,
`Terrain_TokenizeConfigLine @ 0x53CB60`], `weapon <name>` ... `end` entries
with `action <name>` ... `end` blocks inside.

- An entry is found by name in any case. `merge` refuses an entry the def
  does not hold, an entry with no `end` the game reads (a final line with no
  CR LF loses its last byte, so a closing `end` there reads `en`), an entry
  whose FLAGS fire in another mode than the edits were measured in (or fire
  both auto and burst), a def that ends a line with LF alone (the game reads
  that LF as a byte of the line), and an encrypted (`SCR`) def.
- `pos`/`tpos`: the first three values of each such line are rewritten and its
  rotation columns (the cant) kept; a line short of six values, which the game
  reads as none, gets zeros there; an entry with no such line gets one before
  its first ACTION block.
- ACTION keys are set in the action's live block. Each block of a name
  re-initializes the one row of that name, so of repeated blocks only the last
  counts and a key set in an earlier one would be lost [orig:
  `ActionDef_ParseScriptLine @ 0x4023c0`, found or new @0x4024a1, both
  re-initialized by the call @0x4024da to `ActionDef_InitDefaults @ 0x4022b0`].
  A value is rewritten in place, spacing and comments kept (a comment
  written against the value stays after the new one); `delay` is
  DELAYEND's alias. A key the block lacks is added before its END, spelled as
  the block spells its keys. An action with no block gets a new one after the
  entry's last block, with no FUNCTION: the suffix's own handler binds
  [orig: `Anim_InitActions @ 0x541fa0`, the default table @ 0x830B90].
- Every other byte is copied, line breaks and a trailing NUL included, and the
  result is read back through the engine's parser before it is written.
- A note says when an edited `tpos` sits on an entry whose FLAGS draw the
  SIGHTS card: the card replaces the model once aiming settles, so the aimed
  view shows only while the view eases in [orig:
  `Render_ProcessMainSceneFrame @ 0x5ca0f0`, @0x5ca299..0x5ca304].
- A note says when a block the edits key names a FUNCTION binding another
  handler than its suffix's own (EMPTYIDLE `wpn_std_idle`): the merge keeps
  the FUNCTION, and the slot runs that handler (D-WPN-1), not the suffix's
  one the timing measured [orig: `Anim_InitActions @ 0x541fa0`, the rewrite
  @ 0x542117..0x542139].

## The preview

`timing` prints one JSON object on stdout:

- `tick_rate` (62.5), `clip_ticks_per_second` (62), and `pos` / `tpos` (the
  def positions, when given).
- `entries`, one per `entry` in request order: `name`, `mode`, `cycle_ticks`
  and `rpm` (the measured shot-to-shot period), `ready_tick` (the first return
  to idle in the firing run, -1 when it never returns), `rows` (per action, in
  request order with a recoil nobody authored last: `action`, `anim`,
  `delaystart`, `delayend`, the keys the edits set, and `shown_s`, the last
  clip time the viewmodel shows of the action's clip in its own run before
  another clip replaces it; -1 for the idles, which are not run, and for a
  recoil nobody authored) and `events` (per FSM observation: `tick`,
  `action`, `scenario`, `kind` among `enter play active_end eject reload_ammo
  switch_complete ready shot`, and `clip_seconds`, the clip time on show).
  The firing run is scenario `fire` and is fire's and recoil's own run; every
  other authored action but the idles is run on its own from idle.
