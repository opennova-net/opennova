#pragma once

#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <cstdint>

namespace godot {

// The owner-key grammar the present passes hand the effect world's owned
// groups and the ItemEffectDirector anchor registry (ADR 0043 d9). This is
// presentation naming, not engine law: three GUT files assert the
// spellings ('wreck:91:2', 'wreck:wire:4100:1', 'piece:7',
// 'throwable-move:7'), so the four forms live in ONE binding header the
// destruction and throwable presenters share.

// An attached wreck effect family (1 death, 2 fire, 3 other) on an entity
// with a simulation net id.
inline String wreck_owner_key(int p_net_id, int p_family) {
	return vformat("wreck:%d:%d", p_net_id, p_family);
}

// The same family on a runtime-only (wire-identified) wreck: siblings share
// a zero net id, so the packed wire handle is the distinguishing identity.
inline String wreck_wire_owner_key(int p_wire_handle, int p_family) {
	return vformat("wreck:wire:%d:%d", p_wire_handle, p_family);
}

// One death piece's trail group, by pool slot.
inline String piece_owner_key(int p_slot) {
	return vformat("piece:%d", p_slot);
}

// One flying round's effects_table tag-1 "move" group, by the sim's
// generation-qualified visual key.
inline String throwable_move_owner_key(int64_t p_key) {
	return vformat("throwable-move:%d", p_key);
}

} // namespace godot
