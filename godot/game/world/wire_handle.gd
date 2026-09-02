class_name WireHandle
extends RefCounted

# Thin re-export of the wire entity-handle bit layout: the original entity pool
# rides the high nibble and the pool slot the low 12 bits —
# handle = pool << 12 | slot. Pools 0/1/2/3/4 = organic/item/building/marker/
# effects. The engine home is engine/net/npwire wire_handle.h (bound as
# NetProtocol.WIRE_HANDLE_* + wire_handle_* statics), and the witness
# [orig: EntityPool_FindByNetId @ 0x4f0a20] lives there; inspectors, HUD feeds,
# and the wire present pass keep the WireHandle.* spelling instead of
# re-deriving the shifts.

const INVALID := NetProtocol.WIRE_HANDLE_INVALID       # "not found" / no-entity sentinel


static func pool(handle: int) -> int:
	return NetProtocol.wire_handle_pool(handle)


static func slot(handle: int) -> int:
	return NetProtocol.wire_handle_slot(handle)


## The debug label every inspector row uses: "pool/slot".
static func label(handle: int) -> String:
	return "%d/%d" % [pool(handle), slot(handle)]
