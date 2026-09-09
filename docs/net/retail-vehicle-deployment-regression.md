# Retail vehicle snapback after initial deployment

Investigation and fix, 2026-09-09. Owning protocol record:
[novaworld-net-re.md sections 5.10, 5.61 and 5.64](novaworld-net-re.md).

## Cause and fix

The initial deploy-map selection closed the local dialog without sending the
reliable C2S `0x0E` spawn request. Both `ClientRuntime::queue_deployment_pick`
and `JoinerConnection::prepare_deployment_pick` rejected selections after
initial admission reached `Complete`. That conflated completed admission with
completed deployment. Retail can admit an alive player while still holding
that player respawn-pending on a mission with selectable spawn zones.

The server kept the driver's undeployed bit set. Local vehicle prediction
advanced the buggy, while retail kept its vehicle stationary; subsequent
server updates pulled the client back. The corrected row and default-key
handlers queue the real selection. The runtime accepts it from the active
initial overlay, closes the gameplay gate, and uses the existing
ACK-qualified `0x5A` release path. Admission still permits an automatic spawn
without forcing a selection on every host.

SP and a listen host's own player bypass this remote admission exchange.
Host type and mission deployment policy matter: the failure also reproduced
against a locally hosted retail AAS game. Public matchmaking is not required
to trigger it.

## Matched retail comparison

Both runs used the retail executable hosting AAS with `lanmode=1`, the same
private fixture derived from AS - Dormant Volcano Isle, and the same buggy
(type 1291, wire handle `0x1013`). Only team-2 start-marker positions were
changed in the fixture to give the driver a repeatable approach. Both runs
confirmed control bone 1 before holding forward for approximately four
seconds; a passenger or gunner seat was not used for this comparison.

| Observation | Baseline | Fixed |
|---|---:|---:|
| Server driver flags before driving | `0x141` | `0x140` |
| Server vehicle displacement | 0.000 m | 31.071 m |
| Client final vehicle displacement | 1.233 m | 27.064 m |
| Client maximum displacement | 2.756 m | 27.064 m |
| Forward-input interval | 4.040 s | 4.026 s |

The server positions were read from the test process before and after the
client probe. The second server sample follows the probe's return and includes
brief continued motion; it is not simultaneous with the final client sample.
The server's stationary baseline and moving fixed vehicle establish the
behavioral difference. Both keyboard runs had zero analog bytes, isolating
this result from the separate analog correction below.

Both retail captures closed normally with zero dropped, truncated, or
write-error packets. Private observations remain in the ignored task workspace:
`.scratch/baseline3-compare.json`, `.scratch/patched4-compare.json`, and the
corresponding `retail-capture-baseline3` / `retail-capture-patched4` directories.
Temporary runtime instrumentation was removed. No public NovaWorld endpoint
was exercised in this matched comparison.

## Retail witnesses

- **Initial hold:** `Server_OnPlayerJoin @0x51A680` sets player-slot state bit
  `0x10` when the spawn-zone list is nonempty (site `@0x51A6F2`).
- **Per-frame hold:** `NetPacket_WritePlayerState @0x4FF6B0` exposes that slot
  bit as `0x0A flags1 & 2` (`@0x4FF7BD`) and reasserts entity `Flags & 1`
  (`@0x4FF7DD`). Alive health does not prove deployment is released.
- **Selection:** `DeathScreen_OnSpawnListSelect @0x553630` queues input case 12
  for any row whose node is not -1 (`@0x55364D`).
  `Input_HandleActionBinding @0x49AD40`, case 12 (`@0x49B0C5..0x49B17B`),
  resets dialogs and queues `0x0E`, including for an alive initial overlay.
- **Acceptance/release:** `Server_ProcessClientRequestRespawn @0x519AF0`
  accepts dead-or-pending players (`@0x519CC7`). The successful deployment leg
  in `Server_ProcessPlayerDeath @0x517740` clears the slot bit (`@0x517791`)
  and regrants the loadout and spawn seed.
- **Hidden-player body gate:** `Entity_UpdateInfantryPlayerBody @0x4B40E0`
  returns early on entity `Flags & 1` (`@0x4B411B`). The matched live result
  establishes the vehicle consequence; it does not identify a separate
  direct test of this bit inside the vehicle mover.

## Related paths checked

**Six-minute `t35` kick:** The same uncompleted deployment leaves retail's
join-idle timer armed. `Server_TickUpdate @0x51D7E0`, sites
`@0x51E0D4..0x51E13D`, requires a pending state-6 player and elapsed time
strictly greater than 360000 ms before emitting `t35`. A baseline session
received that kick. The existing native `npruntime_host_punt` test pins both
timer boundaries and proves that clearing pending prevents the kick beyond
the deadline. The longer fixed live session covered 360.25 seconds from
connection, which is not evidence of six minutes after state-6 entry.

**Analog bytes lost on send:** `build_player_uplink` omitted the entity's
three analog control bytes and left their wire fields zero. It now preserves
the signed byte patterns at body offsets 21..23, matching
`NetPacket_SerializePlayerState @0x4C09C0` case 3
(`@0x4C1B2E..0x4C1B74`) and case 4 (`@0x4C1E6A..0x4C1EA4`). A real
build/encode/host-apply regression preserves `(-64, 37, -128)`. This fixes the
wire omission; it does not establish complete joystick input support.

**Mount identity and packet pacing:** The joiner-role fixture now keeps an
independent authoritative vehicle, applies newly emitted C2S controls, and
feeds real framed S2C vehicle updates back into prediction. It drives under
both ordinary sending and a dictated twelve-tick period with six ticks of
latency in each direction. This guards the carrier/seat and control paths;
it does not emulate the retail pending-player physics gate.

**Remaining audit gap:** The uplink builder still supplies zero entity-interest
feedback pairs. Retail uses these pairs for replication priority. Their effect
on live replication was not established in this investigation, and they were
not changed or attributed as the snapback cause.

## Regression commands

Final native result: all eight focused suites passed. After building their
named native test targets:

```sh
ctest --test-dir build -C Release -R '^(netsim_build_player_uplink|vehicle_motor|vehicle_mount|npruntime_server_spawn|deploy_screen_feed|inmatch_joiner_role|npruntime_client_runtime|npruntime_host_punt)$' --output-on-failure
```

`npruntime_client_runtime` covers real initial selection, host dispatch and
release, including the twelve-tick boundary. Before the fix it failed with
`zones: the initial deploy-map selection queues a real C2S 0x0E`.
`netsim_build_player_uplink` failed before the analog fix with
`mounted throttle and steering reach authority through the real uplink`.

With a freshly built GDExtension and the configured retail menu fixtures:

```sh
"$GODOT_BIN" --headless --path godot -s addons/gut/gut_cmdln.gd -gtest=res://tests/deploy_screen_presenter_test.gd -gexit
```

The UI suite exercises real UDP host/joiner Simulations, initial row/default-key
selection, death re-picks and release. Check for script parse errors as well
as GUT totals: GUT can return zero after dropping a script. Final result:
8 tests passed, 282 assertions, no skipped or dropped script.
