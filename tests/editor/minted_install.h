#pragma once
// A packed game install minted from OpenNova's own game (ADR 0048): the base game's project in assets/ and its
// expansion's in expansions/onjo1, each file packed where a build of the project puts it (the boot table's three
// archives by its kind's slot, archive_routing.h, through our own PFF writer; the kinds the game reads loose beside
// them; the expansion's pair and its folder's files under expansion/onjo1/), the layout the install view mounts as a
// stock launch does (install_view.h). What the editor's tests that open a game install read in place of a retail one.
//
// The files are the projects' own, each one the game reads (a project's file and its folders' alike, as the editor
// finds a file anywhere in it; its project file, its README and its cache are none): the plain files byte for byte,
// and those .gitattributes keeps in Git LFS (the models, clips, textures, normal maps, sounds and music banks: CI's
// test jobs pull fixtures/ alone, so there they are pointers) each a stand-in of its kind, the editor's blank of it
// (blank/blank_factory.h, through our own writers), so a run reads the same bytes whether LFS was pulled or not. An
// import source (an image or a terrain set under art/, beside its record) is no game file: the files its record lists
// as its outputs, which the editor's Build makes from it, are in its place, stood in the same way, a terrain set's
// terrain by terrain_outputs. Header-only, test support only.
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

#include <base/io/strutil.h>
#include <base/resource_index/file_kind.h>
#include <base/vfs/vfs.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/blank/blank_factory.h>
#include <editor/import/sidecar.h>
#include <editor/import/terrain_import.h>
#include <editor/project_build/archive_routing.h>
#include <formats/dds/dds.h>
#include <formats/lwf/wav_pcm.h>
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

// The expansion the install carries (expansions/onjo1, "Indigo Storm").
inline constexpr const char *kExpansion = "onjo1";

inline std::string repo() {
	return std::string(test_paths_repo_root(__FILE__));
}

// A name's extension, lower case, with its dot ("" for none).
inline std::string extension_of(const std::string &name) {
	const size_t dot = name.find_last_of('.');
	return dot == std::string::npos ? std::string() : opennova::strutil::to_lower(name.substr(dot));
}

// Whether .gitattributes keeps a file of assets/ or expansions/ in Git LFS: the models, clips, textures, normal
// maps, images, sounds and music banks (assets/**/*.3di ... *.wav), but the two textures the menus name, which it
// keeps plain (assets/newarow1.tga, assets/onmnbg.tga) so that the bundled-menu tests read them.
inline bool rides_lfs(const std::string &name) {
	static const char *const kLfs[] = {".3di", ".bad", ".dds", ".mdt", ".pcx", ".png", ".sbf", ".tga", ".wav"};
	const std::string lower = opennova::strutil::to_lower(name);
	if (lower == "newarow1.tga" || lower == "onmnbg.tga") return false;
	const std::string extension = extension_of(name);
	return std::any_of(std::begin(kLfs), std::end(kLfs), [&](const char *e) { return extension == e; });
}

// The stand-in for a file of `name`: the editor's blank of its kind, by its name (make_blank: a model of one
// triangle, a clip of one bone at rest, the game's missing-texture checkerboard in the name's format, a wave of
// one sample of silence, a tile placement of no tile), else the first factory of the kind (a music bank's).
inline bool stand_in(const std::string &name, std::vector<uint8_t> &out, std::string &error) {
	using namespace opennova::editor;
	const AssetKind kind = classify_asset(name, nullptr);
	BlankRequest request;
	request.logical_name = name;
	request.project_title = "OpenNova";
	Diagnostic why;
	if (make_blank(request, kind, out, why)) return true;
	for (size_t i = 0; i < blank_factory_count(); ++i) {
		const BlankFactory *factory = blank_factory_at(i);
		if (factory->kind == kind && make_from(*factory, request, out, why)) return true;
	}
	error = name + ": no blank of its kind (" + why.message + ")";
	return false;
}

