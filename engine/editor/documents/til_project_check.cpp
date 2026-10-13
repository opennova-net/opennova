#include <editor/documents/til_project_check.h>

#include <set>
#include <string>
#include <utility>
#include <vector>

#include <base/vfs/file_stamps.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/project_check.h>
#include <editor/documents/texture_image.h>
#include <editor/documents/til_document.h>
#include <editor/documents/validation_cache.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/til/til.h>
#include <formats/til/til_io.h>
#include <formats/trn/trn_io.h>
#include <runtime/mission/mission_sidecars.h>
#include <runtime/terrain_query/terrain_field_build.h>

namespace opennova::editor {

namespace {

class TilProjectCheck final : public ProjectCheck {
public:
	bool update(const ProjectCheckInput &input) override {
		std::vector<std::string> missions;
		for (const AssetEntry &entry : input.validation.scan.entries)
			if (entry.kind == AssetKind::Mission) missions.push_back(entry.logical_name);
		if (made_ && missions == missions_ && !stamps_.moved(input.files)) return false;
		const std::vector<Diagnostic> before = std::move(findings_);
		findings_.clear();
		stamps_ = FileStamps();
		missions_ = missions;
		made_ = true;
		checked_.clear();
		for (const std::string &mission : missions) check_mission_(input, mission);
		if (findings_.size() != before.size()) return true;
		for (size_t i = 0; i < findings_.size(); ++i)
			if (findings_[i].code() != before[i].code() || findings_[i].asset != before[i].asset ||
			    findings_[i].message != before[i].message)
				return true;
		return false;
	}
	const std::vector<Diagnostic> &findings() const override { return findings_; }
	void clear() override {
		findings_.clear();
		stamps_ = FileStamps();
		missions_.clear();
		checked_.clear();
		made_ = false;
	}

private:
	// A file read through the project's files, its stamp kept: what moves the check.
	bool read_(const ProjectCheckInput &input, const std::string &name, std::vector<uint8_t> &out) {
		stamps_.note(name, input.files.stamp(name));
		return input.files.read(name, out);
	}

	std::string path_of_(const ProjectCheckInput &input, const std::string &name) const {
		const AssetEntry *entry = input.validation.scan.find(name);
		return entry ? entry->relative_path : name;
	}

	void check_mission_(const ProjectCheckInput &input, const std::string &mission) {
		std::vector<uint8_t> bytes;
		bms::File file;
		std::string error;
		if (!read_(input, mission, bytes) || !bms::parse(bytes.data(), bytes.size(), file, error)) return;
		const mission::MissionInfo info = mission::mission_info(file);
		if (info.terrain.empty()) return;
		const terrain::TerrainFileReader read = [&](const std::string &name, std::vector<uint8_t> &out) {
			return read_(input, name, out);
		};
		// The mission's own .til as the game's load takes it, and the terrain's own placement.
		const mission::Sidecar *role = mission::sidecar_for_role("tiles");
		const std::string own_name = role ? mission::sidecar_name(mission, *role) : std::string();
		std::vector<uint8_t> own_bytes;
		TilFile own;
		const bool own_read = !own_name.empty() && read(own_name, own_bytes) &&
		                      til_load_accepts(own_bytes.data(), own_bytes.size()) &&
		                      load_til(own_bytes.data(), own_bytes.size(), own, error);
		std::vector<uint8_t> terrain_bytes;
		const std::string terrain_name = terrain::read_placed_tile_bytes(read, read, std::string(), info.terrain,
		                                                                 info.environment, terrain_bytes);
		TilFile terrains;
		if (!terrain_name.empty() && !load_til(terrain_bytes.data(), terrain_bytes.size(), terrains, error)) terrains = TilFile();
		if (own_read && own.entries.empty() && !terrains.entries.empty())
			findings_.push_back(make_finding(finding_code(TilFinding::Empty), DiagnosticSeverity::Warning,
			                                 "It holds no tile, and the game reads it in place of " + terrain_name + "'s " +
			                                         std::to_string(terrains.entries.size()) +
			                                         " tiles: the mission draws none of them. Delete it to draw what the game "
			                                         "reads in its place (a " + own_name + " the game install ships, else "
			                                         "the terrain's tiles), or lay tiles in it.",
			                                 path_of_(input, own_name)));
		// The placement the mission draws and the tile set it draws from.
		const TilFile &drawn = own_read ? own : terrains;
		const std::string drawn_name = own_read ? own_name : terrain_name;
		if (drawn.entries.empty()) return;
		TrnConfig trn;
		const TrnTextReader read_text = [&](const std::string &name, std::string &text) {
			std::vector<uint8_t> raw;
			if (!read_(input, name, raw)) return false;
			text.assign(raw.begin(), raw.end());
			return true;
		};
		if (!read_mission_trn(read_text, info.terrain + ".trn", info.environment.empty() ? std::string() : info.environment + ".env",
		                      trn, error))
			return;
		const std::string set = trn_mission_tilestrip(trn, info.tile_set);
		if (set.empty() || !checked_.insert(drawn_name + "|" + set).second) return;
		std::vector<uint8_t> set_bytes;
		if (!read_(input, set, set_bytes)) return;
		const TextureHeader header = texture_header(set, set_bytes);
		if (!header.read) return;
		const TilAtlasLayout layout = til_make_atlas_layout(int(header.width), int(header.height));
		const int cells = layout.tiles_x > 0 && layout.tiles_y > 0 ? layout.tiles_x * layout.tiles_y : 0;
		size_t past = 0;
		for (const TilOverlayEntry &entry : drawn.entries) past += entry.tile_index >= cells ? 1 : 0;
		if (!past) return;
		const std::string sides = std::to_string(header.width) + " x " + std::to_string(header.height);
		findings_.push_back(make_finding(
		        finding_code(TilFinding::IndexPastAtlas), DiagnosticSeverity::Warning,
		        std::to_string(past) + (past == 1 ? " tile names a cell" : " tiles name cells") + " past the " +
		                std::to_string(cells) + " of " + set + " (" + sides + ", cut in 64-texel cells), which " +
		                mission + " draws its tiles from: the game draws " + (past == 1 ? "it" : "them") +
		                (cells ? " from the set's last row of texels, clamped" : ", the set too short for a row of cells") + ".",
		        path_of_(input, drawn_name)));
	}

	std::vector<Diagnostic> findings_;
	FileStamps stamps_;
	std::vector<std::string> missions_;
	std::set<std::string> checked_; // a placement and its set reported once
	bool made_ = false;
};

} // namespace

std::unique_ptr<ProjectCheck> make_til_project_check() { return std::make_unique<TilProjectCheck>(); }

} // namespace opennova::editor
