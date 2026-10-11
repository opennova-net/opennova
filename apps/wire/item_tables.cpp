#include "item_tables.h"

#include <formats/def/def.h>
#include <formats/scr/scr.h>

#include <cstdio>
#include <fstream>
#include <vector>

using namespace opennova::def;
using namespace opennova::scr;

namespace opennova::wire {

// The SCR decryptor expects the SCR magic in the first 3 bytes; anything else is
// read as a plaintext .def.
size_t load_items_def(const char *path, ItemTables &out) {
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	if (!f) {
		std::fprintf(stderr, "items: failed to open %s\n", path);
		return 0;
	}
	const std::streamsize n = f.tellg();
	if (n <= 0)
		return 0;
	std::vector<uint8_t> raw(static_cast<size_t>(n));
	f.seekg(0);
	if (!f.read(reinterpret_cast<char *>(raw.data()), n))
		return 0;

	const uint8_t *plain = raw.data();
	size_t plain_size = raw.size();
	std::vector<uint8_t> decrypted;
	if (scr_is_scr(raw.data(), raw.size())) {
		decrypted.resize(raw.size());
		size_t out_size = decrypted.size();
		if (scr_decrypt_buf(raw.data(), raw.size(), decrypted.data(), &out_size, SCR_KEY_JO_DFX2) !=
				0) {
			std::fprintf(stderr, "items: SCR decrypt failed for %s\n", path);
			return 0;
		}
		decrypted.resize(out_size);
		plain = decrypted.data();
		plain_size = decrypted.size();
	}

	DefItemsFile items{};
	if (def_parse_items_memory(plain, plain_size, &items) != 0) {
		std::fprintf(stderr, "items: def_parse_items_memory failed\n");
		return 0;
	}
	// The wire `itemTypeId` is `items.def.id - 100000` (cross-witnessed
	// 2026-06-16: wire 0x050b=1291 ↔ items.def `id 101291` "Drivable Dune
	// Buggy", wire 0x14B9=5305 ↔ id 105305 "Player #1 (Multiplayer)",
	// wire 0x04b0=1200 ↔ id 101200 "Drivable Indonesian LCT"). Engine
	// loader presumably folds the 100000 offset out before storing the
	// runtime `g_ItemDefs[i].id` field that `ItemList_FindIndexByTypeId
	// @ 0x49E100` compares against.
	for (size_t i = 0; i < items.count; ++i) {
		const DefItemDef &it = items.entries[i];
		const int wire_id = it.id - 100000;
		// A wire type resolves to its FIRST items.def row; a later row repeating
		// the id is never reached [orig: ItemList_FindIndexByTypeId @0x49E100,
		// called by NapiNPClientMsg_0x00D @0x4332DA].
		if (wire_id >= 0 && wire_id < 0x10000 && out.names.find(wire_id) == out.names.end()) {
			out.names[wire_id] = it.display_name;
			// §5.10b: the engine reads ItemDef+356 to dispatch the per-entity
			// network-serialize callback; that field is seeded from one of the
			// 4 *_function class-tag directives at items.def load time. We
			// haven't IDA-witnessed which directive specifically — but the
			// player has `ai_function plyr` (matching the documented `plyr →
			// SerializePlayerState` callback), so ai_function is the primary
			// signal. Fall back to move_function for items that omit it.
			EntityClass cls = class_from_tag(it.ai_function);
			if (cls == EntityClass::Unknown)
				cls = class_from_tag(it.move_function);
			if (cls != EntityClass::Unknown)
				out.classes[uint16_t(wire_id)] = cls;
		}
	}
	const size_t loaded = items.count;
	def_free_items(&items);
	return loaded;
}

} // namespace opennova::wire
