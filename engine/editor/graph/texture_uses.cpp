#include <editor/graph/texture_uses.h>

#include <algorithm>
#include <cctype>
#include <iterator>
#include <map>
#include <set>

#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>
#include <editor/graph/asset_graph.h>
#include <editor/model/document.h>
#include <editor/project/project_files.h>
#include <formats/particle/particle.h>
#include <runtime/hud/hud_texture_names.h>
#include <runtime/renderer/material_texture.h>
#include <runtime/renderer/precipitation_frame.h>
#include <runtime/renderer/tracer_frame.h>
#include <runtime/renderer/water_wake_frame.h>
#include <runtime/terrain/terrain_scorch.h>
#include <runtime/world/impact_scar.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;
using R = TextureRoleId;

int64_t whole(const Value &value) {
	if (const int64_t *i = std::get_if<int64_t>(&value)) return *i;
	if (const double *d = std::get_if<double>(&value)) return int64_t(*d);
	return 0;
}

std::string text(const Value &value) {
	const std::string *s = std::get_if<std::string>(&value);
	return s ? *s : std::string();
}

// A model row's use: its record's slot, type and flags, its material's shader, flags and reference (the
// model document's texture and material records, model_table's fields), and the role the dispatcher's
// loader makes it by its runtime type [orig: Material_LoadStageTexture @ 0x5B16F0, the switch @
// 0x5B1737; the type the loader copies, Material_ConvertDefinition @ 0x5B045B..0x5B04A0].
TextureRoleId model_role(const GraphEdge &edge, const Document *model, TextureUseContext &context) {
	context.type = texture_arg_is_row_type(edge.loader_arg) ? uint8_t(edge.loader_arg) : 0;
	// What the edge carries of the row (TextureRowContext); the document, where it reads, adds the
	// material's place and shader.
	const TextureRowContext row = unpack_texture_row_context(edge.use_context);
	context.slot = row.slot;
	context.row_flags = row.row_flags;
	context.material_flags = row.material_flags;
	context.alpha_ref = row.alpha_ref;
	if (model) {
		const NodeAddress row = model->address_at(edge.locator);
		Value value;
		if (row.row && model->get(row, "slot", value)) context.slot = uint8_t(whole(value));
		if (row.row && model->get(row, "type", value)) context.type = uint8_t(whole(value));
		if (row.row && model->get(row, "flags", value)) context.row_flags = uint8_t(whole(value));
		Document::Placement at;
		if (row.row && model->placement(row, at) && at.owner.row) {
			if (model->get(at.owner, "shader", value)) context.shader = text(value);
			// The alpha test as it falls on this row (texture_roles.h texture_row_material_flags).
			if (model->get(at.owner, "flags", value))
				context.material_flags = texture_row_material_flags(context.shader, uint8_t(whole(value)), context.type, context.slot);
			if (model->get(at.owner, "alpha_test", value)) context.alpha_ref = uint8_t(whole(value));
			Document::Placement material;
			if (model->placement(at.owner, material)) context.material = int(material.index);
		}
	}
	switch (renderer::material_texture_runtime_type(context.type)) {
	case 0:
	case 2:
	case 8:
		if (context.slot == 2) return R::ModelDetail;
		return (context.row_flags & 0x01) ? R::ModelFlipFrame : R::ModelDiffuse;
	case 1: return R::ModelPlain;
	case 4:
	case 5:
		// The .mdt as it is; a .tga converted from its height [orig: Texture_LoadAsNormalMap @ 0x58C480].
		return strutil::to_upper(edge.value).find(".MDT") != std::string::npos ? R::ModelNormalMap : R::ModelHeightNormal;
	case 6: return R::ModelHorizon;
	case 7: return R::ModelOcclusion;
	case 16:
	case 17:
	case 18: return R::ModelChunk;
	default: break;
	}
	return R::ModelDiffuse;
}

