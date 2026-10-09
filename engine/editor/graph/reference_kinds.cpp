#include <editor/graph/reference_kinds.h>

#include <algorithm>
#include <cstdint>

#include <base/gameprofile/required_resources.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/animation_slots.h>
#include <editor/documents/text_types.h>
#include <editor/documents/texture_load_rules.h>
#include <editor/documents/texture_roles.h>
#include <editor/graph/asset_graph.h>
#include <editor/project/project_files.h>
#include <formats/mission/mission_params.h>
#include <formats/trn/trn.h>
#include <runtime/anim/rig_files.h>
#include <runtime/menu/menu_assets.h>
#include <runtime/menu/menu_style.h>
#include <runtime/renderer/material_texture.h>
#include <runtime/renderer/texture_load_rules.h>

namespace opennova::editor {

namespace {

using Exists = std::function<bool(const std::string &)>;

std::vector<std::string> one(const std::string &file) {
	return file.empty() ? std::vector<std::string>() : std::vector<std::string>{file};
}

// --- the loaders' own rules (ReferenceKindRow::file_names) ---------------------------------

// The files the reference's loader opens, in its order (ADR 0046 S18), none when it opens none: a
// model's texture row (`loader_arg` its row's type) the one file the loader the dispatcher picks for
// the type opens (renderer::material_texture_source; the shell's TextureFiles::load_material_texture
// reads the same file; the row's type is the one the loader copies into the runtime row [orig:
// Material_ConvertDefinition @ 0x5B045B..0x5B04A0]); any other referrer's by its role's loader, or by
// the game's loader alone where only that is known (texture_loader_arg; renderer::texture_load_attempts,
// which TextureFiles::load_texture reads: a sky map's .dds beside the name first, a colour map's TGA
// reader on the name alone, the HUD's suffixes), no loose-first search competing and no loose folder
// of the game's there (the particle manager's tga\ leg); a use whose loader is not witnessed yet, the
// name as written alone. No loader reads another extension's twin or an _O name.
std::vector<std::string> texture_files(const std::string &name, int32_t loader_arg, const Exists &exists) {
	// A role (ADR 0046 S18): the game's loader for it (texture_role_renderer_loader), a mission's tile set
	// its TGA, a role read by its own name (a foliage map) or whose loader is not witnessed the name alone.
	TextureRoleId role = TextureRoleId::kCount;
	if (texture_arg_role(loader_arg, role)) {
		if (loader_arg & kTextureArgTileSet) return texture_files(name, kTileSetTextureArg, exists);
		// A sky map: its extension made PCX first (kTextureArgPcx).
		if (loader_arg & kTextureArgPcx)
			return texture_files(menu::replace_or_append_extension(name, "pcx"), loader_arg & ~kTextureArgPcx, exists);
		renderer::TextureLoader by;
		return texture_role_renderer_loader(role, by) ? texture_files(name, texture_loader_arg(by), exists) : one(name);
	}
	if (loader_arg >= 0) {
		const uint8_t type = renderer::material_texture_runtime_type(static_cast<uint8_t>(loader_arg));
		return one(renderer::material_texture_source(name, type, exists).file);
	}
	if (loader_arg == kTileSetTextureArg)
		return texture_files(trn_mission_tilestrip(TrnConfig{}, name), texture_loader_arg(renderer::TextureLoader::Tga),
		                     exists);
	renderer::TextureLoader loader;
	if (!texture_loader_of(loader_arg, loader)) return one(name);
	renderer::TextureFileQuery files;
	files.exists = exists;
	std::vector<std::string> out;
	for (const renderer::TextureLoad &load : renderer::texture_load_attempts(loader, name, files))
		if (load.source == renderer::TextureFileSource::Mounted && !load.file.empty() &&
		    std::find(out.begin(), out.end(), load.file) == out.end())
			out.push_back(load.file);
	return out;
}

// A texture the game cannot do without (a terrain's colour map; its blend map, whenever its key names
// one): the reference's role says so (texture_arg_gates).
const char *texture_gates(int32_t loader_arg) {
	if (!texture_arg_gates(loader_arg)) return nullptr;
	return "[orig: PolyTrn_InitTextures @ 0x60B389 (\"colormap\") and @ 0x60B19A (\"blendermap\", the blend on by its "
	       "key alone, Terrain_ParseConfigCallback @ 0x60F7D0) logging the error Game_StartMission @ 0x525AD8 shows as "
	       "\"Polytrn: Critical file not found\" before it aborts the mission, sub_520AA0 @ 0x520B4E..0x520B64]";
}

// What the game does without the file, in the role's words (texture_roles.h); where the project holds the
// name as written, that the loader opens other files for it (a HUD name written .dds, a sky map's name made
// .pcx), and which.
std::string texture_missing(const AssetGraph &graph, const GraphEdge &edge) {
	TextureRoleId role = TextureRoleId::kCount;
	const bool has_role = texture_arg_role(edge.loader_arg, role);
	const std::string then = has_role && *texture_role_row(role).missing ? ": " + std::string(texture_role_row(role).missing) + "." : ".";
	const std::string name = basename_of(edge.value);
	if (!name.empty() && graph.has_file(name)) {
		// What the loader would open were every name it tries there (a .dds beside the name first), then
		// were none.
		std::vector<std::string> tries = texture_files(edge.value, edge.loader_arg, [](const std::string &) { return true; });
		for (const std::string &each : texture_files(edge.value, edge.loader_arg, [](const std::string &) { return false; }))
			if (std::find(tries.begin(), tries.end(), each) == tries.end()) tries.push_back(each);
		std::string opens;
		for (const std::string &each : tries) opens += (opens.empty() ? "" : " or ") + basename_of(each);
		return ", which the project has, but the game's loader for it opens " +
		       (opens.empty() ? std::string("no file of that name") : opens + ", which the project does not have") + then;
	}
	return ", which the project does not have" + then;
}

// The name's extension decides, and a .tga the files lack loads its .dds [orig: the dispatch
// lives at the engine home, menu_assets.h, CTextureManager_LoadOrFindTexture @ 0x654980].
std::vector<std::string> menu_texture_files(const std::string &name, int32_t, const Exists &exists) {
	return one(menu::menu_texture_source(name, exists).file);
}

// The .fnt from the name's first dot [orig: CFontCache_LoadOrGetFont @ 0x652f70].
std::vector<std::string> font_files(const std::string &name, int32_t, const Exists &) {
	return one(menu::menu_font_file(name));
}

// The file's name of the path a sound bank's single holds: an archive holds names, no folders (the
// shell's sound bank reads the same name, audio/sound_bank.cpp).
std::vector<std::string> wave_files(const std::string &name, int32_t, const Exists &) {
	const size_t slash = name.find_last_of("/\\");
	return one(slash == std::string::npos ? name : name.substr(slash + 1));
}

// The extensions a loader appends to a name, tried after the name as written.
constexpr const char *kModel[] = {".3di", nullptr};
constexpr const char *kAnimationMap[] = {".adm", nullptr};
constexpr const char *kAnimation[] = {".bad", nullptr};
constexpr const char *kAiProfile[] = {".aip", nullptr};
constexpr const char *kMenu[] = {".mnu", nullptr};
constexpr const char *kTable[] = {".bin", nullptr};
constexpr const char *kTerrain[] = {".trn", nullptr};
constexpr const char *kEnvironment[] = {".env", nullptr};
constexpr const char *kScriptFile[] = {".wac", nullptr};
constexpr const char *kLoadingImage[] = {".pcx", nullptr};
constexpr const char *kTilePlacement[] = {".til", nullptr};
constexpr const char *kDialogBankFile[] = {".dbf", nullptr};

// --- what a finding says of a name nothing resolves (ReferenceKindRow::missing_message) ---

std::string project_lacks(const AssetGraph &, const GraphEdge &edge) {
	if (!edge.fallback.empty())
		return ", which the project does not have, nor '" + edge.fallback +
		       "', the name its lookup takes next.";
	return ", which the project does not have.";
}

// Defined only where the game does not read it, or not at all.
std::string style_missing(const AssetGraph &graph, const GraphEdge &edge) {
	const std::vector<const GraphSymbol *> defined = graph.symbols_named(ReferenceKind::StyleVar, edge.target);
	if (defined.empty()) return ", which neither menu_style.mns nor brand.mns defines; the game keeps it literal.";
	const std::string &file = defined.front()->file;
	if (menu::is_shell_stylesheet(basename_of(file)))
		return ", which " + file + " defines after the place the game stops reading it; the game keeps it literal.";
	return ", which only " + file +
	       " defines, a stylesheet the game does not read (it reads menu_style.mns and brand.mns); the game keeps it "
	       "literal.";
}

std::string text_id_missing(const AssetGraph &graph, const GraphEdge &edge) {
	// The table the lookup reads as the project's files are (a mission's own, else medmssn.bin).
	const std::string scope = graph.lookup_scope(edge);
	const size_t slash = scope.find('/');
	const std::string table = scope.substr(0, slash);
	const std::string section = slash == std::string::npos ? std::string() : scope.substr(slash + 1);
	if (scope.empty()) return ", which no string table defines; the game shows the id.";
	if (table.empty())
		return ", but its window names no string table (TEXT_RSRC) and neither does the window it falls back to; the "
		       "game shows the id.";
	if (!graph.has_file(table))
		return ", which it looks up in " + table + ", a string table the project does not have; the game shows the id.";
	if (section.empty()) return ", which " + table + " does not define; the game shows the id.";
	return ", which the first \"" + section + "\" section of " + table +
	       " does not define (the game reads that section alone); the game shows the id.";
}

std::string screen_missing(const AssetGraph &, const GraphEdge &edge) {
	return ", which " + edge.scope + " does not have and no other menu of the project has: the game selects no screen "
	       "and nothing changes.";
}

// A window of that name the lookup never reaches (MnuDocument::lookup_names), or none.
std::string window_missing(const AssetGraph &graph, const GraphEdge &edge) {
	const std::string screen = edge.scope.substr(edge.scope.find('/') + 1);
	bool unreached = false;
	for (const GraphSymbol *symbol : graph.symbols_named(ReferenceKind::MenuWindow, edge.target))
		unreached = unreached || scope_matches(symbol->scope, edge.scope);
	return unreached ? ", which screen " + screen + " has only where the game's lookup never finds it (under a window "
	                   "with no NAME, where the search stops, or on an earlier screen of that name the later one "
	                   "hides): the ACTION does nothing."
	                 : ", which no window of screen " + screen + " is named: the ACTION does nothing.";
}

std::string terrain_missing(const AssetGraph &, const GraphEdge &) {
	return ", which the project does not have: the game refuses to start the mission.";
}

std::string bank_missing(const AssetGraph &, const GraphEdge &) {
	return ", which the project does not have: the game plays no sound for it.";
}

std::string wave_missing(const AssetGraph &, const GraphEdge &) {
	return ", which the project does not have: the game plays nothing for it.";
}

// A set name no loaded bank has resolves to no set, which every consumer witnessed plays as silence
// [orig: SoundBank_FindSetByNameAnyBank @ 0x5274f0 returns 0; the profile resolve @ 0x5282b2; the
// one-shot paths return at once, Sound_Play3DPositional @ 0x527cc6].
std::string sound_missing(const AssetGraph &graph, const GraphEdge &edge) {
	// A menu SOUND's trigger is looked up in its own bank alone.
	if (!edge.scope.empty())
		return ", which " + edge.scope + ", the bank the SOUND names, does not have: the menu plays nothing for it.";
	for (const GraphSymbol *symbol : graph.symbols_named(ReferenceKind::Sound, edge.target))
		if (!graph.on_bank_chain(symbol->file))
			return ", which only " + basename_of(symbol->file) +
			       " has, a bank the game never searches for a set by name (it searches gamelocl.lwf, game.lwf, "
			       "game3.lwf, game2.lwf and an expansion's own): the game plays nothing for it.";
	return ", which no sound bank the game searches has: the game plays nothing for it.";
}

// Whether a bank's wave is named by a dialog line (a dialog bank's line, whose wave its mission's dialog
// bank's sounds hold), not by a member of the bank.
bool named_by_dialog_line(const GraphEdge &edge) {
	return asset_kind_for_name(basename_of(edge.source)) == AssetKind::DialogBank;
}

// A member names its wave by the wave's place in the bank, which the save finds by the name: a name
// no wave of the bank has cannot be written. A dialog line's wave the game looks up by its name and
// finds none of: it shows "EX Cannot load audio" in the chat and plays nothing for the line, the dialog
// going on after 12 ticks [orig: Dialog_LoadAudioClip @ 0x44dd46..0x44dd7c].
std::string bank_wave_missing(const AssetGraph &graph, const GraphEdge &edge) {
	const std::string bank = edge.scope.empty() ? std::string("the bank") : edge.scope;
	if (named_by_dialog_line(edge)) {
		const std::string read = graph.lookup_scope(edge);
		if (!read.empty() && !graph.has_file(read))
			return ", which the dialog bank's sounds hold, " + read +
			       ", a bank the project does not have: the game says \"EX Cannot load audio\" and plays nothing for the line.";
		return ", which no wave of " + (read.empty() ? bank : read) +
		       " is named: the game says \"EX Cannot load audio\" and plays nothing for the line.";
	}
	return ", which no wave of " + bank + " is named: the bank cannot be saved until one is.";
}

DiagnosticSeverity bank_wave_severity(const GraphEdge &edge) {
	return named_by_dialog_line(edge) ? DiagnosticSeverity::Warning : DiagnosticSeverity::Error;
}

// A dialog no dialog bank of the mission's has plays nothing [orig: Dialog_PlayByName @ 0x44da70..0x44dab0
// returns 0]: no line, no subtitle; a PLYRDIALOG trigger on it reads it as never playing [orig:
// Dialog_ExistsByIndex @ 0x44e170].
std::string dialog_name_missing(const AssetGraph &graph, const GraphEdge &edge) {
	const std::string bank = edge.scope.substr(0, edge.scope.find('/'));
	if (!bank.empty() && !graph.has_file(bank))
		return ", from " + bank + ", a dialog bank the project does not have: the game plays no dialog of this mission.";
	return ", which " + (bank.empty() ? std::string("the mission's dialog bank") : bank) +
	       " has no dialog of (the game matches a dialog's name exactly): the game plays nothing for it.";
}

// A name no profile has binds the file's first profile [orig: SoundProfile_FindSlotByName @ 0x526e30
// returns the table's base on a miss; ItemDef_ParseProperty @ 0x49fafd].
std::string profile_missing(const AssetGraph &, const GraphEdge &) {
	return ", which SndProf.def has no profile of: the game binds its first profile instead.";
}

std::string credits_missing(const AssetGraph &, const GraphEdge &) {
	return ", which the project does not have: the marquee shows none of its lines.";
}

// A token whose .bad does not load registers failsafe.bad, else nothing, and a slot none of whose
// tokens registered serves the reset row's first clip [orig: AnimMap_FindOrLoadBoneFile @ 0x40C030,
// the failsafe @ 0x40C25B..0x40C2A1, none @ 0x40C260; AnimMap_RegisterEntity @ 0x40BB60, the backfill
// @ 0x40BC24, @ 0x40BD2E], which the game plays in it or never picks it then by the slot's own rule
// (animation_slot_absence). The slot in its words (ADR 0046 S17).
std::string clip_missing(const AssetGraph &graph, const GraphEdge &edge) {
	if (graph.has_file(anim::kFailsafeClip))
		return ", which the project does not have: the game plays failsafe.bad in its place.";
	const int slot = animation_key_slot(edge.record.substr(0, edge.record.find('/')));
	const std::string words = slot >= 0 ? animation_slot_words(slot) : std::string("its row");
	const std::string missing = slot > 0 ? " " + animation_slot_when_missing(slot) : std::string();
	return ", which the project does not have: the game leaves it out of " + words +
	       "'s clips (the project has no failsafe.bad to play in its place); a slot left with none is as one the map "
	       "leaves out." + missing;
}

// A point no lookup finds: an item's particle slot reads the model's first 16 alone (its scope's section,
// kFirstUserPointsSection) and attaches its effect to none; every other lookup scans every point and keeps
// none (its index 0) [orig: ItemDef_GetBoneMaskByName @ 0x49ea40; modelgpm_FindUserpointByName @ 0x5b2170].
std::string user_point_missing(const AssetGraph &, const GraphEdge &edge) {
	const size_t slash = edge.scope.find('/');
	const std::string model = edge.scope.empty() ? std::string("the model") : edge.scope.substr(0, slash);
	if (slash != std::string::npos)
		return ", which " + model + " does not have among its first 16 user points: the effect attaches to none.";
	return ", which " + model + " does not have: the game's lookup finds no point of that name there and keeps none.";
}

// An effect no particle file defines: the game's lookup by name copies the effects' stockeffect under the
// name, or finds none without one [orig: CEffectWorld_InternEffectHandle @ 0x5f7310, the stockeffect copy
// @ 0x5f739f..0x5f73c5].
std::string particle_missing(const AssetGraph &graph, const GraphEdge &) {
	return graph.resolve(ReferenceKind::Particle, "stockeffect") == ReferenceStatus::Present
	               ? ", which no particle file of the project defines: the game plays a copy of stockeffect under that "
	                 "name."
	               : ", which no particle file of the project defines, nor stockeffect, which the game copies for a name "
	                 "it lacks: it plays nothing.";
}

// A weapon action's slot that its weapon's map has no row for plays the map's reset clip, a loaded map's
// every unauthored slot serving it [orig: AnimMap_RegisterBoneNode @ 0x40C2D0, slot 0's backfill @
// 0x40C39A..0x40C3E2]; a key naming no slot of the 252 the action looks up warns and plays none, an auto
// delay of it 0 [orig: Anim_InitActions @ 0x54219E -> AnimMap_FindSlotByName @ 0x40cfa0, "could not find
// anim" @ 0x5421FF..0x542225].
std::string animation_key_missing(const AssetGraph &, const GraphEdge &edge) {
	if (animation_key_slot(edge.value) < 0)
		return ", which names none of the engine's 252 animation slots: the game warns \"could not find anim\" and the "
		       "action plays no clip, an auto delay of it 0.";
	return ", which " + (edge.scope.empty() ? std::string("the weapon's animation map") : edge.scope) +
	       " has no row for: the action plays the map's reset clip in its place.";
}

// The lookups find no record of the SSN: a condition on it reads as its type does for no entity (a
// SingleDestroyed TRUE), an action on it does nothing [orig: EventTrigger_EvaluateCondition
// @0x453620; docs/mission/bms-event-runtime-re.md 7.4].
std::string entity_missing(const AssetGraph &, const GraphEdge &) {
	return ", an SSN no entity of the mission has: the game's lookups find no record for it.";
}

// The load neuters a trigger naming a zone the file lacks, which then reads false (a negated one
// true), and zeroes an action naming one [orig: EventTrigger_ResolveZoneTriggerRefs @0x453000,
// EventTrigger_ResolveZoneActionRefs @0x453100].
std::string zone_missing(const AssetGraph &, const GraphEdge &) {
	return ", a zone id no area trigger of the mission has: the game makes a trigger naming it read false and an "
	       "action naming it do nothing.";
}

// The mission's tile placement the game reads loose first [orig: Terrain_LoadTileInfoFile @0x60a740];
// without it the terrain's own tiles stand.
std::string tiles_missing(const AssetGraph &, const GraphEdge &) {
	return ", which the project does not have: the terrain's own tiles stand.";
}

// A dialog plays from the mission's own bank [orig: DialogSystem_Init @0x5275e0 builds the name
// from the mission's]; without it the game plays no dialog.
std::string dialog_missing(const AssetGraph &, const GraphEdge &) {
	return ", which the project does not have: the game plays no dialog of this mission.";
}

// The mission's text loads from its own table, else medmssn.bin [orig: TextResource_LoadMissionTextBin
// @0x51ed90]; with neither every key the mission reads answers "" [orig:
// MissionText_GetStringByKeyOrGameText @0x51ECD0, the "" @0x51ecea]: no title, no briefing, no
// location name, no objective text.
std::string mission_strings_missing(const AssetGraph &graph, const GraphEdge &) {
	return graph.has_file("medmssn.bin")
	               ? ", which the project does not have: the game reads medmssn.bin in its place."
	               : ", which the project does not have, nor medmssn.bin to read in its place: the mission's title, "
	                 "briefing, location names and objectives show empty.";
}

// A material whose shader tag no registered effect has draws with the registry's first entry [orig:
// Material_ConvertDefinition @ 0x5B0664..0x5B0672 and Material_ResolveEffectSubobjectsAndShader @
// 0x5B18C1..0x5B18CD: HLSLEffect_FindByName's -1 clamped to 0]. The renderer registers _ffp.fx's tags
// first, each opened by the file's name [orig: HLSLEffect_InitAndLoadAll @ 0x5B00F2]; with no effect at
// all that entry holds no pass, and the draw draws nothing [orig: CRenderBatchQueue_FlushBatches @
// 0x5DA220..0x5DA22B].
std::string shader_missing(const AssetGraph &graph, const GraphEdge &) {
	return graph.has_file(kFixedFunctionShaderFile)
	               ? ", which no shader of the project registers: the game draws the material with the first shader it "
	                 "registered instead."
	               : ", which no shader of the project registers, nor does the project have _ffp.fx, the renderer's "
	                 "own: the game draws the material with the first shader it registered, and with none registered "
	                 "draws nothing.";
}

// An item id nothing defines; one a number names by its type id (an ammo's tracer id, the id less 100000) says
// the number as written, and what the ammo's parse does then: it takes the item whose name is the ammo's, else
// warns and keeps none [orig: AmmoDef_ParseProperty @ 0x40A5E2..0x40A60B, ItemList_FindIndexByPrimaryName over
// the ammo's own name; "couldn't find ammodef frndlyTrcrID"].
// A charattr class's camouflage item names it by its type id too, and what the game does then is the item
// lookup's own: index 0, items.def's first row, for a type no item has, which a player of the class spawns as
// [orig: Entity_SpawnFromAnimSlotProperty @ 0x43c3cf -> ItemList_FindIndexByTypeId @ 0x49e100, its 0 for no
// match @ 0x49e12f].
bool named_by_class_cammo(const GraphEdge &edge) {
	return edge.name_offset && (edge.field == "JUNGLE_CAMMO" || edge.field == "DESERT_CAMMO" || edge.field == "ARCTIC_CAMMO");
}

std::string item_missing(const AssetGraph &graph, const GraphEdge &edge) {
	if (!edge.name_offset) return project_lacks(graph, edge);
	const std::optional<int> id = strutil::parse_int(edge.value);
	const std::string written = id ? std::to_string(int64_t(*id) - edge.name_offset) : edge.value;
	if (named_by_class_cammo(edge))
		return " (type id " + written + "), which the project does not have: a player of the class spawns as items.def's "
		       "first row instead.";
	return " (type id " + written + "), which the project does not have: the game takes the item named as the record "
	       "instead, else warns that it finds none.";
}

// The game spawns the class's player as another item: a warning, the game runs on.
DiagnosticSeverity item_severity(const GraphEdge &edge) {
	return named_by_class_cammo(edge) ? DiagnosticSeverity::Warning : DiagnosticSeverity::Error;
}

std::string item_alias_missing(const AssetGraph &, const GraphEdge &) {
	return ", which no item of items.def has as its alias: the block's panel applies to no item.";
}

std::string avatar_part_missing(const AssetGraph &, const GraphEdge &edge) {
	const size_t slash = edge.scope.find('/');
	const std::string kind = slash == std::string::npos ? std::string("part") : strutil::to_lower(edge.scope.substr(slash + 1)) + " part";
	return ", which no " + kind + " of the file defines: the game drops a combination missing its head or body, and keeps "
	       "one missing its arms without them.";
}

// --- the values a Record reference names none by (ReferenceKindRow::none) ----------------------

// A part animation's frame byte (the field holds 0 to 255) names no MTRX row at 0 and at 128 to
// 255: the load sign-extends it [orig: GPM_LoadRenderModel @ 0x5B5698 (movsx)] and the pose reads
// a row only above zero [orig: Model_TransformBoneMatrices @ 0x58E3FE], so row 0 is never read
// (threedi_panm_frame_row, the pose's rule; whether a row turns through a frame at all is its
// field's use, ModelDocument's). Any value past the byte names none too.
bool frame_none(int64_t value) { return value <= 0 || value > 127; }

// A mission's group 0 names none, and a waypoint list's 0 and its commands 123..127: the engine's
// mission::group_names_none and mission::path_names_none (formats/mission/mission_params.h).

// --- the table -------------------------------------------------------------------------------

// A row built up column by column, so each row names only what it sets.
struct Row {
	ReferenceKindRow row;
	constexpr Row(ReferenceKind kind, const char *token, const char *phrase, const char *label) : row() {
		row.kind = kind;
		row.token = token;
		row.phrase = phrase;
		row.label = label;
	}
	// A file, by the name as written and then with each extension, or by the loader's own rule.
	constexpr Row loads(AssetKind file, const char *const *extensions, ReferenceFileNames names = nullptr) const {
		Row out = *this;
		out.row.resolution = ReferenceResolution::File;
		out.row.file = file;
		out.row.extensions = extensions;
		out.row.file_names = names;
		out.row.missing_message = project_lacks;
		return out;
	}
	// A %NAME% of the stylesheets, compared without case, which a new stylesheet defines.
	constexpr Row variable() const {
		Row out = *this;
		out.row.resolution = ReferenceResolution::StyleVariable;
		out.row.name_case = NameCase::NoCase;
		out.row.spell = NameSpelling::StyleVariable;
		out.row.defined_in = AssetKind::MenuStyle;
		return out;
	}
	constexpr Row symbol(NameCase name_case, AssetKind defined_in = AssetKind::Unknown) const {
		Row out = *this;
		out.row.resolution = ReferenceResolution::Symbol;
		out.row.name_case = name_case;
		out.row.defined_in = defined_in;
		out.row.missing_message = project_lacks;
		return out;
	}
	// A record of the reference's own file, by its index among the file's records of the kind whose
	// token is `collection`, compared as written; `none` the values that name none. The picker
	// offers the file's records alone; the graph finds none missing (no missing_message: the file's
	// own validation says what the game makes of an index past them).
	constexpr Row record(const char *collection, RecordNone none = nullptr) const {
		Row out = *this;
		out.row.resolution = ReferenceResolution::Record;
		out.row.collection = collection;
		out.row.index_space = RecordIndexSpace::File;
		out.row.none = none;
		out.row.name_case = NameCase::Exact;
		out.row.picker_scoped = true;
		return out;
	}
	// A symbol found in the file its scope names, which the picker narrows to when `picker`.
	constexpr Row scoped(bool picker) const {
		Row out = *this;
		out.row.scope_names_file = true;
		out.row.picker_scoped = picker;
		return out;
	}
	// The game refuses what names a name of the kind that finds nothing, by the witness given: a
	// missing reference of it refuses a build (blocks_build).
	constexpr Row fatal(const char *orig, ReferenceMissingMessage message) const {
		Row out = *this;
		out.row.gates_when_missing = orig;
		out.row.missing_message = message;
		return out;
	}
	// The game refuses some of the kind's references that find nothing, by what each gives its loader
	// (gates_when_missing_for), each saying what the game does without it.
	constexpr Row fatal_for(const char *(*gates)(int32_t), ReferenceMissingMessage message) const {
		Row out = *this;
		out.row.gates_when_missing_for = gates;
		out.row.missing_message = message;
		return out;
	}
	// The game tolerates the name missing: a warning, saying what the game does instead.
	constexpr Row tolerated(ReferenceMissingMessage message) const {
		Row out = *this;
		out.row.severity_when_missing = DiagnosticSeverity::Warning;
		out.row.missing_message = message;
		return out;
	}
	// A missing name of the kind is an error refusing no build, saying why in its own words.
	constexpr Row says(ReferenceMissingMessage message) const {
		Row out = *this;
		out.row.missing_message = message;
		return out;
	}
	// The severity a missing name takes by the reference that makes it (ReferenceKindRow::severity_for).
	constexpr Row severity_by(DiagnosticSeverity (*severity)(const GraphEdge &)) const {
		Row out = *this;
		out.row.severity_for = severity;
		return out;
	}
	// What the message says depends on which files the project has.
	constexpr Row message_reads_files() const {
		Row out = *this;
		out.row.message_reads_files = true;
		return out;
	}
	constexpr Row offers(ReferenceKind kind) const {
		Row out = *this;
		out.row.also_offers = kind;
		return out;
	}
};

constexpr ReferenceKindRow kRows[] = {
	Row(ReferenceKind::None, "none", "the reference", "reference").row,
	Row(ReferenceKind::Model, "model", "the model", "model").loads(AssetKind::Model, kModel).row,
	Row(ReferenceKind::AnimationMap, "animation_map", "the animation map", "animation map")
	        .loads(AssetKind::AnimationMap, kAnimationMap)
	        .row,
	Row(ReferenceKind::Ammo, "ammo", "the ammo", "ammo").symbol(NameCase::FileName, AssetKind::AmmoDefs).row,
	Row(ReferenceKind::Weapon, "weapon", "the weapon", "weapon").symbol(NameCase::FileName, AssetKind::WeaponDefs).row,
	// An item by its id; a tracer's and a charattr class's camouflage by its type id (item_missing), the
	// latter a warning (item_severity).
	Row(ReferenceKind::Item, "item", "the item id", "item id")
	        .symbol(NameCase::Exact, AssetKind::ItemDefs)
	        .says(item_missing)
	        .severity_by(item_severity)
	        .row,
	// Each by its loader (ADR 0046 S18): the terrain's colour map and its blend map abort the mission when
	// they load nothing (texture_gates).
	Row(ReferenceKind::Texture, "texture", "the texture", "texture")
	        .loads(AssetKind::Texture, nullptr, texture_files)
	        .fatal_for(texture_gates, texture_missing)
	        .row,
	// A set by name across the banks the game loads, the first bank's first set of the name, without
	// case [orig: SoundBank_FindSetByNameAnyBank @ 0x5274f0 over SoundBank_FindTriggerByName @ 0x75be90].
	Row(ReferenceKind::Sound, "sound", "the sound set", "sound set")
	        .symbol(NameCase::NoCase, AssetKind::SoundBank)
	        .tolerated(sound_missing)
	        .row,
	// An effect no file defines plays the stockeffect's copy (particle_missing).
	Row(ReferenceKind::Particle, "particle", "the particle effect", "particle effect")
	        .symbol(NameCase::FileName, AssetKind::Particles)
	        .tolerated(particle_missing)
	        .message_reads_files()
	        .row,
	Row(ReferenceKind::AiProfile, "ai_profile", "the AI profile", "AI profile").loads(AssetKind::AiProfile, kAiProfile).row,
	Row(ReferenceKind::OtherText, "other_text", "the string id", "string id").offers(ReferenceKind::TextId).row,
	Row(ReferenceKind::Font, "font", "the font", "font")
	        .loads(AssetKind::Font, nullptr, font_files)
	        .offers(ReferenceKind::StyleVar)
	        .row,
	Row(ReferenceKind::Menu, "menu", "the menu file", "menu file").loads(AssetKind::Menu, kMenu).row,
	Row(ReferenceKind::TextTable, "text_table", "the string table", "string table").loads(AssetKind::Strings, kTable).row,
	// A string id no table defines shows as itself; its picker offers the ids of the table and
	// section the reference reads.
	Row(ReferenceKind::TextId, "text_id", "the string id", "string id")
	        .symbol(NameCase::NoCase)
	        .scoped(true)
	        .tolerated(text_id_missing)
	        .message_reads_files()
	        .row,
	// A style variable no stylesheet the game reads defines stays literal.
	Row(ReferenceKind::StyleVar, "style_var", "the style variable", "style variable")
	        .variable()
	        .tolerated(style_missing)
	        .row,
	// A mission's terrain that does not load leaves the terrain config cleared, which the loader's tail
	// refuses; the mission does not start (ADR 0046 S14: witnessed, so a missing one refuses a build).
	Row(ReferenceKind::Terrain, "terrain", "the terrain", "terrain")
	        .loads(AssetKind::Terrain, kTerrain)
	        .fatal("[orig: Game_StartMission @ 0x524b26 -> Game_LoadTerrainDuringConnect @ 0x520710 -> "
	               "Terrain_LoadEnvironmentConfig @ 0x610940, the empty colour map refused @ 0x610a2a; the start "
	               "failing @ 0x524b30]",
	               terrain_missing)
	        .row,
	Row(ReferenceKind::Environment, "environment", "the environment", "environment")
	        .loads(AssetKind::Environment, kEnvironment)
	        .row,
	Row(ReferenceKind::MenuTexture, "menu_texture", "the texture", "texture")
	        .loads(AssetKind::Texture, nullptr, menu_texture_files)
	        .offers(ReferenceKind::StyleVar)
	        .row,
	// Opened by the name as written [orig: SoundBank_OpenFile @ 0x75caa0; ConfigFile_LoadGlobal @
	// 0x760ad0]; a sound bank or a credits file that does not open adds nothing [orig:
	// SoundBank_CollectionAddOrRef @ 0x652b40; CMarqueeWnd_LoadCreditsFromIni @ 0x65c5a0].
	Row(ReferenceKind::SoundBank, "sound_bank", "the sound bank", "sound bank")
	        .loads(AssetKind::SoundBank, nullptr)
	        .tolerated(bank_missing)
	        .row,
	Row(ReferenceKind::Credits, "credits", "the credits file", "credits file")
	        .loads(AssetKind::Credits, nullptr)
	        .tolerated(credits_missing)
	        .row,
	// A screen or window no lookup finds leaves the ACTION doing nothing [orig:
	// CUIWidget_HandleScriptedAction @ 0x6497f0].
	Row(ReferenceKind::MenuScreen, "menu_screen", "the screen", "screen")
	        .symbol(NameCase::NoCase)
	        .scoped(true)
	        .tolerated(screen_missing)
	        .row,
	Row(ReferenceKind::MenuWindow, "menu_window", "the window", "window")
	        .symbol(NameCase::NoCase)
	        .scoped(true)
	        .tolerated(window_missing)
	        .row,
	// A clip that does not load plays as failsafe.bad, or registers nothing without one [orig:
	// AnimMap_FindOrLoadBoneFile @ 0x40C030, @0x40c25b..0x40c2a1, none @0x40c260].
	Row(ReferenceKind::Animation, "animation", "the clip", "clip")
	        .loads(AssetKind::Animation, kAnimation)
	        .tolerated(clip_missing)
	        .message_reads_files()
	        .row,
	// Never a reference of its own: a menu's text as the game reads it (a NAME, a shown text, an
	// ACTION's target), which a whole %NAME% of the stylesheets stands in for (a StyleVar edge's
	// through: FieldUse::variable_through).
	Row(ReferenceKind::MenuText, "menu_text", "the text", "text").row,
	// A generator's, a track's or a light's parameter above style 0x70 names a CTRL register of its
	// model by its index in the model's table, which the load swaps for the global register the
	// entry names [orig: ThreediGp_LoadFromFile @ 0x5B5C7A..0x5B5DA2 (materials), @ 0x5B5E08..0x5B5EF6
	// (part animations), @ 0x5B5F4D..0x5B5F62 (lights)]; every index names one.
	Row(ReferenceKind::ModelRegister, "model_register", "the CTRL register", "CTRL register")
	        .record("register")
	        .row,
	// A spinner's or an Euler row's frame byte names an MTRX row of its model (frame_none: 0, and
	// 128 to 255, none).
	Row(ReferenceKind::ModelFrame, "model_frame", "the rotation frame", "rotation frame")
	        .record("frame", frame_none)
	        .row,
	// A user point of the model the scope names: an item's particle slot naming none attaches its effect to
	// none, any other lookup keeps none (user_point_missing).
	Row(ReferenceKind::UserPoint, "user_point", "the user point", "user point")
	        .symbol(NameCase::NoCase)
	        .scoped(true)
	        .tolerated(user_point_missing)
	        .row,
	// A terrain's height data, opened by the name its .trn gives. A .trn that names none is refused
	// [orig: Terrain_LoadEnvironmentConfig @0x610940, the empty polydata name @0x610a3e] (a terrain the
	// graph cannot read); a named file the files lack is opened later with its result unread [orig:
	// Terrain_Init @0x60fbe0 -> PolyTrn_LoadTerrainConfig @0x60e3d0, @0x60fd2d], so what the game makes
	// of it is not witnessed: listed, not gating (ADR 0046 S14).
	Row(ReferenceKind::TerrainData, "terrain_data", "the terrain height data", "terrain height data")
	        .loads(AssetKind::TerrainPolyData, nullptr)
	        .row,
	// A sound bank's single holds its wave's file name in a 256-byte slot the load patches in [orig:
	// SoundBank_LoadTriggerSets @0x75c370, the string pool @0x75c688; docs/audio/lwf-dbf-sound-re.md],
	// read from the archives by that name (wave_files); a wave the files lack plays nothing.
	Row(ReferenceKind::Wave, "wave", "the wave", "wave")
	        .loads(AssetKind::Wave, nullptr, wave_files)
	        .tolerated(wave_missing)
	        .row,
	// A powerup row by name, the first row of the name [orig: PowerUpDef_FindByName @0x442660, stricmp
	// over the rows in order]; an item whose powerupdef names none is destroyed as the mission starts
	// [orig: PowerupEntity_InitFromDef @0x442D10, Entity_Destroy @0x442E26].
	Row(ReferenceKind::Powerup, "powerup", "the powerup", "powerup")
	        .symbol(NameCase::NoCase, AssetKind::PowerupDefs)
	        .row,
	// A waypoint path's stop names a marker of its mission by its index in the markers, which the
	// runtime reads unbounded: past them it reads the pool's zeroed entry [orig: Pool_GetEntryUnchecked
	// @0x441FC0, Pool_Clear @0x442060].
	Row(ReferenceKind::MissionMarker, "mission_marker", "the marker", "marker").record("marker").row,
	// An Event trigger's and a ResetEvent action's first parameter names an event by its index in the
	// event table, read with no bound [orig: EventTrigger_EvaluateCondition @0x453620 main type 3,
	// EventAction_Dispatch @0x4542e0 case 34].
	Row(ReferenceKind::MissionEvent, "mission_event", "the event", "event").record("event").row,
	// A group by its index in the file's 64, a path by its number among its 128: fixed tables, which no
	// edit renumbers.
	Row(ReferenceKind::MissionGroup, "mission_group", "the group", "group").record("group", mission::group_names_none).row,
	Row(ReferenceKind::MissionPath, "mission_path", "the waypoint path", "waypoint path")
	        .record("waypoint_path", mission::path_names_none)
	        .row,
	// An entity by its SSN and an area trigger by its zone id, each found in its own mission by the id
	// its record carries, never by an index [orig: EntityPool_FindByNetId @0x4f0a20;
	// EventTrigger_ResolveZoneTriggerRefs @0x453000]. The game tolerates either missing.
	Row(ReferenceKind::MissionEntity, "mission_entity", "the entity", "entity")
	        .symbol(NameCase::Exact)
	        .scoped(true)
	        .tolerated(entity_missing)
	        .row,
	Row(ReferenceKind::MissionZone, "mission_zone", "the zone", "zone")
	        .symbol(NameCase::Exact)
	        .scoped(true)
	        .tolerated(zone_missing)
	        .row,
	// The files the game finds by a mission's name (documents/mission_file_set.h), each by the name
	// its reader builds [orig: Game_StartMission @0x524360]: the script compiled after game.wac and
	// server.wac [orig: WacScript_InitAndLoad @0x4f91f0], the loading image [orig: Render_LoadingScreen
	// @0x521d10], the tile placement, the dialog bank (its sounds a SoundBank).
	Row(ReferenceKind::Script, "script", "the script", "script").loads(AssetKind::Script, kScriptFile).row,
	Row(ReferenceKind::LoadingImage, "loading_image", "the loading image", "loading image")
	        .loads(AssetKind::Texture, kLoadingImage)
	        .row,
	Row(ReferenceKind::TilePlacement, "tile_placement", "the tile placement", "tile placement")
	        .loads(AssetKind::TileInfo, kTilePlacement)
	        .tolerated(tiles_missing)
	        .row,
	Row(ReferenceKind::DialogBank, "dialog_bank", "the dialog bank", "dialog bank")
	        .loads(AssetKind::DialogBank, kDialogBankFile)
	        .tolerated(dialog_missing)
	        .row,
	// The mission's own string table, a Strings file as a menu's TextTable is, but one the game runs
	// without (its keys read empty): a warning, worded by whether medmssn.bin stands in.
	Row(ReferenceKind::MissionStrings, "mission_strings", "the mission's string table", "mission string table")
	        .loads(AssetKind::Strings, kTable)
	        .tolerated(mission_strings_missing)
	        .message_reads_files()
	        .row,
	// A bank's wave by its name, in the bank its scope names: the bank's lookups find the first of the
	// name, without case [orig: SoundBank_FindEntryByName @ 0x75bba0]. A name none has is an error the
	// game never meets: the bank cannot be written with it (SoundBankDocument::serialize).
	// A dialog line's wave is the same name, read by the dialog line player in the dialog bank's sounds alone
	// [orig: Dialog_LoadAudioClip @ 0x44dcf7..0x44dd15 -> sub_75BDC0 @ 0x75bdc0], which the game tolerates
	// missing (bank_wave_severity).
	Row(ReferenceKind::BankWave, "bank_wave", "the wave", "wave")
	        .symbol(NameCase::NoCase, AssetKind::SoundBank)
	        .scoped(true)
	        .says(bank_wave_missing)
	        .severity_by(bank_wave_severity)
	        .message_reads_files()
	        .row,
	// A profile by name, the first of the name without case [orig: SoundProfile_FindSlotByName @ 0x526e30].
	Row(ReferenceKind::SoundProfile, "sound_profile", "the sound profile", "sound profile")
	        .symbol(NameCase::NoCase, AssetKind::SoundProfileDefs)
	        .tolerated(profile_missing)
	        .row,
	// A model material's shader, by the tag an effect registers under, compared without case [orig:
	// HLSLEffect_FindByName @ 0x5ADE70, stricmp]; one no effect registers draws as another (shader_missing).
	Row(ReferenceKind::Shader, "shader", "the shader", "shader")
	        .symbol(NameCase::NoCase, AssetKind::Shader)
	        .tolerated(shader_missing)
	        .message_reads_files()
	        .row,
	// A weapon action's slot, by its key past its first five characters without case, in its weapon's map
	// (the scope): the slot's clip there is what the action plays and times [orig: Anim_InitActions @
	// 0x54219E -> AnimMap_FindSlotByName @ 0x40cfa0; Anim_GetDurationTicks over the weapon's map @ 0x5421BD].
	Row(ReferenceKind::AnimationKey, "animation_key", "the animation slot", "animation slot")
	        .symbol(NameCase::SlotKey, AssetKind::AnimationMap)
	        .scoped(true)
	        .tolerated(animation_key_missing)
	        .row,
	// A VEHICLE_HUD block's item, by the alias items.def gives it (its sid, else "S%06i" of its id): the block's
	// VEHICLE_END copies it into every item of the alias, compared without case [orig: HUD_ParseHudposToken @
	// 0x59F3DA..0x59F40C, the _stricmp @ 0x59F402]; one no item has applies to none.
	Row(ReferenceKind::ItemAlias, "item_alias", "the item alias", "item alias")
	        .symbol(NameCase::NoCase, AssetKind::ItemDefs)
	        .tolerated(item_alias_missing)
	        .row,
	// A combination's head, body or arms, by the name of a part of that kind its file defines before it, the last
	// of the name, without case [orig: CAvatarDefs_ParseConfigLine @ 0x57a7e7, the stricmp over the parts @
	// 0x57a830..0x57a854]: a combination missing its head or body is dropped, one missing its arms kept without
	// them (the parser's own findings; formats/avatars).
	Row(ReferenceKind::AvatarPart, "avatar_part", "the avatar part", "avatar part")
	        .symbol(NameCase::NoCase, AssetKind::AvatarDefs)
	        .scoped(true)
	        .tolerated(avatar_part_missing)
	        .row,
	// A dialog by its name in the dialog bank its scope names, matched exactly, the first of the name in the
	// bank's order [orig: Dialog_PlayByName @ 0x44d9f0, strcmp @ 0x44da44 / @ 0x44da8b]; a mission names one
	// by the number its name forms, dlg%03i [orig: Dialog_PlayByIndex @ 0x527ae0]. One none has plays nothing.
	Row(ReferenceKind::Dialog, "dialog", "the dialog", "dialog")
	        .symbol(NameCase::Exact, AssetKind::DialogBank)
	        .scoped(true)
	        .tolerated(dialog_name_missing)
	        .message_reads_files()
	        .row,
};

constexpr bool same_token(const char *a, const char *b) {
	while (*a && *a == *b) {
		++a;
		++b;
	}
	return *a == *b;
}

// One row per kind, at the kind's own index, each token its own.
constexpr bool rows_well_formed() {
	for (size_t i = 0; i < kReferenceKindCount; ++i) {
		if (static_cast<size_t>(kRows[i].kind) != i) return false;
		for (size_t j = 0; j < i; ++j)
			if (same_token(kRows[i].token, kRows[j].token)) return false;
	}
	return true;
}

// A Record row names its collection, no file to load and no message of a missing one (the graph
// finds none missing), and counts its index across its file, the one space the core numbers; no
// other row names a collection, what names none or an index space.
constexpr bool records_well_formed() {
	for (const ReferenceKindRow &row : kRows) {
		const bool record = row.resolution == ReferenceResolution::Record;
		if (record != (*row.collection != '\0')) return false;
		if (record && (row.file != AssetKind::Unknown || row.missing_message)) return false;
		if (record != (row.index_space == RecordIndexSpace::File)) return false;
		if (!record && (row.none || row.index_space != RecordIndexSpace::None)) return false;
	}
	return true;
}

// A kind whose missing name refuses a build is one the graph finds missing, at an error's severity.
constexpr bool gates_well_formed() {
	for (const ReferenceKindRow &row : kRows)
		if ((row.gates_when_missing && !*row.gates_when_missing) ||
		    ((row.gates_when_missing || row.gates_when_missing_for) &&
		     (!row.missing_message || row.severity_when_missing != DiagnosticSeverity::Error)))
			return false;
	return true;
}

static_assert(sizeof(kRows) / sizeof(kRows[0]) == kReferenceKindCount, "every ReferenceKind has exactly one row");
static_assert(gates_well_formed(), "a kind that gates when missing is found missing as an error, with its witness");
static_assert(rows_well_formed(), "the rows follow ReferenceKind's order and their tokens are unique");
static_assert(records_well_formed(),
		"a Record row names its collection, no file and no missing message and counts across its file, "
		"and only a Record row names a collection, the values naming none or an index space");

} // namespace

namespace {

// A file a field names by its stem or its name: its file name, `extension` added where it has none, upper case.
std::string named_file_scope(const std::string &name, const char *extension) {
	if (name.empty()) return std::string();
	std::string file = basename_of(name);
	if (path_of(file).extension().empty()) file += extension;
	return strutil::to_upper(file);
}

} // namespace

std::string user_point_scope(const std::string &model, bool first_16) {
	const std::string scope = named_file_scope(model, ".3di");
	return scope.empty() || !first_16 ? scope : scope + "/" + kFirstUserPointsSection;
}

std::string animation_map_scope(const std::string &map) { return named_file_scope(map, ".adm"); }

const ReferenceKindRow &reference_row(ReferenceKind kind) {
	const size_t index = static_cast<size_t>(kind);
	return index < kReferenceKindCount ? kRows[index] : kRows[0];
}

bool reference_kind_from_token(const std::string &token, ReferenceKind &out) {
	for (const ReferenceKindRow &row : kRows)
		if (token == row.token) {
			out = row.kind;
			return true;
		}
	return false;
}

bool record_index(ReferenceKind kind, const Value &value, int64_t &index) {
	const ReferenceKindRow &row = reference_row(kind);
	const int64_t *number = std::get_if<int64_t>(&value);
	if (row.resolution != ReferenceResolution::Record || !number || *number < 0 ||
	    (row.none && row.none(*number)))
		return false;
	index = *number;
	return true;
}

int32_t texture_loader_arg(renderer::TextureLoader loader) {
	return -2 - static_cast<int32_t>(loader);
}

bool texture_loader_of(int32_t loader_arg, renderer::TextureLoader &loader) {
	const int32_t value = -2 - loader_arg;
	if (value < 0 || value > static_cast<int32_t>(renderer::TextureLoader::Particle)) return false;
	loader = static_cast<renderer::TextureLoader>(value);
	return true;
}

ReferenceKind style_value_reference(AssetKind file) {
	for (const ReferenceKindRow &row : kRows)
		if (row.also_offers == ReferenceKind::StyleVar && row.file == file) return row.kind;
	return ReferenceKind::None;
}

StyleVariableUse style_variable_use(ReferenceKind through) {
	switch (through) {
	case ReferenceKind::None: return StyleVariableUse::Colour;
	case ReferenceKind::Font: return StyleVariableUse::Font;
	case ReferenceKind::MenuTexture: return StyleVariableUse::Image;
	default: return StyleVariableUse::Other;
	}
}

bool blocks_build(const Diagnostic &d) {
	if (d.severity != DiagnosticSeverity::Error) return false;
	if (!d.row() || d.row()->gates_build) return true;
	// A listed code gates where its subject names the game's refusal: a reference (missing, or naming
	// a file its loader does not load) of a kind whose row cites it; a required file whose manifest
	// row is the boot's refusal [orig: Game_InitSubsystems @ 0x4a6fed, the string tables' MessageBox and
	// exit; Menu_InitShellResources @ 0x552651, the main menu's dead end], missing, or holding a file of
	// another kind, which the boot reads as its table with no check (its header's offsets made
	// pointers unchecked [orig: TextResource_FixupPointers @ 0x75d050]).
	if (const ReferenceSubject *reference = reference_subject(d)) {
		const ReferenceKindRow &row = reference_row(reference->kind);
		return row.gates_when_missing != nullptr ||
		       (row.gates_when_missing_for && row.gates_when_missing_for(reference->loader_arg) != nullptr);
	}
	if (const RequirementSubject *requirement = requirement_subject(d)) {
		const gameprofile::RequiredResource *row = gameprofile::gameprofile_required_resource_by_role(requirement->role.c_str());
		return (d.row() == &finding_code(CoreFinding::RequirementMissing) ||
		        d.row() == &finding_code(CoreFinding::RequirementWrongKind)) &&
		       row && row->severity == gameprofile::RES_FATAL;
	}
	return false;
}

bool diagnostics_block_build(const std::vector<Diagnostic> &items) {
	for (const Diagnostic &d : items)
		if (blocks_build(d)) return true;
	return false;
}

bool BaseNames::has(const std::string &name) const {
	if (!sorted) return false;
	const std::string wanted = normalized_logical_name(name);
	const auto found = std::lower_bound(sorted->begin(), sorted->end(), wanted,
	                                    [](const std::string &listed, const std::string &key) {
		                                    return normalized_logical_name(listed) < key;
	                                    });
	return found != sorted->end() && normalized_logical_name(*found) == wanted;
}

bool blocks_build(const Diagnostic &d, const BaseNames *base) {
	if (!blocks_build(d)) return false;
	if (!base) return true;
	if (d.row() == &finding_code(CoreFinding::RequirementMissing))
		if (const RequirementSubject *requirement = requirement_subject(d)) return !base->has(requirement->target);
	if (d.row() == &finding_code(CoreFinding::ReferenceMissing)) {
		if (const ReferenceSubject *reference = reference_subject(d)) {
			// A file the reference's loader opens by one of its names, the base's (a symbol is no file).
			const auto in_base = [base](const std::string &name) { return base->has(name); };
			for (const std::string &candidate :
			     reference_file_candidates(reference->kind, reference->target, reference->loader_arg, in_base))
				if (base->has(candidate)) return false;
		}
	}
	return true;
}

bool diagnostics_block_build(const std::vector<Diagnostic> &items, const BaseNames *base) {
	for (const Diagnostic &d : items)
		if (blocks_build(d, base)) return true;
	return false;
}

bool ShippedFiles::has(const std::string &asset) const {
	return !asset.empty() && original.count(asset) != 0 && unsaved.count(asset) == 0;
}

bool blocks_build(const Diagnostic &d, const BaseNames *base, const ShippedFiles *shipped) {
	if (!blocks_build(d, base)) return false;
	return !(shipped && d.row() && d.row()->blocks_save && shipped->has(d.asset));
}

bool diagnostics_block_build(const std::vector<Diagnostic> &items, const BaseNames *base, const ShippedFiles *shipped) {
	for (const Diagnostic &d : items)
		if (blocks_build(d, base, shipped)) return true;
	return false;
}

} // namespace opennova::editor
