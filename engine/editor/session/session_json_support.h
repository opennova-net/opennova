#pragma once

// What session_json.cpp and reference_json.cpp share beside session_json.h (S26 Cy33): a reference's state as
// its token, a texture reference's role and its load, and a record's field by its id.

#include <cstdint>
#include <string>

#include <base/io/json.h>
#include <editor/graph/reference_kinds.h>
#include <editor/graph/reference_queries.h>
#include <editor/model/document.h>

namespace opennova::editor {

struct SessionView;
struct TextureReferenceLoad;

// A reference's state as its token: not_a_reference, present, missing, unverified.
const char *reference_status_token(ReferenceStatus status);
// A texture reference's use, by the role its loader argument names (ADR 0046 S18, texture_roles.h):
// texture_role its token, texture_gates whether the game refuses the mission without it.
void set_texture_role(io::JsonValue &out, ReferenceKind kind, int32_t loader_arg);
// A texture reference's load (S18): its name, its state, what the loader makes of it.
io::JsonValue texture_reference_json(const SessionView &view, const TextureReferenceLoad &load);
// The field `id` of the record at `address` as it applies there, and its value; false where the record or the
// field is not there.
bool field_of(const Document &document, const NodeAddress &address, const std::string &id, FieldUse &field,
              Value &value);

} // namespace opennova::editor
