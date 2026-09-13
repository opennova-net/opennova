# Mission savegames: reverse-engineering record

Date: 2026-09-13. Binary: retail `Jointops.exe`, imagebase `0x400000`.
This is a source-comparison record against the pinned jo-c reconstruction in the
[parity audit](../jo-c-parity-audit-2026-09-13.md), not a new IDA grill or a live
save/load result. The function names below are the curated IDB's (each address was
resolved with `lookup_funcs` on 2026-09-13); no implementation citation exists yet
because no OpenNova code implements this surface.

OpenNova persists options and player profiles, but has no implemented mission
snapshot/restore pipeline. Restarting a mission and reading a profile do not restore
an in-progress entity pool, AI state, script program, or objective state.

## Verdict and correspondence

| Surface | Original entry | OpenNova correspondence | Verdict |
|---|---|---|---|
| Header validation | `[orig: SaveFile_ReadAndValidateHeader @ 0x4ACDF0]` | No mission-save reader | unported; complete schema NEEDS-RE |
| Block framing | `[orig: SaveFile_BeginBlock @ 0x4A9FD0]` | No mission-save block writer | unported; framing and all callers need a joint witness |
| Entity serialization | `[orig: SaveFile_SerializeEntityToRecord @ 0x4AB130]` | No runtime entity snapshot writer | unported |
| Entity restoration | `[orig: SaveFile_ApplyEntityRecord @ 0x4ABB00]` | No runtime entity snapshot reader | unported; handle/reference reconstruction NEEDS-RE |
| AI component restore | `[orig: Entity_CopyVehicleDefToAIComp @ 0x45DB30]` (sole xref from `SaveFile_ApplyEntityRecord @ 0x4AC030`) | Live AI owners exist, no saved-state restoration | partial subsystem, unported restore path |
| Entity-pool flag storage | `[orig: SaveFile_ReadEntityPoolFlags @ 0x4A9DB0; SaveFile_WriteEntityPoolFlags @ 0x4AAA30]` | No pool-flag reader or writer | unported; the overlapping storage form NEEDS-RE |
| Save-slot enumeration | `[orig: SaveFile_CountSaveSlotFiles @ 0x4AB6E0]` | No retail mission-save slot flow | NEEDS-RE across UI and filesystem |
| Start/resume integration | `[orig: SinglePlayer_StartMission @ 0x561AF0]` | Ordinary mission startup exists | resume branch unported; full lifecycle NEEDS-RE |

jo-c's save-state candidate 217 and single-player candidate 230 provide bounded
original/linked comparisons and typed layouts. Their file, model, effect, UI and
filesystem services are explicit fixture boundaries. The entity-pool flag reader
and writer (`SaveFile_ReadEntityPoolFlags @ 0x4A9DB0`, `SaveFile_WriteEntityPoolFlags
@ 0x4AAA30`) retain unsupported overlapping storage of the 38400-by-24-byte table;
screenshot persistence is also a boundary. Neither candidate establishes that a
whole retail save can be restored into a running reconstructed mission. Those
original routines and their fixtures are starting evidence, not a ready-to-copy
application save service.

## Divergence catalog

| ID | Divergence | Disposition |
|---|---|---|
| D-SAVE-1 | Mission save/load, including retail slot/header/block schema, entity/AI state, reference repair and resume orchestration, has no OpenNova runtime owner. Existing profile/options writers do not implement this contract. | **NEEDS-RE**: the missing subsystem is established; complete serialization coverage, pool-flag storage and live restore semantics need witnesses before porting. |

## Implementation and acceptance

1. Inventory all save blocks and call sites from the original routines. Capture a
   minimal save and saves with destroyed sections, NPCs, occupied vehicles and
   advanced scripts. Identify version/checksum/size rules, optional data and
   ordering, including the unsupported pool flags and screenshots.
2. Define typed snapshots owned by the existing entity, AI, weapon, script and
   mission systems. Establish reference fixups and load order after correcting
   their shared runtime state; avoid serializing today's incomplete adapter state.
3. Restore entity/AI, ammunition, objectives, scripts, timers and the remaining
   witnessed state through those owners. Rebuild derived collision, effect and
   presentation state at the original lifecycle points.
4. Connect the authored save/load slot controls and the mission start/resume path.
   Pin missing, corrupt, incompatible and overwritten slot behavior to witnesses.
5. Load into a fresh process and compare the resumed mission's state and observable
   behavior. Where retail file compatibility is the target, validate retail-to-
   OpenNova and OpenNova-to-retail independently; a local roundtrip is insufficient.

No savegame test or full live save/load was executed in this audit. Future tests
must cover the production start/resume seam and original file fixtures; profile
serialization tests cannot close D-SAVE-1. The owning implementation phase is W08
in the [parity plan](../jo-c-parity-audit-2026-09-13.md).

## IDB changes

None. This audit reads the existing records and reconstruction manifests; the
function names above are the IDB's existing names.
