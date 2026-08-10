class_name SpawnOrigin
extends RefCounted

## Thin re-export of the spawn-origin provenance word decode:
## (kind << 24) | (record index & 0xFFFFFF); NONE = no provenance. The engine
## home is engine/runtime/world entity.h spawn_origin_pack/kind/index (bound as
## Simulation.SPAWN_ORIGIN_* + spawn_origin_* statics; packed by mission
## promotion, consumed by the present passes).

const NONE := Simulation.SPAWN_ORIGIN_NONE
const KIND_NONE := Simulation.SPAWN_ORIGIN_KIND_NONE
const INDEX_NONE := Simulation.SPAWN_ORIGIN_INDEX_NONE


static func pack(kind: int, index: int) -> int:
	return Simulation.spawn_origin_pack(kind, index)


static func kind(origin: int) -> int:
	return Simulation.spawn_origin_kind(origin)


static func index(origin: int) -> int:
	return Simulation.spawn_origin_index(origin)
