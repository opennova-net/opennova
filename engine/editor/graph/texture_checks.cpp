#include <editor/graph/texture_checks.h>

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <cstdio>
#include <set>
#include <string>
#include <tuple>
#include <utility>

#include <base/io/bits.h>
#include <base/io/file_io.h>
#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <formats/pcx/pcx_io.h>
#include <formats/trn/charmap_legend.h>
#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/texture_budget.h>
#include <editor/documents/texture_document.h>
#include <editor/documents/texture_image.h>
#include <editor/documents/texture_load_rules.h>
#include <editor/documents/texture_roles.h>
#include <editor/documents/validation_cache.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/texture_uses.h>
#include <editor/graph/use_checks.h>
#include <editor/model/core_finding.h>
#include <editor/project/project_files.h>
#include <formats/particle/particle.h>
#include <formats/til/til.h>
#include <runtime/hud/hud_texture_names.h>
#include <runtime/renderer/material_texture.h>
#include <runtime/renderer/particle_atlas.h>
#include <runtime/terrain/terrain_tile_composer.h>
#include <runtime/terrain_query/surface_type_map.h>

namespace opennova::editor {

namespace {

using R = renderer::TextureRoleId;
using F = CoreFinding;

std::string sides(uint32_t width, uint32_t height) { return std::to_string(width) + " x " + std::to_string(height); }

TextureReader reader_of(TextureFileReader reader) {
	switch (reader) {
	case TextureFileReader::Tga: return TextureReader::Tga;
	case TextureFileReader::Pcx:
	case TextureFileReader::Pcx8: return TextureReader::Pcx;
	case TextureFileReader::Dds: return TextureReader::Dds;
	case TextureFileReader::Png: return TextureReader::Png;
	case TextureFileReader::Chunk:
	case TextureFileReader::None: break;
	}
	return TextureReader::None;
}

const char *reader_words(TextureReader reader) {
	switch (reader) {
	case TextureReader::Tga: return "TGA";
	case TextureReader::Pcx: return "PCX";
	case TextureReader::Dds: return "DDS";
	case TextureReader::Png: return "PNG";
	case TextureReader::None: break;
	}
	return "texture";
}

// Each file's header as a reader reads it, kept while the file's stamp stands (a composition reads only
// the files that moved since) and the source it was read through (held weakly: one let go is never taken for
// another at its address; none for the project's folder).
struct Kept {
	uint64_t size = 0;
	int64_t modified = 0;
	std::weak_ptr<const FileSource> source;
	TextureHeader header;
};
std::mutex g_mutex;
std::map<std::pair<std::string, TextureReader>, Kept> g_headers;
TextureCheckStats g_stats; // g_mutex held

// A walk of the uses' findings, kept while what it read stands (g_mutex held): the graph's texture uses and what
// they resolve to (AssetGraph::texture_uses_generation), the scan (each file's stamp, so each header read), the
// project's folder and the source its files are read through (held weakly: one let go is never taken for another
// at its address). A composition after an edit that left the graph's texture uses as they were walks nothing.
struct WalkKey {
	uint64_t generation = 0, serial = 0;
	std::string root;
	const FileSource *files = nullptr;
	uint64_t scope = 0; // the validation's scope (ValidationScope::version), 0 for every file
	bool operator==(const WalkKey &o) const {
		return generation == o.generation && serial == o.serial && root == o.root && files == o.files &&
		       scope == o.scope;
	}
};

WalkKey walk_key(const AssetGraph &graph, const ValidationInput &input) {
	return {graph.texture_uses_generation(), input.scan.serial(), input.paths.root, input.paths.files.get(),
	        input.scope ? input.scope->version : 0};
}
struct Walk {
	std::weak_ptr<const FileSource> files;
	std::vector<Diagnostic> findings;
};
UseCheckMemo<WalkKey, Walk> g_walks;

// The bytes a header is read from: a PCX's 128 (pcx_read_header), which hold a PNG's signature and IHDR too
// (png_header_size, png_header_form: 26). A TGA's and a DDS's header is read from the whole file, which a BFC1's
// unpacking and the reads past its end (tga_short, the DXT5's first level) need.
constexpr size_t kHeaderPrefixBytes = 128;
size_t header_prefix(TextureReader reader) {
	return reader == TextureReader::Pcx || reader == TextureReader::Png ? kHeaderPrefixBytes : 0;
}

// Where a file of the validation's scan is read from, the key what it read is kept by: through the paths' source
// by its logical name where they have one (the game install as the game is served it, ProjectPaths::files), else
// the file under the project's folder.
std::string read_place(const ProjectPaths &paths, const AssetEntry &entry) {
	return paths.files ? paths.root + "|" + entry.logical_name : join_path(paths.root, entry.relative_path);
}

// A file of the validation's scan read as read_place says, its first `prefix` bytes only where nonzero (the
// project's folder read no further; a source's file is read whole, then cut).
bool read_texture_file(const ProjectPaths &paths, const AssetEntry &entry, size_t prefix, std::vector<uint8_t> &out,
                       size_t &read, std::string &error) {
	out.clear();
	read = 0;
	if (paths.files) {
		if (!paths.files->read(entry.logical_name, out)) {
			error = entry.logical_name + " could not be read.";
			return false;
		}
		read = out.size();
		if (prefix && out.size() > prefix) out.resize(prefix);
		return true;
	}
	const std::string path = join_path(paths.root, entry.relative_path);
	if (!prefix) {
		const bool ok = io::read_file_bytes(path, out, error);
		read = out.size();
		return ok;
	}
	std::FILE *f = io::fopen_utf8(path.c_str(), "rb");
	if (!f) {
		error = "cannot open " + path;
		return false;
	}
	out.resize(prefix);
	const size_t got = std::fread(out.data(), 1, prefix, f);
	const bool ok = std::ferror(f) == 0;
	std::fclose(f);
	out.resize(ok ? got : 0);
	read = out.size();
	if (!ok) error = "cannot read " + path;
	return ok;
}

// Whether a kept read is of `entry` as the scan lists it, through the same source.
template <typename K> bool kept_current(const K &kept, const ProjectPaths &paths, const AssetEntry &entry) {
	return kept.size == entry.size_bytes && kept.modified == entry.modified_ticks && kept.source.lock() == paths.files;
}

// The header of `entry` as `reader` reads it, read where read_place says and kept; null for no file or no reader
// (g_mutex held). `read` the bytes it read now (0 where it was kept).
const TextureHeader *kept_header(const ProjectPaths &paths, const AssetEntry *entry, TextureReader reader,
                                 size_t *read = nullptr) {
	if (read) *read = 0;
	if (!entry || reader == TextureReader::None) return nullptr;
	const auto key = std::make_pair(read_place(paths, *entry), reader);
	const auto found = g_headers.find(key);
	if (found != g_headers.end() && kept_current(found->second, paths, *entry)) return &found->second.header;
	std::vector<uint8_t> bytes;
	std::string error;
	size_t bytes_read = 0;
	Kept kept;
	kept.size = entry->size_bytes;
	kept.modified = entry->modified_ticks;
	kept.source = paths.files;
	if (read_texture_file(paths, *entry, header_prefix(reader), bytes, bytes_read, error))
		kept.header = texture_header_as(reader, bytes);
	else kept.header.refusal = error;
	++g_stats.headers_read;
	g_stats.header_bytes += bytes_read;
	if (read) *read = bytes_read;
	return &(g_headers[key] = std::move(kept)).header;
}

const TextureHeader *header_of(const ValidationInput &input, const std::string &relative, TextureReader reader) {
	return kept_header(input.paths, input.scan.at_path(relative), reader);
}

// A char map's classes at or past the twenty the tile set tables name (TSD_NULL to TSD_*, formats/trn
// charmap_legend.h): how many texels hold one and the highest, as its 8-bit PCX decodes, kept while the file's stamp
// stands (g_mutex held). None for a file that is no 8-bit PCX.
struct KeptClasses {
	uint64_t size = 0;
	int64_t modified = 0;
	std::weak_ptr<const FileSource> source;
	size_t past = 0;
	int highest = -1;
};
std::map<std::string, KeptClasses> g_classes;

const KeptClasses *kept_classes(const ProjectPaths &paths, const AssetEntry *entry, size_t *read = nullptr) {
	if (read) *read = 0;
	if (!entry) return nullptr;
	const std::string place = read_place(paths, *entry);
	const auto found = g_classes.find(place);
	if (found != g_classes.end() && kept_current(found->second, paths, *entry)) return &found->second;
	KeptClasses kept;
	kept.size = entry->size_bytes;
	kept.modified = entry->modified_ticks;
	kept.source = paths.files;
	std::vector<uint8_t> bytes;
	std::string error;
	size_t bytes_read = 0;
	IndexedImage8 image;
	const bool ok = read_texture_file(paths, *entry, 0, bytes, bytes_read, error);
	++g_stats.headers_read;
	g_stats.header_bytes += bytes_read;
	if (read) *read = bytes_read;
	if (ok && decode_pcx_indexed(bytes.data(), bytes.size(), image, error))
		for (const uint8_t index : image.indices)
			if (index >= kCharmapLegendCount) {
				++kept.past;
				kept.highest = std::max(kept.highest, int(index));
			}
	return &(g_classes[place] = kept);
}

const KeptClasses *char_map_classes(const ValidationInput &input, const std::string &relative) {
	return kept_classes(input.paths, input.scan.at_path(relative));
}

// Where a use is, in words: "the terrain colour map of isle.trn".
std::string use_words(const TextureRoleRow &role, const GraphEdge &edge) {
	const std::string file = basename_of(edge.source);
	return "the " + std::string(role.words) + " of " + (edge.record.empty() ? file : edge.record + " in " + file);
}

// A finding on a use: at the referrer's field, about the reference (what Problems' "Make the import fit
// this use" reaches the file by).
Diagnostic on_use(const GraphEdge &edge, F code, DiagnosticSeverity severity, const std::string &message) {
	Diagnostic d = make_finding(finding_code(code), severity, message, edge.source, edge.field);
	d.subject = ReferenceSubject{edge.kind, edge.value, edge.scope, edge.loader_arg};
	d.record = edge.record;
	d.row_id = edge.address.row;
	d.child_id = edge.address.child;
	d.record_kind = edge.address.kind;
	d.line = edge.span.line;
	return d;
}

// What a role asks of the file its use's loader opens, by its header: the findings on the use (`at`),
// or, for a name the game opens itself (no edge), on the file. `context` what the use says of itself (a
// particle graphic's mode).
void check_sizes(R role, const std::string &file, const TextureHeader &header, const std::string &where,
                 const TextureUseContext &context, const std::function<void(F, DiagnosticSeverity, const std::string &)> &add) {
	const uint32_t w = header.width, h = header.height;
	const std::string is = file + ", " + where + ", is " + sides(w, h);
	switch (role) {
	case R::TerrainColourMap: {
		// Checksummed and premultiplied over exactly 0x400000 bytes [orig: PolyTrn_InitTextures @ 0x60B3BE,
		// @ 0x60B5A9..0x60B6FD], its quadrants split at its own width into a buffer of its texels [orig: the
		// split @ 0x60B510]. Listed (S26 A4): a short map's read runs on into the arena's memory past its block
		// (FastMem, no fault [orig: FastMem_Alloc @ 0x7697B0]), and no failure of the split's write past the
		// wider map's buffer is witnessed.
		constexpr uint32_t side = terrain::kTerrainColourMapSide;
		if (w == side && h == side) break;
		const bool short_read = uint64_t(w) * h < uint64_t(side) * side;
		const bool split = terrain::terrain_map_split_overruns(w, h);
		if (short_read || split)
			add(F::TextureColourMapSize, DiagnosticSeverity::Error,
			    is + ": " + (short_read ? "the game reads 1024 x 1024 texels (4 MB) from it" : std::string()) +
			            (short_read && split ? ", and " : "") +
			            (split ? "the game splits it in quadrants of half its width, copying " + sides(w & ~1u, w & ~1u) +
			                             " texels out of it and into a buffer of its own size"
			                   : std::string()) +
			            ": past the end of its texels" + (split ? " and of that buffer" : "") + ". Make it 1024 x 1024.");
		else
			add(F::TextureColourMapSize, DiagnosticSeverity::Warning,
			    is + ": the far terrain and the foliage colours take only its first 1024 x 1024 texels (4 MB), in "
			         "the wrong rows. Make it 1024 x 1024.");
		break;
	}
	case R::TerrainBlendMap:
		// Loaded whole, then split at its width into a buffer of its texels [orig: PolyTrn_InitTextures @
		// 0x60B1B8 (the load), @ 0x60B29C (the buffer), @ 0x60B2C1 (the split)]: listed, no failure of it
		// witnessed (S26 A4).
		if (terrain::terrain_map_split_overruns(w, h))
			add(F::TextureBlendMapSize, DiagnosticSeverity::Error,
			    is + ": the game splits it in quadrants of half its width, copying " + sides(w & ~1u, w & ~1u) +
			            " texels out of it and into a buffer of its own size: past the end of both. Make it at least as "
			            "tall as it is wide.");
		break;
	case R::TerrainFoliageMap: {
		// Sampled at (c & 1023) >> (10 - log2(width)) on both axes; past 1024 the shift is negative, which the
		// processor takes modulo 32, so every sample reads the first texel (terrain::surface_sample_extent, its
		// witnesses). Rows past the file's read the arena's memory past its block [orig: Foliage_LoadFoliageMapPCX
		// @ 0x605B35; Foliage_SampleFoliageMapMask @ 0x606620 (the read @ 0x6066C6)]: wrong foliage, no fault,
		// listed (S26 A4).
		const uint32_t side = uint32_t(terrain::surface_sample_extent(int32_t(std::min<uint32_t>(w, 0x7FFFFFFF))));
		if (w <= 1024 && h < side)
			add(F::TextureFoliageMapOverrun, DiagnosticSeverity::Error,
			    is + ": the game's foliage lookup reads " + std::to_string(side) + " rows of it, past its last.");
		else if (w > 1024)
			add(F::TextureFoliageMapShape, DiagnosticSeverity::Warning,
			    is + ": the game's foliage lookup reads its first texel everywhere once it is wider than 1024, so one "
			         "code covers the whole map. Make it a square whose side is a power of two, at most 1024.");
		else if (w != h || !io::is_power_of_two(w))
			add(F::TextureFoliageMapShape, DiagnosticSeverity::Warning,
			    is + ": the game's foliage lookup takes it as a square whose side is a power of two, at most 1024, so "
			         "its codes land on the wrong ground.");
		break;
	}
	case R::TerrainCharMap: {
		// Sampled as the foliage map is, at (c & 1023) >> (10 - log2(width)) on both axes, its byte the class
		// (terrain::surface_sample_extent) [orig: Terrain_GetSurfaceTypeAtPosition @ 0x606510..0x6065C6], from a heap
		// block the file's own size, width times height ("PolyTrn Charmap") [orig: Terrain_LoadCharmapPCX @ 0x605A58,
		// FastMem_AllocLabeled(w*h)]: rows past the file's read past the block, the arena's memory beyond it, no fault (S26
		// A4): listed, as the other shapes are.
		const uint32_t side = uint32_t(terrain::surface_sample_extent(int32_t(std::min<uint32_t>(w, 0x7FFFFFFF))));
		if (w <= 1024 && h < side)
			add(F::TextureCharMapOverrun, DiagnosticSeverity::Error,
			    is + ": the game's surface lookup reads " + std::to_string(side) + " rows of it, past its last.");
		else if (w > 1024)
			add(F::TextureCharMapShape, DiagnosticSeverity::Warning,
			    is + ": the game's surface lookup reads its first texel everywhere once it is wider than 1024, so one "
			         "class covers the whole map. Make it a square whose side is a power of two, at most 1024.");
		else if (w != h || !io::is_power_of_two(w))
			add(F::TextureCharMapShape, DiagnosticSeverity::Warning,
			    is + ": the game's surface lookup takes it as a square whose side is a power of two, at most 1024, so "
			         "its classes land on the wrong ground.");
		break;
	}
	case R::TerrainTileAtlas: {
		// Cut in 64-texel cells [orig: Terrain_LoadTileSetAtlas @ 0x604B7C] (TIL_ATLAS_TILE_PIXELS).
		constexpr uint32_t cell = uint32_t(TIL_ATLAS_TILE_PIXELS);
		if (w % cell || h % cell)
			add(F::TextureTileAtlasCells, DiagnosticSeverity::Warning,
			    is + ": the game cuts it in " + std::to_string(cell) + "-texel cells, so its last " +
			            (w % cell ? std::to_string(w % cell) + " columns" : std::string()) + (w % cell && h % cell ? " and " : "") +
			            (h % cell ? std::to_string(h % cell) + " rows" : std::string()) + " are never drawn.");
		break;
	}
	case R::ModelNormalMap:
	case R::ModelHeightNormal:
		// Halved to fit 512 a side [orig: Material_LoadStageTexture @ 0x5B1782, flag 0x1000].
		if (w > 512 || h > 512)
			add(F::TextureNormalMapHalved, DiagnosticSeverity::Info, is + ": the game halves it until it fits 512 a side.");
		if (role == R::ModelHeightNormal && (!io::is_power_of_two(w) || !io::is_power_of_two(h)))
			add(F::TextureHeightWrap, DiagnosticSeverity::Warning,
			    is + ": the game makes its normal map wrapping each texel's neighbours by the side's mask "
			         "[orig: Texture_LoadAsNormalMap @ 0x58C985..0x58CAED], which is wrong at the edges of a side that is "
			         "no power of two.");
		break;
	case R::TerrainDetailCoefficient:
		if (!io::is_power_of_two(w) || !io::is_power_of_two(h))
			add(F::TextureHeightWrap, DiagnosticSeverity::Warning,
			    is + ": the game builds its coefficient wrapping each texel's neighbours by the side's mask "
			         "[orig: Texture_GenerateNormalMap @ 0x58C070], which is wrong at the edges of a side that is no "
			         "power of two.");
		break;
	case R::ParticleGraphic: {
		// Packed on an atlas page of the side its mode picks; a graphic no page can hold is never placed, and
		// the build makes a page for it on every pass, forever: the game hangs as the effects load
		// (renderer::particle_atlas_fits, its witnesses).
		const uint8_t mode = uint8_t(context.blend_mode < 0 ? 0 : context.blend_mode);
		if (!renderer::particle_atlas_fits(mode, int(std::min<uint32_t>(w, 0x7FFFFFFF)), int(std::min<uint32_t>(h, 0x7FFFFFFF)))) {
			const int page = renderer::particle_atlas_page_side(mode);
			add(F::TextureParticleTooBig, DiagnosticSeverity::Error,
			    is + ": a " + particle::blend_mode_name(particle::BlendMode(mode)) + " graphic is packed on the particle "
			            "atlas's " + std::to_string(page) + "-texel pages, which hold one narrower than " + std::to_string(page) +
			            " and at most " + std::to_string(page) + " tall. No page holds this one, and the game makes a new "
			            "page for it again and again: it hangs loading the effects. Make it smaller.");
		}
		break;
	}
	case R::HudMfd:
		if (!hud::hud_mfd_texture_takes(w, h))
			add(F::TextureMfdNotPowerOfTwo, DiagnosticSeverity::Warning,
			    is + ": the game makes no material of an MFD texture whose sides are not powers of two [orig: HUD_LoadMfdTexture @ "
			         "0x59B19F..0x59B1BD].");
		break;
	case R::LoadingScreen:
		if (w != 800 || h != 600)
			add(F::TextureLoadingScreenSize, DiagnosticSeverity::Warning,
			    is + ": the game stretches it over the screen and lays the multiplayer text out for 800 x 600 art "
			         "[orig: Render_LoadingScreen @ 0x521D10].");
		break;
	default: break;
	}
}

// What a model row's texture costs the game, and the loader a normal-map slot is read by (ADR 0046 S18, the
// texture budget): a texture past kTextureMemoryWarnBytes with its chain, said once a file, loader and slot
// (`costed`) with what its .dds would cost; a row in a normal-map slot (3 or 4, which the material's normal
// map samplers read) whose type the stage or plain loader reads, so the file is never capped at 512 as a
// normal map is, nor halved for the object texture detail, nor made from a height [orig:
// Material_LoadStageTexture @ 0x5B16F4..0x5B1790: the slot table @ 0x5B181C gives slots 3 to 7 no detail
// word, and types 0 to 2 and 8 go to the stage and plain loaders, which never set the cap].
void check_budget(R role, const std::string &served, const TextureHeader &header, const std::string &where,
                  const TextureUseContext &context, std::set<std::string> &costed,
                  const std::function<void(F, DiagnosticSeverity, const std::string &)> &add) {
	renderer::TextureLoader loader = renderer::TextureLoader::Stage;
	if (!texture_role_budget_loader(role, loader)) return;
	const std::string file = basename_of(served);
	const TextureBudget budget = texture_budget(header, file, loader, context.slot);
	if (!budget.known) return;
	const renderer::DeviceTexture &full = budget.full();
	const bool normal_slot = (context.slot == 3 || context.slot == 4) && loader != renderer::TextureLoader::Normal;
	if (normal_slot) {
		const bool mdt = strutil::to_lower(utf8_of(path_of(file).extension())) == ".mdt";
		const renderer::DeviceTexture capped = renderer::pixel_device_texture(header.width, header.height, renderer::kTextureFlagCap512);
		add(F::TextureNormalSlotLoader, DiagnosticSeverity::Warning,
		    file + ", " + where + ", sits in normal-map slot " + std::to_string(context.slot) + " with type " +
		            std::to_string(context.type) + ", which the game loads as it loads a diffuse: " +
		            device_texture_words(full) + ", never halved to fit 512 a side as a normal map is. " +
		            (mdt ? "Give the row type 4 (a finished normal map), and it takes " + texture_bytes_words(capped.bytes) + "."
		                 : "Store a finished normal map as an .mdt with type 4 (" + texture_bytes_words(capped.bytes) +
		                           "); type 4 or 5 over a .tga makes the normal map from its alpha as a height."));
	}
	if (full.bytes <= kTextureMemoryWarnBytes || !costed.insert(served + "\x01" + texture_loader_token(loader) + "\x01" +
	                                                             std::to_string(context.slot)).second)
		return;
	std::string message = file + ", " + where + ", takes " + texture_bytes_words(full.bytes) + " of the game's memory: " +
	                       device_texture_words(full) + ", every level of which the game keeps.";
	if (budget.offers_dds)
		message += " As a " + std::string(renderer::device_texture_format_name(budget.as_dds.format)) + " .dds beside it, which " +
		           "its loader reads first, it would take " + texture_bytes_words(budget.as_dds.bytes) + ".";
	else if (loader != renderer::TextureLoader::Normal && header.reader == TextureReader::Dds)
		message += " Make it smaller.";
	const renderer::DeviceTexture &lowest = budget.detail[0];
	if (lowest.bytes < full.bytes)
		message += " At the lowest object texture detail the game halves it to " + std::to_string(lowest.width) + " x " +
		           std::to_string(lowest.height) + " (" + texture_bytes_words(lowest.bytes) + ").";
	add(F::TextureMemory, DiagnosticSeverity::Warning, message);
}

// What the game's TGA reader makes of a terrain map's file (tga_reader_read), in words: the sides it reads and
// what it writes for the texels, then what the terrain does with it (`blend`: the detail blend map, which the
// terrain loads without where the read fails [orig: PolyTrn_InitTextures @ 0x60B1C2 -> 0x60B35B]; else the
// colour map, whose failed read logs "colortga" and aborts the mission [orig: @ 0x60B3B1 -> 0x60C60A;
// Game_ShowErrorLogAndAbortMission @ 0x520B4E]).
std::string tga_reader_words(const TgaReaderRead &read, const TextureHeader &header, bool blend) {
	const std::string sides = std::to_string(read.width) + " x " + std::to_string(read.height);
	const std::string form = "its image type " + std::to_string(header.tga_type) + " at " + std::to_string(header.tga_bits) +
	                         " bits";
	switch (read.outcome) {
	case TgaReaderOutcome::Refused:
		return " The game's TGA reader reads its bytes 12 to 15 as a " + sides + " picture and asks its memory pool for " +
		       texture_bytes_words(uint64_t(std::max<int64_t>(read.texel_bytes, 0))) +
		       " of texels, more than the pool (" + texture_bytes_words(uint64_t(kGameMemoryPoolBytes)) +
		       ") can give: the read fails, and " +
		       (blend ? std::string("the game loads the terrain without a detail blend, logging nothing.")
		              : std::string("the mission aborts (\"Polytrn: colortga\")."));
	case TgaReaderOutcome::Faults:
		return " The game's TGA reader reads its bytes 12 to 15 as a " + sides +
		       " picture, whose texels' bytes do not fit the int it sizes their buffer in: it writes them past the buffer "
		       "it gets, and the game faults loading the terrain.";
	case TgaReaderOutcome::Decoded:
		return " The game's TGA reader decodes its bytes as a " + sides + " TGA's texels (" + form +
		       ") and the terrain loads on with them.";
	case TgaReaderOutcome::Black:
		return " The game's TGA reader takes it as " + sides + " black texels (" + form +
		       ", which its reader clears) and the terrain loads on.";
	case TgaReaderOutcome::Stale:
		return " The game's TGA reader takes it as " + sides + " texels it never writes (" + form +
		       ", which no arm of its reader decodes), holding what the game's memory held there, and the terrain loads on.";
	case TgaReaderOutcome::Empty:
		return " The game's TGA reader reads its sides as " + sides + ", no texel, and the terrain loads on.";
	}
	return std::string();
}

// What a use's check reads, resolved (check_use's first half, and texture_use_reads'): its role and what the use says
// of itself, the file its loader opens (`served`, the load, and the reader it reads the file by), and the project's
// file of the name written; for a terrain's detail map, the reader its own name picks instead (`detail`: the TGA
// reader for a .tga, the PCX reader for a .pcx, None for neither). False for no role, or a use that loads no
// project file.
struct UseTarget {
	R role = R::kCount;
	TextureUseContext context;
	const TextureRoleRow *row = nullptr;
	std::string served;
	TextureLoad load;
	TextureReader reader = TextureReader::None;
	const AssetEntry *written = nullptr;
	TextureReader detail = TextureReader::None;
};

bool use_target(const AssetGraph &graph, const ValidationInput &input, const GraphEdge &edge, UseTarget &out) {
	out.role = texture_role_of_edge(edge, nullptr, out.context);
	if (out.role == R::kCount) return false;
	out.row = &texture_role_row(out.role);
	if (graph.resolve(edge, &out.served) != ReferenceStatus::Present || out.served.empty()) return false;
	const TextureNameTest exists = [&graph](const std::string &name) { return graph.has_file(name); };
	out.load = edge.kind == ReferenceKind::Texture ? texture_reference_load(edge.value, edge.loader_arg, exists)
	                                               : texture_load(out.row->loader, edge.value, exists);
	out.reader = reader_of(out.load.reader);
	out.written = input.scan.find(basename_of(edge.value));
	if (out.role == R::TerrainDetailCoefficient) {
		// A terrain's detail map is made into its detail coefficient from the name as written, through the TGA
		// reader for a .tga and the PCX reader for a .pcx, never a .dds [orig: Texture_GenerateNormalMap @
		// 0x58C116..0x58C159, from PolyTrn_InitTextures @ 0x60B155]: its sizes are that file's.
		const std::string named = strutil::to_lower(utf8_of(path_of(basename_of(edge.value)).extension()));
		out.detail = named == ".tga" ? TextureReader::Tga : named == ".pcx" ? TextureReader::Pcx : TextureReader::None;
	}
	return true;
}

bool is_texture_use(const GraphEdge &edge) {
	return edge.kind == ReferenceKind::Texture || edge.kind == ReferenceKind::MenuTexture ||
	       edge.kind == ReferenceKind::LoadingImage;
}

// Under a validation's scope (ValidationScope), the files the loaders of its own files' uses open (a terrain
// detail's coefficient aside, which is read by its own name and shares nothing with another use): a use of one
// of them by a file outside the scope is checked too, its findings left out, so the one finding a texture's cost
// makes is said where a check of every file says it (the first use, in the files' order).
std::set<std::string> scope_served(const AssetGraph &graph, const ValidationInput &input) {
	std::set<std::string> out;
	graph.for_each_edge([&](const GraphEdge &edge) {
		UseTarget target;
		if (is_texture_use(edge) && input.in_scope(edge.source) && use_target(graph, input, edge, target) &&
		    target.role != R::TerrainDetailCoefficient)
			out.insert(target.served);
	});
	return out;
}

// One use's findings into `out`: under a scope (`served_in_scope`, scope_served; null without one), those about
// the scope's files alone (a file outside it whose use opens one of those is checked for what the uses after it
// share, its findings left out; any other is checked only for the file of its name its loader passes over).
void check_use(const AssetGraph &graph, const ValidationInput &input, const GraphEdge &edge,
               std::set<std::pair<std::string, std::string>> &passed_over, std::set<std::string> &costed,
               std::vector<Diagnostic> &out, const std::set<std::string> *served_in_scope = nullptr) {
	UseTarget target;
	if (!use_target(graph, input, edge, target)) return;
	const R role = target.role;
	const TextureUseContext &context = target.context;
	const TextureRoleRow &row = *target.row;
	const std::string &served = target.served;
	const TextureLoad &load = target.load;
	const std::string where = use_words(row, edge);
	// A file of the name written that the loader passes over for another (a .tga beside the .dds a model
	// row loads) [orig: Texture_LoadByNameWithChannel @ 0x58B53C..0x58B5C0]: on that file, once. Not a
	// terrain detail's, which the game reads by its own name too (texture_role_read_by_name).
	const AssetEntry *written = target.written;
	if (written && written->kind == AssetKind::Texture && written->relative_path != served && !renderer::texture_role_read_by_name(role) &&
	    passed_over.insert({written->relative_path, served}).second && input.in_scope(written->relative_path))
		out.push_back(make_finding(finding_code(TextureFinding::NotRead), DiagnosticSeverity::Warning,
		                           "The game never reads " + written->logical_name + " for " + where + ": its loader opens " +
		                                   basename_of(served) + " instead.",
		                           written->relative_path));
	// A use outside the scope goes on only where a use of the scope's opens the same file (what they share).
	const bool own = input.in_scope(edge.source);
	if (!own && (role == R::TerrainDetailCoefficient || !served_in_scope || !served_in_scope->count(served))) return;
	const auto add = [&](F code, DiagnosticSeverity severity, const std::string &message) {
		if (own) out.push_back(on_use(edge, code, severity, message));
	};
	// A terrain's detail map, made into its coefficient by its own name (use_target): its sizes are that file's.
	if (role == R::TerrainDetailCoefficient) {
		const TextureReader by = target.detail;
		const TextureHeader *source = written && by != TextureReader::None ? header_of(input, written->relative_path, by) : nullptr;
		if (!source || !source->read) {
			add(F::TextureWrongReader, DiagnosticSeverity::Warning,
			    basename_of(edge.value) + ", " + where + ", is made into the terrain's detail coefficient by its own name, " +
			            "through the game's TGA reader for a .tga or its PCX reader for a .pcx: " +
			            (by == TextureReader::None ? std::string("a name of neither") : !written ? std::string("the project lacks it")
			                                                                              : "the reader cannot read it") +
			            ", so the game makes no coefficient of it (its .dds is for the near texture alone).");
			return;
		}
		check_sizes(role, written->logical_name, *source, where, context, add);
		return;
	}
	const TextureReader reader = target.reader;
	const TextureHeader *header = header_of(input, served, reader);
	if (!header) return;
	// A terrain's colour map and detail blend map go through the TGA reader whatever the file holds, which fails
	// only for a missing file [orig: CTerrainTileData_LoadTGAFromArchive @ 0x56E585] or an allocation or a
	// decompression that fails (@ 0x56E62C, @ 0x56E65F, @ 0x56E6AA); its texels' allocation is sized by the header's
	// words before the image type is looked at (tga_reader_read). A failed read of the colour map aborts the
	// mission, the game's refusal its row cites; of the blend map it is skipped, nothing logged, the terrain loading
	// on without a blend [orig: PolyTrn_InitTextures @ 0x60B1B8..0x60B1C2 to @ 0x60B35B] (a missing one aborts, the
	// reference's own gate [orig: @ 0x60B18E, "blendermap" @ 0x60B19A]). A read that writes past its buffer faults
	// either way. Any other read loads on, with what the type's arm wrote: a warning, listed.
	const bool terrain_tga = reader == TextureReader::Tga && texture_arg_gates(edge.loader_arg);
	const bool blend = role == R::TerrainBlendMap;
	const TgaReaderRead tga = terrain_tga && header->read ? tga_reader_read(*header) : TgaReaderRead();
	if (terrain_tga && header->read &&
	    (tga.outcome == TgaReaderOutcome::Refused || tga.outcome == TgaReaderOutcome::Faults)) {
		const bool fails = tga.outcome == TgaReaderOutcome::Faults || !blend;
		add(F::TextureWrongReader, fails ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning,
		    basename_of(served) + ", " + where + ", is read by the game's TGA reader." + tga_reader_words(tga, *header, blend));
		return;
	}
	// What the game makes of the file: a terrain map's texels as the TGA reader's arm writes them, or the role's own
	// words for a file it does not get.
	const std::string then = terrain_tga ? (header->read ? tga_reader_words(tga, *header, blend) : std::string())
	                         : *row.missing ? " Without it: " + std::string(row.missing) + "."
	                                        : std::string();
	// A file of a format the role's loader does not read (a colour map that is no TGA, a particle graphic
	// that is no TGA, a loading screen that is no PCX: roles.md's loaders).
	const size_t dot = served.find_last_of('.');
	const std::string extension = dot == std::string::npos ? std::string() : served.substr(dot);
	if (row.formats != 0 && !renderer::texture_role_takes(row, extension)) {
		std::string formats;
		for (const std::string &each : renderer::texture_role_extensions(row)) formats += (formats.empty() ? "" : ", ") + each;
		add(F::TextureWrongReader, DiagnosticSeverity::Warning,
		    basename_of(served) + ", " + where + ", is read by the game's " + reader_words(reader) + " reader, which takes " +
		            (formats.empty() ? std::string("no texture file") : formats) + " files." + then);
		return;
	}
	if (!header->read) {
		// The loader cannot read the file at all (a TGA header cut short). Where the reader is the one the file's
		// name picks and the use is no terrain map, the file's own finding (texture.unloadable) says it already.
		if (reader == texture_reader_for(served) && !texture_arg_gates(edge.loader_arg)) return;
		add(F::TextureWrongReader, DiagnosticSeverity::Warning,
		    basename_of(served) + ", " + where + ", is read by the game's " + reader_words(reader) +
		            " reader, which cannot read it: " + header->refusal + then);
		return;
	}
	check_sizes(role, basename_of(served), *header, where, context, add);
	check_budget(role, served, *header, where, context, costed, add);
	// A char map's classes past the twenty the tile set tables name: the game reads the byte as it stands
	// [orig: Terrain_GetSurfaceTypeAtPosition @ 0x6065C6], and what reads the class bounds it alone (docs/tiles/til-re.md
	// "A char-map class at or past 20"): listed, never gated.
	if (own && role == R::TerrainCharMap && reader == TextureReader::Pcx)
		if (const KeptClasses *classes = char_map_classes(input, served); classes && classes->past > 0)
			add(F::TextureCharMapClass, DiagnosticSeverity::Warning,
			    basename_of(served) + ", " + where + ", holds " + std::to_string(classes->past) + " texel" +
			            (classes->past == 1 ? "" : "s") + " of a class at or past 20 (the highest " +
			            std::to_string(classes->highest) +
			            "), past the twenty surface classes the tile set tables name: the game reads them as they stand, "
			            "its footsteps and weather treat them as no class they name, and a round striking there takes an "
			            "impact row no shipped table authors (classes 20 to 23) or the ammo's obj row (24 and past).");
	// A material that cuts out by this texture's alpha (the row its technique tests, TextureRowContext) over a
	// texture of none: a PCX the game loads with every texel opaque [orig: Texture_LoadPCXFromPFF32 @
	// 0x56EC98..0x56ECF3], or a file of no alpha, read as 255. The test keeps a texel whose alpha is above the
	// reference, or with the inverted test at or below it (render-material-re D-RMAT-1), so every texel is
	// kept, or none is.
	if ((role == R::ModelDiffuse || role == R::ModelFlipFrame || role == R::ModelNormalMap) && context.alpha_test()) {
		const bool opaque_pcx = reader == TextureReader::Pcx && load.transform == TextureLoadTransform::None;
		if (opaque_pcx || !header->alpha) {
			const bool inverted = context.alpha_test_inverted();
			add(F::TextureAlphaNotLoaded, DiagnosticSeverity::Warning,
			    basename_of(served) + ", " + where + ", is cut out by its alpha: the game keeps the texels whose alpha is " +
			            (inverted ? "at or below " : "above ") + std::to_string(context.alpha_ref) + ", but " +
			            (opaque_pcx ? "it loads a PCX fully opaque" : "this file holds no alpha, which reads as 255") +
			            (inverted ? ": no texel is kept, and the material draws nothing." : ": every texel is kept, and nothing is cut out."));
		}
	}
}

} // namespace

TextureHeader texture_file_header(const std::string &root, const AssetEntry &entry, TextureReader reader) {
	const std::lock_guard<std::mutex> lock(g_mutex);
	ProjectPaths paths;
	paths.root = root;
	const TextureHeader *header = kept_header(paths, &entry, reader);
	return header ? *header : TextureHeader();
}

std::vector<TextureRead> texture_use_reads(const AssetGraph &graph, const ValidationInput &input) {
	std::vector<TextureRead> out;
	{
		// None while the check keeps its walk of the graph and the scan as they stand: a composition reads nothing.
		const std::lock_guard<std::mutex> lock(g_mutex);
		const WalkKey key = walk_key(graph, input);
		if (key.serial != 0)
			if (const Walk *kept = g_walks.find(key); kept && kept->files.lock() == input.paths.files) return out;
	}
	std::set<std::tuple<const AssetEntry *, TextureReader, bool>> listed;
	const auto add = [&](const AssetEntry *entry, TextureReader reader, bool classes) {
		if (entry && reader != TextureReader::None && listed.insert({entry, reader, classes}).second)
			out.push_back({entry, reader, classes});
	};
	// Under a scope, the scope's own uses' reads, and the header a use outside it reads of a file one of them
	// opens (check_use goes on there).
	const std::set<std::string> served_in_scope = input.scope ? scope_served(graph, input) : std::set<std::string>();
	graph.for_each_edge([&](const GraphEdge &edge) {
		UseTarget target;
		if (!is_texture_use(edge) || !use_target(graph, input, edge, target)) return;
		const bool own = input.in_scope(edge.source);
		if (target.role == R::TerrainDetailCoefficient) {
			if (own) add(target.written, target.detail, false);
			return;
		}
		if (!own && !served_in_scope.count(target.served)) return;
		const AssetEntry *served = input.scan.at_path(target.served);
		add(served, target.reader, false);
		// A char map's classes (check_use reads its texels past a header its loader reads).
		if (own && target.role == R::TerrainCharMap && target.reader == TextureReader::Pcx)
			add(served, TextureReader::Pcx, true);
	});
	for (const FixedTextureName &fixed : fixed_texture_names()) {
		if (fixed.role != R::HudMfd && fixed.role != R::LoadingScreen) continue;
		const AssetEntry *entry = input.scan.find(fixed.name);
		if (entry && entry->kind == AssetKind::Texture && input.in_scope(entry->relative_path))
			add(entry, TextureReader::Pcx, false);
	}
	return out;
}

uint64_t read_texture_use(const ValidationInput &input, const TextureRead &read) {
	const std::lock_guard<std::mutex> lock(g_mutex);
	size_t bytes = 0;
	if (read.classes) kept_classes(input.paths, read.entry, &bytes);
	else kept_header(input.paths, read.entry, read.reader, &bytes);
	return bytes;
}

TextureCheckStats texture_check_stats() {
	const std::lock_guard<std::mutex> lock(g_mutex);
	return g_stats;
}

void check_texture_role(renderer::TextureRoleId role, const std::string &file, const TextureHeader &header, const std::string &where,
                        const TextureFindingSink &add, const TextureUseContext &context) {
	check_sizes(role, file, header, where, context, add);
}

void check_texture_uses(const AssetGraph &graph, const ValidationCache &files, const ValidationInput &input,
                        std::vector<Diagnostic> &out) {
	(void)files;
	const std::lock_guard<std::mutex> lock(g_mutex);
	// The walk kept while the graph's texture uses, the scan and the files it read stand (a scan never indexed
	// keeps none).
	const WalkKey key = walk_key(graph, input);
	if (key.serial != 0)
		if (const Walk *kept = g_walks.find(key); kept && kept->files.lock() == input.paths.files) {
			out.insert(out.end(), kept->findings.begin(), kept->findings.end());
			return;
		}
	++g_stats.use_walks;
	std::vector<Diagnostic> found;
	std::set<std::pair<std::string, std::string>> passed_over;
	std::set<std::string> costed;
	// Under a scope, its files' findings alone, as a walk of every file makes them (check_use).
	const std::set<std::string> served_in_scope = input.scope ? scope_served(graph, input) : std::set<std::string>();
	graph.for_each_edge([&](const GraphEdge &edge) {
		if (is_texture_use(edge))
			check_use(graph, input, edge, passed_over, costed, found, input.scope ? &served_in_scope : nullptr);
	});
	// The names the game opens itself whose sizes it asks for: the MFD's, the default loading screen's.
	for (const FixedTextureName &fixed : fixed_texture_names()) {
		if (fixed.role != R::HudMfd && fixed.role != R::LoadingScreen) continue;
		const AssetEntry *entry = input.scan.find(fixed.name);
		if (!entry || entry->kind != AssetKind::Texture || !input.in_scope(entry->relative_path)) continue;
		const TextureHeader *header = header_of(input, entry->relative_path, TextureReader::Pcx);
		if (!header || !header->read) continue;
		check_sizes(fixed.role, entry->logical_name, *header, std::string("which the game opens by name ") + fixed.what,
		            TextureUseContext(), [&](F code, DiagnosticSeverity severity, const std::string &message) {
			            found.push_back(make_finding(finding_code(code), severity, message, entry->relative_path));
		            });
	}
	out.insert(out.end(), found.begin(), found.end());
	if (key.serial != 0) g_walks.keep(key, {input.paths.files, std::move(found)});
}

void forget_texture_checks() {
	const std::lock_guard<std::mutex> lock(g_mutex);
	g_walks.clear();
	g_headers.clear();
	g_classes.clear();
}

} // namespace opennova::editor
