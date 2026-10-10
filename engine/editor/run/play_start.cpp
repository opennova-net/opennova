#include <editor/run/play_start.h>

#include <cstdio>
#include <filesystem>
#include <system_error>
#include <vector>

#include <base/vfs/vfs.h>
#include <editor/project/project_files.h>
#include <editor/project_build/archive_routing.h>
#include <formats/mission/bms.h>
#include <formats/pff/pff.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

Diagnostic start_error(const std::string &words) {
	return make_finding(CoreFinding::PlayStart, DiagnosticSeverity::Error, words);
}

} // namespace

bool stage_play_start(const std::string &run_dir, const std::string &expansion, const std::string &mission,
		const mission::PlayerStart &start, PlayStartPlaced &out, Diagnostic &error) {
	out = PlayStartPlaced();
	// The boot table in its slot order, an expansion's pair first [orig: PFF_OpenAllArchives @ 0x4a4310]:
	// the first that holds the mission serves it.
	std::vector<std::string> slots;
	if (!expansion.empty()) {
		slots.push_back(vfs_expansion_archive_path(std::string(), expansion, true));
		slots.push_back(vfs_expansion_archive_path(std::string(), expansion, false));
	}
	for (const char *name : kBootArchiveTable) slots.push_back(name);
	pff::PffArchive archive{};
	const pff::PffEntry *found = nullptr;
	std::string slot;
	std::error_code ec;
	for (const std::string &each : slots) {
		const std::string path = join_path(run_dir, each);
		if (!fs::is_regular_file(system_path(path), ec)) continue;
		if (pff::pff_open(&archive, path.c_str()) != 0) continue;
		found = pff::pff_find(&archive, mission.c_str());
		if (found) {
			slot = each;
			break;
		}
		pff::pff_close(&archive);
	}
	if (!found) {
		error = start_error("Play from here: no archive of the run directory " + run_dir + " holds " + mission + ".");
		return false;
	}
	std::vector<uint8_t> bytes(found->size);
	bms::File file;
	std::string reason;
	const bool read = pff::pff_extract(&archive, found, bytes.data(), bytes.size()) == 0 &&
	                  bms::parse(bytes.data(), bytes.size(), file, reason);
	if (!read || !mission::place_player_start(file, start, out, reason) || !bms::write(file, bytes, reason)) {
		pff::pff_close(&archive);
		error = start_error("Play from here: " + mission + "'s start could not be placed" +
		                    (reason.empty() ? std::string(".") : ": " + reason));
		return false;
	}
	// The archive again, beside its name, every entry as it held it but the mission's (pff_rewrite_with_entry:
	// each entry's stored bytes as the source holds them, the mission's the new bytes, stored plain).
	const std::string target = join_path(run_dir, slot);
	const std::string written = target + ".start";
	const int wrote = pff::pff_rewrite_with_entry(&archive, uint32_t(found - archive.entries), bytes.data(),
	                                              uint32_t(bytes.size()), written.c_str());
	pff::pff_close(&archive);
	if (wrote != pff::PFF_WRITE_OK) {
		fs::remove(system_path(written), ec);
		error = start_error("Play from here: " + target + " could not be written (" + std::to_string(wrote) + ").");
		return false;
	}
	// Its name in the run directory, a link to the build's archive, removed (never written through), and
	// the new archive put in its place.
	fs::remove(system_path(target), ec);
	if (ec || !io::replace_file(written, target, reason)) {
		fs::remove(system_path(written), ec);
		error = start_error("Play from here: " + target + " could not be replaced: " + (reason.empty() ? ec.message() : reason));
		return false;
	}
	out.archive = slot;
	return true;
}

std::string play_start_words(const mission::PlayerStart &start) {
	char words[96];
	std::snprintf(words, sizeof(words), "(%.1f, %.1f, %.1f) facing %d", start.at[0], start.at[1], start.at[2],
	              int(mission::wrapped_yaw(start.yaw)));
	return words;
}

} // namespace opennova::editor
