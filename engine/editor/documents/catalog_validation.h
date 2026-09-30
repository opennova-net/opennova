#pragma once
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/document.h>
#include <editor/model/finding_code_row.h>

namespace opennova::editor {

// The catalog document type's validator over one file (DocumentType::validate_file): used by the
// editor, CLI validate and Build, an open document standing in for its file so the findings
// describe the current draft. Input the game ignores is reported as a warning (saving drops it);
// input the typed model cannot carry is an error. A record whose name an earlier record of its
// kind has, and an item whose id an earlier item of the file has, are warnings naming the one a
// lookup finds (two item tables are two files of one name, of which the game reads one:
// asset.name.duplicate, so an id is compared within its table). The references a
// record makes (models, animation maps, ammo and weapon names, item ids, string ids) are the
// asset graph's.
std::vector<Diagnostic> validate_catalog_file(const DocumentBase &document);

// The catalog type's own finding codes (DocumentType::findings), each a row of its table
// (catalog_validation.cpp, static_asserted into this order): input the reader leaves out, which
// the game ignores and a rewrite drops, or which the typed model cannot carry (the file does not
// serialize); a value the file cannot write; a record with no name, or with one an earlier record
// of its kind has; an item with the id of an earlier item of the file; an item with no type.
enum class CatalogFinding {
	InvalidInput,
	IgnoredInput,
	Unserializable,
	NameEmpty,
	NameDuplicate,
	ItemIdentity,
	ItemType,
	kCount
};
const FindingCodeRow &finding_code(CatalogFinding code);
FindingTable catalog_finding_codes();
} // namespace opennova::editor
