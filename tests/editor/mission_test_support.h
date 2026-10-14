#pragma once
// Mission helpers the editor tests share: a fixture's path and the minted synth_logic.bms, a mission
// document made from bytes, its rows and first records by kind, a batch applied saying why it was
// refused, words compared saying how they differ, and the Tmap island's class blocks and a one-tile
// .til placed over them.
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <editor/documents/document_types.h>
#include <editor/documents/mission_document.h>
#include <editor/model/document.h>
#include <formats/mission/bms.h>
#include <formats/til/til.h>
#include <formats/til/til_io.h>

#include "common/file_io.h"
#include "common/test_paths.h"

namespace mission_test {

// The minted mission the logic tests read (fixtures/bms/synth_logic.bms).
inline std::vector<uint8_t> fixture_bytes() { return test_io::read_file(test_paths_fixture("bms/synth_logic.bms")); }

// A mission document read from `bytes` as the file `name`; null, saying why, when it does not load.
inline std::unique_ptr<opennova::editor::Document> open(const std::vector<uint8_t> &bytes,
		const char *name = "synth_logic.bms") {
	using namespace opennova::editor;
	const DocumentType *type = document_type_for(AssetKind::Mission);
	if (!type) return nullptr;
	std::unique_ptr<Document> document = records_of(type->make());
	Diagnostic error;
	if (!document || !document->load_bytes(bytes, name, AssetKind::Mission, "jo", error)) {
		std::fprintf(stderr, "%s does not load: %s\n", name, error.message.c_str());
		return nullptr;
	}
	return document;
}

inline const opennova::editor::MissionDocument &as_mission(const opennova::editor::Document &document) {
	return dynamic_cast<const opennova::editor::MissionDocument &>(document);
}

// The row of `kind` at `index` among the mission's rows of that kind, and the first one (none: an
// empty address).
inline opennova::editor::NodeAddress row_at(const opennova::editor::Document &document,
		opennova::editor::MissionKind kind, size_t index) {
	const std::vector<const opennova::editor::Node *> rows = as_mission(document).rows_of(kind);
	return index < rows.size() ? opennova::editor::NodeAddress{rows[index]->id, rows[index]->kind, 0}
	                           : opennova::editor::NodeAddress();
}
inline opennova::editor::NodeAddress first_of(const opennova::editor::Document &document,
		opennova::editor::MissionKind kind) {
	return row_at(document, kind, 0);
}

// The first record of `kind` a row holds.
inline opennova::editor::NodeAddress first_child(const opennova::editor::Document &document,
		const opennova::editor::NodeAddress &row, opennova::editor::MissionKind kind) {
	const opennova::editor::NodeKind want = opennova::editor::node_kind(kind);
	for (const opennova::editor::Document::Collection &collection : document.collections_of(row))
		if (collection.spec.kind == want && !collection.ids.empty()) return {row.row, want, collection.ids[0]};
	return opennova::editor::NodeAddress();
}

// A batch applied as one step, printing why when the document refuses it.
inline bool apply_edits(opennova::editor::Document &document, const std::vector<opennova::editor::Edit> &edits) {
	opennova::editor::Diagnostic error;
	const bool ok = document.apply(edits, error);
	if (!ok) std::printf("  refused: %s\n", error.message.c_str());
	return ok;
}

// The words against those wanted, the two printed where they differ.
inline bool same(const std::string &got, const std::string &want) {
	if (got != want) std::printf("  got:  %s\n  want: %s\n", got.c_str(), want.c_str());
	return got == want;
}

// Class c's block's middle texel, mission metres (the island layout: texel (512 + x, 512 - y) of the
// 1024-unit atlas, a 256 map's texel four units), at the texel's middle.
inline void block_middle(int c, double &x, double &y) {
	const int tx = 32 + 32 * (c % 5), tz = 32 + 32 * (c / 5);
	x = 4.0 * tx - 512.0 + 2.0;
	y = 512.0 - 4.0 * tz - 2.0;
}

// A .til of one tile whose surface sampler square starts at (x, y), 16 units east and north (its stored z -y).
inline std::vector<uint8_t> tile_at(double x, double y, uint8_t index) {
	opennova::TilFile til;
	opennova::TilOverlayEntry entry;
	entry.x_fixed = opennova::bms::to_fixed_16_16(x);
	entry.z_fixed = -opennova::bms::to_fixed_16_16(y);
	entry.tile_index = index;
	til.entries.push_back(entry);
	std::vector<uint8_t> bytes;
	std::string error;
	if (!opennova::save_til(til, bytes, error)) std::fprintf(stderr, "the .til: %s\n", error.c_str());
	return bytes;
}

} // namespace mission_test
