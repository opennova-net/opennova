class_name WireHandle
extends RefCounted

# The wire entity-handle bit layout: the original entity pool rides the high
# nibble and the pool slot the low 12 bits — handle = pool << 12 | slot.
# This is the ONE GDScript home for that decode (libs/npwire owns it on the
# native side); inspectors, HUD feeds, and the wire present pass all read it
# here instead of re-deriving the shifts. Pools 0/1/2/3 =
# organic/item/building/marker. [orig: EntityPool_FindByNetId @ 0x4f0a20]

const POOL_SHIFT := 12
const POOL_MASK := 0xF
const SLOT_MASK := 0xFFF


static func pool(handle: int) -> int:
	return (handle >> POOL_SHIFT) & POOL_MASK


static func slot(handle: int) -> int:
	return handle & SLOT_MASK


## The debug label every inspector row uses: "pool/slot".
static func label(handle: int) -> String:
	return "%d/%d" % [pool(handle), slot(handle)]
