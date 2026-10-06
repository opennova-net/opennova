// The extractors: what one file references and defines, from its bytes through the
// engine's own parser for its kind (extract_from_bytes). The record types (the def
// catalogs, the string tables, the menus, the stylesheets, the models, the clips and the
// animation tables) walk their schema: a field's reference, and the symbol a field
// defines (a weapon's name, a string's key, a menu's screen or window by the NAME its
// ACTIONs find it by), and the record sets of the collections a Record reference names (a
// model's CTRL registers and MTRX rows, by their index); a text type reads the names its text
// makes, each at its span (a script's operands, S13 D9), and the names it defines (a shader's tags); the
// native kinds (an environment, the
// avatar table, a particle file, a face animation) read their parsed structs. The names a native text (a
// terrain, an environment, a particle file, the HUD layout, a face animation) writes are rewritable: a
// rename finds each in the text by reading it again (graph/native_text_sites.h).
#include <editor/graph/asset_graph.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <sstream>
#include <utility>
#include <variant>

#include <base/gameprofile/gameprofile.h>
#include <base/io/cp1252.h>
#include <base/io/strutil.h>
#include <base/vfs/vfs_decode.h>
#include <editor/documents/document_types.h>
#include <editor/documents/texture_roles.h>
#include <editor/graph/graph_names.h>
#include <editor/graph/reference_kinds.h>
#include <editor/model/diagnostic.h>
#include <editor/project/project_files.h>
#include <formats/avatars/avatars.h>
#include <formats/def/def.h>
#include <formats/lwf/lwf.h>
#include <formats/env/env.h>
#include <formats/grm/grm.h>
#include <formats/particle/parser.h>
#include <formats/trn/trn_io.h>
#include <runtime/renderer/particle_atlas.h>
#include <runtime/renderer/texture_load_rules.h>

