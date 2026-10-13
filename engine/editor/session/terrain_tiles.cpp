// The tile tools' batches (terrain_tiles.h, ADR 0046 S25 B2): the view's tile placement found, made where the
// mission has none, opened quietly and edited in the view's scope.
#include <editor/session/terrain_tiles.h>

#include <filesystem>
#include <memory>
#include <system_error>

#include <base/io/file_io.h>
#include <editor/assets/install_view.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/mission_document.h>
#include <editor/model/document_base.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/session/document_set.h>
#include <editor/session/editor_request.h>
#include <editor/session/file_chores.h>
#include <editor/session/session_core.h>
#include <editor/session/terrain_family.h>
#include <formats/mission/bms_edit.h>
#include <formats/til/til_io.h>
#include <runtime/terrain_query/terrain_field_build.h>

namespace opennova::editor {

namespace {

void refuse(SessionCore &core, const std::string &why, const std::string &at) {
	core.refuse_request(CoreFinding::TerrainStroke, why, at, DiagnosticSeverity::Error);
}

// The mission's environment, by which the game reads its terrain's configuration (D-TERRAIN-18).
std::string environment_of(const DocumentBase &view) {
	if (const auto *mission = dynamic_cast<const MissionDocument *>(&view))
		if (const MissionRow *row = mission->mission_row()) return mission::mission_info(row->native).environment;
	return std::string();
}

// The mission's .til made where the project has none, seeded with what the game draws for the mission now: its load
// reads <mission>.til, else the terrain's own polytrn_tileinfo [orig: PolyTrn_LoadTerrainConfig @ 0x60E6D2..0x60E6E5],
// each through the project's files and then the game install's (the game plays the project over its install: a
// shipped mission's own .til, the terrain's own), so the file made hides nothing it drew. Written in the original
// editor's form, one step of the file history. A file of that name on the disk the scan has not seen yet is taken as
// it stands (the game reads it). Refused, nothing made, where the install is set but does not open (what it holds for
// the mission cannot be read); with no install set the project's files alone are read, and the words say so. False,
// refused with why.
bool make_tiles(SessionCore &core, const DocumentBase &view, const TerrainFamily &family) {
	const std::string &path = family.tiles.path;
	const std::string target = join_path(core.paths().root, path);
	std::error_code exists_error;
	if (std::filesystem::exists(system_path(target), exists_error)) {
		core.update_files({path});
		return true;
	}
	const std::shared_ptr<const ProjectAssetSource> files = core.view().findings.assets;
	// The game install, opened at its first read.
	std::unique_ptr<InstallView> install;
	bool install_tried = false;
	std::string install_why;
	const std::string install_root = core.base_game();
	const terrain::TerrainFileReader read = [&](const std::string &name, std::vector<uint8_t> &out) {
		if (files && files->read(name, out)) return true;
		if (!install_tried) {
			install_tried = true;
			if (!install_root.empty() && core.view().project.document) {
				install = std::make_unique<InstallView>();
				if (!install->open(install_spec(install_root, *core.view().project.document), install_why)) install.reset();
			}
		}
		const InstallFile *served = install ? install->find(name) : nullptr;
		return served && install->read(*served, out);
	};
	TilFile file;
	std::vector<uint8_t> seed;
	std::string own = terrain::read_placed_tile_bytes(read, read, family.tiles.name, family.terrain_name,
	                                                  environment_of(view), seed);
	std::string why;
	if (!own.empty() && !load_til(seed.data(), seed.size(), file, why)) {
		file = TilFile();
		own.clear();
	}
	if (!install_root.empty() && install_tried && !install) {
		refuse(core, path + " is not made: the game install could not be read (" + install_why + "), so what it draws "
		                    "for the mission is unknown and a new file would hide it.",
		       view.path());
		return false;
	}
	std::vector<uint8_t> bytes;
	std::string error;
	if (!save_til(file, bytes, error)) {
		refuse(core, path + " could not be made: " + error + ".", view.path());
		return false;
	}
	if (!io::ensure_directory(utf8_of(path_of(target).parent_path()), error) ||
	    !io::write_file_atomic(target, bytes.data(), bytes.size(), error)) {
		refuse(core, path + " could not be made: " + error, view.path());
		return false;
	}
	std::string said = file.entries.empty()
	                           ? "New file " + path
	                           : "New file " + path + ", holding " + own + "'s " + std::to_string(file.entries.size()) +
	                                     " tiles (the game reads the mission's tiles in place of the terrain's)";
	if (install_root.empty()) said += " (no game install is set: only the project's files were read for its tiles)";
	core.note(said);
	core.update_files({path});
	core.chores().made(said, {path});
	return true;
}

// What Undo says of a batch of the tile tools.
std::string words_of(const std::vector<Edit> &edits, const std::string &file) {
	bool add = false, remove = false, move = false, place = false, turn = false;
	for (const Edit &edit : edits) {
		add = add || edit.operation == EditOperation::Add || edit.operation == EditOperation::Duplicate;
		remove = remove || edit.operation == EditOperation::Remove;
		move = move || edit.operation == EditOperation::Move;
		place = place || (edit.operation == EditOperation::Set && (edit.field == "x" || edit.field == "y"));
		turn = turn || (edit.operation == EditOperation::Set && edit.field == "orientation");
	}
	const std::string in = " in " + basename_of(file);
	if (add) return "Laid tiles" + in;
	if (remove) return "Removed tiles" + in;
	if (move) return "Reordered tiles" + in;
	if (place && turn) return "Turned tiles about their middle" + in;
	if (place) return "Moved tiles" + in;
	if (turn) return "Turned or flipped tiles" + in;
	return "Edited tiles" + in;
}

} // namespace

void serve_terrain_tiles_edit(SessionCore &core, const EditorRequest &request) {
	if (!core.view().project.open) return;
	DocumentSet &documents = core.documents();
	const DocumentBase *view = documents.document_for(request.terrain_view);
	if (!view) return refuse(core, "Open the mission or the terrain whose tiles these are first.", request.terrain_view);
	TerrainFamily family;
	if (!resolve_terrain_family(core.view(), *view, family)) return refuse(core, family.error, view->path());
	const TerrainFamilyFile &tiles = family.tiles;
	if (!tiles.refusal.empty()) return refuse(core, tiles.refusal, view->path());
	if (tiles.path.empty())
		return refuse(core, basename_of(view->path()) + " has no tile placement: its terrain names none.", view->path());
	if (!request.path.empty() && request.path != tiles.path)
		return refuse(core, basename_of(view->path()) + "'s tiles go to " + tiles.path + ", not " + request.path + ".",
		              view->path());
	const std::string view_path = view->path();
	// A batch of none (the Tiles tool's first look) on a placement not made yet makes nothing: the first edit does.
	if (tiles.missing && request.edits.empty()) {
		core.note(basename_of(tiles.path) + " is not made yet: the first tile laid makes it.");
		return;
	}
	if (tiles.missing && !make_tiles(core, *view, family)) return;
	DocumentBase *target = documents.open_quietly(tiles.path, "the tile tools in " + basename_of(view_path) + " edit it");
	if (!target) return refuse(core, "Could not open " + tiles.path + " to lay tiles in it.", view_path);
	if (request.edits.empty()) return;
	documents.apply_terrain_edits(*target, request.edits, view_path, words_of(request.edits, tiles.path));
}

} // namespace opennova::editor
