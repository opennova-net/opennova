#pragma once

#include <string>

#include <editor/model/document.h>
#include <editor/model/value.h>

// What a field shows (ADR 0046 S12): the words the Inspector, the find in a document and the
// windows' lists give a field, a choice and a value, from the field's schema alone.
namespace opennova::editor {

// The name the editor shows for a field (its label, else its id) and for a choice (its label,
// else the value as the file writes it).
std::string field_title(const FieldSchema &field);
const std::string &choice_title(const FieldChoice &choice);
// The choice a value names (a text compared as the game compares tokens, ignoring case), or
// null.
const FieldChoice *choice_of(const FieldSchema &field, const Value &value);
// A field whose only values are no (0) and yes (1): a tick box.
bool is_yes_no(const FieldSchema &field);
// A value as the Inspector shows it, whole: a choice by its name, a switch as Yes or No, a flags
// field's bits by their names ("none" for no bit), a number as its control writes it (a real to
// nine significant digits), a text as it is ("" for none).
std::string field_text(const FieldSchema &field, const Value &value);
// The same on one line: a text's first line ("(empty)" for none), cut to a few words when long.
std::string shown_value(const FieldSchema &field, const Value &value);
// Whether the file writes the field on the record: present (Document::present), and a yes / no
// field set to yes (the writer puts a flag down only then).
bool written(const Document &document, const NodeAddress &address, const FieldSchema &field);

} // namespace opennova::editor