std::vector<FixedTextureName> collect_fixed() {
	std::vector<FixedTextureName> out;
	std::set<std::string> seen;
	const auto add = [&](const std::string &name, R role, const char *what, const char *witness,
	                     TextureLoader loader = TextureLoader::kCount, int hud_mode = -1) {
		if (name.empty() || !seen.insert(normalized_logical_name(name)).second) return;
		FixedTextureName row;
		row.name = name;
		row.role = role;
		row.what = what;
		row.witness = witness;
		row.loader = loader;
		row.hud_mode = hud_mode;
		out.push_back(std::move(row));
	};
	// The HUD's own art [orig: HUD_LoadAllTextures @ 0x59DDA0] (runtime/hud/hud_texture_names.h): the
	// scoreboard's box, the connection indicators, the tip panel, the map's art through FILE, the rest
	// the HUD's colour art.
	const std::string map_icons[] = {"TSDicon.tga", "WPIndctr.tga", "JO_LFP.tga", "R_LFP.tga", "N_LFP.tga"};
	for (const hud::HudFixedTexture &texture : hud::kHudFixedTextures) {
		const std::string name = texture.name;
		const bool map = std::find(std::begin(map_icons), std::end(map_icons), name) != std::end(map_icons);
		const std::string lower = strutil::to_lower(name);
		if (lower == "border.tga" || lower == "boxtile.tga")
			add(name, R::BoardBox, "for the scoreboard's box", "BoxTexture_LoadAndSetupUVRegions @ 0x56ACD0");
		else if (lower.rfind("neticon", 0) == 0)
			add(name, R::NetIcon, "for the connection indicators", "CNetworkIcons_LoadTextures @ 0x4C2CF0");
		// The tip panel's box through the box loader (the TGA reader, no .dds tried); its two pictures by STAGE.
		else if (lower == "border3.tga")
			add(name, R::BoardBox, "for the tip panel's box", "CTipSystem_Init @ 0x5B6970 (BoxTexture_LoadAndSetupUVRegions @ 0x56ACD0)");
		else if (lower == "k_tip.tga" || lower == "g_tip.tga")
			add(name, R::TipArt, "for the tip panel", "CTipSystem_Init @ 0x5B6970");
		else if (map)
			add(name, R::HudFileArt, "for the HUD's map", "HUD_LoadAllTextures @ 0x59E060..0x59E0F3");
		else
			add(name, R::HudColour, "for the HUD", "HUD_LoadAllTextures @ 0x59DDA0", TextureLoader::kCount, 0);
	}
	// The scoreboard box's third texture [orig: Game_StartMission @ 0x525AA3, through sub_56AB00 and the box
	// loader].
	add("monogram.tga", R::BoardBox, "for the scoreboard's box", "Game_StartMission @ 0x525AA3 (BoxTexture_LoadAndSetupUVRegions @ 0x56ACD0)");
	add(hud::kHudCargoFlagTexture, R::HudColour, "for the HUD's carried flag", "HUD_LoadAllTextures @ 0x59DE53",
	    TextureLoader::kCount, 0);
	add(hud::kHudCargoDocumentTexture, R::HudColour, "for the HUD's carried item", "HUD_LoadAllTextures @ 0x59DE64",
	    TextureLoader::kCount, 0);
	for (int style = hud::kHudCrosshairStyleMin; style <= hud::kHudCrosshairStyleMax; ++style)
		add(hud::hud_crosshair_texture_name(style), R::HudColour, "for a crosshair style", "HUD_LoadAllTextures @ 0x59E3D6",
		    TextureLoader::kCount, 0);
	add("scopexh.tga", R::HudAlphaOnly, "for the scope's crosshair", "HUD_LoadAllTextures @ 0x59DDA0",
	    TextureLoader::kCount, 1);
	add(hud::kViewEffectTextureNames[hud::kViewTexBinocularMask], R::ViewEffect, "for the binoculars",
	    "ViewFx_InitShadersAndTextures @ 0x5CFDB8");
	add(hud::kViewEffectTextureNames[hud::kViewTexBinocularCrosshair], R::ViewEffect, "for the binoculars",
	    "ViewFx_InitShadersAndTextures @ 0x5CFDF3");
	add(hud::kViewEffectTextureNames[hud::kViewTexBinocularDigits], R::HudFileArt, "for the binoculars' range digits",
	    "HUD_LoadAllTextures @ 0x59E109");
	add(hud::kViewEffectTextureNames[hud::kViewTexNvgMask], R::ViewEffect, "for the night vision",
	    "ViewFx_InitShadersAndTextures @ 0x5CFE18");
	add(hud::kViewEffectTextureNames[hud::kViewTexNvgScale], R::ViewEffect, "for the night vision's scale",
	    "ViewFx_InitShadersAndTextures @ 0x5CFE4A", TextureLoader::File);
	add(hud::kViewEffectTextureNames[hud::kViewTexVignette], R::ViewEffect, "for the damage vignette", "sub_5C36B0 @ 0x5C36BC",
	    TextureLoader::Archive);
	add(renderer::kRainTexture, R::WeatherDrop, "for rain", "WeatherParticle_LoadTextures @ 0x5DE840");
	add(renderer::kSnowTexture, R::WeatherDrop, "for snow", "WeatherParticle_LoadTextures @ 0x5DE88E");
	add(renderer::kEmitterPoolTexture, R::TracerSmoke, "for smoke trails", "CEffectEmitterPool_CreateShaders @ 0x5DC8F0");
	add(renderer::kWakeTexture, R::WaterWake, "for the water rings", "WaterRing_LoadResources @ 0x5DDC90");
	add(renderer::kWakeGradientTexture, R::WaterWake, "for the water rings", "WaterRing_LoadResources @ 0x5DDC90");
	for (int strip = 0; strip < world::kScarTextureStripCount; ++strip)
		add(world::scar_texture_strip_name(strip), R::ImpactScar, "for an impact scar", "Scar_LoadTextures @ 0x5CC2E0");
	for (int index = 0; index <= 255; ++index)
		add(std::string(terrain::terrain_scorch_texture_name(static_cast<uint8_t>(index))), R::TerrainScorch,
		    "for a scorch mark", "Terrain_LoadScorchTextures @ 0x604CE0");
	add("newarow1.tga", R::SplashCursor, "for the start-mission cursor", "Game_ShowStartMissionSplash @ 0x520820");
	add("HwmCube.dds", R::PreviewCube, "for the player preview's reflection", "PlayerInfo_InitPreviewModel @ 0x56010C");
	add("loading.pcx", R::BootSplash, "for the boot splash", "Game_ShowLoadingScreen @ 0x4A5420");
	add("loadscrn.pcx", R::LoadingScreen, "for a mission with no loading screen of its own", "Render_LoadingScreen @ 0x521D10");
	add("MFD1.PCX", R::HudMfd, "for the vehicles' MFD", "sub_59B120 @ 0x59B120");
	return out;
}

