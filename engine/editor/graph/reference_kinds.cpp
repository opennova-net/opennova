#include <editor/graph/reference_kinds.h>

#include <cstdint>

#include <base/resource_index/texture_candidates.h>
#include <editor/graph/asset_graph.h>
#include <editor/project/project_files.h>
#include <runtime/anim/rig_files.h>
#include <runtime/menu/menu_assets.h>
#include <runtime/menu/menu_style.h>
#include <runtime/renderer/material_texture.h>

namespace opennova::editor {

namespace {

using Exists = std::function<bool(const std::string &)>;

std::vector<std::string> one(const std::string &file) {
	return file.empty() ? std::vector<std::string>() : std::vector<std::string>{file};
}

// --- the loaders' own rules (ReferenceKindRow::file_names) ---------------------------------

// A model's texture row (`loader_arg` >= 0, the row's type): the one file the loader the
// dispatcher picks for the row's type opens (renderer::material_texture_source; the shell's
// TextureFiles::load_material_texture reads the same file), none when that loader opens none;
// a loose file is never preferred, the project's files being an archive's as the game mounts
// them. The row's type is the one the loader copies into the runtime row [orig:
// Material_ConvertDefinition @ 0x5B045B..0x5B04A0]. A texture of no model row (a particle's
// graphic, a sky map, a def's graphic): the runtime's texture lookup, the name as written, then
// its stem with each texture extension (texture_candidate_filenames, which
// TextureFiles::load_texture probes).
std::vector<std::string> texture_files(const std::string &name, int32_t loader_arg, const Exists &exists) {
	if (loader_arg < 0) return texture_candidate_filenames(name);
	const uint8_t type = renderer::material_texture_runtime_type(static_cast<uint8_t>(loader_arg));
	return one(renderer::material_texture_source(name, type, exists).file);
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

// The extensions a loader appends to a name, tried after the name as written.
constexpr const char *kModel[] = {".3di", nullptr};
constexpr const char *kAnimationMap[] = {".adm", nullptr};
constexpr const char *kAnimation[] = {".bad", nullptr};
constexpr const char *kAiProfile[] = {".aip", nullptr};
constexpr const char *kMenu[] = {".mnu", nullptr};
constexpr const char *kTable[] = {".bin", nullptr};
constexpr const char *kTerrain[] = {".trn", nullptr};
constexpr const char *kEnvironment[] = {".env", nullptr};

// --- what a finding says of a name nothing resolves (ReferenceKindRow::missing_message) ---

std::string project_lacks(const AssetGraph &, const GraphEdge &) { return ", which the project does not have."; }

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
	const size_t slash = edge.scope.find('/');
	const std::string table = edge.scope.substr(0, slash);
	const std::string section = slash == std::string::npos ? std::string() : edge.scope.substr(slash + 1);
	if (edge.scope.empty()) return ", which no string table defines; the game shows the id.";
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

std::string bank_missing(const AssetGraph &, const GraphEdge &) {
	return ", which the project does not have: the game plays no sound for it.";
}

std::string credits_missing(const AssetGraph &, const GraphEdge &) {
	return ", which the project does not have: the marquee shows none of its lines.";
}

std::string clip_missing(const AssetGraph &graph, const GraphEdge &) {
	return graph.has_file(anim::kFailsafeClip)
	               ? ", which the project does not have: the game plays failsafe.bad in its place."
	               : ", which the project does not have: the game registers nothing for it (the project has no "
	                 "failsafe.bad to play in its place).";
}

std::string user_point_missing(const AssetGraph &, const GraphEdge &edge) {
	return ", which " + (edge.scope.empty() ? std::string("the model") : edge.scope) +
	       " does not have among its first 16 user points: the effect attaches to none.";
}

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
	// A symbol found in the file its scope names, which the picker narrows to when `picker`.
	constexpr Row scoped(bool picker) const {
		Row out = *this;
		out.row.scope_names_file = true;
		out.row.picker_scoped = picker;
		return out;
	}
	// The game tolerates the name missing: a warning, saying what the game does instead.
	constexpr Row tolerated(ReferenceMissingMessage message) const {
		Row out = *this;
		out.row.severity_when_missing = DiagnosticSeverity::Warning;
		out.row.missing_message = message;
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
	Row(ReferenceKind::Item, "item", "the item id", "item id").symbol(NameCase::Exact, AssetKind::ItemDefs).row,
	Row(ReferenceKind::Texture, "texture", "the texture", "texture").loads(AssetKind::Texture, nullptr, texture_files).row,
	Row(ReferenceKind::Sound, "sound", "the sound", "sound").row,
	Row(ReferenceKind::Particle, "particle", "the particle effect", "particle effect")
	        .symbol(NameCase::FileName, AssetKind::Particles)
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
	        .row,
	// A style variable no stylesheet the game reads defines stays literal.
	Row(ReferenceKind::StyleVar, "style_var", "the style variable", "style variable")
	        .variable()
	        .tolerated(style_missing)
	        .row,
	Row(ReferenceKind::Terrain, "terrain", "the terrain", "terrain").loads(AssetKind::Terrain, kTerrain).row,
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
	Row(ReferenceKind::WaveBank, "wave_bank", "the sound bank", "sound bank")
	        .loads(AssetKind::WaveBank, nullptr)
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
	        .row,
	// An item's particle slot naming no user point attaches its effect to none.
	Row(ReferenceKind::UserPoint, "user_point", "the user point", "user point")
	        .symbol(NameCase::NoCase)
	        .scoped(true)
	        .tolerated(user_point_missing)
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

static_assert(sizeof(kRows) / sizeof(kRows[0]) == kReferenceKindCount, "every ReferenceKind has exactly one row");
static_assert(rows_well_formed(), "the rows follow ReferenceKind's order and their tokens are unique");

} // namespace

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

ReferenceKind style_value_reference(AssetKind file) {
	for (const ReferenceKindRow &row : kRows)
		if (row.also_offers == ReferenceKind::StyleVar && row.file == file) return row.kind;
	return ReferenceKind::None;
}

} // namespace opennova::editor
