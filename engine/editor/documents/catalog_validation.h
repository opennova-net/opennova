#pragma once
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/document.h>

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
} // namespace opennova::editor
