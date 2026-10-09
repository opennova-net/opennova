#include <editor/run/play_start.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

#include <base/gameprofile/game_type.h>
#include <base/vfs/vfs.h>
#include <editor/project/project_files.h>
#include <editor/project_build/archive_routing.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <formats/pff/pff.h>
#include <runtime/world/spawn_select.h>

namespace fs = std::filesystem;

namespace opennova::editor {

namespace {

// Degrees in 0..359, as the file stores a yaw.
int16_t wrapped_yaw(double degrees) {
	const long whole = std::lround(degrees);
	const long turn = whole % 360;
	return int16_t(turn < 0 ? turn + 360 : turn);
}

bool holds(double metres) {
	return metres >= bms::kFixed16Min && metres <= bms::kFixed16Max;
}

// The 16-byte entry name as a string (it may fill all sixteen, no NUL after it).
std::string entry_name(const pff::PffEntry &entry) {
	return std::string(entry.filename, strnlen(entry.filename, sizeof(entry.filename)));
}

pff::PffFormat format_of(uint32_t magic) {
	if (magic == pff::PFF_MAGIC_PFF4) return pff::PFF_FORMAT_PFF4;
	if (magic == pff::PFF_MAGIC_BHD) return pff::PFF_FORMAT_BHD;
	return pff::PFF_FORMAT_PFF3;
}

// The archive written again: each entry's stored bytes as the source holds them (an encrypted one
// still encrypted, its flags, time and checksum kept), the mission's entry (`replaced`) the new bytes,
// stored plain.
struct Rewrite {
	const pff::PffArchive *source = nullptr;
	uint32_t replaced = 0;
	const std::vector<uint8_t> *bytes = nullptr;
};

int read_entry(void *ctx, uint32_t index, uint8_t *out, uint32_t size) {
	const Rewrite &rewrite = *static_cast<const Rewrite *>(ctx);
	if (index == rewrite.replaced) {
		if (size != rewrite.bytes->size()) return 1;
		if (size) std::memcpy(out, rewrite.bytes->data(), size);
		return 0;
	}
	return pff::pff_extract_raw(rewrite.source, &rewrite.source->entries[index], out, size);
}

Diagnostic start_error(const std::string &words) {
	return make_finding(CoreFinding::PlayStart, DiagnosticSeverity::Error, words);
}

} // namespace

bool place_player_start(bms::File &file, const PlayStart &start, PlayStartPlaced &out, std::string &error) {
	out = PlayStartPlaced();
	for (const double metres : start.at)
		if (!holds(metres)) {
			error = "The point is past what a mission's positions hold (32,768 m from its origin).";
			return false;
		}
	// The single player's chain under the mission's mode: the spawn joins it as team 1 [orig:
	// Server_PositionPlayerForSpawn @ 0x50CF60; runtime/mission's spawn_local_player_at_start].
	const uint32_t mode = game_type::for_mission_attribs(file.header.attrib_flags);
	const world::StartMarkerTypes types = world::start_marker_types(mode, 1);
	if (types.primary == 0) {
		error = "The mission's mode places its single player at no start marker.";
		return false;
	}
	const auto count_of = [&file](int32_t type) {
		size_t n = 0;
		for (const bms::Entity &marker : file.markers)
			if (marker.type_id == type) ++n;
		return n;
	};
	out.type = count_of(types.primary) != 0 || count_of(types.fallback) == 0 ? types.primary : types.fallback;
	const int16_t yaw = wrapped_yaw(start.yaw);
	const auto place = [&](bms::Entity &marker) {
		marker.x = bms::to_fixed_16_16(start.at[0]);
		marker.y = bms::to_fixed_16_16(start.at[1]);
		marker.z = bms::to_fixed_16_16(start.at[2]);
		marker.yaw = yaw;
		marker.pitch = 0;
		marker.roll = 0;
		// A team-2 start queues a mount onto its carrier as the player deploys [orig:
		// Server_PositionPlayerForSpawn @ 0x50D42E..0x50D45A]: the start of its own stands on foot.
		if (marker.team == 2) marker.team = 1;
	};
	for (bms::Entity &marker : file.markers)
		if (marker.type_id == out.type) {
			place(marker);
			++out.moved;
		}
	if (out.moved != 0) return true;
	// None of either: one of the primary type, at the point (bms_edit's new record, its SSN the next).
	mission::EntityTransform at;
	const size_t index = mission::add_entity(file, mission::EntityKind::Marker, out.type + mission::kItemIdOffset, at);
	place(file.markers[index]);
	out.moved = 1;
	out.added = true;
	return true;
}

bool stage_play_start(const std::string &run_dir, const std::string &expansion, const std::string &mission,
		const PlayStart &start, PlayStartPlaced &out, Diagnostic &error) {
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
	if (!read || !place_player_start(file, start, out, reason) || !bms::write(file, bytes, reason)) {
		pff::pff_close(&archive);
		error = start_error("Play from here: " + mission + "'s start could not be placed" +
		                    (reason.empty() ? std::string(".") : ": " + reason));
		return false;
	}
	// The archive again, beside its name, every entry as it held it but the mission's.
	std::vector<pff::PffWriteStreamEntry> entries(archive.entry_count);
	std::vector<std::string> names(archive.entry_count);
	Rewrite rewrite{&archive, uint32_t(found - archive.entries), &bytes};
	for (uint32_t i = 0; i < archive.entry_count; ++i) {
		const pff::PffEntry &entry = archive.entries[i];
		names[i] = entry_name(entry);
		entries[i].name = names[i].c_str();
		entries[i].size = i == rewrite.replaced ? uint32_t(bytes.size()) : entry.size;
		entries[i].flags = i == rewrite.replaced ? 0u : entry.flags;
		entries[i].timestamp = entry.timestamp;
		entries[i].checksum = entry.checksum;
	}
	const std::string target = join_path(run_dir, slot);
	const std::string written = target + ".start";
	const int wrote = pff::pff_write_archive_streamed(written.c_str(), format_of(archive.header.magic), entries.data(),
	                                                  uint32_t(entries.size()), read_entry, &rewrite);
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

std::string play_start_words(const PlayStart &start) {
	char words[96];
	std::snprintf(words, sizeof(words), "(%.1f, %.1f, %.1f) facing %d", start.at[0], start.at[1], start.at[2],
	              int(wrapped_yaw(start.yaw)));
	return words;
}

} // namespace opennova::editor