// What a model use's material makes of it, in words.
std::string model_words(const TextureUse &use) {
	const TextureUseContext &c = use.context;
	std::string out = c.shader;
	if ((use.role == R::ModelDiffuse || use.role == R::ModelFlipFrame || use.role == R::ModelNormalMap) && c.alpha_test()) {
		const std::string test = std::string(c.alpha_test_inverted() ? "cut-out at or below " : "cut-out above ") +
		                         std::to_string(c.alpha_ref);
		out = out.empty() ? test : out + ", " + test;
	}
	return out;
}

std::string use_words(const TextureUse &use) {
	std::string role = use.known() ? texture_role_row(use.role).words : "a use whose loader is not known yet";
	if (!role.empty()) role[0] = char(std::toupper(static_cast<unsigned char>(role[0])));
	if (use.fixed) return role + ": the game opens it by name " + use.fixed_for;
	std::string where = basename_of(use.referrer);
	if (use.context.material >= 0) where = "material " + std::to_string(use.context.material + 1) + " of " + where;
	else if (!use.record.empty()) where = use.record + " in " + where;
	std::string out = role + ": " + where;
	if (use.context.material < 0 && !use.field.empty()) out += " (" + use.field + ")";
	const std::string context = model_words(use);
	if (use.context.material >= 0 && !context.empty()) out += " (" + context + ")";
	return out;
}

} // namespace

const std::vector<FixedTextureName> &fixed_texture_names() {
	static const std::vector<FixedTextureName> names = collect_fixed();
	return names;
}

TextureRoleId texture_role_of_edge(const GraphEdge &edge, const Document *model, TextureUseContext &context) {
	context.key = edge.field;
	if (edge.kind == ReferenceKind::MenuTexture) {
		if (edge.field == "frame.stencil") return R::MenuFrameStencil;
		if (edge.field == "frame.brush") return R::MenuFrameBrush;
		if (edge.field == "cursor.file") return R::MenuCursor;
		return R::MenuImage;
	}
	if (edge.kind == ReferenceKind::LoadingImage) return R::LoadingScreen;
	if (edge.kind != ReferenceKind::Texture) return R::kCount;
	TextureRoleId role = R::kCount;
	if (texture_arg_role(edge.loader_arg, role)) {
		if (role == R::HudAlphaOnly) context.hud_mode = 1;
		else if (role == R::HudColour) context.hud_mode = 0;
		// The graphic's mode, which the particle file's edge carries (graph/extractors.cpp).
		else if (role == R::ParticleGraphic) context.blend_mode = int(edge.use_context & 0xFF);
		return role;
	}
	if (texture_arg_is_row_type(edge.loader_arg)) return model_role(edge, model, context);
	return R::kCount;
}

