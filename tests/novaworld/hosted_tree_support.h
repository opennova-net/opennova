#pragma once

// Synthetic data set trees for the hosted tests (ADR 0064): a base tree packed
// as a retail install root is (resource.pff, localres.pff holding the
// deathmatch mission serve_test_support.h writes, language.pff holding the game
// text), an expansion tree (<code>.pff with a mission, <code>L.pff, version.txt),
// each written into a LocalObjectStore's trees/<id>/ and its row made ready
// (data_set_repository.h finish_tree) with the files' names and sizes and the
// missions listed from the archives the way the worker's finalize lists them
// (mission_catalog::archive_rows over the pair, the multiplayer rows). What
// hosted_local_e2e_test and nw_programs_e2e_test start real opennova-serve
// children over.

#include "hosted/data_set_files.h"
#include "hosted/data_set_repository.h"
#include "local_object_store.h"
#include "serve/serve_test_support.h"

#include <base/io/file_io.h>
#include <base/io/os_path.h>
#include <formats/pff/pff.h>
#include <runtime/mission/mission_catalog.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace hosted_trees {

namespace fs = std::filesystem;
namespace hosted = opennova::novaworld_server::hosted;

inline std::vector<uint8_t> bytes_of(const std::string &text) {
	return std::vector<uint8_t>(text.begin(), text.end());
}

// An archive at `path` holding `entries` (made from scratch by the format's own edit writer).
inline bool make_archive(const fs::path &path, const std::vector<opennova::pff::PffEditEntry> &entries) {
	bool rewritten = false;
	return opennova::pff::pff_apply_edits(path.u8string().c_str(), entries, {}, &rewritten) ==
			opennova::pff::PFF_WRITE_OK;
}

// The game text every synthetic tree carries (the NovaWorld strings and the game-type word
// opennova-serve reads).
inline std::vector<uint8_t> game_text() {
	return serve_test::gametext({
			{ "NovaWorld",
					{ { "STRNOVA07", "Jungle" }, { "STRNOVA08", "Desert" }, { "STRNOVA09", "Snow" },
							{ "STRNOVA10", "None" }, { "STRNOVA11", "Yes" }, { "STRNOVA12", "No" } } },
			{ "GateTypeAbbrev", { { "DM", "DM" } } },
	});
}

// A base tree in `dir`: resource.pff (a stub entry), localres.pff with one deathmatch mission
// `map_file` titled `map_name`, language.pff with the game text, and version.txt.
inline bool write_base_tree(const fs::path &dir, const char *map_file, const char *map_name,
		const std::string &version = "Synthetic Base, V1.0.0.0; English") {
	std::error_code ec;
	fs::create_directories(dir, ec);
	return make_archive(dir / "resource.pff", { { "stub.txt", bytes_of("resource"), 1100000000 } }) &&
			make_archive(dir / "localres.pff", { { map_file, serve_test::deathmatch_mission(map_name), 1100000000 } }) &&
			make_archive(dir / "language.pff", { { "gametext.bin", game_text(), 1100000000 } }) &&
			serve_test::write_text(dir / "version.txt", version + "\r\n");
}

// An expansion tree in `dir`: <code>.pff with one deathmatch mission, <code>L.pff (the game
// text again, so the pair titles nothing it lacks), and version.txt.
inline bool write_expansion_tree(const fs::path &dir, const std::string &code, const char *map_file,
		const char *map_name, const std::string &version = "Synthetic Expansion, V1.0.0.0; English") {
	std::error_code ec;
	fs::create_directories(dir, ec);
	return make_archive(dir / (code + ".pff"), { { map_file, serve_test::deathmatch_mission(map_name), 1100000000 } }) &&
			make_archive(dir / (code + "L.pff"), { { "gametext.bin", game_text(), 1100000000 } }) &&
			serve_test::write_text(dir / "version.txt", version + "\r\n");
}

