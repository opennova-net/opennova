extends GutTest

# Pins the bound NetProtocol wire-handle surface to the witnessed layout. The native
# twin (engine/net/npwire/wire_handle.h) static_asserts the same
# values, and engine/runtime/replication pins both against world::EntityHandle — this is the
# GDScript leg of that agreement. [orig: EntityPool_FindByNetId @ 0x4f0a20]


func test_witnessed_values() -> void:
	assert_eq(NetProtocol.WIRE_HANDLE_INVALID, 0xFFFF, "the not-found sentinel")


func test_pool_slot_decode() -> void:
	assert_eq(NetProtocol.wire_handle_pool(0x2123), 2, "pool = high nibble")
	assert_eq(NetProtocol.wire_handle_slot(0x2123), 0x123, "slot = low 12 bits")
	assert_eq(NetProtocol.wire_handle_pool(NetProtocol.WIRE_HANDLE_INVALID), 0xF,
			"the sentinel's pool nibble sits past the live pools")
