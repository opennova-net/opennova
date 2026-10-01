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

// --- the values a Record reference names none by (ReferenceKindRow::none) ----------------------

// A part animation's frame byte (the field holds 0 to 255) names no MTRX row at 0 and at 128 to
// 255: the load sign-extends it [orig: GPM_LoadRenderModel @ 0x5B5698 (movsx)] and the pose reads
// a row only above zero [orig: Model_TransformBoneMatrices @ 0x58E3FE], so row 0 is never read
// (threedi_panm_frame_row, the pose's rule; whether a row turns through a frame at all is its
// field's use, ModelDocument's). Any value past the byte names none too.
bool frame_none(int64_t value) { return value <= 0 || value > 127; }

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
	// The game tolerates the name missing: a warning, saying what the game does instead.
	constexpr Row tolerated(ReferenceMissingMessage message) const {
		Row out = *this;
		out.row.severity_when_missing = DiagnosticSeverity::Warning;
		out.row.missing_message = message;
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
	        .message_reads_files()
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

static_assert(sizeof(kRows) / sizeof(kRows[0]) == kReferenceKindCount, "every ReferenceKind has exactly one row");
static_assert(rows_well_formed(), "the rows follow ReferenceKind's order and their tokens are unique");
static_assert(records_well_formed(),
		"a Record row names its collection, no file and no missing message and counts across its file, "
		"and only a Record row names a collection, the values naming none or an index space");

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

bool record_index(ReferenceKind kind, const Value &value, int64_t &index) {
	const ReferenceKindRow &row = reference_row(kind);
	const int64_t *number = std::get_if<int64_t>(&value);
	if (row.resolution != ReferenceResolution::Record || !number || *number < 0 ||
	    (row.none && row.none(*number)))
		return false;
	index = *number;
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

} // namespace opennova::editor
