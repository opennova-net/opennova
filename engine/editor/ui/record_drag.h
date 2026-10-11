#pragma once

// A record dragged onto what takes one (ADR 0046 S24): from an outline row, the Inspector's heading or an
// Event board card onto a value that names records of the same document (a board chip, a reference field),
// which takes what the record is to it (ReferencePicker::accept_record: an entity its SSN, an area its zone
// id, a group, a path, an event its index).

#include <cstdint>
#include <string>
#include <type_traits>

#include <editor/model/value.h>

namespace opennova::editor {

// What a record carries while it is dragged: the document it is of (DocumentBase::identity, the instance: a
// drop on another document's value takes nothing) and the record.
inline constexpr const char *kRecordDragPayload = "opennova.record";
struct RecordDrag {
	uint64_t document = 0;
	NodeAddress record;
};
static_assert(std::is_trivially_copyable<RecordDrag>::value, "Dear ImGui copies a payload by its bytes");

class Document;

// The item just drawn made a source of `record`'s drag (lane B, S24): pressed and moved, it carries the record of
// `document` and shows `words` beside the pointer. `anywhere`: the item has no id of its own (a line of text, the
// Inspector's heading), so the drag takes it by where it is. A mission's records alone (what a value of the mission
// takes by a drop); nothing for another document.
void record_drag_source(const Document &document, const NodeAddress &record, const std::string &words,
		bool anywhere = false);

} // namespace opennova::editor
