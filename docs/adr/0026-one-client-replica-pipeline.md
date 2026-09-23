# ADR 0026: one client replica pipeline and one wire presenter

- **Status**: accepted (2026-08-01); decision 5 amended 2026-09-23 (duplicated
  ids resolve to their first definition, as retail does)
- **Updates**: [ADR 0009](0009-in-match-net-seam.md),
  [ADR 0011](0011-single-player-in-process-listen-server.md), and
  [ADR 0013](0013-consolidated-net-core.md)

## Amendment (2026-08-05)

The replay/spectate consumer named throughout — `NovaWorldClient`,
`ReplicaHistory`, and the batch `build_replay_timeline` — was deleted as unused
dev scaffolding (it never shipped and no live path consumed it; the capture
DECODE chain that feeds parity tests is untouched). The decision itself is
unchanged and now reads simpler: `ClientReplicaPipeline` is the sole S2C entity
reducer and `EntityPresenter` its sole presenter (then `wire_present_pass.gd`;
native `godot/src/simulation/entity_presenter_wire.cpp` since the #460
rework), with the live joiner
(`ClientRuntime`) as the one consumer. Mentions of the replay consumer below
are the original decision text, left as written.

## Context

Live play and replay/spectate used the same `npwire` decoders but built two
different client worlds. `Simulation` folded S2C messages into
`NetClientView` and presented its flat snapshot through `EntityPresenter`.
`NovaWorldClient` retained every decoded message, periodically rebuilt a
`ReplayTimeline`, and rendered that model through `NetWorldView`. The two paths
could disagree about spawn identity, compact record widths, Person animation,
Vehicle orientation, lifecycle, and model resolution even when the wire bytes
were identical.

The old names also mixed independent concepts. In retail's S2C `0x0A` dispatch,
“Player”, “Infantry”, and “Vehicle” select record layouts. They do not form a
complete entity taxonomy and they do not answer who controls an entity. A human
Player controls a Person; that Person may occupy a seat and control a separate
Vehicle entity. AI can control a Person or a Vehicle without changing the
physical kind of either entity.

## Decision

1. **`ClientReplicaPipeline` is the sole runtime S2C entity reducer.** Both the
   live `ClientRuntime` adapter and `NovaWorldClient` feed decoded server messages
   through it. It owns the current `ClientState`, including Person and Vehicle
   compact records, lifecycle, mounts, environment state, and transient
   animation edges.
2. **History is an optional projection, not another client world.**
   `ReplicaHistory` journals edges from the state produced by
   `ClientReplicaPipeline` and appends event/uplink records needed by replay
   tools. Live play does not pay for history. The batch `build_replay_timeline`
   function remains an offline capture-analysis utility; it is not a runtime
   spectator renderer.
3. **`EntityPresenter` is the sole decoded-entity presenter.** Live joiners and
   spectators expose the same `PF_*` snapshot contract and use the same spawn,
   model-resolution, transform, animation, visibility, and liveness path.
   `NetWorldView` is removed. Role-specific data may enrich a snapshot:
   `Simulation` can resolve authoritative/local mission state, while a
   spectator has no local Person and reports that explicitly.
4. **`ItemReplicationCatalog` is the single immutable item classification.**
   It is built once from `items.def` and shared by host stamps and client record
   sizing. Its axes stay independent:
   - `WireCompactCodec` selects a wire body (`PlayerPerson`, `OrganicPerson`,
     `Vehicle`, none, or unresolved).
   - `MotionFamily` selects physical movement (`Person`, ground/light/water/air
     Vehicle, guided, static, or unresolved).
   - capabilities describe traits such as player control, AI data, emplaced
     weapon, or missile behavior.
   - `StoragePool` describes runtime allocation. D-NET-97 is not witnessed, so
     this axis remains unresolved rather than being inferred from BMS kind,
     item type, or a wire codec.
5. **Unknown item definitions fail closed; a duplicated id resolves to its
   first definition.** Missing or unwitnessed callback mappings never guess an
   Infantry or Vehicle record width. The raw item fields and all four callback
   tags remain available.

   *Amended 2026-09-23.* The original text also failed duplicate
   definition/wire ids closed, pending "later retail evidence". The evidence
   is in: retail resolves a type id on both sides with the same first-match
   scan, which returns the first `items.def` row carrying the id and 0 when
   none does `[orig: ItemList_FindIndexByTypeId @0x49E100 (the compare
   @0x49E120..0x49E122)]`. The host's entity carries that row's ItemDef
   `[orig: Entity_SpawnFromBMSRecord @0x40E9F0 (the index store @0x40EBFC, the
   ItemDef pointer @0x40EBFF..0x40EC07)]`, and the client's entity creation
   resolves the received wire type through the same scan and takes the index,
   the ItemDef and the row's callbacks from it `[orig: NapiNPClientMsg_0x00D
   @0x432C40 (the call @0x4332DA, the stores @0x4332DF..0x43331A)]`. A later
   row repeating an id is therefore unreachable and never selects a record
   width: `ItemReplicationCatalog` resolves both keys to the first definition
   and keeps the repeats listed in `issues()` (ctest
   `netsim_item_replication_catalog`). The shipped `ITEMS.DEF` repeats four
   ids (100508, 100415, 100439 and 102044; [itemdef record](../world/itemdef-re.md)).
6. **NovaWorld is not a second entity stack.** `NovaWorldClient` owns
   matchmaking, lobby identity, and the handoff to an in-match endpoint.
   `npruntime`/`npwire` own gameplay replication. `NovaWorldClient` is a
   capture/replay transport adapter over that same gameplay replica pipeline.

## Consequences

- A Person and a Vehicle take the same decode → replica → snapshot → presenter
  route. Family-specific bytes remain family-specific, but lifecycle and
  presentation cannot drift through parallel client models.
- “Infantry” is retained only where it names the historical wire compact. New
  domain code and documentation use Person plus an explicit controller kind.
- Replay history can be queried and scrubbed without retaining and refolding the
  complete message stream every publish interval.
- The convergence boundary is testable in portable C++: one test feeds Player
  Person, AI Person, and Vehicle records through the same incremental reducer
  and history; the existing per-family motion and animation tests stay on that
  reducer. Godot tests pin the common presenter contract.
- Pool allocation remains a documented evidence gap; convergence does not turn
  an unwitnessed inference into parity.
