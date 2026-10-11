#pragma once

// The items.def tables the capture decoders key on: a wire itemTypeId's display
// name (the pretty printer's "type=0x04bd[d_5ton truck]") and its compact dispatch
// class (the S2C 0x0A trailing-record walk needs it to size a record; an unmapped
// type halts the walker, fail closed). opennova-wire and opennova-parity both load
// them from the same JO items.def.

#include <net/npwire/entity_class.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace opennova::wire {

// Wire itemTypeId (items.def id - 100000) -> the §5.10b compact class.
using ItemClassMap = std::unordered_map<uint16_t, EntityClass>;

struct ItemTables {
	std::unordered_map<int, std::string> names; // wire itemTypeId -> display_name
	ItemClassMap classes;
};

// Load a JO items.def, plaintext or SCR-encrypted (the JO/DFX2 key), into `out`.
// Returns the count of items.def rows read, 0 on any failure (the reason goes to
// stderr).
size_t load_items_def(const char *path, ItemTables &out);

} // namespace opennova::wire
