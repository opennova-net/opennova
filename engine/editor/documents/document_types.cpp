#include "document_types.h"

#include <editor/documents/animation_document.h>
#include <editor/documents/animation_map_document.h>
#include <editor/documents/catalog_validation.h>
#include <editor/documents/credits_type.h>
#include <editor/documents/def_catalog_document.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/mission_labels.h>
#include <editor/documents/mission_validation.h>
#include <editor/documents/mnu_document.h>
#include <editor/documents/mns_document.h>
#include <editor/documents/model_document.h>
#include <editor/documents/model_labels.h>
#include <editor/documents/music_script_type.h>
#include <editor/documents/script_type.h>
#include <editor/documents/strings_document.h>
#include <editor/documents/text_types.h>
// The menu type's project check, by its hook alone: the render check runs the preview's headless
// screen compile (MenuScreenRender), so it sits with it in preview/ (ADR 0046 S13 V9).
#include <editor/preview/make_menu_render_check.h>

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

constexpr DocumentType kTypes[] = {
	{ DocumentTypeId::Catalog, "catalog", make_catalog, validate_catalog_file,
			DefCatalogDocument::schema, catalog_finding_codes },
	{ DocumentTypeId::Strings, "strings", make_strings, validate_strings_file,
			StringsDocument::schema, strings_finding_codes },
	{ DocumentTypeId::Menu, "menu", make_menu, validate_menu_file, MnuDocument::schema,
			menu_finding_codes, make_menu_render_check },
	{ DocumentTypeId::Styles, "styles", make_styles, validate_styles_file, MnsDocument::schema,
			style_finding_codes },
	// The model: its records and the values naming a part or a surface in a modder's words (S17).
	{ DocumentTypeId::Model, "model", make_model, validate_model_file, ModelDocument::schema,
			model_finding_codes, nullptr, nullptr, nullptr, nullptr, model_record_label, model_value_label },
	{ DocumentTypeId::Animation, "animation", make_animation, validate_animation_file,
			AnimationDocument::schema, animation_finding_codes },
	{ DocumentTypeId::AnimationMap, "animation_map", make_animation_map,
			validate_animation_map_file, AnimationMapDocument::schema,
			animation_map_finding_codes },
	// The mission (S14): its records' references no field's value is are its record_references (the
	// text keys a record's number forms); its records and values in a modder's words, and briefly for a
	// narrow column (S15).
	{ DocumentTypeId::Mission, "mission", make_mission, validate_mission_file, MissionDocument::schema,
			mission_finding_codes, nullptr, nullptr, nullptr, mission_references, mission_record_label,
			mission_value_label, mission_record_brief, mission_game_choices },
	// The text types (S13 D9): one TextDocument class, a row per behaviour, none with records
	// (text_fields) or a project check; the script's text names references, and its compiler's
	// words are its highlights (S13 V10).
	{ DocumentTypeId::Script, "script", make_script_document, validate_script_file, text_fields,
			script_finding_codes, nullptr, script_references, script_highlights },
	{ DocumentTypeId::MusicScript, "music_script", make_music_script_document,
			validate_music_script_file, text_fields, music_script_finding_codes },
	{ DocumentTypeId::Credits, "credits", make_credits_document, validate_credits_file,
			text_fields, credits_finding_codes },
	{ DocumentTypeId::Shader, "shader", make_shader_document, validate_shader_file, text_fields,
			shader_finding_codes },
	{ DocumentTypeId::Text, "text", make_text_document, validate_text_file, text_fields,
			text_finding_codes },
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
	return text_of(*made) ? DocumentContent::Text : DocumentContent::Other;
}

} // namespace

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

DocumentTypeStandIn::DocumentTypeStandIn(const DocumentType &type) { g_stand_in.store(&type); }

DocumentTypeStandIn::~DocumentTypeStandIn() { g_stand_in.store(nullptr); }

} // namespace opennova::editor
