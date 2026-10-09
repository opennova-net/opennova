#include <editor/graph/texture_uses.h>

#include <algorithm>
#include <cctype>
#include <iterator>
#include <map>
#include <set>

#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>
#include <editor/graph/asset_graph.h>
#include <editor/model/document.h>
#include <editor/project/project_files.h>
#include <formats/particle/particle.h>
#include <runtime/renderer/fixed_texture_names.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;
using R = renderer::TextureRoleId;

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
// loader makes it by its runtime type (renderer::model_row_texture_role) [orig: Material_LoadStageTexture @
// 0x5B16F0, the switch @ 0x5B1737].
renderer::TextureRoleId model_role(const GraphEdge &edge, const Document *model, TextureUseContext &context) {
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
				context.material_flags = renderer::texture_row_material_flags(context.shader, uint8_t(whole(value)), context.type, context.slot);
			if (model->get(at.owner, "alpha_test", value)) context.alpha_ref = uint8_t(whole(value));
			Document::Placement material;
			if (model->placement(at.owner, material)) context.material = int(material.index);
		}
	}
	return renderer::model_row_texture_role(context.type, context.slot, context.row_flags, edge.value);
}

// What the game opens a fixed name for, and the witness, in words (renderer::FixedTextureUse).
struct FixedUseWords {
	const char *what;
	const char *witness;
};
FixedUseWords fixed_use_words(renderer::FixedTextureUse use) {
	using U = renderer::FixedTextureUse;
	switch (use) {
	case U::BoardBox: return {"for the scoreboard's box", "BoxTexture_LoadAndSetupUVRegions @ 0x56ACD0"};
	case U::BoardMonogram:
		return {"for the scoreboard's box", "Game_StartMission @ 0x525AA3 (BoxTexture_LoadAndSetupUVRegions @ 0x56ACD0)"};
	case U::NetIcon: return {"for the connection indicators", "CNetworkIcons_LoadTextures @ 0x4C2CF0"};
	case U::TipBox: return {"for the tip panel's box", "CTipSystem_Init @ 0x5B6970 (BoxTexture_LoadAndSetupUVRegions @ 0x56ACD0)"};
	case U::TipArt: return {"for the tip panel", "CTipSystem_Init @ 0x5B6970"};
	case U::HudMap: return {"for the HUD's map", "HUD_LoadAllTextures @ 0x59E060..0x59E0F3"};
	case U::HudArt: return {"for the HUD", "HUD_LoadAllTextures @ 0x59DDA0"};
	case U::HudCargoFlag: return {"for the HUD's carried flag", "HUD_LoadAllTextures @ 0x59DE53"};
	case U::HudCargoItem: return {"for the HUD's carried item", "HUD_LoadAllTextures @ 0x59DE64"};
	case U::Crosshair: return {"for a crosshair style", "HUD_LoadAllTextures @ 0x59E3D6"};
	case U::ScopeCrosshair: return {"for the scope's crosshair", "HUD_LoadAllTextures @ 0x59DDA0"};
	case U::BinocularMask: return {"for the binoculars", "ViewFx_InitShadersAndTextures @ 0x5CFDB8"};
	case U::BinocularCrosshair: return {"for the binoculars", "ViewFx_InitShadersAndTextures @ 0x5CFDF3"};
	case U::BinocularDigits: return {"for the binoculars' range digits", "HUD_LoadAllTextures @ 0x59E109"};
	case U::NvgMask: return {"for the night vision", "ViewFx_InitShadersAndTextures @ 0x5CFE18"};
	case U::NvgScale: return {"for the night vision's scale", "ViewFx_InitShadersAndTextures @ 0x5CFE4A"};
	case U::Vignette: return {"for the damage vignette", "sub_5C36B0 @ 0x5C36BC"};
	case U::Rain: return {"for rain", "WeatherParticle_LoadTextures @ 0x5DE840"};
	case U::Snow: return {"for snow", "WeatherParticle_LoadTextures @ 0x5DE88E"};
	case U::SmokeTrail: return {"for smoke trails", "CEffectEmitterPool_CreateShaders @ 0x5DC8F0"};
	case U::WaterRing: return {"for the water rings", "WaterRing_LoadResources @ 0x5DDC90"};
	case U::ImpactScar: return {"for an impact scar", "Scar_LoadTextures @ 0x5CC2E0"};
	case U::Scorch: return {"for a scorch mark", "Terrain_LoadScorchTextures @ 0x604CE0"};
	case U::SplashCursor: return {"for the start-mission cursor", "Game_ShowStartMissionSplash @ 0x520820"};
	case U::PreviewCube: return {"for the player preview's reflection", "PlayerInfo_InitPreviewModel @ 0x56010C"};
	case U::BootSplash: return {"for the boot splash", "Game_ShowLoadingScreen @ 0x4A5420"};
	case U::LoadingFallback: return {"for a mission with no loading screen of its own", "Render_LoadingScreen @ 0x521D10"};
	case U::Mfd: return {"for the vehicles' MFD", "sub_59B120 @ 0x59B120"};
	case U::kCount: break;
	}
	return {"", ""};
}