// The tree's files as its manifest lists them: every regular file in `dir` (its canonical
// name for the kind) and its size.
inline std::vector<hosted::TreeFile> manifest_of(const fs::path &dir, hosted::DataSetKind kind,
		const std::string &code) {
	std::vector<hosted::TreeFile> out;
	std::error_code ec;
	for (const fs::directory_entry &entry : fs::directory_iterator(dir, ec)) {
		if (!entry.is_regular_file(ec)) continue;
		const std::string canonical =
				hosted::canonical_tree_file(kind, code, entry.path().filename().u8string());
		if (canonical.empty()) continue;
		hosted::TreeFile f;
		f.name = canonical;
		f.size = static_cast<uint64_t>(entry.file_size(ec));
		out.push_back(std::move(f));
	}
	return out;
}

// The tree's multiplayer missions as the worker's finalize lists them: the pair's archive
// rows (mission_catalog::archive_rows), the rows with a game mode.
inline std::vector<hosted::TreeMission> missions_of(const fs::path &dir, hosted::DataSetKind kind,
		const std::string &code) {
	std::vector<hosted::TreeMission> out;
	const hosted::MissionPair pair = hosted::mission_pair_of(kind, code);
	opennova::pff::PffArchive missions{};
	opennova::pff::PffArchive texts{};
	if (opennova::pff::pff_open(&missions, (dir / pair.missions).u8string().c_str()) != 0) return out;
	const bool have_texts = opennova::pff::pff_open(&texts, (dir / pair.texts).u8string().c_str()) == 0;
	std::vector<opennova::mission_catalog::Row> rows =
			opennova::mission_catalog::archive_rows(missions, have_texts ? &texts : nullptr);
	opennova::mission_catalog::sort_rows(rows);
	for (const opennova::mission_catalog::Row &row : rows) {
		if (row.game_mode == 0) continue;
		out.push_back({ row.file, row.title });
	}
	if (have_texts) opennova::pff::pff_close(&texts);
	opennova::pff::pff_close(&missions);
	return out;
}

// The first line of the tree's version.txt.
inline std::string version_of(const fs::path &dir) {
	std::string text = serve_test::read_text(dir / "version.txt");
	const size_t end = text.find_first_of("\r\n");
	return end == std::string::npos ? text : text.substr(0, end);
}

// A ready tree of data set `data_set_id` in `store`: its row made (uploading), the files
// written from `source` into trees/<id>/ (the row's id names the directory), the row
// finalized with the manifest, the version and the missions read from the archives. The
// tree's id, 0 on a failure.
inline int64_t seed_ready_tree(opennova::db::Database &db, opennova::novaworld_server::LocalObjectStore &store,
		int64_t data_set_id, hosted::DataSetKind kind, const std::string &code, const fs::path &source) {
	const int64_t id = hosted::create_tree(db, data_set_id, hosted::TreeState::Uploading, "test");
	if (id == 0) return 0;
	std::error_code ec;
	for (const fs::directory_entry &entry : fs::directory_iterator(source, ec)) {
		if (!entry.is_regular_file(ec)) continue;
		const std::string canonical =
				hosted::canonical_tree_file(kind, code, entry.path().filename().u8string());
		if (canonical.empty()) continue;
		std::string error;
		if (!store.put_file("trees/" + std::to_string(id) + "/" + canonical, entry.path().u8string(), error)) {
			std::fprintf(stderr, "  put_file %s: %s\n", canonical.c_str(), error.c_str());
			return 0;
		}
	}
	const fs::path dir = fs::u8path(store.path_of("trees/" + std::to_string(id)));
	hosted::TreeReady ready;
	ready.files = manifest_of(dir, kind, code);
	ready.missions = missions_of(dir, kind, code);
	ready.version_text = version_of(dir);
	for (const hosted::TreeFile &f : ready.files) ready.bytes += static_cast<int64_t>(f.size);
	if (!hosted::set_tree_state(db, id, hosted::TreeState::Uploading, hosted::TreeState::Finalizing) ||
			!hosted::finish_tree(db, id, hosted::TreeState::Finalizing, ready))
		return 0;
	return id;
}

// The seeded data set named `display_name` (migration 0011's rows), 0 for none.
inline int64_t seeded_data_set(opennova::db::Database &db, const char *display_name) {
	const auto rows = db.query("SELECT id FROM data_sets WHERE display_name = $1;",
			{ opennova::db::BindValue(display_name) });
	return rows.empty() ? 0 : rows.front().as_int(0).value_or(0);
}

} // namespace hosted_trees