namespace {

bool texture_edge(const GraphEdge &edge) {
	return edge.kind == ReferenceKind::Texture || edge.kind == ReferenceKind::MenuTexture ||
	       edge.kind == ReferenceKind::LoadingImage;
}

// The use a texture edge is, its referring model read through `models` once a file (`read`); `served`
// the file its loader finds.
TextureUse edge_use(const AssetGraph &graph, const AssetScan &scan, const GraphEdge &edge, const TextureModelSource &models,
                    const TextureNameTest &exists, std::map<std::string, std::shared_ptr<const Document>> &read) {
	const AssetEntry *source = scan.at_path(edge.source);
	std::shared_ptr<const Document> model;
	if (source && source->kind == AssetKind::Model && texture_arg_is_row_type(edge.loader_arg) && models) {
		auto found = read.find(edge.source);
		if (found == read.end()) found = read.emplace(edge.source, models(edge.source)).first;
		model = found->second;
	}
	TextureUse use;
	use.referrer = edge.source;
	use.record = edge.record;
	use.locator = edge.locator;
	use.field = edge.field;
	use.name_written = edge.value;
	use.role = texture_role_of_edge(edge, model.get(), use.context);
	if (edge.kind == ReferenceKind::Texture) {
		use.loader_arg = edge.loader_arg;
		use.load = texture_reference_load(edge.value, edge.loader_arg, exists);
	}
	std::string served;
	if (graph.resolve(edge, &served) == ReferenceStatus::Present) use.served = served;
	return use;
}

// The use a name the game opens itself is.
TextureUse fixed_use(const AssetScan &scan, const FixedTextureName &fixed, const TextureNameTest &exists) {
	TextureUse use;
	use.role = fixed.role;
	use.fixed = true;
	use.fixed_for = fixed.what;
	use.fixed_witness = fixed.witness;
	use.name_written = fixed.name;
	use.context.hud_mode = fixed.hud_mode;
	const TextureLoader loader = fixed.loader != TextureLoader::kCount ? fixed.loader : texture_role_row(fixed.role).loader;
	use.loader = loader;
	use.load = texture_load(loader, fixed.name, exists, 0, fixed.hud_mode, fixed.role);
	if (const AssetEntry *opened = use.load.file.empty() ? nullptr : scan.find(basename_of(use.load.file)))
		use.served = opened->relative_path;
	return use;
}

std::string stem_key(const std::string &name) {
	return normalized_logical_name(utf8_of(path_of(basename_of(name)).stem()));
}

} // namespace

std::vector<TextureUse> texture_uses(const AssetGraph &graph, const AssetScan &scan, const std::string &file,
                                     const TextureModelSource &models, const TextureNameTest &exists) {
	std::vector<TextureUse> out;
	const AssetEntry *entry = scan.at_path(file);
	if (!entry) return out;
	const std::string key = normalized_logical_name(entry->logical_name);
	// The graph's edges that resolve to the file, then those whose name is the file's though their loader
	// opens another (a .tga beside the .dds the loader takes).
	std::vector<const GraphEdge *> edges;
	for (const GraphEdge *edge : graph.referrers_of_file(file))
		if (texture_edge(*edge)) edges.push_back(edge);
	graph.for_each_edge([&](const GraphEdge &edge) {
		if (!texture_edge(edge) || normalized_logical_name(basename_of(edge.value)) != key) return;
		if (std::find(edges.begin(), edges.end(), &edge) == edges.end()) edges.push_back(&edge);
	});
	std::map<std::string, std::shared_ptr<const Document>> read;
	for (const GraphEdge *edge : edges) {
		TextureUse use = edge_use(graph, scan, *edge, models, exists, read);
		// A terrain detail is read by its own name too (texture_role_read_by_name): the file of the name
		// as written is read whatever its loader's .dds.
		use.reads_file = use.served == file || (texture_role_read_by_name(use.role) &&
		                                        normalized_logical_name(basename_of(use.name_written)) == key);
		use.words = use_words(use);
		out.push_back(std::move(use));
	}
	// The names the game opens itself: those of the file's name, and those whose loader opens this file (a
	// .dds beside the name the game writes).
	const std::string stem = stem_key(entry->logical_name);
	for (const FixedTextureName &fixed : fixed_texture_names()) {
		const bool named = normalized_logical_name(fixed.name) == key;
		if (!named && stem_key(fixed.name) != stem) continue;
		TextureUse use = fixed_use(scan, fixed, exists);
		if (!named && use.served != file) continue;
		use.reads_file = use.served == file;
		use.words = use_words(use);
		out.push_back(std::move(use));
	}
	return out;
}