// A terrain set's outputs (the terrain importer's, import/terrain_import.h, as its record lists them), stood in:
// the .trn naming the others as the import writes it (its names by the set's stem, the set's foliage blocks, the
// four quadrants of the atlas in the middle of an 8 by 8 grid from (-4, -4), the importer's island layout), the
// heights and ground mesh the minted Tmap terrain's (fixtures/terrain/tmap/Tmap.cpt, minimal_terrain_gen: a bake
// is far too slow for a stand-in), the colour, detail and blend maps the import makes where the set gives none
// (their sizes the game's), every other output its blank. False, with `error`, for a set that does not read or
// an output of no stand-in.
inline bool terrain_outputs(const std::string &set_path, const std::vector<std::string> &outputs, std::vector<File> &out,
                            std::string &error) {
	opennova::editor::TerrainSet set;
	std::string why;
	if (!opennova::editor::parse_terrain_set(test_io::read_file(set_path), set, why)) {
		error = set_path + ": " + why;
		return false;
	}
	std::string stem = fs::path(set_path).stem().string();
	const auto has = [&outputs](const std::string &name) {
		return std::find(outputs.begin(), outputs.end(), name) != outputs.end();
	};
	for (const std::string &name : outputs) {
		std::vector<uint8_t> bytes;
		const std::string extension = extension_of(name);
		if (extension == ".trn") {
			opennova::TrnConfig trn;
			trn.name = stem;
			trn.colormap = stem + "_c.tga";
			trn.detailmap = stem + "_dm.tga";
			trn.detailmap_c1 = trn.detailmap_c2 = trn.detailmap_c3 = stem + "_dt.tga";
			trn.detailblendmap = stem + "_d1.tga";
			trn.polydata = stem + ".cpt";
			trn.tileinfo = stem + ".til";
			if (has(stem + "_t.tga")) trn.tilestrip = stem + "_t.tga";
			if (has(stem + "_m.pcx")) trn.charmap = stem + "_m.pcx";
			if (has(stem + "_f.pcx")) trn.foliagemap = stem + "_f.pcx";
			trn.sector_count = trn.sector_rows = 8;
			trn.origin_x = trn.origin_y = -4;
			trn.sector_grid[3][3] = 1;
			trn.sector_grid[3][4] = 3;
			trn.sector_grid[4][3] = 2;
			trn.sector_grid[4][4] = 4;
			trn.foliage_defs = set.foliage;
			std::ostringstream text;
			if (!opennova::save_trn(text, trn, why)) {
				error = name + ": " + why;
				return false;
			}
			const std::string written = text.str();
			bytes.assign(written.begin(), written.end());
		} else if (name == stem + "_c.tga" || name == stem + "_dt.tga" || name == stem + "_dm.tga" || name == stem + "_d1.tga") {
			// The maps the import makes when the set gives none, at their sizes (the game's colour map is exactly 1024
			// a side [orig: PolyTrn_InitTextures @ 0x60B3A7]): the colour map grey, the detail and its coefficient
			// map 512 a side of mid grey, the blend map all red (the first splat layer everywhere).
			const bool blend = name == stem + "_d1.tga", detail = name == stem + "_dt.tga" || name == stem + "_dm.tga";
			const uint32_t side = detail ? 512 : 1024;
			std::vector<uint8_t> pixels(size_t(side) * side * 4, 128);
			for (size_t i = 0; i < pixels.size(); i += 4) {
				if (blend) pixels[i] = 255, pixels[i + 1] = pixels[i + 2] = 0;
				pixels[i + 3] = 255;
			}
			if (!(blend ? opennova::tga::tga_write_rgba32(pixels.data(), side, side, bytes, why)
			            : opennova::tga::tga_write_rgb24(pixels.data(), side, side, bytes, why))) {
				error = name + ": " + why;
				return false;
			}
		} else if (extension == ".cpt") {
			bytes = test_io::read_file(test_paths_fixture("terrain/tmap/Tmap.cpt"));
			if (bytes.empty() || test_io::is_lfs_pointer(bytes)) {
				error = "fixtures/terrain/tmap/Tmap.cpt does not read (git lfs pull fixtures/)";
				return false;
			}
		} else if (!stand_in(name, bytes, error)) {
			return false;
		}
		out.push_back({name, share(std::move(bytes))});
	}
	return true;
}

