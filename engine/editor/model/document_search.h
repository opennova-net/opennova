#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <editor/model/document.h>
#include <editor/model/value.h>

namespace opennova::editor {

// Find in a document (ADR 0046 S12): every record's fields as the Inspector shows them (the
// value in the units its file writes it, a choice by its name, a switch as Yes or No:
// field_text), each field as it applies to its record (Document::field_on). A field the game
// ignores there that the file does not write, and an optional one the file leaves out, are not
// searched (the Inspector hides the one and greys the other: neither is in the file).

struct SearchOptions {
	bool match_case = false; // else ASCII letters compare without case
};

// A field whose shown value holds the text.
struct DocumentHit {
	NodeAddress address;
	std::string locator; // Document::locator: what a Go to (OpenDocument's text) finds it again by
	std::string record;  // Document::record_path
	std::string field;   // the field's id
	std::string label;   // the field's name (field_title)
	std::string text;    // the value as shown
	size_t at = 0;       // where the text is found in it
};

// The hits in document order: the rows in order, each row before the records it holds (in
// pre-order), a record's fields in its kind's order; one per field. None for an empty text.
std::vector<DocumentHit> find_in_document(const Document &document, const std::string &text,
                                          const SearchOptions &options = SearchOptions());

// Where `text` is found in `in` (as `options` compares), or npos.
size_t find_text(const std::string &in, const std::string &text, const SearchOptions &options = SearchOptions());

} // namespace opennova::editor