namespace opennova::editor {

namespace {

using graph_names::is_style_reference;
using graph_names::symbol_name;

GraphEdge edge_of(const std::string &source, const std::string &record, const std::string &field, ReferenceKind kind,
                  const std::string &value, const std::string &scope = std::string(), bool rewritable = false) {
	GraphEdge edge;
	edge.source = source;
	edge.record = record;
	edge.field = field;
	edge.kind = kind;
	edge.value = value;
	edge.scope = scope;
	edge.rewritable = rewritable;
	return edge;
}

// A texture a native file names, used as `role` (ADR 0046 S18, texture_roles.h): its loader picks the
// file (GraphEdge::loader_arg), `flags` what the file's own content says of the use (it gates). A name
// the file writes is a site a rename rewrites in its text (graph/native_text_sites.h); one it derives
// (a flipbook's frame) is not.
GraphEdge texture_edge(const std::string &source, const std::string &record, const std::string &field,
                       const std::string &value, TextureRoleId role, int32_t flags = 0, bool written = true) {
	GraphEdge edge = edge_of(source, record, field, ReferenceKind::Texture, value, std::string(), written);
	edge.loader_arg = texture_role_arg(role, flags);
	return edge;
}

GraphSymbol symbol_of(ReferenceKind kind, const std::string &display, const std::string &file,
                      const std::string &record = std::string(), const std::string &scope = std::string()) {
	GraphSymbol symbol;
	symbol.kind = kind;
	symbol.display = display;
	symbol.name = symbol_name(kind, display);
	symbol.file = file;
	symbol.record = record;
	symbol.scope = scope;
	return symbol;
}

// The name a field's value gives: a text, or a number other than 0 (an item's id); "" for
// none.
std::string value_name(const Value &value) {
	if (const auto *text = std::get_if<std::string>(&value)) return *text;
	if (const auto *number = std::get_if<int64_t>(&value)) return *number ? std::to_string(*number) : std::string();
	return std::string();
}

// The same as the name of a reference or a definition of `kind`: a mission's zone id of 0 is a name
// too (an area trigger may hold it, and a parameter holding it where none does names an area the
// load does not find, the trigger then neutered [orig: EventTrigger_ResolveZoneTriggerRefs
// @0x453000]); no witness makes 0 a zone id that names none.
std::string value_name(ReferenceKind kind, const Value &value) {
	if (kind == ReferenceKind::MissionZone)
		if (const auto *number = std::get_if<int64_t>(&value)) return std::to_string(*number);
	return value_name(value);
}

// A record's references and the symbols it defines, each field as it applies to that record
// (Document::field_on: a menu STRING's value is a string id when its TYPE says so, an
// APPEARANCE's value a texture or a colour by its TYPE, an ACTION's target a screen or a
// window by its verb, a part's NAME defines nothing); a field the file leaves out (an
// optional one, one in a block left out, an empty one), or one the game does not read there
// (a FILE on an ACTION that is not SCREEN, a DATASOURCE on a window that is not a marquee),
// references and defines nothing. A symbol is found where its field's scope says, and the
// type says which definitions no lookup finds and what value one carries
// (Document::refine_symbol). Each field is asked of its record as a FieldUse, which copies
// nothing of the schema (an animation table key's 252 choices stay in the type's table).
void extract_record(const Document &document, const NodeAddress &address, Extracted &out) {
	if (!document.present(address, std::string())) return;
	std::string record, locator, identity;
	const auto place = [&] {
		if (!locator.empty()) return;
		record = document.record_path(address);
		locator = document.locator(address);
		identity = document.record_identity(address);
	};
	for (const FieldSchema &schema : document.fields(address.kind)) {
		const FieldUse field = document.field_on(address, schema);
		if (field.reference == ReferenceKind::None && field.defines == ReferenceKind::None &&
		    field.variable_through == ReferenceKind::None)
			continue;
		if (field.applies == Applicability::Ignored || !document.present(address, schema.id)) continue;
		Value value;
		if (!document.get(address, schema.id, value)) continue;
		const std::string defined = field.defines == ReferenceKind::None ? std::string() : value_name(field.defines, value);
		if (!defined.empty()) {
			place();
			GraphSymbol symbol = symbol_of(field.defines, defined, document.path(), record, field.scope);
			symbol.locator = locator;
			symbol.record_key = identity;
			symbol.address = address;
			symbol.field = schema.id;
			SymbolFacts facts;
			document.refine_symbol(address, facts);
			symbol.value = std::move(facts.value);
			symbol.inert = facts.inert;
			symbol.inert_reason = std::move(facts.inert_reason);
			symbol.line = facts.line;
			out.symbols.push_back(std::move(symbol));
		}
		ReferenceKind kind;
		std::string name, scope;
		if (!reference_target(field, value, kind, name, scope)) continue;
		place();
		GraphEdge edge = edge_of(document.path(), record, schema.id, kind, name, scope, !field.read_only);
		edge.locator = locator;
		edge.record_key = identity;
		edge.address = address;
		edge.loader_arg = field.loader_arg;
		edge.use_context = field.use_context;
		// A text that is one %NAME% stands for the variable's value: a use of the variable alone
		// (FieldUse::variable_through), which Rename rewrites with it.
		if (field.reference == ReferenceKind::None) edge.through = field.variable_through;
		out.edges.push_back(std::move(edge));
		// A menu's font or texture through a style variable is two references: the
		// variable, and the file it names once resolved.
		if (field.reference != ReferenceKind::None && field.reference != ReferenceKind::StyleVar &&
		    is_style_reference(name)) {
			GraphEdge var = edge_of(document.path(), record, schema.id, ReferenceKind::StyleVar, name, std::string(),
			                        !field.read_only);
			var.locator = locator;
			var.record_key = identity;
			var.address = address;
			var.through = field.reference; // what the variable's value must name here
			out.edges.push_back(std::move(var));
		}
	}
}

bool extract_environment(const std::string &name, const std::vector<uint8_t> &bytes, Extracted &out, Diagnostic &error) {
	std::istringstream input(std::string(bytes.begin(), bytes.end()));
	env::Config config;
	std::string message;
	if (!env::load_env(input, config, message)) {
		error = make_finding(CoreFinding::GraphUnreadable, DiagnosticSeverity::Error, message, name);
		return false;
	}
	auto edge = [&](const char *field, ReferenceKind kind, const std::string &value, int32_t loader_arg = -1) {
		if (value.empty()) return;
		out.edges.push_back(edge_of(name, std::string(), field, kind, value, std::string(), true));
		out.edges.back().loader_arg = loader_arg;
	};
	// The cloud layers, through ARCHIVE [orig: Terrain_InitRenderingResources @ 0x578A97], each name's
	// extension made PCX as the parser stores it [orig: TimeOfDay_ParseProperty @ 0x57CC41..0x57CC4B,
	// sky_map2's @ 0x57CC83..0x57CC8D] (kTextureArgPcx).
	for (const auto &[field, map] : {std::pair<const char *, const std::string *>{"sky_map1", &config.sky_map1},
	                                 {"sky_map2", &config.sky_map2}})
		if (!map->empty())
			out.edges.push_back(texture_edge(name, std::string(), field, *map, TextureRoleId::SkyCloud, kTextureArgPcx));
	edge("sun_3di", ReferenceKind::Model, config.sun_3di);
	edge("moon_3di", ReferenceKind::Model, config.moon_3di);
	edge("glare_3di", ReferenceKind::Model, config.glare_3di);
	edge("star_3di", ReferenceKind::Model, config.star_3di);
	return true;
}

// The HUD layout (hudpos.def, ADR 0046 S14): the two fonts the HUD draws its text with, each
// stance's icon, the static frames, the two status icons, the weapon bar's two textures and each
// vehicle panel's three, by the names the HUD's loaders are handed [orig: HUD_ParseHudposToken
// @0x59F370].
bool extract_hudpos(const std::string &name, const std::vector<uint8_t> &bytes, Extracted &out, Diagnostic &error) {
	def::DefHudPosFile file{};
	if (def::def_parse_hudpos_memory(bytes.data(), bytes.size(), &file) != 0) {
		error = make_finding(CoreFinding::GraphUnreadable, DiagnosticSeverity::Error, "The HUD layout could not be read.", name);
		return false;
	}
	const def::DefHudPosDef &hud = file.hud;
	auto edge = [&](const std::string &record, const char *field, ReferenceKind kind, const char *value,
	                int32_t loader_arg = -1) {
		if (!value || !*value) return;
		out.edges.push_back(edge_of(name, record, field, kind, value, std::string(), true));
		out.edges.back().loader_arg = loader_arg;
	};
	// A texture through the HUD's loader, in the mode its keyword loads it in (ADR 0046 S18): a stance's
	// icon and the parachute and armour icons alpha only, the static frame and the loadout's two in
	// colour [orig: HUD_LoadAllTextures @ 0x59DDA0, HUD_LoadImageAsTexture @ 0x591550; record
	// interface/hud-re].
	auto texture = [&](const std::string &record, const char *field, TextureRoleId role, const char *value) {
		if (value && *value) out.edges.push_back(texture_edge(name, record, field, value, role));
	};
	edge(std::string(), "fonthud1_hi", ReferenceKind::Font, hud.font_hi);
	edge(std::string(), "fonthud1_lo", ReferenceKind::Font, hud.font_lo);
	// A stance's icon is its slot's (ids 0 to 5), the last record of an id the one read; the static
	// frame is the last line authored (runtime/hud/hud_frame.h, hud_static_frame_index).
	for (int id = 0; id < 6; ++id) {
		const def::DefHudStance *read = nullptr;
		for (size_t i = 0; i < hud.stances_count; ++i)
			if (hud.stances[i].id == id) read = &hud.stances[i];
		if (read) texture("HUDSTANCE " + std::to_string(id), "texture", TextureRoleId::HudAlphaOnly, read->texture);
	}
	if (hud.static_frames_count > 0)
		texture("StaticFrame", "texture", TextureRoleId::HudColour, hud.static_frames[hud.static_frames_count - 1].texture);
	texture(std::string(), "parachute_icon", TextureRoleId::HudAlphaOnly, hud.parachute_icon.texture);
	texture(std::string(), "armor_icon", TextureRoleId::HudAlphaOnly, hud.armor_icon.texture);
	texture(std::string(), "hudls_bracket", TextureRoleId::HudColour, hud.hudls_bracket);
	texture(std::string(), "hudls_moreav", TextureRoleId::HudColour, hud.hudls_moreav);
	// A vehicle panel's interface art through the HUD loader in alpha mode (render-material-re.md "The
	// game's texture loaders"); its icon and static texture: no load of either is witnessed, the name as
	// written.
	for (size_t i = 0; i < hud.vehicle_huds_count; ++i) {
		const def::DefVehicleHudBlock &vehicle = hud.vehicle_huds[i];
		const std::string record = std::string("VEHICLE_HUD ") + vehicle.sid;
		edge(record, "icon", ReferenceKind::Texture, vehicle.icon);
		texture(record, "interface", TextureRoleId::HudAlphaOnly, vehicle.interface_texture);
		edge(record, "statictexture", ReferenceKind::Texture, vehicle.static_texture);
	}
	def::def_free_hudpos(&file);
	return true;
}

// A terrain (.trn, ADR 0046 S14): its height data, the maps and detail textures its keys name, its
// tile atlas and each foliage block's model [orig: Terrain_ParseConfigCallback @0x60f330]. A config
// the game refuses (load_trn's admission gate) is one the graph does not read. Two files a terrain
// has no edge to: the atlas's .TSD twin, optional and in no shipped game (formats/til/til_tsd.h),
// and its tile placement, which the game finds by the mission's name (mission::sidecars).
bool extract_terrain(const std::string &name, const std::vector<uint8_t> &bytes, Extracted &out, Diagnostic &error) {
	std::istringstream input(std::string(bytes.begin(), bytes.end()));
	TrnConfig config;
	std::string message;
	if (!load_trn(input, config, message)) {
		error = make_finding(CoreFinding::GraphUnreadable, DiagnosticSeverity::Error, message, name);
		return false;
	}
	auto edge = [&](const std::string &record, const char *field, ReferenceKind kind, const std::string &value,
	                int32_t loader_arg = -1) {
		if (value.empty()) return;
		out.edges.push_back(edge_of(name, record, field, kind, value, std::string(), true));
		out.edges.back().loader_arg = loader_arg;
	};
	edge(std::string(), "polytrn_polydata", ReferenceKind::TerrainData, config.polydata);
	// Each map by its role's loader (ADR 0046 S18, the terrain's keys [orig: PolyTrn_InitTextures @
	// 0x60AAA0]). Without its colour map the game logs "colormap" @ 0x60B389; without its blend map, once
	// the key names one at all (the key alone sets the blend on [orig: Terrain_ParseConfigCallback @
	// 0x60F7D0], and every card with pixel shaders takes it, PolyTrn_InitTextures @ 0x60B15D..0x60B176),
	// "blendermap" @ 0x60B19A. Either error aborts the mission [orig: sub_520AA0 @ 0x520B4E], so either
	// missing refuses a build.
	auto texture = [&](const char *field, TextureRoleId role, const std::string &value, int32_t flags = 0) {
		if (!value.empty()) out.edges.push_back(texture_edge(name, std::string(), field, value, role, flags));
	};
	texture("polytrn_colormap", TextureRoleId::TerrainColourMap, config.colormap, kTextureArgGates);
	texture("polytrn_detailmap", TextureRoleId::TerrainDetailCoefficient, config.detailmap);
	texture("polytrn_detailmap_c1", TextureRoleId::TerrainSplatDetail, config.detailmap_c1);
	texture("polytrn_detailmap_c2", TextureRoleId::TerrainSplatDetail, config.detailmap_c2);
	texture("polytrn_detailmap_c3", TextureRoleId::TerrainSplatDetail, config.detailmap_c3);
	texture("polytrn_detailmap2", TextureRoleId::TerrainSecondDetail, config.detailmap2);
	texture("polytrn_detailmapdist", TextureRoleId::TerrainFarDetail, config.detailmapdist);
	texture("polytrn_detailmapdist2", TextureRoleId::TerrainFarDetail, config.detailmapdist2);
	texture("polytrn_detailblendmap", TextureRoleId::TerrainBlendMap, config.detailblendmap, kTextureArgGates);
	texture("polytrn_tilestrip", TextureRoleId::TerrainTileAtlas, config.tilestrip);
	texture("polytrn_charmap", TextureRoleId::TerrainCharMap, config.charmap);
	texture("polytrn_foliagemap", TextureRoleId::TerrainFoliageMap, config.foliagemap);
	for (size_t i = 0; i < config.foliage_defs.size(); ++i)
		edge("foliage " + std::to_string(i + 1), "graphic", ReferenceKind::Model, config.foliage_defs[i].graphic);
	// The terrain's own tile placement, read when a mission has none of its name (S20) [orig: Terrain_Init
	// @ 0x60FCFD; PolyTrn_LoadTerrainConfig @ 0x60E6DC..0x60E6E5, its extension forced to TIL].
	edge(std::string(), "polytrn_tileinfo", ReferenceKind::TilePlacement, config.tileinfo);
	return true;
}

// A sound bank (.lwf, ADR 0046 S14): the wave each of its singles names (formats/lwf).
bool extract_sound_bank(const std::string &name, const std::vector<uint8_t> &bytes, Extracted &out, Diagnostic &error) {
	lwf::File bank;
	std::string message;
	if (!lwf::parse_lwf_buffer(bytes.data(), bytes.size(), bank, message)) {
		error = make_finding(CoreFinding::GraphUnreadable, DiagnosticSeverity::Error, message, name);
		return false;
	}
	for (const lwf::Single &single : bank.singles)
		if (!single.path.empty()) out.edges.push_back(edge_of(name, single.name, "wave", ReferenceKind::Wave, single.path));
	return true;
}

bool extract_avatars(const std::string &name, const std::vector<uint8_t> &bytes, Extracted &out, Diagnostic &error) {
	avatars::AvatarsFile file{};
	if (avatars::avatars_parse_memory(bytes.data(), bytes.size(), &file) != 0) {
		error = make_finding(CoreFinding::GraphUnreadable, DiagnosticSeverity::Error, "The avatar table could not be read.", name);
		return false;
	}
	for (size_t i = 0; i < file.parts_count; ++i) {
		const avatars::AvatarPart &part = file.parts[i];
		const std::string record(part.name);
		auto edge = [&](const char *field, ReferenceKind kind, const char *value) {
			if (value && *value) out.edges.push_back(edge_of(name, record, field, kind, value));
		};
		edge("graphic", ReferenceKind::Model, part.graphic);
		edge("graphic_j", ReferenceKind::Model, part.graphic_j);
		edge("graphic_s", ReferenceKind::Model, part.graphic_s);
		edge("name", ReferenceKind::TextId, part.display_name);
	}
	avatars::avatars_free(&file);
	return true;
}

bool extract_particles(const std::string &name, const std::vector<uint8_t> &bytes, Extracted &out, Diagnostic &error) {
	particle::ParticleFile file;
	particle::ParseError parse_error;
	if (!particle::load_particles_from_buffer(reinterpret_cast<const char *>(bytes.data()), bytes.size(), file, parse_error)) {
		error = make_finding(CoreFinding::GraphUnreadable, DiagnosticSeverity::Error, "The particle file could not be read.", name);
		return false;
	}
	for (const particle::EffectDef &effect : file.effects)
		if (!effect.id.empty()) out.symbols.push_back(symbol_of(ReferenceKind::Particle, effect.id, name, effect.id));
	for (const particle::ParticleDef &definition : file.particles) {
		for (size_t g = 0; g < definition.graphics.size(); ++g) {
			const particle::GraphicLayer &layer = definition.graphics[g];
			if (!layer.present || layer.texture.empty()) continue;
			const std::string field = "graphic" + std::to_string(g + 1);
			// A particle's graphic loads through the particle manager's TGA loader
			// (renderer::TextureLoader::Particle). A flipbook layer loads a file a frame, named
			// from the graphic's (its stem, lower case, and the frame's number), and never the
			// graphic's own name [orig: CParticleDef_ReloadGraphicFrameTextures @0x5e4bb0]: each
			// frame is a reference of its own (ADR 0046 S14).
			const int frames = std::clamp(layer.flip_frames, 1, particle::kMaxParticleFlipFrames);
			// Each is packed into the particle atlas, a TGA alone (ADR 0046 S18 [orig:
			// CParticleTextureEntry_ProbeSizeFromDisk @ 0x5DFAA0]), on a page whose side its graphic's mode
			// picks (the edge carries the mode, GraphEdge::use_context: renderer::particle_atlas_page_side).
			const uint32_t mode = uint32_t(layer.blend_mode);
			if (frames <= 1) {
				out.edges.push_back(texture_edge(name, definition.id, field, layer.texture, TextureRoleId::ParticleGraphic));
				out.edges.back().use_context = mode;
				continue;
			}
			for (int frame = 1; frame <= frames; ++frame) {
				out.edges.push_back(texture_edge(name, definition.id, field + "[" + std::to_string(frame) + "]",
				                                 renderer::retail_particle_frame_name(layer.texture, frames, frame),
				                                 TextureRoleId::ParticleGraphic, 0, false));
				out.edges.back().use_context = mode;
			}
		}
	}
	return true;
}

// A face animation (.grm, ADR 0046 S18): its base texture, the base's .MDT twin and its two eye textures,
// each by STAGE under its name with its path stripped and its extension (from the last '.') made .TGA, the
// twin's .MDT [orig: Shadow_DecalLoadTextures @ 0x588040: PathStripPathA, PathRemoveExtensionA, then
// PathAddExtensionA ".TGA" @ 0x5880EA, ".MDT" @ 0x588117, the eyes @ 0x58814A (+520), @ 0x588180 (+260),
// each through Texture_LoadByNameWithChannel]. A name the file writes so is a site a rename rewrites; one
// the loader derives (another extension, the twin) is not.
bool extract_face_animation(const std::string &name, const std::vector<uint8_t> &bytes, Extracted &out, Diagnostic &error) {
	grm::File file;
	std::string message;
	if (!grm::parse(bytes.data(), bytes.size(), file, message)) {
		error = make_finding(CoreFinding::GraphUnreadable, DiagnosticSeverity::Error, message, name);
		return false;
	}
	const auto loaded = [](const std::string &written, const char *extension) {
		std::string stem = basename_of(written);
		const size_t dot = stem.find_last_of('.');
		if (dot != std::string::npos) stem.erase(dot);
		return stem + extension;
	};
	const auto texture = [&](const std::string &record, const char *field, const std::string &written, const char *extension) {
		if (written.empty()) return;
		const std::string opened = loaded(written, extension);
		const bool as_written = strutil::iequals(opened, written);
		out.edges.push_back(texture_edge(name, record, field, as_written ? written : opened, TextureRoleId::FaceTexture, 0,
		                                 as_written));
	};
	texture(std::string(), "basetexture", file.base_texture, ".TGA");
	texture(std::string(), "basetexture.mdt", file.base_texture, ".MDT");
	texture("eye 1", "eyetexture", file.eye_textures[0], ".TGA");
	texture("eye 2", "eyetexture", file.eye_textures[1], ".TGA");
	return true;
}

// The kinds the graph reads through the engine's own parser, not a document type.
using NativeExtractor = bool (*)(const std::string &name, const std::vector<uint8_t> &bytes, Extracted &out,
                                 Diagnostic &error);
struct NativeKind {
	AssetKind kind;
	NativeExtractor extract;
};
constexpr NativeKind kNativeKinds[] = {
	{AssetKind::HudPosDefs, extract_hudpos},
	{AssetKind::Terrain, extract_terrain},
	{AssetKind::SoundBank, extract_sound_bank},
	{AssetKind::Environment, extract_environment},
	{AssetKind::AvatarDefs, extract_avatars},
	{AssetKind::Particles, extract_particles},
	{AssetKind::FaceAnimation, extract_face_animation},
};

NativeExtractor native_extractor(AssetKind kind) {
	for (const NativeKind &native : kNativeKinds)
		if (native.kind == kind) return native.extract;
	return nullptr;
}

// A native kind's file read through the engine's own parser, from its bytes decoded as the game's loader
// decodes them (or a text document's text, which its load decoded): false when the parser does not read
// it. A native file holds its names in the game's code page (Windows-1252), the project's file names and
// every document's names are UTF-8: its names compared in one encoding, UTF-8 (the plain-words lane; a
// rename of a native text's name writes it back in its code page, native_text_sites).
bool extract_native(NativeExtractor extract, const std::string &name, const std::vector<uint8_t> &decoded, Extracted &out,
                    Diagnostic &error) {
	const size_t edges = out.edges.size(), symbols = out.symbols.size();
	const bool read = extract(name, decoded, out, error);
	for (size_t i = edges; i < out.edges.size(); ++i) {
		GraphEdge &edge = out.edges[i];
		edge.value = cp1252_to_utf8(edge.value);
		edge.record = cp1252_to_utf8(edge.record);
		edge.fallback = cp1252_to_utf8(edge.fallback);
	}
	for (size_t i = symbols; i < out.symbols.size(); ++i) {
		GraphSymbol &symbol = out.symbols[i];
		symbol.display = cp1252_to_utf8(symbol.display);
		symbol.name = symbol_name(symbol.kind, symbol.display);
		symbol.record = cp1252_to_utf8(symbol.record);
	}
	return read;
}

// Whether a type's documents are what the graph reads of their file: a record type's, or a text type's
// whose text names references (DocumentType::references). A text type whose text names none (the text
// type over a native kind, DI-06) leaves its file's reading to the kind's native extractor.
bool read_through_document(const DocumentType &type) {
	const DocumentContent content = document_content(type);
	return content == DocumentContent::Records || (content == DocumentContent::Text && type.references);
}

} // namespace

ReferenceKind value_reference(const FieldUse &field, const Value &value) {
	if (field.reference != ReferenceKind::None) return field.reference;
	return field.variable_through != ReferenceKind::None && is_style_reference(value_name(value)) ? ReferenceKind::StyleVar
	                                                                                                : ReferenceKind::None;
}

bool reference_target(const FieldUse &field, const Value &value, ReferenceKind &kind, std::string &name,
                      std::string &scope) {
	kind = value_reference(field, value);
	scope.clear();
	name.clear();
	if (kind == ReferenceKind::None) return false;
	// A record of the field's own file by its index (S13 D8): a whole number naming one, 0 a
	// record like any other, in the file its scope names (Document::field_on's).
	if (reference_row(kind).resolution == ReferenceResolution::Record) {
		int64_t index = 0;
		if (!record_index(kind, value, index)) return false;
		name = std::to_string(index);
		scope = field.scope;
		return true;
	}
	name = value_name(kind, value);
	// A text's whole %NAME% names the variable, which has no scope (the field's is what it defines).
	if (field.reference == ReferenceKind::None) return true;
	if (name.empty()) return false;
	const std::string normalized = graph_names::key(name);
	if (normalized == "NONE" || normalized == "NULL") return false;
	if (kind == ReferenceKind::StyleVar && !is_style_reference(name)) return false; // a literal color, not a reference
	scope = field.scope; // where the document type says the record's reference resolves
	return true;
}

void extract_from_text(const TextDocument &document, Extracted &out) {
	const DocumentType *type = document_type_for(document.kind());
	if (!type) return;
	// The names the text defines (a shader's tags), each a symbol defined at its span.
	if (type->definitions) {
		std::vector<TextDefinition> definitions;
		type->definitions(document, definitions);
		for (const TextDefinition &definition : definitions) {
			GraphSymbol symbol = symbol_of(definition.kind, cp1252_to_utf8(definition.name), document.path());
			symbol.locator = TextDocument::locator(definition.span.line, definition.span.column);
			out.symbols.push_back(std::move(symbol));
		}
	}
	// A native kind held as a text (DI-06: a particle file, an environment, the HUD layout, the avatars):
	// its text as it stands read by the engine's own parser, as its file is, so what it names follows its
	// edits; a text the parser does not read names nothing (its own validation says why).
	if (!type->references) {
		if (const NativeExtractor extract = native_extractor(document.kind())) {
			Diagnostic error;
			const std::string &text = document.text();
			extract_native(extract, document.path(), std::vector<uint8_t>(text.begin(), text.end()), out, error);
		}
		return;
	}
	std::vector<TextReference> references;
	type->references(document, references);
	for (TextReference &reference : references) {
		// A name its row reads as none (an empty one, NONE, NULL) is no reference, as a field's.
		const std::string normalized = graph_names::key(reference.value);
		if (reference.value.empty() || normalized == "NONE" || normalized == "NULL") continue;
		// The text is the game's code page (Windows-1252), the project's file names and every other
		// document's names UTF-8: a name compared in one encoding, UTF-8 (the plain-words lane; its span
		// keeps the text's own bytes, which a rename rewrites).
		GraphEdge edge = edge_of(document.path(), std::string(), std::string(), reference.kind,
				cp1252_to_utf8(reference.value), reference.scope, reference.rewritable);
		edge.locator = TextDocument::locator(reference.span.line, reference.span.column);
		edge.span = reference.span;
		edge.fallback = cp1252_to_utf8(reference.fallback);
		edge.scopes_after = std::move(reference.scopes_after);
		edge.scope_alternate = std::move(reference.scope_alternate);
		edge.scope_owner = std::move(reference.scope_owner);
		out.edges.push_back(std::move(edge));
	}
}

void extract_from_document(const Document &document, Extracted &out) {
	for (const auto &row : document.rows()) {
		if (!row) continue;
		extract_record(document, {row->id, row->kind, 0}, out);
		document.walk_records(*row, [&](const NodeAddress &nested, const Document::Placement &) {
			extract_record(document, nested, out);
			return true;
		});
	}
	// The type's references that no field's value is (DocumentType::record_references, S14).
	if (const DocumentType *type = document_type_for(document.kind()); type && type->record_references)
		type->record_references(document, out);
	// The record sets (S13 D8): each record of a collection another record names by index, a symbol
	// of the Record kind named by its index in the file (Document::record_sets, the file's order),
	// scoped to the file and defined by no field, its value the record's own name (a register's
	// NAME), which the picker shows and filters by beside the index.
	const std::vector<Document::TargetedCollection> &targets = document.targeted_collections();
	const std::vector<std::vector<NodeAddress>> sets = document.record_sets();
	for (size_t t = 0; t < targets.size(); ++t)
		for (size_t k = 0; k < sets[t].size(); ++k) {
			const NodeAddress &address = sets[t][k];
			GraphSymbol symbol = symbol_of(targets[t].reference, std::to_string(k), document.path(),
			                               document.record_path(address), document.path());
			symbol.locator = document.locator(address);
			symbol.address = address;
			symbol.value = document.own_name(address);
			out.symbols.push_back(std::move(symbol));
		}
	// What the type's own references and the record sets name their records by as themselves too (the
	// fields' edges have theirs): each record's identity worked out once.
	std::map<std::pair<NodeId, NodeId>, std::string> identities;
	const auto identity_of = [&](const NodeAddress &address) -> const std::string & {
		const auto found = identities.find({address.row, address.child});
		if (found != identities.end()) return found->second;
		return identities[{address.row, address.child}] = document.record_identity(address);
	};
	for (GraphEdge &edge : out.edges)
		if (edge.address.row && edge.record_key.empty()) edge.record_key = identity_of(edge.address);
	for (GraphSymbol &symbol : out.symbols)
		if (symbol.address.row && symbol.record_key.empty()) symbol.record_key = identity_of(symbol.address);
	// Each record in its type's own words where they are not its name (record_own_title), worked out once:
	// what a place apart from the document names it by (a closed file's Problems row, a find's uses, the
	// import plan; the plain-words lane).
	std::map<std::pair<NodeId, NodeId>, std::string> titles;
	const auto title_of = [&](const NodeAddress &address) -> const std::string & {
		const auto found = titles.find({address.row, address.child});
		if (found != titles.end()) return found->second;
		return titles[{address.row, address.child}] = record_own_title(document, address);
	};
	for (GraphEdge &edge : out.edges)
		if (edge.address.row) edge.record_title = title_of(edge.address);
	for (GraphSymbol &symbol : out.symbols)
		if (symbol.address.row) symbol.title = title_of(symbol.address);
}

bool graph_reads_kind(AssetKind kind) {
	// A record type's documents, whose records the extraction reads, a text type's whose text names
	// references (S13 D9), or a native extractor; any other type's documents give the graph nothing
	// (S13 D6).
	const DocumentType *type = document_type_for(kind);
	if (type) {
		const DocumentContent content = document_content(*type);
		if (content == DocumentContent::Records) return true;
		if (content == DocumentContent::Text && (type->references || type->definitions)) return true;
	}
	return native_extractor(kind) != nullptr;
}

bool extract_from_bytes(const std::string &name, AssetKind kind, const std::vector<uint8_t> &bytes,
                        const std::string &game, Extracted &out, Diagnostic &error) {
	if (!graph_reads_kind(kind))
		return true;
	// A record type's document, its records extracted; a text type's whose text names references, its
	// text's; any other kind (a text type over a native kind among them, DI-06) falls through to a native
	// extractor, or gives nothing.
	if (const DocumentType *type = document_type_for(kind); type && read_through_document(*type)) {
		const std::unique_ptr<DocumentBase> document = type->make();
		if (!document->load_bytes(bytes, name, kind, game, error)) return false;
		if (const Document *records = records_of(*document))
			extract_from_document(*records, out);
		else
			extract_from_text(*text_of(*document), out);
		return true;
	}
	const NativeExtractor extract = native_extractor(kind);
	if (!extract) return true;
	// Decoded as the game's loader decodes a stored file (a document decodes its own).
	std::vector<uint8_t> decoded = bytes;
	vfs_decode_payload(decoded, gameprofile::gameprofile_scr_policy_for_code(game.c_str()));
	return extract_native(extract, name, decoded, out, error);
}

bool extract_from_asset(const ProjectPaths &paths, const ProjectDocument &project, const AssetEntry &asset,
                        Extracted &out, Diagnostic &error) {
	std::vector<uint8_t> bytes;
	std::string message;
	// A project's files from its folder; a source's (the game install's) by their logical names.
	const bool read = paths.files ? paths.files->read(asset.logical_name, bytes)
	                              : read_file_bytes(join_path(paths.root, asset.relative_path), bytes, message);
	if (!read) {
		if (message.empty()) message = asset.logical_name + " could not be read.";
		error = make_finding(CoreFinding::GraphUnreadable, DiagnosticSeverity::Error, message, asset.relative_path);
		return false;
	}
	return extract_from_bytes(asset.relative_path, asset.kind, bytes, project.target_game, out, error);
}

} // namespace opennova::editor
