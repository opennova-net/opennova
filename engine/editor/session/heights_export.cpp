#include <editor/session/heights_export.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <base/io/file_io.h>
#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/project_asset_source.h>
#include <editor/model/core_finding.h>
#include <editor/project/project_files.h>
#include <editor/session/session_core.h>
#include <editor/session/view/session_view.h>
#include <formats/cpt/cpt_io.h>
#include <formats/cpt/trngen/heightmap_depth.h>
#include <formats/cpt/trngen/terrain_bake.h>

namespace opennova::editor {

void export_heights(SessionCore &core, const EditorRequest &request) {
	const SessionView &view = core.view();
	const std::string &named = request.path.empty() ? view.documents.active : request.path;
	const auto refuse = [&](const std::string &message) { core.refuse_now(CoreFinding::HeightsExport, message, named); };
	std::string to;
	for (const auto &[key, value] : request.values) {
		if (key != "to") return refuse("An export_heights takes no value \"" + key + "\" (it takes to).");
		to = value;
	}
	if (!strutil::ends_with_icase(to, ".raw")) return refuse("Name the .raw the heights go to (values.to).");
	const std::filesystem::path root = path_of(core.paths().root);
	std::filesystem::path out = path_of(to);
	if (out.is_relative()) out = root / out;
	out = out.lexically_normal();
	if (io::path_within(out, root))
		return refuse("The heights cannot land in " + utf8_of(out) +
		              ": it is inside the project, and a push sends every file of its folder. Choose a file outside it.");
	if (named.empty()) return refuse("Name the .cpt whose heights to export (path).");
	const AssetEntry *file = core.project_file(named);
	if (!file || file->kind != AssetKind::TerrainPolyData) return refuse(basename_of(named) + " is no .cpt of the project.");
	const std::string shown = basename_of(file->relative_path);
	// The files by the game's flat names: another folder's file of the name is the one that reads, never this one's.
	const std::shared_ptr<const ProjectAssetSource> files = view.findings.assets;
	if (!files || files->path_of(file->logical_name) != file->relative_path)
		return refuse(file->relative_path + " is not the file its name reads: the project holds another " + shown + ".");
	std::vector<uint8_t> bytes;
	if (!files->read(file->logical_name, bytes)) return refuse(shown + " could not be read.");
	CptFile cpt;
	std::string why;
	if (!load_cpt(bytes.data(), bytes.size(), cpt, why)) return refuse(shown + " does not read as a .cpt: " + why + ".");
	const size_t texels = size_t(trngen::kDepthSide) * trngen::kDepthSide;
	if (cpt.depth_buffer.size() != texels)
		return refuse(shown + " holds " + std::to_string(cpt.depth_buffer.size()) + " heights, not the 1024 x 1024 the game reads.");
	const std::vector<uint8_t> raw = trngen::encode_heightmap_raw16(cpt.depth_buffer);
	if (!io::write_file_atomic(utf8_of(out), raw.data(), raw.size(), why))
		return refuse("The heights of " + shown + " were not written: " + why + ".");
	core.view().activity.status = "Wrote the heights of " + shown + " to " + utf8_of(out) + ".";
	core.touch(ViewConcern::Output);
}

} // namespace opennova::editor