bool texture_use_opens(const TextureUse &use, const std::string &file) {
	const std::string wanted = normalized_logical_name(basename_of(file));
	if (wanted.empty() || use.name_written.empty()) return false;
	const TextureNameTest only = [&wanted](const std::string &name) { return normalized_logical_name(basename_of(name)) == wanted; };
	TextureLoad load;
	if (use.loader != TextureLoader::kCount)
		load = texture_load(use.loader, use.name_written, only, 0, use.context.hud_mode, use.role);
	else if (use.loader_arg >= 0)
		load = texture_reference_load(use.name_written, use.loader_arg, only);
	else if (use.known())
		load = texture_load(texture_role_row(use.role).loader, use.name_written, only, 0, use.context.hud_mode, use.role);
	else
		return normalized_logical_name(basename_of(use.name_written)) == wanted;
	return !load.file.empty() && normalized_logical_name(basename_of(load.file)) == wanted;
}

std::vector<TextureUse> texture_uses_named(const AssetGraph &graph, const AssetScan &scan, const std::string &stem,
                                           const TextureModelSource &models, const TextureNameTest &exists) {
	std::vector<TextureUse> out;
	const std::string key = normalized_logical_name(stem);
	if (key.empty()) return out;
	std::map<std::string, std::shared_ptr<const Document>> read;
	graph.for_each_edge([&](const GraphEdge &edge) {
		if (!texture_edge(edge) || stem_key(edge.value) != key) return;
		TextureUse use = edge_use(graph, scan, edge, models, exists, read);
		use.reads_file = !use.served.empty();
		use.words = use_words(use);
		out.push_back(std::move(use));
	});
	for (const FixedTextureName &fixed : fixed_texture_names()) {
		if (stem_key(fixed.name) != key) continue;
		TextureUse use = fixed_use(scan, fixed, exists);
		use.reads_file = !use.served.empty();
		use.words = use_words(use);
		out.push_back(std::move(use));
	}
	return out;
}

JsonValue texture_use_json(const TextureUse &use) {
	JsonValue out = JsonValue::make_object();
	out.set("role", json_string(use.known() ? texture_role_row(use.role).token : ""));
	out.set("words", json_string(use.words));
	if (!use.fixed) {
		out.set("referrer", json_string(use.referrer));
		if (!use.record.empty()) out.set("record", json_string(use.record));
		if (!use.locator.empty()) out.set("locator", json_string(use.locator));
		out.set("field", json_string(use.field));
	}
	out.set("name_written", json_string(use.name_written));
	JsonValue load = JsonValue::make_object();
	load.set("file", json_string(use.load.file));
	load.set("reader", json_string(texture_file_reader_token(use.load.reader)));
	load.set("transform", json_string(texture_load_transform_token(use.load.transform)));
	out.set("load", std::move(load));
	out.set("served", json_string(use.served));
	out.set("reads_file", JsonValue::make_bool(use.reads_file));
	if (use.fixed) {
		out.set("fixed", JsonValue::make_bool(true));
		out.set("fixed_for", json_string(use.fixed_for));
		out.set("witness", json_string(use.fixed_witness));
	}
	JsonValue context = JsonValue::make_object();
	if (use.context.material >= 0) {
		context.set("material", json_number(use.context.material));
		context.set("slot", json_number(use.context.slot));
		context.set("type", json_number(use.context.type));
		context.set("row_flags", json_number(use.context.row_flags));
		context.set("shader", json_string(use.context.shader));
		context.set("alpha_test", JsonValue::make_bool(use.context.alpha_test()));
		context.set("alpha_ref", json_number(use.context.alpha_ref));
	}
	if (!use.context.key.empty()) context.set("key", json_string(use.context.key));
	if (use.context.hud_mode >= 0) context.set("hud_mode", json_number(use.context.hud_mode));
	if (use.context.blend_mode >= 0)
		context.set("blend_mode", json_string(particle::blend_mode_name(particle::BlendMode(use.context.blend_mode))));
	out.set("context", std::move(context));
	if (use.budget.known) out.set("budget", texture_budget_json(use.budget));
	return out;
}

TextureBudget texture_use_budget(const TextureUse &use, const TextureHeader &header) {
	TextureBudgetLoader loader = TextureBudgetLoader::Stage;
	if (!use.known() || use.fixed || use.served.empty() || !texture_role_budget_loader(use.role, loader)) return TextureBudget();
	return texture_budget(header, basename_of(use.served), loader, use.context.slot);
}

} // namespace opennova::editor
