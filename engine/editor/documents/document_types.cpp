#include "document_types.h"

#include <editor/documents/ai_profile_document.h>
#include <editor/documents/animation_document.h>
#include <editor/documents/animation_map_document.h>
#include <editor/documents/avatars_document.h>
#include <editor/documents/catalog_labels.h>
#include <editor/documents/catalog_validation.h>
#include <editor/documents/menu_labels.h>
#include <editor/documents/charattr_document.h>
#include <editor/documents/credits_type.h>
#include <editor/documents/def_catalog_document.h>
#include <editor/documents/dialog_bank_document.h>
#include <editor/documents/environment_document.h>
#include <editor/documents/face_animation_document.h>
#include <editor/documents/font_document.h>
#include <editor/documents/music_bank_document.h>
#include <editor/documents/wave_document.h>
#include <editor/documents/hud_layout_type.h>
#include <editor/documents/hudfx_document.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/mission_labels.h>
#include <editor/documents/mission_validation.h>
#include <editor/documents/mnu_document.h>
#include <editor/documents/mns_document.h>
#include <editor/documents/model_document.h>
#include <editor/documents/model_labels.h>
#include <editor/documents/music_script_type.h>
#include <editor/documents/particle_type.h>
#include <editor/documents/score_document.h>
#include <editor/documents/script_type.h>
#include <editor/documents/sound_bank_document.h>
#include <editor/documents/sound_profile_document.h>
#include <editor/documents/strings_document.h>
#include <editor/documents/text_types.h>
#include <editor/documents/terrain_document.h>
#include <editor/documents/texture_document.h>
// The menu type's project check, by its hook alone: the render check runs the preview's headless
// screen compile (MenuScreenRender), so it sits with it in preview/ (ADR 0046 S13 V9).
#include <editor/preview/make_menu_render_check.h>
// The mission type's, by its hook alone too: the ground check reads the preview's scene, terrain and
// people's poses (preview/mission_ground_check.h, DI-28).
#include <editor/preview/make_mission_ground_check.h>

#include <array>
#include <atomic>

