# ADR 0012 — The player is a host-side server entity driven by a wire-shaped intent

Status: accepted

## Context

Companion to [ADR 0011](0011-single-player-in-process-listen-server.md) (SP = in-process listen
server). With SP network-shaped, a question follows: is the player a client-side avatar that the
present pass animates, or an authoritative simulation entity?

The original engine ([docs/net/novaworld-net-re.md §5.2a](../net/novaworld-net-re.md), witnessed
2026-06-16) is unambiguous: even in single-player the **host's own player is a server-side entity**.
`[orig: Server_InitNewRoundState @ 0x51c8e0]` → `[orig: CNapiServer_ProcessPendingPlayerSpawns @
0x4c8dc0]` → `[orig: Server_BuildPlayerInfoAndAdd @ 0x51d560]` builds the `GamePlayerEntity` in the
pools (stored at `CGameSession+4512`); the host's local client is just another entry in the
pending-spawn list. The player is type `0x14B9` (player infantry), health initialized from
`ItemDef+0x17C` (healthMax) into `entity+286` via `[orig: Entity_InitFromItemDef @ 0x49e550]`.

Player input is a **client→server uplink**, not a local control hack: `[orig: Player_BuildTag0CInputBody
@ 0x42A550]` serializes movement/aim/buttons into the C2S `0x0C` extended body (§5.10, 43 B) only
after three gates pass — entity buffer non-NULL, `entity+286` (healthMax) non-zero, and `entity+36`
bit 1 clear. The host receives that uplink and applies it to the owned entity authoritatively.

## Decision

1. **The player is an authoritative pool-0 `World` entity**, spawned by a reimpl of the §5.2a host
   spawn machine, never a wire-received or present-pass-only avatar. Its fields are shaped from the
   witnessed `GamePlayerEntity` model (health `entity+286`, team `entity+354`, mount/movement gates),
   so it is indistinguishable from any other simulated entity to WAC/BMS/AI.

2. **Player input is a wire-shaped intent.** Godot input produces a player-intent struct whose fields
   are exactly the C2S `0x0C` extended field map (§5.10) — movement/heading/pitch/anim/fire — gated by
   the witnessed `entity+286` / `entity+36` bit-1 checks (`Player_BuildTag0CInputBody @ 0x42A550`).
   Under SP the intent feeds the owned entity through the in-process loopback (ADR 0011); under MP the
   identical intent is the C2S `0x0C` uplink a remote client sends. **One input path serves both** — no
   bespoke "local control" path that MP would have to contradict.

3. **The FlyCamera is demoted to a debug camera.** Gameplay control binds to the player entity; the
   render camera follows it. The free camera remains available for the editor and debugging.

4. **Spawn side-effects are reproduced explicitly.** The receive-side spawn signal (compact player
   record `state flags & 2` → `[orig: Game_InitNewRound @ 0x422740]` + `[orig: Entity_ResetToSpawnState
   @ 0x4B9610]`, §5.10) and the host spawn-gate clear (the per-frame `0x0A` `flags1 & 0x01`, §5.2a) are
   driven through the loopback exactly as the original drives them, not assumed.

## Consequences

- Weapons/damage/death/combat-AI (this plan's Phase 3) all act on one authoritative entity model, so
  the player and NPCs share the same fire→hit→damage→death pipeline.
- The deferred MP serializer (Phase 4) is an encoder over the already-faithful player/entity/intent
  model, never a model rewrite — the guard ADR 0011 §4 mandates (the loopback identity test) covers it.
- `World::cached.local_player` is the existing hook for the player handle; the present pass resolves
  the player avatar through the same snapshot path as every other entity.
