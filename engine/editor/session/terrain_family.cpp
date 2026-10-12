#include <editor/session/terrain_family.h>

#include <iterator>
#include <mutex>

#include <base/io/strutil.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/terrain_document.h>
#include <editor/model/document_base.h>
#include <editor/project/project_files.h>
#include <editor/session/terrain_uses.h>
#include <editor/session/view/session_view.h>
#include <formats/mission/bms_edit.h>
#include <formats/trn/trn_io.h>
#include <runtime/mission/mission_sidecars.h>
#include <runtime/terrain_query/terrain_field_build.h>

namespace opennova::editor {

namespace {

// The name without its extension.
std::string stem(const std::string &name) {
	const size_t dot = name.rfind('.');
	return dot == std::string::npos ? name : name.substr(0, dot);
}

// The key a terrain set names each plane's image by (S20's terrain_import keys; the blend map's is the round's).
const char *set_key(TerrainPlane plane) {
	switch (plane) {
	case TerrainPlane::Heights: return "heightmap";
	case TerrainPlane::Surface: return "surface";
	case TerrainPlane::Foliage: return "foliagemap";
	case TerrainPlane::Colour: return "colormap";
	case TerrainPlane::Blend: return "blendmap";
	case TerrainPlane::kCount: break;
	}
	return "";
}

// The file the .trn names for each plane [orig: Terrain_ParseConfigCallback @ 0x60f330: polytrn_polydata,
// polytrn_charmap, polytrn_foliagemap, polytrn_colormap, polytrn_detailblendmap].
const std::string &plane_name(const TrnConfig &trn, TerrainPlane plane) {
	static const std::string kNone;
	switch (plane) {
	case TerrainPlane::Heights: return trn.polydata;
	case TerrainPlane::Surface: return trn.charmap;
	case TerrainPlane::Foliage: return trn.foliagemap;
	case TerrainPlane::Colour: return trn.colormap;
	case TerrainPlane::Blend: return trn.detailblendmap;
	case TerrainPlane::kCount: break;
	}
	return kNone;
}

// A file of the family by the name the game reads it by: the project's file of that name, where it has one; an
// import's output given its source image in its place (`import`: the terrain's import, `key` its set's key).
TerrainFamilyFile family_file(const SessionView &view, const std::string &name, const TerrainImport &import,
                              const char *key, const char *words) {
	TerrainFamilyFile file;
	file.name = name;
	if (name.empty()) {
		file.missing = true;
		file.refusal = std::string("The terrain names no ") + words + ": add one first.";
		return file;
	}
	const AssetEntry *entry = view.project.scan ? view.project.scan->find(name) : nullptr;
	if (!entry) {
		file.missing = true;
		file.refusal = name + " is not a file of the project (it is the game's own, or missing): copy it into the "
		                      "project first (Import, from the game install), then brush it.";
		return file;
	}
	if (entry->imported_from.empty()) {
		file.path = entry->relative_path;
		file.words = basename_of(file.path) + ", in place";
		return file;
	}
	// An import's output: the source image the import makes it from, painted in its place and made into the terrain
	// as the project saves (the import runs again).
	file.import_output = entry->relative_path;
	for (const TerrainSetImage &image : import.images)
		if (key && image.key == key && !image.file.empty()) {
			file.path = image.file;
			file.words = "painted into " + basename_of(image.file) + ", made into the terrain as you save";
			return file;
		}
	file.refusal = name + " is made by the import of " + entry->imported_from + ", whose set names no " + words +
	               " image to brush in its place: name one in the set first.";
	return file;
}

struct Cached {
	std::string path;
	uint64_t identity = 0, load = 0, revision = 0, generation = 0;
	const void *scan = nullptr;
	bool ok = false;
	TerrainFamily family;
};

std::mutex g_cache_mutex;
Cached g_cache[4];
size_t g_cache_next = 0;

bool resolve_uncached(const SessionView &view, const DocumentBase &document, TerrainFamily &out) {
	out = TerrainFamily();
	out.view = document.path();
	std::string environment;
	TrnConfig trn;
	bool read = false;
	if (const auto *mission = dynamic_cast<const MissionDocument *>(&document)) {
		if (const MissionRow *row = mission->mission_row()) {
			const mission::MissionInfo info = mission::mission_info(row->native);
			out.terrain_name = info.terrain;
			environment = info.environment;
		}
		// The mission's own tiles, which the game reads in place of the terrain's [orig: Terrain_Init @ 0x60FCFD;
		// runtime/mission/mission_sidecars.h's tiles row].
		if (const mission::Sidecar *tiles = mission::sidecar_for_role("tiles")) {
			const std::string name = mission::sidecar_name(basename_of(document.path()), *tiles);
			out.tiles.name = name;
			const AssetEntry *entry = view.project.scan ? view.project.scan->find(name) : nullptr;
			if (entry) {
				out.tiles.path = entry->relative_path;
				out.tiles.words = basename_of(entry->relative_path);
			} else {
				// Made beside the mission with its first tile.
				const std::string folder = document.path().find('/') == std::string::npos
				                                   ? std::string()
				                                   : document.path().substr(0, document.path().rfind('/') + 1);
				out.tiles.path = folder + name;
				out.tiles.missing = true;
				out.tiles.words = name + ", made with its first tile";
			}
		}
		if (out.terrain_name.empty()) {
			out.error = basename_of(document.path()) + " names no terrain.";
			return false;
		}
	} else if (const auto *terrain = dynamic_cast<const TerrainDocument *>(&document)) {
		out.terrain = document.path();
		out.terrain_name = stem(basename_of(document.path()));
		if (const TrnConfig *config = terrain->config()) {
			trn = *config;
			read = true;
		}
	} else {
		out.error = basename_of(document.path()) + " is neither a mission nor a terrain: it has no terrain to brush.";
		return false;
	}
	// The .trn by the terrain's name, then the files it names, as the game reads them for the mission: the .trn, then
	// overcast.def and the mission's .env through one parser (D-TERRAIN-18), the .cpt the .trn names (D-TERRAIN-17)
	// [orig: PolyTrn_LoadTerrainConfig @ 0x60E62F -> sub_603CC0 @ 0x603D0A; Environment_LoadTimeOfDayConfig @
	// 0x57db44].
	if (out.terrain.empty() && view.project.scan)
		if (const AssetEntry *entry = view.project.scan->find(out.terrain_name + ".trn")) out.terrain = entry->relative_path;
	if (!read && view.findings.assets) {
		const ProjectAssetSource &files = *view.findings.assets;
		const TrnTextReader read_text = [&files](const std::string &name, std::string &text) {
			std::vector<uint8_t> bytes;
			if (!files.read(name, bytes)) return false;
			text.assign(bytes.begin(), bytes.end());
			return true;
		};
		std::string error;
		read = read_mission_trn(read_text, out.terrain_name + ".trn", environment.empty() ? std::string() : environment + ".env",
		                        trn, error) ||
		       !trn.polydata.empty();
	}
	if (!read) {
		out.error = "The project has no terrain " + out.terrain_name + ".trn to brush.";
		return false;
	}
	const TerrainImport import = out.terrain.empty() ? TerrainImport() : terrain_uses(view, out.terrain).import;
	for (size_t i = 0; i < kTerrainPlaneCount; ++i) {
		const TerrainPlane plane = static_cast<TerrainPlane>(i);
		out.planes[i] = family_file(view, plane_name(trn, plane), import, set_key(plane), terrain_plane_words(plane));
	}
	// A terrain under no mission: its own tiles (an import's output refused: its import makes them).
	if (dynamic_cast<const TerrainDocument *>(&document)) {
		const std::string own = terrain::terrain_tileinfo_name(trn.tileinfo);
		out.tiles = family_file(view, own, TerrainImport(), nullptr, "tile layout");
		if (!out.tiles.import_output.empty())
			out.tiles.refusal = own + " is made by the terrain's import: lay tiles in a mission's own .til instead.";
	}
	return true;
}

} // namespace

bool resolve_terrain_family(const SessionView &view, const DocumentBase &document, TerrainFamily &out) {
	const uint64_t generation = view.findings.assets ? view.findings.assets->generation() : 0;
	const void *scan = view.project.scan.get();
	{
		std::lock_guard<std::mutex> lock(g_cache_mutex);
		for (const Cached &cached : g_cache)
			if (cached.path == document.path() && cached.identity == document.identity() &&
			    cached.load == document.load_generation() && cached.revision == document.revision() &&
			    cached.generation == generation && cached.scan == scan && cached.identity != 0) {
				out = cached.family;
				return cached.ok;
			}
	}
	const bool ok = resolve_uncached(view, document, out);
	std::lock_guard<std::mutex> lock(g_cache_mutex);
	Cached &slot = g_cache[g_cache_next++ % std::size(g_cache)];
	slot.path = document.path();
	slot.identity = document.identity();
	slot.load = document.load_generation();
	slot.revision = document.revision();
	slot.generation = generation;
	slot.scan = scan;
	slot.ok = ok;
	slot.family = out;
	return ok;
}

} // namespace opennova::editor