// The game files of the project at `root`: every file of it (its folders' too) but its project file, its README,
// its cache and export folders (.opennova/, build/) and its import sources with their records, by its last part;
// a file .gitattributes keeps in LFS as its stand-in, and an import record's outputs in its source's place (the
// image importer's textures their blanks, the terrain importer's terrain_outputs), in the order of the files'
// paths. False, with `error`, for two files of one name (the game reads one), a plain file that is an LFS pointer
// (an LFS rule rides_lfs does not know), or a file of no stand-in.
inline bool project_files(const std::string &root, std::vector<File> &out, std::string &error) {
	std::vector<fs::path> paths;
	std::error_code ec;
	for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
		const std::string part = it->path().filename().string();
		if (it->is_directory(ec)) {
			if (part == ".opennova" || part == "build") it.disable_recursion_pending();
			continue;
		}
		if (it->is_regular_file(ec)) paths.push_back(it->path());
	}
	if (ec) {
		error = root + ": " + ec.message();
		return false;
	}
	std::sort(paths.begin(), paths.end());
	// The import sources: each record's source and the other files its import reads (a terrain set's images).
	const std::string suffix = opennova::editor::kImportSidecarSuffix;
	std::vector<fs::path> sources;
	for (const fs::path &path : paths) {
		if (extension_of(path.filename().string()) != suffix) continue;
		const std::string source = path.string().substr(0, path.string().size() - suffix.size());
		sources.push_back(fs::path(source));
		opennova::editor::ImportSidecar record;
		opennova::editor::Diagnostic unread;
		if (opennova::editor::load_import_sidecar(path.string(), record, unread))
			for (const std::string &input : record.inputs) sources.push_back(fs::path(source).parent_path() / input);
	}
	const auto imported = [&sources](const fs::path &path) {
		return std::find(sources.begin(), sources.end(), path) != sources.end();
	};
	std::map<std::string, std::string> taken; // normalized name -> the path that gave it
	const auto add = [&](const std::string &name, Bytes bytes, const std::string &from) {
		const std::string key = opennova::pff::normalized_logical_name(name);
		if (!taken.emplace(key, from).second) {
			error = name + " twice: " + taken[key] + " and " + from;
			return false;
		}
		out.push_back({name, std::move(bytes)});
		return true;
	};
	for (const fs::path &path : paths) {
		const std::string name = path.filename().string();
		const std::string lower = opennova::strutil::to_lower(name);
		if (lower == "project.opennova" || extension_of(name) == ".md" || imported(path)) continue;
		if (extension_of(name) == suffix) {
			// An import record: the files its source makes, in its place.
			opennova::editor::ImportSidecar record;
			opennova::editor::Diagnostic unread;
			if (!opennova::editor::load_import_sidecar(path.string(), record, unread)) {
				error = path.string() + ": " + unread.message;
				return false;
			}
			const std::string source = path.string().substr(0, path.string().size() - suffix.size());
			std::vector<File> made;
			if (record.importer == opennova::editor::kTerrainImporterId) {
				if (!terrain_outputs(source, record.outputs, made, error)) return false;
			} else {
				for (const std::string &output : record.outputs) {
					std::vector<uint8_t> bytes;
					if (!stand_in(output, bytes, error)) return false;
					made.push_back({output, share(std::move(bytes))});
				}
			}
			for (File &file : made)
				if (!add(file.name, file.bytes, path.string())) return false;
			continue;
		}
		std::vector<uint8_t> bytes;
		if (rides_lfs(name)) {
			if (!stand_in(name, bytes, error)) return false;
		} else {
			bytes = test_io::read_file(path.string());
			if (test_io::is_lfs_pointer(bytes)) {
				error = path.string() + " is a Git LFS pointer: an LFS rule of .gitattributes minted_install.h does not know";
				return false;
			}
		}
		if (!add(name, share(std::move(bytes)), path.string())) return false;
	}
	return true;
}

// The base game's files (assets/) and the expansion's (expansions/onjo1).
inline bool base_game(std::vector<File> &out, std::string &error) {
	return project_files(repo() + "/assets", out, error);
}
inline bool expansion_game(std::vector<File> &out, std::string &error) {
	return project_files(repo() + "/expansions/" + kExpansion, out, error);
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
	const size_t base = files.size();
	std::vector<size_t> sources;
	for (size_t i = 0; i < base; ++i) {
		const std::string copy = "x0" + extension_of(files[i].name);
		if (classify_asset(copy, files[i].bytes.get()) == classify_asset(files[i].name, files[i].bytes.get()) &&
		    route_asset(classify_asset(copy, files[i].bytes.get())) != opennova::ArchiveSlot::None)
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
// stamps them (the effect loaders skip an entry of 0, D-VFS-12). A file no build packs (no kind the game reads) is
// left out. False, with `error`, when a file cannot be written.
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
