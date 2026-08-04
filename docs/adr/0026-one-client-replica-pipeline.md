# ADR 0026: one client replica pipeline and one wire presenter

- **Status**: accepted (2026-08-01)
- **Updates**: [ADR 0009](0009-in-match-net-seam.md),
  [ADR 0011](0011-single-player-in-process-listen-server.md), and
  [ADR 0013](0013-consolidated-net-core.md)

## Context

Live play and replay/spectate used the same `npwire` decoders but built two
different client worlds. `NovaSimulation` folded S2C messages into
`NetClientView` and presented its flat snapshot through `WirePresentPass`.
`NovaNetClient` retained every decoded message, periodically rebuilt a
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
   live `ClientRuntime` adapter and `NovaNetClient` feed decoded server messages
   through it. It owns the current `ClientState`, including Person and Vehicle
   compact records, lifecycle, mounts, environment state, and transient
   animation edges.
2. **History is an optional projection, not another client world.**
   `ReplicaHistory` journals edges from the state produced by
   `ClientReplicaPipeline` and appends event/uplink records needed by replay
   tools. Live play does not pay for history. The batch `build_replay_timeline`
   function remains an offline capture-analysis utility; it is not a runtime
   spectator renderer.
3. **`WirePresentPass` is the sole decoded-entity presenter.** Live joiners and
   spectators expose the same `PF_*` snapshot contract and use the same spawn,
   model-resolution, transform, animation, visibility, and liveness path.
   `NetWorldView` is removed. Role-specific data may enrich a snapshot:
   `NovaSimulation` can resolve authoritative/local mission state, while a
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
5. **Unknown and ambiguous item definitions fail closed.** Missing callback
   mappings and duplicate definition/wire ids never guess an Infantry or
   Vehicle record width. The raw item fields and all four callback tags remain
   available for later retail evidence.
6. **NovaWorld is not a second entity stack.** `NovaWorldClient` owns
   matchmaking, lobby identity, and the handoff to an in-match endpoint.
   `npruntime`/`npwire` own gameplay replication. `NovaNetClient` is a
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