namespace opennova::editor {
namespace {

std::unique_ptr<DocumentBase> make_mission() { return std::make_unique<MissionDocument>(); }

std::unique_ptr<DocumentBase> make_catalog() { return std::make_unique<DefCatalogDocument>(); }
std::unique_ptr<DocumentBase> make_strings() { return std::make_unique<StringsDocument>(); }
std::unique_ptr<DocumentBase> make_menu() { return std::make_unique<MnuDocument>(); }
std::unique_ptr<DocumentBase> make_styles() { return std::make_unique<MnsDocument>(); }
std::unique_ptr<DocumentBase> make_model() { return std::make_unique<ModelDocument>(); }
std::unique_ptr<DocumentBase> make_animation() { return std::make_unique<AnimationDocument>(); }
std::unique_ptr<DocumentBase> make_animation_map() {
	return std::make_unique<AnimationMapDocument>();
}
std::unique_ptr<DocumentBase> make_sound_bank() { return std::make_unique<SoundBankDocument>(); }
std::unique_ptr<DocumentBase> make_sound_profiles() { return std::make_unique<SoundProfileDocument>(); }
std::unique_ptr<DocumentBase> make_environment() { return std::make_unique<EnvironmentDocument>(); }
std::unique_ptr<DocumentBase> make_terrain() { return std::make_unique<TerrainDocument>(); }
std::unique_ptr<DocumentBase> make_dialog_bank() { return std::make_unique<DialogBankDocument>(); }
std::unique_ptr<DocumentBase> make_face_animation() { return std::make_unique<FaceAnimationDocument>(); }
std::unique_ptr<DocumentBase> make_font() { return std::make_unique<FontDocument>(); }
std::unique_ptr<DocumentBase> make_music_bank() { return std::make_unique<MusicBankDocument>(); }
std::unique_ptr<DocumentBase> make_ai_profile() { return std::make_unique<AiProfileDocument>(); }
std::unique_ptr<DocumentBase> make_hudfx() { return std::make_unique<HudFxDocument>(); }
std::unique_ptr<DocumentBase> make_charattr() { return std::make_unique<CharAttrDocument>(); }
std::unique_ptr<DocumentBase> make_score() { return std::make_unique<ScoreDocument>(); }
std::unique_ptr<DocumentBase> make_avatars() { return std::make_unique<AvatarsDocument>(); }

constexpr DocumentType kTypes[] = {
	// The catalogs: a weapon, an ammo, a mounted gun by the names the player sees (the plain-words lane).
	// Each defines a name another file names and nothing defines (DI-15, define_symbol): a weapon, an ammo, an
	// item or a powerup row; a string id; a screen or a window; a style variable.
	{ DocumentTypeId::Catalog, "catalog", make_catalog, validate_catalog_file,
			DefCatalogDocument::schema, catalog_finding_codes, nullptr, nullptr, catalog_references,
			catalog_record_label, nullptr, nullptr, nullptr, nullptr, nullptr, define_catalog_symbol },
	{ DocumentTypeId::Strings, "strings", make_strings, validate_strings_file,
			StringsDocument::schema, strings_finding_codes, nullptr, nullptr, nullptr, nullptr, nullptr,
			nullptr, nullptr, nullptr, nullptr, define_string_id },
	// The menu: its parts by what they do (an action, a look, a sound), never "Action 1" (the plain-words
	// lane).
	{ DocumentTypeId::Menu, "menu", make_menu, validate_menu_file, MnuDocument::schema,
			menu_finding_codes, make_menu_render_check, nullptr, nullptr, menu_record_label, nullptr,
			nullptr, nullptr, nullptr, nullptr, define_menu_name },
	{ DocumentTypeId::Styles, "styles", make_styles, validate_styles_file, MnsDocument::schema,
			style_finding_codes, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
			nullptr, define_style_variable },
	// The model: its records and the values naming a part or a surface in a modder's words (S17).
	{ DocumentTypeId::Model, "model", make_model, validate_model_file, ModelDocument::schema,
			model_finding_codes, nullptr, nullptr, nullptr, model_record_label, model_value_label },
	{ DocumentTypeId::Animation, "animation", make_animation, validate_animation_file,
			AnimationDocument::schema, animation_finding_codes },
	{ DocumentTypeId::AnimationMap, "animation_map", make_animation_map,
			validate_animation_map_file, AnimationMapDocument::schema,
			animation_map_finding_codes },
	// The mission (S14): its records' references no field's value is are its record_references (the
	// text keys a record's number forms); its records and values in a modder's words, and briefly for a
	// narrow column (S15); its project check, what the game grounds where (DI-28).
	{ DocumentTypeId::Mission, "mission", make_mission, validate_mission_file, MissionDocument::schema,
			mission_finding_codes, make_mission_ground_check, nullptr, mission_references,
			mission_record_label, mission_value_label, mission_record_brief, mission_game_choices },
	// The text types (S13 D9): one TextDocument class, a row per behaviour, none with records
	// (text_fields) or a project check; the script's text names references (its compiler's words are
	// the script device's highlights, preview/script_viewport's table).
	{ DocumentTypeId::Script, "script", make_script_document, validate_script_file, text_fields,
			script_finding_codes, nullptr, script_references },
	// The music script's plays name its bank's streams by their places (S23 B).
	{ DocumentTypeId::MusicScript, "music_script", make_music_script_document,
			validate_music_script_file, text_fields, music_script_finding_codes, nullptr, music_script_references },
	// The credits' text names the images its lines draw and the fonts its text lines draw in.
	{ DocumentTypeId::Credits, "credits", make_credits_document, validate_credits_file,
			text_fields, credits_finding_codes, nullptr, credits_references },
	// The shader's text defines the tags it registers (its EffectTag; _ffp.fx the fixed-function tags).
	{ DocumentTypeId::Shader, "shader", make_shader_document, validate_shader_file, text_fields,
			shader_finding_codes, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
			shader_definitions },
	{ DocumentTypeId::Text, "text", make_text_document, validate_text_file, text_fields,
			text_finding_codes },
	// The texture (S18): its texels as the game reads them, edited by the whole-image operations
	// (documents/texture_operations), its findings what the game's reader makes of its file (a PCX's short
	// rows, a DDS's sides), its content on the wire.
	{ DocumentTypeId::Texture, "texture", make_texture_document, validate_texture_file, texture_fields,
			texture_finding_codes, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
			texture_content_json },
	// The sound lane: a bank's waves and sets (a set's name a sound, a member's wave one of the bank's); and
	// SndProf.def's profiles, each slot naming a set. (A wave the game's loader refuses is the wave type's own
	// finding since round S23.)
	// Each defines its kind's names (DI-15): a sound set, a sound profile.
	{ DocumentTypeId::SoundBank, "sound_bank", make_sound_bank, validate_sound_bank_file, SoundBankDocument::schema,
			sound_bank_finding_codes, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
			nullptr, nullptr, define_sound_set },
	{ DocumentTypeId::SoundProfiles, "sound_profiles", make_sound_profiles, validate_sound_profiles_file,
			SoundProfileDocument::schema, sound_profile_finding_codes, nullptr, nullptr, nullptr, nullptr,
			nullptr, nullptr, nullptr, nullptr, nullptr, define_sound_profile },
	// The particle file (DI-14): a text the effect system's reader reads, its findings that reader's;
	// what it names the asset graph reads through the same reader (no references of the type's own). An
	// effect a file names and no file defines is written by the engine's own effect writer (DI-15).
	{ DocumentTypeId::Particles, "particle", make_particle_document, validate_particle_file, text_fields,
			particle_finding_codes, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
			nullptr, define_particle_effect },
	// The environment (DI-19a): a .env's keywords and keyframes over env::Config, its references its
	// fields' (the cloud layers' textures, the sun, moon, glare and star models).
	{ DocumentTypeId::Environment, "environment", make_environment, validate_environment_file,
			EnvironmentDocument::schema, environment_finding_codes },
	// The HUD layout (the plan's DI-20): hudpos.def held as its text, its line ends the game's reader's;
	// its picture the HUD viewport's, its names the graph's through the engine's own parser.
	{ DocumentTypeId::HudLayout, "hud_layout", make_hud_layout_document, validate_hud_layout_file, text_fields,
			hud_layout_finding_codes },
	// The terrain (DI-30): a .trn's keys, grid rows and foliage definitions over TrnConfig, its references its
	// fields' (each map by its role's loader, the height data, the tile placement, the definitions' models).
	{ DocumentTypeId::Terrain, "terrain", make_terrain, validate_terrain_file, TerrainDocument::schema,
			terrain_finding_codes },
	// The dialog bank (DI-32): a .dbf's dialogs and their lines, a dialog's name what a mission's Play dialog
	// plays, a line's wave one of the bank's sounds; a dialog a mission names and the bank lacks added there
	// (DI-15).
	{ DocumentTypeId::DialogBank, "dialog_bank", make_dialog_bank, validate_dialog_bank_file, DialogBankDocument::schema,
			dialog_bank_finding_codes, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
			nullptr, define_dialog },
	// The character attributes (DI-09's charattr follow-up; S23 B its rows): charattr.def's classes, each its keys
	// as the loader reads them, a camouflage item an items.def item by its id less 100000; the ConfigFile pool rule's
	// finding its validator's, over the text its save writes.
	{ DocumentTypeId::CharAttrs, "charattr", make_charattr, validate_charattr_file, CharAttrDocument::schema,
			charattr_finding_codes },
	// The face animation (round S23 lane A): a .grm's face over grm::File, its references its texture fields' (each
	// name made .TGA by the stage loader) and the base's .MDT twin; a triangle names its vertices by index.
	{ DocumentTypeId::FaceAnimation, "face_animation", make_face_animation, validate_face_animation_file,
			FaceAnimationDocument::schema, face_animation_finding_codes, nullptr, nullptr,
			face_animation_references },
	// The font (round S23 lane A): a .fnt's header and its 224 glyphs over the engine's reader and writer, its pages'
	// texels as read; it names nothing.
	{ DocumentTypeId::Font, "font", make_font, validate_font_file, FontDocument::schema, font_finding_codes },
	// The music bank (round S23 lane A): a .sbf's header and its streams over the engine's reader and writer, each
	// stream's chunks as read; it names nothing (the music script plays its streams by place).
	{ DocumentTypeId::MusicBank, "music_bank", make_music_bank, validate_music_bank_file, MusicBankDocument::schema,
			music_bank_finding_codes, nullptr, nullptr, music_bank_references },
	// The wave (round S23 lane A): a .wav held as its bytes, its facts and the game loader's verdict its content on the
	// wire, the one place a wave the loader refuses is found (asset.wave_unplayable); trimmed and normalised whole.
	{ DocumentTypeId::Wave, "wave", make_wave_document, validate_wave_file, wave_fields, wave_finding_codes, nullptr,
			nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, wave_content_json },
	// The AI profile (S23 B): an .aip's one profile, its keys the reader's in the units the file writes them, each
	// read by its type's key set; a weapon an ammo's name.
	{ DocumentTypeId::AiProfile, "ai_profile", make_ai_profile, validate_ai_profile_file, AiProfileDocument::schema,
			ai_profile_finding_codes },
	// The HUD effects (S23 B): hudfx.def's model lines, the first the one the game reads, a model by its file.
	{ DocumentTypeId::HudFx, "hudfx", make_hudfx, validate_hudfx_file, HudFxDocument::schema, hudfx_finding_codes },
	// The score table (S23 B): score.ini's game type blocks, each its scoreboard columns and the points each event
	// scores, written in ScoreConfig_SaveFile's form.
	{ DocumentTypeId::ScoreTable, "score", make_score, validate_score_file, ScoreDocument::schema, score_finding_codes },
	// The avatars table (S23 B): Avatars.def's parts and nationalities, a part's name what a combination after it
	// names, its shown name a key of Game.bin's Avatars section, its models files.
	{ DocumentTypeId::Avatars, "avatars", make_avatars, validate_avatars_file, AvatarsDocument::schema,
			avatars_finding_codes },
};

// One type per DocumentTypeId past None, in its order, each making its documents, validating its
// files, naming its records' fields and declaring its finding codes.
constexpr bool types_in_order() {
	for (size_t i = 0; i < kDocumentTypeCount; ++i)
		if (static_cast<size_t>(kTypes[i].id) != i + 1 || !kTypes[i].make ||
				!kTypes[i].validate_file || !kTypes[i].fields || !kTypes[i].findings)
			return false;
	return true;
}

// A type may have no project check; one it has is its own: two rows naming the same would make
// two instances of it, each over the whole project, and list its findings twice.
constexpr bool project_checks_own() {
	for (size_t i = 0; i < kDocumentTypeCount; ++i)
		for (size_t j = i + 1; j < kDocumentTypeCount; ++j)
			if (kTypes[i].project_check && kTypes[i].project_check == kTypes[j].project_check)
				return false;
	return true;
}

static_assert(sizeof(kTypes) / sizeof(kTypes[0]) == kDocumentTypeCount,
              "every DocumentTypeId has exactly one type");
static_assert(types_in_order(),
		"the types follow DocumentTypeId's order, each with its make, validate_file, fields and "
		"findings");
static_assert(project_checks_own(),
		"a type's project check is its own (a type may have none): no two rows name the same");

// A test's type in a registered one's place (DocumentTypeStandIn), null for none.
std::atomic<const DocumentType *> g_stand_in{nullptr};

// What a type's documents hold: asked of one it makes.
DocumentContent content_made(const DocumentType &type) {
	if (!type.make) return DocumentContent::Other;
	const std::unique_ptr<DocumentBase> made = type.make();
	if (records_of(*made)) return DocumentContent::Records;
	if (text_of(*made)) return DocumentContent::Text;
	return made->holds_bytes() ? DocumentContent::Bytes : DocumentContent::Other;
}

} // namespace

std::string record_own_title(const Document &document, const NodeAddress &address) {
	std::string title;
	if (const DocumentType *type = document_type_for(document.kind()); type && type->record_label)
		title = type->record_label(document, address, nullptr);
	if (title.empty()) title = document.record_title(address);
	return title == document.record_name(address) ? std::string() : title;
}

const DocumentType *document_type(DocumentTypeId id) {
	if (const DocumentType *stand_in = g_stand_in.load(); stand_in && stand_in->id == id)
		return stand_in;
	return registered_document_type(id);
}

const DocumentType *registered_document_type(DocumentTypeId id) {
	const size_t index = static_cast<size_t>(id);
	return index >= 1 && index <= kDocumentTypeCount ? &kTypes[index - 1] : nullptr;
}

const DocumentType *document_type_for(AssetKind kind) {
	return document_type(asset_kind_row(kind).document);
}

bool is_editable_kind(AssetKind kind) { return document_type_for(kind) != nullptr; }

DocumentContent document_content(const DocumentType &type) {
	// A registered type is asked once; a stand-in (a test's) each time.
	const size_t index = static_cast<size_t>(type.id);
	if (index < 1 || index > kDocumentTypeCount || &type != &kTypes[index - 1])
		return content_made(type);
	static const std::array<DocumentContent, kDocumentTypeCount> answers = [] {
		std::array<DocumentContent, kDocumentTypeCount> out{};
		for (size_t i = 0; i < kDocumentTypeCount; ++i) out[i] = content_made(kTypes[i]);
		return out;
	}();
	return answers[index - 1];
}

bool validates_files(const DocumentType &type) {
	return document_content(type) != DocumentContent::Bytes || type.validate_file ||
	       (type.findings && type.findings().count > 0);
}

DocumentTypeStandIn::DocumentTypeStandIn(const DocumentType &type) { g_stand_in.store(&type); }

DocumentTypeStandIn::~DocumentTypeStandIn() { g_stand_in.store(nullptr); }

} // namespace opennova::editor
