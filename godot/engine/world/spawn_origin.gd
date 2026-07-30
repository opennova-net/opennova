class_name SpawnOrigin
extends RefCounted

## The ONE GDScript home for the spawn-origin provenance word decode:
## (kind << 24) | (record index & 0xFFFFFF); NONE = no provenance.
## C++ twin: libs/world entity.h spawn_origin_pack/kind/index (packed by
## mission promotion; consumed by the present passes).

const KIND_SHIFT := 24
const KIND_MASK := 0xFF
const INDEX_MASK := 0xFFFFFF
const NONE := 0xFFFFFFFF


static func pack(kind: int, index: int) -> int:
	return ((kind & KIND_MASK) << KIND_SHIFT) | (index & INDEX_MASK)


static func kind(origin: int) -> int:
	return (origin >> KIND_SHIFT) & KIND_MASK


static func index(origin: int) -> int:
	return origin & INDEX_MASK
