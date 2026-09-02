extends GutTest

# Pins the GDScript wire-handle constants to the witnessed layout. The native
# twin (engine/net/npwire/wire_handle.h) static_asserts the same
# values, and engine/net/netsim pins both against world::EntityHandle — this is the
# GDScript leg of that agreement. [orig: EntityPool_FindByNetId @ 0x4f0a20]


func test_witnessed_values() -> void:
	assert_eq(WireHandle.INVALID, 0xFFFF, "the not-found sentinel")


func test_pool_slot_label_decode() -> void:
	assert_eq(WireHandle.pool(0x2123), 2, "pool = high nibble")
	assert_eq(WireHandle.slot(0x2123), 0x123, "slot = low 12 bits")
	assert_eq(WireHandle.label(0x2123), "2/291", "inspector label is pool/slot")
	assert_eq(WireHandle.pool(WireHandle.INVALID), 0xF,
			"the sentinel's pool nibble sits past the live pools")