std::vector<FixedTextureName> collect_fixed() {
	std::vector<FixedTextureName> out;
	for (const renderer::FixedTextureName &fixed : renderer::fixed_texture_names()) {
		const FixedUseWords words = fixed_use_words(fixed.use);
		FixedTextureName row;
		row.name = fixed.name;
		row.role = fixed.role;
		row.what = words.what;
		row.witness = words.witness;
		row.loader = fixed.loader;
		row.hud_mode = fixed.loader == renderer::TextureLoader::HudAlpha   ? 1
		               : fixed.loader == renderer::TextureLoader::HudColor ? 0
		                                                                   : -1;
		out.push_back(std::move(row));
	}
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

renderer::TextureRoleId texture_role_of_edge(const GraphEdge &edge, const Document *model, TextureUseContext &context) {
	context.key = edge.field;
	if (edge.kind == ReferenceKind::MenuTexture) {
		if (edge.field == "frame.stencil") return R::MenuFrameStencil;
		if (edge.field == "frame.brush") return R::MenuFrameBrush;
		if (edge.field == "cursor.file") return R::MenuCursor;
		return R::MenuImage;
	}
	if (edge.kind == ReferenceKind::LoadingImage) return R::LoadingScreen;
	if (edge.kind != ReferenceKind::Texture) return R::kCount;
	renderer::TextureRoleId role = R::kCount;
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
	use.loader = fixed.loader;
	use.load = texture_load(fixed.loader, fixed.name, exists, 0, fixed.hud_mode);
	if (const AssetEntry *opened = use.load.file.empty() ? nullptr : scan.find(basename_of(use.load.file)))
		use.served = opened->relative_path;
	return use;
}

std::string stem_key(const std::string &name) {
	return pff::normalized_logical_name(utf8_of(path_of(basename_of(name)).stem()));
}

} // namespace

std::vector<TextureUse> texture_uses(const AssetGraph &graph, const AssetScan &scan, const std::string &file,
                                     const TextureModelSource &models, const TextureNameTest &exists) {
	std::vector<TextureUse> out;
	const AssetEntry *entry = scan.at_path(file);
	if (!entry) return out;
	const std::string key = pff::normalized_logical_name(entry->logical_name);
	// The graph's edges that resolve to the file, then those whose name is the file's though their loader
	// opens another (a .tga beside the .dds the loader takes).
	std::vector<const GraphEdge *> edges;
	for (const GraphEdge *edge : graph.referrers_of_file(file))
		if (texture_edge(*edge)) edges.push_back(edge);
	graph.for_each_edge([&](const GraphEdge &edge) {
		if (!texture_edge(edge) || pff::normalized_logical_name(basename_of(edge.value)) != key) return;
		if (std::find(edges.begin(), edges.end(), &edge) == edges.end()) edges.push_back(&edge);
	});
	std::map<std::string, std::shared_ptr<const Document>> read;
	for (const GraphEdge *edge : edges) {
		TextureUse use = edge_use(graph, scan, *edge, models, exists, read);
		// A terrain detail is read by its own name too (texture_role_read_by_name): the file of the name
		// as written is read whatever its loader's .dds.
		use.reads_file = use.served == file || (renderer::texture_role_read_by_name(use.role) &&
		                                        pff::normalized_logical_name(basename_of(use.name_written)) == key);
		use.words = use_words(use);
		out.push_back(std::move(use));
	}
	// The names the game opens itself: those of the file's name, and those whose loader opens this file (a
	// .dds beside the name the game writes).
	const std::string stem = stem_key(entry->logical_name);
	for (const FixedTextureName &fixed : fixed_texture_names()) {
		const bool named = pff::normalized_logical_name(fixed.name) == key;
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
	const std::string wanted = pff::normalized_logical_name(basename_of(file));
	if (wanted.empty() || use.name_written.empty()) return false;
	const TextureNameTest only = [&wanted](const std::string &name) { return pff::normalized_logical_name(basename_of(name)) == wanted; };
	TextureLoad load;
	if (use.loader != renderer::TextureLoader::kCount)
		load = texture_load(use.loader, use.name_written, only, 0, use.context.hud_mode);
	else if (use.loader_arg >= 0)
		load = texture_reference_load(use.name_written, use.loader_arg, only);
	else if (use.known())
		load = texture_load(texture_role_row(use.role).loader, use.name_written, only, 0, use.context.hud_mode);
	else
		return pff::normalized_logical_name(basename_of(use.name_written)) == wanted;
	return !load.file.empty() && pff::normalized_logical_name(basename_of(load.file)) == wanted;
}

std::vector<TextureUse> texture_uses_named(const AssetGraph &graph, const AssetScan &scan, const std::string &stem,
                                           const TextureModelSource &models, const TextureNameTest &exists) {
	std::vector<TextureUse> out;
	const std::string key = pff::normalized_logical_name(stem);
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
		// What its alpha is to the game, by its material's technique (texture_row_alpha_meaning).
		const renderer::TextureAlphaMeaning alpha = renderer::texture_row_alpha_meaning(use.context.shader, use.context.material_flags,
		                                                            use.context.type, use.context.slot, use.name_written);
		context.set("alpha", json_string(texture_alpha_meaning_token(alpha)));
		context.set("alpha_words", json_string(texture_alpha_meaning_words(alpha, use.context.shader, use.context.alpha_ref,
		                                                                   use.context.alpha_test_inverted())));
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
	renderer::TextureLoader loader = renderer::TextureLoader::Stage;
	if (!use.known() || use.fixed || use.served.empty() || !texture_role_budget_loader(use.role, loader)) return TextureBudget();
	return texture_budget(header, basename_of(use.served), loader, use.context.slot);
}

} // namespace opennova::editor
