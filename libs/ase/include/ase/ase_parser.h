// ASE file parser — two-pass tokenizer with material flattening.
#pragma once

#include <string>

#include "ase/types.h"

namespace ase {

// Parse an ASE file into a Document. Returns true on success.
// On failure, error is filled with a description.
// Caller must call free_document() to release allocations.
bool parse_file(const std::string& path, Document& out_doc, std::string& error);

// Release all heap allocations inside a Document and zero it.
void free_document(Document& doc);

}  // namespace ase
