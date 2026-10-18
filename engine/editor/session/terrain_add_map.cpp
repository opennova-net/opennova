// Add a surface map, a foliage map or a blend map (terrain_add_map.h, ADR 0046 S25 B4).
#include <editor/session/terrain_add_map.h>

#include <filesystem>

#include <base/io/file_io.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/terrain_document.h>
#include <editor/graph/rename_transaction.h>
#include <editor/import/terrain_import.h>
#include <editor/model/document_base.h>
#include <editor/project/project_files.h>
#include <editor/session/document_set.h>
#include <editor/session/file_chores.h>
#include <editor/session/import_controller.h>
#include <editor/session/session_core.h>
#include <editor/session/terrain_stroke.h>
#include <editor/session/terrain_family.h>
#include <editor/session/terrain_uses.h>
#include <formats/pcx/pcx_io.h>
#include <formats/png/png_encode.h>
#include <formats/tga/tga.h>
#include <formats/trn/charmap_legend.h>

namespace opennova::editor {

namespace {

// The sides a new map takes: the shipped terrains' own (their char maps 512 x 512, their foliage maps 256 x 256; the
// game reads any power of two to 1024 [orig: Terrain_GetSurfaceTypeAtPosition @ 0x6065C6; Foliage_SampleFoliageMapMask
// @ 0x606620]); a blend map the colour map's 1024, as the terrain import makes one.
constexpr int kCharMapSide = 512;
constexpr int kFoliageMapSide = 256;
constexpr int kBlendMapSide = 1024;

// The class every texel of a new char map holds: the game's own where a terrain names none [orig: Terrain_LoadCharmapPCX].
constexpr uint8_t kDefaultClass = 1;

// The .trn's field naming each plane's map, and the terrain set's key [orig: Terrain_ParseConfigCallback @ 0x60f330].
const char *trn_field(TerrainPlane plane) {
	switch (plane) {
	case TerrainPlane::Surface: return "polytrn_charmap";
	case TerrainPlane::Foliage: return "polytrn_foliagemap";
	case TerrainPlane::Blend: return "polytrn_detailblendmap";
	default: return "";
	}
}
const char *suffix(TerrainPlane plane) {
	switch (plane) {
	case TerrainPlane::Surface: return "_m.pcx";
	case TerrainPlane::Foliage: return "_f.pcx";
	case TerrainPlane::Blend: return "_d1.tga";
	default: return "";
	}
}

// The folder a project-relative path sits in ("" the top level).
std::string in_folder(const std::string &folder, const std::string &name) {
	return folder.empty() ? name : folder + "/" + name;
}

// The legend's palette as the shipped char maps hold it (each class its legend colour, the indices past the legend's
// white).
void legend_palette(uint8_t (&palette)[256][3]) {
	for (int i = 0; i < 256; ++i) {
		const CharmapLegendColour &c = i < kCharmapLegendCount ? kCharmapLegend[i] : kCharmapLegendRest;
		palette[i][0] = c.r;
		palette[i][1] = c.g;
		palette[i][2] = c.b;
	}
}

// The first detail layer everywhere, as the terrain import makes a blend map from no blendmap.
std::vector<uint8_t> first_layer(int side) {
	std::vector<uint8_t> pixels(size_t(side) * size_t(side) * 4, 0);
	for (size_t i = 0; i < pixels.size(); i += 4) {
		pixels[i] = 255;
		pixels[i + 3] = 255;
	}
	return pixels;
}

// A new map in the game's form (an authored terrain's): an 8-bit PCX of one class or code, or a 32-bit TGA.
bool game_form(TerrainPlane plane, std::vector<uint8_t> &bytes, std::string &why) {
	if (plane == TerrainPlane::Blend)
		return tga::tga_write_rgba32(first_layer(kBlendMapSide).data(), kBlendMapSide, kBlendMapSide, bytes, why);
	IndexedImage8 image;
	image.width = image.height = plane == TerrainPlane::Surface ? kCharMapSide : kFoliageMapSide;
	image.indices.assign(size_t(image.width) * size_t(image.height), plane == TerrainPlane::Surface ? kDefaultClass : 0);
	if (plane == TerrainPlane::Surface) {
		legend_palette(image.palette);
	} else {
		for (int i = 0; i < 256; ++i) image.palette[i][0] = image.palette[i][1] = image.palette[i][2] = uint8_t(i);
	}
	return encode_pcx_indexed(image, bytes, why);
}

// A new source image of an imported terrain's set: the surface map a palette PNG of the legend's colours (its indices
// the classes), the foliage map an 8-bit grey PNG (its levels the codes), the blend map an RGBA PNG of the import's
// output (`seed`, its weights; empty: the first layer everywhere).
std::vector<uint8_t> source_form(TerrainPlane plane, const RgbaImage *seed) {
	if (plane == TerrainPlane::Surface) {
		std::vector<uint8_t> indices(size_t(kCharMapSide) * kCharMapSide, kDefaultClass);
		uint8_t palette[256][3];
		legend_palette(palette);
		return png::encode_png_indexed(indices.data(), kCharMapSide, kCharMapSide, &palette[0][0], 256);
	}
	if (plane == TerrainPlane::Foliage) {
		const std::vector<uint8_t> levels(size_t(kFoliageMapSide) * kFoliageMapSide, 0);
		return png::encode_png_gray(levels.data(), kFoliageMapSide, kFoliageMapSide);
	}
	if (seed && !seed->pixels.empty())
		return png::encode_png_rgba(seed->pixels.data(), uint32_t(seed->width), uint32_t(seed->height));
	return png::encode_png_rgba(first_layer(kBlendMapSide).data(), kBlendMapSide, kBlendMapSide);
}

bool write_new(SessionCore &core, const std::string &path, const std::vector<uint8_t> &bytes, std::string &error) {
	const std::string target = join_path(core.paths().root, path);
	return io::ensure_directory(utf8_of(path_of(target).parent_path()), error) &&
	       io::write_file_atomic(target, bytes.data(), bytes.size(), error);
}

// Whether a project-relative path holds a file on the disk (the scan may not have seen it yet).
bool on_disk(const SessionCore &core, const std::string &path) {
	std::error_code ec;
	return std::filesystem::is_regular_file(system_path(join_path(core.paths().root, path)), ec);
}

// An authored terrain's: the map made beside the .trn (or the project's file of its name kept, on the disk or in the
// scan), then the .trn's field naming it, an edit from the view. Two undo homes, which the words name: the view's Undo
// takes the name back, Undo file the file.
bool add_authored(SessionCore &core, const DocumentBase &view, const TerrainFamily &family, TerrainPlane plane) {
	const std::string view_path = view.path();
	const std::string name = family.terrain_name + suffix(plane);
	const std::string path = in_folder(folder_of_path(family.terrain), name);
	const AssetEntry *held = core.view().project.scan ? core.view().project.scan->find(name) : nullptr;
	if (!held && on_disk(core, path)) {
		core.update_files({path});
		held = core.view().project.scan ? core.view().project.scan->find(name) : nullptr;
	}
	if (!held) {
		std::vector<uint8_t> bytes;
		std::string error;
		if (!game_form(plane, bytes, error) || !write_new(core, path, bytes, error)) {
			refuse_stroke(core, path + " could not be made: " + error + ".", view_path);
			return false;
		}
		const std::string said = "New file " + path + ", " + terrain_plane_words(plane) + " of " +
		                         basename_of(family.terrain) + " (Undo file takes it back; the view's Undo its name)";
		core.note(said);
		core.update_files({path});
		core.chores().made(said, {path});
	}
	DocumentSet &documents = core.documents();
	DocumentBase *trn = documents.open_quietly(family.terrain, "Add names " + std::string(terrain_plane_words(plane)) + " in it");
	auto *terrain = dynamic_cast<TerrainDocument *>(trn);
	if (!terrain || terrain->rows().empty()) {
		refuse_stroke(core, "Could not open " + family.terrain + " to name " + terrain_plane_words(plane) + " in it.", view_path);
		return false;
	}
	Edit edit;
	edit.address = {terrain->rows().front()->id, node_kind(TerrainKind::Terrain), 0};
	edit.field = trn_field(plane);
	edit.value = name;
	return documents.apply_terrain_edits(*trn, {edit}, view_path,
	                                     "Named " + name + " as " + basename_of(family.terrain) + "'s " +
	                                             (terrain_plane_words(plane) + 4));
}

// An imported terrain's: the source image made beside the set (or one of its name already there taken as it stands)
// and the set's key written as an edit of its own text, one step of the file history that Undo file takes back whole
// (the set's bytes as they were, the image to the trash) and Redo file does again, the set imported again each way.
bool add_imported(SessionCore &core, const DocumentBase &view, const TerrainFamily &family, TerrainPlane plane,
                  const TerrainImport &import) {
	const std::string view_path = view.path();
	const std::string key = terrain_set_key(plane);
	std::string set_stem = basename_of(import.source);
	const size_t dot = set_stem.rfind('.');
	if (dot != std::string::npos) set_stem.resize(dot);
	const std::string name = set_stem + "_" + key + ".png";
	const std::string path = in_folder(folder_of_path(import.source), name);
	const AssetScan *scan = core.view().project.scan.get();
	const bool adopted = (scan && scan->at_path(path)) || on_disk(core, path);
	std::vector<uint8_t> set_bytes;
	std::string error;
	TerrainSet set;
	if (!io::read_file_bytes(join_path(core.paths().root, import.source), set_bytes, error) ||
	    !parse_terrain_set(set_bytes, set, error)) {
		refuse_stroke(core, import.source + " does not read: " + error + ".", view_path);
		return false;
	}
	std::vector<std::pair<std::string, std::vector<uint8_t>>> make;
	if (!adopted) {
		// The blend map seeded from the import's output (the weights the terrain draws now).
		RgbaImage seed;
		const TerrainFamilyFile &file = family.plane(plane);
		if (plane == TerrainPlane::Blend && !file.import_output.empty()) {
			std::vector<uint8_t> output;
			std::string why;
			if (!io::read_file_bytes(join_path(core.paths().root, file.import_output), output, why) ||
			    !decode_terrain_image("blendmap", basename_of(file.import_output), output, seed, why))
				seed = RgbaImage();
		}
		std::vector<uint8_t> bytes = source_form(plane, &seed);
		if (bytes.empty()) {
			refuse_stroke(core, path + " could not be made: the encoder refused it.", view_path);
			return false;
		}
		make.emplace_back(path, std::move(bytes));
	}
	const std::string words = std::string(adopted ? "Named " : "Made ") + name + " " + basename_of(import.source) + "'s " +
	                          key + " (" + (terrain_plane_words(plane) + 4) + ")";
	if (!core.chores().write_files(words, {{import.source, terrain_set_with_key(set_bytes, key, name)}}, make)) return false;
	core.note(words + ": importing the set again makes the map. Undo file takes it back.");
	return true;
}

} // namespace

bool add_terrain_map(SessionCore &core, const DocumentBase &view, const TerrainFamily &family, TerrainPlane plane) {
	const std::string view_path = view.path();
	if (plane != TerrainPlane::Surface && plane != TerrainPlane::Foliage && plane != TerrainPlane::Blend) {
		refuse_stroke(core, std::string("Add makes a surface, foliage or blend map: ") + terrain_plane_words(plane) +
		                     " every terrain has of its own (the game reads no terrain without them).",
		       view_path);
		return false;
	}
	const TerrainFamilyFile &file = family.plane(plane);
	if (!file.addable) {
		refuse_stroke(core, file.resolved() ? basename_of(view_path) + "'s terrain has " + terrain_plane_words(plane) +
		                                       " already: " + basename_of(file.path) + "."
		                             : file.refusal,
		       view_path);
		return false;
	}
	const TerrainImport import = terrain_uses(core.view(), family.terrain).import;
	return import.imported ? add_imported(core, view, family, plane, import) : add_authored(core, view, family, plane);
}

} // namespace opennova::editor
