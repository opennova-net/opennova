#pragma once

#include <string>

#include <base/io/json.h>
#include <editor/session/editor_queries.h>

namespace opennova::editor {

// The particle_keys query (ADR 0046 S23 B, documents/particle_keys): a particle file's blocks as the game's reader
// reads them, each its kind, index, id and lines, and its keys: each key's value as the reader takes it with its
// place (line, column and length: a span edit there sets it), whether the block writes it, and what the reader takes
// it as, in words. The open particle document `path` names, else the active one.
io::JsonValue answer_particle_keys(const QueryContext &context, const QueryArgs &args, std::string &error);

} // namespace opennova::editor
