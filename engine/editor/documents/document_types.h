#pragma once

#include <memory>
#include <vector>

#include <base/io/json.h>
#include <editor/assets/asset_kind.h>
#include <editor/assets/asset_kinds.h>
#include <editor/documents/name_source.h>
#include <editor/graph/graph_edge.h>
#include <editor/model/diagnostic.h>
#include <editor/model/document.h>
#include <editor/model/finding_code_row.h>
#include <editor/model/text_document.h>

namespace opennova::editor {

class ProjectCheck;

// The registry of editable file kinds (ADR 0046 d9): one row per document type, one per
// DocumentTypeId past None in its order (a static_assert checks it), saying how to make a
// document (a DocumentBase: a record type's is its Document, as_records; a text type's a
// TextDocument, as_text), how to validate one file of the kinds it opens, which are the asset kinds
// whose row names its id (AssetKindRow::document), what its records' fields are, its own finding
// codes, the check of its own it runs across the project's files, if any, and for a text type the
// names its text references. The session, the windows and the shell reach a document type only
// through this table (its view through ui/document_views, keyed by the same id).
struct DocumentType {
	DocumentTypeId id = DocumentTypeId::None;
	const char *name = "";
	std::unique_ptr<DocumentBase> (*make)() = nullptr;
	// One file's own findings (ADR 0046 S13 D4), from its document alone, an open one standing in
	// for its file: what the game makes of what the file holds, on the records and fields that
	// cause it (a record type reads its records through its own document, as_records). What other
	// files make of what it defines is a use check's (graph/use_checks.h) and what it references
	// is the asset graph's; graph/project_validation.h runs the three over the project, the
	// validation cache keeping each file's findings until the file changes.
	std::vector<Diagnostic> (*validate_file)(const DocumentBase &document) = nullptr;
	// A kind of record's fields without a document (S13 V3): the table the type's documents'
	// Document::fields(kind) answer, in storage that outlives every document (the type's static
	// table), none for a kind it does not hold (a text type's: none for any kind, its documents
	// holding no records). What a field of a file of the type is called and what it takes, asked
	// where no document of the file is open (a graph edge's field in Files and the Inspector, a
	// rename's site); a test's stand-in type may leave it null.
	const std::vector<FieldSchema> &(*fields)(NodeKind kind) = nullptr;
	// The type's own finding codes (ADR 0046 S13 A6): its table, in the order of its enum, every
	// code its validator, its parse and the checks it answers for (a use check of the files it
	// defines, its project check) make beside the editor's own (model/finding_code_row.h), every
	// row the type's group. The tables are read as registered (registered_document_type), so a
	// test's stand-in type may leave it null.
	FindingTable (*findings)() = nullptr;
	// The type's project check (ADR 0046 S13 V9; documents/project_check.h), null for a type with
	// none: a check of its own across the project's files that keeps what it made from one
	// validation to the next (the menu type's render check). The row makes it; the instance lives
	// with whoever validates, keyed by the type's id (documents/project_checks.h), since the row
	// is constexpr.
	std::unique_ptr<ProjectCheck> (*project_check)() = nullptr;
	// A text type's references (ADR 0046 S13 D9): the names its document's text makes, each at its
	// span (a script's operands naming an effect, a sound set, an ammo or a text key), which the
	// asset graph makes edges of; null for a type whose text names nothing, and for a record type
	// (the graph reads a record document's references through its schema). Read from the document
	// alone, as validate_file is.
	void (*references)(const TextDocument &document, std::vector<TextReference> &out) = nullptr;
	// A record type's references that no field's value is (ADR 0046 S14): the files a mission's own
	// name finds (its text table, script, loading image, tile placement and dialog bank), a text key
	// formed from a number (a marker's name index). Added to what the schema's fields give
	// (extract_from_document), each edge with its record and locator where it has one; null for a
	// type whose references are all its fields'.
	void (*record_references)(const Document &document, Extracted &out) = nullptr;
	// A record type's display names (ADR 0046 S15, Names; graph/display_names.h dispatches to them):
	// what a record reads as with the project's names (`names` null: the document's own words), ""
	// leaving it to Document::record_title; and a field's value worded where it names something the
	// generic words do not reach (a mission's SSN, zone, event, group, path, stop, text key), false
	// leaving it to them. Null for a type whose titles and values say it already.
	std::string (*record_label)(const Document &document, const NodeAddress &address, const NameSource *names) = nullptr;
	bool (*value_label)(const Document &document, const NodeAddress &address, const FieldUse &field, const Value &value,
	                    const NameSource *names, DisplayName &out) = nullptr;
	// A record's words for a column too narrow for its title, what tells it apart first (a mission's
	// entity by its SSN, an event by its first trigger's subject and verb); "" (or null) where the
	// title cut to the column says it.
	std::string (*record_brief)(const Document &document, const NodeAddress &address, const NameSource *names) = nullptr;
	// The values a field's picker offers that name no definition because the game resolves them itself (a
	// mission's SSN 10000, the player), added before the graph's names; null for a type with none.
	void (*game_choices)(const Document &document, const NodeAddress &address, const FieldUse &field,
	                     std::vector<GameChoice> &out) = nullptr;
	// What a document of another content than records or a text holds, on the wire (S18: a texture's
	// reader, sides, levels and facts), the document query's member named by the type's name; null for
	// a type whose documents the base's lifecycle, the rows or the lines say.
	io::JsonValue (*content_json)(const DocumentBase &document) = nullptr;
	// A text type's definitions: the names its document's text defines, each at its span (a shader's tag),
	// which the asset graph makes symbols of; null for a type whose text defines nothing, and for a record
	// type (the graph reads a record document's definitions through its schema). Read from the document
	// alone, as validate_file is.
	void (*definitions)(const TextDocument &document, std::vector<TextDefinition> &out) = nullptr;
	// A name another file names and nothing defines, defined in a document of the type (ADR 0046 DI-15, Add it
	// there): the batch adding a record of the kind the name is (`missing`: its kind, the name as the reference
	// writes it, its scope) to `document`, named as referenced, where the game's lookup finds it, born as the
	// type's own Add makes one (its kind's defaults: the game's reader's where it gives a new record any), with
	// the offer's words (`out`: one undo step of the document, which Problems opens first and the Add selects).
	// False for a name the type does not define, or has no place for in this file. Null for a type defining
	// nothing another file names.
	bool (*define_symbol)(const DocumentBase &document, const ReferenceSubject &missing, PlannedFix &out) = nullptr;
	// A type of a kind the ConfigFile text reader reads: the entry lines of its document's text (each line's first
	// offset) that change nothing its game loader reads, so that the line gone, the game loads the same (a line
	// of no key it asks for, a later section of a label it reads the first of, a key at the value a missing one
	// reads), which the ConfigFile pool rule's fix comments out (documents/config_overrun.h). Null for a type
	// with no port of its loader's reads, whose overrun finding has no fix.
	void (*config_idle_lines)(const TextDocument &document, std::vector<size_t> &line_starts) = nullptr;
};

// A record in its type's own words with no project names (its record_label with none, else the
// document's record_title), "" where those words are its name (record_name): what a place kept apart
// from its document names the record by (GraphEdge::record_title, GraphSymbol::title,
// Diagnostic::record_title; the plain-words lane), so a closed file's reads as an open one's.
std::string record_own_title(const Document &document, const NodeAddress &address);

// The type its row names (null for DocumentTypeId::None); the type that opens a kind (null for a
// kind the build packs as it is).
const DocumentType *document_type(DocumentTypeId id);
// The type the registry holds for an id whatever stands in its place (document_type answers a
// test's stand-in): what the finding codes' tables are read from (session/finding_codes.h).
const DocumentType *registered_document_type(DocumentTypeId id);
const DocumentType *document_type_for(AssetKind kind);
bool is_editable_kind(AssetKind kind);
// What the documents a type makes hold: records (DocumentBase::as_records), a text
// (DocumentBase::as_text, S13 D9), its file's bytes whole (DocumentBase::holds_bytes: S18 a texture's
// texels, round S23 a wave's samples, each edited a whole file at a time, which names nothing the graph
// reads and which the validation reads through its type's validate_file where the type has finding
// codes or a validate_file, its file left unread where it has neither), or content of another kind,
// which the graph's extraction, the validation and the rename read nothing of until it has hooks of its
// own (a test's blob; a raster to come). Asked of a document the type makes, once per registered type.
enum class DocumentContent { Records, Text, Bytes, Other };
DocumentContent document_content(const DocumentType &type);
// Whether the validation reads a type's files: every type's but a whole-file type's with neither finding
// codes nor a validate_file, which could make no finding of a file.
bool validates_files(const DocumentType &type);

// A test's document type in a registered one's place (S13 D6: a type whose documents hold
// neither records nor a text): while it lives, document_type answers it for its id, and so
// document_type_for for every asset kind whose row names that id. The registry's one seam:
// nothing but a test makes one, one at a time.
class DocumentTypeStandIn {
public:
	explicit DocumentTypeStandIn(const DocumentType &type);
	~DocumentTypeStandIn();
	DocumentTypeStandIn(const DocumentTypeStandIn &) = delete;
	DocumentTypeStandIn &operator=(const DocumentTypeStandIn &) = delete;
};

} // namespace opennova::editor
