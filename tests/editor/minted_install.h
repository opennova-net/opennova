#pragma once
// A synthetic packed install for editor integration tests. Files deliberately cover
// menus, symbols, sound dependencies, terrain, missions, and expansion mounting.
// No dependency on authored content: no repository holds OpenNova's game (ADR 0067).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <base/gameprofile/required_resources.h>
#include <base/io/strutil.h>
#include <base/resource_index/file_kind.h>
#include <base/vfs/vfs.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/blank/blank_factory.h>
#include <editor/documents/document_types.h>
#include <editor/project_build/archive_routing.h>
#include <formats/dds/dds.h>
#include <formats/def/def.h>
#include <formats/def/def_write.h>
#include <formats/lwf/lwf.h>
#include <formats/lwf/wav_pcm.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission.h>
#include <formats/pff/pff.h>
#include <formats/tga/tga.h>
#include <formats/trn/trn_io.h>

#include "common/file_io.h"
#include "common/test_paths.h"

namespace minted_install {

namespace fs = std::filesystem;
using opennova::editor::AssetKind;

// A file's bytes, shared: the scaled install's fillers of one extension are one buffer (scale_to).
using Bytes = std::shared_ptr<const std::vector<uint8_t>>;

inline Bytes share(std::vector<uint8_t> bytes) {
	return std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
}

// A file of the game: its name as the game reads it (a packed name), its bytes.
struct File {
	std::string name;
	Bytes bytes;
};

// The synthetic expansion the install carries.
inline constexpr const char *kExpansion = "nifty";
// The mission the editor's end-to-end scripts run on (scripts/mcp/editor_mission_e2e.py, editor_terrain_e2e.py; their
// CI job passes --mission onjo_e2e.bms), and the item its one entity is (items.def's row, drawn by test.3di).
inline constexpr const char *kE2eMission = "onjo_e2e";
inline constexpr int kE2eItem = 107001; // an id the engine keeps for no kind (formats/def/reserved_items)

inline std::string repo() {
	return std::string(test_paths_repo_root(__FILE__));
}

// A name's extension, lower case, with its dot ("" for none).
inline std::string extension_of(const std::string &name) {
	const size_t dot = name.find_last_of('.');
	return dot == std::string::npos ? std::string() : opennova::strutil::to_lower(name.substr(dot));
}

// A small install corpus made by our writers and focused menu fixtures.
inline void put(std::vector<File> &out, const std::string &name, std::vector<uint8_t> bytes) {
	const std::string key = opennova::pff::normalized_logical_name(name);
	for (File &file : out) if (opennova::pff::normalized_logical_name(file.name) == key) {
		file.bytes = share(std::move(bytes));
		return;
	}
	out.push_back({name, share(std::move(bytes))});
}
inline void text_file(std::vector<File> &out, const std::string &name, const std::string &text) {
	const std::string stored = opennova::strutil::normalized_crlf_line_ends(text);
	put(out, name, std::vector<uint8_t>(stored.begin(), stored.end()));
}
inline bool blank(std::vector<File> &out, const std::string &name, std::string &error,
                  const std::string &role = "",
                  const std::vector<std::pair<std::string, std::string>> &values = {}) {
	using namespace opennova::editor;
	BlankRequest request{name, role, "Synthetic install", values};
	std::vector<uint8_t> bytes;
	Diagnostic why;
	if (!make_blank(request, classify_asset(name, nullptr), bytes, why)) {
		error = name + ": " + why.message;
		return false;
	}
	// Empty definition tables are valid authoring output, but the retail VFS's
	// zero-byte extraction path cannot serve them. A comment keeps the table empty.
	if (bytes.empty()) {
		const std::string comment = "// Empty synthetic table.\r\n";
		bytes.assign(comment.begin(), comment.end());
	}
	put(out, name, std::move(bytes));
	return true;
}
inline bool fixture(std::vector<File> &out, const std::string &name, const std::string &path, std::string &error) {
	auto bytes = test_io::read_file(test_paths_fixture(path.c_str()));
	if (bytes.empty() || test_io::is_lfs_pointer(bytes)) {
		error = "Missing fixture " + path + " (pull fixtures/ from LFS)";
		return false;
	}
	put(out, name, std::move(bytes));
	return true;
}
inline bool manifest(std::vector<File> &out, bool expansion, std::string &error) {
	using namespace opennova::gameprofile;
	using namespace opennova::editor;
	for (int i = 0; i < gameprofile_required_resource_count(); ++i) {
		const RequiredResource &row = *gameprofile_required_resource_at(i);
		if (bool(row.flags & RES_F_EXPANSION) != expansion || (row.flags & RES_F_PLAYER_FILE)) continue;
		if (!expansion && (row.flags & RES_F_PATTERN)) continue;
		if (!find_blank_factory_for_role(row.role)) continue;
		const std::string name = expansion ? gameprofile_expansion_file_name(&row, kExpansion) : row.name;
		if (!blank(out, name, error, row.role)) return false;
	}
	return true;
}
inline bool sound_bank(std::vector<File> &out, const std::string &name, std::string &error) {
	opennova::lwf::File bank;
	bank.singles.push_back({"tone", 0, "tone.wav"});
	opennova::lwf::Multi set;
	set.name = "TEST_TONE";
	set.pitch_base = opennova::lwf::kPitchUnityQ16;
	opennova::lwf::Playlist list;
	list.falloff_radius = 100;
	opennova::lwf::Sndparm member;
	member.pitch_scaled = opennova::lwf::kPitchUnityQ16;
	member.volume = member.clamp_volume = 255;
	for (const char *trigger : {"TEST_TONE", "MOUSE_OVER", "CLICK_SELECT", "MOVER_COMBO", "CLICK_VALUE"}) {
		set.name = trigger;
		set.playlist_indices = {uint32_t(bank.playlists.size())};
		list.sndparm_indices = {uint32_t(bank.sndparms.size())};
		bank.multis.push_back(set);
		bank.playlists.push_back(list);
		bank.sndparms.push_back(member);
	}
	std::vector<uint8_t> bytes;
	if (!opennova::lwf::encode_lwf(bank, bytes, error)) return false;
	put(out, name, std::move(bytes));
	return true;
}
inline bool mission(std::vector<File> &out, const std::string &stem, std::string &error) {
	return blank(out, stem + ".bms", error, "", {{"terrain", "Tmap"}, {"environment", "testday"}}) &&
	       blank(out, stem + ".bin", error, opennova::editor::kBlankMissionTextRole) &&
	       blank(out, stem + ".til", error) && blank(out, stem + ".pcx", error);
}
// The base game's item table: the blank's Null marker, then the e2e mission's crate (an object test.3di draws), each
// through the engine's own writer.
inline bool item_table(std::vector<File> &out, std::string &error) {
	opennova::def::DefItemDef rows[2] = {};
	opennova::def::def_init_item(rows[0]);
	std::snprintf(rows[0].display_name, sizeof rows[0].display_name, "%s", "Null");
	rows[0].id = 100000;
	rows[0].type = opennova::def::DEF_ITEM_TYPE_MARKER;
	opennova::def::def_init_item(rows[1]);
	std::snprintf(rows[1].display_name, sizeof rows[1].display_name, "%s", "E2E Crate");
	std::snprintf(rows[1].graphic, sizeof rows[1].graphic, "%s", "test");
	rows[1].id = kE2eItem;
	rows[1].type = opennova::def::DEF_ITEM_TYPE_OBJECT;
	rows[1].hp = 100;
	opennova::def::DefItemsFile file{};
	file.entries = rows;
	file.count = 2;
	const opennova::def::DefWriteResult written = opennova::def::def_write_items(file);
	if (!written.ok()) {
		error = "items.def: " + written.diagnostics.front().message;
		return false;
	}
	text_file(out, "items.def", written.text);
	return true;
}
// The e2e scripts' mission: onjo_m1's blank (Tmap, testday) with one entity, the crate, the first record with a
// position: the mission run moves it 64 m east and Play must stand it where it was put; the terrain run raises the
// ground under it (a 20 m brush), paints beside it and stamps cell 3 of the tile set east of it. Tmap's ground is its
// four sectors about the origin (the .trn's ids 1..4 at rows and columns 3 and 4 from its -4 origin, 512 m each:
// -512..512 m both ways), so the crate stands west of the middle and every step's point stays on the ground.
inline bool e2e_mission(std::vector<File> &out, std::string &error) {
	if (!mission(out, kE2eMission, error)) return false;
	const std::string name = std::string(kE2eMission) + ".bms";
	const File *blank_bms = nullptr;
	for (const File &file : out)
		if (file.name == name) blank_bms = &file;
	opennova::bms::File bms;
	if (!blank_bms || !opennova::bms::parse(blank_bms->bytes->data(), blank_bms->bytes->size(), bms, error)) {
		error = name + ": " + (error.empty() ? std::string("the blank was not made") : error);
		return false;
	}
	opennova::mission::EntityTransform at;
	at.x = -300.0f;
	at.y = -100.0f;
	at.z = 30.0f;
	opennova::mission::add_entity(bms, opennova::mission::EntityKind::Item, kE2eItem, at);
	std::vector<uint8_t> bytes;
	if (!opennova::bms::write(bms, bytes, error)) return false;
	put(out, name, std::move(bytes));
	return true;
}
inline bool base_game(std::vector<File> &out, std::string &error) {
	out.clear();
	if (!manifest(out, false, error)) return false;
	for (const char *name : {"main.mnu", "sp.mnu", "mp.mnu", "options.mnu", "game.mnu", "player.mnu",
	                        "weapon.mnu", "pre.mnu", "cmap.mnu", "stat.mnu", "death.mnu", "vehicle.mnu",
	                        "menu_style.mns", "brand.mns", "menutxt.bin", "gametext.bin"})
		if (!fixture(out, name, std::string("game/") + name, error)) return false;
	for (const char *name : {"onmnu19.fnt", "onmnub23.fnt", "onttl40.fnt", "newarow1.tga", "onmnbg.tga",
	                        "tone.wav", "test.3di", "test.bad", "test.adm", "test_rst.bad", "test.ptl",
	                        "compring.tga", "TSDicon.tga", "cross01.tga", "testday.env", "testnight.env",
	                        "testdawn.env", "testdusk.env", "testfog.env", "Cloud01.pcx", "Cloud01b.pcx",
	                        "msun.3di", "fmoon4.3di", "mglare.3di", "oncredit.kda"})
		if (!blank(out, name, error)) return false;
	text_file(out, "overcast.def", "// Synthetic overcast defaults.\n");
	for (int i = 2; i <= 25; ++i) {
		char name[16];
		std::snprintf(name, sizeof name, "cross%02d.tga", i);
		if (!blank(out, name, error)) return false;
	}
	// This terrain fixture's dependencies use generated textures and models.
	for (const char *name : {"Tmap.trn", "Tmap.cpt", "Tmap_m.pcx", "Tmap_f.pcx"})
		if (!fixture(out, name, std::string("terrain/tmap/") + name, error)) return false;
	for (const char *name : {"mnml_c.tga", "mnml_dm.tga", "mnml_dc1.tga", "mnml_dc2.tga", "mnml_dc3.tga",
	                        "mnml_dmd.tga", "mnml_t.tga", "mnml_d1.tga", "bush1.3di", "bush2.3di"})
		if (!blank(out, name, error)) return false;
	// Terrain's color map has a fixed 1024-square reader contract.
	std::vector<uint8_t> color, pixels(1024 * 1024 * 4, 128);
	for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
	if (!opennova::tga::tga_write_rgb24(pixels.data(), 1024, 1024, color, error)) return false;
	put(out, "mnml_c.tga", std::move(color));
	// The tile set four 64-texel cells in a row (the terrain run stamps cell 3).
	std::vector<uint8_t> strip, cells(256 * 64 * 4, 96);
	for (size_t i = 3; i < cells.size(); i += 4) cells[i] = 255;
	if (!opennova::tga::tga_write_rgb24(cells.data(), 256, 64, strip, error)) return false;
	put(out, "mnml_t.tga", std::move(strip));
	text_file(out, "weapon.def", "weapon \"TEST_WEAPON\"\nround_type AT_NULL\nend\n");
	if (!item_table(out, error) || !sound_bank(out, "menu.lwf", error) || !sound_bank(out, "game.lwf", error) ||
	    !mission(out, "onjo_m1", error) || !e2e_mission(out, error)) return false;
	return true;
}
inline bool expansion_game(std::vector<File> &out, std::string &error) {
	out.clear();
	return manifest(out, true, error) && mission(out, "onjo_e1", error);
}

inline uint64_t total_bytes(const std::vector<File> &files) {
	uint64_t total = 0;
	for (const File &file : files) total += file.bytes->size();
	return total;
}

// `files` grown to `count` files of `bytes` bytes in all (a scale an install was witnessed at): copies of its own
// files, in its order and again, each under a name of its own ("x" and a number, the file's extension), a copy only
// of a file whose kind its extension alone gives (a mission, a menu, a model; never items.def, whose kind is its
// name's); a copy of a texture (.tga, .mdt, .dds) or a wave at the size that brings the whole to `bytes` (each
// texture a square of opaque grey, each wave silence, one buffer of each), every other copy its file's own bytes.
// False, with `error`, when nothing is left to copy or the textures alone pass `bytes`.
inline bool scale_to(std::vector<File> &files, size_t count, uint64_t bytes, std::string &error) {
	using namespace opennova::editor;
	files.erase(std::remove_if(files.begin(), files.end(), [](const File &file) {
		return route_asset(classify_asset(file.name, file.bytes.get())) == opennova::ArchiveSlot::None;
	}), files.end());
	const size_t base = files.size();
	std::vector<size_t> sources;
	for (size_t i = 0; i < base; ++i) {
		const std::string copy = "x0" + extension_of(files[i].name);
		const AssetKind kind = classify_asset(copy, files[i].bytes.get());
		const opennova::ArchiveSlot slot = route_asset(kind);
		// Grow the boot archives' document corpus. Loose banks are enumerated by
		// the install view separately and are not part of this archive export sweep.
		if (kind == classify_asset(files[i].name, files[i].bytes.get()) &&
		    slot != opennova::ArchiveSlot::None && slot != opennova::ArchiveSlot::Loose &&
		    (document_type_for(kind) || kind == AssetKind::Texture || kind == AssetKind::Font))
			sources.push_back(i);
	}
	if (sources.empty()) {
		error = "nothing to copy";
		return false;
	}
	std::map<std::string, size_t> names;
	for (const File &file : files) names[opennova::pff::normalized_logical_name(file.name)] = 1;
	const auto sized_texture = [](const std::string &extension) {
		return extension == ".tga" || extension == ".mdt" || extension == ".dds";
	};
	// The copies: which file each is of, which are sized.
	std::vector<std::pair<std::string, size_t>> copies;
	size_t textures = 0, waves = 0;
	uint64_t kept = total_bytes(files);
	for (size_t n = 0, next = 0; files.size() + copies.size() < count; ++n) {
		const size_t of = sources[next++ % sources.size()];
		const std::string extension = extension_of(files[of].name);
		std::string name = "x";
		for (size_t v = n; ; v /= 36) {
			name += "0123456789abcdefghijklmnopqrstuvwxyz"[v % 36];
			if (v < 36) break;
		}
		name += extension;
		if (names.count(opennova::pff::normalized_logical_name(name))) continue;
		copies.push_back({name, of});
		if (sized_texture(extension)) ++textures;
		else if (extension == ".wav") ++waves;
		else kept += files[of].bytes->size();
	}
	if (kept >= bytes || (textures == 0 && waves == 0)) {
		error = "the copies' own bytes pass the scale";
		return false;
	}
	// The sized copies share what is left, the textures a square side a share, the waves the rest.
	const uint64_t left = bytes - kept;
	const uint64_t share_each = left / (textures + waves);
	uint32_t side = 1;
	while (uint64_t(side + 1) * (side + 1) * 4 + 148 <= share_each) ++side;
	const std::vector<uint8_t> grey(size_t(side) * side * 4, 128);
	std::vector<uint8_t> tga, dds;
	std::string why;
	if (textures && (!opennova::tga::tga_write_rgba32(grey.data(), side, side, tga, why) ||
	                 !opennova::dds::dds_write_a8r8g8b8(grey.data(), side, side, dds, why))) {
		error = "a texture of side " + std::to_string(side) + ": " + why;
		return false;
	}
	uint64_t texture_bytes = 0;
	for (const auto &copy : copies) {
		const std::string extension = extension_of(copy.first);
		if (sized_texture(extension)) texture_bytes += extension == ".dds" ? dds.size() : tga.size();
	}
	if (texture_bytes >= left) {
		error = "the textures pass the scale";
		return false;
	}
	std::vector<uint8_t> wave;
	if (waves) {
		const uint64_t each = (left - texture_bytes) / waves;
		const std::vector<uint8_t> silence(each > 44 ? size_t(each - 44) : size_t(1), 128);
		if (!opennova::lwf::wav_write_pcm_mono(silence.data(), silence.size(), 22050, 8, wave, why)) {
			error = "a wave of " + std::to_string(silence.size()) + " samples: " + why;
			return false;
		}
	}
	const Bytes tga_bytes = share(std::move(tga)), dds_bytes = share(std::move(dds)), wave_bytes = share(std::move(wave));
	for (const auto &copy : copies) {
		const std::string extension = extension_of(copy.first);
		Bytes made = files[copy.second].bytes;
		if (extension == ".dds") made = dds_bytes;
		else if (sized_texture(extension)) made = tga_bytes;
		else if (extension == ".wav") made = wave_bytes;
		files.push_back({copy.first, made});
	}
	return true;
}

// The install at `dir` (made, its folders too): the base game's files `base` packed into the boot table's three
// archives by their kinds' slots (route_asset; the three always written, as a build writes them) and its loose kinds'
// beside them, the expansion `expansion`'s `expansion_files` into its pair and its folder (route_for_expansion: its
// language slot's kinds in <e>L.pff, the others in <e>.pff, its own table, version text and music banks loose in
// expansion/<e>/), each archive's entries in their names' order, each stamped PFF_NEW_ENTRY_TIMESTAMP as a build
// stamps them (the effect loaders skip an entry of 0, D-VFS-12), and JO:CA's UPDATE.WIZ beside them, naming its
// JOPTR.CMD (the install is Joint Operations', boot_policy.h folder_product: a folder with no wizard is OP: Acid
// Gambit's). A file no build packs (no kind the game reads) is left out. False, with `error`, when a file cannot be
// written.
inline bool write_install(const std::string &dir, const std::vector<File> &base, const std::string &expansion,
                          const std::vector<File> &expansion_files, std::string &error) {
	using namespace opennova::editor;
	namespace pff = opennova::pff;
	std::map<std::string, std::vector<const File *>> archives; // path under dir -> its files
	std::vector<std::pair<std::string, const File *>> loose;
	for (const char *archive : opennova::kBootArchiveTable) archives[archive];
	for (const File &file : base) {
		const opennova::ArchiveSlot slot = route_asset(classify_asset(file.name, file.bytes.get()));
		if (slot == opennova::ArchiveSlot::Loose) loose.push_back({file.name, &file});
		else if (slot != opennova::ArchiveSlot::None) archives[opennova::archive_slot_file_name(slot)].push_back(&file);
	}
	if (!expansion.empty()) {
		const std::string language = opennova::vfs_expansion_archive_path("", expansion, true);
		const std::string resources = opennova::vfs_expansion_archive_path("", expansion, false);
		archives[language];
		archives[resources];
		for (const File &file : expansion_files) {
			AssetEntry entry;
			entry.logical_name = file.name;
			entry.kind = classify_asset(file.name, file.bytes.get());
			switch (route_for_expansion(entry, expansion)) {
			case ExpansionPlace::LanguageArchive: archives[language].push_back(&file); break;
			case ExpansionPlace::Archive: archives[resources].push_back(&file); break;
			case ExpansionPlace::Folder:
				loose.push_back({opennova::vfs_expansion_dir("", expansion) + "/" + file.name, &file});
				break;
			case ExpansionPlace::RootOnly:
			case ExpansionPlace::None: break;
			}
		}
	}
	std::error_code ec;
	for (auto &[path, files] : archives) {
		std::sort(files.begin(), files.end(), [](const File *a, const File *b) {
			return pff::normalized_logical_name(a->name) < pff::normalized_logical_name(b->name);
		});
		std::vector<pff::PffWriteEntry> entries;
		for (const File *file : files)
			entries.push_back({file->name.c_str(), file->bytes->data(), uint32_t(file->bytes->size()), 0,
			                   pff::PFF_NEW_ENTRY_TIMESTAMP, 0});
		const std::string target = dir + "/" + path;
		fs::create_directories(fs::path(target).parent_path(), ec);
		const int rc = pff::pff_write_archive(target.c_str(), pff::PFF_FORMAT_PFF3, entries.data(), uint32_t(entries.size()));
		if (rc != pff::PFF_WRITE_OK) {
			error = target + ": " + pff::pff_write_error_string(rc);
			return false;
		}
	}
	for (const auto &[path, file] : loose)
		if (!test_io::write_file(dir + "/" + path, *file->bytes)) {
			error = dir + "/" + path + " cannot be written";
			return false;
		}
	if (!test_io::write_text(dir + "/UPDATE.WIZ", "[HEADER]\r\nCMD=JOPTR.CMD\r\nPRODUCT_LOGIN=JOTRqq\r\n")) {
		error = dir + "/UPDATE.WIZ cannot be written";
		return false;
	}
	return true;
}

// The install of the base game and its expansion at `dir`: what a test opens as the game install. False, with
// `error` (said on the error stream too), where it cannot be made.
inline bool mint(const std::string &dir, std::string &error) {
	std::vector<File> base, expansion;
	if (base_game(base, error) && expansion_game(expansion, error) && write_install(dir, base, kExpansion, expansion, error))
		return true;
	std::fprintf(stderr, "the minted install: %s\n", error.c_str());
	return false;
}

// The install of mint() with `extra` among the expansion's files: each served by the expansion's mount, in place of
// a base game's file of its name where one has it (a later layer's own copy, as an expansion patches a menu).
inline bool mint_with(const std::string &dir, const std::vector<File> &extra, std::string &error) {
	std::vector<File> base, expansion;
	if (base_game(base, error) && expansion_game(expansion, error)) {
		expansion.insert(expansion.end(), extra.begin(), extra.end());
		if (write_install(dir, base, kExpansion, expansion, error)) return true;
	}
	std::fprintf(stderr, "the minted install: %s\n", error.c_str());
	return false;
}

} // namespace minted_install
