#pragma once

// A record dragged onto what takes one (ADR 0046 S24): from an outline row, the Inspector's heading or an
// Event board card onto a value that names records of the same document (a board chip, a reference field),
// which takes what the record is to it (ReferencePicker::accept_record: an entity its SSN, an area its zone
// id, a group, a path, an event its index).

#include <cstdint>
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

} // namespace opennova::editor
